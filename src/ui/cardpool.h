#ifndef CARDPOOL_H
#define CARDPOOL_H

#include <QList>
#include <QString>

#include <filesystem>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "libraryview.h"   // LibraryGameCard, SortMode

class AppModel;

// ---------------------------------------------------------------------------
// CardPool — which tiles a surface shows, and in what order: the Library's installed games, the Catalog's games to
// download. One rule for every surface that shows them (the Library and Catalog tabs, the controller mode's shelves),
// so a game never shows in one and not the other. The cards are the caller's (new'd; qDeleteAll them).
// ---------------------------------------------------------------------------
namespace CardPool
{
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
