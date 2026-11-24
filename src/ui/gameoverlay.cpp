#include "gameoverlay.h"

#include "apppaths.h"
#include "controllerlayout.h"
#include "controllerservice.h"
#include "deckstyle.h"
#include "layouteditor.h"
#include "padinput.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#ifdef __linux__
#include <QtGui/qguiapplication_platform.h>
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>
#endif

std::vector<std::pair<const char *, unsigned>> OverlayProperties(bool Open)
{
    return { { "STEAM_OVERLAY", 1u }, { "STEAM_INPUT_FOCUS", Open ? 1u : 0u }, { "_NET_WM_WINDOW_OPACITY", Open ? 0xFFFFFFFFu : 0u } };
}

namespace {
//Sets 32-bit CARDINAL properties on a window, when the app is on X11 itself.
void SetCardinals(QWidget *W, const std::vector<std::pair<const char *, unsigned>> &Props)
{
#ifdef __linux__
    //Only on X11 itself: the offscreen platform answers for X11 too, with no connection behind it.
    if (!W || QGuiApplication::platformName() != QLatin1String("xcb")) return;
    auto *X = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    xcb_connection_t *C = X ? X->connection() : nullptr;
    if (!C) return;
    const auto Win = static_cast<xcb_window_t>(W->winId());
    for (const auto &[Name, Value] : Props)
    {
        xcb_intern_atom_reply_t *R = xcb_intern_atom_reply(C, xcb_intern_atom(C, 0, static_cast<uint16_t>(std::strlen(Name)), Name), nullptr);
        if (!R) continue;
        const uint32_t V = Value;
        xcb_change_property(C, XCB_PROP_MODE_REPLACE, Win, R->atom, XCB_ATOM_CARDINAL, 32, 1, &V);
        std::free(R);
    }
    xcb_flush(C);
#else
    (void)W; (void)Props;
#endif
}
}

void MarkExternalOverlay(QWidget *W) { SetCardinals(W, { { "GAMESCOPE_EXTERNAL_OVERLAY", 1u } }); }

void MarkOverlayWindow(QWidget *W, bool Open) { SetCardinals(W, OverlayProperties(Open)); }

bool PadButtonFromUiEvent(const QString &E, Pad::Button &B)
{
    static const std::pair<const char *, Pad::Button> Map[] = {
        { "ui_accept", Pad::Button::A }, { "ui_back", Pad::Button::B }, { "ui_action", Pad::Button::X }, { "ui_context", Pad::Button::Y },
        { "ui_up", Pad::Button::Up }, { "ui_down", Pad::Button::Down }, { "ui_left", Pad::Button::Left }, { "ui_right", Pad::Button::Right },
        { "ui_l1", Pad::Button::LB }, { "ui_r1", Pad::Button::RB }, { "ui_l2", Pad::Button::LT }, { "ui_r2", Pad::Button::RT },
        { "ui_l3", Pad::Button::L3 }, { "ui_r3", Pad::Button::R3 }, { "ui_option", Pad::Button::Start }, { "ui_select", Pad::Button::Back },
        { "ui_guide", Pad::Button::Guide },
    };
    for (const auto &[Name, Button] : Map)
        if (E == QLatin1String(Name)) { B = Button; return true; }
    return false;
}

namespace { std::string LayoutsDir() { return (AppPaths::DataRoot() / "ControllerLayouts").string(); } }

GameOverlay &GameOverlay::instance()
{
    static GameOverlay *O = new GameOverlay();   // a top-level window for the app's life
    return *O;
}

GameOverlay::GameOverlay() : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setObjectName(QStringLiteral("gameOverlay"));
    setWindowTitle(tr("VidyaGod overlay"));
    setAttribute(Qt::WA_TranslucentBackground);

    auto *Root = new QVBoxLayout(this);
    Root->addStretch(1);
    auto *PanelW = new QWidget(this);
    PanelW->setObjectName(QStringLiteral("overlayPanel"));
    PanelW->setMaximumWidth(760);
    auto *P = new QVBoxLayout(PanelW);
    P->setSpacing(14);
    Title = new QLabel(PanelW);
    Title->setObjectName(QStringLiteral("overlayTitle"));
    Title->setFont(DeckStyle::Display(36, 1.0));
    Title->setWordWrap(true);
    P->addWidget(Title);
    auto *Info = new QHBoxLayout;
    Clock = new QLabel(PanelW);
    Battery = new QLabel(PanelW);
    Info->addWidget(Clock);
    Info->addStretch(1);
    Info->addWidget(Battery);
    P->addLayout(Info);
    Resume = new QPushButton(tr("Resume"), PanelW);
    Resume->setObjectName(QStringLiteral("resume"));
    P->addWidget(Resume);
    auto *LayoutRow = new QHBoxLayout;
    auto *LL = new QLabel(tr("Controller layout"), PanelW);
    Layouts = new QComboBox(PanelW);
    Layouts->setObjectName(QStringLiteral("overlayLayouts"));
    Layouts->setProperty("padAdjust", true);   // left and right switch it, live
    Layouts->setMinimumContentsLength(24);
    EditButton = new QPushButton(tr("Edit…"), PanelW);
    EditButton->setObjectName(QStringLiteral("overlayEditLayout"));
    LayoutRow->addWidget(LL);
    LayoutRow->addWidget(Layouts, 1);
    LayoutRow->addWidget(EditButton);
    P->addLayout(LayoutRow);
    //A keyboard for the game: the field opens VidyaGod's on-screen keyboard (as every text field does with A), Type sends
    //it to the game as key presses.
    auto *TypeRow = new QHBoxLayout;
    TypeEdit = new QLineEdit(PanelW);
    TypeEdit->setObjectName(QStringLiteral("overlayTypeText"));
    TypeEdit->setPlaceholderText(tr("Type into the game…"));
    TypeEnter = new QCheckBox(tr("and press Enter"), PanelW);
    TypeEnter->setObjectName(QStringLiteral("overlayTypeEnter"));
    TypeEnter->setChecked(true);
    TypeButton = new QPushButton(tr("Type"), PanelW);
    TypeButton->setObjectName(QStringLiteral("overlayType"));
    TypeRow->addWidget(TypeEdit, 1);
    TypeRow->addWidget(TypeEnter);
    TypeRow->addWidget(TypeButton);
    P->addLayout(TypeRow);
    QuitButton = new QPushButton(tr("Quit game"), PanelW);
    QuitButton->setObjectName(QStringLiteral("quitGame"));
    P->addWidget(QuitButton);
    auto *Center = new QHBoxLayout;
    Center->addStretch(1);
    Center->addWidget(PanelW, 3);
    Center->addStretch(1);
    Root->addLayout(Center);
    Root->addStretch(1);
    //What the buttons do here, as the controller labels them.
    Hints = new QLabel(this);
    Hints->setObjectName(QStringLiteral("overlayHints"));
    Hints->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    Root->addWidget(Hints);

    using namespace DeckStyle;
    setStyleSheet(QStringLiteral(
        "#gameOverlay QLabel { color: %1; font-size: 18px; }"
        "#gameOverlay QLabel#overlayTitle { color: %1; font-size: 46px; }"
        "#gameOverlay QLabel#overlayHints { color: %6; font-size: 18px; }"
        "#gameOverlay QPushButton { background: %2; color: %1; border: none; border-radius: 24px; padding: 12px 30px; font-size: 20px; }"
        "#gameOverlay QPushButton:focus { background: %3; color: %4; }"
        "#gameOverlay QPushButton#quitGame:focus { background: %5; color: %4; }"
        "#gameOverlay QComboBox { background: %2; color: %1; border: 3px solid transparent; border-radius: 20px; padding: 6px 18px; font-size: 18px; }"
        "#gameOverlay QComboBox:focus { border-color: %3; }"
        "#gameOverlay QLineEdit { background: %2; color: %1; border: 3px solid transparent; border-radius: 20px; padding: 6px 18px; font-size: 18px; }"
        "#gameOverlay QLineEdit:focus { border-color: %3; }"
        "#gameOverlay QCheckBox { color: %1; font-size: 18px; }")
        .arg(Text.name(), Panel.name(), Lime.name(), Ink.name(), Bad.name(), Dim.name()));

    connect(Resume, &QPushButton::clicked, this, &GameOverlay::closeOverlay);
    connect(EditButton, &QPushButton::clicked, this, &GameOverlay::editLayout);
    connect(QuitButton, &QPushButton::clicked, this, &GameOverlay::quitGame);
    connect(TypeButton, &QPushButton::clicked, this, [this] {
        QString E;
        if (!typeIntoGame(TypeEdit->text(), &E)) Battery->setText(E);
    });
    connect(Layouts, &QComboBox::currentIndexChanged, this, [this](int) {
        if (Layouts->signalsBlocked()) return;
        QString E;
        if (!setLayout(Layouts->currentData().toString(), &E)) Battery->setText(E);
    });
    Tick = new QTimer(this);
    Tick->setInterval(30000);
    connect(Tick, &QTimer::timeout, this, &GameOverlay::refresh);
}

void GameOverlay::attach()
{
    if (Attached) return;
    Attached = true;
    connect(&ControllerService::instance(), &ControllerService::uiEvent, this, &GameOverlay::onUiEvent);
    connect(&ControllerService::instance(), &ControllerService::gameChanged, this, &GameOverlay::onGameChanged);
}

void GameOverlay::setGame(const QString &Title, const QString &Key, std::function<void()> Quit)
{
    GameTitle = Title;
    GameKey = Key;
    QuitGame = std::move(Quit);
    if (Open) refresh();
}

void GameOverlay::clearGame()
{
    GameTitle.clear();
    GameKey.clear();
    QuitGame = nullptr;
    closeOverlay();
}

QString GameOverlay::title() const { return Title->text(); }
QString GameOverlay::layoutName() const { return Layouts->currentData().toString(); }
bool GameOverlay::quitArmed() const { return QuitButton->property("armed").toBool(); }

void GameOverlay::refresh()
{
    const bool R = hasGame();
    Title->setText(R ? GameTitle : tr("No game is running"));
    Clock->setText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    //The battery, when the machine has one (a handheld): the first BAT* the kernel lists.
    QString Bat;
    for (const QString &D : QDir(QStringLiteral("/sys/class/power_supply")).entryList({ QStringLiteral("BAT*") }, QDir::Dirs | QDir::NoDotAndDotDot))
    {
        QFile F(QStringLiteral("/sys/class/power_supply/") + D + QStringLiteral("/capacity"));
        if (F.open(QIODevice::ReadOnly)) { Bat = tr("Battery %1%").arg(QString::fromLatin1(F.readAll()).trimmed()); break; }
    }
    Battery->setText(Bat);
    const Pad::Glyphs G = PadInput::instance().glyphs();
    Hints->setText(tr("%1 Select     %2 Resume     %3 Resume").arg(DeckStyle::GlyphText(Pad::Button::A, G), DeckStyle::GlyphText(Pad::Button::B, G),
                                                                   DeckStyle::GlyphText(Pad::Button::Guide, G)));
    const std::string Chosen = R ? ControllerLayout::ForGame(LayoutsDir(), GameKey.toStdString()) : std::string();
    Layouts->blockSignals(true);
    Layouts->clear();
    for (const auto &T : ControllerLayout::Templates()) Layouts->addItem(QString::fromStdString(T.Name), QString::fromStdString(T.Name));
    for (const auto &L : ControllerLayout::Saved(LayoutsDir())) Layouts->addItem(QString::fromStdString(L.Name), QString::fromStdString(L.Name));
    Layouts->setCurrentIndex(std::max(0, Layouts->findData(Chosen.empty() ? QStringLiteral("Gamepad") : QString::fromStdString(Chosen))));
    Layouts->blockSignals(false);
    Layouts->setEnabled(R);
    TypeEdit->setEnabled(R);
    TypeButton->setEnabled(R);
    EditButton->setEnabled(R);
    QuitButton->setEnabled(R);
}

bool GameOverlay::setLayout(const QString &Name, QString *Error)
{
    if (!hasGame()) { if (Error) *Error = tr("No game is running"); return false; }
    const auto L = ControllerLayout::Named(LayoutsDir(), Name.toStdString());
    if (!L) { if (Error) *Error = tr("No layout %1").arg(Name); return false; }
    const bool AsIs = Name == QLatin1String("Gamepad");
    ControllerLayout::SetForGame(LayoutsDir(), GameKey.toStdString(), AsIs ? std::string() : Name.toStdString());
    //As it is, too, is the game's layer (Gamepad's profile): never VidyaGod's own, under it.
    if (!ControllerService::instance().setGameLayout(QString::fromStdString(L->toProfile().dump()), Error)) return false;
    Layouts->blockSignals(true);
    Layouts->setCurrentIndex(std::max(0, Layouts->findData(Name)));
    Layouts->blockSignals(false);
    return true;
}

void GameOverlay::editLayout()
{
    if (!hasGame()) return;
    LayoutEditor E(QString::fromStdString(LayoutsDir()), GameKey, GameTitle, this);
    MarkOverlayWindow(&E, true);   // over the game too, under gamescope
    E.exec();
    //What the game uses may have changed (chosen, or edited): on the controller now.
    refresh();
    setLayout(Layouts->currentData().toString());
}

void GameOverlay::quitGame()
{
    if (!quitArmed())
    {
        QuitButton->setProperty("armed", true);
        QuitButton->setText(tr("Press again to quit"));
        return;
    }
    if (QuitGame) QuitGame();
    closeOverlay();
}

bool GameOverlay::typeIntoGame(const QString &Text, QString *Error)
{
    if (!hasGame()) { if (Error) *Error = tr("No game is running"); return false; }
    if (Text.isEmpty() && !TypeEnter->isChecked()) return true;
    //Back to the game first: the keys go to whatever has the keyboard.
    closeOverlay();
    if (!ControllerService::instance().typeText(TypeEnter->isChecked() ? Text + QLatin1Char('\n') : Text, Error)) return false;
    TypeEdit->clear();
    return true;
}

void GameOverlay::openOverlay()
{
    if (Open) return;
    refresh();
    if (QScreen *S = QGuiApplication::primaryScreen()) setGeometry(S->geometry());
    winId();                        // a native window first: gamescope reads the overlay properties as it maps
    MarkOverlayWindow(this, true);
    show();
    raise();
    activateWindow();
    Resume->setFocus();
    Open = true;
    Tick->start();
}

void GameOverlay::closeOverlay()
{
    if (!Open) return;
    Open = false;
    QuitButton->setProperty("armed", false);
    QuitButton->setText(tr("Quit game"));
    Tick->stop();
    MarkOverlayWindow(this, false);
    hide();
    //Back to the game: only the Guide button intercepted again.
    if (ControllerService::instance().wants(ControllerService::Need::Game)) ControllerService::instance().setIntercept(1);
}

void GameOverlay::onUiEvent(const QString &Event, double Value)
{
    if (Value < 0.5) return;   // presses; their releases follow at once (inject)
    if (Event == QLatin1String("ui_guide")) { Open ? closeOverlay() : openOverlay(); return; }
    if (!Open) return;
    Pad::Button B;
    if (PadButtonFromUiEvent(Event, B)) PadInput::instance().inject(B);
}

void GameOverlay::onGameChanged(bool Running)
{
    if (Running) { ControllerService::instance().setIntercept(1); return; }
    closeOverlay();
    ControllerService::instance().setIntercept(0);
}

bool GameOverlay::padPressed(Pad::Button B)
{
    if (B == Pad::Button::B || B == Pad::Button::Guide) { closeOverlay(); return true; }
    return false;
}

QList<PadTarget::Hint> GameOverlay::padHints() const
{
    return { { Pad::Button::A, tr("Select") }, { Pad::Button::B, tr("Resume") } };
}

void GameOverlay::keyPressEvent(QKeyEvent *E)
{
    if (E->key() == Qt::Key_Escape) { closeOverlay(); return; }
    QWidget::keyPressEvent(E);
}

void GameOverlay::paintEvent(QPaintEvent *)
{
    QPainter P(this);
    QColor G = DeckStyle::Ground;
    G.setAlpha(236);
    P.fillRect(rect(), G);
    //The mark's split rule along the top: lime, then magenta.
    P.fillRect(QRect(0, 0, width() / 2, 4), DeckStyle::Lime);
    P.fillRect(QRect(width() / 2, 0, width() - width() / 2, 4), DeckStyle::Magenta);
}
