// `VidyaGod coding …` — the coding algebra (MetaPackageFormat chapter 22): shipped bytes (a compressed stream, an
// archive) described as DATA, decoded into their content and a record of the decisions, and coded back bit-exact.
// What a packager runs to make a coded layer, and what anyone runs to check one: describe a format, mint a shipped
// file (refused unless it codes back byte for byte), verify or rebuild it through the read path the mount uses.
// The older hand-wired deflate layer (one stream, its decisions) is here too.
#include "command.h"
#include "cmdutil.h"
#include "bytesource.h"
#include "coding.h"
#include "viewmode.h"
#include "treelayer.h"
#include "vgdelta.h"
#include "ziplayer.h"
#include "zipscan.h"
#include "layerspec.h"
#include "lz4family.h"
#include "codedcache.h"
#include "apppaths.h"
#include "deflatelayer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/mman.h>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

std::vector<uint8_t> Slurp(const std::string &P)
{
    std::ifstream F(P, std::ios::binary);
    if (!F) throw Command::BadArgs("cannot read " + P);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(F), {});
}
//Written beside its place and renamed over it once whole: a failed write leaves nothing half-written.
void Spit(const std::string &P, const uint8_t *B, size_t N)
{
    const std::string Tmp = P + ".part";
    {
        std::ofstream F(Tmp, std::ios::binary | std::ios::trunc);
        F.write(reinterpret_cast<const char *>(B), static_cast<std::streamsize>(N));
        F.close();
        if (!F) { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + P); }
    }
    if (std::rename(Tmp.c_str(), P.c_str()) != 0) { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + P); }
}
std::shared_ptr<ByteSource> Map(const std::string &P)
{
    auto M = std::make_shared<MmapByteSource>();
    if (!M->Map(P)) throw Command::BadArgs("cannot read " + P + " (missing or empty)");
    return M;
}
coding::Description Described(const std::string &P)
{
    const auto B = Slurp(P);
    coding::Description D;
    std::string Err;
    if (!coding::Parse(B.data(), B.size(), D, Err)) throw std::runtime_error("the description " + P + ": " + Err);
    return D;
}

// ---- TREE (.vgtz) tools: a base's files, by path, as bytes (an archive's stored members, a view's files, one file)
struct TreeBaseFiles
{
    std::vector<std::pair<std::string, std::shared_ptr<ByteSource>>> files;   // path → bytes ("" a single file itself)
};
TreeBaseFiles LoadTreeBase(const json &B)
{
    TreeBaseFiles Out;
    const std::string Kind = B.value("kind", std::string()), Path = B.value("path", std::string());
    if (Kind == "file") { Out.files.push_back({ std::string(), Map(Path) }); return Out; }
    if (Kind == "zip")
    {
        auto Src = Map(Path);
        auto Z = BuildZipIndex(Src, Path);
        if (!Z) throw std::runtime_error("the base " + Path + " is not a zip");
        for (const auto &[Name, E] : Z->byName)
        {
            if (E.isDir || E.symlink) continue;
            if (!E.stored) { std::fprintf(stderr, "tree make: the base %s's %s is compressed: no file is read from it\n", Path.c_str(), Name.c_str()); continue; }
            const uint64_t D = ZipDataOffset(*Z, E);
            if (D != UINT64_MAX) Out.files.push_back({ Name, std::make_shared<SliceByteSource>(Src, D, E.size) });
        }
        std::sort(Out.files.begin(), Out.files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });   // one order, every run
        return Out;
    }
    if (Kind == "view")
    {
        auto C = Map(B.value("content", std::string()));
        const auto L = Slurp(Path);
        std::vector<coding::ViewEntry> V; std::string Err;
        if (!coding::ReadViewListing(std::string(L.begin(), L.end()), V, Err, C->size())) throw std::runtime_error("the view " + Path + ": " + Err);
        for (const auto &E : V)
        {
            if (E.dir) continue;
            if (E.extents.size() == 1) { Out.files.push_back({ E.path, std::make_shared<SliceByteSource>(C, E.extents[0].first, E.extents[0].second) }); continue; }
            std::vector<std::shared_ptr<ByteSource>> Parts;
            for (const auto &[O, N] : E.extents) Parts.push_back(std::make_shared<SliceByteSource>(C, O, N));
            Out.files.push_back({ E.path, std::make_shared<ConcatByteSource>(Parts) });
        }
        return Out;
    }
    throw Command::BadArgs("a base's kind is file, zip or view (got '" + Kind + "')");
}
// A file read when asked, its descriptor open only for that read: an install of a hundred thousand files holds none.
struct LazyFile : ByteSource
{
    std::string path; uint64_t n = 0;
    LazyFile(std::string P, uint64_t N) : path(std::move(P)), n(N) {}
    ssize_t pread(void *buf, size_t len, uint64_t off) override
    {
        if (off >= n) return 0;
        std::FILE *F = std::fopen(path.c_str(), "rb");
        if (!F) return -1;
        const size_t want = (size_t)std::min<uint64_t>(len, n - off);
        const ssize_t got = fseeko(F, (off_t)off, SEEK_SET) == 0 ? (ssize_t)std::fread(buf, 1, want, F) : -1;
        std::fclose(F);
        return got;
    }
    uint64_t size() const override { return n; }
};
// Scratch removed however the call ends.
struct Scratch { std::vector<std::string> paths; ~Scratch() { for (const auto &P : paths) std::remove(P.c_str()); } };
uint64_t HashOf(ByteSource &S)
{
    std::vector<uint8_t> B(4u << 20);
    uint64_t H = 1469598103934665603ull;
    for (uint64_t O = 0; O < S.size(); O += B.size())
    {
        const size_t N = (size_t)std::min<uint64_t>(B.size(), S.size() - O);
        if (!S.preadAll(B.data(), N, O)) throw std::runtime_error("a base file does not read");
        for (size_t i = 0; i < N; ++i) { H ^= B[i]; H *= 1099511628211ull; }
    }
    return H;
}
bool SameBytes(ByteSource &A, ByteSource &B)
{
    if (A.size() != B.size()) return false;
    std::vector<uint8_t> X(4u << 20), Y(4u << 20);
    for (uint64_t O = 0; O < A.size(); O += X.size())
    {
        const size_t N = (size_t)std::min<uint64_t>(X.size(), A.size() - O);
        if (!A.preadAll(X.data(), N, O) || !B.preadAll(Y.data(), N, O) || std::memcmp(X.data(), Y.data(), N)) return false;
    }
    return true;
}
std::vector<uint8_t> AllOf(ByteSource &S)
{
    std::vector<uint8_t> B((size_t)S.size());
    if (!S.preadAll(B.data(), B.size(), 0)) throw std::runtime_error("a file does not read");
    return B;
}
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Read Src front to back in 8 MiB pieces (asking ahead first, as a layer above would), comparing with Want or writing to
// Out; the read path the mount uses. Returns the seconds it took.
double ReadThrough(ByteSource &Src, const uint8_t *Want, std::FILE *Out)
{
    std::vector<uint8_t> B(8u << 20);
    const double T0 = Now();
    for (uint64_t O = 0; O < Src.size(); O += B.size())
    {
        const size_t N = static_cast<size_t>(std::min<uint64_t>(B.size(), Src.size() - O));
        Src.prefetch(O + N, std::min<uint64_t>(B.size(), Src.size() - std::min(Src.size(), O + N)));
        if (!Src.preadAll(B.data(), N, O)) throw std::runtime_error("read failed at " + std::to_string(O));
        if (Want && std::memcmp(B.data(), Want + O, N)) throw std::runtime_error("DIFFERENT in [" + std::to_string(O) + ", " + std::to_string(O + N) + ")");
        if (Out && std::fwrite(B.data(), 1, N, Out) != N) throw std::runtime_error("cannot write");
    }
    return Now() - T0;
}
}

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    using Command::AsPath;
    Command::RegisterGroup("coding", "the coding algebra: check a description, mint a shipped file into content + record, verify it back");

    Command::Register({
        "coding check", "read and validate a description (alone, or the one a node's CODED layers carry); write its canonical text",
        "A description is data a CODED layer carries inline (MetaPackageFormat 04, 22.16): readable JSON, one instruction a string. It is checked "
        "as a package's would be (every op, register, table and label in bounds; nothing newer than this build's "
        "instruction set). --out: its canonical text (the same names; values laid out one canonical way).",
        { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), AsPath(Opt("out", "the canonical text to write")) },
        { "coding check ~/x/BSA105.json", "coding check ~/x/inno.json --out ~/x/inno.canonical.json" },
        { "coding::Read", "coding::Print", "coding::Validate" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const auto B = Slurp(Command::Str(A, "description"));
            coding::Description D; coding::TextNames N; std::string Err, Text;
            if (!coding::Read(std::string(B.begin(), B.end()), D, Err, &N) || !coding::Print(D, Text, Err, &N))
                throw std::runtime_error("the description " + Command::Str(A, "description") + ": " + Err);
            if (const std::string O = Command::Str(A, "out", ""); !O.empty()) Spit(O, reinterpret_cast<const uint8_t *>(Text.data()), Text.size());
            return json{{"name", D.name}, {"isa", D.isa}, {"instructions", D.ops.size()}, {"values", D.ex.size()},
                        {"family", coding::UsesPrediction(D)}, {"canonical", Text == std::string(B.begin(), B.end())}};
        }, {} });

    Command::RegisterGroup("view", "VIEW descriptions (ISA 10): the file table a description reads out of a disc image or archive");
    Command::Register({
        "view ls", "run a VIEW description over a content and list the files in it (the canonical .vgview listing)",
        "A VIEW (MetaPackageFormat 22.17) is data: run over a content (a disc image, an archive), it lists the folders and "
        "files in it, each file's bytes as ranges of the content. The run is bounded (entries, names, fuel) and refused for "
        "anything a view may not list. --out: the listing to write (its CID is the VIEW layer's identity).",
        { AsPath(Pos("description", "a VIEW description JSON")), AsPath(Pos("content", "the content it reads")), AsPath(Opt("out", "the .vgview listing to write")) },
        { "view ls 'ISO9660 view v1.json' disc.iso --out disc.vgview" },
        { "coding::RunView", "coding::ViewListing", "coding::DecodeViewName", "coding::GoodViewPath", "coding::PrintablePath",
          "coding::ValidUtf8Text", "coding::ViewEntry::size" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const auto B = Slurp(Command::Str(A, "description"));
            coding::Description D; std::string Err;
            if (!coding::Read(std::string(B.begin(), B.end()), D, Err)) throw std::runtime_error("the description: " + Err);
            const auto C = Map(Command::Str(A, "content"));
            std::vector<coding::ViewEntry> V;
            const double T0 = Now();
            if (!coding::RunView(D, *C, V, Err)) throw std::runtime_error("the view: " + Err);
            const std::string L = coding::ViewListing(V);
            if (const std::string O = Command::Str(A, "out", ""); !O.empty()) Spit(O, reinterpret_cast<const uint8_t *>(L.data()), L.size());
            uint64_t Files = 0, Bytes = 0;
            for (const auto &E : V) if (!E.dir) { ++Files; Bytes += E.size(); }
            return json{{"name", D.name}, {"entries", V.size()}, {"files", Files}, {"folders", V.size() - Files}, {"bytes", Bytes},
                        {"content", C->size()}, {"listing_bytes", L.size()}, {"seconds", Now() - T0}};
        }, {} });
    Command::Register({
        "view verify", "check a stored .vgview listing: well formed, and what its description lists over its content",
        "Reads the listing as a mount would (refusing anything a listing may not hold), then runs the description over the "
        "content and compares byte for byte.",
        { AsPath(Pos("description", "the VIEW description JSON")), AsPath(Pos("content", "the content it reads")), AsPath(Pos("listing", "the .vgview listing")) },
        { "view verify 'ISO9660 view v1.json' disc.iso disc.vgview" },
        { "coding::ReadViewListing" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const auto B = Slurp(Command::Str(A, "description")), Ls = Slurp(Command::Str(A, "listing"));
            coding::Description D; std::string Err;
            if (!coding::Read(std::string(B.begin(), B.end()), D, Err)) throw std::runtime_error("the description: " + Err);
            const auto C = Map(Command::Str(A, "content"));
            std::vector<coding::ViewEntry> Stored, V;
            if (!coding::ReadViewListing(std::string(Ls.begin(), Ls.end()), Stored, Err, C->size())) throw std::runtime_error("the listing: " + Err);
            if (!coding::RunView(D, *C, V, Err)) throw std::runtime_error("the view: " + Err);
            const bool Same = coding::ViewListing(V) == std::string(Ls.begin(), Ls.end());
            if (!Same) throw std::runtime_error("the listing is NOT what the description lists over this content");
            return json{{"entries", Stored.size()}, {"identical", true}};
        }, {} });

    Command::RegisterGroup("tree", "TREE (.vgtz): an install as files over the bytes of its bases (views, archives, files), readable: a zip of TREE.txt, patches and adds");
    Command::Register({
        "tree make", "make a .vgtz: the target install's files read from its bases where they are found, the rest added",
        "--bases: a JSON list, in BASE_SOURCES order: {\"cid\", \"kind\": \"view\"|\"zip\"|\"file\", \"path\", \"content\" (a view's)}. "
        "Each target file is matched whole against every base file (BASE); else against the base file of its own path or name "
        "(DELTA: the copy runs the delta matcher finds, the rest added); else ADD. --target: the install, a folder or a STORE zip. "
        "The tree is verified file by file against the target before it is named. --work: scratch (the SSD).",
        { AsPath(Pos("tree", "the .vgtz to write")), AsPath(Opt("target", "the install: a folder or a STORE zip")), AsPath(Opt("bases", "the bases JSON")),
          AsPath(Opt("work", "scratch folder (default: beside the tree)")) },
        { "tree make install.vgtz --target install.zip --bases bases.json --work ~/.fastwork/m2/x" },
        { "vgtree::WriteTree", "vgtree::TableText", "vgtree::ReadTable", "vgtree::OpenTree", "vgtree::EntryBytes", "vgtree::InsideLink", "vgtree::PatchInfo", "vgtree::SeekableZstdTo", "vgtree::Lazily", "vgtree::MakePatch", "vgtree::ApplyPatch", "vgtree::TextEncoding", "vgtree::SeekableZstd", "vgtree::SeekableZstdSource",
          "SliceByteSource::pread", "SliceByteSource::prefetch", "SliceByteSource::size" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const double T0 = Now();
            const std::string Out = Command::Str(A, "tree");
            const auto BJ = json::parse(Slurp(Command::Str(A, "bases")));
            if (!BJ.is_array()) throw Command::BadArgs("--bases is a JSON list");
            vgtree::Table T;
            std::vector<TreeBaseFiles> Bases;
            for (const auto &B : BJ) { T.bases.push_back(B.value("cid", std::string())); Bases.push_back(LoadTreeBase(B)); }
            //every base file by (size, hash); by path; by name
            std::map<std::pair<uint64_t, uint64_t>, std::vector<std::pair<uint32_t, size_t>>> ByHash;
            std::map<std::string, std::vector<std::pair<uint32_t, size_t>>> ByPath, ByName;
            uint64_t BaseBytes = 0;
            for (uint32_t i = 0; i < Bases.size(); ++i)
                for (size_t k = 0; k < Bases[i].files.size(); ++k)
                {
                    auto &F = Bases[i].files[k];
                    BaseBytes += F.second->size();
                    ByHash[{ F.second->size(), HashOf(*F.second) }].push_back({ i, k });
                    ByPath[F.first].push_back({ i, k });
                    std::string N = F.first.substr(F.first.rfind('/') == std::string::npos ? 0 : F.first.rfind('/') + 1);
                    for (char &c : N) c = (char)std::tolower((unsigned char)c);
                    ByName[N].push_back({ i, k });
                }
            //the target's files, links and folders
            struct Tf { std::string path; std::shared_ptr<ByteSource> bytes; uint32_t mode = 0; bool dir = false; std::string link; };
            std::vector<Tf> Target;
            const std::string Tp = Command::Str(A, "target");
            std::shared_ptr<MmapByteSource> TZip;
            if (std::filesystem::is_directory(Tp))
            {
                for (auto It = std::filesystem::recursive_directory_iterator(Tp); It != std::filesystem::recursive_directory_iterator(); ++It)
                {
                    const std::string Rel = It->path().lexically_relative(Tp).generic_string();   // the link itself, never its target
                    Tf X; X.path = Rel;
                    const auto St = It->symlink_status();
                    if (std::filesystem::is_symlink(St)) { X.link = std::filesystem::read_symlink(It->path()).generic_string(); if (X.link.empty() || X.link.size() > 4096) throw std::runtime_error("the target's link " + Rel + " names no target, or one over 4096 bytes"); }
                    else if (std::filesystem::is_directory(St)) X.dir = true;
                    else if (std::filesystem::is_regular_file(St)) { X.bytes = std::make_shared<LazyFile>(It->path().string(), (uint64_t)std::filesystem::file_size(It->path())); X.mode = (uint32_t)St.permissions() & 0777; }
                    else throw std::runtime_error("the target holds " + Rel + ", which is neither a file, a folder nor a link");
                    Target.push_back(std::move(X));
                }
            }
            else
            {
                TZip = std::static_pointer_cast<MmapByteSource>(Map(Tp));
                bool Ok = zipscan::WalkCentralDirFull(*TZip, [&](const zipscan::CdEntry &E) {
                    Tf X; X.path = CleanMemberName(E.utf8Name);
                    if (X.path.empty()) return true;
                    const uint32_t Unix = (E.madeBy >> 8) == 3 ? (E.extAttr >> 16) : 0;
                    const uint64_t D = zipscan::LocalDataOffset(*TZip, E.lhOffset);
                    if (E.method != 0 || D == UINT64_MAX) throw std::runtime_error("the target zip's member " + X.path + " is not STORED");
                    if (!E.utf8Name.empty() && E.utf8Name.back() == '/') X.dir = true;
                    else if ((Unix & 0170000) == 0120000)
                    {
                        if (E.uncompSize > 4096) throw std::runtime_error("the target zip's link " + X.path + " names a target over 4096 bytes");
                        std::vector<uint8_t> L((size_t)E.uncompSize);
                        if (L.empty() || !TZip->preadAll(L.data(), L.size(), D)) throw std::runtime_error("the target zip's link " + X.path + " names nothing");
                        X.link.assign(L.begin(), L.end());
                    }
                    else { X.bytes = std::make_shared<SliceByteSource>(TZip, D, E.uncompSize); X.mode = Unix & 0777; }   // permission bits only, as a folder's
                    Target.push_back(std::move(X));
                    return true; });
                if (!Ok) throw std::runtime_error("the target is neither a folder nor a zip");
            }
            std::sort(Target.begin(), Target.end(), [](const Tf &a, const Tf &b) { return a.path < b.path; });   // one order: the same tree every run
            for (size_t i = 1; i < Target.size(); ++i) if (Target[i].path == Target[i - 1].path) throw std::runtime_error("the target lists " + Target[i].path + " twice");
            {   //a tree's links stay inside it (22.18.2): every one that does not, named, before any work
                std::string Out; size_t N = 0;
                for (const Tf &X : Target) if (!X.link.empty() && !vgtree::InsideLink(X.path, X.link)) { if (++N <= 20) Out += "\n  " + X.path + " -> " + X.link; }
                if (N) throw std::runtime_error(std::to_string(N) + " link(s) of the target leave it (absolute, or above its top); a tree cannot hold them:" + Out + (N > 20 ? "\n  …" : ""));
            }
            //each file: whole (BASE), a delta over its counterpart (DELTA), or added (ADD)
            const std::string Work = Command::Str(A, "work", std::filesystem::absolute(Out).parent_path().string());
            std::filesystem::create_directories(Work);
            const std::string AddPath = Work + "/" + std::filesystem::path(Out).filename().string() + ".add";
            Scratch Sc; Sc.paths = { AddPath, Out + ".making" };
            std::FILE *AddF = std::fopen(AddPath.c_str(), "wb");
            if (!AddF) throw std::runtime_error("cannot write " + AddPath);
            struct Closer { std::FILE *&F; ~Closer() { if (F) std::fclose(F); } } Cl{ AddF };
            uint64_t AddLen = 0, NSame = 0, NPatch = 0, NPieces = 0, NNew = 0, BytesBase = 0, BytesAdded = 0;
            struct AddAt { uint64_t off, len; bool text; };
            std::map<std::string, AddAt> AddsAt;                 // each file's new bytes: where they lie in the scratch
            std::map<std::string, std::string> Patches;
            const auto Append = [&](ByteSource &S, uint64_t O, uint64_t N) {
                std::vector<uint8_t> B(4u << 20);
                for (uint64_t k = 0; k < N; k += B.size()) { const size_t n = (size_t)std::min<uint64_t>(B.size(), N - k); if (!S.preadAll(B.data(), n, O + k) || std::fwrite(B.data(), 1, n, AddF) != n) throw std::runtime_error("cannot add"); }
                const uint64_t At = AddLen; AddLen += N; BytesAdded += N; return At;
            };
            const auto Zero = [](const uint8_t *P, size_t N) { for (size_t i = 0; i < N; ++i) if (P[i]) return false; return true; };
            std::set<std::string> Dirs;
            for (Tf &X : Target)
            {
                if (X.dir) { Dirs.insert(X.path); continue; }
                for (size_t S = X.path.find('/'); S != std::string::npos; S = X.path.find('/', S + 1)) Dirs.insert(X.path.substr(0, S));
                vgtree::Entry E; E.path = X.path;
                if (!X.link.empty()) { E.kind = vgtree::Entry::Link; E.link = X.link; T.entries.push_back(std::move(E)); continue; }
                E.size = X.bytes->size(); E.mode = X.mode;
                bool Done = false;
                if (E.size)                                       // the same bytes in a base: "same"
                    if (const auto It = ByHash.find({ E.size, HashOf(*X.bytes) }); It != ByHash.end())
                        for (const auto &[i, k] : It->second)
                            if (SameBytes(*X.bytes, *Bases[i].files[k].second)) { E.op = vgtree::Entry::Same; E.base = i; E.basePath = Bases[i].files[k].first; Done = true; ++NSame; BytesBase += E.size; break; }
                if (!Done && E.size && E.size <= (512ull << 20))
                {   //its counterpart: the base file of its path, else of its name (case aside, one such)
                    std::string N = X.path.substr(X.path.rfind('/') == std::string::npos ? 0 : X.path.rfind('/') + 1);
                    for (char &c : N) c = (char)std::tolower((unsigned char)c);
                    const std::vector<std::pair<uint32_t, size_t>> *C = nullptr;
                    if (const auto P = ByPath.find(X.path); P != ByPath.end()) C = &P->second;
                    else if (const auto Q = ByName.find(N); Q != ByName.end() && Q->second.size() == 1) C = &Q->second;
                    if (C && !C->empty())
                    {
                        const auto [i, k] = C->front();
                        auto &Bf = *Bases[i].files[k].second;
                        if (Bf.size() && Bf.size() <= (512ull << 20))
                        {
                            const auto Bb = AllOf(Bf), Tb = AllOf(*X.bytes);
                            //a text file: a real patch, when one rebuilds it exactly
                            std::string Pt;
                            if (vgtree::MakePatch(std::string(Bb.begin(), Bb.end()), std::string(Tb.begin(), Tb.end()), Bases[i].files[k].first, X.path, Pt))
                            {
                                E.op = vgtree::Entry::Patch; E.base = i; E.basePath = Bases[i].files[k].first;
                                Patches[X.path] = std::move(Pt); Done = true; ++NPatch;
                            }
                            else
                            {   //binary: the delta matcher's copy runs kept, its literals new, zero runs zero
                                const auto Dl = vgdelta::GenerateDelta(Bb.data(), Bb.size(), Tb.data(), Tb.size(), vgdelta::DEFAULT_BLOCK, Work);
                                std::string W;
                                auto Ds = Dl.empty() ? nullptr : vgdelta::DeltaByteSource::Create(std::make_shared<MemByteSource>(Dl), std::make_shared<MemByteSource>(Bb), W, false);
                                uint64_t Copied = 0;
                                if (Ds) for (const auto &R : Ds->flatRuns()) if (R.kind == vgdelta::KIND_COPY) Copied += R.len;
                                if (Ds && Copied * 10 >= E.size)          // worth it: a tenth or more of it from the base
                                {
                                    //its pieces first (touching ones of a kind joined), then its new bytes added
                                    struct Run { vgtree::Piece pc; uint64_t at; };
                                    std::vector<Run> Runs;
                                    for (const auto &R : Ds->flatRuns())
                                    {
                                        vgtree::Piece Pc; Pc.len = R.len;
                                        const uint8_t *Src = R.kind == vgdelta::KIND_COPY ? Bb.data() + R.srcOff : Tb.data() + R.targetOff;
                                        if (Zero(Src, (size_t)R.len)) Pc.kind = vgtree::Piece::Zero;
                                        else if (R.kind == vgdelta::KIND_COPY) { Pc.kind = vgtree::Piece::Keep; Pc.off = R.srcOff; }
                                        else Pc.kind = vgtree::Piece::New;
                                        if (!Runs.empty() && Runs.back().pc.kind == Pc.kind && (Pc.kind != vgtree::Piece::Keep || Runs.back().pc.off + Runs.back().pc.len == Pc.off)) Runs.back().pc.len += Pc.len;
                                        else Runs.push_back({ Pc, R.targetOff });
                                    }
                                    const size_t Kept = (size_t)std::count_if(Runs.begin(), Runs.end(), [](const Run &R) { return R.pc.kind == vgtree::Piece::Keep; });
                                    E.base = i; E.basePath = Bases[i].files[k].first;
                                    if (Runs.size() == 1 && Kept == 1)
                                    {   //one run of its base file: a range of it
                                        E.op = vgtree::Entry::Range; E.off = Runs[0].pc.off; E.len = Runs[0].pc.len;
                                        BytesBase += E.len; Done = true; ++NPieces;
                                    }
                                    else if (Kept)
                                    {
                                        E.op = vgtree::Entry::Pieces;
                                        uint64_t Mine = 0, Off0 = 0; bool Started = false;
                                        for (const Run &R : Runs)
                                        {
                                            if (R.pc.kind == vgtree::Piece::Keep) BytesBase += R.pc.len;
                                            if (R.pc.kind == vgtree::Piece::New) { const uint64_t At = Append(*X.bytes, R.at, R.pc.len); if (!Started) { Off0 = At; Started = true; } Mine += R.pc.len; }
                                            E.pieces.push_back(R.pc);
                                        }
                                        if (Mine) AddsAt[X.path] = { Off0, Mine, false };
                                        Done = true; ++NPieces;
                                    }   //nothing kept (its copies all zeros): new
                                }
                            }
                        }
                    }
                }
                if (!Done)
                {   //new: its bytes, readable when they are text
                    E.op = vgtree::Entry::New; ++NNew;
                    if (E.size) {
                        const auto Tb = E.size <= (64ull << 20) ? AllOf(*X.bytes) : std::vector<uint8_t>();
                        std::string Enc;
                        const bool Text = !Tb.empty() && vgtree::TextEncoding(Tb.data(), Tb.size(), Enc);
                        AddsAt[X.path] = { Append(*X.bytes, 0, E.size), E.size, Text };
                    }
                }
                T.entries.push_back(std::move(E));
            }
            std::fclose(AddF); AddF = nullptr;
            for (const std::string &D : Dirs) { vgtree::Entry E; E.path = D; E.kind = vgtree::Entry::Dir; T.entries.push_back(std::move(E)); }
            std::sort(T.entries.begin(), T.entries.end(), [](const vgtree::Entry &a, const vgtree::Entry &b) { return a.path < b.path; });
            std::string Err;
            {
                auto Add = AddLen ? Map(AddPath) : nullptr;
                const uint8_t *Ap = AddLen ? std::static_pointer_cast<MmapByteSource>(Add)->p : nullptr;
                std::map<std::string, vgtree::AddBytes> Adds;
                for (const auto &[P, At] : AddsAt) Adds[P] = { Ap + At.off, (size_t)At.len, At.text };
                if (!vgtree::WriteTree(Out + ".making", T, Adds, Patches, Err)) throw std::runtime_error("the tree could not be made: " + Err);
            }
            //verified, file by file, through the same reads a mount makes
            vgtree::Opened Made;
            if (!vgtree::OpenTree(Map(Out + ".making"), Made, Err)) throw std::runtime_error("the tree made does not open: " + Err);
            const vgtree::Table &Back = Made.table;
            std::vector<std::map<std::string, std::shared_ptr<ByteSource>>> ByBase(Bases.size());
            for (size_t i = 0; i < Bases.size(); ++i) for (const auto &F : Bases[i].files) ByBase[i].emplace(F.first, F.second);
            std::map<std::string, const Tf *> ByTarget;
            for (const Tf &X : Target) ByTarget.emplace(X.path, &X);
            const auto BaseFile = [&](uint32_t i, const std::string &P) -> std::shared_ptr<ByteSource> {
                if (i >= ByBase.size()) return nullptr;
                const auto It = ByBase[i].find(P);
                return It == ByBase[i].end() ? nullptr : It->second; };
            size_t Checked = 0;
            for (size_t i = 0; i < Back.entries.size(); ++i)
            {
                const auto &E = Back.entries[i];
                if (E.kind != vgtree::Entry::File) continue;
                const auto It = ByTarget.find(E.path);
                auto Got = vgtree::EntryBytes(E, BaseFile, Made, Err);
                if (It == ByTarget.end() || !Got || !It->second->bytes || !SameBytes(*Got, *It->second->bytes)) throw std::runtime_error("the tree does not rebuild " + E.path + (Err.empty() ? "" : ": " + Err));
                ++Checked;
            }
            std::filesystem::rename(Out + ".making", Out);
            Sc.paths = { AddPath };
            return json{{"entries", Back.entries.size()}, {"files", Checked}, {"same", NSame}, {"patch", NPatch}, {"pieces", NPieces}, {"new", NNew},
                        {"bytes_from_bases", BytesBase}, {"bytes_added", BytesAdded}, {"tree_bytes", std::filesystem::file_size(Out)},
                        {"base_bytes_scanned", BaseBytes}, {"seconds", Now() - T0}, {"verified", true}};
        }, {} });
    Command::Register({
        "tree ls", "a .vgtz's table: its bases and every entry, with the op that makes it",
        "", { AsPath(Pos("tree", "the .vgtz")) }, { "tree ls install.vgtz" },
        {}, Needs::Config, false,
        [](Command::Context &, const json &A) {
            vgtree::Opened Tr; std::string Err, Text;
            if (!vgtree::OpenTree(Map(Command::Str(A, "tree")), Tr, Err) || !vgtree::TableText(Tr.table, Text, Err)) throw std::runtime_error(Err);
            const vgtree::Table &T = Tr.table;
            std::fwrite(Text.data(), 1, Text.size(), stdout);
            return json{{"entries", T.entries.size()}, {"bases", T.bases}};
        }, {} });

    Command::Register({
        "coding info", "what a description is: its name, instruction set, size, and the checkpoint spacing it suits",
        "A description is checked as the mount checks one (every op, register and table in bounds; nothing newer than "
        "this build's instruction set). --shipped: the spacing for a stream of that size.",
        { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), Opt("shipped", "a shipped stream's size in bytes", Type::Int) },
        { "coding info ~/x/BSA105.json --shipped 1073741824" },
        { "coding::Parse", "coding::SuggestedSpacing" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const coding::Description D = Described(Command::Str(A, "description"));
            return json{{"name", D.name}, {"isa", D.isa}, {"this_build_isa", coding::kIsa}, {"ops", D.ops.size()},
                        {"expressions", D.ex.size()}, {"slots", D.nslots}, {"window", D.window}, {"family", coding::UsesPrediction(D)},
                        {"spacing", coding::SuggestedSpacing(D, static_cast<uint64_t>(Command::Int(A, "shipped", 0)))},
                        {"plain_spacing", coding::SuggestedSpacing(D, static_cast<uint64_t>(Command::Int(A, "shipped", 0)), true)}};
        }, {} });

    Command::Register({
        "coding mint", "decode a shipped file into its content and a record, and refuse unless it codes back bit-exact",
        "The content is what the description decodes (an archive's header region and members, a stream's bytes); the record "
        "holds the decisions and checkpoints. Nothing is written unless every shipped byte codes back exactly. A description "
        "with a family mints a PREDICTED record (only what the encoder did otherwise than its family: small, for keeping and "
        "sending; a machine reads its expansion, `coding expand`); --plain mints every decision. --spacing: shipped bytes "
        "between checkpoints (default: what the description suits). --in-memory: hold the content in an ordinary buffer "
        "(small inputs; the default reserves a lazy 4 GiB map, paid for only as the decode fills it).",
        { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), AsPath(Pos("shipped", "the shipped file")), AsPath(Pos("content", "the content to write")),
          AsPath(Pos("record", "the record to write")), Opt("spacing", "shipped bytes between checkpoints", Type::Int),
          Flag("in-memory", "decode into an ordinary buffer"), Flag("plain", "every decision in the record (no family)") },
        { "coding mint ~/x/BSA105.json 'Skyrim - Interface.bsa' Interface.content Interface.vgcr" },
        { "coding::MintInto", "coding::Mint", "coding::UsesPrediction", "coding::Lz4hcParser::parse", "coding::ZlibParser::ValidKnob",
          "coding::ZlibParser::ensure", "coding::ZlibParser::at", "coding::ZlibParser::reached", "coding::ZlibParser::lo", "coding::ZlibParser::kept",
          "coding::ZlibParser::tokens" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const coding::Description D = Described(Command::Str(A, "description"));
            MmapByteSource S;
            if (!S.Map(Command::Str(A, "shipped"))) throw Command::BadArgs("cannot read " + Command::Str(A, "shipped"));
            const bool Plain = Command::Bool(A, "plain", false);
            const uint64_t Spacing = A.contains("spacing") ? static_cast<uint64_t>(Command::Int(A, "spacing")) : coding::SuggestedSpacing(D, S.n, Plain);
            std::vector<uint8_t> Rec;
            std::string Err;
            size_t Len = 0;
            const double T0 = Now();
            if (Command::Bool(A, "in-memory", false))
            {
                std::vector<uint8_t> Content;
                const size_t Cap = static_cast<size_t>(std::min<uint64_t>(S.n * 64 + (1u << 20), 0xFFFFFFFFu));
                if (!coding::Mint(D, S.p, static_cast<size_t>(S.n), 0, Cap, Spacing, Content, Rec, Err, !Plain))
                    throw std::runtime_error("refused: " + Err);
                Len = Content.size();
                Spit(Command::Str(A, "content"), Content.data(), Content.size());
            }
            else
            {
                const size_t Cap = 0xFFFFFFFFu;
                void *Buf = ::mmap(nullptr, Cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (Buf == MAP_FAILED) throw std::runtime_error("cannot reserve the content buffer");
                struct Unmap { void *P; size_t N; ~Unmap() { ::munmap(P, N); } } U{ Buf, Cap };
                if (!coding::MintInto(D, S.p, static_cast<size_t>(S.n), 0, static_cast<uint8_t *>(Buf), Cap, Spacing, Len, Rec, Err, !Plain))
                    throw std::runtime_error("refused: " + Err);
                Spit(Command::Str(A, "content"), static_cast<const uint8_t *>(Buf), Len);
            }
            Spit(Command::Str(A, "record"), Rec.data(), Rec.size());
            return json{{"description", D.name}, {"shipped", S.n}, {"content", Len}, {"record", Rec.size()},
                        {"record_kind", Rec.size() > 3 && Rec[3] == 'Q' ? "predicted" : "plain"}, {"spacing", Spacing},
                        {"seconds", Now() - T0}, {"codes_back", "bit-exact"}};
        }, {} });

    Command::Register({
        "coding twins", "make the plain twins a mount spec's predicted coded layers read (this machine's cache)",
        "What a launch does before it mounts (VfsMount): every coded layer whose record is predicted gets its plain twin in "
        "<data root>/CACHE/coded, made here if it is not there yet, so the first launch does not wait for it. The spec is the "
        "mount spec a launch writes (vidyagodfs.spec.json in its temporary folder) or any spec of that shape. --out: write "
        "the spec with the twins in place.",
        { AsPath(Pos("spec", "a vidyagodfs mount spec (JSON)")), AsPath(Opt("out", "write the spec reading the twins here")),
          Flag("retry", "try again the twins that could not be made before") },
        { "coding twins /tmp/vg/vidyagodfs.spec.json" },
        { "CodedCache::UsePlainRecords", "CodedCache::PlainTwin" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::ifstream F(Command::Str(A, "spec"));
            if (!F) throw Command::BadArgs("cannot read " + Command::Str(A, "spec"));
            json Spec = json::parse(F, nullptr, false);
            if (Spec.is_discarded() || !Spec.is_object()) throw Command::BadArgs(Command::Str(A, "spec") + " is not a mount spec");
            const double T0 = Now();
            const size_t N = CodedCache::UsePlainRecords(Spec, AppPaths::DataRoot() / "CACHE" / "coded", Command::Bool(A, "retry", false));
            json Twins = json::array();
            for (const auto &L : Spec.value("layers", json::array()))
                if (L.is_object() && L.value("type", std::string()) == "coded") Twins.push_back({{"name", L.value("name", std::string())}, {"reads", L.value("source", std::string())}});
            if (A.contains("out")) { const std::string D = Spec.dump(2); Spit(Command::Str(A, "out"), reinterpret_cast<const uint8_t *>(D.data()), D.size()); }
            return json{{"twins", N}, {"coded", Twins}, {"seconds", Now() - T0}};
        }, {} });

    Command::Register({
        "coding expand", "a predicted record's plain twin: every decision stored, so reads never run the family",
        "What a machine keeps beside a coded layer's content (VidyaGod does it itself, once per record, in its cache): the "
        "shipped bytes are generated through the predicted record, then minted plain, refused unless they code back "
        "bit-exact from the very same content. Takes about a mint's time. --spacing: as `coding mint`'s, for the plain record.",
        { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), AsPath(Pos("record", "the predicted record")), AsPath(Pos("content", "the content")),
          AsPath(Pos("out", "the plain record to write")), Opt("spacing", "shipped bytes between checkpoints", Type::Int) },
        { "coding expand ~/x/BSA105.json Textures1.vgcq Textures1.content Textures1.vgcr" },
        { "coding::ExpandInto", "coding::CodedByteSource::predicted" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const coding::Description D = Described(Command::Str(A, "description"));
            auto Desc = Map(Command::Str(A, "description")), Rec = Map(Command::Str(A, "record")), Con = Map(Command::Str(A, "content"));
            std::string Err;
            auto Probe = coding::CodedByteSource::Create(Desc, Rec, Con, 0, Err);
            if (!Probe) throw std::runtime_error("refused: " + Err);
            const uint64_t Sn = Probe->size(), Cn = Con->size();
            Probe.reset();
            const uint64_t Spacing = A.contains("spacing") ? static_cast<uint64_t>(Command::Int(A, "spacing")) : coding::SuggestedSpacing(D, Sn, true);
            void *Sb = ::mmap(nullptr, Sn + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            void *Cb = ::mmap(nullptr, Cn + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (Sb == MAP_FAILED || Cb == MAP_FAILED) throw std::runtime_error("cannot reserve the buffers");
            struct Unmap { void *P; size_t N; ~Unmap() { if (P != MAP_FAILED) ::munmap(P, N); } } U1{ Sb, Sn + 1 }, U2{ Cb, Cn + 1 };
            std::vector<uint8_t> Plain;
            const double T0 = Now();
            if (!coding::ExpandInto(Desc, Rec, Con, static_cast<uint8_t *>(Sb), Sn, static_cast<uint8_t *>(Cb), Cn, Spacing, Plain, Err))
                throw std::runtime_error("refused: " + Err);
            Spit(Command::Str(A, "out"), Plain.data(), Plain.size());
            return json{{"shipped", Sn}, {"predicted_record", Rec->size()}, {"plain_record", Plain.size()}, {"spacing", Spacing}, {"seconds", Now() - T0}};
        }, {} });

    const auto Coded = [](const json &A, uint64_t Expect) {
        std::string Err;
        auto S = coding::CodedByteSource::Create(Map(Command::Str(A, "description")), Map(Command::Str(A, "record")),
                                                 Map(Command::Str(A, "content")), Expect, Err);
        if (!S) throw std::runtime_error("refused: " + Err);
        return S;
    };
    Command::Register({
        "coding verify", "rebuild the shipped bytes through the read path (checkpoints, copy runs, the pool) and compare",
        "Every byte, front to back, as a game reading the mounted file would get it. Exit status says identical or not.",
        { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), AsPath(Pos("record", "the record")), AsPath(Pos("content", "the content")),
          AsPath(Pos("shipped", "the shipped file to compare with")) },
        { "coding verify ~/x/BSA105.json Interface.vgcr Interface.content 'Skyrim - Interface.bsa'" },
        { "coding::CodedByteSource::Create", "coding::CodedByteSource::pread", "coding::CodedByteSource::prefetch",
          "coding::CodedByteSource::size", "coding::CodedByteSource::checkpoints", "coding::CodedByteSource::segmentsDecoded",
          "coding::CodedByteSource::bytesGenerated" }, Needs::Config, false,
        [Coded](Command::Context &, const json &A) {
            MmapByteSource W;
            if (!W.Map(Command::Str(A, "shipped"))) throw Command::BadArgs("cannot read " + Command::Str(A, "shipped"));
            auto S = Coded(A, W.n);
            const double T = ReadThrough(*S, W.p, nullptr);
            return json{{"identical", true}, {"bytes", S->size()}, {"seconds", T}, {"mb_per_s", T > 0 ? S->size() / T / 1e6 : 0.0},
                        {"checkpoints", S->checkpoints()}, {"segments_decoded", S->segmentsDecoded()}, {"bytes_generated", S->bytesGenerated()}};
        }, {} });

    Command::Register({
        "coding cat", "rebuild the shipped bytes into a file",
        "", { AsPath(Pos("description", "a description JSON, or a node whose CODED layers carry one")), AsPath(Pos("record", "the record")), AsPath(Pos("content", "the content")),
              AsPath(Pos("out", "the file to write")) },
        { "coding cat ~/x/BSA105.json Interface.vgcr Interface.content rebuilt.bsa" },
        { "coding::CodedByteSource::Create" }, Needs::Config, false,
        [Coded](Command::Context &, const json &A) {
            auto S = Coded(A, 0);
            const std::string Path = Command::Str(A, "out"), Tmp = Path + ".part";
            std::FILE *Out = std::fopen(Tmp.c_str(), "wb");
            if (!Out) throw std::runtime_error("cannot write " + Path);
            double T = 0;
            try { T = ReadThrough(*S, nullptr, Out); }
            catch (...) { std::fclose(Out); std::remove(Tmp.c_str()); throw; }
            if (std::fclose(Out) != 0 || std::rename(Tmp.c_str(), Path.c_str()) != 0)
            { std::remove(Tmp.c_str()); throw std::runtime_error("cannot write " + Path); }
            return json{{"bytes", S->size()}, {"seconds", T}};
        }, {} });

    Command::Register({
        "coding deflate-split", "split one raw deflate stream into its decoded content and its decisions (the deflate layer)",
        "The hand-wired deflate layer the mount reads: refused unless the stream is exactly one complete raw deflate stream "
        "and the split rebuilds it byte for byte.",
        { AsPath(Pos("stream", "a raw deflate stream")), AsPath(Pos("decisions", "the decisions to write")), AsPath(Pos("content", "the content to write")) },
        { "coding deflate-split member.deflate member.dec member.content" },
        { "deflatelayer::DeflateDecisions" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const auto S = Slurp(Command::Str(A, "stream"));
            std::vector<uint8_t> Dec, Content;
            std::string Err;
            if (!deflatelayer::DeflateDecisions(S.data(), S.size(), Dec, Content, Err)) throw std::runtime_error("refused: " + Err);
            Spit(Command::Str(A, "decisions"), Dec.data(), Dec.size());
            Spit(Command::Str(A, "content"), Content.data(), Content.size());
            return json{{"stream", S.size()}, {"decisions", Dec.size()}, {"content", Content.size()}};
        }, {} });

    Command::Register({
        "coding deflate-verify", "rebuild a raw deflate stream from its decisions and content through the deflate layer, and compare",
        "", { AsPath(Pos("decisions", "the decisions")), AsPath(Pos("content", "the content")), AsPath(Pos("stream", "the stream to compare with")) },
        { "coding deflate-verify member.dec member.content member.deflate" },
        { "deflatelayer::DeflateByteSource::Create", "deflatelayer::DeflateByteSource::pread", "deflatelayer::DeflateByteSource::size",
          "deflatelayer::DeflateByteSource::blocks" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            MmapByteSource W;
            if (!W.Map(Command::Str(A, "stream"))) throw Command::BadArgs("cannot read " + Command::Str(A, "stream"));
            std::string Err;
            auto S = deflatelayer::DeflateByteSource::Create(Map(Command::Str(A, "decisions")), Map(Command::Str(A, "content")), W.n, Err);
            if (!S) throw std::runtime_error("refused: " + Err);
            const double T = ReadThrough(*S, W.p, nullptr);
            return json{{"identical", true}, {"bytes", S->size()}, {"blocks", S->blocks()}, {"seconds", T}};
        }, {} });
}
