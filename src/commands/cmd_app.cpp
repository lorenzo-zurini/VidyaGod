// `VidyaGod app …`, `instances …`, `deps …` — the app's own footing: its data root, run mode and path overrides, this
// machine's platform facts, the diagnostics tally, a game's instances (its USERDATA folders), and what the host has of
// what a launch needs.
#include "command.h"
#include "appmodel.h"
#include "apppaths.h"
#include "commonutils.h"
#include "depcheck.h"
#include "filesystemoperations.h"
#include "instancestore.h"
#include "platform/platform.h"
#include "sandboxlayer.h"

#include <QDir>
#include <fstream>
#include <sstream>

namespace
{
using json = nlohmann::ordered_json;

const char *ModeName(AppPaths::Mode M)
{
    switch (M)
    {
    case AppPaths::Mode::Normal: return "normal";
    case AppPaths::Mode::Portable: return "portable";
    case AppPaths::Mode::InPackage: return "in-package";
    case AppPaths::Mode::CliPaths: return "cli-paths";
    }
    return "?";
}

const char *StatusName(DepCheck::Status S)
{
    switch (S)
    {
    case DepCheck::Status::Ok: return "ok";
    case DepCheck::Status::Partial: return "partial";
    case DepCheck::Status::Missing: return "missing";
    }
    return "?";
}

json ReportJson(const Diagnostics::Report &R)
{
    json E = json::array();
    for (const auto &X : R.Entries) E.push_back({{"level", static_cast<int>(X.Level)}, {"context", X.Context}, {"message", X.Message}});
    return {{"warnings", R.Warnings}, {"errors", R.Errors}, {"clean", R.Empty()}, {"entries", E}};
}

Command::Arg UidArg() { return Command::Pos("uid", "the game's family UID (its package's)"); }

void Check(bool Ok, const std::string &Err)
{
    if (!Ok) throw std::runtime_error(Err);
}

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("app", "the app's footing: data root, run mode, path overrides, platform facts, diagnostics");
    Command::RegisterGroup("instances", "a game's instances: its USERDATA folders, their configs, which is active");
    Command::RegisterGroup("deps", "what this machine has of what a launch needs, and what to install");

    // ---------------- app ----------------
    Command::Register({
        "app paths", "the data root, the run mode, and the per-launch path overrides",
        "Where this run keeps its data, how it was started (normal, portable, in-package, cli-paths — which decides "
        "whether it may stay resident or start on login), the USERDATA root, and the overrides a launch honours: set "
        "for one call with the global --package-dir, --runtime-dir and --userdata-dir.", {}, { "app paths", "app paths --userdata-dir /tmp/u" },
        { "AppPaths::DataRoot", "AppPaths::DataRootSet", "AppPaths::SetDataRoot", "AppPaths::GetMode", "AppPaths::SetMode", "AppPaths::DaemonSupported",
          "AppPaths::AutostartSupported", "AppPaths::PackagePathOverride", "AppPaths::RuntimePathOverride", "AppPaths::UserDataPathOverride",
          "AppPaths::SetPackagePathOverride", "AppPaths::SetRuntimePathOverride", "AppPaths::SetUserDataPathOverride", "InstanceStore::Root",
          "AppModel::appDataDir", "AppModel::config" },
        Needs::Config, false,
        [](Command::Context &C, const json &) {
            return json{{"data_root", AppPaths::DataRoot().string()}, {"data_root_set", AppPaths::DataRootSet()},
                        {"mode", ModeName(AppPaths::GetMode())},
                        {"may_stay_resident", AppPaths::DaemonSupported()}, {"may_start_on_login", AppPaths::AutostartSupported()},
                        {"userdata_root", InstanceStore::Root(*C.Config).string()},
                        {"package_dir", AppPaths::PackagePathOverride().string()}, {"runtime_dir", AppPaths::RuntimePathOverride().string()},
                        {"userdata_dir", AppPaths::UserDataPathOverride().string()},
                        // With the app up: the data dir and config it runs on (the ones this call reached).
                        {"app_data_dir", C.Model ? C.Model->appDataDir()->absolutePath().toStdString() : std::string()},
                        {"app_config_is_this", C.Model && C.Model->config() == C.Config}};
        }, {} });

    Command::Register({
        "app platform", "this machine: the executable, its system folders, a free drive letter, a mount's target",
        "The running executable's path; the Windows profile and system folders a native game reads its anchors as "
        "(empty off Windows) and every name they may have; a drive letter no volume holds (Windows); and with --path, "
        "what the mount-point junction there names.", { Opt("path", "a folder: what mount it is") }, { "app platform" },
        { "Platform::SelfExe", "Platform::HostFolders", "Platform::HostFolderNames", "Platform::FreeDriveLetter", "Platform::MountPointTarget" },
        Needs::Config, false,
        [](Command::Context &, const json &A) {
            json Out = {{"executable", Platform::SelfExe().string()}, {"folders", Platform::HostFolders()},
                        {"folder_names", Platform::HostFolderNames()}, {"free_drive", Platform::FreeDriveLetter()}};
            if (A.contains("path")) Out["mount_target"] = Platform::MountPointTarget(Command::Str(A, "path"));
            return Out;
        }, {} });

    Command::Register({
        "app diagnostics", "the warnings and errors logged since collecting began",
        "The process's diagnostics tally (every launch collects one): whether it is collecting, and what it holds — "
        "deduplicated warnings and errors. --begin starts (or restarts) collecting, --take ends it, --verdict prints "
        "the launch-style verdict for a name.",
        { Flag("begin", "start collecting"), Flag("take", "take the report and stop"), Opt("verdict", "take it and print the verdict for this") },
        { "app diagnostics", "app diagnostics --begin", "app diagnostics --verdict 'the test run'" },
        { "Diagnostics::Begin", "Diagnostics::Take", "Diagnostics::Peek", "Diagnostics::Collecting", "Diagnostics::ReportVerdict",
          "Diagnostics::Report::Empty" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (Command::Bool(A, "begin", false)) { Diagnostics::Begin(); return json{{"collecting", Diagnostics::Collecting()}}; }
            if (A.contains("verdict")) return ReportJson(Diagnostics::ReportVerdict(Command::Str(A, "verdict")));
            if (Command::Bool(A, "take", false)) return ReportJson(Diagnostics::Take());
            json Out = ReportJson(Diagnostics::Peek());
            Out["collecting"] = Diagnostics::Collecting();
            return Out;
        }, {} });

    Command::Register({
        "app is-package", "whether a folder is a package (holds node files)",
        "The check the app makes when a folder is added or the app starts inside one.", { Pos("dir", "the folder") },
        { "app is-package ~/x/'[749][v1.0] Age of Empires II'" }, { "FSOps::CheckPackageValid" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            QDir D(QString::fromStdString(Command::Str(A, "dir")));
            return json{{"package", FSOps::CheckPackageValid(&D)}};
        }, {} });

    // ---------------- instances ----------------
    Command::Register({
        "instances list", "a game's instances, newest-played first, and which is active",
        "Every instance of a game (its USERDATA/<uid>/<instance> folders), newest LASTRUN first; the active one (what a "
        "launch uses: created as DefaultInstance with --create when there is none); and the folders and config paths.",
        { UidArg(), Flag("create", "create DefaultInstance when the game has none") },
        { "instances list 749" },
        { "InstanceStore::List", "InstanceStore::ResolveActive", "InstanceStore::Active", "InstanceStore::PackageDir", "InstanceStore::InstanceDir",
          "InstanceStore::ConfigPath", "InstanceStore::SanitizeUid" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string U = Command::Str(A, "uid");
            const std::string Act = Command::Bool(A, "create", false) ? InstanceStore::Active(*C.Config, U) : InstanceStore::ResolveActive(*C.Config, U);
            json Rows = json::array();
            for (const auto &I : InstanceStore::List(*C.Config, U))
                Rows.push_back({{"instance", I}, {"active", I == Act}, {"dir", InstanceStore::InstanceDir(*C.Config, U, I).string()},
                                {"config", InstanceStore::ConfigPath(*C.Config, U, I).string()}});
            return json{{"uid", InstanceStore::SanitizeUid(U)}, {"dir", InstanceStore::PackageDir(*C.Config, U).string()}, {"active", Act}, {"instances", Rows}};
        }, {} });

    Command::Register({
        "instances new", "create an instance (an empty USERDATA folder)",
        "A new instance of a game: a fresh place for its saves and settings, beside the others.",
        { UidArg(), Pos("name", "the instance's name (one folder name, ≤ 64 characters)") },
        { "instances new 749 'Second campaign'" }, { "InstanceStore::Create", "InstanceStore::ValidName" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string N = Command::Str(A, "name");
            if (!InstanceStore::ValidName(N)) throw Command::BadArgs("'" + N + "' is not a valid instance name");
            std::string Err;
            Check(InstanceStore::Create(*C.Config, Command::Str(A, "uid"), N, &Err), Err);
            return json{{"created", N}};
        }, {} });

    Command::Register({
        "instances rename", "rename an instance", "Its folder moves, saves and all.",
        { UidArg(), Pos("from", "its name"), Pos("to", "the new name") }, { "instances rename 749 DefaultInstance Main" },
        { "InstanceStore::Rename" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::string Err;
            Check(InstanceStore::Rename(*C.Config, Command::Str(A, "uid"), Command::Str(A, "from"), Command::Str(A, "to"), &Err), Err);
            return json{{"renamed", Command::Str(A, "to")}};
        }, {} });

    Command::Register({
        "instances clone", "copy an instance (its saves and settings) under a new name",
        "The copy leaves the instance's sandbox box out (a Windows game's prefix).", { UidArg(), Pos("from", "its name"), Pos("to", "the copy's name") },
        { "instances clone 749 Main Experiment" }, { "InstanceStore::Clone" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::string Err;
            Check(InstanceStore::Clone(*C.Config, Command::Str(A, "uid"), Command::Str(A, "from"), Command::Str(A, "to"), &Err), Err);
            return json{{"cloned", Command::Str(A, "to")}};
        }, {} });

    Command::Register({
        "instances delete", "delete an instance — its saves and settings go with it",
        "Removes the instance's folder entirely. There is no undo: --yes is required.",
        { UidArg(), Pos("name", "its name"), Flag("yes", "yes, delete its saves") }, { "instances delete 749 Experiment --yes" },
        { "InstanceStore::Delete" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this deletes the instance's saves: add --yes");
            std::string Err;
            Check(InstanceStore::Delete(*C.Config, Command::Str(A, "uid"), Command::Str(A, "name"), &Err), Err);
            return json{{"deleted", Command::Str(A, "name")}};
        }, {} });

    Command::Register({
        "instances config", "an instance's config (instance.json), or write it",
        "What an instance keeps beside its saves: settings, variables, LASTRUN, what each option's file last held. "
        "--set writes a whole new config (JSON); --merge merges keys into it; --touch stamps LASTRUN now (makes it active).",
        { UidArg(), Opt("instance", "which (default: the active one)"), WithSample(Opt("set", "the new config (JSON)", Type::Json), "{\"VARIABLES\":{},\"LASTRUN\":0}"),
          WithSample(Opt("merge", "keys to merge in (JSON)", Type::Json), "{\"VARIABLES\":{\"aoe2_res\":\"1920x1080\"}}"), Flag("touch", "stamp LASTRUN = now") },
        { "instances config 749", "instances config 749 --merge '{\"VARIABLES\":{\"aoe2_res\":\"1920x1080\"}}'", "instances config 749 --instance Main --touch" },
        { "InstanceStore::ReadConfig", "InstanceStore::WriteConfig", "InstanceStore::TouchLastRun" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string U = Command::Str(A, "uid"), I = Command::Str(A, "instance", "");
            std::string Err;
            if (A.contains("set") || A.contains("merge"))
            {
                json Cfg = A.contains("set") ? A["set"] : InstanceStore::ReadConfig(*C.Config, U, I);
                if (A.contains("merge")) Cfg.merge_patch(A["merge"]);
                Check(InstanceStore::WriteConfig(*C.Config, U, I, Cfg, &Err), Err);
            }
            if (Command::Bool(A, "touch", false)) InstanceStore::TouchLastRun(*C.Config, U, I);
            return InstanceStore::ReadConfig(*C.Config, U, I);
        }, {} });

    // ---------------- deps ----------------
    Command::Register({
        "deps check", "what this machine has of what a launch needs",
        "Probe the host (libraries via ldconfig, programs on PATH, files beside the app, the Vulkan driver, the sandbox, "
        "WinFsp) and classify every dependency: ok, partial (the 32-bit half missing), missing — with the package to "
        "install on this distro, and the one command that installs them all. Settings → Dependencies shows the same. "
        "--missing lists only what is not ok.",
        { Flag("missing", "only what is not ok") }, { "deps check", "deps check --missing --json" },
        { "DepCheck::AllDeps", "DepCheck::ProbeSystem", "DepCheck::Check", "DepCheck::MissingCore", "DepCheck::DistroFromOsRelease",
          "DepCheck::DistroLabel", "DepCheck::CategoryLabel", "DepCheck::InstallHint", "DepCheck::InstallCommand", "SandboxLayer::Available" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const DepCheck::SystemState St = DepCheck::ProbeSystem(SandboxLayer::Available());
            const DepCheck::Distro D = DepCheck::DistroFromOsRelease();
            json Rows = json::array();
            std::vector<std::string> Missing;
            for (const auto &I : DepCheck::AllDeps())
            {
                const DepCheck::Status S = DepCheck::Check(I, St);
                if (S != DepCheck::Status::Ok) Missing.push_back(DepCheck::InstallHint(I, S, D));
                if (Command::Bool(A, "missing", false) && S == DepCheck::Status::Ok) continue;
                Rows.push_back({{"dependency", I.Name}, {"category", DepCheck::CategoryLabel(I.Cat)}, {"status", StatusName(S)},
                                {"install", S == DepCheck::Status::Ok ? std::string() : DepCheck::InstallHint(I, S, D)}, {"for", I.Purpose}});
            }
            return json{{"distro", DepCheck::DistroLabel(D)}, {"can_launch", DepCheck::MissingCore(St).empty()},
                        {"missing_core", DepCheck::MissingCore(St)}, {"install_command", DepCheck::InstallCommand(Missing, D)},
                        {"dependencies", Rows}};
        }, {} });

    Command::Register({
        "deps ldconfig", "parse an `ldconfig -p` listing: which libraries have their 64- and 32-bit halves",
        "The library half of the probe on a listing you give (a file, or - for stdin) — a machine's libraries without "
        "running anything there.", { Pos("file", "the `ldconfig -p` output (- for stdin)"), Opt("lib", "only this soname") },
        { "deps ldconfig ldconfig.txt --lib libvulkan.so.1" }, { "DepCheck::ParseLdconfig" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string F = Command::Str(A, "file");
            std::stringstream S;
            if (F == "-") S << std::cin.rdbuf();
            else { std::ifstream In(F); if (!In) throw Command::BadArgs("cannot read '" + F + "'"); S << In.rdbuf(); }
            json Out = json::object();
            for (const auto &[Lib, Has] : DepCheck::ParseLdconfig(S.str()))
                if (!A.contains("lib") || Lib == Command::Str(A, "lib")) Out[Lib] = {{"64", Has.first}, {"32", Has.second}};
            return Out;
        }, {} });
}
