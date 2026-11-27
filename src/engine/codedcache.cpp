#include "codedcache.h"

#include "bytesource.h"     // VidyaGodFS: MmapByteSource, FdByteSource
#include "coding.h"         // VidyaGodFS: the coding algebra (ExpandInto)
#include "commonutils.h"    // Log*
#include "layerspec.h"      // VidyaGodFS: ParseSpecText
#include "treeplan.h"       // VidyaGodFS: PlanTree, LayerBytes (a layer's bytes, delta chains included)

#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
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
#ifndef _WIN32
//A file of N bytes mapped writable: what an expansion writes the shipped bytes and the content into. File-backed, so
//gigabytes of it are page cache the kernel can drop, not memory the launch holds.
struct MappedFile
{
    std::filesystem::path P; int Fd = -1; void *M = MAP_FAILED; size_t N = 0;
    bool Open(const std::filesystem::path &Path, size_t Size)
    {
        P = Path; N = Size + 1;
        Fd = ::open(P.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (Fd < 0 || ::ftruncate(Fd, static_cast<off_t>(N)) != 0) return false;
        M = ::mmap(nullptr, N, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
        return M != MAP_FAILED;
    }
    ~MappedFile()
    {
        if (M != MAP_FAILED) ::munmap(M, N);
        if (Fd >= 0) ::close(Fd);
        std::error_code Ec;
        if (!P.empty()) std::filesystem::remove(P, Ec);
    }
};
#endif
}

std::string Key(const std::string &R, const std::string &C, const std::string &D) { return R + "." + C + "." + D; }

bool PlainTwin(const std::filesystem::path &Dir, const std::filesystem::path &Description, const std::filesystem::path &Record,
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
    auto Desc = std::make_shared<MmapByteSource>(), Rec = std::make_shared<MmapByteSource>();
    if (!Desc->Map(Description.string())) { Err = "cannot read its description " + Description.string(); return false; }
    if (!Rec->Map(Record.string())) { Err = "cannot read its record " + Record.string(); return false; }
    auto Probe = coding::CodedByteSource::Create(Desc, Rec, Content, 0, Err);
    if (!Probe) return false;
    const uint64_t Sn = Probe->size(), Cn = Content->size();
    Probe.reset();
    coding::Description D;
    if (!coding::Parse(Desc->p, static_cast<size_t>(Desc->n), D, Err)) return false;
    const std::string Tag = RecordKey + "." + std::to_string(::getpid());
    MappedFile Ship, Con;
    if (!Ship.Open(Dir / (Tag + ".shipped.part"), static_cast<size_t>(Sn)) || !Con.Open(Dir / (Tag + ".content.part"), static_cast<size_t>(Cn)))
    { Err = "cannot make the expansion's scratch files in " + Dir.string(); return false; }
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

size_t UsePlainRecords(nlohmann::ordered_json &Spec, const std::filesystem::path &Dir)
{
    auto It = Spec.find("layers");
    if (It == Spec.end() || !It->is_array()) return 0;
    auto &Layers = *It;
    bool Any = false;
    for (const auto &L : Layers)
        if (L.is_object() && L.value("type", std::string()) == "coded" && Magic(L.value("source", std::string()), "VGCQ")) { Any = true; break; }
    if (!Any) return 0;
    //the FS's own reading of the plan: the content of a coded layer is whatever layer carries its CID (a delta chain, a
    //delta over nothing), so its bytes come from the same code the mount runs
    ::Spec S; std::string Err;
    nlohmann::ordered_json Plan = Spec; Plan["readonly"] = true;
    if (!ParseSpecText(Plan.dump(), S, Err)) { LogErr("CodedCache::UsePlainRecords", "the mount plan does not parse (" + Err + "): coded layers read their predicted records"); return 0; }
    const TreePlan T = PlanTree(S);
    LayerBytes Bytes(S, T, std::vector<bool>(S.layers.size(), true));
    size_t Twins = 0;
    for (size_t I = 0; I < S.layers.size() && I < Layers.size(); ++I)
    {
        const LayerSpec &X = S.layers[I];
        if (X.type != LayerType::Coded || !Magic(X.source, "VGCQ")) continue;
        const std::string Name = X.name.empty() ? X.source : X.name;
        if (T.broken[I] || T.bases[I].empty()) { LogErr("CodedCache::UsePlainRecords", Name + ": its content is not in this launch; it reads as planned"); continue; }
        auto Content = Bytes.Get(T.bases[I][0], &Err);
        if (!Content) { LogErr("CodedCache::UsePlainRecords", Name + ": its content has no bytes (" + Err + "); it reads its predicted record"); continue; }
        //the description's CID: the file layer it was resolved from (ResolveCodedDescriptions made it that layer's path)
        std::string DescCid;
        for (const LayerSpec &Y : S.layers) if (Y.type == LayerType::File && Y.source == X.description && !Y.cid.empty()) { DescCid = Y.cid; break; }
        const std::string &ContentCid = S.layers[T.bases[I][0]].cid;
        std::filesystem::path Out;
        if (!Magic(Dir / (Key(X.cid, ContentCid, DescCid) + ".vgcr"), "VGCR"))
            LogOut("CodedCache::UsePlainRecords", "Preparing " + Name + " for this machine (once): expanding its record");
        if (!PlainTwin(Dir, X.description, X.source, X.cid, ContentCid, DescCid, Content, Out, Err))
        {
            LogErr("CodedCache::UsePlainRecords", Name + ": no plain twin (" + Err + "); it reads its predicted record (the same bytes, slower)");
            continue;
        }
        Layers[I]["source"] = Out.string();
        ++Twins;
    }
    return Twins;
}
}
