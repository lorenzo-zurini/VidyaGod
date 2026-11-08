// `VidyaGod catalog …` — the node graph the app holds: the shelf of games (each edition's install state, what it would
// fetch), one node in full, the re-index and the validator. What the Library and Catalog tabs are built from.
#include <regex>
#include <sstream>
#include <map>
#include "cli/cliapp.h"   // RenderGeneric
#include "installed.h"
#include "shelf.h"
#include "validate.h"
#include "command.h"
#include "appmodel.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "cmdutil.h"

#include <chrono>
#include <fstream>

namespace
{

using json = nlohmann::ordered_json;

json Hydration(const Installed::NodeHydration &H)
{
    return {{"installed", H.Hydrated && H.OnDisk}, {"hydrated", H.Hydrated}, {"on_disk", H.OnDisk}, {"has_content", H.HasContent},
            {"fetchable", H.HasCid}, {"incomplete", H.Incomplete}};
}

// Where a game stands, in one word: installed (playable here), downloadable (not here, can be fetched), waiting (a
// package it contains has not landed), unavailable (neither), empty (no content at all).
std::string Standing(const Installed::NodeHydration &H)
{
    if (!H.HasContent) return "empty";
    if (H.Hydrated && H.OnDisk) return "installed";
    if (H.Incomplete) return "waiting";
    return H.HasCid ? "downloadable" : "unavailable";
}

const Node *FindNode(const NodeIndex &Idx, const std::string &Id)
{
    if (const Node *N = Idx.Find(Id)) return N;
    throw Command::BadArgs("no node '" + Id + "' (a CID, a working handle, or a LABEL)");
}

} // namespace

namespace {
// A validation's findings for a person: one line per kind of finding. Ten warnings that differ only in the variable
// they name ("runners disagree where %AppData% is …", "… %Documents% …") are one line with a count and the names;
// -v (and --json) list each. Errors before warnings.
std::string RenderFindings(const json &Rows, bool Colour)
{
    if (!Rows.is_array()) return CliApp::RenderGeneric(Rows, Colour);
    if (Rows.empty()) return std::string(Colour ? "\033[32m" : "") + "no problems" + (Colour ? "\033[0m" : "") + "\n";
    static const std::regex Token("%[A-Za-z0-9_]+%"), Quoted("'[^']*'");
    struct Kind { std::string Severity, First; std::vector<std::string> Tokens; size_t Count = 0; };
    std::vector<Kind> Kinds;
    std::map<std::string, size_t> At;
    for (const auto &R : Rows)
    {
        const std::string Sev = R.value("severity", std::string()), Msg = R.value("message", std::string());
        const std::string Shape = Sev + "\t" + std::regex_replace(std::regex_replace(Msg, Quoted, "'…'"), Token, "%…%");
        auto It = At.find(Shape);
        if (It == At.end() || Command::VerboseOutput()) { It = At.emplace(Shape + (Command::VerboseOutput() ? std::to_string(Kinds.size()) : ""), Kinds.size()).first; Kinds.push_back({Sev, Msg, {}, 0}); }
        Kind &K = Kinds[It->second];
        ++K.Count;
        if (std::smatch M; std::regex_search(Msg, M, Token) && std::find(K.Tokens.begin(), K.Tokens.end(), M.str()) == K.Tokens.end()) K.Tokens.push_back(M.str());
    }
    std::stable_sort(Kinds.begin(), Kinds.end(), [](const Kind &A, const Kind &B) { return A.Severity == "error" && B.Severity != "error"; });
    std::ostringstream O;
    size_t Errors = 0, Warnings = 0;
    for (const auto &K : Kinds)
    {
        const bool Err = K.Severity == "error";
        (Err ? Errors : Warnings) += K.Count;
        const std::string Label = Err ? "error  " : "warning";
        O << (Colour ? (Err ? "\033[31m" : "\033[33m") : "") << Label << (Colour ? "\033[0m" : "") << "  " << K.First << "\n";
        if (K.Count > 1)
        {
            std::string Names;
            for (size_t I = 1; I < K.Tokens.size(); ++I) Names += (I > 1 ? ", " : "") + K.Tokens[I];
            O << "         " << (Colour ? "\033[2m" : "") << "… and " << K.Count - 1 << " more like it"
              << (Names.empty() ? "" : ", for " + Names) << " (-v lists each)" << (Colour ? "\033[0m" : "") << "\n";
        }
    }
    O << (Colour ? "\033[2m" : "") << Errors << (Errors == 1 ? " error, " : " errors, ") << Warnings << (Warnings == 1 ? " warning" : " warnings")
      << (Colour ? "\033[0m" : "") << "\n";
    return O.str();
}
} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    Command::RegisterGroup("catalog", "the games and nodes the app holds: the shelf, one node, re-index, validate");

    Command::Register({
        "catalog shelf", "every game on the shelf, with each edition's state",
        "One row per tile — the games the Library and Catalog tabs are built from — with its family, its editions "
        "(variants) and where each stands: installed, downloadable, waiting (a package it contains has not landed), "
        "unavailable (nothing to fetch it from), or empty.",
        { Command::Opt("state", "only games with an edition in this state", Type::String, false,
                       {"installed", "downloadable", "waiting", "unavailable", "empty"}),
          Command::Opt("search", "only titles containing this") },
        { "catalog shelf", "catalog shelf --state downloadable", "catalog shelf --search 'Age of' --json" },
        { "Shelf::Tiles", "Installed::HydrationMap", "Installed::ForEachContentLayer", "AppModel::catalogSnapshot" , "PackageMainCard", "ManifestModel::AllGraftBases" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = C.Model->catalogSnapshot();
            const auto Hyd = Installed::HydrationMap(*Snap);
            const std::string Only = Command::Str(A, "state", ""), Search = Command::Str(A, "search", "");
            json Out = json::array();
            for (const auto &T : Shelf::Tiles(*Snap))
            {
                const std::string Title = T.Tile.value("TITLE", T.Uid);
                if (!Search.empty() && !QString::fromStdString(Title).contains(QString::fromStdString(Search), Qt::CaseInsensitive)) continue;
                json Eds = json::array();
                bool Match = Only.empty();
                for (const Node *N : T.Rows)
                {
                    const auto H = Hyd.find(N->Key());
                    const std::string S = H == Hyd.end() ? "unknown" : Standing(H->second);
                    Match = Match || S == Only;
                    Eds.push_back(N->Variant.empty() ? N->NodeId + " (" + S + ")" : N->Variant + " (" + S + ")");
                }
                if (!Match) continue;
                json Row = {{"tile", T.Uid}, {"title", Title}, {"in", T.Tile.value("PARENTUID", std::string())}, {"editions", Eds}};
                if (!T.Graft.empty()) Row["graft"] = T.Graft;
                if (!T.Rows.empty() && T.Rows.front()->Received) Row["received"] = true;
                Out.push_back(std::move(Row));
            }
            return Out;
        }, {} });

    Command::Register({
        "catalog node", "one node in full: what it is, holds, and where it stands",
        "A node by CID, working handle or LABEL: its identity, package folder, variant, what it contains and requires, its "
        "tile, its default entry, its install state — and with --json-node the node as authored.",
        { Command::Pos("node", "a CID, working handle, or LABEL"), Command::Flag("json-node", "include the node as authored") },
        { "catalog node 'Patch 2.0a'", "catalog node 'The Age of Kings' --json-node" },
        { "NodeIndex::Find", "Installed::HydrationMap", "PackageCatalog::MissingFromClosure" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = C.Model->catalogSnapshot();
            const Node &N = *FindNode(*Snap, Command::Str(A, "node"));
            const auto Hyd = Installed::HydrationMap(*Snap);
            json J = {{"cid", N.Cid}, {"label", N.NodeId}, {"variant", N.Variant}, {"package", N.BundleDir.string()},
                      {"received", N.Received}, {"graft", N.IsGraft}, {"runner", N.HasRunner}, {"contains", N.Refs},
                      {"requires", N.Requires}, {"tiles", N.Faces}, {"family", N.PackageUid}, {"recommended", N.Recommended}};
            if (!N.Label.empty()) J["entry"] = N.Label;
            if (!N.LowerError.empty()) J["error"] = N.LowerError;
            if (const auto H = Hyd.find(N.Key()); H != Hyd.end()) { J["state"] = Standing(H->second); J["hydration"] = Hydration(H->second); }
            if (const auto M = PackageCatalog::MissingFromClosure(*Snap, N.Key()); !M.empty()) J["missing"] = M;
            if (Command::Bool(A, "json-node", false)) J["node"] = *N.Json;
            return J;
        }, {} });

    Command::Register({
        "catalog rebuild", "re-scan the library from disk",
        "Re-index every package folder (LIBRARY and received CATALOG) into the node graph; every tab refreshes from it.", {},
        { "catalog rebuild" }, { "AppModel::rebuildCatalog", "PackageCatalog::BuildCatalogIndex", "PackageCatalog::PublishAppIndex", "Platform::ReleaseFreedMemory", "LibraryTab::rebuild", "CatalogTab::rebuild", "RunnersPage::rebuild", "PackagesView::rebuildList", "LibraryView::setCards", "LibraryView::setGroups", "LibraryView::setEmptyMessage", "LibraryView::refreshVisuals", "LibraryView::repaintCards", "LibraryView::setHoverAction", "AppModel::catalogIndex", "AppModel::hydration" }, Needs::Model, false,
        [](Command::Context &C, const json &) {
            C.Model->rebuildCatalog();
            return json{{"nodes", C.Model->catalogIndex().Nodes.size()}, {"tiles", C.Model->catalogIndex().Tiles.size()}};
        }, {} });

    Command::Register({
        "catalog cache", "the on-disk index cache: whether it matches the library now; check it, or clear it",
        "Every index build is kept at <data dir>/CACHE/index.cbor with what it was built from (every node file's size and "
        "time, this program); a build whose inputs are unchanged loads it instead of reading the files. --check builds the "
        "index from the files and compares it with the cached one, node by node and field by field (it writes nothing); "
        "--clear deletes the cache (the next build makes it again).",
        { Command::Flag("check", "compare the cached index with one built from the files"), Command::Flag("clear", "delete the cache") },
        { "catalog cache", "catalog cache --check", "catalog cache --clear" },
        { "PackageCatalog::IndexCacheStatus", "PackageCatalog::ClearIndexCache", "Platform::FileStamp", "PackageCatalog::ScanCatalogIndex",
          "PackageCatalog::CurrentIndex", "PackageCatalog::ReadIndexCache", "ManifestModel::NodeToJson", "ManifestModel::NodeFromJson", "ManifestModel::IndexTablesToJson",
          "ManifestModel::IndexTablesFromJson", "NodeGraph::ListTreeFiles", "NodeGraph::TopoLevels", "Node::LoweredLayers" },
        Needs::Config, false,
        [](Command::Context &C, const json &A) {
            json Out = json::object();
            if (Command::Bool(A, "clear", false)) Out["cleared"] = PackageCatalog::ClearIndexCache();
            const auto St = PackageCatalog::IndexCacheStatus(*C.Config);
            Out["path"] = St.Path; Out["exists"] = St.Exists; Out["bytes"] = St.Bytes; Out["current"] = St.Current; Out["files"] = St.Files;
            if (Command::Bool(A, "check", false))
            {
                if (!St.Current) throw std::runtime_error("the cache does not match the library now: nothing to check (the next build writes it)");
                const std::optional<NodeIndex> CachedIdx = PackageCatalog::ReadIndexCache(*C.Config);   // the file itself
                if (!CachedIdx) throw std::runtime_error("the cache could not be read");
                const NodeIndex *Cached = &*CachedIdx;
                const NodeIndex Fresh = PackageCatalog::ScanCatalogIndex(*C.Config);
                json Differ = json::array();
                for (const auto &[Id, N] : Fresh.Nodes)
                {
                    const Node *M = Cached->Find(Id);
                    if (!M) { Differ.push_back({{"node", Id}, {"why", "missing from the cache"}}); continue; }
                    const json F = ManifestModel::NodeToJson(N), G = ManifestModel::NodeToJson(*M);
                    if (F != G)
                        for (const auto &[K, V] : F.items())
                            if (V != G[K]) { Differ.push_back({{"node", Id}, {"why", "field " + K + " differs"}}); break; }
                    if (*N.Json != *M->Json || N.LoweredLayers() != M->LoweredLayers()) Differ.push_back({{"node", Id}, {"why", "lowered layers differ"}});
                }
                if (Cached->Nodes.size() != Fresh.Nodes.size()) Differ.push_back({{"why", "node counts differ"}});
                if (ManifestModel::IndexTablesToJson(*Cached) != ManifestModel::IndexTablesToJson(Fresh)) Differ.push_back({{"why", "tiles, duplicates or refused differ"}});
                Out["checked"] = Fresh.Nodes.size();
                Out["differences"] = Differ;
                if (!Differ.empty()) throw std::runtime_error(std::to_string(Differ.size()) + " difference(s) between the cache and the files: "
                                                              + Differ.front().dump() + " (catalog cache --clear rebuilds it)");
            }
            return Out;
        }, {} });

    Command::Register({
        "catalog stats", "what the node index holds and weighs: nodes, tiles, and the JSON each part of a node keeps",
        "Counts, and per part of a node (its JSON, its content layers, entries, default entry, tile) the size of that "
        "part written out as JSON, summed over the library — what the index keeps in memory, roughly in proportion — and "
        "what its other lowered layers would weigh (made on demand, not kept). --build also times a build from the files (never the cache); "
        "--dump writes every node, field by field, so two builds of one library compare with diff.",
        { Command::Flag("build", "time a build from the files (never the cache)"), Command::Opt("dump", "write every node, field by field, to this file") },
        { "catalog stats", "catalog stats --build --json", "catalog stats --dump index.jsonl" },
        { "PackageCatalog::BuildCatalogIndex", "PackageCatalog::ScanCatalogIndex" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = C.Model->catalogSnapshot();
            json Out = {{"nodes", Snap->Nodes.size()}, {"tiles", Snap->Tiles.size()}, {"refused", Snap->RefusedNodes.size()}};
            size_t Js = 0, La = 0, Co = 0, En = 0, Ex = 0, Me = 0, Ti = 0, Re = 0;
            for (const auto &[Id, N] : Snap->Nodes)
            {
                (void)Id;
                Js += N.Json->dump().size(); La += N.LoweredLayers().dump().size(); Co += N.ContentLayers.dump().size();
                En += N.Entries.dump().size();
                Ex += N.Exec.dump().size(); Me += N.Meta.dump().size(); Re += N.Refs.size() + N.Requires.size();
            }
            for (const auto &[U, T] : Snap->Tiles) { (void)U; Ti += T.dump().size(); }
            Out["json_bytes"] = {{"node", Js}, {"content_layers", Co}, {"entries", En}, {"exec", Ex}, {"meta", Me}, {"tiles", Ti},
                                 {"lowered_layers_on_demand", La}};
            Out["refs"] = Re;
            if (A.contains("dump"))   // one line per node (by key), then the tables: two builds of one library compare with diff
            {
                std::ofstream F(Command::Str(A, "dump"), std::ios::binary);
                if (!F) throw std::runtime_error("cannot write " + Command::Str(A, "dump"));
                for (const auto &[Id, N] : Snap->Nodes)
                    F << json{{"key", Id}, {"cid", N.Cid}, {"handle", N.Handle}, {"label", N.NodeId}, {"lower_error", N.LowerError},
                              {"variant", N.Variant}, {"recommended", N.Recommended}, {"json", *N.Json}, {"layers", N.LoweredLayers()},
                              {"refs", N.Refs}, {"requires", N.Requires}, {"graft", N.IsGraft}, {"entries", N.Entries},
                              {"has_exec", N.HasExec}, {"has_runner", N.HasRunner}, {"own_runner", N.OwnRunner}, {"entry", N.Label},
                              {"host", N.HostPlatform}, {"guest", N.GuestPlatform}, {"exec", N.Exec}, {"faces", N.Faces}, {"uid", N.Uid},
                              {"package_uid", N.PackageUid}, {"meta", N.Meta}, {"own_tile", N.OwnTile}, {"file", N.File.string()},
                              {"bundle", N.BundleDir.string()}, {"received", N.Received}}.dump() << "\n";
                F << json{{"tiles", Snap->Tiles}}.dump() << "\n" << json{{"tile_graft", Snap->TileGraft}}.dump() << "\n";
                json Dup = json::object();
                for (const auto &[Id, Ps] : Snap->Duplicates) for (const auto &P : Ps) Dup[Id].push_back(P.string());
                F << json{{"duplicates", Dup}}.dump() << "\n";
                for (const auto &R : Snap->RefusedNodes)
                    F << json{{"refused", R.Handle}, {"file", R.File.string()}, {"bundle", R.BundleDir.string()}, {"why", R.Why}, {"too_large", R.TooLarge}}.dump() << "\n";
                Out["dumped"] = Command::Str(A, "dump");
            }
            if (Command::Bool(A, "build", false))
            {
                const auto T0 = std::chrono::steady_clock::now();
                const NodeIndex Fresh = PackageCatalog::ScanCatalogIndex(*C.Config);   // from the files, never the cache
                Out["build_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count();
                Out["built_nodes"] = Fresh.Nodes.size();
            }
            return Out;
        }, {} });

    Command::Register({
        "catalog validate", "check the library's nodes: errors and warnings",
        "Everything --validate-nodes checks (the format's rules, content, deltas, case, refused files) over the whole "
        "library, or the package, UID or node given.",
        { Command::Opt("scope", "a package UID, package folder name, or node id"), Command::Flag("errors-only", "leave the warnings out") },
        { "catalog validate", "catalog validate --scope 749", "catalog validate --errors-only --json" },
        { "Validate::Nodes", "ManifestModel::Closure" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = CmdUtil::Index(C);
            const NodeIndex &Idx = *Snap;
            std::set<std::string> Scope;
            if (A.contains("scope"))
            {
                const std::string S = Command::Str(A, "scope");
                for (const auto &[Id, N] : Idx.Nodes)
                    if (Id == S || N.Uid == S || N.BundleDir.filename().string() == S)
                        for (const std::string &D : ManifestModel::Closure(Idx, Id)) Scope.insert(D);
                if (Scope.empty()) throw Command::BadArgs("scope '" + S + "' matched no package or node");
            }
            std::vector<std::string> Errors, Warnings;
            Validate::Nodes(Idx, Errors, Warnings, Scope.empty() ? nullptr : &Scope);
            json Out = json::array();
            for (const auto &E : Errors) Out.push_back({{"severity", "error"}, {"message", E}});
            if (!Command::Bool(A, "errors-only", false)) for (const auto &W : Warnings) Out.push_back({{"severity", "warning"}, {"message", W}});
            if (!Errors.empty()) throw Command::CheckFailed(Out, std::to_string(Errors.size()) + " error(s)");
            return Out;
        }, &RenderFindings, true });
}
