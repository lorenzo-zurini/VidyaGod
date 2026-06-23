#include "runnerinstall.h"
#include "vfsmount.h"        // VfsMount::SpawnVidyagodfs (mount the build to wineboot)
#include "varsubst.h"        // VarSubst::StringVariableSubstitution (wineboot arg/env expansion)
#include "processenv.h"      // RunCommand + SystemToolEnv
#include "runnerwrapper.h"   // RunnerWrapper::DefPrefixDir
#include "ipfswrapper.h"     // IpfsWrapper (build-layer content fetch)
#include "commonutils.h"     // Log*

#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
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

//Installs a runner NODE: fetches its build closure's VFS layers (IPFS), and for a PREFIX_GENERATE runner generates
//the one-time read-only DEFPREFIX by mounting the build and running `wineboot` once. Idempotent.
bool RunnerInstall::ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                                     const std::string &RunnerNodeId, std::string *Error)
{
    (void)GlobalConfigJSON;
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("RunnerInstall::ImportRunnerNode", M); return false; };

    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return Fail("not a runner node: " + RunnerNodeId);
    const nlohmann::ordered_json E = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    const std::filesystem::path Bundle(R->BundleDir);
    std::error_code Ec;

    //1. Hydrate the build closure's VFS layers concurrently (bounded by the global download throttle).
    std::vector<IpfsWrapper::FetchTarget> Targets;
    if (!CollectRunnerNodeTargets(Idx, RunnerNodeId, Targets, Error)) return false;
    { std::string Er; if (!IpfsWrapper::FetchTargetsConcurrent(Targets, &Er)) return Fail("could not fetch runner build (" + Er + ")"); }
    LogSucc("RunnerInstall::ImportRunnerNode", "Hydrated runner build for " + RunnerNodeId + " (" + std::to_string(Targets.size()) + " layer(s))");

    //2. PREFIX_GENERATE → build the one-time read-only DEFPREFIX (idempotent). The whole artifact is mounted at the
    //runtime root at launch, so the prefix hives live at <artifact>/<PrefixRoot>, PrefixRoot = CONTENT_ROOT up to
    /// drive_c ("" for wine, "pfx" for proton) — derived, no PREFIX_SUBPATH.
    if (!E.value("PREFIX_GENERATE", false)) return true;
    const std::filesystem::path DefArtifact = RunnerWrapper::DefPrefixDir(R->BundleDir.string());
    const std::string ContentRoot = E.value("CONTENT_ROOT", std::string());
    std::string PrefixRoot;
    { const auto Pos = ContentRoot.find("drive_c");
      if (Pos != std::string::npos && Pos != 0) PrefixRoot = ContentRoot.substr(0, Pos - 1); }
    const std::filesystem::path PrefixDir = PrefixRoot.empty() ? DefArtifact : (DefArtifact / PrefixRoot);
    if (std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec))
    { LogOut("RunnerInstall::ImportRunnerNode", "DEFPREFIX already present for " + RunnerNodeId); return true; }

    //Mount the (now-local) build read-only at a temp mount under __DEFPREFIX__ so `wineboot` can run from it.
    const std::filesystem::path MountDir = Bundle / "__DEFPREFIX__" / ".buildmount";
    std::filesystem::create_directories(MountDir.parent_path(), Ec);
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = MountDir.string(); Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &S : C->Layers)
        {
            const std::string T = S.value("TYPE", std::string());
            const std::string LType = (T == "VFSZipLayer") ? "zip" : (T == "VFSDirLayer") ? "dir" : (T == "VFSFileLayer") ? "file" : "";
            if (LType.empty()) continue;
            Layers.push_back({{"type", LType}, {"source", ResolveLayerSource(S, C->BundleDir)},
                              {"target", S.value("TARGET", std::string())},
                              {"submounts", S.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}});
        }
    Spec["layers"] = Layers;
    if (!VfsMount::SpawnVidyagodfs(Spec, MountDir, Bundle / "__DEFPREFIX__" / ".buildmount.spec.json"))
        return Fail("could not mount runner build for install");

    //Build the runner ENV/ARGS/EXECUTABLE with the install-time variable bindings. The runner ENV points its prefix
    //var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; bind it to the artifact root we're building, so
    //proton writes <artifact>/pfx and wine writes <artifact>/drive_c (mounted whole at launch).
    std::map<std::string, std::string> Vars;
    Vars["RunnerMount"] = MountDir.string();
    Vars["RuntimePath"] = DefArtifact.string();
    Vars["TempPath"]    = DefArtifact.string();

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
    LogOut("RunnerInstall::ImportRunnerNode", "Generating DEFPREFIX: " + Program + " " + Args.join(' ').toStdString());
    QProcess P; P.setProgram(QString::fromStdString(Program)); P.setArguments(Args); P.setProcessEnvironment(ProcEnv);
    P.start(); P.waitForFinished(-1);
    std::cout << P.readAllStandardError().toStdString() << std::endl << P.readAllStandardOutput().toStdString() << std::endl;
    const bool BootOk = (P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0 && std::filesystem::exists(PrefixDir, Ec));

    RunCommand("fusermount3", {"-uz", MountDir.string()}, SystemToolEnv());
    std::filesystem::remove_all(MountDir, Ec);                                   // drop the transient build mountpoint
    if (!BootOk) { std::filesystem::remove_all(DefArtifact, Ec); return Fail("wineboot failed to build DEFPREFIX for " + RunnerNodeId); }
    LogSucc("RunnerInstall::ImportRunnerNode", "Installed runner " + RunnerNodeId + " (DEFPREFIX at " + DefArtifact.string() + ")");
    return true;
}

bool RunnerInstall::RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;

    //Every build VFS layer hydrated locally (the runner's content closure).
    bool AnyBuild = false;
    std::error_code Ec;
    for (const Node *C : RunnerBuildNodes(Idx, RunnerNodeId))
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            AnyBuild = true;
            if (!std::filesystem::exists(Local, Ec)) return false;                // a build layer missing → not imported
        }
    if (!AnyBuild) return true;                                                   // ships no build → always installed

    //PREFIX_GENERATE runners also need their DEFPREFIX artifact present (<bundle>/__DEFPREFIX__/default/<prefixRoot>).
    const bool PrefixGen = R->Exec.is_object() && R->Exec.value("PREFIX_GENERATE", false);
    if (!PrefixGen) return true;
    const std::string CR = R->Exec.is_object() ? R->Exec.value("CONTENT_ROOT", std::string()) : std::string();
    std::string PrefixRoot;
    { const auto Pos = CR.find("drive_c"); if (Pos != std::string::npos && Pos != 0) PrefixRoot = CR.substr(0, Pos - 1); }
    const std::filesystem::path Artifact = RunnerWrapper::DefPrefixDir(R->BundleDir.string());
    const std::filesystem::path PrefixDir = PrefixRoot.empty() ? Artifact : (Artifact / PrefixRoot);
    return std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec);
}
