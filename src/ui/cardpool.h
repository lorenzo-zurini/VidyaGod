#ifndef CARDPOOL_H
#define CARDPOOL_H

#include <QList>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"
#include "libraryview.h"   // LibraryGameCard, SortMode
#include "installed.h"     // Installed::NodeHydration

class AppModel;

// ---------------------------------------------------------------------------
// CardPool — which tiles a surface shows, and in what order: the Library's installed games, the Catalog's games to
// download. One rule for every surface that shows them (the Library and Catalog tabs, the controller mode's shelves),
// so a game never shows in one and not the other. The cards are the caller's (new'd; qDeleteAll them).
// ---------------------------------------------------------------------------
namespace CardPool
{
using HydrationMap = std::unordered_map<std::string, ::Installed::NodeHydration>;
// A Library card's rows: the installed ones (what it plays), and the ones not installed that can be fetched (its
// launch window lists them, to download there). Both in shelf order.
struct TileRows
{
    std::vector<std::string> Here, ToFetch;
    bool operator==(const TileRows &) const = default;
};
// The rows tile FaceUid's Library card holds, as Installed() builds it; nullopt when that tile has no playable card
// (not on the shelf, nothing of it installed, or missing).
std::optional<TileRows> LibraryRows(const NodeIndex &Idx, const HydrationMap &Hyd, const std::string &FaceUid);
// One card per tile whose every row (and its graft) is installed, and that has content of its own on disk.
QList<LibraryGameCard *> Installed(AppModel &Model);
// One card per tile with something missing that can be fetched. A package with several such games shows its base
// game's card (PackageMainCard) with the others as its Secondaries (IsSecondary set on them; they stay in the list).
QList<LibraryGameCard *> Available(AppModel &Model);
// The Library's order for Mode. Recent reads each card's LastPlayedCache (empty: never played, last).
void Sort(QList<LibraryGameCard *> &Cards, SortMode Mode);
// The Catalog's order: by section (SectionOf), then by Mode within it.
void SortBySection(QList<LibraryGameCard *> &Cards, SortMode Mode, const nlohmann::ordered_json &Config);
// The Catalog section a package belongs to: its CID source's name, the peer's "<nick> - <library>" folder it was
// received into, else "Local".
QString SectionOf(const nlohmann::ordered_json &Config, const std::filesystem::path &BundleDir);
// A search matches a card when its title, or one of its secondaries' titles, holds the text (case-insensitive).
bool Matches(const LibraryGameCard &Card, const QString &Search);
}

// The index of the card that is a package's base game (its tile UID is the package's own), or -1.
int PackageMainCard(const std::vector<std::string> &CardUids, const std::string &PackageUid);

#endif // CARDPOOL_H
