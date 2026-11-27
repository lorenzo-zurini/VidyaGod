#include "codedcache.h"

#include "bytesource.h"     // VidyaGodFS: MmapByteSource, FdByteSource
#include "cid.h"            // the description's CID (of its canonical text)
#include "coding.h"         // VidyaGodFS: the coding algebra (ExpandInto)
#include "commonutils.h"    // Log*
#include "layerspec.h"      // VidyaGodFS: ParseSpecText
#include "treeplan.h"       // VidyaGodFS: PlanTree, LayerBytes (a layer's bytes, delta chains included)

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace CodedCache
{
namespace
{
bool Magic(const std::filesystem::path &P, const char *M)
{
    std::ifstream F(P, std::ios::binary);
    char B[4] = {0};
    return F.read(B, 4) && std::memcmp(B, M, 4) == 0;
}
std::string Key(const std::string &R, const std::string &C, const std::string &D) { return R + "." + C + "." + D; }
std::atomic<unsigned> Seq{0};
#ifndef _WIN32
//A file of N bytes mapped writable: what an expansion writes the shipped bytes and the content into. File-backed, so
//gigabytes of it are page cache the kernel can drop, not memory the launch holds. Its blocks are RESERVED first: a disk
//without room fails here, cleanly, never as a fault inside the mapping later (SIGBUS takes the whole app).
struct MappedFile
{
    std::filesystem::path P; int Fd = -1; void *M = MAP_FAILED; size_t N = 0;
    bool Open(const std::filesystem::path &Path, size_t Size, std::string &Err)
    {
        P = Path; N = Size + 1;
        Fd = ::open(P.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (Fd < 0) { Err = "cannot make " + P.string() + ": " + std::strerror(errno); P.clear(); return false; }
        if (const int E = ::posix_fallocate(Fd, 0, static_cast<off_t>(N)); E != 0)
        { Err = "no room for the expansion's scratch (" + std::to_string(N >> 20) + " MiB in " + P.parent_path().string() + "): " + std::strerror(E); return false; }
        M = ::mmap(nullptr, N, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
        if (M == MAP_FAILED) { Err = "cannot map " + P.string(); return false; }
        return true;
    }
    ~MappedFile()
    {
        if (M != MAP_FAILED) ::munmap(M, N);
        if (Fd >= 0) ::close(Fd);
        std::error_code Ec;
        if (!P.empty()) std::filesystem::remove(P, Ec);
    }
};
//Scratch an expansion left behind when its launch died (named <key>.<pid>.<n>.<what>.part): its process is gone.
void SweepStale(const std::filesystem::path &Dir)
{
    std::error_code Ec;
    for (const auto &E : std::filesystem::directory_iterator(Dir, Ec))
    {
        const std::string N = E.path().filename().string();
        if (N.size() < 6 || N.compare(N.size() - 5, 5, ".part") != 0) continue;
        std::vector<std::string> F; size_t A = 0, B;
        while ((B = N.find('.', A)) != std::string::npos) { F.push_back(N.substr(A, B - A)); A = B + 1; }
        F.push_back(N.substr(A));
        if (F.size() < 5) continue;
        const std::string &Pid = F[F.size() - 4];                         // <key>.<pid>.<n>.<what>.part
        if (Pid.empty() || Pid.find_first_not_of("0123456789") != std::string::npos) continue;
        const long P = std::atol(Pid.c_str());
        if (P > 0 && (::kill(static_cast<pid_t>(P), 0) == 0 || errno == EPERM)) continue;   // its process lives
        std::filesystem::remove(E.path(), Ec);
    }
}
#endif
}

bool PlainTwin(const std::filesystem::path &Dir, const std::string &Description, const std::filesystem::path &Record,
               const std::string &RecordCid, const std::string &ContentCid, const std::string &DescriptionCid,
               const std::shared_ptr<ByteSource> &Content, std::filesystem::path &Out, std::string &Err)
{
    for (const std::string *C : {&RecordCid, &ContentCid, &DescriptionCid})
    {
        if (C->empty()) { Err = "a CID to keep its twin by is missing (record, content and description each need one)"; return false; }
        if (C->find_first_of("/\\.") != std::string::npos || C->size() > 80) { Err = "not a CID: " + *C; return false; }
    }
    const std::string RecordKey = Key(RecordCid, ContentCid, DescriptionCid);
    Out = Dir / (RecordKey + ".vgcr");
    if (Magic(Out, "VGCR")) return true;
#ifdef _WIN32
    (void)Description; (void)Record; (void)Content;
    Err = "expanding a record is not built for Windows yet";
    return false;
#else
    std::error_code Ec;
    std::filesystem::create_directories(Dir, Ec);
    auto Desc = std::make_shared<MemByteSource>(std::vector<uint8_t>(Description.begin(), Description.end()));
    auto Rec = std::make_shared<MmapByteSource>();
    if (!Rec->Map(Record.string())) { Err = "cannot read its record " + Record.string(); return false; }
    auto Probe = coding::CodedByteSource::Create(Desc, Rec, Content, 0, Err);
    if (!Probe) return false;
    const uint64_t Sn = Probe->size(), Cn = Content->size();
    Probe.reset();
    //what a record says its stream is, its content must account for (ExpandInto checks it too): before any scratch exists
    if (Sn > Cn + Cn / 8 + Rec->n + (1u << 20)) { Err = "the record claims a stream far bigger than its content"; return false; }
    //room for the scratch (the stream and its content) and the twin (about a quarter of the stream), with a margin
    if (const auto Sp = std::filesystem::space(Dir, Ec); !Ec && Sp.available < Sn + Cn + Sn / 2 + (256u << 20))
    { Err = "not enough free disk in " + Dir.string() + " (" + std::to_string((Sn + Cn + Sn / 2) >> 20) + " MiB needed, " + std::to_string(Sp.available >> 20) + " free)"; return false; }
    coding::Description D;
    if (!coding::Read(Description, D, Err)) return false;
    const std::string Tag = RecordKey + "." + std::to_string(::getpid()) + "." + std::to_string(++Seq);
    MappedFile Ship, Con;
    if (!Ship.Open(Dir / (Tag + ".shipped.part"), static_cast<size_t>(Sn), Err) || !Con.Open(Dir / (Tag + ".content.part"), static_cast<size_t>(Cn), Err))
        return false;
    std::vector<uint8_t> Plain;
    if (!coding::ExpandInto(Desc, Rec, Content, static_cast<uint8_t *>(Ship.M), static_cast<size_t>(Sn), static_cast<uint8_t *>(Con.M),
                            static_cast<size_t>(Cn), coding::SuggestedSpacing(D, Sn, true), Plain, Err))
        return false;
    //written beside its place and named once whole: a launch that dies here leaves no half twin behind
    const std::filesystem::path Part = Dir / (Tag + ".vgcr.part");
    {
        std::ofstream F(Part, std::ios::binary | std::ios::trunc);
        F.write(reinterpret_cast<const char *>(Plain.data()), static_cast<std::streamsize>(Plain.size()));
        F.close();
        if (!F) { std::filesystem::remove(Part, Ec); Err = "cannot write " + Part.string(); return false; }
    }
    std::filesystem::rename(Part, Out, Ec);
    if (Ec) { std::filesystem::remove(Part, Ec); Err = "cannot name " + Out.string(); return false; }
    return true;
#endif
}

size_t UsePlainRecords(nlohmann::ordered_json &Spec, const std::filesystem::path &Dir, bool Retry)
{
    auto It = Spec.find("layers");
    if (It == Spec.end() || !It->is_array()) return 0;
    auto &Layers = *It;
    bool Any = false;
    for (const auto &L : Layers)
        if (L.is_object() && L.value("type", std::string()) == "coded" && Magic(L.value("source", std::string()), "VGCQ")) { Any = true; break; }
    if (!Any) return 0;
#ifndef _WIN32
    { std::error_code Ec; std::filesystem::create_directories(Dir, Ec); }
    SweepStale(Dir);
#endif
    //the FS's own reading of the plan: the content of a coded layer is whatever layer carries its CID (a delta chain, a
    //delta over nothing), so its bytes come from the same code the mount runs
    ::Spec S; std::string Err;
    nlohmann::ordered_json Plan = Spec; Plan["readonly"] = true;
    if (!ParseSpecText(Plan.dump(), S, Err)) { LogErr("CodedCache::UsePlainRecords", "the mount plan does not parse (" + Err + "): coded layers read their predicted records"); return 0; }
    const TreePlan T = PlanTree(S);
    LayerBytes Bytes(S, T, std::vector<bool>(S.layers.size(), true));
    //what to expand: each predicted layer with its content's bytes (got here, in order: the plan's reader is one thread's)
    struct Job { size_t I = 0; std::string Name, Key, ContentCid, DescCid; std::shared_ptr<ByteSource> Content; std::filesystem::path Out; std::string Err; bool Ok = false, Transient = false; };
    std::vector<Job> Jobs;
    for (size_t I = 0; I < S.layers.size() && I < Layers.size(); ++I)
    {
        const LayerSpec &X = S.layers[I];
        if (X.type != LayerType::Coded || !Magic(X.source, "VGCQ")) continue;
        const std::string Name = X.name.empty() ? X.source : X.name;
        if (T.broken[I] || T.bases[I].empty()) { LogErr("CodedCache::UsePlainRecords", Name + ": its content is not in this launch; it reads as planned"); continue; }
        auto Content = Bytes.Get(T.bases[I][0], &Err);
        if (!Content) { LogErr("CodedCache::UsePlainRecords", Name + ": its content has no bytes (" + Err + "); it reads its predicted record"); continue; }
        //the description's CID: that of its canonical text (two texts that read alike key one twin)
        std::string DescCid;
        {   coding::Description D; std::string Why;
            if (coding::Read(X.description, D, Why)) { const auto B = coding::Serialize(D); DescCid = Cid::OfBytes(std::string(B.begin(), B.end())); } }
        if (DescCid.empty()) { LogErr("CodedCache::UsePlainRecords", Name + ": its description does not read; it reads as planned"); continue; }
        Job J; J.I = I; J.Name = Name; J.ContentCid = S.layers[T.bases[I][0]].cid; J.DescCid = DescCid; J.Content = std::move(Content);
        J.Key = Key(X.cid, J.ContentCid, DescCid);
        //a twin that could not be made is not tried at every launch (`coding twins --retry` tries again)
        if (!Retry && !Magic(Dir / (J.Key + ".vgcr"), "VGCR") && std::filesystem::exists(Dir / (J.Key + ".failed")))
        {
            std::ifstream W(Dir / (J.Key + ".failed")); const std::string Why((std::istreambuf_iterator<char>(W)), {});
            LogErr("CodedCache::UsePlainRecords", Name + ": its twin could not be made before (" + Why + "); it reads its predicted record");
            continue;
        }
        Jobs.push_back(std::move(J));
    }
    size_t Todo = 0;
    for (const Job &J : Jobs) if (!Magic(Dir / (J.Key + ".vgcr"), "VGCR")) ++Todo;
    if (Todo) LogOut("CodedCache::UsePlainRecords", "Preparing " + std::to_string(Todo) + " coded file(s) for this machine (once): expanding their records");
    //two at a time: an expansion holds its stream's decisions in memory (a 1.5 GB archive: about 1 GB)
    std::atomic<size_t> Next{0};
    size_t Done = 0;
    std::mutex LogMu;
    const auto Work = [&] {
        for (size_t K; (K = Next++) < Jobs.size();)
        {
            Job &J = Jobs[K];
            const LayerSpec &X = S.layers[J.I];
            try { J.Ok = PlainTwin(Dir, X.description, X.source, X.cid, J.ContentCid, J.DescCid, J.Content, J.Out, J.Err); }
            catch (const std::bad_alloc &) { J.Ok = false; J.Err = "the expansion ran out of memory"; J.Transient = true; }
            catch (const std::exception &E) { J.Ok = false; J.Err = std::string("the expansion failed: ") + E.what(); }   // about the data
            catch (...) { J.Ok = false; J.Err = "the expansion failed"; J.Transient = true; }
            //what is about this machine (room, memory, a file it could not make) is tried again next launch; what is about
            //the data (a record its content cannot code back) is remembered
            if (!J.Ok && (J.Err.rfind("not enough free disk", 0) == 0 || J.Err.rfind("no room", 0) == 0 || J.Err.rfind("cannot ", 0) == 0))
                J.Transient = true;
            std::lock_guard<std::mutex> G(LogMu);
            if (J.Ok) LogOut("CodedCache::UsePlainRecords", "  " + std::to_string(++Done) + "/" + std::to_string(Jobs.size()) + " " + J.Name);
            else
            {
                LogErr("CodedCache::UsePlainRecords", J.Name + ": no plain twin (" + J.Err + "); it reads its predicted record (the same bytes, slower)");
                std::error_code Ec;
                if (!J.Transient && std::filesystem::is_directory(Dir, Ec)) std::ofstream(Dir / (J.Key + ".failed")) << J.Err;
            }
        }
    };
    std::vector<std::thread> Th;
    for (size_t W = 0; W < std::min<size_t>(2, Jobs.size()); ++W) Th.emplace_back(Work);
    for (auto &X : Th) X.join();
    size_t Twins = 0;
    for (const Job &J : Jobs)
    {
        if (!J.Ok) continue;
        Layers[J.I]["source"] = J.Out.string();
        std::error_code Ec;
        std::filesystem::remove(Dir / (J.Key + ".failed"), Ec);
        ++Twins;
    }
    return Twins;
}
}
