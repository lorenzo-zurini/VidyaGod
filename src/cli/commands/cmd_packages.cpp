// `VidyaGod packages …`, `sources …`, `updates …`, `received …`, `grafts …`, `runners …` — the package catalog's
// functions (packagecatalog.h): what is installed and where, the sources a library syncs from, a peer's packages as
// received and installed, the optional pieces a game takes, and the runners that carry it.
#include "nodegraph.h"
#include "instancestore.h"
#include "installed.h"
#include "shelf.h"
#include "command.h"
#include <chrono>
#include "platform/platform.h"   // the library's write lock
#include "cmdutil.h"
#include "appmodel.h"
#include "peersharing.h"
#include "updateservice.h"
#include <QEventLoop>
#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "nodefiles.h"
#include "ipfswrapper.h"
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
    return {{"package", U.Pkg}, {"library", U.Lib}, {"from", U.Nick.empty() ? U.Peer : U.Nick}, {"version", U.Version}, {"nodes", U.Listing.size()}, {"dir", U.Dir.string()}};
}

PackageCatalog::PackageUpdate FindUpdate(Command::Context &C, const std::string &Q)
{
    for (const auto &U : PackageCatalog::AvailableUpdates(*C.Config))
        if (U.Pkg == Q || U.Version == Q || QString::fromStdString(U.Pkg).contains(QString::fromStdString(Q), Qt::CaseInsensitive)) return U;
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
    Command::RegisterGroup("updates", "newer versions of installed packages, from peers");
    Command::RegisterGroup("received", "a peer's packages as they arrive: planned, landed, adopted");
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
        "or added locally, holds content, its pin folder, its editor-layout key, and the "
        "content CIDs its nodes name.", { Command::MaybeAPath(Pos("package", "a folder path, its name, its UID, or part of its title")) },
        { "packages show 749", "packages show 'Age of Empires II' --json" },
        { "PackageCatalog::ScanBundleIdentity", "PackageCatalog::PackageTitle", "PackageCatalog::FolderTitle", "PackageCatalog::LibraryOf",
          "PackageCatalog::IsLocalPackagePath", "PackageCatalog::IsPackageSourcePath", "PackageCatalog::PackageSourceNameForPath",
          "PackageCatalog::DirHasContent", "PackageCatalog::PackagePin", "PackageCatalog::EditorLayoutKey",
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
                        {"pin_cid", PackageCatalog::PackagePin(*C.Config, D)},
                        {"layout_key", PackageCatalog::EditorLayoutKey(D)}, {"content_cids", Cids},
                        {"seed_targets", Seeds}, {"manifest_targets", Man}};
        }, {} });

    Command::Register({
        "packages new", "create an empty package (a game) or library package",
        "A new, empty package folder in one of your libraries — the package editor's New package. A game is named "
        "\"[<uid>] <title>\"; with --library-package, a library package (dgVoodoo, a runtime) is named by its name.",
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
            std::string Err;
            const bool Ok = PackageCatalog::CollectContentTargets(*I, N.Key(), Grafts(A), T, &Err);
            PackageCatalog::CollectRunnerChainTargets(*I, N.Key(), *C.Config, R);
            json Sizes = json::object();
            for (const auto &[Cid, Sz] : PackageCatalog::NodeContentSizes(*I, N.Key(), Grafts(A))) Sizes[Cid] = Sz;
            return json{{"node", N.Key()}, {"hydrated", Installed::NodeHydrated(*I, N.Key())},
                        {"has_content", Installed::NodeHasContent(*I, N.Key())},
                        {"incomplete", PackageCatalog::NodeClosureIncomplete(*I, N.Key())},
                        {"content_cids", PackageCatalog::NodeContentCids(*I, N.Key(), Grafts(A))}, {"sizes", Sizes},
                        {"targets", Ok ? Targets(T) : json(Err)}, {"runner_targets", Targets(R)}};
        }, {} });

    Command::Register({
        "packages hydrate", "download a game's content in place (blocking)",
        "Fetch every missing content file of a game's closure (its cover, its runner chain's build) where its nodes say "
        "— the one download the Catalog's Download also runs (received packages it reaches are adopted first), "
        "blocking until done.", { GameArg(), Opt("grafts", "the grafts to fetch too (a,b,c)") },
        { "packages hydrate 'Patch 2.0a'" }, { "PackageCatalog::Download" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::string Err;
            if (!PackageCatalog::Download(I, { N.Key() }, {}, Grafts(A), *C.Config, &Err)) throw std::runtime_error(Err);
            if (C.Model) C.Model->rebuildCatalog();   // in the app: what it adopted leaves the Catalog for the Library
            return json{{"hydrated", N.Key()}};
        }, {} });

    Command::Register({
        "packages dehydrate", "delete a game's downloaded content (keep its nodes)",
        "Remove the game's own downloaded content files from disk and stop seeding them — it goes back to the Catalog as "
        "downloadable. Its node files and cover stay; a library its closure contains (other games' too) and a file with "
        "no CID (nothing to download it again from) are left. Only a package in the library: a local package's files are "
        "its masters.", { GameArg() },
        { "packages dehydrate 'Patch 2.0a'" }, { "PackageCatalog::DehydrateNode", "PackageCatalog::SourceOfPackage" },
        Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node *N = NodeOf(*I, Command::Str(A, "game"));
            //As the Library's Remove: only a package one of your sources brings, which it can bring again — never an
            //authored library's (its masters), a local package's, or one kept from a removed source.
            if (PackageCatalog::SourceOfPackage(*C.Config, N->BundleDir).empty())
                throw std::runtime_error(N->BundleDir.string() + " does not come from one of your sources: nothing could download "
                                         "it again (an authored or local package's files are its masters)");
            return json{{"files_removed", PackageCatalog::DehydrateNode(*I, N->Key())}};
        }, {} });

    Command::Register({
        "packages publish", "publish one package: seed its content, re-mint, record its share sheet entries, optionally export it",
        "Share a package of yours: add every content file to IPFS (recording its SOURCE and SIZE), save its nodes "
        "(re-minted: recording a SOURCE changes a node's bytes and so its CID), then publish it into the share sheet: "
        "its nodes and those of the packages it contains are stored, their files named by their labels, their pin "
        "folders made, and their entries put in place of their own (every other package's untouched; a contained "
        "package is redone only when it is stale). With the app up, that last step runs in the app (follow it: events "
        "wait --signal libraryPublished). "
        "It holds the library's write lock (<data dir>/library-write.lock, the one the packaging scripts flock) for the whole "
        "run: with another writer on it, it does nothing and says the library is busy. "
        "With --export, a copy of its node files alone is written there.", { Command::MaybeAPath(Pos("package", "a folder path, its name, its UID, or part of its title")), Command::AsPath(Opt("export", "a folder for the node-files-only copy")) },
        { "packages publish 749", "packages publish 749 --export /tmp/aoe2-nodes" },
        { "PackageCatalog::PublishPackage", "PackageCatalog::ExportDestRefused", "PackageCatalog::SeedNodeContent", "PackageCatalog::SeedWork", "PackageCatalog::StampNodeCids",
          "PackageCatalog::PublishPackages", "AppModel::publishPackage", "PackageCatalog::LibraryWriteLock",
          "Platform::ScopedFileLock::held", "Platform::ScopedFileLock::opened" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto D = PackageDir(C, Command::Str(A, "package"), false);
            std::string Err;
            {   // seeding writes SOURCE and SIZE into its nodes: under the library's lock too, released for the publish below
                const Platform::ScopedFileLock Lock(PackageCatalog::LibraryWriteLock(*C.Config));
                if (!Lock.opened()) throw std::runtime_error("the library's write lock cannot be opened");
                if (!Lock.held()) throw std::runtime_error(PackageCatalog::LibraryBusy);
                //With the library's roots and the instances: a node re-minted here is re-pointed wherever another package or a
                //saved instance names it (without them, those were left naming a CID no file has).
                if (!PackageCatalog::PublishPackage(D.string(), Command::Str(A, "export", ""), &Err, PackageCatalog::EditableRoots(*C.Config),
                                                    InstanceStore::Root(*C.Config).string()))
                    throw std::runtime_error(Err);
            }
            if (C.Model)
            {
                C.Model->publishPackage(QString::fromStdString(D.string()));
                return json{{"published", D.string()}, {"share", "started"}, {"follow", "VidyaGod events wait --signal libraryPublished"}};
            }
            std::string Gaps;
            const auto Out = PackageCatalog::PublishPackages(*C.Config, { D }, &Err, &Gaps);
            if (Out.empty()) throw std::runtime_error(Err);
            Save(C);
            return json{{"published", D.string()}, {"packages", Out.size()}, {"gaps", Gaps}};
        }, {} });

    Command::Register({
        "packages sheet", "the share sheet: each published node and where it lives",
        "What publishing recorded and peers are given: node CID → <library>/<package>/<file>. With a package, its "
        "entries only; with --cid, where that node lives (ours, or a peer's sheet); with --peer, what that peer "
        "shares with us; with --wire, the entries as they travel ({library: [{cid, pkg, node}]}).",
        { Opt("package", "<library>/<package>"), Opt("cid", "a node CID"), Opt("peer", "a peer's peer ID"), Flag("wire", "as it travels") },
        { "packages sheet --package 'VidyaGodLibraries/xinput'", "packages sheet --cid bafkrei…", "packages sheet --peer 12D3KooW… --wire" },
        { "PackageCatalog::SheetEntries", "PackageCatalog::SheetPath", "PackageCatalog::WireEntries", "PackageCatalog::Wire" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            if (const std::string Cid = Command::Str(A, "cid", ""); !Cid.empty())
                return json{{"cid", Cid}, {"path", PackageCatalog::SheetPath(*C.Config, Cid)}};
            const std::string Pkg = Command::Str(A, "package", ""), Peer = Command::Str(A, "peer", "");
            const json Peers = C.Config->value("PeerLibraries", json::object());
            const auto All = Peer.empty() ? PackageCatalog::SheetEntries(C.Config->value("ShareSheet", json::object()))
                                            : PackageCatalog::WireEntries(Peers.value(Peer, json::object()));
            std::vector<PackageCatalog::SheetEntry> Some;
            for (const auto &E : All) if (Pkg.empty() || E.Lib + "/" + E.Pkg == Pkg) Some.push_back(E);
            if (Command::Bool(A, "wire", false)) return PackageCatalog::Wire(Some);
            json Out = json::object();
            for (const auto &E : Some) Out[E.Cid] = E.Lib + "/" + E.Pkg + "/" + E.File;
            return Out;
        }, {} });

    Command::Register({
        "packages node-files", "a package folder's node files, and the names they take",
        "Each node file in a package folder by its node's CID: its file, whether it is the node's canonical form "
        "(frozen) or a draft, and the name the rule gives it (its label made a safe name, bumped with its CID's tail when "
        "names clash; MetaPackageFormat chapter 02 §2.5). With --apply, the frozen files are renamed to it (publishing "
        "does this too).", { Command::MaybeAPath(Pos("package", "a folder path, its name, its UID, or part of its title")), Flag("apply", "rename them") },
        { "packages node-files 749", "packages node-files 'VidyaGodLibraries/xinput' --apply" },
        { "NodeFiles::In", "NodeFiles::Names", "NodeFiles::Normalize", "NodeFiles::ParkedName", "NodeFiles::SafeName", "ContentFile::ClaimsOfNode" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            const auto D = PackageDir(C, Command::Str(A, "package"));
            json Moved = json::array();
            std::string Err;
            if (Command::Bool(A, "apply", false))
            {
                std::vector<std::pair<std::filesystem::path, std::filesystem::path>> M;
                const bool Ok = NodeFiles::Normalize(D, &M, &Err);
                std::vector<std::pair<std::string, std::string>> Refs;
                for (const auto &[F, T] : M) { Refs.push_back({F.string(), T.string()}); Moved.push_back({F.filename().string(), T.filename().string()}); }
                std::string MErr;
                if (!Refs.empty() && IpfsWrapper::MoveRefs(Refs, &MErr) < 0) Err += (Err.empty() ? "" : "; ") + MErr;
                if (!Ok && Err.empty()) Err = "not every file could be renamed";
            }
            const auto Files = NodeFiles::In(D);
            std::map<std::string, nlohmann::ordered_json> Package;
            for (const auto &[Cid, F] : Files) Package.emplace(Cid, F.Node);
            const auto Names = NodeFiles::Names(Package);
            json Out = json::object();
            for (const auto &[Cid, F] : Files)
                Out[Cid] = {{"file", F.Path.filename().string()}, {"frozen", F.Frozen}, {"name", Names.at(Cid)},
                            {"label", NodeFiles::SafeName(F.Node.value("LABEL", std::string()))}};
            return json{{"dir", D.string()}, {"nodes", Out}, {"renamed", Moved}, {"error", Err}};
        }, {} });

    Command::Register({
        "packages pins", "the pin folders a pinning service needs for some packages",
        "Each named package's pin folder (its node files and its content) and those of every package it contains, "
        "transitively — what to pin so the packages are whole on a pinning service. Packages are <library>/<package>.",
        { Pos("packages", "<library>/<package>, comma-separated") },
        { "packages pins 'VidyaGod/[749] Age of Empires II'" },
        { "PackageCatalog::PackagesContained", "PackageCatalog::NodeContains", "PackageCatalog::CurrentIndex" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::vector<std::string> Named;
            const QStringList Parts = QString::fromStdString(Command::Str(A, "packages")).split(',', Qt::SkipEmptyParts);
            for (const QString &P : Parts) Named.push_back(P.trimmed().toStdString());
            const std::shared_ptr<const NodeIndex> Idx = C.Model ? C.Model->catalogSnapshot() : PackageCatalog::CurrentIndex(*C.Config);
            const json Pins = C.Config->value("Pins", json::object());
            json Out = json::array();
            for (const std::string &K : PackageCatalog::PackagesContained(C.Config->value("ShareSheet", json::object()), Named,
                     [&](const std::string &Cid) { return PackageCatalog::NodeContains(*Idx, Cid); }))
                Out.push_back({{"package", K}, {"pin", Pins.value(K, std::string())}});
            return Out;
        }, {} });

    Command::Register({
        "packages publish-all", "republish the whole library (Verify & Publish)",
        "Freeze every node of the library, store it, name every node file by its label, make each package's pin folder, "
        "and record the share sheet — the Network tab's Verify & Publish. What did not publish whole (nodes that did not freeze, "
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
            for (const auto &E : PackageCatalog::SheetEntries((*C.Config)["ShareSheet"]))
                Shared[E.Lib] = Shared.value(E.Lib, 0) + 1;
            return json{{"packages", Out.size()}, {"nodes", Shared}, {"gaps", Gaps}};
        }, {} });

    Command::Register({
        "packages meta-cid", "the CID a folder's node files would publish as",
        "Build (and store) the node-files-only folder of a package source tree and print its CID — what a source "
        "upgrade or a peer would fetch.", { Command::AsPath(Pos("dir", "the folder")) }, { "packages meta-cid ~/.VidyaGod/LIBRARY/VidyaGod" },
        { "PackageCatalog::PublishMetaCid", "PackageCatalog::MirrorDehydrated" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            //Re-minting re-points the library's other packages and the saved instances: under the library's write lock.
            const Platform::ScopedFileLock Lock(PackageCatalog::LibraryWriteLock(*C.Config));
            if (!Lock.opened()) throw std::runtime_error("the library's write lock cannot be opened");
            if (!Lock.held()) throw std::runtime_error(PackageCatalog::LibraryBusy);
            const std::string Cid = PackageCatalog::PublishMetaCid(Command::Str(A, "dir"), &Err, PackageCatalog::EditableRoots(*C.Config),
                                                                   InstanceStore::Root(*C.Config).string());
            if (Cid.empty()) throw std::runtime_error(Err);
            return json{{"cid", Cid}};
        }, {} });

    Command::Register({
        "packages mirror", "copy a tree's node files (and covers) only",
        "Copy the node files of a package tree — no content — into another folder: a dehydrated mirror.",
        { Command::AsPath(Pos("from", "the source folder")), Command::AsPath(Pos("to", "the destination folder")) }, { "packages mirror ~/.VidyaGod/LIBRARY/VidyaGod /tmp/nodes" },
        { "PackageCatalog::MirrorDehydrated" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            return json{{"files", PackageCatalog::MirrorDehydrated(Command::Str(A, "from"), Command::Str(A, "to"))}};
        }, {} });

    Command::Register({
        "packages seed", "add a folder's content to IPFS (serve it), or report what is not held",
        "Seed every file a folder's nodes name: added in place (no copy), so this machine serves it. --verify reads "
        "each back (catches a file whose bytes changed); --unheld only re-adds what the store does not hold; "
        "--covers-only just the covers.",
        { Command::AsPath(Pos("dir", "the folder (a package or a library)")), Flag("verify", "read every file back"), Flag("unheld", "only what is not held"),
          Flag("covers-only", "only the covers"), Flag("overwrite", "re-add even what is held") },
        { "packages seed ~/.VidyaGod/LIBRARY/VidyaGod/'[749] Age of Empires II'", "packages seed ~/.VidyaGod/LIBRARY --unheld" },
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
        }, {}, true });

    Command::Register({
        "packages held", "whether a folder's content is held whole here, checked together",
        "Every content file a folder's nodes name, asked at once (a worker per CPU, each backing file stat'd once): how "
        "many are held whole, the ones that are not, and how long the check took — what every start asks.",
        { Command::AsPath(Pos("dir", "the folder (a package or a library)")) },
        { "packages held ~/.VidyaGod/LIBRARY" },
        { "IpfsWrapper::HeldWholeMany" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const auto Targets = PackageCatalog::SeedTargets(Command::Str(A, "dir"));
            std::vector<std::string> Cids;
            for (const auto &[Path, Cid] : Targets) Cids.push_back(Cid);
            const auto T0 = std::chrono::steady_clock::now();
            const std::vector<bool> Whole = IpfsWrapper::HeldWholeMany(Cids);
            const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count();
            json Not = json::array();
            size_t At = 0, N = 0;
            for (const auto &[Path, Cid] : Targets) { if (Whole[At++]) ++N; else Not.push_back({{"path", Path}, {"cid", Cid}}); }
            return json{{"files", Targets.size()}, {"whole", N}, {"not_whole", Not}, {"ms", Ms}};
        }, {}, true });

    Command::Register({
        "packages seed-roots", "every folder whose content this machine serves",
        "Its own library and each package source: what the start-up seeding walks.", {}, { "packages seed-roots" },
        { "PackageCatalog::SeedRoots" }, Needs::Config, false,
        [](Command::Context &C, const json &) { return Strings(PackageCatalog::SeedRoots(*C.Config)); }, {} });

    Command::Register({
        "packages heal", "re-point content references that broke (moved or changed files)",
        "Repair this machine's serving of the library: re-add files whose references were lost, re-seed changed bytes, "
        "drop references to what is gone. --deep reads every reference back from the store.",
        { Flag("deep", "verify every reference's bytes (slow)"), Flag("prune-unreferenced", "also unpin healthy pins nothing references (the Downloads tab's \"unknown\")") }, { "packages heal", "packages heal --deep" },
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
        }, {}, true });

    Command::Register({
        "packages stamp-cids", "write minted CIDs back into a working tree",
        "Rename each node file in a tree to the CID a mint gave it and re-point its references — the write-back after a "
        "publish. --map is a JSON object {handle: cid}.",
        { Command::AsPath(Pos("root", "the tree")), WithSample(Opt("map", "{handle: cid} as JSON", Type::Json, true), "{\"draft-1\":\"bafkrei…\"}") }, { "packages stamp-cids ~/.VidyaGod/LIBRARY --map '{}'" },
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
                QString Why;
                if (!C.Model->addPackageSource(QString::fromStdString(Cid), QString::fromStdString(Name), &Why)) throw std::runtime_error(Why.toStdString());
                return json{{"added", Name}, {"syncing", true}};
            }
            std::string Why;
            if (!PackageCatalog::AddPackageSource(*C.Config, Cid, Name, &Why)) throw std::runtime_error(Why);
            Save(C);
            return json{{"added", Name}};
        }, {} });

    Command::Register({
        "sources remove", "remove a package source (its entries and fetched folder go; what you installed from it stays)",
        "Drop a source: its config entry, its library entries and its fetched folder — packages you installed from it stay, "
        "as local packages, unless --delete-installed. A folder the source's own fetch did not make is left in place.",
        { Pos("source", "its name or CID"), Flag("delete-installed", "delete what was installed from it too") },
        { "sources remove 'Some library'" }, { "AppModel::removePackageSource", "PackageCatalog::RemovePackageSource" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const int Index = static_cast<int>(SourceIndex(C, Command::Str(A, "source")));
            const bool Keep = !Command::Bool(A, "delete-installed", false);
            if (C.Model) C.Model->removePackageSource(Index, Keep);
            else { PackageCatalog::RemovePackageSource(*C.Config, Index, Keep); Save(C); }
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
        "updates list", "newer versions of installed packages that peers share",
        "For each package you installed from a peer, a newer version they now share — its package, library, peer "
        "and folder CID.", {}, { "updates list" }, { "PackageCatalog::AvailableUpdates" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            json Out = json::array();
            for (const auto &U : PackageCatalog::AvailableUpdates(*C.Config)) Out.push_back(Update(U));
            return Out;
        }, {} });

    Command::Register({
        "updates apply", "switch an installed package to the newer version (shows what changes)",
        "Stage the newer version beside the installed one and list the files it adds, removes and keeps; --yes switches "
        "to it. Without --yes nothing changes and the stage is dropped (--discard says so explicitly).",
        { Pos("package", "the package (its name or the update's folder CID)"), Flag("yes", "switch to it"), Flag("discard", "discard the stage instead") },
        { "updates apply 'Age of Empires II'", "updates apply 'Age of Empires II' --yes" },
        { "PackageCatalog::StageUpdate", "PackageCatalog::ApplyStagedUpdate", "PackageCatalog::DiscardStagedUpdate", "PackageCatalog::KeptNodes" }, Needs::Config, true,
        [](Command::Context &C, const json &A) {
            PackageCatalog::StagedUpdate S;
            std::string Err;
            if (!PackageCatalog::StageUpdate(*C.Config, FindUpdate(C, Command::Str(A, "package")), &S, &Err)) throw std::runtime_error(Err);
            json Out = Staged(S);
            //Only shown (no --yes): the stage goes with the answer — the next call stages again (its nodes are here by
            //then). Refused: the stage goes too, unless the switch began (the next start finishes it).
            if (!Command::Bool(A, "yes", false)) Out["discarded"] = PackageCatalog::DiscardStagedUpdate(S);
            else
            {
                if (!PackageCatalog::ApplyStagedUpdate(*C.Config, S, &Err)) { PackageCatalog::DiscardStagedUpdate(S); throw std::runtime_error(Err); }
                if (C.Model) C.Model->rebuildCatalog();
                Out["applied"] = true;
            }
            return Out;
        }, {} });

    Command::Register({
        "updates switch", "switch an installed package to a peer's version as Settings does: off the GUI thread, with progress",
        "The switch Settings → Installed Packages makes (UpdateService): the version's node files are fetched into its stage, "
        "the plan is made, then — with --yes — applied, and the library read again off the GUI thread. Without --yes the plan "
        "is declined and its stage dropped. In the running app it begins and returns (follow it with events wait --signal "
        "finished --object UpdateService); here it waits. --cancel stops the switch under way.",
        { Command::OptPos("package", "the package (its name or the update's folder CID)"), Flag("yes", "switch to it"),
          Flag("cancel", "stop the switch under way") },
        { "updates switch 'Jet Set Radio Future' --yes", "updates switch --cancel" },
        { "AppModel::updates", "UpdateService::start", "UpdateService::busy", "UpdateService::current", "UpdateService::cancel", "UpdateService::started" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            UpdateService &Svc = C.Model->updates();
            if (Command::Bool(A, "cancel", false))
            {
                const bool Was = Svc.busy();
                const std::string Pkg = Svc.current().toStdString();
                Svc.cancel();
                return json{{"cancelled", Was}, {"package", Pkg}};
            }
            if (Svc.busy()) throw std::runtime_error("a switch of " + Svc.current().toStdString() + " is under way");
            const PackageCatalog::PackageUpdate U = FindUpdate(C, Command::Str(A, "package"));
            const bool Yes = Command::Bool(A, "yes", false);
            auto Plan = std::make_shared<json>();
            Svc.start(U, [Yes, Plan](const PackageCatalog::StagedUpdate &S, std::function<void(bool)> Answer) {
                *Plan = Staged(S);
                Answer(Yes);
            });
            if (C.Live) return json{{"switching", Svc.busy()}, {"package", U.Pkg}, {"follow", "VidyaGod events wait --signal finished --object UpdateService"}};
            json Out;
            QEventLoop Wait;
            QObject::connect(&Svc, &UpdateService::finished, &Wait, [&](UpdateService::Outcome O, const QString &R) {
                static const char *Names[] = { "done", "failed", "declined", "cancelled" };
                Out = json{{"outcome", Names[static_cast<int>(O)]}, {"result", R.toStdString()}, {"plan", *Plan}};
                Wait.quit();
            });
            if (Svc.busy()) Wait.exec();
            if (Out.value("outcome", std::string()) == "failed") throw Command::CheckFailed(Out, Out.value("result", std::string()));
            return Out;
        }, {} });

    Command::Register({
        "updates sweep", "what a start does with UPDATING: finish a switch a crash cut short, drop every other stage",
        "A switch writes its plan into its stage before the first change; one cut short (a crash, a full disk) is finished "
        "as confirmed. Any other stage is a fetch nothing will apply: it goes, with the folders it leaves empty — UPDATING "
        "itself too. The app does this once the node is up.",
        {}, { "updates sweep" }, { "PackageCatalog::SweepUpdateStages", "PackageCatalog::UpdatesRoot" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            std::vector<std::string> Errors;
            const auto Done = PackageCatalog::SweepUpdateStages(*C.Config, &Errors);
            if (C.Model && !Done.empty()) C.Model->rebuildCatalog();
            return json{{"finished", Done}, {"errors", Errors}, {"updating", PackageCatalog::UpdatesRoot(*C.Config).string()},
                        {"left", std::filesystem::exists(PackageCatalog::UpdatesRoot(*C.Config))}};
        }, {} });

    // ---------------- received ----------------
    Command::Register({
        "received plan", "where a peer's shared packages would land",
        "The folders a peer's share snapshot lands into (CATALOG/<nick> - <library>/<package>) and the fetch each is, "
        "and whether any received package is still incomplete.", { Pos("peer", "the peer's peer id (VidyaGod peers list)") },
        { "received plan 12D3KooW9z4dCR2LVkuc4xb4zTrss5AR9N5WcAAYdJSDat1VCfAR" },
        { "PackageCatalog::PlanReceivedFetches", "PackageCatalog::ReceivedFolderNames", "PackageCatalog::ReceivedFetchTargets", "PackageCatalog::ReceivedNickLabel", "PackageCatalog::AssignReceivedLabel", "PackageCatalog::ReceivedLiveNick",
          "PackageCatalog::ReceivedPackagesIncomplete" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string P = Command::Str(A, "peer");
            const json &Fl = C.Config->value("PeerLibraries", json::object());
            if (!Fl.contains(P)) return json{{"peer", P}, {"held", false}};
            const auto Plan = PackageCatalog::PlanReceivedFetches(*C.Config, PackageCatalog::ReceivedNickLabel(*C.Config, P), Fl[P]);
            json Rows = json::array();
            for (const auto &R : Plan) Rows.push_back({{"cid", R.Cid}, {"to", R.Dest}});
            return json{{"peer", P}, {"label", PackageCatalog::ReceivedNickLabel(*C.Config, P)}, {"fetches", Rows},
                        {"targets", Targets(PackageCatalog::ReceivedFetchTargets(Plan))},
                        {"incomplete", PackageCatalog::ReceivedPackagesIncomplete(*C.Config)}};
        }, {} });

    Command::Register({
        "received land", "land every received package's node files now",
        "Fetch each package folder peers share with you (node files only — content stays lazy until install). With "
        "the app up it lands in the app.", {}, { "received land" },
        { "PackageCatalog::LandReceivedPackages", "PeerSharing::completeReceivedClosures", "IpfsWrapper::WaitBatchWhileMoving" }, Needs::Config, true,
        [](Command::Context &C, const json &) {
            if (C.Model) { C.Model->peers().completeReceivedClosures(); return json{{"started", true}}; }
            std::string Err;
            if (!PackageCatalog::LandReceivedPackages(*C.Config, &Err)) throw std::runtime_error(Err);
            return json{{"landed", true}};
        }, {} });

    Command::Register({
        "received prune", "drop received node files no share lists any more",
        "Remove the node files of older generations of peers' packages (an installed package keeps its content).", {},
        { "received prune" }, { "PackageCatalog::PruneStaleReceived" }, Needs::Config, false,
        [](Command::Context &C, const json &) { return json{{"pruned", PackageCatalog::PruneStaleReceived(*Index(C), *C.Config)}}; }, {} });

    Command::Register({
        "received adopt", "install a received package: move it into your library",
        "What a download does first: move a peer's package folder out of CATALOG into LIBRARY/<library>/<package>, "
        "where it is an ordinary package of yours. --for lists every package a game's download would adopt instead.",
        { Command::AsPath(Opt("package", "the received package folder")), Opt("for", "a game's node: list what its download adopts") },
        { "received adopt --for 'Patch 2.0a'", "received adopt --package 'Age of Empires II'" },
        { "PackageCatalog::PackagesToAdopt", "PackageCatalog::AdoptReceivedPackage", "PackageCatalog::AdoptReceivedPackages" }, Needs::Config, false,
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
        "grafts tick", "what a choice becomes when one part is ticked",
        "The engine's rules for a choice of parts (a set: parts have no order): ticking G adds it and unticks what it "
        "excludes or what excludes it (a condition naming the other). Prints the new choice; `packages settings` saves it.",
        { GameArg(), Opt("ticked", "the current choice (a,b,c)", Type::String, true), Opt("graft", "the part to tick") },
        { "grafts tick 'Patch 1.0e (Unofficial)' --ticked 'UserPatch 1.5' --graft 'Forgotten Empires 2.2'" },
        { "Shelf::TickGraft" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::vector<std::string> T = List(A, "ticked"), Unticked;
            if (A.contains("graft")) T = Shelf::TickGraft(*I, N.Key(), T, Command::Str(A, "graft"), &Unticked);
            return json{{"ticked", T}, {"unticked", Unticked}};
        }, {} });

    Command::Register({
        "grafts conflicts", "where ticked parts write one thing differently (a file, a key, a value)",
        "The parts the choice applies, and every unit two of them write differently (MetaPackageFormat 23 §23.4): a file "
        "path (compared without case), a keyed edit (file#key), a registry value, an environment variable, a DLL, a "
        "variable. Identical writes do not conflict, nor do a part and one it goes into.",
        { GameArg(), Opt("ticked", "the choice (a,b,c: CIDs or LABELs)", Type::String, true), Opt("instance", "whose RESOLUTIONS (default the active one)") },
        { "grafts conflicts 'Wipeout XL' --ticked 'Wipeout XL 200 fps,Wipeout XL widescreen'" },
        { "Shelf::PartConflicts", "Shelf::OrderResolved", "Shelf::ResolutionFor", "Shelf::ResolutionHolds" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            std::vector<std::string> T;
            for (const std::string &G : List(A, "ticked")) T.push_back(NodeOf(*I, G)->Key());
            const json Inst = InstanceStore::ReadConfig(*C.Config, N.PackageUid, Command::Str(A, "instance", ""));
            const json Res = Inst.contains("RESOLUTIONS") && Inst["RESOLUTIONS"].is_object() ? Inst["RESOLUTIONS"] : json::object();
            const auto Applied = Shelf::OrderResolved(*I, Shelf::AppliedGrafts(*I, N.Key(), T), Res);
            json Out = json::array();
            const auto All = Shelf::PartConflicts(*I, N.Key(), Applied);
            for (const auto &X : All)
            {
                const std::string Kept = Shelf::ResolutionFor(Res, X.Address);
                Out.push_back({{"address", X.Address}, {"a", X.A}, {"b", X.B}, {"kept", Kept}, {"holds", !Kept.empty() && Shelf::ResolutionHolds(Applied, X, Kept, All)}});
            }
            return json{{"applied", Applied}, {"conflicts", Out}};
        }, {} });

    Command::Register({
        "grafts resolve", "settle a conflict between two parts: which one's write stands (kept in the instance)",
        "Kept in the instance's RESOLUTIONS ({address: the winning part}): at launch the winner folds after the other parts "
        "at its depth, so its write stands. --clear forgets the resolution for that address. A published resolution is a "
        "part that goes into both (MetaPackageFormat 23 §23.4).",
        { GameArg(), Opt("address", "the conflict's address (from grafts conflicts)", Type::String, true), Opt("keep", "the part whose write stands (CID or LABEL)"),
          Flag("clear", "forget this address's resolution"), Opt("instance", "which instance (default the active one)") },
        { "grafts resolve 'Wipeout XL' --address ENV/WOXL_FPS --keep 'Wipeout XL 200 fps'" },
        { "Shelf::OrderResolved", "InstanceStore::ReadConfig", "InstanceStore::SetSetting" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "game"));
            const std::string Inst = Command::Str(A, "instance", "");
            const json Cfg = InstanceStore::ReadConfig(*C.Config, N.PackageUid, Inst);
            json R = Cfg.contains("RESOLUTIONS") && Cfg["RESOLUTIONS"].is_object() ? Cfg["RESOLUTIONS"] : json::object();
            const std::string Addr = Command::Str(A, "address");
            if (Command::Bool(A, "clear", false)) R.erase(Addr);
            else
            {
                if (!A.contains("keep")) throw Command::BadArgs("--keep the part whose write stands, or --clear");
                R[Addr] = NodeOf(*I, Command::Str(A, "keep"))->Key();
            }
            InstanceStore::SetSetting(*C.Config, N.PackageUid, "RESOLUTIONS", R, Inst);
            return json{{"resolutions", R}};
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
        { Pos("platform", "the input platform (win32, dos, n64, …)"), Command::AsPath(Opt("package", "the game's package folder (for \"custom\")")) },
        { "runners candidates win32", "runners candidates custom --package 749" }, { "Installed::CandidateRunners" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::filesystem::path P = A.contains("package") ? PackageDir(C, Command::Str(A, "package")) : std::filesystem::path();
            return Runners(Installed::CandidateRunners(*Index(C), Command::Str(A, "platform"), P));
        }, {} });

    Command::Register({
        "runners state", "whether a runner is installed here, and whether it ships inside a game",
        "A runner node: installed (its build present and ready), and embedded (bundled inside a game's package rather "
        "than a runner package of its own).", { Pos("runner", "the runner's node (CID or LABEL)") },
        { "runners state geproton_10_34" }, { "Installed::RunnerInstalled", "Installed::IsEmbeddedRunner", "Installed::EmbeddedRunners" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "runner"));
            return json{{"runner", N.Key()}, {"installed", Installed::RunnerInstalled(*I, N.Key())},
                        {"embedded", Installed::IsEmbeddedRunner(*I, N.Key())},
                        {"embedded_runners", Installed::EmbeddedRunners(*I).size()}};
        }, {} });
}
