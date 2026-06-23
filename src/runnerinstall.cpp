#include "runnerinstall.h"
#include "vfsmount.h"        // VfsMount::SpawnVidyagodfs (mount the build to hydrate / wineboot)
#include "varsubst.h"        // VarSubst::StringVariableSubstitution (wineboot arg/env expansion)
#include "processenv.h"      // RunCommand + SystemToolEnv
#include "runnerwrapper.h"   // RunnerWrapper predicates (GeneratesPrefix / DefPrefixArtifact / Variant / ...)
#include "ipfswrapper.h"     // IpfsWrapper (build-layer content fetch/has-local)
#include "commonutils.h"     // Log*

#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

//VFS layer helpers (IsVfsLayer/LayerType/ResolveNodeOrder/...) live in ManifestModel; bring them in unqualified.
using namespace ManifestModel;

namespace {
//Synthesize the minimal runner "package" json from a ROLE:runner node + its content closure (the build). Shared by
//the node-level install (ImportRunnerNode) and node-level target collection (CollectRunnerNodeTargets).
bool BuildRunnerPkgFromNode(const NodeIndex &Idx, const std::string &RunnerNodeId,
                            nlohmann::ordered_json &Pkg, std::string &PackageDir, std::string &Vid, std::string *Error)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) { if (Error) *Error = "not a runner node: " + RunnerNodeId; return false; }

    nlohmann::ordered_json Components = nlohmann::ordered_json::array();
    nlohmann::ordered_json Modules    = nlohmann::ordered_json::array();
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (!C || !C->Layers.is_array() || C->Layers.empty()) continue;
        Components.push_back(nlohmann::ordered_json{{"COMPONENTID", Id}, {"SUBCOMPONENTS", C->Layers}});
        Modules.push_back(nlohmann::ordered_json{{"COMPONENT", Id}});
    }
    nlohmann::ordered_json Variant = R->Exec.is_object() ? R->Exec : nlohmann::ordered_json::object();
    Variant["VARIANT_ID"]     = "default";
    Variant["HOST_PLATFORM"]  = R->HostPlatform;
    Variant["GUEST_PLATFORM"] = R->GuestPlatform;
    Variant["MODULES"]        = Modules;
    Variant["RECOMMENDED"]    = true;

    Pkg = nlohmann::ordered_json::object();
    Pkg["PACKAGEUID"] = R->Uid.empty() ? R->NodeId : R->Uid;
    Pkg["RUNNERS"]    = nlohmann::ordered_json::array({ nlohmann::ordered_json{
        {"RUNNER_ID", R->NodeId}, {"NAME", R->NodeId}, {"VARIANTS", nlohmann::ordered_json::array({Variant})} } });
    Pkg["COMPONENTS"] = Components;
    PackageDir = R->BundleDir.string();
    Vid = "default";
    return true;
}

//Gather a runner variant's MISSING build-layer fetch targets (no fetching). Shared by ImportRunner (fetch loop) and
//the node-level collection. The runner build = the VFS layers of the components this variant's MODULES enable.
void CollectBuildTargets(const nlohmann::ordered_json &RunnerPkg, const std::filesystem::path &Pkg,
                         const nlohmann::ordered_json &Variant, std::vector<IpfsWrapper::FetchTarget> &Out)
{
    std::vector<std::string> WantComps;
    for (const auto &M : Variant.value("MODULES", nlohmann::ordered_json::array()))
        if (M.is_object()) { std::string C = M.value("COMPONENT", std::string()); if (!C.empty()) WantComps.push_back(C); }
    auto Wanted = [&](const nlohmann::ordered_json &C){ for (const auto &W : WantComps) if (C.value("COMPONENTID", std::string()) == W) return true; return false; };

    std::error_code Ec;
    for (const auto &C : RunnerPkg.value("COMPONENTS", nlohmann::ordered_json::array()))
    {
        if (!Wanted(C) || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            if (!IsVfsLayer(S.value("TYPE", std::string()))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(S, Pkg, Local, Cid);
            if (Cid.empty() || Local == Pkg || std::filesystem::exists(Local, Ec)) continue;
            Out.push_back({Cid, Local.string(), false});
        }
    }
}
} // namespace

//Installs a runner package: fetches its VFSZipLayer CIDs (IPFS), and for wine runners generates the
//one-time read-only DEFPREFIX artifact by mounting the build and running `wineboot` once. Idempotent.
bool RunnerInstall::ImportRunner(nlohmann::ordered_json &GlobalConfigJSON, const nlohmann::ordered_json &RunnerPkg, const std::string &PackageDir, const std::string &VariantId, std::string *Error)
{
    (void)GlobalConfigJSON;
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("RunnerInstall::ImportRunner", M); return false; };

    const std::string Id  = RunnerWrapper::RunnerId(RunnerPkg);
    const std::string Vid = VariantId.empty() ? RunnerWrapper::DefaultVariantId(RunnerPkg) : VariantId;
    if (Id.empty())  return Fail("runner package has no RUNNER_ID");
    if (Vid.empty()) return Fail("runner '" + Id + "' has no variant to install");
    if (PackageDir.empty()) return Fail("runner '" + Id + "' has no package directory");
    const nlohmann::ordered_json R = RunnerWrapper::Variant(RunnerPkg, Vid);   // the runner VARIANT (exec params)
    if (R.empty())   return Fail("runner '" + Id + "' has no variant '" + Vid + "'");
    const std::string Label = Id + ":" + Vid;
    const std::filesystem::path Pkg(PackageDir);
    std::error_code Ec;

    //The components this variant's MODULES enable — the runner build is their VFS layers.
    std::vector<std::string> WantComps;
    for (const auto &M : R.value("MODULES", nlohmann::ordered_json::array()))
        if (M.is_object()) { std::string C = M.value("COMPONENT", std::string()); if (!C.empty()) WantComps.push_back(C); }
    auto Wanted = [&](const nlohmann::ordered_json &C){ for (const auto &W : WantComps) if (C.value("COMPONENTID", std::string()) == W) return true; return false; };

    //1. Hydrate this variant's build layers IN PLACE in the runner's library dir, concurrently (bounded by the
    //global download throttle), so a multi-layer runner build downloads in parallel instead of one-at-a-time.
    std::vector<IpfsWrapper::FetchTarget> Targets;
    CollectBuildTargets(RunnerPkg, Pkg, R, Targets);
    { std::string E; if (!IpfsWrapper::FetchTargetsConcurrent(Targets, &E)) return Fail("could not fetch runner build (" + E + ")"); }
    LogSucc("RunnerInstall::ImportRunner", "Hydrated runner build for " + Label + " (" + std::to_string(Targets.size()) + " layer(s))");
    if (!RunnerWrapper::GeneratesPrefix(RunnerPkg, Vid)) return true;    // no PREFIX_GENERATE: no prefix to build

    //2. Generate the one-time DEFPREFIX in the runner's library dir (idempotent): PackageDir/__DEFPREFIX__/<vid>.
    //The whole artifact is mounted at the runtime root later, so the prefix hives live at <artifact>/<PrefixRoot>,
    //PrefixRoot = CONTENT_ROOT up to /drive_c ("" for wine, "pfx" for proton) — derived, no PREFIX_SUBPATH.
    const std::filesystem::path DefArtifact = RunnerWrapper::DefPrefixArtifact(PackageDir, Vid);
    const std::string ContentRoot = R.value("CONTENT_ROOT", std::string());
    std::string PrefixRoot;
    { const auto Pos = ContentRoot.find("drive_c");
      if (Pos != std::string::npos && Pos != 0) PrefixRoot = ContentRoot.substr(0, Pos - 1); }
    const std::filesystem::path PrefixDir = PrefixRoot.empty() ? DefArtifact : (DefArtifact / PrefixRoot);
    if (std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec))
    { LogOut("RunnerInstall::ImportRunner", "DEFPREFIX already present for " + Label); return true; }

    //Mount the (now-local) build read-only at a temp mount under __DEFPREFIX__ so `wineboot` can run from it.
    const std::filesystem::path MountDir = Pkg / "__DEFPREFIX__" / (".buildmount-" + Vid);
    std::filesystem::create_directories(MountDir.parent_path(), Ec);
    nlohmann::ordered_json Spec;
    Spec["mountpoint"] = MountDir.string(); Spec["uid"] = 1000; Spec["gid"] = 1000;
    Spec["readonly"] = true; Spec["writelayer"] = nullptr;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const auto &C : RunnerPkg.value("COMPONENTS", nlohmann::ordered_json::array()))
    {
        if (!Wanted(C)) continue;
        for (const auto &S : C.value("SUBCOMPONENTS", nlohmann::ordered_json::array()))
        {
            const std::string T = S.value("TYPE", std::string());
            const std::string LType = (T == "VFSZipLayer") ? "zip" : (T == "VFSDirLayer") ? "dir" : (T == "VFSFileLayer") ? "file" : "";
            if (LType.empty()) continue;
            Layers.push_back({{"type", LType}, {"source", ResolveLayerSource(S, Pkg)},
                              {"target", S.value("TARGET", std::string())},
                              {"submounts", S.value("SUBMOUNTS", nlohmann::ordered_json::array())}, {"rw", false}});
        }
    }
    Spec["layers"] = Layers;
    if (!VfsMount::SpawnVidyagodfs(Spec, MountDir, Pkg / "__DEFPREFIX__" / (".buildmount-" + Vid + ".spec.json")))
        return Fail("could not mount runner build for install");

    //Build the runner ENV/ARGS/EXECUTABLE with the install-time variable bindings. The runner ENV points its
    //prefix var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; bind it to the artifact root we're
    //building, so proton writes <artifact>/pfx and wine writes <artifact>/drive_c (mounted whole at launch).
    std::map<std::string, std::string> Vars;
    Vars["RunnerMount"]  = MountDir.string();
    Vars["RuntimePath"]  = DefArtifact.string();
    Vars["TempPath"]     = DefArtifact.string();

    std::string Program = R.value("EXECUTABLE", std::string("%RunnerMount%/proton"));
    VarSubst::StringVariableSubstitution(Program, Vars);
    //Args with the content target swapped for wineboot — keep the launcher verb, drop the content-bearing arg.
    QStringList Args;
    for (const auto &A : R.value("ARGS", nlohmann::ordered_json::array()))
    { std::string a = std::string(A); if (ArgReferencesContent(a)) continue;
      VarSubst::StringVariableSubstitution(a, Vars); Args << QString::fromStdString(a); }
    Args << "wineboot";

    QProcessEnvironment Env = SystemToolEnv();                                   // system proton/wine, not AppImage libs
    const nlohmann::ordered_json RemoveEnv = R.value("REMOVE_ENV", nlohmann::ordered_json::array());
    const nlohmann::ordered_json RunnerEnv = R.value("ENV", nlohmann::ordered_json::object());
    for (const auto &K : RemoveEnv) Env.remove(QString::fromStdString(std::string(K)));
    for (auto &[K, V] : RunnerEnv.items())
    { std::string v = V.get<std::string>(); VarSubst::StringVariableSubstitution(v, Vars); Env.insert(QString::fromStdString(K), QString::fromStdString(v)); }

    std::filesystem::create_directories(DefArtifact, Ec);
    LogOut("RunnerInstall::ImportRunner", "Generating DEFPREFIX: " + Program + " " + Args.join(' ').toStdString());
    QProcess P; P.setProgram(QString::fromStdString(Program)); P.setArguments(Args); P.setProcessEnvironment(Env);
    P.start(); P.waitForFinished(-1);
    std::cout << P.readAllStandardError().toStdString() << std::endl << P.readAllStandardOutput().toStdString() << std::endl;
    const bool BootOk = (P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0 && std::filesystem::exists(PrefixDir, Ec));

    RunCommand("fusermount3", {"-uz", MountDir.string()}, SystemToolEnv());
    std::filesystem::remove_all(MountDir, Ec);                                   // drop the transient build mountpoint
    if (!BootOk) { std::filesystem::remove_all(DefArtifact, Ec); return Fail("wineboot failed to build DEFPREFIX for " + Label); }
    LogSucc("RunnerInstall::ImportRunner", "Installed runner " + Label + " (DEFPREFIX at " + DefArtifact.string() + ")");
    return true;
}

//Synthesizes a minimal RUNNERS[]-shaped package from a runner node + its content closure so the tested
//ImportRunner machinery installs it unchanged. The build content nodes (the runner's PARENTS) become COMPONENTS;
//the runner node's EXEC becomes the single "default" variant — matching DerivePaths (RunnerVariantID="default").
bool RunnerInstall::ImportRunnerNode(nlohmann::ordered_json &GlobalConfigJSON, const NodeIndex &Idx,
                                        const std::string &RunnerNodeId, std::string *Error)
{
    nlohmann::ordered_json Pkg; std::string PackageDir, Vid;
    if (!BuildRunnerPkgFromNode(Idx, RunnerNodeId, Pkg, PackageDir, Vid, Error)) return false;
    return ImportRunner(GlobalConfigJSON, Pkg, PackageDir, Vid, Error);
}

bool RunnerInstall::CollectRunnerNodeTargets(const NodeIndex &Idx, const std::string &RunnerNodeId,
                                             std::vector<IpfsWrapper::FetchTarget> &Out, std::string *Error)
{
    nlohmann::ordered_json Pkg; std::string PackageDir, Vid;
    if (!BuildRunnerPkgFromNode(Idx, RunnerNodeId, Pkg, PackageDir, Vid, Error)) return false;
    CollectBuildTargets(Pkg, std::filesystem::path(PackageDir), RunnerWrapper::Variant(Pkg, Vid), Out);
    return true;
}

bool RunnerInstall::RunnerNodeImported(const NodeIndex &Idx, const std::string &RunnerNodeId)
{
    const Node *R = Idx.Find(RunnerNodeId);
    if (!R || !R->IsRunner()) return false;

    //Every build VFS layer hydrated locally (the runner's content closure).
    bool AllPresent = true; bool AnyBuild = false;
    for (const std::string &Id : ManifestModel::ResolveNodeOrder(Idx, RunnerNodeId, {}))
    {
        if (Id == RunnerNodeId) continue;
        const Node *C = Idx.Find(Id);
        if (!C || !C->Layers.is_array()) continue;
        for (const auto &L : C->Layers)
        {
            if (!IsVfsLayer(LayerType(L))) continue;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(L, C->BundleDir, Local, Cid);
            if (Local == C->BundleDir) continue;                                  // no PATH
            AnyBuild = true;
            std::error_code Ec;
            if (!std::filesystem::exists(Local, Ec)) AllPresent = false;
        }
    }
    if (!AnyBuild) return true;                                                   // ships no build → always installed
    if (!AllPresent) return false;

    //PREFIX_GENERATE runners also need their DEFPREFIX artifact present (<bundle>/__DEFPREFIX__/default/<prefixRoot>).
    const bool PrefixGen = R->Exec.is_object() && R->Exec.value("PREFIX_GENERATE", false);
    if (!PrefixGen) return true;
    const std::string CR = R->Exec.is_object() ? R->Exec.value("CONTENT_ROOT", std::string()) : std::string();
    std::string PrefixRoot;
    { const auto Pos = CR.find("drive_c"); if (Pos != std::string::npos && Pos != 0) PrefixRoot = CR.substr(0, Pos - 1); }
    std::filesystem::path Artifact = std::filesystem::path(RunnerWrapper::DefPrefixArtifact(R->BundleDir.string(), "default"));
    std::filesystem::path PrefixDir = PrefixRoot.empty() ? Artifact : (Artifact / PrefixRoot);
    std::error_code Ec;
    return std::filesystem::exists(PrefixDir, Ec) && !std::filesystem::is_empty(PrefixDir, Ec);
}