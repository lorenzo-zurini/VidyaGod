// `VidyaGod vfs …` — the runtime filesystem (VidyaGodFS) without mounting it: a mount spec (what a launch writes for
// vidyagodfs) loaded in-process, and the filesystem's own operations run on it — list, stat (which layer serves a
// path, whether a deletion hides it, the case it is spelt in), read, write, make, delete, rename, link. Writes land in
// the spec's writable layer exactly as a mounted game's would. And the archives it serves: a zip's index and entries,
// a .vgdelta made, verified and read.
#include "command.h"
#include "cmdutil.h"
#include "bytesource.h"
#include "layerspec.h"
#include "overlay.h"
#include "vfsops.h"
#include "vgdelta.h"
#include "ziplayer.h"
#include "zipscan.h"

#include <fstream>
#include <sstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

struct Loaded
{
    Spec Sp;
    std::unique_ptr<VfsState> S = std::make_unique<VfsState>();
};

Loaded Load(const json &A)
{
    Loaded L;
    std::string Err;
    if (!ParseSpec(Command::Str(A, "spec"), L.Sp, Err)) throw Command::BadArgs("the spec: " + Err);
    if (!L.S->Init(L.Sp, Err)) throw std::runtime_error("the filesystem would not start: " + Err);
    return L;
}



// A path as the mounted backend hands it to the operations: normalised, then in the case the layers spell it.
std::string V(const Loaded &L, const json &A, const char *Key = "path") { return CanonicalCase(*L.S, NormalizeVPath(Command::Str(A, Key, ""))); }

void Ok(int Rc, const std::string &What)
{
    if (Rc < 0) throw std::runtime_error(What + ": " + std::strerror(-Rc));
}

// A file mode as chmod takes it: octal digits ("644", "0755").
uint32_t Octal(const std::string &V)
{
    if (V.empty() || V.size() > 6 || V.find_first_not_of("01234567") != std::string::npos)
        throw Command::BadArgs("--mode takes an octal mode (644, 0755), not '" + V + "'");
    return static_cast<uint32_t>(std::stoul(V, nullptr, 8));
}

json Attr(const VfsAttr &T)
{
    char Perm[8];
    std::snprintf(Perm, sizeof Perm, "%04o", T.perms & 07777);
    return {{"type", T.isDir ? "dir" : T.isSymlink ? "link" : "file"}, {"size", T.size}, {"mode", Perm}, {"mtime", T.mtime}, {"links", T.nlink}};
}

const char *KindOf(HitKind K)
{
    switch (K)
    {
    case HitKind::None: return "none";
    case HitKind::ImplicitDir: return "implicit folder";
    case HitKind::WriteLayer: return "writable layer";
    case HitKind::DirLayer: return "folder layer";
    case HitKind::FileLayer: return "file layer";
    case HitKind::ZipEntry: return "zip entry";
    }
    return "?";
}

std::shared_ptr<MmapByteSource> Map(const std::string &P)
{
    auto M = std::make_shared<MmapByteSource>();
    if (!M->Map(P)) throw Command::BadArgs("cannot read '" + P + "'");
    return M;
}

Command::Arg SpecArg() { return Command::Pos("spec", "the mount spec (a launch's vidyagodfs.spec.json)"); }
Command::Arg PathArg() { return Command::Pos("path", "a path in the filesystem ('' = its root)"); }

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("vfs", "the runtime filesystem without mounting it: list, stat, read, write a spec's view; zips and deltas");

    Command::Register({
        "vfs spec", "a mount spec: its layers (lowest first), its writable layer, its options",
        "", { SpecArg() }, { "vfs spec ~/.VidyaGod/TEMP/…/vidyagodfs.spec.json" }, { "ParseSpec", "VfsState::Init" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            static const char *T[] = { "dir", "zip", "file", "delta" };
            json Ls = json::array();
            for (const auto &X : L.Sp.layers)
                Ls.push_back({{"type", T[static_cast<int>(X.type)]}, {"source", X.source}, {"target", X.target}, {"bases", X.baseTargets}});
            return json{{"mountpoint", L.Sp.mountpoint}, {"writable", L.Sp.writelayer}, {"read_only", L.Sp.readOnly},
                        {"case_insensitive", L.Sp.caseInsensitive}, {"layers", Ls}};
        }, {} });

    Command::Register({
        "vfs ls", "list a folder of the filesystem (as the game would see it)", "",
        { SpecArg(), Opt("path", "the folder ('' = the root)") }, { "vfs ls spec.json", "vfs ls spec.json --path 'drive_c/Games/AoE2'" },
        { "VfsReaddirTyped", "VfsReaddir", "NormalizeVPath" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            json Out = json::array();
            Ok(VfsReaddirTyped(*L.S, V(L, A), [&](const std::string &N, const VfsAttr &T) { json R = Attr(T); R["name"] = N; Out.push_back(R); }), "list");
            int Names = 0;
            Ok(VfsReaddir(*L.S, V(L, A), [&](const std::string &) { ++Names; }), "list");
            return json{{"entries", Out}, {"names", Names}};
        }, {} });

    Command::Register({
        "vfs stat", "one path: its attributes, which layer serves it, what hides it, how it is spelt",
        "Getattr, and the overlay's answer for the path: served by the writable layer, a folder / file layer or a zip "
        "entry (and its host path), deleted (a whiteout) or under a deleted or remade folder, a kept folder's "
        "passthrough, the case the layers spell it in; per layer whether it covers or masks it and the path it maps to; "
        "where its writable copy and whiteout marker would be.", { SpecArg(), PathArg() }, { "vfs stat spec.json 'drive_c/Games/AoE2/empires2.exe'" },
        { "VfsGetattr", "Resolve", "CanonicalCase", "FoldCase", "IsWhiteouted", "IsUnderWhiteout", "IsUnderOpaque", "PassthroughHost", "KeptLayerFor",
          "KeptMasked", "KeptHidden", "KeptOpaqueDir", "LayerCovers", "LayerMasked", "RelUnder", "SourceRel", "WLPath", "WhiteoutPath" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            const std::string P = V(L, A);
            VfsAttr T;
            const int Rc = VfsGetattr(*L.S, P, T);
            const ResolveResult R = Resolve(*L.S, P);
            std::string Host;
            const bool Pass = PassthroughHost(*L.S, P, Host);
            json Layers = json::array();
            for (size_t I = 0; I < L.S->layers.size(); ++I)
            {
                const Layer &Ly = L.S->layers[I];
                Layers.push_back({{"layer", I}, {"source", Ly.source}, {"covers", LayerCovers(Ly, P)}, {"masked", LayerMasked(Ly, P)},
                                  {"under_target", RelUnder(Ly, P)}, {"in_source", SourceRel(Ly, P)}, {"kept_masks", KeptMasked(Ly, P)},
                                  {"kept_opaque", KeptOpaqueDir(Ly, P)}});
            }
            const Layer *K = KeptLayerFor(*L.S, P);
            return json{{"path", P}, {"exists", Rc == 0}, {"attr", Rc == 0 ? Attr(T) : json(std::strerror(-Rc))}, {"served_by", KindOf(R.kind)},
                        {"host", R.hostPath}, {"zip_entry", R.zipEntry ? json(R.zipEntry->name) : json(nullptr)}, {"rw_passthrough", R.rwPassthrough},
                        {"spelt", CanonicalCase(*L.S, P)}, {"key", FoldCase(P)}, {"deleted", IsWhiteouted(*L.S, P)},
                        {"under_deleted", IsUnderWhiteout(*L.S, P)}, {"under_remade", IsUnderOpaque(*L.S, P)},
                        {"passthrough", Pass ? json(Host) : json(nullptr)}, {"kept_by", K ? json(K->source) : json(nullptr)},
                        {"kept_hidden", KeptHidden(*L.S, P)}, {"writable_copy", WLPath(*L.S, P)}, {"whiteout_marker", WhiteoutPath(*L.S, P)},
                        {"layers", Layers}};
        }, {} });

    Command::Register({
        "vfs cat", "read a file of the filesystem (to stdout, or --out a file)", "",
        { SpecArg(), PathArg(), Opt("out", "write it here"), Opt("bytes", "at most this many", Type::Int), Opt("offset", "from here", Type::Int) },
        { "vfs cat spec.json 'drive_c/Games/AoE2/language.ini'", "vfs cat spec.json 'x/y.bin' --out /tmp/y.bin" },
        { "VfsOpen", "VfsRead", "VfsRelease" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            OpenFile *F = nullptr;
            Ok(VfsOpen(*L.S, V(L, A), 0, F), "open");
            if (!F) throw Command::BadArgs("that is a folder");
            std::string Data;
            uint64_t Off = static_cast<uint64_t>(Command::Int(A, "offset", 0));
            const long long Max = Command::Int(A, "bytes", -1);
            std::vector<char> Buf(1 << 16);
            for (;;)
            {
                const ssize_t N = VfsRead(F, Buf.data(), Buf.size(), Off);
                if (N < 0) { VfsRelease(F); Ok(static_cast<int>(N), "read"); }
                if (N == 0) break;
                Data.append(Buf.data(), static_cast<size_t>(N));
                Off += static_cast<uint64_t>(N);
                if (Max >= 0 && static_cast<long long>(Data.size()) >= Max) { Data.resize(static_cast<size_t>(Max)); break; }
            }
            VfsRelease(F);
            if (A.contains("out")) { std::ofstream(Command::Str(A, "out"), std::ios::binary) << Data; return json{{"bytes", Data.size()}, {"out", Command::Str(A, "out")}}; }
            return json{{"bytes", Data.size()}, {"data", Data}};
        }, {} });

    Command::Register({
        "vfs write", "write a file in the filesystem (it lands in the writable layer, copied up)",
        "--text or --from a host file; --append adds to the end; a file a lower layer serves is copied up first, a "
        "deleted name made again.", { SpecArg(), PathArg(), Opt("text", "the content"), Opt("from", "a host file's content"), Flag("append", "append") },
        { "vfs write spec.json 'drive_c/Games/AoE2/x.ini' --text 'a=1'" },
        { "VfsCreate", "VfsWrite", "VfsFsync", "VfsTruncate", "CopyUp", "EnsureWriteParent", "RemoveWhiteout", "UnhideLower" , "VfsState::CopyUpLock" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            std::string Data = Command::Str(A, "text", "");
            if (A.contains("from")) { std::ifstream In(Command::Str(A, "from"), std::ios::binary); std::stringstream S; S << In.rdbuf(); Data = S.str(); }
            const std::string P = V(L, A);
            OpenFile *F = nullptr;
            VfsAttr T;
            const bool Exists = VfsGetattr(*L.S, P, T) == 0;
            Ok(Exists ? VfsOpen(*L.S, P, 2 /* O_RDWR */, F) : VfsCreate(*L.S, P, 0644, 2, F), "open");
            const bool App = Command::Bool(A, "append", false);
            if (!App) Ok(VfsTruncate(*L.S, P, F, 0), "truncate");
            const ssize_t N = VfsWrite(*L.S, F, Data.data(), Data.size(), App && Exists ? T.size : 0);
            const int Sync = VfsFsync(F);
            VfsRelease(F);
            Ok(static_cast<int>(N), "write");
            Ok(Sync, "sync");
            return json{{"written", N}, {"path", P}};
        }, {} });

    Command::Register({
        "vfs make", "make a folder, a symbolic link or a hard link in the filesystem", "",
        { SpecArg(), PathArg(), Flag("dir", "a folder"), Opt("symlink-to", "a symbolic link to this target"), Opt("hardlink-to", "a hard link to this path") },
        { "vfs make spec.json 'drive_c/Saves' --dir", "vfs make spec.json 'a/latest' --symlink-to 'slot1'" },
        { "VfsMkdir", "VfsSymlink", "VfsLink", "MarkOpaque", "KeptMarkOpaque" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            const std::string P = V(L, A);
            if (Command::Bool(A, "dir", false)) Ok(VfsMkdir(*L.S, P, 0755), "mkdir");
            else if (A.contains("symlink-to")) Ok(VfsSymlink(*L.S, Command::Str(A, "symlink-to"), P), "symlink");
            else if (A.contains("hardlink-to")) Ok(VfsLink(*L.S, NormalizeVPath(Command::Str(A, "hardlink-to")), P), "link");
            else throw Command::BadArgs("give --dir, --symlink-to or --hardlink-to");
            return json{{"made", P}};
        }, {} });

    Command::Register({
        "vfs remove", "delete a file or (--dir) an empty folder (a whiteout hides what a lower layer had)", "",
        { SpecArg(), PathArg(), Flag("dir", "a folder") }, { "vfs remove spec.json 'drive_c/Games/AoE2/intro.avi'" },
        { "VfsUnlink", "VfsRmdir", "CreateWhiteout", "HideLower", "KeptDropMarkersUnder", "UnmarkOpaque" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            const std::string P = V(L, A);
            Ok(Command::Bool(A, "dir", false) ? VfsRmdir(*L.S, P) : VfsUnlink(*L.S, P), "remove");
            return json{{"removed", P}, {"deleted", IsWhiteouted(*L.S, P)}};
        }, {} });

    Command::Register({
        "vfs move", "rename a file or folder in the filesystem", "", { SpecArg(), Pos("from", "the path"), Pos("to", "the new path") },
        { "vfs move spec.json 'a/old.ini' 'a/new.ini'" }, { "VfsRename", "RenameMaskPrefix", "KeptRenamePrefix" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            Ok(VfsRename(*L.S, V(L, A, "from"), V(L, A, "to")), "rename");
            return json{{"moved", V(L, A, "to")}};
        }, {} });

    Command::Register({
        "vfs set", "a path's mode, times or size, or read a link", "",
        { SpecArg(), PathArg(), Opt("mode", "octal mode (0644)"), Opt("mtime", "seconds since epoch", Type::Int), Opt("size", "truncate / extend to", Type::Int),
          Flag("readlink", "what the link names") },
        { "vfs set spec.json 'a/run.sh' --mode 0755", "vfs set spec.json 'a/latest' --readlink" },
        { "VfsChmod", "VfsUtimens", "VfsTruncate", "VfsReadlink" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            const std::string P = V(L, A);
            json Out = json::object();
            if (A.contains("mode")) Ok(VfsChmod(*L.S, P, Octal(Command::Str(A, "mode"))), "chmod");
            if (A.contains("mtime")) Ok(VfsUtimens(*L.S, P, Command::Int(A, "mtime"), Command::Int(A, "mtime")), "utimens");
            if (A.contains("size")) Ok(VfsTruncate(*L.S, P, nullptr, static_cast<uint64_t>(Command::Int(A, "size"))), "truncate");
            if (Command::Bool(A, "readlink", false)) { std::string T; Ok(VfsReadlink(*L.S, P, T), "readlink"); Out["target"] = T; }
            VfsAttr T;
            if (VfsGetattr(*L.S, P, T) == 0) Out["attr"] = Attr(T);
            return Out;
        }, {} });

    Command::Register({
        "vfs df", "the filesystem's free space (its writable layer's volume)", "", { SpecArg() }, { "vfs df spec.json" }, { "VfsStatfs" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            Loaded L = Load(A);
            HostIO::StatvfsInfo I;
            Ok(VfsStatfs(*L.S, I), "statfs");
            return json{{"block", I.bsize}, {"blocks", I.blocks}, {"free", I.bfree}, {"available", I.bavail}};
        }, {} });

    Command::Register({
        "vfs zip", "a zip as the filesystem serves it: its entries, stored or not, offsets; read one",
        "The archive's index (each entry's name, size, a folder or a link, stored or compressed), its first compressed "
        "entry (a zip the filesystem cannot mount), and with --entry that entry's bytes (--out a file; a link resolved "
        "within the archive).", { Pos("zip", "the zip file"), Opt("entry", "read this entry"), Opt("out", "(--entry) write it here"), Flag("offsets", "each entry's data offset") },
        { "vfs zip game.zip", "vfs zip game.zip --entry data/readme.txt" },
        { "BuildZipIndex", "OpenZipEntry", "ReadZipEntry", "ResolveZipSymlink", "zipscan::WalkCentralDir", "zipscan::CentralDirOffsets",
          "zipscan::FirstCompressedEntry", "zipscan::LocalDataOffset", "zipscan::Rd16", "zipscan::Rd32", "zipscan::Rd64",
          "MmapByteSource::Map", "MmapByteSource::pread", "MmapByteSource::size", "ByteSource::preadAll" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string Z = Command::Str(A, "zip");
            auto M = Map(Z);
            const auto Ix = BuildZipIndex(M, Z);
            if (!Ix) throw std::runtime_error("not a zip it can read");
            json Out = {{"entries", Ix->byName.size()}, {"first_compressed", zipscan::FirstCompressedEntry(*M)}};
            if (A.contains("entry"))
            {
                const std::string N = NormalizeVPath(Command::Str(A, "entry"));
                const ZipEntry *E = nullptr;
                if (const auto It = Ix->byName.find(N); It != Ix->byName.end()) E = &It->second;
                if (!E) throw Command::BadArgs("no entry '" + N + "'");
                if (E->symlink) if (const ZipEntry *T = ResolveZipSymlink(*Ix, *E)) E = T;
                ZipReader R;
                Ok(OpenZipEntry(*Ix, *E, R), "open entry");
                std::string D(E->size, '\0');
                const int Got = ReadZipEntry(R, D.data(), D.size(), 0);
                Ok(Got, "read entry");
                D.resize(static_cast<size_t>(Got));
                if (A.contains("out")) { std::ofstream(Command::Str(A, "out"), std::ios::binary) << D; Out["out"] = Command::Str(A, "out"); }
                else Out["data"] = D;
                Out["bytes"] = D.size();
                return Out;
            }
            json Es = json::array();
            const auto Offs = Command::Bool(A, "offsets", false) ? zipscan::CentralDirOffsets(*M) : std::unordered_map<std::string, uint64_t>();
            for (const auto &[Name, E] : Ix->byName)
            {
                json R = {{"name", E.name}, {"size", E.size}, {"dir", E.isDir}, {"link", E.symlink}, {"stored", E.stored}};
                if (const auto It = Offs.find(E.name); It != Offs.end()) R["data_at"] = zipscan::LocalDataOffset(*M, It->second);
                Es.push_back(R);
            }
            int Walked = 0;
            zipscan::WalkCentralDir(*M, [&](const std::string &, uint16_t, uint64_t) { ++Walked; return true; });
            Out["list"] = Es;
            Out["central_directory"] = Walked;
            return Out;
        }, {} });

    Command::Register({
        "vfs delta", "make a .vgdelta (target over base), verify one, or read what one rebuilds",
        "--make: the delta that rebuilds --target from --base (written to --out). --verify: rebuild --target from "
        "--base through the delta and compare every byte. --rebuild: write what the delta over --base rebuilds to "
        "--out. Several --base files are one base, concatenated in order (a delta over several sources).",
        { Pos("delta", "the .vgdelta"), Opt("base", "the base file (again for each more: --base a --base b)", Type::String, true), Opt("target", "(--make, --verify) the target file"),
          Opt("out", "(--make, --rebuild) write here"), Flag("make", "make it"), Flag("verify", "verify it"), Flag("rebuild", "rebuild the target"),
          Opt("block", "(--make) block size", Type::Int) },
        { "vfs delta p.vgdelta --make --base a.zip --target b.zip", "vfs delta p.vgdelta --verify --base a.zip --target b.zip" },
        { "vgdelta::GenerateDeltaToFile", "vgdelta::VerifyDelta", "vgdelta::DeltaByteSource::Create", "vgdelta::DeltaByteSource::pread",
          "vgdelta::DeltaByteSource::size", "vgdelta::Fnv1a64", "ConcatByteSource::pread", "ConcatByteSource::size", "MemByteSource::pread",
          "MemByteSource::size" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::vector<std::shared_ptr<MmapByteSource>> Maps;
            std::vector<std::shared_ptr<ByteSource>> Parts;
            for (const auto &B : PathList(A, "base")) { Maps.push_back(Map(B)); Parts.push_back(Maps.back()); }
            std::shared_ptr<ByteSource> Base = Parts.size() == 1 ? Parts.front() : std::make_shared<ConcatByteSource>(Parts);
            const std::string D = Command::Str(A, "delta");
            if (Command::Bool(A, "make", false))
            {
                //The matcher wants the base as one flat range: one base file is its mapping; several are joined in
                //memory (they are one base, concatenated). The delta streams to the file, never resident.
                std::vector<uint8_t> Joined;
                const uint8_t *B = Maps.size() == 1 ? Maps.front()->p : nullptr;
                if (!B)
                {
                    Joined.resize(Base->size());
                    if (!Base->preadAll(Joined.data(), Joined.size(), 0)) throw std::runtime_error("cannot read the base");
                    B = Joined.data();
                }
                auto T = Map(Command::Str(A, "target"));
                std::string Err;
                if (!vgdelta::GenerateDeltaToFile(B, Base->size(), T->p, T->n, D, Err,
                                                  static_cast<uint32_t>(Command::Int(A, "block", vgdelta::DEFAULT_BLOCK)),
                                                  std::filesystem::absolute(D).parent_path().string()))   // scratch beside it, not a tmpfs
                    throw std::runtime_error("the delta could not be made: " + Err);
                auto Made = Map(D);
                return json{{"delta", D}, {"bytes", Made->n}, {"target_bytes", T->n}, {"fnv1a64", vgdelta::Fnv1a64(Made->p, Made->n)}};
            }
            auto Dm = Map(D);
            if (Command::Bool(A, "verify", false))
            {
                auto T = Map(Command::Str(A, "target"));
                std::string Err;
                const bool Good = vgdelta::VerifyDelta(std::shared_ptr<ByteSource>(Dm), Base, *T, Err);
                return json{{"verified", Good}, {"why", Err}};
            }
            std::string Err;
            auto R = vgdelta::DeltaByteSource::Create(Dm, Base, Err, true);
            if (!R) throw std::runtime_error(Err);
            json Out = {{"rebuilds_bytes", R->size()}};
            if (Command::Bool(A, "rebuild", false))
            {
                std::ofstream O(Command::Str(A, "out"), std::ios::binary);
                std::vector<char> Buf(1 << 20);
                for (uint64_t Off = 0; Off < R->size();)
                {
                    const ssize_t N = R->pread(Buf.data(), Buf.size(), Off);
                    if (N <= 0) throw std::runtime_error("read failed at " + std::to_string(Off));
                    O.write(Buf.data(), N);
                    Off += static_cast<uint64_t>(N);
                }
                Out["out"] = Command::Str(A, "out");
            }
            return Out;
        }, {} });
}
