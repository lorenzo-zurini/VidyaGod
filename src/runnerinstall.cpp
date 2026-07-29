#include "runnerinstall.h"
#include "vfsmount.h"        // VfsMount::SpawnVidyagodfs (mount the build to wineboot)
#include "varsubst.h"        // VarSubst::StringVariableSubstitution (wineboot arg/env expansion)
#include "processenv.h"      // RunCommand + SystemToolEnv
#include "runnerwrapper.h"   // RunnerWrapper::DefPrefixDir
#include "ipfswrapper.h"     // IpfsWrapper (build-layer content fetch)
#include "registrywrapper.h" // RegistryWrapper::DiffToRegEdits (capture wineboot's registry delta as RegEdits)
#include "commonutils.h"     // Log*
#include <sstream>

#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

//VFS-layer helpers (IsVfsLayer/LayerType/ResolveNodeOrder/ResolveLayerSource/LayerLocator) live in ManifestModel.
using namespace ManifestModel;

namespace {
//A runner's BUILD = its content closure (PARENTS), excluding the runner node itself: the content nodes carrying the
//runner's VFS layers (proton/wine binaries, …), in load order. A runner node's OWN LAYERS are never the build (they
//are ignored — validation warns); the build always lives on PARENT content nodes.
std::vector<const Node *> RunnerBuildNodes(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    std::vector<const Node *> Out;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (C && C->Layers.is_array() && !C->Layers.empty()) Out.push_back(C);
    }
    return Out;
}

//The DEFPREFIX completion sentinel: a sibling of the artifact dir (…/DEFPREFIX/default.ok) so it is NEVER part of
//the prefix that gets mounted at launch. Its presence == a wineboot that ran to completion; absence (even with a
//non-empty artifact dir) == not built / partial.
std::filesystem::path DefPrefixSentinel(const Node &R)
{
    return std::filesystem::path(RunnerWrapper::DefPrefixDir(R.BundleDir.string()) + ".ok");
}
} // namespace

bool RunnerInstall::CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                                             std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) { if (Error) *Error = "not a runner node: " + RunnerNodeId; return false; }
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);                            // resolve against the layer's OWN node bundle
            if (Cid.empty() || Local == C->BundleDir || std::filesystem::exists(Local, Ec)) continue;  // present / no source
            Out.push_back({Cid, Local.string(), false});
        }
    return true;
}

//Generate a PREFIX_GENERATE runner's one-time read-only DEFPREFIX by mounting its (ALREADY-LOCAL) build and running
//`wineboot` once. NO fetching — the build must already be hydrated by the download pump / --import-runner. Completion
//is marked by a sibling sentinel (…/default.ok); Force wipes any existing/partial artifact and rebuilds.
bool RunnerInstall::GenerateRunnerDefPrefix(const NodeIndex &Idx, const std::string &RunnerNodeId, bool Force, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("RunnerInstall::GenerateRunnerDefPrefix", M); return false; };

    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return Fail("not a runner node: " + RunnerNodeId);
    const nlohmann::ordered_json E = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    const std::filesystem::path Bundle(R->BundleDir);
    std::error_code Ec;

    //Non-PREFIX_GENERATE runners have no prefix to build — nothing to do.
    if (!E.value("PREFIX_GENERATE", false)) return true;

    //Node-declared prefix: the runner ships its prefix as node LAYERS (a config_info FileEdit + default_pfx/DLL
    //VFSDirLayers), assembled from the delta chain at launch. No per-machine wineboot, no DEFPREFIX artifact — the
    //install step is GONE. (Detected by the config_info FileEdit the authoring emits.)
    if (R->Layers.is_array())
        for (const auto &L : R->Layers)
            if (L.value("TYPE", std::string()) == "FileEdit" && L.value("FILE", std::string()) == "config_info")
            { LogOut("RunnerInstall::GenerateRunnerDefPrefix", "Node-declared prefix — no wineboot/DEFPREFIX for " + RunnerNodeId); return true; }

    const std::filesystem::path DefArtifact = RunnerWrapper::DefPrefixDir(R->BundleDir.string());
    const std::filesystem::path Sentinel    = DefPrefixSentinel(*R);

    //Already fully built (sentinel present) and not forcing → done. A non-empty artifact WITHOUT the sentinel is a
    //partial/interrupted prefix: fall through and rebuild it (this is the crux of the "partial looks complete" bug).
    if (!Force && std::filesystem::exists(Sentinel, Ec))
    { LogOut("RunnerInstall::GenerateRunnerDefPrefix", "DEFPREFIX already present for " + RunnerNodeId); return true; }

    //Rebuild from a clean slate: drop any existing (partial or forced) artifact + its stale sentinel, and force-unmount
    //+ remove a `.buildmount` a killed prior run may have left wedged (the "delete leftover files to stop the crash").
    std::filesystem::remove_all(DefArtifact, Ec);
    std::filesystem::remove(Sentinel, Ec);
    const std::filesystem::path MountDir = Bundle / "DEFPREFIX" / ".buildmount";
    RunCommand("fusermount3", {"-uz", MountDir.string()}, SystemToolEnv());
    std::filesystem::remove_all(MountDir, Ec);

    const std::string ContentRoot = E.value("CONTENT_ROOT", std::string());
    std::string PrefixRoot;
    { const auto Pos = ContentRoot.find("drive_c");
      if (Pos != std::string::npos && Pos != 0) PrefixRoot = ContentRoot.substr(0, Pos - 1); }
    const std::filesystem::path PrefixDir = PrefixRoot.empty() ? DefArtifact : (DefArtifact / PrefixRoot);

    //Mount the (now-local) build read-only at a temp mount under DEFPREFIX so `wineboot` can run from it.
    std::filesystem::create_directories(MountDir.parent_path(), Ec);
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = MountDir.string(); Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    // Collect CustomVar DEFAULTs from the runner closure so a shared library node's TARGET:"%X_TARGET%" resolves during
    // prefix generation too (the composing runner defines X_TARGET; there is no user override at prefix-gen time, so the
    // author DEFAULT is exactly right). Mirrors the launch-time substitution in VfsMount::MountRunnerBuild.
    std::map<std::string, std::string> TgtVars;
    auto CollectCvars = [&](const Node *C){
        if (!C || !C->Layers.is_array()) return;
        for (const auto &S : C->Layers)
            if (S.value("TYPE", std::string()) == "CustomVar" && S.contains("KEY") && S["KEY"].is_string())
                TgtVars[std::string(S["KEY"])] = S.value("DEFAULT", std::string());
    };
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId)) CollectCvars(C);
    CollectCvars(R);   // the runner node itself carries the placement bindings (X_TARGET) — RunnerBuildNodes omits it
    auto SubstTarget = [&](const std::string &In) -> std::string {
        std::string T = In;
        VarSubst::StringVariableSubstitution(T, TgtVars);
        for (char &c : T) if (c == '\\') c = '/';
        while (!T.empty() && T.front() == '/') T.erase(T.begin());
        while (!T.empty() && T.back()  == '/') T.pop_back();
        return T;
    };
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &S : C->Layers)
        {
            // A runner build may be a .vgdelta CHAIN (Proton versions = base + deltas): MakeVfsSpecLayer handles
            // VFSDeltaLayer so the DEFPREFIX is generated from the actual pinned version, not just the base zip.
            if (!IsVfsLayer(S.value("TYPE", std::string()))) continue;
            std::string BaseTarget;
            if (S.value("TYPE", std::string()) == "VFSDeltaLayer" && S.contains("BASE_TARGET") && S["BASE_TARGET"].is_string())
                BaseTarget = SubstTarget(std::string(S["BASE_TARGET"]));
            Layers.push_back(MakeVfsSpecLayer(S, ResolveLayerSource(S, C->BundleDir), SubstTarget(S.value("TARGET", std::string())), BaseTarget));
        }
    Spec["layers"] = Layers;
    if (!VfsMount::SpawnVidyagodfs(Spec, MountDir, Bundle / "DEFPREFIX" / ".buildmount.spec.json"))
        return Fail("could not mount runner build for prefix generation");

    //Build the runner ENV/ARGS/EXECUTABLE with the install-time variable bindings. The runner ENV points its prefix
    //var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; bind it to the artifact root we're building, so
    //proton writes <artifact>/pfx and wine writes <artifact>/drive_c (mounted whole at launch).
    std::map<std::string, std::string> Vars;
    Vars["RunnerMount"] = MountDir.string();
    Vars["RuntimePath"] = DefArtifact.string();
    Vars["TempPath"]    = DefArtifact.string();
    //Fold the runner-closure CustomVar defaults (already gathered in TgtVars) into the ENV substitution so the wineboot
    //runs with the SAME toggle values a game launch does (e.g. PROTON_USE_WINED3D → "0", not the literal "%..%"). Proton
    //derives config_info fields (use_wined3d/use_dxvk_dxgi) from these — if they differ, the captured recipe's config_info
    //won't match at launch and proton re-copies the DLLs instead of using the mounted ones (defeats zero-copy).
    for (const auto &[K, V] : TgtVars) Vars.emplace(K, V);

    std::string Program = E.value("EXECUTABLE", std::string("%RunnerMount%/proton"));
    VarSubst::StringVariableSubstitution(Program, Vars);
    //Args with the content target swapped for wineboot — keep the launcher verb, drop the content-bearing arg.
    QStringList Args;
    for (const auto &A : E.value("ARGS", nlohmann::ordered_json::array()))
    { std::string a = std::string(A); if (ArgReferencesContent(a)) continue;
      VarSubst::StringVariableSubstitution(a, Vars); Args << QString::fromStdString(a); }
    Args << "wineboot";

    QProcessEnvironment ProcEnv = SystemToolEnv();                               // system proton/wine, not AppImage libs
    for (const auto &K : E.value("REMOVE_ENV", nlohmann::ordered_json::array())) ProcEnv.remove(QString::fromStdString(std::string(K)));
    for (auto &[K, V] : E.value("ENV", nlohmann::ordered_json::object()).items())
    { std::string v = V.get<std::string>(); VarSubst::StringVariableSubstitution(v, Vars); ProcEnv.insert(QString::fromStdString(K), QString::fromStdString(v)); }

    std::filesystem::create_directories(DefArtifact, Ec);
    LogOut("RunnerInstall::GenerateRunnerDefPrefix", "Generating DEFPREFIX: " + Program + " " + Args.join(' ').toStdString());
    QProcess P; P.setProgram(QString::fromStdString(Program)); P.setArguments(Args); P.setProcessEnvironment(ProcEnv);
    P.start(); P.waitForFinished(-1);
    const std::string Serr = P.readAllStandardError().toStdString();
    const std::string Sout = P.readAllStandardOutput().toStdString();
    std::cout << Serr << '\n' << Sout << '\n';
    const bool BootOk = (P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0 && std::filesystem::exists(PrefixDir, Ec));

    // Capture a tiny PREFIX RECIPE while the build is still mounted: config_info (with the mount paths templatized to
    // %RunnerMount%/%TempPath% so it matches at launch regardless of where the build mounts), the wineboot registry
    // delta as declarative RegEdits (vs the shipped default_pfx), and tracked_files/version. This is what lets the
    // prefix be ASSEMBLED from mounts at launch — proton sees a matching version+config_info and skips its DLL copy —
    // instead of storing a ~700 MB DEFPREFIX tree per runner-version.
    if (BootOk)
    {
        const std::filesystem::path Recipe = Bundle / "DEFPREFIX" / "recipe";
        std::filesystem::create_directories(Recipe, Ec);
        std::string DefaultPfxDir;
        { std::ifstream in(DefArtifact / "config_info"); std::stringstream ss; ss << in.rdbuf(); std::string CI = ss.str();
          //config_info line 8 (index 7) is default_pfx_dir — grab it (absolute, mount still up) for the RegEdit baseline.
          { std::stringstream ls(CI); std::string ln; for (int i = 0; std::getline(ls, ln); ++i) if (i == 7) { DefaultPfxDir = ln; break; } }
          auto Repl = [&](const std::string &F, const std::string &T){ if (F.empty()) return; size_t p; while ((p = CI.find(F)) != std::string::npos) CI.replace(p, F.size(), T); };
          Repl(MountDir.string(), "%RunnerMount%"); Repl(DefArtifact.string(), "%TempPath%");
          //config_info line 12 (index 11) is builtin_dll_copy. The wineboot captured GE's protonfixes "*", but at LAUNCH
          //we inject a user_settings.py that restores proton's default list (VfsMount::ProtonDefaultDllCopy) so builtins
          //symlink instead of copy. Record THAT here so the captured config_info matches the launch → config-match skip.
          { std::vector<std::string> Ls; { std::stringstream ls(CI); std::string ln; while (std::getline(ls, ln)) Ls.push_back(ln); }
            if (Ls.size() > 11) { Ls[11] = VfsMount::ProtonDefaultDllCopy; CI.clear(); for (size_t i = 0; i < Ls.size(); ++i) CI += Ls[i] + (i + 1 < Ls.size() ? "\n" : ""); } }
          std::ofstream(Recipe / "config_info.tmpl") << CI; }
        if (!DefaultPfxDir.empty())
        { RegistryWrapper Pfx, Base;
          if (Pfx.LoadPrefix(PrefixDir) && Base.LoadPrefix(DefaultPfxDir))
              std::ofstream(Recipe / "wineboot.regedits.json") << Pfx.DiffToRegEdits(Base).dump(1); }
        std::filesystem::copy_file(DefArtifact / "tracked_files", Recipe / "tracked_files", std::filesystem::copy_options::overwrite_existing, Ec);
        std::filesystem::copy_file(DefArtifact / "version",       Recipe / "version",       std::filesystem::copy_options::overwrite_existing, Ec);
        LogSucc("RunnerInstall::GenerateRunnerDefPrefix", "Captured prefix recipe (config_info + wineboot RegEdits + tracked_files) at " + Recipe.string());
    }

    RunCommand("fusermount3", {"-uz", MountDir.string()}, SystemToolEnv());
    std::filesystem::remove_all(MountDir, Ec);                                   // drop the transient build mountpoint
    if (!BootOk)
    {
        //Drop the PARTIAL prefix but KEEP the fetched build (so a retry is prefix-only, no re-download). Fold the
        //wineboot output tail into the error so the GUI/log shows WHY it failed (env/wow64/…), not just "failed".
        std::filesystem::remove_all(DefArtifact, Ec);
        std::string Tail = Serr.empty() ? Sout : Serr;
        if (Tail.size() > 600) Tail = "…" + Tail.substr(Tail.size() - 600);
        return Fail("wineboot failed to build DEFPREFIX for " + RunnerNodeId + (Tail.empty() ? "" : (" — " + Tail)));
    }
    { std::ofstream Ok(Sentinel); Ok << "ok\n"; }                                // completion marker (sibling of the artifact)
    LogSucc("RunnerInstall::GenerateRunnerDefPrefix", "Generated DEFPREFIX for " + RunnerNodeId + " (" + DefArtifact.string() + ")");
    return true;
}

bool RunnerInstall::RunnerBuildPresent(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;

    //Every build VFS layer hydrated locally (the runner's content closure). A runner that ships no build is "present".
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            if (!std::filesystem::exists(Local, Ec)) return false;                // a build layer missing → not present
        }
    return true;
}

bool RunnerInstall::RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;
    if (!RunnerBuildPresent(Idx, RunnerNodeId)) return false;                     // build not hydrated → not imported

    //PREFIX_GENERATE runners are only "imported" once their DEFPREFIX is FULLY built — marked by the completion
    //sentinel (a partial/interrupted prefix leaves the artifact dir non-empty but the sentinel absent).
    const bool PrefixGen = R->Exec.is_object() && R->Exec.value("PREFIX_GENERATE", false);
    if (!PrefixGen) return true;
    std::error_code Ec;
    return std::filesystem::exists(DefPrefixSentinel(*R), Ec);
}
