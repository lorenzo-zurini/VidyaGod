#include "deckshell.h"
#include "deckshell_p.h"

#include "appmodel.h"
#include "covercache.h"
#include "deckgrid.h"
#include "deckstyle.h"
#include "downloadmanager.h"
#include "ipfsmodel.h"
#include "ipfstab.h"
#include "librarytab.h"   // AddLocalPackage — the menu's
#include "packageeditor.h"
#include "ipfswrapper.h"   // IpfsWrapper::Available — the node's state in the header
#include "networktab.h"
#include "osk.h"
#include "settingstab.h"

#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QLayout>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using json = nlohmann::ordered_json;

// ═════════════════════════════════════════════════════════════════════════════
// ControllerMode
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
//The app's mark (the capsule), as the header and the menu draw it: decoded once, at a size that stays sharp on a TV.
const QPixmap &Mark()
{
    static const QPixmap M = QPixmap(":/vidyagod.png").scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return M;
}
}

namespace ControllerMode
{
namespace
{
const json *Settings(const json &Config)
{
    if (!Config.is_object() || !Config.contains("Settings") || !Config["Settings"].is_object()) return nullptr;
    const json &S = Config["Settings"];
    if (!S.contains("Controller") || !S["Controller"].is_object()) return nullptr;
    return &S["Controller"];
}
bool Flag(const json &Config, const char *Key, bool Default)
{
    const json *S = Settings(Config);
    return S && S->contains(Key) && (*S)[Key].is_boolean() ? (*S)[Key].get<bool>() : Default;
}
}

std::string StartName(Start S) { return S == Start::Controller ? "controller" : S == Start::Desktop ? "desktop" : "auto"; }

std::optional<Start> StartFrom(const std::string &N)
{
    if (N == "auto") return Start::Auto;
    if (N == "controller") return Start::Controller;
    if (N == "desktop") return Start::Desktop;
    return std::nullopt;
}

Start StartSetting(const json &Config)
{
    const json *S = Settings(Config);
    if (!S || !S->contains("Start") || !(*S)["Start"].is_string()) return Start::Auto;
    return StartFrom((*S)["Start"].get<std::string>()).value_or(Start::Auto);
}

void SetStart(json &Config, Start S) { Config["Settings"]["Controller"]["Start"] = StartName(S); }
bool DesktopNavigation(const json &Config) { return Flag(Config, "Desktop", true); }
void SetDesktopNavigation(json &Config, bool On) { Config["Settings"]["Controller"]["Desktop"] = On; }
bool Haptics(const json &Config) { return Flag(Config, "Haptics", false); }
void SetHaptics(json &Config, bool On) { Config["Settings"]["Controller"]["Haptics"] = On; }

bool InGameMode()
{
    //Steam's Game Mode runs its apps in a gamescope session: SteamOS says so with SteamGamepadUI, gamescope with
    //its own display; the desktop (KDE on the Deck) has neither.
    return qEnvironmentVariable("SteamGamepadUI") == "1" || qEnvironmentVariable("XDG_CURRENT_DESKTOP") == "gamescope"
        || !qEnvironmentVariable("GAMESCOPE_WAYLAND_DISPLAY").isEmpty();
}

bool StartsInController(const json &Config, bool FlagController, bool FlagDesktop)
{
    if (FlagController) return true;
    if (FlagDesktop) return false;
    switch (StartSetting(Config))
    {
    case Start::Controller: return true;
    case Start::Desktop:    return false;
    default:                return InGameMode();
    }
}
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckHosted
// ═════════════════════════════════════════════════════════════════════════════

DeckHosted::DeckHosted(QWidget *P, double S, QWidget *Parent) : QWidget(Parent), Page(P)
{
    setProperty("padOwnFocus", false);   // its controls are the desktop's: the focus ring shows which one
    auto *L = new QVBoxLayout(this);
    L->addWidget(Page);
    Page->setParent(this);
    setScale(S);
}

void DeckHosted::setScale(double S)
{
    if (auto *L = qobject_cast<QVBoxLayout *>(layout())) L->setContentsMargins(36 * S, 10 * S, 36 * S, 6 * S);
    Page->setStyleSheet(DeckStyle::HostedStyleSheet(S));
    //The Settings sidebar is a fixed 200 px: wide enough for its names at the desktop's size, not at the couch's.
    for (QListWidget *L : Page->findChildren<QListWidget *>())
        if (L->objectName() == "SettingsSidebar") L->setFixedWidth(static_cast<int>(270 * S));
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckMenu
// ═════════════════════════════════════════════════════════════════════════════

DeckMenu::DeckMenu(DeckShell &S) : QWidget(&S), Shell(S)
{
    setObjectName("deckMenu");
    setProperty("padSkip", true);
    setProperty("padOwnFocus", true);
    hide();
}

QStringList DeckMenu::items() const
{
    return { "Desktop mode", Shell.model().networkingEnabled() ? "Turn networking off" : "Turn networking on",
             "Check the library for changes", "Add a local package…", "Package editor", "Controller settings", "Quit VidyaGod" };
}

void DeckMenu::present()
{
    Current = 0;
    setGeometry(Shell.rect());
    show();
    raise();
    PadRouter::instance().pushOverlay(this);
    update();
}

void DeckMenu::dismiss()
{
    PadRouter::instance().popOverlay(this);
    hide();
}

void DeckMenu::choose(const QString &Item)
{
    dismiss();
    if (Item == "Desktop mode") emit Shell.desktopRequested();
    else if (Item.startsWith("Turn networking")) Shell.model().setNetworkingEnabled(!Shell.model().networkingEnabled());
    else if (Item == "Check the library for changes") Shell.model().refreshFromDisk();
    else if (Item == "Add a local package…") AddLocalPackage(Shell.model(), &Shell);
    else if (Item == "Package editor") PackageEditor::OpenFor(Shell.model().config(), &Shell, "");
    else if (Item == "Controller settings") Shell.showSection(DeckShell::Section::Settings);
    else if (Item == "Quit VidyaGod") emit Shell.quitRequested();
}

bool DeckMenu::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    const QStringList I = items();
    switch (B)
    {
    case P::Up:   Current = (Current + I.size() - 1) % I.size(); PadInput::instance().tick(); update(); break;
    case P::Down: Current = (Current + 1) % I.size(); PadInput::instance().tick(); update(); break;
    case P::A:    choose(I.value(Current)); break;
    case P::B: case P::Start: dismiss(); break;
    default: break;
    }
    return true;
}

QList<PadTarget::Hint> DeckMenu::padHints() const { return { { Pad::Button::A, "Select" }, { Pad::Button::B, "Close" } }; }

json DeckMenu::padState() const
{
    json Items = json::array();
    for (const QString &I : items()) Items.push_back(I.toStdString());
    return json{{"kind", "menu"}, {"items", Items}, {"current", items().value(Current).toStdString()}};
}

QRectF DeckMenu::itemRect(int I) const
{
    const double S = Shell.scale(), W = 460 * S, H = 58 * S;
    const double Top = (height() - items().size() * H) / 2;
    return QRectF((width() - W) / 2, Top + I * H, W, H - 6 * S);
}

void DeckMenu::mouseReleaseEvent(QMouseEvent *E)
{
    const QStringList I = items();
    for (int K = 0; K < I.size(); ++K)
        if (itemRect(K).contains(E->position())) { choose(I[K]); return; }
    dismiss();
}

void DeckMenu::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    QColor Scrim = DeckStyle::GroundLow;
    Scrim.setAlpha(205);
    P.fillRect(rect(), Scrim);
    const double S = Shell.scale();
    const QStringList I = items();
    const QRectF First = itemRect(0), Last = itemRect(static_cast<int>(I.size()) - 1);
    const QRectF Panel = First.united(Last).adjusted(-18 * S, -64 * S, 18 * S, 62 * S);
    QPainterPath Pp;
    Pp.addRoundedRect(Panel, 22 * S, 22 * S);
    DeckStyle::DrawLift(P, Pp, S * 1.4);
    P.fillPath(Pp, DeckStyle::Panel);
    //Its title: the mark, and the name in the display face.
    const double Mh = 30 * S;
    if (const QPixmap &M = Mark(); !M.isNull())
        P.drawPixmap(QRectF(Panel.left() + 22 * S, Panel.top() + 16 * S, Mh, Mh), M, QRectF(M.rect()));
    P.setFont(DeckStyle::Display(22, S));
    P.setPen(DeckStyle::Text);
    P.drawText(QRectF(Panel.left() + 30 * S + Mh, Panel.top() + 14 * S, Panel.width(), 34 * S), Qt::AlignLeft | Qt::AlignVCenter, "VidyaGod");
    P.setFont(DeckStyle::Font(19, S, QFont::DemiBold));
    for (int K = 0; K < I.size(); ++K)
    {
        const QRectF R = itemRect(K);
        if (K == Current)
        {
            const QPainterPath Hp = DeckStyle::Capsule(R);
            DeckStyle::DrawLift(P, Hp, S);
            P.fillPath(Hp, DeckStyle::Lime);
        }
        P.setPen(K == Current ? DeckStyle::Ink : DeckStyle::Text);
        P.drawText(R.adjusted(24 * S, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, I[K]);
    }
    DeckStyle::DrawHints(P, QRectF(Panel.left() + 20 * S, Panel.bottom() - 46 * S, Panel.width() - 40 * S, 34 * S), padHints(),
                         PadInput::instance().glyphs(), S);
}

// ═════════════════════════════════════════════════════════════════════════════
// The hint strip: what the buttons do, along the bottom of a window controller mode opened (the shell has its own)
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
class HintStrip : public QWidget
{
public:
    HintStrip(QWidget *Window, double Scale) : QWidget(Window), S(Scale)
    {
        setObjectName("controllerHints");
        setProperty("padSkip", true);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        //Room for it under the window's own content, whatever its layout.
        if (QLayout *L = Window->layout())
        {
            const QMargins M = L->contentsMargins();
            L->setContentsMargins(M.left(), M.top(), M.right(), M.bottom() + height_());
        }
        Window->installEventFilter(this);
        connect(&PadRouter::instance(), &PadRouter::hintsChanged, this, [this] { update(); });
        place();
        show();
    }
    int height_() const { return static_cast<int>(48 * S); }

protected:
    bool eventFilter(QObject *W, QEvent *E) override
    {
        if (W == parentWidget() && (E->type() == QEvent::Resize || E->type() == QEvent::Show)) place();
        return false;
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter P(this);
        P.fillRect(rect(), DeckStyle::GroundLow);
        DeckStyle::DrawSplitRule(P, QRectF(0, 0, width(), 2 * S));
        //Only while this window is the one presses go to (another window's hints would be a lie).
        if (PadRouter::targetWindow() != window()) return;
        DeckStyle::DrawHints(P, QRectF(24 * S, 0, width() - 48 * S, height()), PadRouter::instance().hints(), PadInput::instance().glyphs(), S);
    }

private:
    void place()
    {
        if (!parentWidget()) return;
        setGeometry(0, parentWidget()->height() - height_(), parentWidget()->width(), height_());
        raise();
    }
    double S;
};
}

// ═════════════════════════════════════════════════════════════════════════════
// DeckShell
// ═════════════════════════════════════════════════════════════════════════════

namespace { bool ControllerOn = false; }

bool DeckShell::On() { return ControllerOn; }

DeckShell::DeckShell(AppModel &M, IpfsModel &I, DownloadManager &D, QWidget *Parent)
    : QWidget(Parent), Model(M), Ipfs(I), Downloads(D)
{
    setObjectName("controllerShell");
    setAccessibleName("Controller mode");
    setFocusPolicy(Qt::StrongFocus);
    setProperty("padOwnFocus", true);
    setAutoFillBackground(false);

    Body = new QStackedWidget(this);
    Home = new DeckHome(*this);
    Library = new DeckShelf(*this, DeckShelf::Kind::Library);
    Catalog = new DeckShelf(*this, DeckShelf::Kind::Catalog);
    DownloadsPage = new DeckDownloads(*this);
    Body->addWidget(Home);
    Body->addWidget(Library);
    Body->addWidget(Catalog);
    Body->addWidget(DownloadsPage);
    //Friends, Node and Settings are the desktop pages, built the first time they are shown (as the desktop's own tabs
    //build theirs): placeholders hold their places.
    for (int K = 0; K < 3; ++K) Body->addWidget(new QWidget(Body));
    Game = new DeckGamePage(*this);
    Game->hide();
    Menu = new DeckMenu(*this);

    connect(&Model, &AppModel::catalogChanged, this, &DeckShell::rebuildAll);
    //The play times and sizes land after a rebuild (counted off the GUI thread): Home's hero is the game played last.
    connect(Library, &DeckShelf::sizesCounted, this, [this] { Home->refresh(); if (gameOpen()) Game->refresh(); update(); });
    connect(&Model, &AppModel::coversReady, this, [this] {
        Library->refreshCovers();
        Catalog->refreshCovers();
        Home->refresh();
        if (gameOpen()) Game->refresh();
    });
    connect(CoverCache::instance(), &CoverCache::coversTooSmall, this, [this] {
        for (DeckShelf *Sh : { Library, Catalog })
            for (LibraryGameCard *C : Sh->cards()) if (C && !C->CoverOriginal.isNull()) C->CoverStale = true;
        Library->refreshCovers();
        Catalog->refreshCovers();
    });
    connect(&Model, &AppModel::networkingChanged, this, [this] { update(); });
    connect(&Model, &AppModel::nodeReady, this, [this] { update(); });
    connect(&Downloads, &DownloadManager::downloadStarted, this, [this](const QString &K) {
        Catalog->setDownload(K, true, -1);
        DownloadsPage->refresh();
        if (gameOpen()) Game->refresh();
        update();
    });
    connect(&Downloads, &DownloadManager::downloadProgress, this, [this](const QString &K, double Pct) {
        Catalog->setDownload(K, true, Pct);
        DownloadsPage->setProgress(K, Pct);
        if (gameOpen() && Game->key() == K) Game->refresh();
    });
    connect(&Downloads, &DownloadManager::downloadFinished, this, [this](const QString &K) {
        Catalog->setDownload(K, false, -1);
        DownloadsPage->refresh();
        if (gameOpen()) Game->refresh();
        update();
    });
    connect(&Ipfs, &IpfsModel::nodeStatusChanged, this, [this] { if (Current == Section::Downloads) DownloadsPage->update(); update(); });
    connect(&Ipfs, &IpfsModel::cidChanged, this, [this] { if (Current == Section::Downloads) DownloadsPage->update(); });
    connect(&PadRouter::instance(), &PadRouter::hintsChanged, this, [this] { update(QRect(0, height() - static_cast<int>(60 * scale()), width(), static_cast<int>(60 * scale()))); });
    connect(&PadInput::instance(), &PadInput::padsChanged, this, [this] { update(); });

    Clock = new QTimer(this);
    Clock->setInterval(15000);
    connect(Clock, &QTimer::timeout, this, [this] { update(); });

    rebuildAll();
}

DeckShell::~DeckShell()
{
    if (Active) setActive(false);
}

double DeckShell::scale() const { return DeckStyle::ScaleFor(height() > 0 ? height() : 800); }

QString DeckShell::SectionName(Section S)
{
    static const char *N[] = { "Home", "Library", "Catalog", "Downloads", "Friends", "Node", "Settings" };
    return N[static_cast<int>(S)];
}

std::optional<DeckShell::Section> DeckShell::SectionFrom(const QString &Name)
{
    for (int I = 0; I < SectionCount; ++I)
        if (SectionName(static_cast<Section>(I)).compare(Name, Qt::CaseInsensitive) == 0) return static_cast<Section>(I);
    return std::nullopt;
}

void DeckShell::rebuildAll()
{
    Library->rebuild();
    Catalog->rebuild();
    Home->refresh();
    DownloadsPage->refresh();
    if (gameOpen())
    {
        if (!card(Game->key())) closeGame();   // gone (removed, or no longer anything to download)
        else Game->refresh();
    }
    update();
}

LibraryGameCard *DeckShell::card(const QString &Key) const
{
    if (LibraryGameCard *C = Library->find(Key)) return C;
    return Catalog->find(Key);
}

bool DeckShell::installed(const QString &Key) const { return Library->find(Key) != nullptr; }
const QList<LibraryGameCard *> &DeckShell::libraryCards() const { return Library->cards(); }
const QList<LibraryGameCard *> &DeckShell::catalogCards() const { return Catalog->cards(); }

void DeckShell::showSection(Section S)
{
    if (gameOpen()) closeGame();
    if (menuOpen()) closeMenu();
    const int I = static_cast<int>(S);
    //A desktop page the first time it is shown.
    if (I >= 4 && !qobject_cast<DeckHosted *>(Body->widget(I)))
    {
        QWidget *Page = nullptr;
        if (S == Section::Friends) Page = new NetworkTab(Model);
        else if (S == Section::Node) Page = new IpfsTab(Ipfs);
        else Page = new SettingsTab(Model);
        auto *H = new DeckHosted(Page, scale(), Body);
        QWidget *Old = Body->widget(I);
        Body->insertWidget(I, H);
        Body->removeWidget(Old);
        Old->deleteLater();
    }
    if (auto *N = qobject_cast<DeckHosted *>(Body->widget(4))) qobject_cast<NetworkTab *>(N->page())->setActive(S == Section::Friends && Active);
    if (auto *N = qobject_cast<DeckHosted *>(Body->widget(5))) qobject_cast<IpfsTab *>(N->page())->setActive(S == Section::Node && Active);
    Ipfs.setActive(Active && (S == Section::Downloads || S == Section::Node));
    Current = S;
    Body->setCurrentIndex(I);
    focusSection();
    update();
    emit PadRouter::instance().hintsChanged();
}

void DeckShell::focusSection()
{
    QWidget *W = Body->currentWidget();
    if (auto *Sh = qobject_cast<DeckShelf *>(W)) { Sh->grid()->setFocus(Qt::OtherFocusReason); return; }
    if (auto *H = qobject_cast<DeckHosted *>(W))
    {
        //The first control in reading order on the page.
        QList<QWidget *> All = PadNav::Focusable(window());
        QWidget *First = nullptr;
        for (QWidget *C : All)
        {
            if (!H->isAncestorOf(C)) continue;
            const QRect R = PadNav::FocusRect(C), F = First ? PadNav::FocusRect(First) : QRect();
            if (!First || R.top() < F.top() || (R.top() == F.top() && R.left() < F.left())) First = C;
        }
        (First ? First : H)->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (W) W->setFocus(Qt::OtherFocusReason);
}

bool DeckShell::openGame(const QString &Key)
{
    if (!card(Key)) return false;
    if (menuOpen()) closeMenu();
    Game->showGame(Key);
    Game->setGeometry(Body->geometry());
    Game->show();
    Game->raise();
    Game->setFocus(Qt::OtherFocusReason);
    emit PadRouter::instance().hintsChanged();
    update();
    return true;
}

void DeckShell::closeGame()
{
    if (!gameOpen()) return;
    Game->hide();
    focusSection();
    //Focus back on the cover the page was opened from, where the shelf still shows it.
    if (auto *Sh = qobject_cast<DeckShelf *>(Body->currentWidget())) Sh->grid()->setCurrentKey(Game->key());
    emit PadRouter::instance().hintsChanged();
    update();
}

bool DeckShell::gameOpen() const { return Game && Game->isVisible(); }
void DeckShell::openMenu() { Menu->present(); update(); }
void DeckShell::closeMenu() { Menu->dismiss(); update(); }
bool DeckShell::menuOpen() const { return Menu && Menu->isVisible(); }

void DeckShell::setActive(bool On)
{
    if (Active == On) return;
    Active = On;
    ControllerOn = On;
    if (On)
    {
        NativeDialogsWere = QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        qApp->installEventFilter(this);
        Clock->start();
        //Covers as large as the shelf draws them (a TV draws them larger than the desktop's cards).
        Library->grid()->resize(Body->size());
        const QSizeF T = Library->grid()->tileSize();
        const QSize Have = CoverCache::instance()->displaySize();
        if (T.isValid() && (T.width() > Have.width() || T.height() > Have.height()))
            CoverCache::instance()->setDisplaySize(QSize(static_cast<int>(T.width()), static_cast<int>(T.height())).expandedTo(Have));
    }
    else
    {
        qApp->removeEventFilter(this);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, NativeDialogsWere);
        Clock->stop();
        if (menuOpen()) closeMenu();
    }
    showSection(Current);
}

void DeckShell::styleWindow(QWidget *W)
{
    if (W->property("deckStyled").toBool()) return;
    W->setProperty("deckStyled", true);
    const double S = scale();
    W->setStyleSheet(W->styleSheet() + DeckStyle::HostedStyleSheet(S));
    //The launch window is a screen of its own here; under Steam's Game Mode every window is (gamescope shows one
    //window at a time, scaled to the screen: a small dialog would come up blurred and huge).
    const bool Launch = W->inherits("PreLaunchWindow");
    new HintStrip(W, S);
    //The launch window opens on Play: A, and the game starts.
    if (Launch)
        if (auto *Play = W->findChild<QPushButton *>("playButton"))
            QTimer::singleShot(0, Play, [Play] { if (Play->isEnabled()) Play->setFocus(Qt::OtherFocusReason); });
    //What opens is where the controller goes: activated here, whatever the window manager would do (gamescope
    //activates a new window; a bare X server activates nothing).
    QTimer::singleShot(0, W, [W] { W->raise(); W->activateWindow(); });
    if (Launch || ControllerMode::InGameMode())
        QTimer::singleShot(0, W, [W] {
            W->setWindowState(W->windowState() | Qt::WindowFullScreen);
            if (W->screen()) W->setGeometry(W->screen()->geometry());   // where no window manager makes it so
        });
    else
    {
        //Centred over the shell, at least a third of it wide.
        QTimer::singleShot(0, W, [W, this] {
            const QSize Want = W->sizeHint().expandedTo(QSize(width() / 3, 0)).boundedTo(size());
            W->resize(Want.expandedTo(W->size()).boundedTo(size()));
            W->move(mapToGlobal(QPoint((width() - W->width()) / 2, (height() - W->height()) / 2)));
        });
    }
}

bool DeckShell::eventFilter(QObject *Watched, QEvent *Event)
{
    if (Event->type() == QEvent::Show && Watched->isWidgetType())
    {
        auto *W = static_cast<QWidget *>(Watched);
        if (W->isWindow() && W != window() && W->windowType() != Qt::Popup && W->windowType() != Qt::ToolTip && !W->property("padSkip").toBool()
            && (qobject_cast<QDialog *>(W) || W->inherits("PreLaunchWindow")))
            styleWindow(W);
    }
    return QWidget::eventFilter(Watched, Event);
}

void DeckShell::resizeEvent(QResizeEvent *)
{
    place();
    for (int I = 4; I < Body->count(); ++I)
        if (auto *H = qobject_cast<DeckHosted *>(Body->widget(I))) H->setScale(scale());
}

void DeckShell::place()
{
    const double S = scale();
    const int Top = static_cast<int>(64 * S), Bottom = static_cast<int>(58 * S);
    Body->setGeometry(0, Top, width(), std::max(0, height() - Top - Bottom));
    if (Game) Game->setGeometry(Body->geometry());
    if (Menu) Menu->setGeometry(rect());
}

QRectF DeckShell::tabRect(int I) const
{
    //After the mark and LB: each section's name in a capsule of its own (the whole header's height, for a tap).
    const double S = scale();
    const QFontMetricsF M(DeckStyle::Display(18, S));
    double X = 40 * S + 36 * S + 16 * S + DeckStyle::GlyphWidth(Pad::Button::LB, PadInput::instance().glyphs(), 26 * S) + 10 * S;
    for (int K = 0; K < I; ++K) X += M.horizontalAdvance(SectionName(static_cast<Section>(K))) + 30 * S + 4 * S;
    return QRectF(X, 0, M.horizontalAdvance(SectionName(static_cast<Section>(I))) + 30 * S, 64 * S);
}

void DeckShell::mouseReleaseEvent(QMouseEvent *E)
{
    //A tap on a section's title (the Deck's touch screen).
    for (int I = 0; I < SectionCount; ++I)
        if (tabRect(I).contains(E->position())) { showSection(static_cast<Section>(I)); return; }
}

QString DeckShell::battery() const
{
    const qint64 Now = QDateTime::currentMSecsSinceEpoch();
    if (Now - BatteryAt < 30000) return BatteryText;
    BatteryAt = Now;
    BatteryText.clear();
    //A laptop's or a Deck's battery (BAT0 / BAT1): its charge, and ⚡ while it charges.
    const QDir Supply("/sys/class/power_supply");
    for (const QString &B : Supply.entryList({ "BAT*" }, QDir::Dirs | QDir::NoDotAndDotDot))
    {
        QFile Cap(Supply.filePath(B + "/capacity")), St(Supply.filePath(B + "/status"));
        if (!Cap.open(QIODevice::ReadOnly)) continue;
        const QString C = QString::fromLatin1(Cap.readAll()).trimmed();
        const bool Charging = St.open(QIODevice::ReadOnly) && QString::fromLatin1(St.readAll()).trimmed() == "Charging";
        BatteryText = (Charging ? "⚡" : "") + C + "%";
        break;
    }
    return BatteryText;
}

QString DeckShell::statusText() const
{
    if (gameOpen()) return {};
    switch (Current)
    {
    case Section::Home:      return Home->statusText();
    case Section::Library:   return Library->statusText();
    case Section::Catalog:   return Catalog->statusText();
    case Section::Downloads: return DownloadsPage->statusText();
    default:                 return {};
    }
}

void DeckShell::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    P.setRenderHint(QPainter::Antialiasing);
    const double S = scale();
    QLinearGradient G(0, 0, 0, height());
    G.setColorAt(0, DeckStyle::Ground);
    G.setColorAt(1, DeckStyle::GroundLow);
    P.fillRect(rect(), G);
    const Pad::Glyphs Gl = PadInput::instance().glyphs();

    //The header: the mark, LB, the sections, RB; and on the right the node, the battery and the clock.
    const double Hh = 64 * S, Gh = 26 * S, Mh = 36 * S;
    if (const QPixmap &M = Mark(); !M.isNull()) P.drawPixmap(QRectF(40 * S, (Hh - Mh) / 2, Mh, Mh), M, QRectF(M.rect()));
    DeckStyle::DrawGlyph(P, QRectF(40 * S + Mh + 16 * S, (Hh - Gh) / 2, Gh, Gh), Pad::Button::LB, Gl);
    const QFont Tf = DeckStyle::Display(18, S);
    P.setFont(Tf);
    double LastRight = 0;
    for (int I = 0; I < SectionCount; ++I)
    {
        const QRectF R = tabRect(I);
        const bool On = static_cast<int>(Current) == I;
        const QRectF Pill(R.left(), Hh / 2 - 18 * S, R.width(), 36 * S);
        if (On) P.fillPath(DeckStyle::Capsule(Pill), DeckStyle::Lime);
        P.setPen(On ? DeckStyle::Ink : DeckStyle::Dim);
        P.drawText(Pill.translated(0, -1 * S), Qt::AlignCenter, SectionName(static_cast<Section>(I)));
        if (static_cast<Section>(I) == Section::Downloads && !Downloads.activeKeys().isEmpty())
        {
            //How many are coming down, on the section's title.
            const QString N = QString::number(Downloads.activeKeys().size());
            QFont Bf = DeckStyle::Font(11, S, QFont::Bold);
            P.setFont(Bf);
            const QRectF Bd(R.right() - 14 * S, Pill.top() - 6 * S, std::max(18 * S, QFontMetricsF(Bf).horizontalAdvance(N) + 9 * S), 18 * S);
            P.fillPath(DeckStyle::Capsule(Bd), DeckStyle::Magenta);
            P.setPen(Qt::white);
            P.drawText(Bd, Qt::AlignCenter, N);
            P.setFont(Tf);
        }
        LastRight = R.right();
    }
    DeckStyle::DrawGlyph(P, QRectF(LastRight + 10 * S, (Hh - Gh) / 2, Gh, Gh), Pad::Button::RB, Gl);

    QStringList Right;
    if (!Model.networkingEnabled()) Right << "Offline";
    else if (!IpfsWrapper::Available()) Right << "Starting…";
    else Right << QString("%1 peer%2").arg(Ipfs.nodeStatus().peers).arg(Ipfs.nodeStatus().peers == 1 ? "" : "s");
    if (const QString B = battery(); !B.isEmpty()) Right << B;
    Right << QDateTime::currentDateTime().toString("HH:mm");
    P.setFont(DeckStyle::Font(15, S, QFont::DemiBold));
    const QString Status = Right.join("     ");
    const QFontMetricsF Sm(P.font());
    const double Sw = Sm.horizontalAdvance(Status);
    const double Dot = 9 * S;
    P.setPen(DeckStyle::Text);
    P.drawText(QRectF(width() - 40 * S - Sw, 0, Sw + 2, Hh), Qt::AlignRight | Qt::AlignVCenter, Status);
    const QColor DotC = !Model.networkingEnabled() ? DeckStyle::Faint : IpfsWrapper::Available() ? DeckStyle::Good : DeckStyle::Gold;
    P.setPen(Qt::NoPen);
    P.setBrush(DotC);
    P.drawEllipse(QRectF(width() - 40 * S - Sw - Dot - 9 * S, Hh / 2 - Dot / 2, Dot, Dot));

    //The footer: the section's status on the left, what the buttons do on the right.
    const QRectF Foot(40 * S, height() - 58 * S, width() - 80 * S, 58 * S);
    P.fillRect(QRectF(0, Foot.top(), width(), Foot.height()), DeckStyle::GroundLow);
    DeckStyle::DrawSplitRule(P, QRectF(0, Foot.top(), width(), 2 * S));
    P.setFont(DeckStyle::Font(15, S));
    P.setPen(DeckStyle::Dim);
    const QString St = statusText();
    P.drawText(Foot, Qt::AlignLeft | Qt::AlignVCenter, St);
    QList<PadTarget::Hint> H = PadRouter::instance().hints();
    DeckStyle::DrawHints(P, Foot, H, Gl, S);
}

bool DeckShell::padPressed(Pad::Button B)
{
    using P = Pad::Button;
    switch (B)
    {
    case P::LB:
    case P::RB:
    {
        const int Step = B == P::RB ? 1 : -1;
        const int To = static_cast<int>(Current) + Step;
        if (To < 0 || To >= SectionCount) return true;   // at the end: the shoulder does nothing, nothing else either
        showSection(static_cast<Section>(To));
        PadInput::instance().tick();
        return true;
    }
    case P::Start: openMenu(); return true;
    case P::B:
        if (gameOpen()) { closeGame(); return true; }
        if (Current != Section::Home) { showSection(Section::Home); return true; }
        return false;
    default: return false;
    }
}

QList<PadTarget::Hint> DeckShell::padHints() const
{
    QList<Hint> H;
    if (gameOpen() || Current != Section::Home) H.push_back({ Pad::Button::B, "Back" });
    H.push_back({ Pad::Button::Start, "Menu" });
    return H;
}

json DeckShell::padState() const
{
    json J{{"kind", "shell"}, {"section", SectionName(Current).toStdString()}};
    if (gameOpen()) J["game"] = Game->padState();
    if (menuOpen()) J["menu"] = Menu->padState();
    switch (Current)
    {
    case Section::Home:      J["page"] = Home->padState(); break;
    case Section::Library:   J["page"] = Library->padState(); break;
    case Section::Catalog:   J["page"] = Catalog->padState(); break;
    case Section::Downloads: J["page"] = DownloadsPage->padState(); break;
    default: break;
    }
    J["status"] = statusText().toStdString();
    return J;
}
