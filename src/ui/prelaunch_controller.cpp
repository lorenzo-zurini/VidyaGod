// The launch window as controller mode shows it: a screen of its own in the shell's look rather than a desktop dialog
// made bigger. The same widgets and the same logic — every option, add-on, instance and version the desktop window
// has — arranged for a controller across a full screen:
//
//   ┌──────────┬──────────────────────────────────────────────┐
//   │          │ Title (display serif)                        │
//   │  cover   │ developer · year · edition                   │
//   │ (lifted) │ [ ▶ Play ]  [Edit package]  [Close]          │
//   │          │ Instance   [Main ◀▶] ⋯   Last played …       │
//   │ details  │ Version    [Arcade (MAME 0.289) ◀▶]          │
//   │ LAN      │ Runs with  [runner] → [runner]               │
//   │          │ (Options)(Add-ons)(Advanced)(Log)   LB/RB     │
//   │          │  every option, sections open                 │
//   │          │ status · progress                            │
//   └──────────┴──────────────────────────────────────────────┘
//
// Start plays from anywhere on it. Option sections are open (a controller cannot open a folded tree row), and a
// choice turns with left and right as a console's settings do.

#include "prelaunchwindow.h"

#include "apppaths.h"
#include "commonutils.h"
#include "controllerlayout.h"
#include "controllerservice.h"
#include "gameoverlay.h"
#include "layouteditor.h"
#include "deckstyle.h"

#include <QAbstractScrollArea>
#include <QGridLayout>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QTabBar>

#include <algorithm>
#include <cmath>

void PreLaunchWindow::ArrangeForController()
{
    DeckLayout = true;
    setProperty("deckLayout", true);
    DeckScale = DeckStyle::ScaleFor(screen() ? screen()->geometry().height() : 800);
    const double S = DeckScale;
    const auto Px = [S](double V) { return QString::number(std::lround(V * S)); };

    //The widgets the desktop layout put in its body and footer, taken out of them; the old containers then go.
    QWidget *OldBody = CoverLabel->parentWidget()->parentWidget();
    QWidget *OldFooter = LaunchButton->parentWidget();
    QWidget *InstanceRow = InstanceCombo->parentWidget();
    QWidget *ChainRow = ChainContainer->parentWidget();
    QWidget *StatusBox = StatusLabel->parentWidget();
    QPushButton *EditButton = OldFooter->findChild<QPushButton *>("editPackageButton");

    auto *Deck = new QWidget(this);
    Deck->setObjectName("deckLaunch");
    auto *Root = new QHBoxLayout(Deck);
    Root->setContentsMargins(40 * S, 30 * S, 40 * S, 14 * S);
    Root->setSpacing(44 * S);

    //Left: the cover, large; what the game is and the virtual LAN under it.
    const int CoverW = static_cast<int>(300 * S);
    auto *Left = new QWidget(Deck);
    Left->setFixedWidth(CoverW + static_cast<int>(8 * S));   // room for the cover's lifted shadow
    auto *LL = new QVBoxLayout(Left);
    LL->setContentsMargins(0, 0, 0, 0);
    LL->setSpacing(18 * S);
    CoverLabel->setParent(Left);
    CoverLabel->setFixedWidth(CoverW);
    LL->addWidget(CoverLabel, 0, Qt::AlignTop | Qt::AlignLeft);
    DetailsLabel->setParent(Left);
    LL->addWidget(DetailsLabel);
    LanGroup->setParent(Left);
    LL->addWidget(LanGroup);
    LL->addStretch(1);
    Root->addWidget(Left, 0);

    //Right: the game's name, Play, how it is set up, and its options.
    auto *Right = new QWidget(Deck);
    auto *RL = new QVBoxLayout(Right);
    RL->setContentsMargins(0, 0, 0, 0);
    RL->setSpacing(10 * S);
    TitleLabel->setParent(Right);
    TitleLabel->setObjectName("deckTitle");
    TitleLabel->setFont(DeckStyle::Display(40, S));
    RL->addWidget(TitleLabel);
    MetaLabel->setParent(Right);
    RL->addWidget(MetaLabel);
    RL->addSpacing(10 * S);

    auto *Actions = new QHBoxLayout();
    Actions->setSpacing(14 * S);
    for (QPushButton *B : { LaunchButton, KillButton, EditButton, CloseButton })
        if (B) { B->setParent(Right); Actions->addWidget(B); }
    Actions->addStretch(1);
    RL->addLayout(Actions);
    RL->addSpacing(12 * S);

    auto *Setup = new QGridLayout();
    Setup->setHorizontalSpacing(18 * S);
    Setup->setVerticalSpacing(10 * S);
    const auto Field = [&](const QString &T) {
        auto *L = new QLabel(T, Right);
        L->setProperty("role", "field");
        return L;
    };
    InstanceRow->setParent(Right);
    Setup->addWidget(Field("Instance"), 0, 0);
    Setup->addWidget(InstanceRow, 0, 1);
    QLabel *Version = Field("Version");
    const bool Versions = !VariantCombo->isHidden();
    VariantCombo->setParent(Right);
    VariantCombo->setVisible(Versions);
    Version->setVisible(Versions);
    VariantLabel = Version;   // what hides with the combo when there is one version only (the old one goes below)
    Setup->addWidget(Version, 1, 0);
    //As wide as its longest name, within the column: a version's name is what tells versions apart.
    VariantCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    VariantCombo->setMinimumContentsLength(28);
    VariantCombo->setMaximumWidth(static_cast<int>(640 * S));
    Setup->addWidget(VariantCombo, 1, 1, Qt::AlignLeft);
    ChainRow->setParent(Right);
    Setup->addWidget(Field("Runs with"), 2, 0);
    Setup->addWidget(ChainRow, 2, 1);
    LayoutRow->setParent(Right);
    Setup->addWidget(Field("Controller"), 3, 0);
    Setup->addWidget(LayoutRow, 3, 1);
    Setup->setColumnStretch(1, 1);
    RL->addLayout(Setup);
    RL->addSpacing(14 * S);

    Tabs->setParent(Right);
    Tabs->tabBar()->setFont(DeckStyle::Display(18, S));
    RL->addWidget(Tabs, 1);
    StatusBox->setParent(Right);
    RL->addWidget(StatusBox);
    Root->addWidget(Right, 1);

    layout()->addWidget(Deck);
    delete OldBody;
    delete OldFooter;

    //Choices turn with left and right (the generic navigation's "padAdjust").
    for (QComboBox *C : { InstanceCombo, VariantCombo }) C->setProperty("padAdjust", true);
    if (OptionsTree) OptionsTree->expandAll();
    if (ModuleTree) ModuleTree->expandAll();

    //Its look: the shell's. Sizes in px at this screen's scale; colours by name.
    QString Sheet = QStringLiteral(
        "#prelaunch { background: transparent; }"
        "#prelaunch QLabel { color: @Text; }"
        "#prelaunch QLabel[role=\"dim\"], #prelaunch QLabel[role=\"field\"] { color: @Dim; }"
        "#prelaunch QLabel[role=\"field\"] { font-size: @f16px; }"
        "#prelaunch QLabel#deckTitle { font-size: @f40px; }"
        "#prelaunch QTabWidget::pane { border: none; background: transparent; top: @f10px; }"
        "#prelaunch QTabBar::tab { font-size: @f18px; padding: @f6px @f20px; margin-right: @f6px; border-radius: @f17px;"
        " color: @Dim; background: transparent; }"
        "#prelaunch QTabBar::tab:selected { background: @Lime; color: @Ink; }"
        "#prelaunch QTreeWidget { border: none; background: transparent; }"
        "#prelaunch QTreeWidget::item { padding: @f4px 0px; }"
        "#prelaunch QToolButton#instanceMenu::menu-indicator { image: none; width: 0px; }"
        "#prelaunch #playButton { background: @Lime; color: @Ink; font-size: @f22px; font-weight: 800; min-width: @f220px;"
        " min-height: @f52px; padding: 0px @f36px; border-radius: @f26px; border: none; }"
        "#prelaunch #playButton:disabled { background: @Panel; color: @Faint; }"
        "#prelaunch #stopButton { background: @Bad; color: white; font-size: @f22px; font-weight: 800; min-width: @f220px;"
        " min-height: @f52px; padding: 0px @f36px; border-radius: @f26px; border: none; }"
        "#prelaunch QProgressBar { max-height: @f6px; border: none; background: @Panel; border-radius: @f3px; }"
        "#prelaunch QProgressBar::chunk { background: @LimeHi; border-radius: @f3px; }"
        "#prelaunch QGroupBox { color: @Dim; border: 1px solid @Panel; border-radius: @f12px; margin-top: @f20px;"
        " padding-top: @f12px; }"
        "#prelaunch QGroupBox::title { subcontrol-origin: margin; left: @f12px; }");
    static const QRegularExpression Size("@f(\\d+)");
    for (QRegularExpressionMatch M = Size.match(Sheet); M.hasMatch(); M = Size.match(Sheet))
        Sheet.replace(M.capturedStart(), M.capturedLength(), Px(M.captured(1).toDouble()));
    const std::pair<const char *, QColor> Named[] = { { "@Text", DeckStyle::Text }, { "@Dim", DeckStyle::Dim }, { "@Faint", DeckStyle::Faint },
        { "@PanelHi", DeckStyle::PanelHi }, { "@Panel", DeckStyle::Panel }, { "@LimeHi", DeckStyle::LimeHi }, { "@Lime", DeckStyle::Lime },
        { "@Ink", DeckStyle::Ink }, { "@Bad", DeckStyle::Bad } };
    for (const auto &[Name, C] : Named) Sheet.replace(QLatin1String(Name), C.name());
    setStyleSheet(Sheet);
    UpdateCoverScaled();
}

void PreLaunchWindow::paintEvent(QPaintEvent *Event)
{
    if (!DeckLayout) { QDialog::paintEvent(Event); return; }
    //The shell's ground, the cover washed in the mark's two tones over it, dark where the text is.
    QPainter P(this);
    QLinearGradient G(0, 0, 0, height());
    G.setColorAt(0, DeckStyle::Ground);
    G.setColorAt(1, DeckStyle::GroundLow);
    P.fillRect(rect(), G);
    if (!DeckWash.isNull())
    {
        P.save();
        P.setRenderHint(QPainter::SmoothPixmapTransform);
        P.setOpacity(0.45);
        const QSizeF Src = DeckWash.size();
        const double K = std::max(width() / Src.width(), height() / Src.height());
        P.drawPixmap(QRectF(width() / 2.0 - Src.width() * K / 2, height() / 2.0 - Src.height() * K / 2, Src.width() * K, Src.height() * K),
                     DeckWash, QRectF(QPointF(0, 0), Src));
        P.restore();
    }
    const auto Ground = [](int A) { QColor C = DeckStyle::Ground; C.setAlpha(A); return C; };
    QLinearGradient L(0, 0, width(), 0);
    L.setColorAt(0, Ground(150));
    L.setColorAt(0.45, Ground(205));
    L.setColorAt(1, Ground(150));
    P.fillRect(rect(), L);
    //The cover lifted off its magenta shadow.
    if (CoverLabel && CoverLabel->pixmap().isNull() == false && CoverLabel->isVisible())
    {
        const QRect C(CoverLabel->mapTo(this, QPoint(0, 0)), CoverLabel->size());
        QPainterPath Shape;
        Shape.addRoundedRect(QRectF(C), 10 * DeckScale, 10 * DeckScale);
        DeckStyle::DrawLift(P, Shape, DeckScale * 1.6);
    }
}

namespace
{
//The next shown tab of T from its current one, By (−1 or +1) at a time, round past either end; −1 when it is the only
//one shown.
int NextTab(const QTabWidget *T, int By)
{
    const int N = T->count();
    for (int Step = 1; Step < N; ++Step)
    {
        const int I = ((T->currentIndex() + By * Step) % N + N) % N;
        if (T->isTabVisible(I)) return I;
    }
    return -1;
}
}

bool PreLaunchWindow::padPressed(Pad::Button B)
{
    if (!DeckLayout) return false;
    //Start plays, wherever the focus is on the screen: the one thing the screen is for.
    if (B == Pad::Button::Start)
    {
        if (!LaunchButton || !LaunchButton->isVisible() || !LaunchButton->isEnabled()) return false;
        LaunchButton->click();
        return true;
    }
    //The shoulders turn the tabs (Options, Add-ons, Advanced, Log) from anywhere on it, as the shell's sections turn;
    //the focus goes along to the page's first control. They go round, so each always names where it goes.
    if ((B == Pad::Button::LB || B == Pad::Button::RB) && Tabs)
    {
        const int To = NextTab(Tabs, B == Pad::Button::RB ? 1 : -1);
        if (To < 0) return true;   // the only tab: nothing to turn to, and nothing else takes the press
        Tabs->setCurrentIndex(To);
        const QList<QWidget *> On = PadNav::Focusable(Tabs->currentWidget());
        //A page with nothing to press (the Log) takes the focus on what scrolls, so the right stick scrolls it.
        QWidget *Page = Tabs->currentWidget();
        QAbstractScrollArea *Scroll = Page->findChild<QAbstractScrollArea *>();
        if (!On.isEmpty()) On.first()->setFocus(Qt::OtherFocusReason);
        else (Scroll ? static_cast<QWidget *>(Scroll) : Page)->setFocus(Qt::OtherFocusReason);
        return true;
    }
    return false;
}

QList<PadTarget::Hint> PreLaunchWindow::padHints() const
{
    if (!DeckLayout) return {};
    QList<PadTarget::Hint> Out;
    //Two tabs: both shoulders go to the other one; RB alone says so.
    if (Tabs && NextTab(Tabs, -1) >= 0 && NextTab(Tabs, -1) != NextTab(Tabs, 1))
        Out.push_back({ Pad::Button::LB, Tabs->tabText(NextTab(Tabs, -1)) });
    if (Tabs && NextTab(Tabs, 1) >= 0) Out.push_back({ Pad::Button::RB, Tabs->tabText(NextTab(Tabs, 1)) });
    if (LaunchButton && LaunchButton->isVisible() && LaunchButton->isEnabled()) Out.push_back({ Pad::Button::Start, "Play" });
    return Out;
}

// ---- The game's controller layout -----------------------------------------------------------------------------------

namespace { std::string LayoutsDir() { return (AppPaths::DataRoot() / "ControllerLayouts").string(); } }

QString PreLaunchWindow::LayoutKey() const { return QString::fromStdString(Face(CurrentLaunch())); }

void PreLaunchWindow::RefreshLayoutRow()
{
    if (!LayoutNameLabel) return;
    const std::string Name = ControllerLayout::ForGame(LayoutsDir(), LayoutKey().toStdString());
    LayoutNameLabel->setText(Name.empty() ? tr("Gamepad (the controller as it is)") : QString::fromStdString(Name));
}

void PreLaunchWindow::EditLayout()
{
    LayoutEditor E(QString::fromStdString(LayoutsDir()), LayoutKey(), TitleLabel ? TitleLabel->text() : QString(), this);
    E.exec();
    RefreshLayoutRow();
}

void PreLaunchWindow::ApplyGameLayout(bool On)
{
    ControllerService &S = ControllerService::instance();
    //The overlay's game: this one while it runs (its Quit ends it as Stop does).
    if (On) GameOverlay::instance().setGame(TitleLabel ? TitleLabel->text() : QString(), LayoutKey(), [this] { if (KillButton->isVisible()) onKillClicked(); });
    else GameOverlay::instance().clearGame();
    if (!On) { if (!S.gameLayout().isEmpty()) S.setGameLayout({}); return; }
    //Its own layout, or the controller as it is — never VidyaGod's own (the menus'), which waits under it.
    const std::string Name = ControllerLayout::ForGame(LayoutsDir(), LayoutKey().toStdString());
    if (!Name.empty() && !ControllerLayout::Named(LayoutsDir(), Name))
        LogWarn("PreLaunchWindow", "the controller layout '" + Name + "' this game uses is gone: the controller as it is");
    const ControllerLayout::Layout L = ControllerLayout::GameLayout(LayoutsDir(), LayoutKey().toStdString());
    QString Error;
    if (!S.setGameLayout(QString::fromStdString(L.toProfile().dump()), &Error))
        LogWarn("PreLaunchWindow", "the controller refused the layout '" + L.Name + "': " + Error.toStdString());
}
