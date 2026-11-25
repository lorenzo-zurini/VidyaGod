#include "cardpool.h"

#include "appmodel.h"
#include "commonutils.h"      // LogOut
#include "installed.h"
#include "manifestmodel.h"
#include "packagecatalog.h"   // PackageSourceNameForPath
#include "shelf.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace CardPool
{

QList<LibraryGameCard *> Installed(AppModel &Model)
{
    QList<LibraryGameCard *> Out;
    //One tile per presentable GROUP of launchable nodes, holding the editions that are INSTALLED (content present
    //locally): a game is in the Library as soon as one edition plays — its other editions stay in the Catalog to get.
    //A tile with nothing installed and nothing to fetch (a local package whose files are gone) is shown too, greyed
    //as missing: it was in no tab at all.
    const auto &Hyd = Model.hydration();   // once per rebuild, shared with the other views — never per-launchable
    //The cards and the launch dialogs they open read one snapshot: a later re-index replaces it, and they are rebuilt.
    const std::shared_ptr<const NodeIndex> Snapshot = Model.catalogSnapshot();
    for (const Shelf::Tile &Tile : Shelf::Tiles(*Snapshot))
    {
        std::vector<std::string> Ids, All;
        bool HasContent = false, AnyFetchable = false;
        for (const Node *N : Tile.Rows)
        {
            All.push_back(N->Key());
            auto H = Hyd.find(N->Key());
            if (H != Hyd.end() && H->second.Hydrated)
            {
                Ids.push_back(N->Key());
                if (H->second.OnDisk) HasContent = true;   // a file of it is HERE, not only declared
            }
            else if (H != Hyd.end() && (H->second.HasCid || H->second.Incomplete)) AnyFetchable = true;
        }
        bool GraftHere = true;
        if (!Tile.Graft.empty())   // a graft's card: the graft is the game, so it must be installed too
            if (auto H = Hyd.find(Tile.Graft); H == Hyd.end() || !H->second.Hydrated)
            {
                GraftHere = false;
                if (H != Hyd.end() && (H->second.HasCid || H->second.Incomplete)) AnyFetchable = true;
            }
        const bool Playable = !Ids.empty() && HasContent && GraftHere;
        //Nothing of it here (its files gone — a row with nothing to fetch counts as installed, so it is the files on
        //disk that say) and nothing to get it from: shown as missing.
        const bool Missing  = !Playable && !AnyFetchable && !All.empty();
        if (!Playable && !Missing) continue;   // the Catalog has it
        if (Missing)
            LogOut("CardPool", "missing: '" + Tile.Tile.value("TITLE", Tile.Uid) + "' (tile " + Tile.Uid
                               + ") — none of its rows has content of its own on disk, and nothing offers it");
        auto *C = new LibraryGameCard(Model.config(), Snapshot, Playable ? std::move(Ids) : std::move(All), Tile.Uid);
        C->InitializeClassVariables();
        C->Missing = Missing;
        Out.append(C);
    }
    return Out;
}

QList<LibraryGameCard *> Available(AppModel &Model)
{
    QList<LibraryGameCard *> Out;
    //One card per un-hydrated presentable group: at least one edition's content is still missing AND fetchable
    //over IPFS. (A fully-hydrated group lives in the Library.)
    const auto &Hyd = Model.hydration();
    const std::shared_ptr<const NodeIndex> Snapshot = Model.catalogSnapshot();
    for (const Shelf::Tile &Tile : Shelf::Tiles(*Snapshot))
    {
        std::vector<std::string> Ids;
        bool AnyMissing = false, AnyFetchable = false;
        for (const Node *N : Tile.Rows)
        {
            Ids.push_back(N->NodeId);
            auto H = Hyd.find(N->Key());
            if (H == Hyd.end() || !H->second.Hydrated)
            {
                AnyMissing = true;
                // Fetchable = it names content CIDs, OR a package it contains has not landed yet (or is not shared):
                // the card stays, and its download dialog says what it is waiting for. From the one memoized pass: a
                // closure walk per edition froze the GUI for minutes on a peer's 903 Minecraft versions.
                if (H != Hyd.end() && (H->second.HasCid || H->second.Incomplete)) AnyFetchable = true;
            }
        }
        if (!Tile.Graft.empty())   // a graft's card: the graft is the game, so a missing graft is something to download
            if (auto H = Hyd.find(Tile.Graft); H == Hyd.end() || !H->second.Hydrated)
            {
                AnyMissing = true;
                if (H != Hyd.end() && (H->second.HasCid || H->second.Incomplete)) AnyFetchable = true;
            }
        if (!AnyMissing || !AnyFetchable || Ids.empty()) continue;
        bool AnyHere = false;   // an edition of it is installed: the game is in the Library too, this card has the rest
        for (const Node *N : Tile.Rows)
            if (auto H = Hyd.find(N->Key()); H != Hyd.end() && H->second.Hydrated && H->second.OnDisk) AnyHere = true;
        auto *C = new LibraryGameCard(Model.config(), Snapshot, std::move(Ids), Tile.Uid);
        C->InitializeClassVariables();
        C->InLibrary = AnyHere;
        Out.append(C);
    }

    // ── Per-package grouping: a package with several un-hydrated games shows its base game as the main tile and the
    // rest as secondaries beneath it. The base is the game whose tile UID is the package's own (PackageMainCard). ──
    // Sibling games group by FAMILY (%PackageUID%: the base game a tile's PARENTUID chain ends at) — not by
    // directory: a received package's games may land per-tile before the seeder's dir name reaches the wire (and a
    // dir is an on-disk accident anyway). A card with no family falls back to its bundle dir.
    std::map<std::string, std::vector<LibraryGameCard *>> ByBundle;
    std::vector<std::string> BundleOrder;
    for (LibraryGameCard *C : Out)
    {
        const Node *N = Snapshot->Find(C->RepNodeId);
        std::string Key = N ? N->PackageUid : std::string();
        if (Key.empty()) Key = C->PackagePath.string();
        if (ByBundle.find(Key) == ByBundle.end()) BundleOrder.push_back(Key);
        ByBundle[Key].push_back(C);
    }
    for (const std::string &Key : BundleOrder)
    {
        std::vector<LibraryGameCard *> &Games = ByBundle[Key];
        if (Games.size() < 2) continue;                                // single-game package → normal full tile
        std::vector<std::string> Uids;
        for (LibraryGameCard *C : Games) Uids.push_back(C->GameKey.toStdString());
        const int M = PackageMainCard(Uids, Key);
        if (M < 0) continue;                                           // its base is not here: nothing to nest under
        LibraryGameCard *Main = Games[(size_t)M];
        std::vector<LibraryGameCard *> Rest;
        for (LibraryGameCard *C : Games) if (C != Main) Rest.push_back(C);
        std::sort(Rest.begin(), Rest.end(), [](LibraryGameCard *A, LibraryGameCard *B) { return A->SortTitle < B->SortTitle; });
        for (LibraryGameCard *C : Rest)
        {
            C->IsSecondary = true;
            Main->Secondaries.push_back(C);
        }
    }
    return Out;
}

void Sort(QList<LibraryGameCard *> &Cards, SortMode Mode)
{
    switch (Mode)
    {
    case SortMode::Name:
        std::sort(Cards.begin(), Cards.end(), [](auto *A, auto *B) { return A->SortTitle < B->SortTitle; });
        break;
    case SortMode::Date:
        std::sort(Cards.begin(), Cards.end(), [](auto *A, auto *B) { return A->SortDate < B->SortDate; });
        break;
    case SortMode::Series:
        // Games without a series sort last, alphabetically by title within series.
        std::sort(Cards.begin(), Cards.end(), [](auto *A, auto *B) {
            const bool ANo = A->SeriesName.isEmpty(), BNo = B->SeriesName.isEmpty();
            if (ANo != BNo) return BNo;   // series before no-series
            if (ANo && BNo) return A->SortTitle < B->SortTitle;
            return A->SortSeriesKey < B->SortSeriesKey;
        });
        break;
    case SortMode::Recent:
        // Most recently played first; games never played after them, by title.
        std::stable_sort(Cards.begin(), Cards.end(), [](auto *A, auto *B) {
            const QString &X = A->LastPlayedCache, &Y = B->LastPlayedCache;
            if (X.isEmpty() != Y.isEmpty()) return Y.isEmpty();
            if (X != Y) return X > Y;
            return A->SortTitle < B->SortTitle;
        });
        break;
    }
}

void SortBySection(QList<LibraryGameCard *> &Cards, SortMode Mode, const nlohmann::ordered_json &Config)
{
    const auto KeyOf = [Mode](const LibraryGameCard *A) -> QString {
        switch (Mode)
        {
        case SortMode::Date:   return A->SortDate + "|" + A->SortTitle;
        case SortMode::Series: return A->SortSeriesKey;
        default:               return A->SortTitle;
        }
    };
    std::map<const LibraryGameCard *, QString> Section;   // one lookup per card, not one per comparison
    for (const LibraryGameCard *C : Cards) Section[C] = SectionOf(Config, C->PackagePath);
    std::sort(Cards.begin(), Cards.end(), [&](const LibraryGameCard *A, const LibraryGameCard *B) {
        const QString &RA = Section[A], &RB = Section[B];
        if (RA != RB) return RA < RB;
        return KeyOf(A) < KeyOf(B);
    });
}

QString SectionOf(const nlohmann::ordered_json &Config, const std::filesystem::path &BundleDir)
{
    // Each CID package source is its own named catalog section.
    const std::string Name = PackageCatalog::PackageSourceNameForPath(Config, BundleDir);
    if (!Name.empty()) return QString::fromStdString(Name);
    // A RECEIVED share lives under CATALOG/"<Nick> - <Lib>"/… — that is PUBLISHED content, so it sections under its
    // peer/library, NEVER "Local". Return the dir directly under CATALOG. Only a bundle out of BOTH trees is "Local".
    bool AfterCatalog = false;
    for (const auto &Part : BundleDir)
    {
        if (AfterCatalog) return QString::fromStdString(Part.string());
        if (Part.string() == "CATALOG") AfterCatalog = true;
    }
    return QStringLiteral("Local");   // genuinely out-of-tree / unpublished
}

bool Matches(const LibraryGameCard &Card, const QString &Search)
{
    //Its title, its series, or who made it ("Bethesda", "Saga").
    if (Search.isEmpty() || Card.GameTitle.contains(Search, Qt::CaseInsensitive)
        || Card.SeriesName.contains(Search, Qt::CaseInsensitive) || Card.Developer.contains(Search, Qt::CaseInsensitive)) return true;
    for (const LibraryGameCard *S : Card.Secondaries)
        if (S && S->GameTitle.contains(Search, Qt::CaseInsensitive)) return true;
    return false;
}

}

int PackageMainCard(const std::vector<std::string> &CardUids, const std::string &PackageUid)
{
    for (size_t I = 0; I < CardUids.size(); ++I)
        if (!PackageUid.empty() && CardUids[I] == PackageUid) return (int)I;
    return -1;
}
