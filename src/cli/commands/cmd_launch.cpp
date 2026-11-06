// `VidyaGod launch …`, `registry …`, `edits …` — the launch engine: resolve a node into the launch it would be, run it,
// or build its runtime and keep it up to look inside (the app keeps it until `launch teardown`); what is running, and
// stopping it; the mounts sessions left. And the engine's pieces on their own: a wine prefix's registry (read, edit,
// diff, apply a node's REG edits), and the file edits and binary patches a launch applies, on any folder.
#include "command.h"
#include "cmdutil.h"
#include "boxprefix.h"
#include "binarypatch.h"
#include "cli/climodes.h"
#include "containerwrapper.h"
#include "fileedits.h"
#include "launchresolver.h"
#include "registrywrapper.h"
#include "vfsmount.h"

#include <cctype>
#include <set>
#include <fstream>
#include <sstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;
namespace fs = std::filesystem;

// What a launch logged as errors, one a line (the reason a launch or a resolve failed).
std::string LoggedErrors(const Diagnostics::Report &R)
{
    std::string M;
    for (const auto &E : R.Entries) if (E.Level == LogLevel::ERR) M += "\n  " + E.Context + ": " + E.Message;
    return M.empty() ? " (VidyaGod -v … shows the log)" : M;
}

CliModes::LaunchRequest Request(Command::Context &C, const json &A)
{
    CliModes::LaunchRequest R;
    const std::string Q = Command::Str(A, "node", "");
    if (!Q.empty()) R.LaunchNodeId = NodeOf(*Index(C), Q)->Key();
    R.LaunchTile = Command::Str(A, "tile", "");
    R.LaunchVariant = Command::Str(A, "variant", "");
    //The game, named as a person names it: a tile UID or a game's title (its shelf's default row, or --variant's), else
    //a node (a LABEL or a CID).
    if (const std::string G = Command::Str(A, "game", ""); !G.empty())
    {
        if (!Q.empty() || !R.LaunchTile.empty()) throw Command::BadArgs("name the game once: as <game>, or with --node / --tile");
        const auto I = Index(C);
        std::string Tile;
        if (I->Tile(G)) Tile = G;
        else
        {
            std::vector<std::string> Hits;
            for (const auto &[Uid, T] : I->Tiles)
                if (T.is_object() && T.value("TITLE", std::string()) == G && std::find(Hits.begin(), Hits.end(), Uid) == Hits.end()) Hits.push_back(Uid);
            if (Hits.size() > 1)
                throw Command::BadArgs("'" + G + "' is the title of " + std::to_string(Hits.size()) + " games — name one by its UID: " + [&] {
                    std::string L; for (const auto &H : Hits) L += (L.empty() ? "" : ", ") + H; return L; }());
            if (Hits.size() == 1) Tile = Hits.front();
        }
        if (!Tile.empty()) R.LaunchTile = Tile;
        else R.LaunchNodeId = NodeOf(*I, G)->Key();
    }
    if (R.LaunchNodeId.empty() && R.LaunchTile.empty()) throw Command::BadArgs("name the game (a UID, title, LABEL or CID), or give --node or --tile");
    R.Entrypoint = Command::Str(A, "entry", "");
    R.InstanceName = Command::Str(A, "instance", "");
    R.RunnerChain = List(A, "runners");
    if (A.contains("grafts")) R.Grafts = List(A, "grafts");
    for (const auto &KV : List(A, "var"))
    {
        const auto Eq = KV.find('=');
        if (Eq == std::string::npos) throw Command::BadArgs("--var takes NAME=VALUE pairs");
        R.VariableOverrides[KV.substr(0, Eq)] = KV.substr(Eq + 1);
    }
    return R;
}

//"PackageUID" -> "package_uid", "SubComponentsArray" -> "sub_components_array": the command's keys are snake_case
//like every other command's (the --resolve-only file keeps the dump's own spelling).
std::string Snake(const std::string &K)
{
    std::string Out;
    for (size_t I = 0; I < K.size(); ++I)
    {
        const char C = K[I];
        const bool Up = std::isupper(static_cast<unsigned char>(C));
        if (Up && I > 0 && (std::islower(static_cast<unsigned char>(K[I - 1])) || std::isdigit(static_cast<unsigned char>(K[I - 1]))
                            || (I + 1 < K.size() && std::islower(static_cast<unsigned char>(K[I + 1])) && std::isupper(static_cast<unsigned char>(K[I - 1])))))
            Out += '_';
        Out += static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
    }
    return Out;
}

//The plan with the dump's own field names in snake_case — its top level and each runner link. What the fields hold is
//left as it is: environment variables, option and path variables, guest roots and drives are names of their own, and
//the layers and entries are in the node's notation.
json SnakePlan(const json &Dump)
{
    if (!Dump.is_object()) return Dump;
    json Out = json::object();
    for (const auto &[K, V] : Dump.items())
    {
        if (K == "RunnerChain" && V.is_array())
        {
            json Links = json::array();
            for (const auto &L : V)
            {
                if (!L.is_object()) { Links.push_back(L); continue; }
                json S = json::object();
                for (const auto &[Lk, Lv] : L.items()) S[Snake(Lk)] = Lv;
                Links.push_back(std::move(S));
            }
            Out[Snake(K)] = std::move(Links);
        }
        else Out[Snake(K)] = V;
    }
    return Out;
}

std::vector<Command::Arg> LaunchArgs()
{
    return { Command::OptPos("game", "the game: a tile UID, a title, a LABEL or a CID"),
             Command::Opt("node", "the node to launch (CID, handle or LABEL)"), Command::Opt("tile", "a tile: its row (with --variant), as the shelf shows it"),
             Command::Opt("variant", "(--tile) the row's name"), Command::Opt("entry", "which entry (its LABEL; default the first game entry)"),
             Command::Opt("instance", "which instance (default the last played)"), Command::Opt("runners", "a pinned runner chain, innermost first (a,b)"),
             Command::Opt("grafts", "the grafts to apply, in order (a,b; '' none)"), Command::Opt("var", "option values: NAME=VALUE[,…]") };
}

std::vector<Command::Arg> With(std::vector<Command::Arg> V, std::vector<Command::Arg> More)
{
    for (auto &M : More) V.push_back(std::move(M));
    return V;
}

std::unique_ptr<CliModes::PreparedLaunch> &Built()
{
    static std::unique_ptr<CliModes::PreparedLaunch> P;   // a runtime `launch build` keeps up (the app only)
    return P;
}

std::string Slurp(const std::string &P)
{
    std::ifstream In(P, std::ios::binary);
    if (!In) throw Command::BadArgs("cannot read '" + P + "'");
    std::stringstream S;
    S << In.rdbuf();
    return S.str();
}

RegistryWrapper LoadPrefix(const std::string &P)
{
    RegistryWrapper R;
    if (!R.LoadPrefix(P)) throw Command::BadArgs("no wine registry (system.reg / user.reg) under '" + P + "'");
    return R;
}

json Value(const RegistryValue &V) { return RegistryWrapper::ValueToManifestString(V); }

} // namespace

namespace {
//A RegEdit that did not apply says why as a warning (the launch goes on without it); a command applying one fails
//with that reason.
std::string RegEditWhy(LogRecorder &Said)
{
    std::string Out;
    for (const auto &E : Said.Take()) Out += "\n  " + E.Message;
    return Out;
}
} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("launch", "the launch engine: resolve a node into its launch, run it, or build its runtime to look inside");
    Command::RegisterGroup("registry", "a wine prefix's registry: read keys and values, set, delete, diff, apply a node's REG edits");
    Command::RegisterGroup("edits", "the file edits and binary patches a launch applies, on any folder or file");

    // ---------------- launch ----------------
    Command::Register({
        "launch plan", "resolve a node into the launch it would be (nothing mounted, nothing run)",
        "The whole resolved launch: the package, platform, the runner chain (each link, ending in this machine), the "
        "runner's program, arguments and environment, every path (content root, prefix, the program), DLL overrides, "
        "what is kept, the options and every variable, the layers it would mount — what `--resolve-only` writes. The "
        "warnings and errors the resolve logged are counted (app diagnostics).",
        LaunchArgs(), { "launch plan 749", "launch plan 'Patch 2.0a'", "launch plan --node 'Patch 2.0a'", "launch plan --tile 749 --variant 'Patch 2.0a (No-CD)' --var aoe2_res=1920x1080 --json" },
        { "CliModes::Launch", "CliModes::PrepareLaunch", "CliModes::ResolutionOf", "ContainerWrapper::Resolved", "ContainerParams::GetVariablesMap",
          "LaunchResolver::InitializeFromNode", "LaunchResolver::ResolveRunnerChain", "LaunchResolver::ResolveChainIds", "LaunchResolver::ResolveChainTail",
          "LaunchResolver::BoundaryLinkIndex", "LaunchResolver::ChainHasInnerLinks", "LaunchResolver::DerivePaths", "LaunchResolver::DerivePersistence",
          "LaunchResolver::ResolveCustomVariables", "LaunchResolver::ResolveExecutableDefinition", "LaunchResolver::BuildSubComponentsArray",
          "LaunchResolver::AnchorLayouts", "LaunchResolver::AnchorRegEdits", "LaunchResolver::ComposeGuestTarget", "LaunchResolver::GuestPath",
          "LaunchResolver::GuestToLayout", "LaunchResolver::RunnerOps", "LaunchResolver::SessionRoot", "LaunchResolver::InnerRunnerMountRel",
          "RunnerLink::NativeNamespace", "RunnerLink::Passthrough", "BuiltinRunners::All", "BuiltinRunners::AddTo", "BuiltinRunners::IsBuiltin",
          "RunnerWrapper::DefPrefixDir", "RunnerWrapper::ExecutableAvailable", "OptionReadBack::Sites", "ArgReferencesContent" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            CliModes::LaunchRequest R = Request(C, A);
            R.ResolveOnly = true;
            json Dump;
            Diagnostics::Report V;
            if (CliModes::Launch(R, *C.Config, *C.AppDataDir, &Dump, &V) != 0) throw std::runtime_error("it did not resolve:" + LoggedErrors(V));
            return SnakePlan(Dump);
        }, {} });

    Command::Register({
        "launch run", "launch a node: build its runtime, run it to its exit, take it down",
        "A real launch, as Play does: the runtime is built (sources fetched, layers mounted, the prefix made, edits and "
        "registry applied, what is kept restored), the game runs in the sandbox until it exits, then what it keeps is "
        "captured and the runtime comes down. Blocks until the game exits; prints its exit code. On the machine's own "
        "display and speakers — off-screen testing is the caller's to arrange.",
        With(LaunchArgs(), { Flag("yes", "yes, run it") }), { "launch run --node 'Patch 2.0a' --yes" },
        { "ContainerWrapper::BuildContainerRuntime", "ContainerWrapper::Execute", "ContainerWrapper::Cleanup", "LaunchSources::EnsureSources",
          "LaunchSources::MaterializeLayers", "LaunchSources::VerifyDependencies", "VfsMount::MountVFS", "VfsMount::PrepareMount", "VfsMount::BuildLayerSpec",
          "VfsMount::BuildRunnerLayerSpec", "VfsMount::MountRunnerBuild", "VfsMount::RunnerShipsMountableBuild", "VfsMount::SpawnVidyagodfs",
          "VfsMount::MaterializePlanPaths", "VfsMount::ReportMissingSources", "VfsMount::CheckCaseConflicts", "VfsMount::ClaimSession",
          "VfsMount::ReleaseSession", "VfsMount::UnmountDurable", "VfsMount::UnmountLazy", "PersistLayer::SeedPersistFiles", "PersistLayer::CapturePersistFiles",
          "PersistLayer::PrepareWriteDir", "PersistLayer::PrepareWritePath", "PersistLayer::HasSavedCopy", "PersistLayer::SavedCopy", "PersistLayer::Printable",
          "RegistryLayer::BuildDefaultData", "RegistryLayer::ApplyOverrideRegEdits", "RegistryLayer::CapturePersistRegKeys", "FileEdits::ProcessFileEdits",
          "FileEdits::ProcessDLLOverrides", "FileEdits::KeptPathMet", "BinaryPatch::ProcessBinaryPatches", "OptionReadBack::Read", "OptionReadBack::Current",
          "OptionReadBack::Adopt", "OptionReadBack::Written", "OptionReadBack::Commit", "SandboxLayer::Wrap", "SandboxLayer::WrapGame", "SandboxLayer::FromParams",
          "SandboxLayer::HomeEnv", "SandboxLayer::HomeLeftovers", "SandboxLayer::TakeLoaderEnv", "SandboxLayer::PrepareBox", "SandboxLayer::BoxIniCommands",
          "SandboxLayer::BoxStartArgs", "SandboxLayer::RunInBox", "SandboxLayer::TerminateBox", "SandboxLayer::EmptyBox", "BoxLaunch::Prepare",
          "BoxLaunch::Harvest", "BoxLaunch::Discard", "BoxPrefix::Materialise", "BoxPrefix::SyncKeepsBack", "BoxPrefix::InBox", "BoxPrefix::BoxRelForReal",
          "BoxPrefix::RealForLayout", "BoxPrefix::KeptNotInBox", "Platform::IsLink", "Platform::RemoveTree", "LaunchResolver::ProbePrefixLayout",
          "LaunchResolver::ApplyInnerEnv", "RunnerInstall::RunnerBuildPresent", "RunnerInstall::RunnerNodeImported", "RunnerInstall::CollectRunnerNodeTargets" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this runs the game: add --yes");
            Diagnostics::Report V;
            const int Rc = CliModes::Launch(Request(C, A), *C.Config, *C.AppDataDir, nullptr, &V);
            json Errors = json::array();
            for (const auto &E : V.Entries) if (E.Level == LogLevel::ERR) Errors.push_back(E.Context + ": " + E.Message);
            return json{{"exit_code", Rc}, {"warnings", V.Warnings}, {"errors", Errors}};
        }, {} });

    Command::Register({
        "launch build", "build a node's runtime and keep it up (to look inside with vfs / ls), until `launch teardown`",
        "Resolves and builds the runtime as a launch does — mounted, prefix made, edits applied — without running the "
        "game; prints where it is. In the app only (a mount outlives no process): `launch teardown` captures what is "
        "kept and takes it down.", LaunchArgs(), { "launch build --node 'Patch 2.0a'" },
        { "ContainerWrapper::BuildContainerRuntime" }, Needs::App, true,
        [](Command::Context &C, const json &A) {
            if (!C.Live) throw std::runtime_error("a built runtime lives in the app: start VidyaGod on this data dir (off-screen is fine), then run this");
            if (Built()) throw std::runtime_error("a runtime is up (" + Built()->Name + "): VidyaGod launch teardown");
            std::string Err;
            auto P = CliModes::PrepareLaunch(Request(C, A), *C.Config, Err);
            if (!P) throw std::runtime_error(Err);
            if (!P->W->BuildContainerRuntime()) { P->W->Cleanup(); throw std::runtime_error("the runtime did not build"); }
            json Out = CliModes::ResolutionOf(*P);
            Out = {{"runtime", P->W->ContainerParams.RuntimePath.string()}, {"content", Out["Content"]}, {"prefix", Out["PrefixRoot"]}};
            Built() = std::move(P);
            return Out;
        }, {} });

    Command::Register({
        "launch teardown", "take down the runtime `launch build` kept up (what it keeps is captured first)", "",
        {}, { "launch teardown" }, {}, Needs::App, false,
        [](Command::Context &, const json &) {
            if (!Built()) return json{{"torn_down", false}, {"why", "no runtime is up"}};
            Built()->W->Cleanup();
            Built().reset();
            return json{{"torn_down", true}};
        }, {} });

    Command::Register({
        "launch running", "what is running: live games and runtimes, and the mounts left under the data dir",
        "Games running now, whether a stop is under way, runtimes still mounted, and every mount under the app's data "
        "folders (live, or left by a session that died) with the session that owns it. --sweep takes down what dead "
        "sessions left and removes stale runtimes.", { Flag("sweep", "clean up after dead sessions") }, { "launch running", "launch running --sweep" },
        { "ContainerWrapper::LiveCount", "ContainerWrapper::Stopping", "VfsMount::RuntimesLive", "VfsMount::MountpointsUnder", "VfsMount::LiveMountpointsUnder",
          "VfsMount::SessionOwned", "VfsMount::SweepDeadSessions", "VfsMount::CleanStaleRuntime" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            //Sessions live under the session root, never the data dir: a sweep of the data dir would take each package
            //folder and USERDATA entry for a dead session and remove it. Mounts are listed under both.
            const fs::path Sessions = LaunchResolver::SessionRoot(), Data = C.AppDataDir->absolutePath().toStdString();
            if (Command::Bool(A, "sweep", false)) VfsMount::SweepDeadSessions(Sessions);
            json Mounts = json::array(), Live = json::array();
            for (const fs::path &Root : { Sessions, Data })
            {
                for (const auto &M : VfsMount::MountpointsUnder(Root)) Mounts.push_back({{"mount", M}, {"owned", VfsMount::SessionOwned(M)}});
                for (const auto &M : VfsMount::LiveMountpointsUnder(Root)) Live.push_back(M);
            }
            return json{{"games", ContainerWrapper::LiveCount()}, {"stopping", ContainerWrapper::Stopping()}, {"runtimes_live", VfsMount::RuntimesLive()},
                        {"mounts", Mounts}, {"live_mounts", Live}};
        }, {} });

    Command::Register({
        "launch stop", "stop every running game (what each keeps is still captured)",
        "What quitting the app does to its games: each is told to stop; waits until none is live.",
        {}, { "launch stop" },
        { "ContainerWrapper::StopAll", "ContainerWrapper::KillGame", "ContainerWrapper::WaitNoneLive", "LaunchThread::kill" }, Needs::Config, false,
        [](Command::Context &, const json &) {
            ContainerWrapper::StopAll();
            ContainerWrapper::WaitNoneLive();
            return json{{"games", ContainerWrapper::LiveCount()}};
        }, {} });

    Command::Register({
        "launch audit", "resolve every launchable and report what is wrong (the --audit-packages report)",
        "Every launchable of the library (or --scope: a package's UID or folder) resolved in this process, and the "
        "warnings, errors and silently inert authoring each produced; non-zero exit when anything was found.",
        { Opt("scope", "a package (UID or folder name)") }, { "launch audit", "launch audit --scope 749" }, { "CliModes::RunAuditPackages" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const int Rc = CliModes::RunAuditPackages(*C.Config, Command::Str(A, "scope", ""));
            if (Rc != 0) throw std::runtime_error("the audit found problems (listed above)");
            return json{{"clean", true}};
        }, {} });

    Command::Register({
        "launch mirror", "every content file and runner build in the catalog, as fetch targets (and fetch them)",
        "What a full mirror fetches: every node's content closure and every runner's build — counted, and with --fetch "
        "fetched through the queue. Nodes naming content no one serves are listed, never fatal.", { Flag("fetch", "fetch them all") },
        { "launch mirror", "launch mirror --fetch" }, { "CliModes::CollectCatalogTargets", "IpfsWrapper::FetchTargetsConcurrent" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            std::vector<IpfsWrapper::FetchTarget> T;
            CliModes::CatalogTargetStats S;
            json Sourceless = json::array();
            CliModes::CollectCatalogTargets(*Index(C), T, &S, [&](const std::string &N, const std::string &E) { Sourceless.push_back({{"node", N}, {"why", E}}); });
            json Out = {{"targets", T.size()}, {"nodes", S.Nodes}, {"launchables", S.Launchables}, {"runners", S.Runners}, {"sourceless", Sourceless}};
            if (Command::Bool(A, "fetch", false))
            {
                std::string Err;
                if (!IpfsWrapper::FetchTargetsConcurrent(T, &Err)) throw std::runtime_error(Err);
                Out["fetched"] = true;
            }
            return Out;
        }, {} });

    // ---------------- registry ----------------
    Command::Register({
        "registry show", "a wine prefix's registry: a key's values and subkeys, or every key under one",
        "Reads the prefix's hives (system.reg, user.reg, userdef.reg) and shows --key: its values (in the node's "
        "notation) and subkeys (a hive's root, HKLM or HKCU: its top-level keys); --all every key under it. --arch 32 shows the key as a 32-bit program sees it.",
        { Pos("prefix", "the prefix folder (holding system.reg)"), Opt("key", "the key (HKLM\\Software\\…)"), Opt("arch", "32 or 64"), Flag("all", "every key under it") },
        { "registry show ~/x/pfx --key HKLM", "registry show ~/x/pfx --key 'HKLM\\Software\\Microsoft\\Microsoft Games'", "registry show ~/x/pfx --key HKCU --all" },
        { "RegistryWrapper::LoadPrefix", "RegistryWrapper::LoadHiveFile", "RegistryWrapper::GetKey", "RegistryWrapper::GetValue", "RegistryWrapper::ForEachKey",
          "RegistryWrapper::ArchPath", "RegistryWrapper::NormalizeRootPrefix", "RegistryWrapper::RootForPath", "RegistryWrapper::StripRootPrefix",
          "RegistryWrapper::NamesNoKey", "RegistryWrapper::ValueToManifestString" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            RegistryWrapper R = LoadPrefix(Command::Str(A, "prefix"));
            std::string K = RegistryWrapper::NormalizeRootPrefix(Command::Str(A, "key", "HKLM"));
            if (A.contains("arch")) K = RegistryWrapper::ArchPath(K, Command::Str(A, "arch"));
            static const char *HiveNames[] = { "machine (system.reg)", "user (user.reg)", "default user (userdef.reg)" };
            const bool Root = RegistryWrapper::NamesNoKey(K);   // a whole hive: HKLM, HKCU
            while (Root && !K.empty() && K.back() == '\\') K.pop_back();
            json Out = {{"key", K}, {"hive", HiveNames[static_cast<int>(RegistryWrapper::RootForPath(K))]}};
            if (!Root) Out["in_hive"] = RegistryWrapper::StripRootPrefix(K);
            if (Root && !Command::Bool(A, "all", false))
            {
                //A hive's top: the keys directly under it (a hive has no values of its own).
                json Sub = json::array();
                std::set<std::string> Seen;
                const HiveRoot Want = RegistryWrapper::RootForPath(K);
                R.ForEachKey([&](const std::string &P, const RegistryKey &) {
                    if (RegistryWrapper::RootForPath(P) != Want) return;
                    const std::string In = RegistryWrapper::StripRootPrefix(P);
                    const std::string Top = In.substr(0, In.find('\\'));
                    if (!Top.empty() && Seen.insert(QString::fromStdString(Top).toLower().toStdString()).second) Sub.push_back(Top);
                });
                Out["values"] = json::object();
                Out["subkeys"] = Sub;
                return Out;
            }
            if (Command::Bool(A, "all", false))
            {
                json Keys = json::object();
                R.ForEachKey([&](const std::string &P, const RegistryKey &Key) {
                    if (P.rfind(K, 0) != 0) return;
                    json V = json::object();
                    for (const auto &[N, X] : Key.Values) V[N.empty() ? "@" : N] = Value(X);
                    Keys[P] = V;
                });
                Out["keys"] = Keys;
                return Out;
            }
            const RegistryKey *Key = R.GetKey(K);
            if (!Key) throw Command::BadArgs("no key '" + K + "'");
            json V = json::object(), Sub = json::array();
            for (const auto &[N, X] : Key->Values) V[N.empty() ? "@" : N] = Value(X);
            for (const auto &Ch : Key->Children) Sub.push_back(Ch->Name);
            Out["values"] = V;
            Out["subkeys"] = Sub;
            return Out;
        }, {} });

    Command::Register({
        "registry set", "set or delete a value or key in a prefix's registry (saved back to its hives)",
        "--value NAME --data VALUE (the node's notation: \"text\", dword:…, hex:…) sets a value (the key is made); "
        "--delete-value NAME, --delete-key remove; --copy-from PREFIX copies the key (and below) from another prefix.",
        { Pos("prefix", "the prefix folder"), Pos("key", "the key"), Opt("value", "a value's name ('' = the default)"), Opt("data", "(--value) its data"),
          Opt("delete-value", "a value to delete"), Flag("delete-key", "delete the key"), Opt("copy-from", "a prefix to copy the key from") },
        { "registry set ~/x/pfx 'HKCU\\Software\\X' --value Res --data '\"1920x1080\"'", "registry set ~/x/pfx 'HKCU\\Software\\X' --delete-key" },
        { "RegistryWrapper::SetValue", "RegistryWrapper::EnsureKey", "RegistryWrapper::DeleteValue", "RegistryWrapper::DeleteKey", "RegistryWrapper::MergeKeyFrom",
          "RegistryWrapper::SavePrefix", "RegistryWrapper::SaveHiveFile", "RegistryWrapper::ManifestStringToValue", "RegistryWrapper::SzValue" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string P = Command::Str(A, "prefix"), K = RegistryWrapper::NormalizeRootPrefix(Command::Str(A, "key"));
            RegistryWrapper R = LoadPrefix(P);
            bool Ok = true;
            if (A.contains("value"))
            {
                const std::string D = Command::Str(A, "data", "");
                const json J = json::parse(D, nullptr, false);
                R.EnsureKey(K);
                R.SetValue(K, Command::Str(A, "value"), J.is_discarded() ? RegistryWrapper::SzValue(D) : RegistryWrapper::ManifestStringToValue(J));
            }
            if (A.contains("delete-value")) Ok = R.DeleteValue(K, Command::Str(A, "delete-value")) && Ok;
            if (Command::Bool(A, "delete-key", false)) Ok = R.DeleteKey(K) && Ok;
            if (A.contains("copy-from")) Ok = R.MergeKeyFrom(LoadPrefix(Command::Str(A, "copy-from")), K) && Ok;
            if (!Ok) throw std::runtime_error("nothing there to change");
            if (!R.SavePrefix(P)) throw std::runtime_error("could not save the hives");
            return json{{"saved", P}};
        }, {} });

    Command::Register({
        "registry diff", "what changed between two prefixes' registries, as a node's REG edits",
        "The edits that take --from to the prefix: the capture an authoring session makes.", { Pos("prefix", "the changed prefix"), Opt("from", "the baseline prefix", Type::String, true) },
        { "registry diff ~/after/pfx --from ~/before/pfx" }, { "RegistryWrapper::DiffToRegEdits" }, Needs::Config, false,
        [](Command::Context &, const json &A) { return LoadPrefix(Command::Str(A, "prefix")).DiffToRegEdits(LoadPrefix(Command::Str(A, "from"))); }, {} });

    Command::Register({
        "registry apply", "apply REG edits to a prefix (as a launch does), with %variables% substituted",
        "--edits: the engine's RegEdit list (or one); --var NAME=VALUE substitutes first; --override applies only the "
        "edits marked OVERRIDE (what is re-applied over the user's kept registry). Saved back to the hives.",
        { Pos("prefix", "the prefix folder"), WithSample(Opt("edits", "the RegEdit layer(s) (JSON)", Type::Json, true), "{\"REGPATH\":\"HKCU\\\\Software\\\\X\",\"KEYVALUES\":{\"V\":\"1\"}} or a list of them"), Opt("var", "NAME=VALUE[,…]"), Flag("override", "only OVERRIDE edits"),
          Flag("dry-run", "show the substituted edits, change nothing") },
        { "registry apply ~/x/pfx --edits @reg.json --var res=1920x1080" },
        { "RegistryWrapper::ApplyRegEdits", "RegistryWrapper::ApplyRegEdit", "RegistryWrapper::SubstituteRegEdit" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::map<std::string, std::string> Vars;
            for (const auto &KV : List(A, "var")) { const auto Eq = KV.find('='); if (Eq != std::string::npos) Vars[KV.substr(0, Eq)] = KV.substr(Eq + 1); }
            json E = A["edits"].is_array() ? A["edits"] : json::array({A["edits"]});
            for (auto &X : E) X = RegistryWrapper::SubstituteRegEdit(X, Vars);
            if (Command::Bool(A, "dry-run", false)) return E;
            const std::string P = Command::Str(A, "prefix");
            RegistryWrapper R = LoadPrefix(P);
            LogRecorder Said({ "RegistryWrapper::ApplyRegEdit", "RegistryWrapper::ApplyRegEdits" });
            const bool Ok = E.size() == 1 && !Command::Bool(A, "override", false) ? R.ApplyRegEdit(E[0]) : R.ApplyRegEdits(E, Command::Bool(A, "override", false));
            if (!Ok) throw std::runtime_error("an edit did not apply" + RegEditWhy(Said));
            if (!R.SavePrefix(P)) throw std::runtime_error("could not save the hives");
            return json{{"applied", E.size()}};
        }, {} });

    Command::Register({
        "registry regfile", "a .reg export to and from a prefix's keys (what the Windows sandbox box keeps), and its text codec",
        "--to FILE writes the prefix's registry as a regedit export (what the box keeps; counted: what the format cannot "
        "carry); --from FILE reads one (UTF-8 text) into the prefix. --decode FILE prints a UTF-16 LE file as text; --encode TEXT --out FILE writes text as UTF-16 LE.",
        { Opt("prefix", "the prefix folder"), Opt("to", "write a .reg here"), Opt("from", "read this .reg"), Opt("decode", "a UTF-16 LE file"),
          Opt("encode", "text to encode"), Opt("out", "(--encode) the file") },
        { "registry regfile --prefix ~/x/pfx --to x.reg", "registry regfile --decode x.reg" },
        { "BoxPrefix::ToRegFile", "BoxPrefix::FromRegFile", "BoxPrefix::Utf16LeToUtf8", "BoxPrefix::Utf8ToUtf16Le" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (A.contains("decode")) return json{{"text", BoxPrefix::Utf16LeToUtf8(Slurp(Command::Str(A, "decode")))}};
            if (A.contains("encode")) { std::ofstream(Command::Str(A, "out"), std::ios::binary) << BoxPrefix::Utf8ToUtf16Le(Command::Str(A, "encode")); return json{{"out", Command::Str(A, "out")}}; }
            const std::string P = Command::Str(A, "prefix", "");
            RegistryWrapper R = LoadPrefix(P);
            if (A.contains("to"))
            {
                size_t Skipped = 0;
                std::ofstream(Command::Str(A, "to"), std::ios::binary) << BoxPrefix::ToRegFile(R, &Skipped);
                return json{{"written", Command::Str(A, "to")}, {"left_out", Skipped}};
            }
            if (A.contains("from"))
            {
                std::string Err;
                if (!BoxPrefix::FromRegFile(Slurp(Command::Str(A, "from")), R, Err)) throw std::runtime_error(Err);
                if (!R.SavePrefix(P)) throw std::runtime_error("could not save the hives");
                return json{{"imported", Command::Str(A, "from")}};
            }
            throw Command::BadArgs("give --to, --from, --decode or --encode");
        }, {} });

    // ---------------- edits ----------------
    Command::Register({
        "edits apply", "apply one file edit to a file, as a launch does: a setting, a JSON or XML value, a line, the whole file, a deletion",
        "--config START replaces the line that starts with START (\"Width=\") by START + --value; --json-set POINTER sets "
        "a JSON value; --xml PATH an XML element or attribute (to --value); --append LINE adds a line once; --overwrite "
        "TEXT replaces the file; --delete removes it. --base checks the file stays inside that folder (a link leading out is refused).",
        { Pos("file", "the file"), Opt("config", "the line's start (Width=)"), Opt("json-set", "a JSON pointer"), Opt("xml", "an XPath (/Config/Video/@w)"),
          Opt("value", "(--config, --json-set, --xml) the value"), Opt("append", "a line"),
          Opt("overwrite", "the new text"), Flag("delete", "delete it"), Opt("base", "the folder it must stay in") },
        { "edits apply game.ini --config 'Width=' --value 1920", "edits apply settings.json --json-set /video/width --value 1920",
          "edits apply cfg.xml --xml '/Config/Video/@w' --value 1920" },
        { "FileEdits::ConfigWrite", "FileEdits::JsonSet", "FileEdits::XmlSet", "FileEdits::XmlEdit", "FileEdits::AppendLine", "FileEdits::FileOverwrite",
          "FileEdits::DeletePath", "FileEdits::PathEscapesBase", "FileEdits::LinkLeadsOut" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const fs::path F = Command::Str(A, "file");
            if (A.contains("base"))
            {
                fs::path N;
                std::string Why;
                if (FileEdits::PathEscapesBase(F, Command::Str(A, "base"), N)) throw std::runtime_error("it leaves the folder");
                if (FileEdits::LinkLeadsOut(Command::Str(A, "base"), N, Why)) throw std::runtime_error(Why);
            }
            const auto KV = [&](const char *K) { return std::make_pair(Command::Str(A, K), Command::Str(A, "value")); };
            bool Ok = true;
            if (A.contains("config")) { const auto [K, V] = KV("config"); Ok = FileEdits::ConfigWrite(K, V, F); }
            else if (A.contains("json-set")) { const auto [K, V] = KV("json-set"); Ok = FileEdits::JsonSet(K, V, F); }
            else if (A.contains("xml"))
            {
                const auto [K, V] = KV("xml");
                std::string Out, Why;
                if (!FileEdits::XmlEdit(K, V, Slurp(F.string()), Out, Why)) throw std::runtime_error(Why);
                Ok = FileEdits::XmlSet(K, V, F);
            }
            else if (A.contains("append")) Ok = FileEdits::AppendLine(Command::Str(A, "append"), F);
            else if (A.contains("overwrite")) Ok = FileEdits::FileOverwrite(Command::Str(A, "overwrite"), F);
            else if (Command::Bool(A, "delete", false)) Ok = FileEdits::DeletePath(F);
            else throw Command::BadArgs("give --config, --json-set, --xml, --append, --overwrite or --delete");
            if (!Ok) throw std::runtime_error("the edit did not apply");
            return json{{"edited", F.string()}};
        }, {} });

    Command::Register({
        "edits xml-get", "read an XML element or attribute (how an option reads its value back)", "",
        { Pos("file", "the XML file"), Pos("path", "an XPath (/Config/Video/@w)") }, { "edits xml-get cfg.xml '/Config/Video/@w'" },
        { "FileEdits::XmlGet" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::string Why;
            const auto V = FileEdits::XmlGet(Command::Str(A, "path"), Slurp(Command::Str(A, "file")), Why);
            if (!V) throw std::runtime_error(Why);
            return json{{"value", *V}};
        }, {} });

    Command::Register({
        "edits patch", "apply a BinaryPatch (Replace, Cave, Poke) to a program file",
        "The patch as the node writes it (--patch JSON), --var values for its tokens; the file is changed in place "
        "unless --dry-run (then only what it would do is said).",
        { Pos("file", "the program file"), WithSample(Opt("patch", "the patch (JSON)", Type::Json, true), "{\"MODE\":\"Replace\",\"OFFSET\":\"0x1234\",\"EXPECT\":\"74 05\",\"REPLACE\":\"EB 05\"}"), Opt("var", "NAME=VALUE[,…]"), Flag("dry-run", "change nothing") },
        { "edits patch game.exe --patch @nocd.json --dry-run" }, { "BinaryPatch::ApplyOne" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string F = Command::Str(A, "file");
            const std::string Bytes = Slurp(F);
            std::vector<uint8_t> Img(Bytes.begin(), Bytes.end());
            std::map<std::string, std::string> Vars;
            for (const auto &KV : List(A, "var")) { const auto Eq = KV.find('='); if (Eq != std::string::npos) Vars[KV.substr(0, Eq)] = KV.substr(Eq + 1); }
            std::string Msg;
            const auto R = BinaryPatch::ApplyOne(A["patch"], Img, Vars, Msg);
            static const char *Names[] = { "applied", "skipped", "error" };
            if (R == BinaryPatch::Result::Applied && !Command::Bool(A, "dry-run", false))
                std::ofstream(F, std::ios::binary).write(reinterpret_cast<const char *>(Img.data()), static_cast<std::streamsize>(Img.size()));
            if (R == BinaryPatch::Result::Error) throw std::runtime_error(Msg);
            return json{{"result", Names[static_cast<int>(R)]}, {"note", Msg}};
        }, {} });
}
