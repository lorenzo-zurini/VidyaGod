// `VidyaGod catalog …` — the node graph the app holds: the shelf of games (each edition's install state, what it would
// fetch), one node in full, the re-index and the validator. What the Library and Catalog tabs are built from.
#include "command.h"
#include "appmodel.h"
#include "manifestmodel.h"
#include "packagecatalog.h"

namespace
{

using json = nlohmann::ordered_json;

json Hydration(const PackageCatalog::NodeHydration &H)
{
    return {{"installed", H.Hydrated && H.OnDisk}, {"hydrated", H.Hydrated}, {"on_disk", H.OnDisk}, {"has_content", H.HasContent},
            {"fetchable", H.HasCid}, {"incomplete", H.Incomplete}};
}

// Where a game stands, in one word: installed (playable here), downloadable (not here, can be fetched), waiting (a
// package it contains has not landed), unavailable (neither), empty (no content at all).
std::string Standing(const PackageCatalog::NodeHydration &H)
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
        { "PackageCatalog::ShelfTiles", "PackageCatalog::HydrationMap", "AppModel::catalogSnapshot" , "PackageMainCard" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = C.Model->catalogSnapshot();
            const auto Hyd = PackageCatalog::HydrationMap(*Snap);
            const std::string Only = Command::Str(A, "state", ""), Search = Command::Str(A, "search", "");
            json Out = json::array();
            for (const auto &T : PackageCatalog::ShelfTiles(*Snap))
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
        { "catalog node bafkreid5cz7cjdoknaoev4p4su2fmoalbjyhvxyjifvtb47zeoco35kywa", "catalog node 'Original (Steam)' --json-node" },
        { "NodeIndex::Find", "PackageCatalog::HydrationMap", "PackageCatalog::MissingFromClosure" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const auto Snap = C.Model->catalogSnapshot();
            const Node &N = *FindNode(*Snap, Command::Str(A, "node"));
            const auto Hyd = PackageCatalog::HydrationMap(*Snap);
            json J = {{"cid", N.Cid}, {"label", N.NodeId}, {"variant", N.Variant}, {"package", N.BundleDir.string()},
                      {"received", N.Received}, {"graft", N.IsGraft}, {"runner", N.HasRunner}, {"contains", N.Refs},
                      {"requires", N.Requires}, {"tiles", N.Faces}, {"family", N.PackageUid}, {"recommended", N.Recommended}};
            if (!N.Label.empty()) J["entry"] = N.Label;
            if (!N.LowerError.empty()) J["error"] = N.LowerError;
            if (const auto H = Hyd.find(N.Key()); H != Hyd.end()) { J["state"] = Standing(H->second); J["hydration"] = Hydration(H->second); }
            if (const auto M = PackageCatalog::MissingFromClosure(*Snap, N.Key()); !M.empty()) J["missing"] = M;
            if (Command::Bool(A, "json-node", false)) J["node"] = N.Json;
            return J;
        }, {} });

    Command::Register({
        "catalog rebuild", "re-scan the library from disk",
        "Re-index every package folder (LIBRARY and received CATALOG) into the node graph; every tab refreshes from it.", {},
        { "catalog rebuild" }, { "AppModel::rebuildCatalog", "PackageCatalog::BuildCatalogIndex" , "LibraryTab::rebuild", "CatalogTab::rebuild", "RunnersPage::rebuild", "PackagesView::rebuildList", "LibraryView::setCards", "LibraryView::setGroups", "LibraryView::setSeriesGroups", "LibraryView::setEmptyMessage", "LibraryView::refreshVisuals", "LibraryView::repaintCards", "LibraryView::setHoverAction", "AppModel::catalogIndex" }, Needs::Model, false,
        [](Command::Context &C, const json &) {
            C.Model->rebuildCatalog();
            return json{{"nodes", C.Model->catalogIndex().Nodes.size()}, {"tiles", C.Model->catalogIndex().Tiles.size()}};
        }, {} });

    Command::Register({
        "catalog validate", "check the library's nodes: errors and warnings",
        "Everything --validate-nodes checks (the format's rules, content, deltas, case, refused files) over the whole "
        "library, or the package, UID or node given.",
        { Command::Opt("scope", "a package UID, package folder name, or node id"), Command::Flag("errors-only", "leave the warnings out") },
        { "catalog validate", "catalog validate --scope 749", "catalog validate --errors-only --json" },
        { "ManifestModel::ValidateNodeGraph", "ManifestModel::Closure" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const NodeIndex &Idx = C.Model->catalogIndex();
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
            ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings, Scope.empty() ? nullptr : &Scope);
            json Out = json::array();
            for (const auto &E : Errors) Out.push_back({{"severity", "error"}, {"message", E}});
            if (!Command::Bool(A, "errors-only", false)) for (const auto &W : Warnings) Out.push_back({{"severity", "warning"}, {"message", W}});
            return Out;
        }, {} });
}
