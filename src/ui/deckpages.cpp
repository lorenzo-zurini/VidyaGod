// The controller UI's screens: Home, the shelves, a game's page, Downloads (deckshell_p.h).
#include "deckshell_p.h"
#include "apppaths.h"
#include "layouteditor.h"

#include "appmodel.h"
#include "asyncwork.h"
#include "cardpool.h"
#include "covercache.h"
#include "deckgrid.h"
#include "deckstyle.h"
#include "downloaddialog.h"   // StartCardDownload / CancelCardDownload — the Catalog's own
#include "downloadmanager.h"
#include "guiformat.h"        // PlayedWhen, HumanBytesQ
#include "instancestore.h"
#include "ipfsmodel.h"
#include "ipfswrapper.h"
#include "manifestmodel.h"
#include "osk.h"
#include "packagecatalog.h"

#include <QDate>
#include <QImage>
#include <QImageReader>
#include <QTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <map>

using json = nlohmann::ordered_json;

namespace
{
//A cover decoded off the GUI thread at the size it is drawn, and a blurred wash of it in the mark's two tones for
//behind the page (scaled down to a handful of pixels and back up: a blur for free).
struct Art { QImage Cover, Wash; };

Art DecodeArt(const QString &Path, QSize CoverSize, QSize WashSize)
{
    Art A;
    QImageReader R(Path);
    R.setAutoTransform(true);
    const QSize Full = R.size();
    if (Full.isValid() && CoverSize.isValid())
        R.setScaledSize(Full.scaled(CoverSize, Qt::KeepAspectRatioByExpanding).boundedTo(Full));
    A.Cover = R.read();
    if (A.Cover.isNull()) return A;
    QImage Small = DeckStyle::Duotone(A.Cover.scaled(28, 42, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    A.Wash = Small.scaled(WashSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return A;
}

//The tile a card shows, and the META inside it (where a package keeps its developer and its dates).
const json &TileOf(const LibraryGameCard &C)
{
    static const json None = json::object();
    const json *T = C.Index ? C.Index->Tile(C.FaceUid) : nullptr;
    return T && T->is_object() ? *T : None;
}

QString MetaStr(const json &Tile, const char *Key)
{
    for (const json *J : { Tile.contains("META") && Tile["META"].is_object() ? &Tile["META"] : nullptr, &Tile })
        if (J && J->contains(Key) && (*J)[Key].is_string()) return QString::fromStdString((*J)[Key].get<std::string>());
    return {};
}

QString NiceDate(const QString &Iso)
{
    const QDate D = QDate::fromString(Iso, Qt::ISODate);
    if (D.isValid()) return D.toString("d MMMM yyyy");
    return Iso.left(4) == "9999" ? QString() : Iso;
}

QString CoverPath(const LibraryGameCard &C)
{
    const json &T = TileOf(C);
    if (!T.contains("COVER")) return {};
    return CoverCache::instance()->resolve(T["COVER"], QString::fromStdString(C.PackagePath.string()));
}

void Wash(QPainter &P, const QRectF &R, const QPixmap &Backdrop, double Fade)
{
    if (!Backdrop.isNull())
    {
        P.save();
        P.setClipRect(R, Qt::IntersectClip);   // scaled to cover R, it would spill past it
        P.setOpacity(0.5);
        const QSizeF Src = Backdrop.size();
        const double K = std::max(R.width() / Src.width(), R.height() / Src.height());
        P.drawPixmap(QRectF(R.center().x() - Src.width() * K / 2, R.center().y() - Src.height() * K / 2, Src.width() * K, Src.height() * K),
                     Backdrop, QRectF(QPointF(0, 0), Src));
        P.restore();
    }
    //Dark to the left (where the text is) and at the bottom (into the page below).
    const auto G = [](int A) { QColor C = DeckStyle::Ground; C.setAlpha(A); return C; };
    QLinearGradient L(R.topLeft(), R.topRight());
    L.setColorAt(0, G(235));
    L.setColorAt(0.6, G(140));
    L.setColorAt(1, G(70));
    P.fillRect(R, L);
    QLinearGradient B(R.topLeft(), R.bottomLeft());
    B.setColorAt(0, G(0));
    B.setColorAt(Fade, G(0));
    B.setColorAt(1, DeckStyle::Ground);
    P.fillRect(R, B);
}

using DeckStyle::ButtonWidth;
using DeckStyle::DrawButton;
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckShelf
// ═════════════════════════════════════════════════════════════════════════════

DeckShelf::DeckShelf(DeckShell &S, Kind Which) : QWidget(nullptr), Shell(S), K(Which)
{
    setObjectName(Which == Kind::Library ? "controllerLibrary" : "controllerCatalog");
    setProperty("padOwnFocus", true);
    Grid = new DeckGrid(this);
    Grid->setObjectName(Which == Kind::Library ? "controllerLibraryGrid" : "controllerCatalogGrid");
    Grid->setEmptyText(Which == Kind::Library ? "No games are installed yet.\nWhat your peers share is in the Catalog."
                                              : "Nothing to download yet.\nAdd a peer under Peers, or a package source under Settings → Sources.");
    connect(Grid, &DeckGrid::activated, this, [this](LibraryGameCard *C) { if (C) Shell.openGame(C->GameKey); });
    connect(Grid, &DeckGrid::currentChanged, this, [this] { update(); });
    const json &C = *Shell.model().config();
    const char *Key = Which == Kind::Library ? "LibrarySort" : "CatalogSort";
    if (C.contains("Settings") && C["Settings"].contains("Controller") && C["Settings"]["Controller"].contains(Key)
        && C["Settings"]["Controller"][Key].is_number_integer())
        Sort = static_cast<SortMode>(std::clamp(C["Settings"]["Controller"][Key].get<int>(), 0, 3));
}

DeckShelf::~DeckShelf() { qDeleteAll(Pool); }

LibraryGameCard *DeckShelf::find(const QString &Key) const
{
    for (LibraryGameCard *C : Pool) if (C && C->GameKey == Key) return C;
    return nullptr;
}

QList<LibraryGameCard *> DeckShelf::rebuild()
{
    const QList<LibraryGameCard *> Old = Pool;
    Pool = K == Kind::Library ? CardPool::Installed(Shell.model()) : CardPool::Available(Shell.model());
    for (LibraryGameCard *C : Pool)
        if (const auto It = Active.constFind(C->GameKey); It != Active.constEnd()) { C->Downloading = true; C->DownloadPercent = It.value(); }
    apply();
    if (K == Kind::Library) countSizes();
    return Old;
}

void DeckShelf::refreshCovers()
{
    bool Any = false;
    for (LibraryGameCard *C : Pool)
        if (C && (C->CoverOriginal.isNull() || C->CoverStale)) { C->InitializeClassVariables(); Any = true; }
    if (Any) Grid->repaintCards();
}

void DeckShelf::apply()
{
    Shown.clear();
    for (LibraryGameCard *C : Pool)
        if (!C->IsSecondary && CardPool::Matches(*C, Search)) Shown.push_back(C);
    QVector<DeckGrid::Section> Sections;
    if (K == Kind::Library)
    {
        CardPool::Sort(Shown, Sort);
        if (Sort == SortMode::Series)
            for (int I = 0; I < Shown.size();)
            {
                int E = I;
                while (E + 1 < Shown.size() && Shown[E + 1]->SeriesName == Shown[I]->SeriesName) ++E;
                Sections.push_back({ Shown[I]->SeriesName.isEmpty() ? QStringLiteral("Other") : Shown[I]->SeriesName, I, E });
                I = E + 1;
            }
    }
    else
    {
        CardPool::SortBySection(Shown, Sort, *Shell.model().config());
        for (int I = 0; I < Shown.size();)
        {
            const QString Sec = CardPool::SectionOf(*Shell.model().config(), Shown[I]->PackagePath);
            int E = I;
            while (E + 1 < Shown.size() && CardPool::SectionOf(*Shell.model().config(), Shown[E + 1]->PackagePath) == Sec) ++E;
            Sections.push_back({ Sec, I, E });
            I = E + 1;
        }
    }
    Grid->setEmptyText(!Search.isEmpty() ? "Nothing matches “" + Search + "”."
                       : K == Kind::Library ? "No games are installed yet.\nWhat your peers share is in the Catalog."
                                            : "Nothing to download yet.\nAdd a peer under Peers, or a package source under Settings → Sources.");
    Grid->setCards(Shown, Sections);
    update();
}

void DeckShelf::setSearch(const QString &T)
{
    if (T == Search) return;
    Search = T;
    apply();
}

void DeckShelf::setSort(SortMode M)
{
    Sort = M;
    (*Shell.model().config())["Settings"]["Controller"][K == Kind::Library ? "LibrarySort" : "CatalogSort"] = static_cast<int>(M);
    Shell.model().save();
    apply();
}

void DeckShelf::cycleSort()
{
    //The Library by name, by when it was last played, by release, by series; the Catalog has no play times.
    static const SortMode LibOrder[] = { SortMode::Name, SortMode::Recent, SortMode::Date, SortMode::Series };
    static const SortMode CatOrder[] = { SortMode::Name, SortMode::Date, SortMode::Series };
    const SortMode *Order = K == Kind::Library ? LibOrder : CatOrder;
    const int N = K == Kind::Library ? 4 : 3;
    int I = 0;
    while (I < N && Order[I] != Sort) ++I;
    setSort(Order[(I + 1) % N]);
}

void DeckShelf::setDownload(const QString &Key, bool On, double Pct)
{
    if (On) Active[Key] = Pct; else Active.remove(Key);
    for (LibraryGameCard *C : Pool)
        if (C->GameKey == Key) { C->Downloading = On; C->DownloadPercent = On ? Pct : -1; }
    Grid->repaintCards();
    update();
}

void DeckShelf::countSizes()
{
    if (Counting) { CountAgain = true; return; }
    Counting = true;
    auto Cfg = std::make_shared<const json>(*Shell.model().config());
    std::shared_ptr<const NodeIndex> Idx = Shell.model().catalogSnapshot();
    auto Found = std::make_shared<std::vector<PackageCatalog::InstalledPackage>>();
    //When each game was last played: its instances' files, read here with the sizes, off the GUI thread (a read per
    //game held a large library's window back).
    auto Played = std::make_shared<std::map<std::string, QString>>();
    for (LibraryGameCard *C : Pool) if (C && !C->RepUid.empty()) (*Played)[C->RepUid];
    AsyncWork::Run(this, [Cfg, Idx, Found, Played](const AsyncWork::Job &) {
            *Found = PackageCatalog::InstalledPackages(*Cfg, *Idx);
            for (auto &[Uid, At] : *Played) At = QString::fromStdString(InstanceStore::LastRun(*Cfg, Uid));
        },
        [this, Found, Played] {
            Counting = false;
            if (CountAgain) { CountAgain = false; countSizes(); return; }
            for (LibraryGameCard *C : Pool)
                if (const auto It = Played->find(C->RepUid); C && It != Played->end()) { C->LastPlayedCache = It->second; C->LastPlayedRead = true; }
            if (Sort == SortMode::Recent) apply();
            std::map<std::string, std::uintmax_t> ByDir;
            std::uintmax_t Total = 0;
            for (const auto &P : *Found) { ByDir[P.Dir.lexically_normal().string()] = P.Bytes; Total += P.Bytes; }
            for (LibraryGameCard *C : Pool)
                if (const auto It = ByDir.find(C->PackagePath.lexically_normal().string()); It != ByDir.end()) C->Bytes = static_cast<qint64>(It->second);
            TotalBytes = static_cast<qint64>(Total);
            update();
            emit sizesCounted();
        });
}

QString DeckShelf::statusText() const
{
    const int N = static_cast<int>(Shown.size());
    int All = 0;
    for (LibraryGameCard *C : Pool) if (!C->IsSecondary) ++All;
    const QString Noun = K == Kind::Library ? "game" : "game to download";
    const QString Nouns = K == Kind::Library ? "games" : "games to download";
    if (!Search.isEmpty()) return QString("%1 of %2 %3 match “%4”").arg(N).arg(All).arg(All == 1 ? Noun : Nouns, Search);
    QString T = QString("%1 %2").arg(All).arg(All == 1 ? Noun : Nouns);
    if (K == Kind::Library && TotalBytes >= 0) T += " · " + HumanBytesQ(TotalBytes);
    return T;
}

static QString SortName(SortMode M)
{
    switch (M)
    {
    case SortMode::Date:   return "Release date";
    case SortMode::Series: return "Series";
    case SortMode::Recent: return "Recently played";
    default:               return "Name";
    }
}

bool DeckShelf::padPressed(Pad::Button B)
{
    switch (B)
    {
    case Pad::Button::Y: cycleSort(); return true;
    case Pad::Button::X:
        OnScreenKeyboard::Ask(this, K == Kind::Library ? "Search the Library" : "Search the Catalog", Search,
                              [this](const QString &T) { setSearch(T); }, [this](const QString &T) { setSearch(T); });
        return true;
    case Pad::Button::B:
        if (Search.isEmpty()) return false;
        setSearch({});
        return true;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckShelf::padHints() const
{
    QList<Hint> H;
    if (Grid->current()) H.push_back({ Pad::Button::A, "Open" });
    H.push_back({ Pad::Button::X, "Search" });
    H.push_back({ Pad::Button::Y, "Sort" });   // which one, the header says (it said it twice)
    if (!Search.isEmpty()) H.push_back({ Pad::Button::B, "Clear search" });
    return H;
}

json DeckShelf::padState() const
{
    json J{{"kind", K == Kind::Library ? "library" : "catalog"}, {"sort", SortName(Sort).toStdString()}, {"shown", Shown.size()}};
    if (!Search.isEmpty()) J["search"] = Search.toStdString();
    J["grid"] = Grid->padState();
    return J;
}

void DeckShelf::resizeEvent(QResizeEvent *)
{
    const double S = Shell.scale();
    Grid->setGeometry(0, static_cast<int>(96 * S), width(), std::max(0, height() - static_cast<int>(96 * S)));
}

void DeckShelf::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = Shell.scale();
    const QRectF Head(40 * S, 6 * S, width() - 80 * S, 90 * S);
    //Right: the sort and the search.
    const Pad::Glyphs G = PadInput::instance().glyphs();
    QList<PadTarget::Hint> Right{ { Pad::Button::Y, SortName(Sort) } };
    if (!Search.isEmpty()) Right.push_front({ Pad::Button::X, "“" + Search + "”" });
    const double Rw = DeckStyle::DrawHints(P, QRectF(Head.left(), Head.top(), Head.width(), 34 * S), Right, G, S);
    //Left: the focused game — its title large, its facts under it.
    LibraryGameCard *C = Grid->current();
    if (!C) return;
    P.setFont(DeckStyle::Display(28, S));
    P.setPen(DeckStyle::Text);
    const QFontMetricsF Tm(P.font());
    P.drawText(QRectF(Head.left(), Head.top(), Head.width() - Rw - 30 * S, 40 * S), Qt::AlignLeft | Qt::AlignVCenter,
               Tm.elidedText(C->GameTitle, Qt::ElideRight, Head.width() - Rw - 30 * S));
    QStringList Facts;
    const json &T = TileOf(*C);
    if (const QString D = MetaStr(T, "DEVELOPER"); !D.isEmpty()) Facts << D;
    if (C->SortDate.size() >= 4 && C->SortDate != "9999") Facts << C->SortDate.left(4);
    if (!C->SeriesName.isEmpty()) Facts << C->SeriesName;
    if (K == Kind::Library)
    {
        if (C->Bytes >= 0) Facts << HumanBytesQ(C->Bytes);
        if (const QString W = PlayedWhen(C->lastPlayed()); !W.isEmpty()) Facts << "Played " + W;
    }
    else
    {
        Facts << "From " + CardPool::SectionOf(*Shell.model().config(), C->PackagePath);
        if (!C->Secondaries.empty()) Facts << QString("+%1 more in this package").arg(C->Secondaries.size());
        if (C->Downloading) Facts << (C->DownloadPercent >= 0 ? QString("Downloading %1%").arg(static_cast<int>(C->DownloadPercent)) : QStringLiteral("Queued"));
    }
    P.setFont(DeckStyle::Font(15, S));
    P.setPen(DeckStyle::Dim);
    P.drawText(QRectF(Head.left(), Head.top() + 42 * S, Head.width(), 26 * S), Qt::AlignLeft | Qt::AlignVCenter, Facts.join("  ·  "));
    if (C->Golden)
    {
        P.setFont(DeckStyle::Font(13, S, QFont::Bold));
        P.setPen(DeckStyle::Gold);
        P.drawText(QRectF(Head.left(), Head.top() + 66 * S, Head.width(), 22 * S), Qt::AlignLeft | Qt::AlignVCenter, "★ GOLDEN");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckHome
// ═════════════════════════════════════════════════════════════════════════════

DeckHome::DeckHome(DeckShell &S) : QWidget(nullptr), Shell(S)
{
    setObjectName("controllerHome");
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);
    Recent = new DeckGrid(this);
    Fresh = new DeckGrid(this);
    for (DeckGrid *G : { Recent, Fresh })
    {
        G->setStrip(true);
        G->setLit(false);
        G->setFocusPolicy(Qt::NoFocus);   // the controller stays on Home; it lights the strip it is in
        connect(G, &DeckGrid::activated, this, [this](LibraryGameCard *C) { if (C) Shell.openGame(C->GameKey); });
    }
    Recent->setObjectName("controllerHomeRecent");
    Fresh->setObjectName("controllerHomeCatalog");
}

QStringList DeckHome::buttons() const
{
    if (HeroKey.isEmpty()) return { "Browse the Catalog", "Add a peer" };
    return { "▶  Play", "Details" };
}

void DeckHome::refresh()
{
    //The hero: the game played last (the first by name when none has been played).
    QList<LibraryGameCard *> Lib;
    for (LibraryGameCard *C : Shell.libraryCards()) if (C && !C->IsSecondary) Lib.push_back(C);
    CardPool::Sort(Lib, SortMode::Recent);
    LibraryGameCard *Hero = Lib.isEmpty() ? nullptr : Lib.front();
    const QString Was = HeroKey;
    HeroKey = Hero ? Hero->GameKey : QString();
    if (Hero)
    {
        HeroTitle = Hero->GameTitle;
        QStringList L;
        const QString W = PlayedWhen(Hero->LastPlayedCache);
        HeroPlayed = !W.isEmpty();
        if (HeroPlayed) L << "Last played " + W;
        else L << "Not played yet";
        const json &T = TileOf(*Hero);
        if (const QString D = MetaStr(T, "DEVELOPER"); !D.isEmpty()) L << D;
        if (Hero->SortDate.size() >= 4 && Hero->SortDate != "9999") L << Hero->SortDate.left(4);
        HeroLine = L.join("  ·  ");
        if (HeroKey != Was || HeroCover.isNull()) HeroCover = Hero->CoverOriginal;   // until the large one is decoded
    }
    else
    {
        HeroTitle = "Welcome to VidyaGod";
        HeroLine = Shell.catalogCards().isEmpty() ? "Add a peer, and the games they share show up in the Catalog."
                                                  : "Your Library is empty — your peers' games are waiting in the Catalog.";
        HeroCover = QPixmap();
    }
    if (HeroKey != Was) { Backdrop = QPixmap(); loadBackdrop(); }
    //Played lately (or, before anything has been, the Library by name); then what there is to download.
    QList<LibraryGameCard *> Played;
    for (LibraryGameCard *C : Lib) if (!C->LastPlayedCache.isEmpty() && Played.size() < 14) Played.push_back(C);
    if (Played.isEmpty())
    {
        QList<LibraryGameCard *> ByName = Lib;
        CardPool::Sort(ByName, SortMode::Name);
        Played = ByName.mid(0, 14);
    }
    QList<LibraryGameCard *> Cat;
    for (LibraryGameCard *C : Shell.catalogCards()) if (C && !C->IsSecondary && !C->GameTitle.trimmed().isEmpty()) Cat.push_back(C);   // no blank "—" tile
    std::stable_sort(Cat.begin(), Cat.end(), [](LibraryGameCard *A, LibraryGameCard *B) {
        if (A->Downloading != B->Downloading) return A->Downloading;
        return A->SortTitle < B->SortTitle;
    });
    Recent->setCards(Played);
    Fresh->setCards(Cat.mid(0, 14));
    if (Area == 1 && Recent->count() == 0) setArea(Fresh->count() ? 2 : 0);
    if (Area == 2 && Fresh->count() == 0) setArea(Recent->count() ? 1 : 0);
    Button = std::min(Button, static_cast<int>(buttons().size()) - 1);
    resizeEvent(nullptr);
    update();
}

void DeckHome::loadBackdrop()
{
    LibraryGameCard *C = Shell.card(HeroKey);
    if (!C) return;
    const QString Path = CoverPath(*C);
    if (Path.isEmpty()) return;
    const QString Key = HeroKey;
    const QSize Cv(static_cast<int>(heroRect().height()), static_cast<int>(heroRect().height() * 1.5));
    const QSize Wash = size().isEmpty() ? QSize(1280, 400) : QSize(width(), static_cast<int>(heroRect().height()));
    auto Out = std::make_shared<Art>();
    AsyncWork::Run(this, [Out, Path, Cv, Wash](const AsyncWork::Job &) { *Out = DecodeArt(Path, Cv, Wash); }, [this, Out, Key] {
        if (Key != HeroKey) return;
        if (!Out->Wash.isNull()) Backdrop = QPixmap::fromImage(Out->Wash);
        if (!Out->Cover.isNull()) HeroCover = QPixmap::fromImage(Out->Cover);
        update();
    });
}

QRectF DeckHome::heroRect() const { return QRectF(0, 0, width(), height() * 0.46); }

QRectF DeckHome::buttonRect(int I) const
{
    const double S = Shell.scale();
    const QRectF H = heroRect();
    const double CoverW = HeroKey.isEmpty() ? 0 : (H.height() - 44 * S) / 1.5 + 36 * S;
    double X = 40 * S + CoverW;
    const QStringList B = buttons();
    for (int K = 0; K < I; ++K) X += ButtonWidth(B[K], K == 0, S) + 14 * S;
    return QRectF(X, H.bottom() - 22 * S - 54 * S, ButtonWidth(B.value(I), I == 0, S), 54 * S);
}

void DeckHome::resizeEvent(QResizeEvent *)
{
    const double S = Shell.scale();
    const double Top = heroRect().bottom() + 4 * S, Label = 34 * S;
    const double Each = (height() - Top - 2 * Label - 6 * S) / 2;
    Recent->setGeometry(0, static_cast<int>(Top + Label), width(), static_cast<int>(Each));
    Fresh->setGeometry(0, static_cast<int>(Top + 2 * Label + Each), width(), static_cast<int>(Each));
    Recent->setVisible(Recent->count() > 0);
    Fresh->setVisible(Fresh->count() > 0);
}

void DeckHome::setArea(int A)
{
    Area = A;
    Recent->setLit(A == 1);
    Fresh->setLit(A == 2);
    update();
}

void DeckHome::press(int I)
{
    if (HeroKey.isEmpty())
    {
        Shell.showSection(I == 0 ? DeckShell::Section::Catalog : DeckShell::Section::Peers);
        return;
    }
    LibraryGameCard *C = Shell.card(HeroKey);
    if (!C) return;
    if (I == 0) C->play();
    else Shell.openGame(HeroKey);
}

bool DeckHome::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    DeckGrid *Strip = Area == 1 ? Recent : Area == 2 ? Fresh : nullptr;
    const auto Has = [&](int A) { return A == 0 || (A == 1 ? Recent->count() : Fresh->count()) > 0; };
    switch (B)
    {
    case P::Up:
        for (int A = Area - 1; A >= 0; --A) if (Has(A)) { setArea(A); PadInput::instance().tick(); return true; }
        return true;
    case P::Down:
        for (int A = Area + 1; A <= 2; ++A) if (Has(A)) { setArea(A); PadInput::instance().tick(); return true; }
        return true;
    case P::Left:
    case P::Right:
        if (Strip) { Strip->padPressed(B); return true; }
        {
            const int To = Button + (B == P::Right ? 1 : -1);
            if (To >= 0 && To < buttons().size()) { Button = To; PadInput::instance().tick(); update(); }
        }
        return true;
    case P::A:
        if (Strip) return Strip->padPressed(B);
        press(Button);
        return true;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckHome::padHints() const
{
    if (Area == 0) return { { Pad::Button::A, buttons().value(Button).remove("▶").trimmed() } };
    return { { Pad::Button::A, "Open" } };
}

json DeckHome::padState() const
{
    json J{{"kind", "home"}, {"area", Area == 0 ? "hero" : Area == 1 ? "recent" : "catalog"}};
    J["hero"] = HeroKey.isEmpty() ? json(nullptr) : json{{"key", HeroKey.toStdString()}, {"title", HeroTitle.toStdString()},
                                                          {"cover", {HeroCover.width(), HeroCover.height()}}};   // the size drawn from
    json Bs = json::array();
    for (const QString &Bt : buttons()) Bs.push_back(Bt.toStdString());
    J["buttons"] = Bs;
    if (Area == 0) J["button"] = buttons().value(Button).toStdString();
    J["recent"] = Recent->padState();
    J["catalog"] = Fresh->padState();
    return J;
}

QString DeckHome::statusText() const
{
    const int N = static_cast<int>(std::count_if(Shell.libraryCards().begin(), Shell.libraryCards().end(), [](LibraryGameCard *C) { return !C->IsSecondary; }));
    return QString("%1 game%2 in your Library").arg(N).arg(N == 1 ? "" : "s");
}

void DeckHome::mouseReleaseEvent(QMouseEvent *E)
{
    for (int I = 0; I < buttons().size(); ++I)
        if (buttonRect(I).contains(E->position())) { setArea(0); Button = I; press(I); return; }
}

void DeckHome::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = Shell.scale();
    const QRectF H = heroRect();
    Wash(P, H, Backdrop, 0.55);
    double X = 40 * S;
    if (!HeroKey.isEmpty())
    {
        const double Ch = H.height() - 44 * S;
        const QRectF Cr(X, H.top() + 18 * S, Ch / 1.5, Ch);
        QPainterPath Cp;
        Cp.addRoundedRect(Cr, 10 * S, 10 * S);
        DeckStyle::DrawLift(P, Cp, S * 1.4);
        DeckStyle::DrawCover(P, Cr, HeroCover, HeroTitle, 10 * S);
        X = Cr.right() + 36 * S;
    }
    DeckStyle::DrawLabel(P, QRectF(X, H.top() + 24 * S, width() - X - 40 * S, 28 * S),
                         HeroKey.isEmpty() ? "Get started" : HeroPlayed ? "Continue playing" : "In your library",
                         true, S);
    //The title (two lines at most), its facts right under it.
    P.setFont(DeckStyle::Display(40, S));
    P.setPen(DeckStyle::Text);
    const QRectF TitleBox(X, H.top() + 52 * S, width() - X - 40 * S, buttonRect(0).top() - H.top() - 52 * S - 44 * S);
    const QRectF Used = QFontMetricsF(P.font()).boundingRect(TitleBox, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, HeroTitle);
    P.drawText(TitleBox, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, HeroTitle);
    P.setFont(DeckStyle::Font(16, S));
    P.setPen(DeckStyle::Dim);
    P.drawText(QRectF(X, std::min(Used.bottom(), TitleBox.bottom()) + 8 * S, width() - X - 40 * S, 28 * S), Qt::AlignLeft | Qt::AlignVCenter, HeroLine);
    const QStringList B = buttons();
    for (int I = 0; I < B.size(); ++I) DrawButton(P, buttonRect(I), B[I], Area == 0 && I == Button, I == 0, S);
    //The strips' titles.
    const auto Label = [&](DeckGrid *G, const QString &T, bool On) {
        if (G->isVisible()) DeckStyle::DrawLabel(P, QRectF(40 * S, G->y() - 34 * S, width() - 80 * S, 32 * S), T, On, S);
    };
    bool AnyPlayed = false;
    for (LibraryGameCard *C : Shell.libraryCards()) if (C && !C->LastPlayedCache.isEmpty()) { AnyPlayed = true; break; }
    Label(Recent, AnyPlayed ? "Recently played" : "Your library", Area == 1);
    Label(Fresh, "Ready to download", Area == 2);
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckGamePage
// ═════════════════════════════════════════════════════════════════════════════

DeckGamePage::DeckGamePage(DeckShell &S) : QWidget(&S), Shell(S)
{
    setObjectName("controllerGame");
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);
    setAutoFillBackground(false);
    Others = new DeckGrid(this);
    Others->setObjectName("controllerGameOthers");
    Others->setStrip(true);
    Others->setLit(false);
    Others->setFocusPolicy(Qt::NoFocus);
    connect(Others, &DeckGrid::activated, this, [this](LibraryGameCard *C) { if (C) Shell.openGame(C->GameKey); });
}

void DeckGamePage::showGame(const QString &K)
{
    if (K != Key) { Cover = QPixmap(); Backdrop = QPixmap(); LoadedCoverFor.clear(); }
    Key = K;
    Area = 0;
    Button = 0;
    Confirm.clear();
    Others->setLit(false);
    refresh();
}

QStringList DeckGamePage::buttons() const
{
    if (Installed) return { "▶  Play", "Launch options", "Controller layout", Golden ? "Unmark golden" : "Mark golden" };
    if (Downloading) return { Confirm == "cancel" ? "Press again to cancel" : "Cancel download" };
    return { "↓  Download" };
}

void DeckGamePage::refresh()
{
    LibraryGameCard *C = Shell.card(Key);
    if (!C) return;
    Installed = Shell.installed(Key);
    Golden = C->Golden;
    Downloading = C->Downloading || Shell.downloads().downloading(Key);
    Percent = C->DownloadPercent;
    Title = C->GameTitle;
    const json &T = TileOf(*C);
    QStringList L1;
    for (const char *K : { "DEVELOPER", "PUBLISHER" })
        if (const QString V = MetaStr(T, K); !V.isEmpty() && !L1.contains(V)) L1 << V;
    if (const QString D = NiceDate(MetaStr(T, "RELEASEDATE")); !D.isEmpty()) L1 << D;
    Line1 = L1.join("  ·  ");
    Line2 = MetaStr(T, "EDITION");
    Chips.clear();
    if (!C->SeriesName.isEmpty()) Chips << C->SeriesName;
    else if (const QString Sr = MetaStr(T, "SERIES"); !Sr.isEmpty()) Chips << Sr;
    const auto Yes = [&](const char *K) { return MetaStr(T, K).compare("Yes", Qt::CaseInsensitive) == 0; };
    if (Yes("LOCALMULTIPLAYER")) Chips << "Local multiplayer";
    if (Yes("LOCALCOOP")) Chips << "Local co-op";
    if (Yes("LANMULTIPLAYER")) Chips << "LAN";
    if (Yes("ONLINEMULTIPLAYER")) Chips << "Online";
    if (Yes("ONLINEDRM")) Chips << "Online DRM";
    if (Yes("EDITOR")) Chips << "Editor";
    //Its state: installed and played, or where it comes from and how far it has come down.
    QStringList St;
    if (Installed)
    {
        St << "Installed";
        if (C->Bytes >= 0) St << HumanBytesQ(C->Bytes);
        if (const QString W = PlayedWhen(C->lastPlayed()); !W.isEmpty()) St << "Last played " + W; else St << "Not played yet";
        if (Golden) St << "Golden since " + QString::fromStdString(PackageCatalog::GoldenSince(*Shell.model().config(), C->RepUid));
    }
    else
    {
        St << "From " + CardPool::SectionOf(*Shell.model().config(), C->PackagePath);
        if (Downloading) St << (Percent >= 0 ? QString("Downloading · %1%").arg(Percent, 0, 'f', 1) : QStringLiteral("Queued"));
        else St << "Not installed";
    }
    Status = St.join("  ·  ");
    //Its versions: the rows of the tile, by their shelf names; the recommended one marked.
    Versions.clear();
    for (const std::string &Id : C->GroupNodeIds)
    {
        const Node *N = C->Index ? C->Index->Find(Id) : nullptr;
        if (!N) continue;
        QString Name = QString::fromStdString(!N->Variant.empty() ? N->Variant : N->Meta.is_object() ? N->Meta.value("TITLE", Id) : Id);
        if (std::find(N->Recommended.begin(), N->Recommended.end(), C->FaceUid) != N->Recommended.end()) Name = "★ " + Name;
        Versions << Name;
    }
    //And those not installed here (its launch screen downloads one), after them.
    for (const std::string &Id : C->NotInstalled)
    {
        const Node *N = C->Index ? C->Index->Find(Id) : nullptr;
        if (!N) continue;
        Versions << QString::fromStdString(!N->Variant.empty() ? N->Variant : N->Meta.is_object() ? N->Meta.value("TITLE", Id) : Id) + " (not installed)";
    }
    QList<LibraryGameCard *> Rest(C->Secondaries.begin(), C->Secondaries.end());
    Others->setCards(Rest);
    Others->setVisible(!Rest.isEmpty());
    if (Area == 1 && Rest.isEmpty()) { Area = 0; Others->setLit(false); }
    Button = std::min(Button, static_cast<int>(buttons().size()) - 1);
    if (Cover.isNull()) Cover = C->CoverOriginal;
    if (LoadedCoverFor != Key) loadCover();
    resizeEvent(nullptr);
    update();
    emit PadRouter::instance().hintsChanged();
}

void DeckGamePage::loadCover()
{
    LibraryGameCard *C = Shell.card(Key);
    if (!C) return;
    LoadedCoverFor = Key;
    const QString Path = CoverPath(*C);
    if (Path.isEmpty()) return;
    const QString K = Key;
    const QRectF R = coverRect();
    const QSize Cv(static_cast<int>(R.width() * devicePixelRatioF()), static_cast<int>(R.height() * devicePixelRatioF()));
    const QSize Wash = size().isEmpty() ? QSize(1280, 680) : size();
    auto Out = std::make_shared<Art>();
    AsyncWork::Run(this, [Out, Path, Cv, Wash](const AsyncWork::Job &) { *Out = DecodeArt(Path, Cv, Wash); }, [this, Out, K] {
        if (K != Key) return;
        if (!Out->Cover.isNull()) { Cover = QPixmap::fromImage(Out->Cover); Cover.setDevicePixelRatio(devicePixelRatioF()); }
        if (!Out->Wash.isNull()) Backdrop = QPixmap::fromImage(Out->Wash);
        update();
    });
}

QRectF DeckGamePage::coverRect() const
{
    const double S = Shell.scale();
    const double H = std::min(height() * (Others->isVisible() ? 0.62 : 0.8), 520 * S);
    return QRectF(56 * S, 30 * S, H / 1.5, H);
}

QRectF DeckGamePage::buttonRect(int I) const
{
    const double S = Shell.scale();
    const QRectF C = coverRect();
    double X = C.right() + 48 * S;
    const QStringList B = buttons();
    for (int K = 0; K < I; ++K) X += ButtonWidth(B[K], K == 0, S) + 14 * S;
    return QRectF(X, C.bottom() - 56 * S, ButtonWidth(B.value(I), I == 0, S), 56 * S);
}

void DeckGamePage::resizeEvent(QResizeEvent *)
{
    const double S = Shell.scale();
    const double Top = coverRect().bottom() + 46 * S;
    Others->setGeometry(0, static_cast<int>(Top), width(), std::max(0, height() - static_cast<int>(Top) - static_cast<int>(6 * S)));
}

void DeckGamePage::press(const QString &B)
{
    LibraryGameCard *C = Shell.card(Key);
    if (!C) return;
    if (B.contains("Play")) { C->play(); return; }
    if (B == "Launch options") { C->edit(); return; }
    if (B == "Controller layout")   // the game's controller layout, in the configurator
    {
        LayoutEditor E(QString::fromStdString((AppPaths::DataRoot() / "ControllerLayouts").string()), C->GameKey, C->GameTitle, window());
        E.exec();
        return;
    }
    if (B.endsWith("golden"))
    {
        const bool On = !Golden;
        PackageCatalog::SetGolden(*Shell.model().config(), C->RepUid, On, QDate::currentDate().toString(Qt::ISODate).toStdString());
        Shell.model().save();
        for (const QList<LibraryGameCard *> *Pool : { &Shell.libraryCards(), &Shell.catalogCards() })
            for (LibraryGameCard *O : *Pool) if (O && O->RepUid == C->RepUid) O->Golden = On;
        refresh();
        return;
    }
    if (B.contains("Download")) { StartCardDownload(Shell.downloads(), Shell.model(), C, window()); refresh(); return; }
    if (B == "Cancel download") { Confirm = "cancel"; update(); emit PadRouter::instance().hintsChanged(); return; }
    if (B == "Press again to cancel") { Confirm.clear(); Shell.downloads().cancelKey(Key); refresh(); return; }
}

bool DeckGamePage::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    const QStringList Bs = buttons();
    if (B != P::A && !Confirm.isEmpty()) { Confirm.clear(); update(); }   // anything but a second press: not cancelled
    switch (B)
    {
    case P::Left:
    case P::Right:
        if (Area == 1) { Others->padPressed(B); return true; }
        {
            const int To = Button + (B == P::Right ? 1 : -1);
            if (To >= 0 && To < Bs.size()) { Button = To; PadInput::instance().tick(); update(); }
        }
        return true;
    case P::Down:
        if (Area == 0 && Others->count() > 0) { Area = 1; Others->setLit(true); PadInput::instance().tick(); update(); }
        return true;
    case P::Up:
        if (Area == 1) { Area = 0; Others->setLit(false); PadInput::instance().tick(); update(); }
        return true;
    case P::A:
        if (Area == 1) return Others->padPressed(B);
        press(Bs.value(Button));
        return true;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckGamePage::padHints() const
{
    if (Area == 1) return { { Pad::Button::A, "Open" }, { Pad::Button::B, "Back" } };
    return { { Pad::Button::A, buttons().value(Button).remove("▶").remove("↓").trimmed() }, { Pad::Button::B, "Back" } };
}

json DeckGamePage::padState() const
{
    json Bs = json::array();
    for (const QString &B : buttons()) Bs.push_back(B.toStdString());
    json V = json::array();
    for (const QString &N : Versions) V.push_back(N.toStdString());
    json J{{"kind", "game"}, {"key", Key.toStdString()}, {"title", Title.toStdString()}, {"installed", Installed},
           {"status", Status.toStdString()}, {"buttons", Bs}, {"versions", V}};
    if (Area == 0) J["button"] = buttons().value(Button).toStdString();
    else J["others"] = Others->padState();
    if (Golden) J["golden"] = true;
    if (Downloading) J["downloading"] = Percent;
    return J;
}

void DeckGamePage::mouseReleaseEvent(QMouseEvent *E)
{
    const QStringList Bs = buttons();
    for (int I = 0; I < Bs.size(); ++I)
        if (buttonRect(I).contains(E->position())) { Area = 0; Button = I; press(Bs[I]); return; }
}

void DeckGamePage::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = Shell.scale();
    P.fillRect(rect(), DeckStyle::Ground);
    Wash(P, rect(), Backdrop, 0.7);
    const QRectF C = coverRect();
    QPainterPath Shape;
    Shape.addRoundedRect(C, 12 * S, 12 * S);
    DeckStyle::DrawLift(P, Shape, S * 1.6);
    DeckStyle::DrawCover(P, C, Cover, Title, 12 * S);

    const double X = C.right() + 48 * S, W = width() - X - 48 * S;
    double Y = C.top() + 6 * S;
    P.setFont(DeckStyle::Display(42, S));
    P.setPen(DeckStyle::Text);
    const QRectF Tr = QFontMetricsF(P.font()).boundingRect(QRectF(X, Y, W, 200 * S), Qt::TextWordWrap, Title);
    P.drawText(QRectF(X, Y, W, Tr.height()), Qt::AlignLeft | Qt::TextWordWrap, Title);
    Y += Tr.height() + 10 * S;
    if (!Line1.isEmpty())
    {
        P.setFont(DeckStyle::Font(17, S));
        P.setPen(DeckStyle::Dim);
        P.drawText(QRectF(X, Y, W, 26 * S), Qt::AlignLeft | Qt::AlignVCenter, Line1);
        Y += 28 * S;
    }
    if (!Line2.isEmpty())
    {
        P.setFont(DeckStyle::Font(15, S));
        P.setPen(DeckStyle::Faint);
        P.drawText(QRectF(X, Y, W, 24 * S), Qt::AlignLeft | Qt::AlignVCenter, Line2);
        Y += 26 * S;
    }
    //The chips: its series and what it can do.
    Y += 10 * S;
    QFont Cf = DeckStyle::Font(13, S, QFont::DemiBold);
    P.setFont(Cf);
    double Cx = X;
    for (const QString &Ch : Chips)
    {
        const double Cw = QFontMetricsF(Cf).horizontalAdvance(Ch) + 22 * S;
        if (Cx + Cw > X + W) break;
        QPainterPath Cp = DeckStyle::Capsule(QRectF(Cx, Y, Cw, 28 * S));
        P.fillPath(Cp, Ch == "Online DRM" ? QColor(0x4a, 0x22, 0x2a) : DeckStyle::PanelHi);
        P.setPen(Ch == "Online DRM" ? DeckStyle::Bad : DeckStyle::Text);
        P.drawText(QRectF(Cx, Y, Cw, 28 * S), Qt::AlignCenter, Ch);
        Cx += Cw + 8 * S;
    }
    if (!Chips.isEmpty()) Y += 40 * S;
    P.setFont(DeckStyle::Font(16, S, QFont::DemiBold));
    P.setPen(Golden ? DeckStyle::Gold : Installed ? DeckStyle::Good : DeckStyle::Text);
    P.drawText(QRectF(X, Y, W, 26 * S), Qt::AlignLeft | Qt::AlignVCenter, Status);
    Y += 32 * S;
    //Its versions, as many as there is room for above the buttons.
    if (!Versions.isEmpty())
    {
        const double Bottom = buttonRect(0).top() - 18 * S;
        if (Y + 22 * S < Bottom)
        {
            DeckStyle::DrawLabel(P, QRectF(X, Y - 2 * S, W, 26 * S),
                                 Versions.size() == 1 ? QStringLiteral("Version") : QString("%1 versions").arg(Versions.size()), false, S);
            Y += 26 * S;
        }
        P.setFont(DeckStyle::Font(15, S));
        P.setPen(DeckStyle::Text);
        int Shown = 0;
        for (const QString &V : Versions)
        {
            if (Y + 22 * S > Bottom) break;
            const bool Last = Y + 44 * S > Bottom && Shown + 1 < Versions.size();
            const QString T = Last ? QString("… and %1 more").arg(Versions.size() - Shown) : V;
            P.drawText(QRectF(X, Y, W, 22 * S), Qt::AlignLeft | Qt::AlignVCenter, QFontMetricsF(P.font()).elidedText(T, Qt::ElideRight, W));
            Y += 23 * S;
            ++Shown;
            if (Last) break;
        }
    }
    const QStringList Bs = buttons();
    for (int I = 0; I < Bs.size(); ++I) DrawButton(P, buttonRect(I), Bs[I], Area == 0 && I == Button, I == 0, S);
    //A download under way: its bar beside the button.
    if (Downloading && Percent >= 0)
    {
        const QRectF B = buttonRect(static_cast<int>(Bs.size()) - 1);
        const QRectF Bar(B.right() + 24 * S, B.center().y() - 5 * S, std::min(360 * S, X + W - B.right() - 24 * S), 10 * S);
        if (Bar.width() > 40 * S)
        {
            QPainterPath Tp;
            Tp.addRoundedRect(Bar, 5 * S, 5 * S);
            P.fillPath(Tp, DeckStyle::Panel);
            QPainterPath Fp;
            Fp.addRoundedRect(QRectF(Bar.left(), Bar.top(), Bar.width() * std::clamp(Percent / 100.0, 0.02, 1.0), Bar.height()), 5 * S, 5 * S);
            P.fillPath(Fp, DeckStyle::LimeHi);
        }
    }
    if (Others->isVisible())
    {
        DeckStyle::DrawLabel(P, QRectF(56 * S, Others->y() - 36 * S, width(), 32 * S), "Also in this package", Area == 1, S);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckDownloads
// ═════════════════════════════════════════════════════════════════════════════

DeckDownloads::DeckDownloads(DeckShell &S) : QWidget(nullptr), Shell(S)
{
    setObjectName("controllerDownloads");
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);
}

void DeckDownloads::refresh()
{
    const QString Was = Keys.value(Current);
    Keys = Shell.downloads().activeKeys();
    std::sort(Keys.begin(), Keys.end(), [this](const QString &A, const QString &B) { return row(A).Title.toLower() < row(B).Title.toLower(); });
    for (auto It = Progress.begin(); It != Progress.end();)
        It = Keys.contains(It.key()) ? std::next(It) : Progress.erase(It);
    Current = std::max(0, static_cast<int>(Keys.indexOf(Was)));
    if (Current >= Keys.size()) Current = std::max(0, static_cast<int>(Keys.size()) - 1);
    if (!Keys.contains(Confirm)) Confirm.clear();
    update();
    emit PadRouter::instance().hintsChanged();
}

void DeckDownloads::setProgress(const QString &Key, double Pct)
{
    Progress[Key] = Pct;
    update();
}

DeckDownloads::Row DeckDownloads::row(const QString &Key) const
{
    Row R;
    R.Percent = Progress.value(Key, -1);
    if (Key.startsWith("runner:"))
    {
        const std::string Id = Key.mid(7).toStdString();
        const Node *N = Shell.model().catalogIndex().Find(Id);
        R.Title = "Runner: " + QString::fromStdString(N && !N->NodeId.empty() ? N->NodeId : Id);
    }
    else if (LibraryGameCard *C = Shell.card(Key)) R.Title = C->GameTitle;
    else if (const json *T = Shell.model().catalogIndex().Tile(Key.toStdString()); T && T->contains("TITLE") && (*T)["TITLE"].is_string())
        R.Title = QString::fromStdString((*T)["TITLE"].get<std::string>());
    else R.Title = Key;
    //Its state from its files' transfers: the speed of those coming down, else why nothing is.
    double Speed = 0;
    int Down = 0, Queued = 0, Stalled = 0;
    QString Error;
    for (const QString &Cid : Shell.downloads().cidsOf(Key))
    {
        const IpfsModel::CidState *St = Shell.ipfs().stateRef(Cid);
        if (!St) continue;
        switch (St->phase)
        {
        case IpfsModel::CidState::Downloading: case IpfsModel::CidState::Pinning: ++Down; if (St->speedBps > 0) Speed += St->speedBps; break;
        case IpfsModel::CidState::Queued: case IpfsModel::CidState::Pending: ++Queued; break;
        case IpfsModel::CidState::Stalled: ++Stalled; break;
        case IpfsModel::CidState::Errored: if (Error.isEmpty()) Error = St->error.isEmpty() ? "a file failed" : St->error; break;
        default: break;
        }
    }
    if (Confirm == Key) R.Line = "Press X again to cancel it";
    else if (Firsted == Key) R.Line = "First in line now";
    else if (!Error.isEmpty()) { R.Line = "Error: " + Error; R.Bad = true; }
    else if (Down) R.Line = Speed > 0 ? "Downloading · " + HumanBytesQ(static_cast<qint64>(Speed)) + "/s" : QStringLiteral("Downloading");
    else if (Stalled) { R.Line = "Stalled — no peer is sending it right now"; R.Bad = true; }
    else if (Queued) R.Line = "Queued";
    else R.Line = "Starting…";
    return R;
}

QRectF DeckDownloads::rowRect(int I) const
{
    const double S = Shell.scale(), H = 96 * S;
    return QRectF(40 * S, 70 * S + I * (H + 10 * S) - Scroll, width() - 80 * S, H);
}

bool DeckDownloads::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    if (B != P::X && !Confirm.isEmpty()) { Confirm.clear(); update(); }
    if (Keys.isEmpty())
    {
        if (B == P::A && !Shell.model().networkingEnabled()) { Shell.model().setNetworkingEnabled(true); return true; }
        return B == P::Up || B == P::Down || B == P::Left || B == P::Right;
    }
    switch (B)
    {
    case P::Up: case P::Down:
    {
        const int To = Current + (B == P::Down ? 1 : -1);
        if (To < 0 || To >= Keys.size()) return true;
        Current = To;
        //Keep the row in sight.
        const QRectF R = rowRect(Current);
        if (R.bottom() > height() - 10) Scroll += R.bottom() - height() + 10;
        if (R.top() < 70 * Shell.scale()) Scroll = std::max(0.0, Scroll - (70 * Shell.scale() - R.top()));
        PadInput::instance().tick();
        update();
        return true;
    }
    case P::A: Shell.openGame(Keys[Current]); return true;
    case P::X:
        if (Confirm == Keys[Current]) { Confirm.clear(); Shell.downloads().cancelKey(Keys[Current]); refresh(); }
        else { Confirm = Keys[Current]; update(); }
        emit PadRouter::instance().hintsChanged();
        return true;
    case P::Y:
        for (const QString &Cid : Shell.downloads().cidsOf(Keys[Current])) Shell.ipfs().prioritize(Cid);
        //Said on its row for a moment: the list keeps its order (by title), so nothing else shows it.
        Firsted = Keys[Current];
        if (!FirstedTimer)
        {
            FirstedTimer = new QTimer(this);
            FirstedTimer->setSingleShot(true);
            connect(FirstedTimer, &QTimer::timeout, this, [this] { Firsted.clear(); update(); });
        }
        FirstedTimer->start(2500);
        PadInput::instance().tick();
        update();
        return true;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckDownloads::padHints() const
{
    if (Keys.isEmpty()) return Shell.model().networkingEnabled() ? QList<Hint>{} : QList<Hint>{ { Pad::Button::A, "Turn networking on" } };
    QList<Hint> H;
    if (Shell.card(Keys.value(Current))) H.push_back({ Pad::Button::A, "Details" });
    H.push_back({ Pad::Button::X, Confirm == Keys.value(Current) ? "Confirm cancel" : "Cancel" });
    H.push_back({ Pad::Button::Y, "Download first" });
    return H;
}

json DeckDownloads::padState() const
{
    json Rows = json::array();
    for (const QString &K : Keys)
    {
        const Row R = row(K);
        Rows.push_back(json{{"key", K.toStdString()}, {"title", R.Title.toStdString()}, {"percent", R.Percent}, {"state", R.Line.toStdString()}});
    }
    json J{{"kind", "downloads"}, {"rows", Rows}};
    if (!Keys.isEmpty()) J["current"] = Keys.value(Current).toStdString();
    return J;
}

QString DeckDownloads::statusText() const
{
    if (Keys.isEmpty()) return "Nothing downloading";
    return QString("%1 download%2").arg(Keys.size()).arg(Keys.size() == 1 ? "" : "s");
}

void DeckDownloads::mouseReleaseEvent(QMouseEvent *E)
{
    for (int I = 0; I < Keys.size(); ++I)
        if (rowRect(I).contains(E->position())) { Current = I; update(); Shell.openGame(Keys[I]); return; }
}

void DeckDownloads::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = Shell.scale();
    //The node: up or not, its peers and speeds.
    const IpfsModel::NodeStatus &N = Shell.ipfs().nodeStatus();
    QString Node;
    if (!Shell.model().networkingEnabled()) Node = "Networking is off: nothing can download.";
    else if (!IpfsWrapper::Available()) Node = "The node is starting…";
    else Node = QString("Node online  ·  %1 peer%2  ·  ↓ %3/s  ·  ↑ %4/s").arg(N.peers).arg(N.peers == 1 ? "" : "s")
                    .arg(HumanBytesQ(static_cast<qint64>(N.downBps)), HumanBytesQ(static_cast<qint64>(N.upBps)));
    if (N.diskFree >= 0 && IpfsWrapper::Available()) Node += "  ·  " + HumanBytesQ(N.diskFree) + " free";
    P.setFont(DeckStyle::Font(16, S, QFont::DemiBold));
    P.setPen(Shell.model().networkingEnabled() ? DeckStyle::Dim : DeckStyle::Gold);
    P.drawText(QRectF(40 * S, 14 * S, width() - 80 * S, 40 * S), Qt::AlignLeft | Qt::AlignVCenter, Node);
    if (Keys.isEmpty())
    {
        P.setFont(DeckStyle::Display(30, S, true));
        P.setPen(DeckStyle::Dim);
        P.drawText(rect().adjusted(0, static_cast<int>(60 * S), 0, 0), Qt::AlignCenter, "Nothing is downloading");
        return;
    }
    P.save();
    P.setClipRect(QRectF(0, 62 * S, width(), height()));
    for (int I = 0; I < Keys.size(); ++I)
    {
        const QRectF R = rowRect(I);
        if (R.bottom() < 0 || R.top() > height()) continue;
        const Row Rw = row(Keys[I]);
        const bool On = I == Current;
        QPainterPath Bg;
        Bg.addRoundedRect(R, 14 * S, 14 * S);
        if (On) DeckStyle::DrawLift(P, Bg, S);
        P.fillPath(Bg, On ? DeckStyle::PanelHi : DeckStyle::Panel);
        if (On) DeckStyle::DrawRing(P, Bg, S);
        //Its cover, small.
        const QRectF Cv(R.left() + 14 * S, R.top() + 10 * S, (R.height() - 20 * S) / 1.5, R.height() - 20 * S);
        LibraryGameCard *C = Shell.card(Keys[I]);
        DeckStyle::DrawCover(P, Cv, C ? C->CoverOriginal : QPixmap(), Rw.Title, 5 * S);
        const double X = Cv.right() + 18 * S, W = R.right() - X - 20 * S;
        P.setFont(DeckStyle::Font(19, S, QFont::DemiBold));
        P.setPen(DeckStyle::Text);
        P.drawText(QRectF(X, R.top() + 10 * S, W, 28 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Title);
        P.setFont(DeckStyle::Font(15, S));
        P.setPen(Rw.Bad ? DeckStyle::Bad : DeckStyle::Dim);
        P.drawText(QRectF(X, R.top() + 38 * S, W, 24 * S), Qt::AlignLeft | Qt::AlignVCenter, Rw.Line);
        const QString Pct = Rw.Percent >= 0 ? QString("%1%").arg(Rw.Percent, 0, 'f', 1) : QString();
        P.setFont(DeckStyle::Font(17, S, QFont::DemiBold));
        P.setPen(DeckStyle::Text);
        P.drawText(QRectF(X, R.top() + 10 * S, W, 28 * S), Qt::AlignRight | Qt::AlignVCenter, Pct);
        const QRectF Bar(X, R.bottom() - 22 * S, W, 8 * S);
        QPainterPath Tp;
        Tp.addRoundedRect(Bar, 4 * S, 4 * S);
        P.fillPath(Tp, DeckStyle::GroundLow);
        if (Rw.Percent >= 0)
        {
            QPainterPath Fp;
            Fp.addRoundedRect(QRectF(Bar.left(), Bar.top(), Bar.width() * std::clamp(Rw.Percent / 100.0, 0.01, 1.0), Bar.height()), 4 * S, 4 * S);
            P.fillPath(Fp, Rw.Bad ? DeckStyle::Bad : DeckStyle::LimeHi);
        }
    }
    P.restore();
}
