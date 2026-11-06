#ifndef SHELF_H
#define SHELF_H

#include "manifestmodel.h"   // NodeIndex, Node

#include <map>
#include <optional>
#include <string>
#include <vector>

// Shelf — what the library presents and how a row runs: tiles, their rows, and the grafts a row offers and applies.
// Queries of the index alone.
namespace Shelf
{

//Which grafts a content walk includes: the given list, or (unset) the row's pre-ticked grafts.
using GraftChoice = std::optional<std::vector<std::string>>;

// One presentable game on the shelf: a tile (its UID), its presentation (the merged tile), and its rows — the variant
// nodes whose fold presents it.
struct Tile
{
    std::string Uid;
    nlohmann::ordered_json Tile;
    std::vector<const Node*> Rows;
    std::string Graft;            // the graft presenting this tile ("" for a variant's): every row launches with it
};
// The shelf: one entry per tile UID (faces merged by UID across the library). A family nests by PARENTUID — the base
// game, then its children by depth, then by title — and families order by the base game's title. Under a tile, the
// variant RECOMMENDED under it comes first, then by VARIANT name. A variant presenting two tiles is a row under each.
// A tile a graft presents (NodeIndex::TileGraft) is a card too: its rows are the variants the graft applies onto
// (GraftBases), and the card names the graft (Tile::Graft). A graft that is a VERSION (it carries VARIANT) is a
// row: under the tiles its own entries present, else under its bases' tiles; it runs on a base with it applied.
std::vector<Tile> Tiles(const NodeIndex &Idx);
// The row a tile shows for a variant: the variant (a node with VARIANT) presenting tile Uid whose VARIANT is Variant;
// with Variant empty, the tile's default row — the variant RECOMMENDED under it, else the first by VARIANT name. ""
// when there is none, *Why saying what the tile offers instead.
std::string RowUnderTile(const NodeIndex &Idx, const std::string &Uid, const std::string &Variant, std::string *Why = nullptr);
// The versions a graft applies onto: the variants (not grafts themselves) containing, transitively, a node its leading
// ANY names — what a graft that is a card, or a version, runs on. Recommended-free order: by VARIANT, then key.
std::vector<std::string> GraftBases(const NodeIndex &Idx, const std::string &Graft);
// The version a graft that is a version runs on when nothing picks one (the engine's default): GraftBases' first. ""
// for a node that is not a graft, or a graft with nothing here it applies onto.
std::string DefaultGraftBase(const NodeIndex &Idx, const std::string &Graft);


// ---- grafts ----
// The grafts a row applies, in order (Fold::ApplyGrafts over the local grafts): Chosen = the instance's list, each
// kept when it is offered with those before it applied (the rest go to *Dropped); nullopt = a fresh instance's list,
// the grafts RECOMMENDED under the row's tile (Builtins' %UID% when given — the launched tile — else the node's first).
// A received browse stub never applies: it would mount un-hydrated.
std::vector<std::string> AppliedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId, const GraftChoice &Chosen,
                                       const std::map<std::string, std::string> &Instance = {},
                                       const std::map<std::string, std::string> &Builtins = {},
                                       std::vector<std::string> *Dropped = nullptr);
// Move the graft at Ticked[From] one place (By = -1 up, +1 down) in the instance's ordered graft list. The order is
// the order they apply in, and a graft is only applied when it is offered with those before it applied — so a move
// after which ANY graft that applied no longer does (a graft on a graft moved above the graft it needs, or one of two
// grafts that exclude each other swapped in ahead) is REFUSED, *Why naming it; Ticked is changed only on success.
// Judged as the launch judges it: Instance = the instance's values, Builtins = at least %UID% (the tile) and
// %PackageUID% (see AppliedGrafts).
bool MoveGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, std::vector<std::string> &Ticked, size_t From,
               int By, std::string *Why = nullptr, const std::map<std::string, std::string> &Instance = {},
               const std::map<std::string, std::string> &Builtins = {});
// The saved graft list as the current library sees it: a list naming a graft that no longer exists was saved against
// an older version of the package (every edit re-mints its grafts), so the choice is stale and the tile falls back to
// its defaults (nullopt: the grafts RECOMMENDED under it). A list whose grafts all exist is the user's choice.
GraftChoice CurrentGraftChoice(const NodeIndex &Idx, const GraftChoice &Saved);
// Tick graft G in the instance's ordered graft list: G goes last, and every ticked graft it cannot apply beside is
// unticked (into *Unticked) — one whose NOT names G, or one G's NOT names. The graft just ticked wins, so grafts that
// exclude each other behave as a choice of one. Judged in list order with the grafts kept so far in place (G or X may
// need another ticked graft): X is unticked when adding it is what stops G applying. Instance/Builtins as MoveGraft.
std::vector<std::string> TickGraft(const NodeIndex &Idx, const std::string &LaunchNodeId, const std::vector<std::string> &Ticked,
                                   const std::string &G, std::vector<std::string> *Unticked = nullptr,
                                   const std::map<std::string, std::string> &Instance = {},
                                   const std::map<std::string, std::string> &Builtins = {});
// The grafts this row offers with Chosen applied (a graft on a graft is offered once the graft it needs is), in the
// default order (LABEL, then CID); into *PreTicked, a fresh instance's list (AppliedGrafts with nullopt).
std::vector<std::string> OfferedGrafts(const NodeIndex &Idx, const std::string &LaunchNodeId,
                                       std::vector<std::string> *PreTicked = nullptr,
                                       const GraftChoice &Chosen = std::nullopt,
                                       const std::string &FaceUid = std::string());   // "" = the node's first tile

} // namespace Shelf

#endif // SHELF_H
