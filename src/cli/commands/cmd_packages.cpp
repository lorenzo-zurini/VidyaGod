// `VidyaGod packages …`, `sources …`, `updates …`, `received …`, `grafts …`, `runners …` — the package catalog's
// functions (packagecatalog.h): what is installed and where, the sources a library syncs from, a friend's packages as
// received and installed, the optional pieces a game takes, and the runners that carry it.
#include "nodegraph.h"
#include "instancestore.h"
#include "installed.h"
#include "shelf.h"
#include "command.h"
#include "cmdutil.h"
#include "appmodel.h"
#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "manifestmodel.h"

#include <QString>
#include <fstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

json Strings(const std::vector<std::string> &V) { return json(V); }

json Paths(const std::vector<std::filesystem::path> &V)
{
    json Out = json::array();
    for (const auto &X : V) Out.push_back(X.string());
    return Out;
}

json Runners(const std::vector<const Node *> &V)
{
    json Out = json::array();
    for (const Node *N : V)
        Out.push_back({{"runner", N->NodeId.empty() ? N->Key() : N->NodeId}, {"host", N->HostPlatform}, {"guests", N->GuestPlatform},
                       {"package", N->BundleDir.filename().string()}, {"cid", N->Key()}});
    return Out;
}

json Targets(const std::vector<IpfsWrapper::FetchTarget> &T)
{
    json Out = json::array();
    for (const auto &X : T) Out.push_back({{"cid", X.Cid}, {"to", X.LocalPath}, {"folder", X.Dir}, {"optional", X.Optional}, {"verify", X.Verify}});
    return Out;
}

std::map<std::string, std::string> Vars(const json &A, const char *Key)
{
    std::map<std::string, std::string> Out;
    for (const std::string &KV : List(A, Key))
    {
        const auto Eq = KV.find('=');
        if (Eq == std::string::npos) throw Command::BadArgs(std::string("--") + Key + " takes NAME=VALUE pairs, not '" + KV + "'");
        Out[KV.substr(0, Eq)] = KV.substr(Eq + 1);
    }
    return Out;
}

Shelf::GraftChoice Grafts(const json &A)
{
    if (!A.contains("grafts")) return std::nullopt;
    return List(A, "grafts");
}

Command::Arg GameArg() { return Command::Pos("game", "a game's node: its CID, working handle, or LABEL"); }

// A package source named on the command line (its NAME or CID): its index in Settings.PackageSources.
size_t SourceIndex(Command::Context &C, const std::string &Q)
{
    const auto &Srcs = (*C.Config)["Settings"]["PackageSources"];
    for (size_t I = 0; I < Srcs.size(); ++I)
    {
        const auto &S = Srcs[I];
        if ((S.is_object() && (S.value("NAME", std::string()) == Q || S.value("CID", std::string()) == Q)) || (S.is_string() && S == Q))
            return I;
    }
    throw Command::BadArgs("no package source '" + Q + "' (VidyaGod sources list shows them)");
}

json Update(const PackageCatalog::PackageUpdate &U)
{
    return {{"package", U.Pkg}, {"library", U.Lib}, {"from", U.Nick.empty() ? U.Peer : U.Nick}, {"folder", U.FolderCid}, {"dir", U.Dir.string()}};
}

PackageCatalog::PackageUpdate FindUpdate(Command::Context &C, const std::string &Q)
{
    for (const auto &U : PackageCatalog::AvailableUpdates(*C.Config))
        if (U.Pkg == Q || U.FolderCid == Q || QString::fromStdString(U.Pkg).contains(QString::fromStdString(Q), Qt::CaseInsensitive)) return U;
    throw Command::BadArgs("no update for '" + Q + "' (VidyaGod updates list shows them)");
}

json Staged(const PackageCatalog::StagedUpdate &S)
{
    return {{"update", Update(S.U)}, {"stage", S.Stage.string()}, {"added", S.Added}, {"removed", S.Removed}, {"kept", S.Kept}, {"moves", S.Moves.size()},
            {"deletes", S.Deletes.size()}, {"delete_bytes", S.DeleteBytes}};
}

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("packages", "package folders: where they are, their settings, publish, seed, install, uninstall");
    Command::RegisterGroup("sources", "the package sources a library syncs from");
    Command::RegisterGroup("updates", "newer versions of installed packages, from friends");
    Command::RegisterGroup("received", "a friend's packages as they arrive: planned, landed, adopted");
    Command::RegisterGroup("grafts", "the optional pieces a game takes (mods, fixes, soundtracks)");
    Command::RegisterGroup("runners", "what can run a game here: compatible, installed, usable, the chain");

    // ---------------- packages ----------------
    Command::Register({
        "packages paths", "where the library, the received catalog and the covers live",
        "The managed library root, the received-packages root (CATALOG), the covers cache (ASSETS), and every folder "
        "the package editor may author into.", {}, { "packages paths" },
        { "PackageCatalog::LibraryRootDir", "PackageCatalog::CatalogRootDir", "PackageCatalog::AssetsRootDir",
          "PackageCatalog::EditableRoots", "PackageCatalog::LocalPackageDirs", "PackageCatalog::LibraryNames" },
        Needs::Config, false,
        [](Command::Context &C, const json &) {
            return json{{"library", PackageCatalog::LibraryRootDir(*C.Config)}, {"catalog", PackageCatalog::CatalogRootDir(*C.Config)},
                        {"assets", PackageCatalog::AssetsRootDir(*C.Config)}, {"libraries", PackageCatalog::LibraryNames(*C.Config)},
                        {"editable", Paths(PackageCatalog::EditableRoots(*C.Config))},
                        {"local_packages", Paths(PackageCatalog::LocalPackageDirs(*C.Config))}};
        }, {} });

    Command::Register({
        "packages list", "every package you author or installed, by library",
        "The packages of your library folders (LIBRARY/<library>/<package>), each with its title and library — what the "
        "package editor's Open lists.", {}, { "packages list", "packages list --json" },
        { "PackageCatalog::AuthoredPackages" }, Needs::Config, false,
        [](Command::Context &C, const json &) {
            json Out = json::array();
            for (const auto &P : PackageCatalog::AuthoredPackages(*C.Config))
                Out.push_back({{"package", P.Dir.filename().string()}, {"title", P.Title}, {"library", P.Library}, {"dir", P.Dir.string()}});
            return Out;
        }, {} });

    Command::Register({
        "packages installed", "the packages installed here, with their size and versions",
        "What Settings → Installed Packages lists: every package of LIBRARY (or added from outside it) with a launchable "
        "whose content is all on disk — its UID, title, library, versions, folder and size.",
        { Flag("no-sizes", "skip walking the folders for their sizes") }, { "packages installed", "packages installed --json" },
        { "PackageCatalog::InstalledPackages" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            json Out = json::array();
            for (const auto &P : PackageCatalog::InstalledPackages(*C.Config, *Index(C), !Command::Bool(A, "no-sizes", false)))
                Out.push_back({{"uid", P.Uid}, {"title", P.Title}, {"library", P.Local ? "(added locally)" : P.Library}, {"removable", P.Local || P.HasEntry},
                               {"versions", P.Variants}, {"bytes", P.Bytes}, {"dir", P.Dir.string()}});
            return Out;
        }, {} });

    Command::Register({
        "packages show", "one package folder: its identity, title, library, state, CIDs",
        "What a package folder is: its UID and name (from its nodes), title, library, whether it is a package source's "
        "or added locally, holds content, its folder CID (as shared) and pin folder, its editor-layout key, and the "
        "content CIDs its nodes name.", { Pos("package", "a folder path, its name, its UID, or part of its title") },
        { "packages show 749", "packages show 'Age of Empires II' --json" },
        { "PackageCatalog::ScanBundleIdentity", "PackageCatalog::PackageTitle", "PackageCatalog::FolderTitle", "PackageCatalog::LibraryOf",
          "PackageCatalog::IsLocalPackagePath", "PackageCatalog::IsPackageSourcePath", "PackageCatalog::PackageSourceNameForPath",
          "PackageCatalog::DirHasContent", "PackageCatalog::PackageFolderCid", "PackageCatalog::EditorLayoutKey",
          "PackageCatalog::SeedTargets", "PackageCatalog::ManifestTargets", "PackageCatalog::NodeContentCids" },
        Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto D = PackageDir(C, Command::Str(A, "package"));
            const auto Id = PackageCatalog::ScanBundleIdentity(D.string());
            json Cids = json::array();
            for (const auto &E : std::filesystem::directory_iterator(D))
                if (E.path().extension() == ".json")
                {
                    json N;
                    if (NodeGraph::ReadTreeJsonBounded(E.path(), N) && N.is_object()) for (const auto &X : PackageCatalog::NodeContentCids(N)) Cids.push_back(X);
                }
            json Seeds = json::object();
            for (const auto &[Path, Cid] : PackageCatalog::SeedTargets(D.string())) Seeds[Path] = Cid;
            json Man = json::object();
            for (const auto &[Path, Cid] : PackageCatalog::ManifestTargets(D.string())) Man[Path] = Cid;
            return json{{"dir", D.string()}, {"uid", Id.Uid}, {"name", Id.Name}, {"title", PackageCatalog::PackageTitle(D)},
                        {"folder_title", PackageCatalog::FolderTitle(D)},
                        {"library", PackageCatalog::LibraryOf(D, PackageCatalog::LibraryRootDir(*C.Config))},
                        {"launchable", Id.HasLaunchable}, {"runner", Id.HasRunner},
                        {"local", PackageCatalog::IsLocalPackagePath(*C.Config, D)}, {"from_source", PackageCatalog::IsPackageSourcePath(*C.Config, D)},
                        {"source", PackageCatalog::PackageSourceNameForPath(*C.Config, D)}, {"has_content", PackageCatalog::DirHasContent(D.string())},
                        {"folder_cid", PackageCatalog::PackageFolderCid(*C.Config, D)}, {"pin_cid", PackageCatalog::PackageFolderCid(*C.Config, D, "pin")},
                        {"layout_key", PackageCatalog::EditorLayoutKey(D)}, {"content_cids", Cids},
                        {"seed_targets", Seeds}, {"manifest_targets", Man}};
        }, {} });

    Command::Register({
        "packages new", "create an empty package (a game) or library package",
        "A new, empty package folder in one of your libraries — the package editor's New package. A game is named "
        "\"[<uid>][v1.0] <title>\"; with --library-package, a library package (dgVoodoo, a runtime) is named by its name.",
        { Pos("library", "the library to create it in (VidyaGod, VidyaGodLibraries, …)"), Pos("title", "the game's title (or the library package's name)"),
          Opt("uid", "the game's UID (digits; required for a game)"), Flag("library-package", "a library package: no UID, no tile") },
        { "packages new VidyaGod 'Some Game' --uid 123456", "packages new VidyaGodLibraries dxwrapper --library-package" },
        { "PackageCatalog::CreatePackageFolder", "PackageCatalog::CreateLibraryFolder" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::string Err;
            const std::string Lib = Command::Str(A, "library"), Title = Command::Str(A, "title");
            const auto D = Command::Bool(A, "library-package", false)
                ? PackageCatalog::CreateLibraryFolder(*C.Config, Lib, Title, Err)
                : PackageCatalog::CreatePackageFolder(*C.Config, Lib, Command::Str(A, "uid"), Title, Err);
            if (D.empty()) throw std::runtime_error(Err);
            return json{{"created", D.string()}};
        }, {} });

    Command::Register({
        "packages settings", "a game's saved settings (its instance file), or set one",
        "What the launch window saves for a game — its instance's settings (RUNNER_CHAIN, the grafts ticked, …) and its "
        "VARIABLES (the options and secrets). With --set KEY=VALUE (a JSON value) a setting is written; with --var "
        "NAME=VALUE a variable.",
        { Pos("uid", "the game's family UID (its package's)"), Opt("instance", "which instance (default: the most recently played)"),
          Opt("set", "write a setting: KEY=<json>"), Opt("var", "write variables: NAME=VALUE[,NAME=VALUE…]") },
        { "packages settings 749", "packages settings 749 --var aoe2_res=1920x1080", "packages settings 749 --set 'GRAFTS=[]'" },
        { "InstanceStore::ReadConfig", "InstanceStore::SetSetting", "InstanceStore::Variables",
          "InstanceStore::MergeVariables" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string Uid = Command::Str(A, "uid"), Inst = Command::Str(A, "instance", "");
            if (!Index(C)->Tile(Uid) && InstanceStore::ReadConfig(*C.Config, Uid, Inst).empty()
                && InstanceStore::Variables(*C.Config, Uid, Inst).empty())
                throw Command::BadArgs("no game with UID '" + Uid + "' (VidyaGod catalog shelf lists them)");
            if (A.contains("set"))
            {
                const std::string KV = Command::Str(A, "set");
                const auto Eq = KV.find('=');
                if (Eq == std::string::npos) throw Command::BadArgs("--set takes KEY=<json>");
                json V = json::parse(KV.substr(Eq + 1), nullptr, false);
                if (V.is_discarded()) V = KV.substr(Eq + 1);   // a bare word: a string
                InstanceStore::SetSetting(*C.Config, Uid, KV.substr(0, Eq), V, Inst);
            }
            if (A.contains("var")) InstanceStore::MergeVariables(*C.Config, Uid, Vars(A, "var"), Inst);
            return json{{"settings", InstanceStore::ReadConfig(*C.Config, Uid, Inst)},
                        {"variables", InstanceStore::Variables(*C.Config, Uid, Inst)}};
        }, {} });

    Command::Register({
        "packages golden", "a game's golden mark: when it was set, or set / clear it",
        "Whether a game is marked golden on this machine (it passed the golden checklist) and since when; --on DATE "
        "sets it, --off clears it.",
        { Pos("uid", "the game's family UID"), Opt("on", "mark it golden, as of this date (YYYY-MM-DD)"), Flag("off", "clear the mark") },
        { "packages golden 749", "packages golden 749 --on 2026-11-04", "packages golden 749 --off" },
        { "PackageCatalog::GoldenSince", "PackageCatalog::SetGolden" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string Uid = Command::Str(A, "uid");
            if (A.contains("on")) { PackageCatalog::SetGolden(*C.Config, Uid, true, Command::Str(A, "on")); Save(C); }
            else if (Command::Bool(A, "off", false)) { PackageCatalog::SetGolden(*C.Config, Uid, false, ""); Save(C); }
            const std::string Since = PackageCatalog::GoldenSince(*C.Config, Uid);
            return json{{"uid", Uid}, {"golden", !Since.empty()}, {"since", Since}};
        }, {} });

    Command::Register({
        "packages prune-moved", "forget local packages whose folder is gone",
        "Drop the library entries of locally-added packages whose folder was moved or deleted (source packages are "
        "never touched).", {}, { "packages prune-moved" }, { "PackageCatalog::PruneMovedLocalPackages" }, Needs::Config, false,
        [](Command::Context &C, const json &) {
            const int N = PackageCatalog::PruneMovedLocalPackages(*C.Config);
            if (N > 0) Save(C);
            return json{{"forgotten", N}};
        }, {} });

    Command::Register({
        "packages hydrated", "whether a game is installed here, and what it would fetch",
        "A game's install state from its node: every content file present (hydrated), any content at all, a package it "
        "contains not landed yet; and the content CIDs (with their sizes) a download would fetch, the targets it would "
        "write, and the runner chain's build targets.",
        { GameArg(), Opt("grafts", "the grafts ticked (a,b,c; default: as saved)") },
        { "packages hydrated 'Patch 2.0a'" },
        { "Installed::NodeHydrated", "Installed::NodeHasContent", "PackageCatalog::NodeClosureIncomplete",
          "PackageCatalog::NodeContentCids", "PackageCatalog::NodeContentSizes", "PackageCatalog::CollectContentTargets",
          "PackageCatalog::CollectRunnerChainTargets" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::vector<IpfsWrapper::FetchTarget> T, R;
            std::string Err, RErr;
            const bool Ok = PackageCatalog::CollectContentTargets(*I, N.Key(), Grafts(A), T, &Err);
            const bool ROk = PackageCatalog::CollectRunnerChainTargets(*I, N.Key(), *C.Config, R, &RErr);
            json Sizes = json::object();
            for (const auto &[Cid, Sz] : PackageCatalog::NodeContentSizes(*I, N.Key(), Grafts(A))) Sizes[Cid] = Sz;
            return json{{"node", N.Key()}, {"hydrated", Installed::NodeHydrated(*I, N.Key())},
                        {"has_content", Installed::NodeHasContent(*I, N.Key())},
                        {"incomplete", PackageCatalog::NodeClosureIncomplete(*I, N.Key())},
                        {"content_cids", PackageCatalog::NodeContentCids(*I, N.Key(), Grafts(A))}, {"sizes", Sizes},
                        {"targets", Ok ? Targets(T) : json(Err)}, {"runner_targets", ROk ? Targets(R) : json(RErr)}};
        }, {} });

    Command::Register({
        "packages hydrate", "download a game's content in place (blocking)",
        "Fetch every missing content file of a game's closure (and its cover) where its nodes say — the engine's "
        "download, blocking until done. The Catalog's Download goes through the app's download manager instead "
        "(VidyaGod downloads start).", { GameArg(), Opt("grafts", "the grafts to fetch too (a,b,c)") },
        { "packages hydrate 'Patch 2.0a'" }, { "PackageCatalog::HydrateNode" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::string Err;
            if (!PackageCatalog::HydrateNode(*I, N.Key(), Grafts(A), &Err, C.Config)) throw std::runtime_error(Err);
            return json{{"hydrated", N.Key()}};
        }, {} });

    Command::Register({
        "packages dehydrate", "delete a game's downloaded content (keep its nodes)",
        "Remove the content files of a game's closure from disk and stop seeding them — it goes back to the Catalog as "
        "downloadable. Its node files and cover stay.", { GameArg() },
        { "packages dehydrate 'Patch 2.0a'" }, { "PackageCatalog::DehydrateNode" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            return json{{"files_removed", PackageCatalog::DehydrateNode(*I, NodeOf(*I, Command::Str(A, "game"))->Key())}};
        }, {} });

    Command::Register({
        "packages publish", "publish one package: seed its content, re-mint, optionally export it",
        "Share a package of yours: add every content file to IPFS (recording its SOURCE and SIZE), save its nodes "
        "(re-minted: recording a SOURCE changes a node's bytes and so its CID), and with --export write a copy of its "
        "node files alone there.", { Pos("package", "a folder path, its name, its UID, or part of its title"), Opt("export", "a folder for the node-files-only copy") },
        { "packages publish 749", "packages publish 749 --export /tmp/aoe2-nodes" },
        { "PackageCatalog::PublishPackage", "PackageCatalog::SeedNodeContent", "PackageCatalog::StampNodeCids" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto D = PackageDir(C, Command::Str(A, "package"), false);
            std::string Err;
            if (!PackageCatalog::PublishPackage(D.string(), Command::Str(A, "export", ""), &Err)) throw std::runtime_error(Err);
            return json{{"published", D.string()}};
        }, {} });

    Command::Register({
        "packages publish-all", "publish the whole library (Verify & Publish)",
        "Freeze every node of the library, store it, make one folder per package (and its pin folder), and record the "
        "share sheet — the Network tab's Verify & Publish. What did not publish whole (nodes that did not freeze, "
        "content not held here, content naming no SOURCE) is listed as gaps. With the app up, it runs in the app "
        "(follow it: events wait --signal libraryPublished).", {}, { "packages publish-all" },
        { "AppModel::publishLibraries", "AppModel::publishing", "PackageCatalog::PublishLibrary", "PackageCatalog::SharedWith" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            if (C.Model) { C.Model->publishLibraries(); return json{{"started", true}, {"follow", "VidyaGod events wait --signal libraryPublished"}}; }
            std::string Err, Gaps;
            const auto Out = PackageCatalog::PublishLibrary(*C.Config, &Err, &Gaps);
            if (Out.empty()) throw std::runtime_error(Err);
            Save(C);
            json Shared = json::object();
            for (const auto &[Lib, Rows] : (*C.Config)["Libraries"].items()) Shared[Lib] = Rows.size();
            return json{{"packages", Out.size()}, {"libraries", Shared}, {"gaps", Gaps}};
        }, {} });

    Command::Register({
        "packages meta-cid", "the CID a folder's node files would publish as",
        "Build (and store) the node-files-only folder of a package source tree and print its CID — what a source "
        "upgrade or a friend would fetch.", { Pos("dir", "the folder") }, { "packages meta-cid ~/.VidyaGod/LIBRARY/VidyaGod" },
        { "PackageCatalog::PublishMetaCid", "PackageCatalog::MirrorDehydrated" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            const std::string Cid = PackageCatalog::PublishMetaCid(Command::Str(A, "dir"), &Err);
            if (Cid.empty()) throw std::runtime_error(Err);
            return json{{"cid", Cid}};
        }, {} });

    Command::Register({
        "packages mirror", "copy a tree's node files (and covers) only",
        "Copy the node files of a package tree — no content — into another folder: a dehydrated mirror.",
        { Pos("from", "the source folder"), Pos("to", "the destination folder") }, { "packages mirror ~/.VidyaGod/LIBRARY/VidyaGod /tmp/nodes" },
        { "PackageCatalog::MirrorDehydrated" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            return json{{"files", PackageCatalog::MirrorDehydrated(Command::Str(A, "from"), Command::Str(A, "to"))}};
        }, {} });

    Command::Register({
        "packages seed", "add a folder's content to IPFS (serve it), or report what is not held",
        "Seed every file a folder's nodes name: added in place (no copy), so this machine serves it. --verify reads "
        "each back (catches a file whose bytes changed); --unheld only re-adds what the store does not hold; "
        "--covers-only just the covers.",
        { Pos("dir", "the folder (a package or a library)"), Flag("verify", "read every file back"), Flag("unheld", "only what is not held"),
          Flag("covers-only", "only the covers"), Flag("overwrite", "re-add even what is held") },
        { "packages seed ~/.VidyaGod/LIBRARY/VidyaGod/'[749][v1.0] Age of Empires II'", "packages seed ~/.VidyaGod/LIBRARY --unheld" },
        { "PackageCatalog::SeedDirectory", "PackageCatalog::SeedUnheld" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::vector<PackageCatalog::SeedFailure> F;
            json Fails = json::array();
            int N = 0, Mismatched = 0;
            if (Command::Bool(A, "unheld", false)) N = PackageCatalog::SeedUnheld(Command::Str(A, "dir"), &F);
            else N = PackageCatalog::SeedDirectory(Command::Str(A, "dir"), {}, &Mismatched, Command::Bool(A, "covers-only", false),
                                                   Command::Bool(A, "overwrite", false), Command::Bool(A, "verify", false), &F);
            for (const auto &X : F) Fails.push_back({{"path", X.Path}, {"recorded", X.RecordedCid}, {"actual", X.ActualCid}});
            return json{{"seeded", N}, {"mismatched", Mismatched}, {"failures", Fails}};
        }, {} });

    Command::Register({
        "packages seed-roots", "every folder whose content this machine serves",
        "Its own library and each package source: what the start-up seeding walks.", {}, { "packages seed-roots" },
        { "PackageCatalog::SeedRoots" }, Needs::Config, false,
        [](Command::Context &C, const json &) { return Strings(PackageCatalog::SeedRoots(*C.Config)); }, {} });

    Command::Register({
        "packages heal", "re-point content references that broke (moved or changed files)",
        "Repair this machine's serving of the library: re-add files whose references were lost, re-seed changed bytes, "
        "drop references to what is gone. --deep reads every reference back from the store.",
        { Flag("deep", "verify every reference's bytes (slow)"), Flag("prune-unreferenced", "also unpin healthy pins nothing references (the IPFS tab's \"unknown\")") }, { "packages heal", "packages heal --deep" },
        { "PackageCatalog::HealSourceContent", "PackageCatalog::HealReport::Ok" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            PackageCatalog::HealOptions O;
            O.Deep = Command::Bool(A, "deep", false);
            O.PruneUnreferenced = Command::Bool(A, "prune-unreferenced", false);
            const auto R = PackageCatalog::HealSourceContent(*C.Config, O);
            const json Out{{"ok", R.Ok()}, {"repointed", R.Repointed}, {"stale_repaired", R.StaleRepaired}, {"pruned_unservable", R.PrunedUnservable},
                           {"pruned_unreferenced", R.PrunedUnreferenced}, {"verified", R.Verified}, {"drift", R.Drift}, {"unrepaired", R.Unrepaired}};
            if (!R.Ok()) throw Command::CheckFailed(Out, "some references could not be repaired");
            return Out;
        }, {} });

    Command::Register({
        "packages stamp-cids", "write minted CIDs back into a working tree",
        "Rename each node file in a tree to the CID a mint gave it and re-point its references — the write-back after a "
        "publish. --map is a JSON object {handle: cid}.",
        { Pos("root", "the tree"), WithSample(Opt("map", "{handle: cid} as JSON", Type::Json, true), "{\"draft-1\":\"bafkrei…\"}") }, { "packages stamp-cids ~/.VidyaGod/LIBRARY --map '{}'" },
        { "PackageCatalog::StampNodeCids" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::map<std::string, std::string> M;
            for (const auto &[K, V] : A["map"].items()) M[K] = V.get<std::string>();
            std::string Err;
            const int N = PackageCatalog::StampNodeCids(Command::Str(A, "root"), M, &Err);
            if (N < 0) throw std::runtime_error(Err);
            return json{{"stamped", N}};
        }, {} });

    // ---------------- sources ----------------
    Command::Register({
        "sources list", "the package sources and where each syncs",
        "Every package source (a folder CID): its name, CID, its folder here and whether it is synced, and whether any "
        "source is still missing.", {}, { "sources list" },
        { "PackageCatalog::PackageSourceCID", "PackageCatalog::PackageSourceDir", "PackageCatalog::SourceDirSynced",
          "PackageCatalog::PackageSourceDirs", "PackageCatalog::HasMissingSources" }, Needs::Config, false,
        [](Command::Context &C, const json &) {
            json Rows = json::array();
            for (const auto &S : (*C.Config)["Settings"]["PackageSources"])
            {
                const std::string Dir = PackageCatalog::PackageSourceDir(*C.Config, S);
                std::error_code Ec;
                Rows.push_back({{"name", S.is_object() ? S.value("NAME", std::string()) : std::string()}, {"cid", PackageCatalog::PackageSourceCID(S)},
                                {"dir", Dir}, {"synced", PackageCatalog::SourceDirSynced(Dir, Ec)}});
            }
            return json{{"sources", Rows}, {"synced_dirs", PackageCatalog::PackageSourceDirs(*C.Config)},
                        {"missing", PackageCatalog::HasMissingSources(*C.Config)}};
        }, {} });

    Command::Register({
        "sources add", "add a package source (a folder CID)",
        "Add a source to sync packages from — Settings → Sources → Add. With the app up it fetches the tree at once "
        "(events wait --signal packageSourcesChanged); headless, run `sources sync`.",
        { Pos("cid", "the source's folder CID"), Pos("name", "a name for it") },
        { "sources add bafybei… 'Some library'" }, { "AppModel::addPackageSource", "PackageCatalog::AddPackageSource" , "SourcesPage::rebuild" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const std::string Cid = Command::Str(A, "cid"), Name = Command::Str(A, "name");
            if (C.Model)
            {
                if (!C.Model->addPackageSource(QString::fromStdString(Cid), QString::fromStdString(Name))) throw std::runtime_error("empty or already added");
                return json{{"added", Name}, {"syncing", true}};
            }
            if (!PackageCatalog::AddPackageSource(*C.Config, Cid, Name)) throw std::runtime_error("empty or already added");
            Save(C);
            return json{{"added", Name}};
        }, {} });

    Command::Register({
        "sources remove", "remove a package source (its folder and entries go)",
        "Drop a source: its config entry, its fetched folder and its library entries. --keep-installed keeps packages "
        "you installed from it (as local packages).",
        { Pos("source", "its name or CID"), Flag("keep-installed", "keep what was installed from it") },
        { "sources remove 'Some library'" }, { "AppModel::removePackageSource", "PackageCatalog::RemovePackageSource" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const int Index = static_cast<int>(SourceIndex(C, Command::Str(A, "source")));
            if (C.Model && !Command::Bool(A, "keep-installed", false)) C.Model->removePackageSource(Index);
            else { PackageCatalog::RemovePackageSource(*C.Config, Index, Command::Bool(A, "keep-installed", false)); Save(C); }
            return json{{"removed", Index}};
        }, {} });

    Command::Register({
        "sources sync", "fetch every source not yet here and index it",
        "Sync the package sources: fetch what is missing, index each — what start-up does. With the app up, it syncs "
        "in the app (events wait --signal packageSourcesChanged).", {}, { "sources sync" },
        { "AppModel::syncSources", "PackageCatalog::SyncPackageSources", "PackageCatalog::MergeListChange", "PackageCatalog::LibraryEntryKey", "PackageCatalog::SourceKey" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            if (C.Model) { C.Model->syncSources(); return json{{"started", true}}; }
            std::string Err;
            const int N = PackageCatalog::SyncPackageSources(*C.Config, &Err);
            Save(C);
            if (!Err.empty()) throw std::runtime_error(Err);
            return json{{"synced", N}};
        }, {} });

    Command::Register({
        "sources content", "the content CIDs a source's packages name",
        "Every content CID the packages of a source reference: what serving it means.", { Pos("source", "its name or CID") },
        { "sources content 'Some library'" }, { "PackageCatalog::SourceContentCids" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto Cids = PackageCatalog::SourceContentCids(*C.Config, (*C.Config)["Settings"]["PackageSources"][SourceIndex(C, Command::Str(A, "source"))]);
            return json(std::vector<std::string>(Cids.begin(), Cids.end()));
        }, {} });

    Command::Register({
        "sources upgrade", "upgrade a source to a newer CID: plan it, then apply",
        "Fetch the new version of a source into a staging folder and diff it against what is here (nothing changes); "
        "with --apply, switch to it (the old version kept under LIBRARY/.deprecated). --force applies even when the "
        "new CID is not a newer version of the same collection.",
        { Pos("source", "its name"), Pos("cid", "the new CID"), Flag("apply", "apply the plan"), Flag("force", "apply even across collections") },
        { "sources upgrade 'Some library' bafybei…", "sources upgrade 'Some library' bafybei… --apply" },
        { "PackageCatalog::PlanSourceUpgrade", "PackageCatalog::ApplySourceUpgrade", "AppModel::planSourceUpgrade", "AppModel::applySourceUpgrade" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            const auto Plan = PackageCatalog::PlanSourceUpgrade(*C.Config, Command::Str(A, "source"), Command::Str(A, "cid"), &Err);
            if (!Plan.Valid) throw std::runtime_error(Err);
            json Out = {{"name", Plan.Name}, {"from", Plan.OldCid}, {"to", Plan.NewCid}, {"added", Plan.JsonAdded}, {"changed", Plan.JsonChanged},
                        {"removed", Plan.JsonRemoved}, {"staging", Plan.StagingDir}};
            if (Command::Bool(A, "apply", false))
            {
                if (!PackageCatalog::ApplySourceUpgrade(*C.Config, Plan, Command::Bool(A, "force", false), &Err)) throw std::runtime_error(Err);
                Save(C);
                if (C.Model) C.Model->rebuildCatalog();
                Out["applied"] = true;
            }
            return Out;
        }, {} });

    // ---------------- updates ----------------
    Command::Register({
        "updates list", "newer versions of installed packages that friends share",
        "For each package you installed from a friend, a newer version they now share — its package, library, friend "
        "and folder CID.", {}, { "updates list" }, { "PackageCatalog::AvailableUpdates" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            json Out = json::array();
            for (const auto &U : PackageCatalog::AvailableUpdates(*C.Config)) Out.push_back(Update(U));
            return Out;
        }, {} });

    Command::Register({
        "updates apply", "switch an installed package to the newer version (shows what changes)",
        "Stage the newer version beside the installed one and list the files it adds, removes and keeps; --yes switches "
        "to it, --discard throws the stage away.",
        { Pos("package", "the package (its name or the update's folder CID)"), Flag("yes", "switch to it"), Flag("discard", "discard the stage instead") },
        { "updates apply 'Age of Empires II'", "updates apply 'Age of Empires II' --yes" },
        { "PackageCatalog::StageUpdate", "PackageCatalog::ApplyStagedUpdate", "PackageCatalog::DiscardStagedUpdate" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            PackageCatalog::StagedUpdate S;
            std::string Err;
            if (!PackageCatalog::StageUpdate(*C.Config, FindUpdate(C, Command::Str(A, "package")), &S, &Err)) throw std::runtime_error(Err);
            json Out = Staged(S);
            if (Command::Bool(A, "discard", false)) { PackageCatalog::DiscardStagedUpdate(S); Out["discarded"] = true; }
            else if (Command::Bool(A, "yes", false))
            {
                if (!PackageCatalog::ApplyStagedUpdate(*C.Config, S, &Err)) throw std::runtime_error(Err);
                if (C.Model) C.Model->rebuildCatalog();
                Out["applied"] = true;
            }
            return Out;
        }, {} });

    // ---------------- received ----------------
    Command::Register({
        "received plan", "where a friend's shared packages would land",
        "The folders a friend's share snapshot lands into (CATALOG/<nick> - <library>/<package>) and the fetch each is, "
        "and whether any received package is still incomplete.", { Pos("friend", "the friend's peer id (VidyaGod friends list)") },
        { "received plan 12D3KooW9z4dCR2LVkuc4xb4zTrss5AR9N5WcAAYdJSDat1VCfAR" },
        { "PackageCatalog::PlanReceivedFetches", "PackageCatalog::ReceivedFetchTargets", "PackageCatalog::ReceivedNickLabel",
          "PackageCatalog::ReceivedPackagesIncomplete" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string P = Command::Str(A, "friend");
            const json &Fl = C.Config->value("FriendLibraries", json::object());
            if (!Fl.contains(P)) return json{{"friend", P}, {"held", false}};
            const auto Plan = PackageCatalog::PlanReceivedFetches(*C.Config, PackageCatalog::ReceivedNickLabel(P), Fl[P]);
            json Rows = json::array();
            for (const auto &R : Plan) Rows.push_back({{"cid", R.Cid}, {"to", R.Dest}});
            return json{{"friend", P}, {"label", PackageCatalog::ReceivedNickLabel(P)}, {"fetches", Rows},
                        {"targets", Targets(PackageCatalog::ReceivedFetchTargets(Plan))},
                        {"incomplete", PackageCatalog::ReceivedPackagesIncomplete(*C.Config)}};
        }, {} });

    Command::Register({
        "received land", "land every received package's node files now",
        "Fetch each package folder friends share with you (node files only — content stays lazy until install). With "
        "the app up it lands in the app.", {}, { "received land" },
        { "PackageCatalog::LandReceivedPackages", "AppModel::completeReceivedClosures" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            if (C.Model) { C.Model->completeReceivedClosures(); return json{{"started", true}}; }
            std::string Err;
            if (!PackageCatalog::LandReceivedPackages(*C.Config, &Err)) throw std::runtime_error(Err);
            return json{{"landed", true}};
        }, {} });

    Command::Register({
        "received prune", "drop received node files no share lists any more",
        "Remove the node files of older generations of friends' packages (an installed package keeps its content).", {},
        { "received prune" }, { "PackageCatalog::PruneStaleReceived" }, Needs::Config, false,
        [](Command::Context &C, const json &) { return json{{"pruned", PackageCatalog::PruneStaleReceived(*Index(C), *C.Config)}}; }, {} });

    Command::Register({
        "received adopt", "install a received package: move it into your library",
        "What a download does first: move a friend's package folder out of CATALOG into LIBRARY/<library>/<package>, "
        "where it is an ordinary package of yours. --for lists every package a game's download would adopt instead.",
        { Opt("package", "the received package folder"), Opt("for", "a game's node: list what its download adopts") },
        { "received adopt --for 'Patch 2.0a'", "received adopt --package 'Age of Empires II'" },
        { "PackageCatalog::PackagesToAdopt", "PackageCatalog::AdoptReceivedPackage" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("for"))
            {
                const auto I = Index(C);
                const auto Set = PackageCatalog::PackagesToAdopt(*I, { NodeOf(*I, Command::Str(A, "for"))->Key() }, {}, *C.Config);
                return Paths(std::vector<std::filesystem::path>(Set.begin(), Set.end()));
            }
            std::filesystem::path New;
            std::string Err;
            if (!PackageCatalog::AdoptReceivedPackage(*C.Config, PackageDir(C, Command::Str(A, "package")), &New, &Err)) throw std::runtime_error(Err);
            if (C.Model) C.Model->rebuildCatalog();
            return json{{"installed_at", New.string()}};
        }, {} });

    // ---------------- grafts ----------------
    Command::Register({
        "grafts offered", "the optional pieces a game offers, and which are ticked",
        "Every graft offered on a game (mods, fixes, a soundtrack): the author's pre-ticked ones, the ones applied for a "
        "choice (in order), and what a choice leaves out (an exclusion, a missing base).",
        { GameArg(), Opt("grafts", "a choice to evaluate (a,b,c; default: the author's)"), Opt("tile", "the tile it is offered under") },
        { "grafts offered 'Patch 1.0e (Unofficial)'" },
        { "Shelf::OfferedGrafts", "Shelf::AppliedGrafts", "Shelf::CurrentGraftChoice", "Shelf::GraftBases",
          "Shelf::DefaultGraftBase" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::vector<std::string> Pre, Dropped;
            const auto Offered = Shelf::OfferedGrafts(*I, N.Key(), &Pre, Grafts(A), Command::Str(A, "tile", ""));
            const auto Choice = Shelf::CurrentGraftChoice(*I, Grafts(A));
            const auto Applied = Shelf::AppliedGrafts(*I, N.Key(), Choice ? Choice : Shelf::GraftChoice(Pre), {}, {}, &Dropped);
            json Rows = json::array();
            for (const auto &G : Offered)
            {
                const Node *Gn = I->Find(G);
                Rows.push_back({{"graft", Gn && !Gn->NodeId.empty() ? Gn->NodeId : G}, {"cid", G},
                                {"pre_ticked", std::find(Pre.begin(), Pre.end(), G) != Pre.end()},
                                {"applied", std::find(Applied.begin(), Applied.end(), G) != Applied.end()},
                                {"bases", Shelf::GraftBases(*I, G)}, {"default_base", Shelf::DefaultGraftBase(*I, G)}});
            }
            return json{{"grafts", Rows}, {"applied_order", Applied}, {"left_out", Dropped}};
        }, {} });

    Command::Register({
        "grafts tick", "what a choice becomes when one graft is ticked (or moved)",
        "The engine's rules for a graft choice: ticking G adds it (and unticks what excludes it); --move N shifts the "
        "ticked graft at position --from by N places (load order). Prints the new choice; `packages settings` saves it.",
        { GameArg(), Opt("ticked", "the current choice (a,b,c)", Type::String, true), Opt("graft", "the graft to tick"),
          Opt("from", "the position to move from (0-based)", Type::Int), Opt("move", "places to move by (±)", Type::Int) },
        { "grafts tick 'Patch 1.0e (Unofficial)' --ticked 'UserPatch 1.5' --graft 'Forgotten Empires 2.2'", "grafts tick 'Patch 1.0e (Unofficial)' --ticked 'UserPatch 1.5,Forgotten Empires 2.2' --from 1 --move -1" },
        { "Shelf::TickGraft", "Shelf::MoveGraft" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::vector<std::string> T = List(A, "ticked"), Unticked;
            if (A.contains("graft")) T = Shelf::TickGraft(*I, N.Key(), T, Command::Str(A, "graft"), &Unticked);
            std::string Why;
            if (A.contains("move") && !Shelf::MoveGraft(*I, N.Key(), T, static_cast<size_t>(Command::Int(A, "from")),
                                                                 static_cast<int>(Command::Int(A, "move")), &Why))
                throw std::runtime_error(Why);
            return json{{"ticked", T}, {"unticked", Unticked}};
        }, {} });

    Command::Register({
        "grafts row", "the row a tile shows for a variant",
        "Which node a tile's row named <variant> is — the variant presenting the tile under that name.",
        { Pos("tile", "the tile UID"), Pos("variant", "the variant's name") }, { "grafts row 749 'Patch 2.0a (No-CD)'" },
        { "Shelf::RowUnderTile" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::string Why;
            const std::string R = Shelf::RowUnderTile(*Index(C), Command::Str(A, "tile"), Command::Str(A, "variant"), &Why);
            if (R.empty()) throw std::runtime_error(Why);
            return json{{"node", R}};
        }, {} });

    // ---------------- runners ----------------
    Command::Register({
        "runners for", "the runners that can carry a game here, and the chain it would use",
        "For a game's node: the runners compatible with it on this machine (installed or not), those installed and "
        "usable, whether any chain reaches this machine, and the runner chain the launch resolves.",
        { GameArg() }, { "runners for 'Patch 2.0a'" },
        { "Installed::CompatibleRunners", "Installed::UsableRunners", "Installed::HasUsableChain",
          "PackageCatalog::RunnerChainIds" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            return json{{"compatible", Runners(Installed::CompatibleRunners(*I, N))}, {"usable", Runners(Installed::UsableRunners(*I, N))},
                        {"has_chain", Installed::HasUsableChain(*I, N)}, {"chain", PackageCatalog::RunnerChainIds(*I, N.Key(), *C.Config)}};
        }, {} });

    Command::Register({
        "runners candidates", "the installed runners that take a platform as input",
        "The choices for one step of a runner chain: installed runners whose GUEST takes <platform> (a game's own "
        "\"custom\" engine only from its own package, --package).",
        { Pos("platform", "the input platform (win32, dos, n64, …)"), Opt("package", "the game's package folder (for \"custom\")") },
        { "runners candidates win32", "runners candidates custom --package 749" }, { "Installed::CandidateRunners" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::filesystem::path P = A.contains("package") ? PackageDir(C, Command::Str(A, "package")) : std::filesystem::path();
            return Runners(Installed::CandidateRunners(*Index(C), Command::Str(A, "platform"), P));
        }, {} });

    Command::Register({
        "runners state", "whether a runner is installed here, and whether it ships inside a game",
        "A runner node: installed (its build present and ready), and embedded (bundled inside a game's package rather "
        "than a runner package of its own).", { Pos("runner", "the runner's node (CID or LABEL)") },
        { "runners state geproton_10_34" }, { "Installed::RunnerInstalled", "Installed::IsEmbeddedRunner" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "runner"));
            return json{{"runner", N.Key()}, {"installed", Installed::RunnerInstalled(*I, N.Key())},
                        {"embedded", Installed::IsEmbeddedRunner(*I, N.Key())}};
        }, {} });

    Command::Register({
        "catalog fresh", "build a new index of the library as it is on disk (the app's untouched)",
        "Build a node index of the library as it is on disk now — what a download or a check works from — and report its "
        "size, without replacing the app's. When no node file changed since the index cache was written, the build is "
        "that cache (the same index); `catalog cache --check` compares the two.", {}, { "catalog fresh" }, { "PackageCatalog::FreshCatalogIndex", "PackageCatalog::BuildCatalogIndex" },
        Needs::Config, false,
        [](Command::Context &C, const json &) {
            const auto I = PackageCatalog::FreshCatalogIndex(*C.Config);
            return json{{"nodes", I->Nodes.size()}, {"tiles", I->Tiles.size()}, {"refused", I->RefusedNodes.size()}};
        }, {} });
}
