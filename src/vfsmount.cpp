#include "vfsmount.h"
#include "platform/platform.h"
#include "procenv.h"         // SystemToolEnv + RunCommand
#include "commonutils.h"        // Log*
#include "launchresolver.h"     // BoundaryLinkIndex / InnerRunnerMountRel (cross-namespace inner-runner mounts)
#include "varsubst.h"           // %variable% substitution in layer TARGETs (e.g. %ContentDir% → next to the exe)
#include <fstream>              // prefix-recipe read + marker seeding (zero-copy prefix assembly)
#include <sstream>

#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QProcess>
#include <QString>
#include <QThread>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

//VFS layer helpers (IsVfsLayer/LayerType/ResolveLayerSource/...) live in ManifestModel; bring them in unqualified.
using namespace ManifestModel;

//Locates the vidyagodfs helper: next to the running binary first (via /proc/self/exe, which works
//in headless mode before any QApplication exists), else on PATH.
static std::string VidyagodfsPath()
{
#ifdef _WIN32
    constexpr const char *HelperName = "vidyagodfs.exe";
#else
    constexpr const char *HelperName = "vidyagodfs";
#endif
    std::filesystem::path Self = Platform::SelfExe();
    if (!Self.empty())
    {
        std::error_code ec;
        std::filesystem::path CoLocated = Self.parent_path() / HelperName;
        if (std::filesystem::exists(CoLocated, ec)) return CoLocated.string();
    }
    return HelperName;
}

//Returns the name of the first COMPRESSED (non-STORE) entry in the zip at Path, or "" if every entry
//is STORED (or the archive can't be parsed). Self-contained (no libzip): walks the zip central
//directory (ZIP64-aware) and checks each record's compression-method field (offset 10; 0 == STORE).
//Used to block compressed zip layers before mounting — vidyagodfs serves zip layers zero-copy, which
//requires STORE.
static std::string ZipFirstCompressedEntry(const std::string &Path)
{
    auto rd16 = [](const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); };
    auto rd32 = [](const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };
    auto rd64 = [](const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; };

    // Portable positioned reads via ifstream (POSIX pread has no MinGW equivalent). Single reader, so
    // seeking the shared cursor per call is fine.
    std::ifstream File(Path, std::ios::binary);
    if (!File) return "";
    File.seekg(0, std::ios::end);
    uint64_t fsize = (uint64_t)File.tellg();
    auto preadAll = [&](void *b, size_t n, uint64_t off) -> bool {
        File.clear();
        File.seekg((std::streamoff)off, std::ios::beg);
        File.read((char *)b, (std::streamsize)n);
        return (uint64_t)File.gcount() == n;
    };

    std::string result;
    if (fsize >= 22)
    {
        uint64_t tail = std::min<uint64_t>(fsize, 22 + 65535);
        std::vector<uint8_t> buf(tail);
        if (preadAll(buf.data(), tail, fsize - tail))
        {
            long eocd = -1;
            for (long i = (long)tail - 22; i >= 0; --i) if (rd32(&buf[i]) == 0x06054b50u) { eocd = i; break; }
            if (eocd >= 0)
            {
                const uint8_t *E = &buf[eocd];
                uint64_t total = rd16(E + 10), cdoff = rd32(E + 16), eocdOff = fsize - tail + (uint64_t)eocd;
                if (cdoff == 0xFFFFFFFFu || total == 0xFFFFu) // ZIP64
                {
                    uint8_t loc[20], z64[56];
                    if (eocdOff >= 20 && preadAll(loc, 20, eocdOff - 20) && rd32(loc) == 0x07064b50u)
                    {
                        uint64_t z = rd64(loc + 8);
                        if (preadAll(z64, 56, z) && rd32(z64) == 0x06064b50u) { total = rd64(z64 + 32); cdoff = rd64(z64 + 48); }
                    }
                }
                uint64_t pos = cdoff;
                for (uint64_t i = 0; i < total && result.empty(); ++i)
                {
                    uint8_t rec[46];
                    if (!preadAll(rec, 46, pos) || rd32(rec) != 0x02014b50u) break;
                    uint16_t method = rd16(rec + 10), nl = rd16(rec + 28), el = rd16(rec + 30), cl = rd16(rec + 32);
                    if (method != 0) // not STORE
                    {
                        std::vector<uint8_t> nm(nl);
                        result = (nl && preadAll(nm.data(), nl, pos + 46)) ? std::string((const char *)nm.data(), nl) : "(entry)";
                    }
                    pos += 46u + nl + el + cl;
                }
            }
        }
    }
    return result;
}

//Builds the vidyagodfs JSON layer-spec from the resolved container: the DEFPREFIX base (when the runner
//generates a prefix), every VFS subcomponent rooted at CONTENT_ROOT + its TARGET (logically — no staging
//dirs), the KEEP dirs as durable RW passthrough layers, the DROP paths as ephemeral RW shadows, and the
//writable top branch (the durable UserDataPath when the whole runtime is kept, else the ephemeral WRITELAYER).
//Array order is union priority (lowest first).
nlohmann::ordered_json VfsMount::BuildLayerSpec(struct ContainerParams &ContainerParams)
{
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = ContainerParams.RuntimePath.string();
    Spec["uid"] = 1000;
    Spec["gid"] = 1000;
    Spec["readonly"] = ContainerParams.ReadOnlyVFS;
    if (ContainerParams.ReadOnlyVFS)
        Spec["writelayer"] = nullptr;
    else
    {
        const std::filesystem::path RW = ContainerParams.PersistAll ? ContainerParams.UserDataPath : ContainerParams.WriteLayerPath;
        std::filesystem::create_directories(RW);
        Spec["writelayer"] = RW.string();
    }

    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();

    //Prefix assembly has NO special case: a prefix-generating runner declares it as node LAYERS (default_pfx/DLL
    //VFSDirLayers + config_info/marker FileEdits), routed into SubComponentsArray by the resolver, so the generic
    //layer loop below + BuildDefaultData assemble the prefix from the delta chain — ZERO COPY, no wineboot, no
    //DEFPREFIX tree. Nothing to do here.

    //Host content placement is a RUNNER property (CONTENT_ROOT, resolved): "" = root; "drive_c/<UID>" maps
    //the game under Wine's C:; "pfx/drive_c/<UID>" places it inside Proton's pfx. Same game, different runner.
    const std::string &ContentRoot = ContainerParams.ContentRoot;

    // A layer's TARGET is %variable%-substituted (like runner ARGS/ENV) then placed RELATIVE TO THE VFS ROOT — there is
    // no implicit CONTENT_ROOT prefixing. The author states the anchor explicitly with variables: content root is
    // "%PrefixRoot%/drive_c/%PackageUID%", the wine system dir is "%PrefixRoot%/drive_c/windows/syswow64", etc. This is
    // what lets a node place files ANYWHERE in the runtime tree (prefix, system dir, userdata) instead of being trapped
    // under drive_c/<UID>. An absent TARGET means the VFS root ("" — the author composes from there). Content layers
    // pass an empty Base below; the inner-runner nesting passes its own mount base (engine-internal, not author TARGET).
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    auto ResolveTargetKey = [&](const std::string &Base, const nlohmann::ordered_json &Sub, const char *Key) -> std::string {
        if (!Sub.contains(Key) || !Sub[Key].is_string()) return Base;
        std::string T = Sub[Key];
        VarSubst::StringVariableSubstitution(T, Vars);
        for (char &c : T) if (c == '\\') c = '/';                          // normalize win-style separators
        while (!T.empty() && T.front() == '/') T.erase(T.begin());          // no leading slash → clean join
        while (!T.empty() && T.back()  == '/') T.pop_back();
        if (T.empty()) return Base;
        return Base.empty() ? T : (Base + "/" + T);
    };
    // The mount TARGET is the common case; a cross-target VFSDeltaLayer also carries BASE_TARGET (the target of its
    // byte-base zip), resolved identically so the two strings match the FS's per-target base map.
    auto ResolveTarget = [&](const std::string &Base, const nlohmann::ordered_json &Sub) { return ResolveTargetKey(Base, Sub, "TARGET"); };
    // A layer's SOURCE is %variable%-substituted too (like TARGET): after substitution an ABSOLUTE path is used as-is
    // (LayerLocator), so a node can source from a RUNTIME path — e.g. a prefix-assembly VFSDirLayer with
    // PATH "%RunnerMount%/files/share/default_pfx". Package-content layers carry no %VAR% and are unaffected.
    auto ResolveSource = [&](const nlohmann::ordered_json &Sub, const std::filesystem::path &PkgPath) -> std::string {
        nlohmann::ordered_json S = Sub;
        auto SubP = [&](nlohmann::ordered_json &J){ if (J.contains("PATH") && J["PATH"].is_string()) {
            std::string P = J["PATH"]; VarSubst::StringVariableSubstitution(P, Vars); J["PATH"] = P; } };
        SubP(S);
        if (S.contains("SOURCE") && S["SOURCE"].is_object()) SubP(S["SOURCE"]);
        return ResolveLayerSource(S, PkgPath);
    };

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (!IsVfsLayer(Sub.value("TYPE", std::string()))) continue;
        std::string BaseTarget;                                   // cross-target byte-base of a VFSDeltaLayer (else empty)
        if (Sub.value("TYPE", std::string()) == "VFSDeltaLayer" && Sub.contains("BASE_TARGET") && Sub["BASE_TARGET"].is_string())
            BaseTarget = ResolveTargetKey(std::string(), Sub, "BASE_TARGET");
        Layers.push_back(MakeVfsSpecLayer(Sub, ResolveSource(Sub, ContainerParams.PackagePath),
                                          ResolveTarget(std::string(), Sub), BaseTarget));   // VFS-root-relative target
    }

    //CROSS-NAMESPACE NESTING: an INNER chain runner (e.g. a win32 emulator under proton) runs inside the boundary's
    //guest fs, so its build mounts as content under <CONTENT_ROOT>/__runner_<id>__. Gated on inner links existing —
    //a complete no-op for every classic [content-runner, native] chain (BoundaryLinkIndex == 0).
    if (const int B = LaunchResolver::BoundaryLinkIndex(ContainerParams); B > 0)
        for (int i = 0; i < B; ++i)
        {
            const RunnerLink &L = ContainerParams.RunnerChain[i];
            const std::string Base = ContentRoot.empty() ? LaunchResolver::InnerRunnerMountRel(L.NodeId)
                                                         : (ContentRoot + "/" + LaunchResolver::InnerRunnerMountRel(L.NodeId));
            for (const auto &Sub : L.Layers)
            {
                if (!IsVfsLayer(Sub.value("TYPE", std::string()))) continue;
                std::string BaseTarget;
                if (Sub.value("TYPE", std::string()) == "VFSDeltaLayer" && Sub.contains("BASE_TARGET") && Sub["BASE_TARGET"].is_string())
                    BaseTarget = ResolveTargetKey(Base, Sub, "BASE_TARGET");
                Layers.push_back(MakeVfsSpecLayer(Sub, ResolveLayerSource(Sub, L.PackagePath), ResolveTarget(Base, Sub), BaseTarget));
            }
        }

    //DEFAULTDATA — package-encoded base (non-OVERRIDE) Reg/File edits, at the runtime root. Above the
    //component layers (so the edits override the package's own content) but below the writelayer (so the
    //user's persisted changes win). Built by BuildDefaultData for any runner; absent for edit-less packages.
    if (!ContainerParams.DefaultDataPath.empty()
        && std::filesystem::exists(ContainerParams.DefaultDataPath))
        Layers.push_back({{"type", "dir"}, {"source", ContainerParams.DefaultDataPath.string()}, {"target", ""}, {"rw", false}});

    //KEEP dirs → durable RW passthrough layers (high priority, appended after content). The dir unions over the lower
    //layers at its target (so a prefix skeleton stays visible) while writes land in UserDataPath/<rel> — live, durable.
    //Skipped when the whole runtime is kept (the writable branch is already the durable UserDataPath).
    if (!ContainerParams.PersistAll)
        for (const std::string &Rel : ContainerParams.KeepDirs)
        {
            std::filesystem::path Src = ContainerParams.UserDataPath / Rel;
            std::filesystem::create_directories(Src);
            Layers.push_back({{"type", "dir"}, {"source", Src.string()}, {"target", Rel}, {"rw", true}});
        }

    //DROP paths → ephemeral RW shadow layers (highest priority, appended LAST so they win). The source is an empty
    //per-launch dir under TempPath, so writes to <rel> go nowhere durable — carving an ephemeral hole in a whole-runtime
    //keep, or in an enclosing KEEP dir. Always emitted (harmless when nothing durable encloses it).
    for (const std::string &Rel : ContainerParams.DropPaths)
    {
        std::filesystem::path Src = ContainerParams.TempPath / "DROPS" / Rel;
        std::filesystem::create_directories(Src);
        Layers.push_back({{"type", "dir"}, {"source", Src.string()}, {"target", Rel}, {"rw", true}});
    }

    Spec["layers"] = Layers;
    return Spec;
}

//Writes the layer-spec and mounts it by spawning vidyagodfs onto RuntimePath, replacing the former
//unionfs+fuse-zip+bindfs+staging pipeline with a single FUSE mount. The helper daemonizes once the
//mount is live, so the spawn returns; we then poll mountinfo to confirm readiness before proceeding.
//RuntimePath registers for the non-lazy save-safe unmount whenever durable data is reachable through
//the mount (a whole-runtime keep's writelayer or any KEEP-dir RW passthrough), else the lazy path.
bool VfsMount::MountVFS(struct ContainerParams &ContainerParams)
{
    nlohmann::ordered_json Spec = BuildLayerSpec(ContainerParams);

    //Zip layers are served zero-copy, which requires STORE (uncompressed). Block any compressed
    //archive up front with a re-zip dialog (GUI) / log (headless), before mounting.
    for (const auto &L : Spec.value("layers", nlohmann::ordered_json::array()))
    {
        if (L.value("type", std::string()) != "zip") continue;
        std::string Src = L.value("source", std::string());
        std::string Bad = ZipFirstCompressedEntry(Src);
        if (Bad.empty()) continue;

        LogErr("VfsMount::MountVFS", "Compressed zip layer (not STORE): " + Src + " — entry '" + Bad + "'");
        if (qApp)
        {
            QString T = "Compressed zip layer — re-zip as STORE";
            QString M = QString::fromStdString(
                "This package contains a COMPRESSED zip layer, which is no longer supported.\n\n"
                + Src + "\n\nfirst compressed entry:  " + Bad + "\n\n"
                "VidyaGod serves zip layers zero-copy, which requires STORED (uncompressed) archives. "
                "Re-create the archive with STORE:\n\n    zip -0 -r <archive.zip> <files>\n\n"
                "or run tools/store-zips.sh on your library to convert everything at once.");
            QMetaObject::invokeMethod(qApp, [T, M]() { QMessageBox::critical(nullptr, T, M); }, Qt::QueuedConnection);
        }
        return false;
    }

    std::filesystem::path SpecPath = ContainerParams.TempPath / "vidyagodfs.spec.json";
    long long HelperPid = 0;
    if (!VfsMount::SpawnVidyagodfs(Spec, ContainerParams.RuntimePath, SpecPath, &HelperPid))
    {
        LogErr("VfsMount::MountVFS", "vidyagodfs mount did not appear at " + ContainerParams.RuntimePath.string());
        return false;
    }
    if (HelperPid > 0) ContainerParams.VfsHelperPids.push_back(HelperPid);   // Windows: terminated on Cleanup

    //Durable-backed when a live RW path reaches USERDATA through the mount: a whole-runtime keep (writelayer IS
    //UserDataPath) or any KEEP-dir passthrough. KEEP files/registry are copy-captured before unmount, not live.
    if (ContainerParams.PersistAll || !ContainerParams.KeepDirs.empty())
        ContainerParams.CleanupPersistPaths.push_back(ContainerParams.RuntimePath);
    else
        ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RuntimePath);
    LogSucc("VfsMount::MountVFS", "Successfully mounted VFS.");
    return true;
}

//Low-level vidyagodfs mount: write Spec to SpecPath, spawn the helper onto Mountpoint, poll until live.
bool VfsMount::SpawnVidyagodfs(const nlohmann::ordered_json &Spec, const std::filesystem::path &Mountpoint,
                                       const std::filesystem::path &SpecPath, long long *OutPid)
{
    {
        std::ofstream Out(SpecPath);   // SpecPath lives under TempPath (already created) — safe on both OSes
        if (!Out) { LogErr("VfsMount::SpawnVidyagodfs", "Cannot write spec " + SpecPath.string()); return false; }
        Out << Spec.dump(2);
    }
    const std::string Helper = VidyagodfsPath();
    LogOut("VfsMount::SpawnVidyagodfs", "Mounting " + Mountpoint.string() + " via " + Helper);
    //--watch-pid: the helper's watchdog auto-unmounts if this (spawner) process dies, so a crash never leaks a mount.
    const std::string WatchPid = std::to_string(QCoreApplication::applicationPid());
#ifdef _WIN32
    // WinFsp CREATES the mountpoint itself (a reparse-point dir, or a drive letter) — mounting onto an
    // existing directory fails, so unlike Linux we must NOT pre-create it. And WinFsp's fuse_main BLOCKS
    // serving (no daemonize), so start the helper detached and poll until the mountpoint materializes.
    QStringList Args{ QString::fromStdString(SpecPath.string()), QString::fromStdString(Mountpoint.string()),
                      "--watch-pid", QString::fromStdString(WatchPid) };
    qint64 Pid = 0;
    if (!QProcess::startDetached(QString::fromStdString(Helper), Args, QString(), &Pid))
    { LogErr("VfsMount::SpawnVidyagodfs", "Failed to start " + Helper); return false; }
    if (OutPid) *OutPid = (long long)Pid;
    for (int i = 0; i < 100; ++i)   // ~10s: mountpoint appears once WinFsp has mounted (exists() flips false→true)
    {
        if (std::filesystem::exists(Mountpoint)) { LogSucc("VfsMount::SpawnVidyagodfs", "mount live at " + Mountpoint.string()); return true; }
        QThread::msleep(100);
    }
    LogErr("VfsMount::SpawnVidyagodfs", "vidyagodfs mount did not appear at " + Mountpoint.string());
    return false;
#else
    std::filesystem::create_directories(Mountpoint);   // FUSE mounts onto an existing empty dir
    int result = RunCommand(Helper, {SpecPath.string(), Mountpoint.string(), "--watch-pid", WatchPid, "-o", "auto_cache"});
    LogOut("VfsMount::SpawnVidyagodfs", "vidyagodfs spawn exit: " + std::to_string(result));
    if (OutPid) *OutPid = 0;   // Linux unmounts by mountpoint (fusermount3), not by pid
    for (int i = 0; i < 50; ++i)
    {
        if (!VfsMount::MountpointsUnder(Mountpoint).empty()) return true;
        QThread::msleep(100);
    }
    return false;
#endif
}

//Mounts the selected runner's build (its VFSZipLayer subcomponents) read-only at RunnerMountPath — the
//separate-mount, installed-runner model. The zips are mounted (never extracted) at the mount root by their
//resolved (cached) source. Registers the mount for lazy cleanup. No-op when the runner ships no build or is
//unified into the game RUNTIME.
bool VfsMount::MountRunnerBuild(struct ContainerParams &ContainerParams)
{
    if (!ContainerParams.RunnerShipsBuild || ContainerParams.UnifiedRuntime) return true;
    if (ContainerParams.RunnerLayers.empty()) return true;

    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = ContainerParams.RunnerMountPath.string();
    Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    // A runner build may itself be a .vgdelta CHAIN (many Proton versions = base + deltas); MakeVfsSpecLayer handles
    // VFSDeltaLayer so pinning a specific version reconstructs it (its base zip precedes it — closure order is
    // parents-first). TARGET/BASE_TARGET are bare (beside the runner-mount root — no per-link base prefix here) and
    // %VAR%-substituted from the closure, so a SHARED library node (e.g. a dedup'd dxvk pinned by many proton versions)
    // can place itself per-referrer via TARGET:"%DXVK_TARGET%" — the composing runner defines DXVK_TARGET.
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    auto SubstTarget = [&](const std::string &In) -> std::string {
        std::string T = In;
        VarSubst::StringVariableSubstitution(T, Vars);
        for (char &c : T) if (c == '\\') c = '/';
        while (!T.empty() && T.front() == '/') T.erase(T.begin());
        while (!T.empty() && T.back()  == '/') T.pop_back();
        return T;
    };
    for (auto &Sub : ContainerParams.RunnerLayers)
    {
        if (!IsVfsLayer(Sub.value("TYPE", std::string()))) continue;
        std::string BaseTarget;
        if (Sub.value("TYPE", std::string()) == "VFSDeltaLayer" && Sub.contains("BASE_TARGET") && Sub["BASE_TARGET"].is_string())
            BaseTarget = SubstTarget(std::string(Sub["BASE_TARGET"]));
        Layers.push_back(MakeVfsSpecLayer(Sub, ResolveLayerSource(Sub, ContainerParams.RunnerPackagePath),
                                          SubstTarget(Sub.value("TARGET", std::string())), BaseTarget));
    }
    // NB: GE-Proton's protonfixes hack forces PROTON_DLL_COPY='*' (COPY every builtin DLL into the prefix, ~650 MB,
    // instead of symlinking). It is neutralized by the `proton-settings` content node, which every runner PARENTs:
    // it ships a user_settings.py (VFSFileLayer, TARGET "") mounted right here at the runner-mount root, and proton
    // `import user_settings` (from its install dir, on sys.path) BEFORE setup_prefix runs — the import's side-effect
    // restores proton's default DLL_COPY list, so builtins SYMLINK into the RO wine mount. Pure node data; no engine
    // special-case (the loop above already mounted it like any other runner-build VFS layer).
    Spec["layers"] = Layers;

    const std::filesystem::path SpecPath = ContainerParams.TempPath / "vidyagodfs.runner.spec.json";
    long long RunnerHelperPid = 0;
    if (!VfsMount::SpawnVidyagodfs(Spec, ContainerParams.RunnerMountPath, SpecPath, &RunnerHelperPid))
    {
        LogErr("VfsMount::MountRunnerBuild", "runner build mount did not appear at " + ContainerParams.RunnerMountPath.string());
        return false;
    }
    if (RunnerHelperPid > 0) ContainerParams.VfsHelperPids.push_back(RunnerHelperPid);   // Windows: terminated on Cleanup
    ContainerParams.CleanupUnmountPaths.push_back(ContainerParams.RunnerMountPath);  // ephemeral, lazy unmount
    LogSucc("VfsMount::MountRunnerBuild", "Mounted runner build at " + ContainerParams.RunnerMountPath.string());
    return true;
}

//Walks DirectoryPath recursively, lowercasing every path and checking for duplicates.
//Collisions mean two files differ only in case — problematic under Wine because its
//filesystem emulation may resolve to the wrong file depending on access order.
//Reports all conflicts via a QMessageBox warning and returns false if any are found.
bool VfsMount::CheckCaseConflicts(std::filesystem::path DirectoryPath)
{
    std::unordered_set<std::string> FilePathList;
    std::unordered_set<std::string> CaseConflictList;
    bool NoConflict = true;
    //Walking a freshly-mounted FUSE tree can throw filesystem_error (a transient entry, a permission
    //issue) — catch it so the launch worker thread never dies here. skip_permission_denied avoids the
    //most common throw outright.
    try
    {
        for (const auto& FilePath : std::filesystem::recursive_directory_iterator(
                 DirectoryPath, std::filesystem::directory_options::skip_permission_denied))
        {
            std::string FilePathLowercase = FilePath.path().string();
            std::transform(FilePathLowercase.begin(), FilePathLowercase.end(),FilePathLowercase.begin(),[](unsigned char c){ return std::tolower(c); });

            if (FilePathList.find(FilePathLowercase) != FilePathList.end())
            {
                NoConflict = false;
                CaseConflictList.insert(FilePathLowercase);
            }
            else
            {
                FilePathList.insert(FilePathLowercase);
            }
        }
    }
    catch (const std::exception &e)
    {
        LogWarn("VfsMount::CheckCaseConflicts", std::string("Stopped scanning early: ") + e.what());
    }

    if(!NoConflict)
    {
        std::ostringstream oss;
        std::for_each(CaseConflictList.begin(), CaseConflictList.end(),[&oss](const std::string& s){ oss << s << '\n'; });
        std::cerr << "CASE CONFLICTS:\n" << oss.str() << std::endl;
        LogWarn("VfsMount::CheckCaseConflicts", "Case conflicts detected in the runtime (see above).");
        //This runs on the launch worker thread; a QWidget dialog must be created on the GUI thread.
        //Marshal it there (and skip entirely when headless — no QApplication). It's only a warning;
        //the launch is not aborted (caller ignores the return).
        if (qApp)
        {
            QString Msg = "Files differing only in case were found in the runtime — Windows games are\n"
                          "case-insensitive, so Wine may open the wrong one. The game will still launch.\n\n"
                          + QString::fromStdString(oss.str());
            QMetaObject::invokeMethod(qApp, [Msg]() { QMessageBox::warning(nullptr, "Case conflicts", Msg); }, Qt::QueuedConnection);
        }
        return false;
    }
    return true;
}

//Returns every mountpoint at or under Prefix from /proc/self/mountinfo, deepest-first.
std::vector<std::string> VfsMount::MountpointsUnder(const std::filesystem::path &Prefix)
{
    std::vector<std::string> Result;
    if (Prefix.empty()) return Result;

    std::ifstream Mounts("/proc/self/mountinfo");
    if (!Mounts.is_open()) return Result;

    //mountinfo escapes space/tab/newline/backslash as 3-digit octal (\040 etc.); decode them.
    auto Unescape = [](const std::string &In) -> std::string
    {
        std::string Out; Out.reserve(In.size());
        for (size_t i = 0; i < In.size(); ++i)
        {
            if (In[i] == '\\' && i + 3 < In.size()
                && In[i+1] >= '0' && In[i+1] <= '7'
                && In[i+2] >= '0' && In[i+2] <= '7'
                && In[i+3] >= '0' && In[i+3] <= '7')
            {
                Out.push_back(static_cast<char>((In[i+1]-'0')*64 + (In[i+2]-'0')*8 + (In[i+3]-'0')));
                i += 3;
            }
            else Out.push_back(In[i]);
        }
        return Out;
    };

    const std::string PrefixStr = Prefix.string();
    std::string Line;
    while (std::getline(Mounts, Line))
    {
        //Field 5 (0-indexed 4) of a mountinfo line is the mount point.
        std::istringstream Iss(Line);
        std::string Field, MountPoint;
        for (int Idx = 0; Iss >> Field; ++Idx) { if (Idx == 4) { MountPoint = Field; break; } }
        if (MountPoint.empty()) continue;
        MountPoint = Unescape(MountPoint);

        if (MountPoint == PrefixStr || MountPoint.rfind(PrefixStr + "/", 0) == 0)
            Result.push_back(MountPoint);
    }

    //Deepest-first (longest path first) so children unmount before their parents.
    std::sort(Result.begin(), Result.end(),
              [](const std::string &A, const std::string &B){ return A.size() > B.size(); });
    return Result;
}

//Clears a previous run's leftovers under TempPath before a new build. See header for the
//save-safety contract (never wipe while a mount under TempPath is still live).
void VfsMount::CleanStaleRuntime(const std::filesystem::path &TempPath)
{
    if (TempPath.empty()) return;

    std::vector<std::string> StaleMounts = MountpointsUnder(TempPath);
    const bool Exists = std::filesystem::exists(TempPath);
    if (StaleMounts.empty() && !Exists) return; //nothing left over — clean slate

    if (!StaleMounts.empty())
        LogWarn("VfsMount::CleanStaleRuntime",
                "Found " + std::to_string(StaleMounts.size()) + " stale mount(s) under " +
                TempPath.string() + " from a previous run; clearing before mount.");

    //Lazy-detach every stale mount (deepest-first). Lazy avoids the "target is busy" errors a
    //non-lazy unmount spews while a lingering wineserver still holds handles; the actual teardown
    //then finishes in the background, so we VERIFY it has fully cleared below before touching disk.
    for (const std::string &Mount : StaleMounts)
        RunCommand("fusermount3", {"-uz", Mount}, SystemToolEnv());

    //Save-safety gate (mirrors Cleanup): poll until nothing under TempPath is mounted, so remove_all
    //can never traverse a still-live PERSIST bind into PackagePath/USERDATA. Wait, don't assume.
    bool Clear = StaleMounts.empty();
    for (int Attempt = 0; !Clear && Attempt < 30; ++Attempt) //~6s max for lazy unmounts to settle
    {
        QThread::msleep(200);
        Clear = MountpointsUnder(TempPath).empty();
    }
    if (!Clear)
    {
        LogErr("VfsMount::CleanStaleRuntime",
               "Stale mounts under TEMP did not clear in time; leaving it in place to protect USERDATA.");
        return;
    }

    if (Exists)
    {
        std::error_code Ec;
        std::filesystem::remove_all(TempPath, Ec);
        if (Ec) LogWarn("VfsMount::CleanStaleRuntime", "Could not remove stale TEMP: " + Ec.message());
        else if (!StaleMounts.empty() || Exists)
            LogSucc("VfsMount::CleanStaleRuntime", "Stale runtime cleared.");
    }
}