#include "guiformat.h"   // PlainLabel
#include "installed.h"
#include "theme.h"   // the named colours
#include "shelf.h"
#include "prelaunchwindow.h"
#include "fold.h"   // a row's options and entries are its resolution's
#include "apppaths.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "launchsession.h"     // ResolveTakingBack — what the game changed, read back before the values show
#include "launchresolver.h"     // ResolveChainIds / ResolveChainTail / kNativeTerminalId (runner daisy-chain UI)
#include "jsonoperations.h"
#include "ipfswrapper.h"     // LanPeers/SetLanExcluded/LanLaunchVars — the Virtual LAN panel
#include "variantpicker.h"     // NaturalLess — deterministic version-aware variant ordering
#include "instancestore.h"     // the instance picker
#include "asyncwork.h"        // the size under the cover, counted off the GUI thread

#include <set>
#include <algorithm>

#include <QBrush>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFont>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QGridLayout>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include "keycapture.h"
#include <QPainterPath>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QPointer>

#include "prelaunchwindow_p.h"

namespace PreLaunchUi {

//The graft rows of the graft tree, depth-first (section rows carry no key) — the order grafts apply in.
std::vector<QTreeWidgetItem*> GraftRows(QTreeWidget* T)
{
    std::vector<QTreeWidgetItem*> Out;
    if (!T) return Out;
    for (QTreeWidgetItemIterator It(T); *It; ++It)
        if (!(*It)->data(0, Qt::UserRole).toString().isEmpty()) Out.push_back(*It);
    return Out;
}

//The row of a SECTION path ("UserPatch/Interface": '/' nests) — created on first use under its parent section,
//collapsed; "" is the tree's root. Sections appear in the order their first entry does.
QTreeWidgetItem* SectionRow(QTreeWidget* T, std::map<std::string, QTreeWidgetItem*>& Made, const std::string& Path)
{
    if (Path.empty()) return T->invisibleRootItem();
    if (const auto It = Made.find(Path); It != Made.end()) return It->second;
    const size_t Slash = Path.rfind('/');
    QTreeWidgetItem* Parent = SectionRow(T, Made, Slash == std::string::npos ? std::string() : Path.substr(0, Slash));
    auto* S = new QTreeWidgetItem(Parent);
    S->setText(0, QString::fromStdString(Slash == std::string::npos ? Path : Path.substr(Slash + 1)));
    QFont F = S->font(0); F.setBold(true); S->setFont(0, F);
    S->setFlags(Qt::ItemIsEnabled);
    S->setFirstColumnSpanned(true);
    S->setData(0, Qt::UserRole + 10, QString::fromStdString(Path));   // a section row
    S->setExpanded(false);
    Made[Path] = S;
    return S;
}

} // namespace PreLaunchUi

namespace {
//The cover with rounded corners, at the given width (its height follows its aspect).
QPixmap RoundedCover(const QPixmap& Src, int Width, qreal Dpr)
{
    if (Src.isNull() || Width <= 0) return QPixmap();
    const QPixmap Scaled = Src.scaledToWidth((int)(Width * Dpr), Qt::SmoothTransformation);
    QPixmap Out(Scaled.size());
    Out.fill(Qt::transparent);
    QPainter P(&Out);
    P.setRenderHint(QPainter::Antialiasing);
    QPainterPath Clip;
    Clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(Scaled.size())), 8 * Dpr, 8 * Dpr);
    P.setClipPath(Clip);
    P.drawPixmap(0, 0, Scaled);
    P.end();
    Out.setDevicePixelRatio(Dpr);
    return Out;
}
}


// ============================================================================
// PreLaunchWindow — node-native launch dialog (one library tile = a GROUP of launchable nodes).
// ============================================================================

PreLaunchWindow::PreLaunchWindow(
    nlohmann::ordered_json*  GlobalConfigJSON,
    std::shared_ptr<const NodeIndex> Graph,
    std::vector<std::string> GroupNodeIds,
    std::string              FaceUid,
    QWidget*                 parent)
    : QDialog(parent)
    , GlobalConfigJSON(GlobalConfigJSON)
    , IndexOwned(std::move(Graph))
    , Index(IndexOwned.get())
    , GroupNodeIds(std::move(GroupNodeIds))
    , FaceUid(std::move(FaceUid))
{
    setWindowTitle("Launch");
    setMinimumSize(760, 520);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("prelaunch");
    //One quiet, consistent look over the desktop palette: secondary text dimmed, cards a shade off the window, the
    //primary action in the highlight colour. palette() keeps it right in light and dark themes alike.
    setStyleSheet(
        "#prelaunch QLabel[role=\"dim\"] { color: palette(placeholder-text); }"
        "#prelaunch QLabel[role=\"title\"] { font-size: 17pt; font-weight: 600; }"
        "#prelaunch QLabel[role=\"field\"] { color: palette(placeholder-text); }"
        "#prelaunch QTabWidget::pane { border: 1px solid palette(mid); border-radius: 6px; top: -1px; background: palette(base); }"
        "#prelaunch QTabBar::tab { padding: 5px 14px; border: none; margin-right: 2px; color: palette(placeholder-text); }"
        "#prelaunch QTabBar::tab:selected { color: palette(text); border-bottom: 2px solid palette(highlight); }"
        "#prelaunch QTreeWidget { border: none; background: transparent; }"
        "#prelaunch QTreeWidget::item { padding: 3px 0px; }"
        "#prelaunch QToolButton#instanceMenu::menu-indicator { image: none; width: 0px; }"
        "#prelaunch QToolButton#instanceMenu { padding: 2px 8px; font-weight: 600; }"
        "#prelaunch #footer { background: palette(alternate-base); border-top: 1px solid palette(mid); }"
        "#prelaunch #playButton { background: palette(highlight); color: palette(highlighted-text); font-weight: 600;"
        "  padding: 7px 26px; border-radius: 5px; border: none; }"
        "#prelaunch #playButton:disabled { background: palette(mid); color: palette(placeholder-text); }"
        "#prelaunch #stopButton { background: #c0392b; color: white; font-weight: 600; padding: 7px 26px; border-radius: 5px; border: none; }"
        "#prelaunch QProgressBar { max-height: 4px; border: none; background: palette(mid); border-radius: 2px; }"
        "#prelaunch QProgressBar::chunk { background: palette(highlight); border-radius: 2px; }");

    //A card a graft presents (a mod with its own card): every row launches with that graft applied.
    if (Index) if (const auto It = Index->TileGraft.find(this->FaceUid); It != Index->TileGraft.end()) TileGraft = It->second;

    // Initial variant = the one RECOMMENDED under this tile, else the first row.
    if (!this->GroupNodeIds.empty()) LaunchNodeId = this->GroupNodeIds.front();
    for (const std::string& Id : this->GroupNodeIds)
        if (const Node* N = Index ? Index->Find(Id) : nullptr;
            N && std::find(N->Recommended.begin(), N->Recommended.end(), Face(N)) != N->Recommended.end()) { LaunchNodeId = Id; break; }
    if (const Node* L = CurrentLaunch()) { BundleDir = L->BundleDir.string(); PackageUID = L->GameKey(); }
    if (!PackageUID.empty()) InstanceName = InstanceStore::ResolveActive(*GlobalConfigJSON, PackageUID);

    // ----- Layout: [cover | header + setup + tabs] over a footer (status, actions) -----
    QVBoxLayout* Outer = new QVBoxLayout(this);
    Outer->setContentsMargins(0, 0, 0, 0);
    Outer->setSpacing(0);
    QWidget*     Body       = new QWidget(this);
    QHBoxLayout* RootLayout = new QHBoxLayout(Body);
    RootLayout->setContentsMargins(18, 18, 18, 12);
    RootLayout->setSpacing(18);
    Outer->addWidget(Body, 1);

    // LEFT: the cover at a fixed width (its height follows its aspect), the Virtual LAN panel under it.
    QWidget*     LeftWidget = new QWidget(Body);
    QVBoxLayout* LeftCol    = new QVBoxLayout(LeftWidget);
    LeftCol->setContentsMargins(0, 0, 0, 0);
    LeftCol->setSpacing(12);
    LeftWidget->setFixedWidth(230);
    RootLayout->addWidget(LeftWidget, 0);
    CoverLabel = new QLabel(LeftWidget);
    CoverLabel->setObjectName("CoverLabel");
    CoverLabel->installEventFilter(this);   // rescale on the LABEL's own resize (see eventFilter)
    CoverLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    CoverLabel->setFixedWidth(230);
    CoverLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    LeftCol->addWidget(CoverLabel, 0, Qt::AlignTop);
    //Under the cover, the game itself: who made it, how it is played together, what it takes here, when it was last
    //played — what a card does not say, without a trip to the editor.
    DetailsLabel = new QLabel(LeftWidget);
    DetailsLabel->setObjectName("gameDetails");
    DetailsLabel->setAccessibleName("Game details");
    DetailsLabel->setWordWrap(true);
    DetailsLabel->setTextFormat(Qt::RichText);
    DetailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    LeftCol->addWidget(DetailsLabel);
    BuildLanPanel(LeftCol);
    LeftCol->addStretch(1);

    // RIGHT: what is launched and how.
    QWidget*     RightWidget = new QWidget(Body);
    QVBoxLayout* RightLayout = new QVBoxLayout(RightWidget);
    RightLayout->setContentsMargins(0, 0, 0, 0);
    RightLayout->setSpacing(10);
    RootLayout->addWidget(RightWidget, 1);
    QWidget* ControlWidget = RightWidget;

    TitleLabel = new QLabel(RightWidget);
    TitleLabel->setProperty("role", "title");
    TitleLabel->setWordWrap(true);
    RightLayout->addWidget(TitleLabel);
    MetaLabel = new QLabel(RightWidget);
    MetaLabel->setProperty("role", "dim");
    RightLayout->addWidget(MetaLabel);
    RightLayout->addSpacing(4);

    QFormLayout* PickerForm = new QFormLayout();
    PickerForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    PickerForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    PickerForm->setHorizontalSpacing(12);
    PickerForm->setVerticalSpacing(8);
    PickerForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    RightLayout->addLayout(PickerForm);
    auto FieldLabel = [&](const QString& T) { auto* L = new QLabel(T, RightWidget); L->setProperty("role", "field"); return L; };

    // Instance: which config + saves this launch uses; the menu manages them.
    {
        QWidget*     Row = new QWidget(RightWidget);
        QHBoxLayout* RL  = new QHBoxLayout(Row);
        RL->setContentsMargins(0, 0, 0, 0);
        RL->setSpacing(6);
        InstanceCombo = new QComboBox(Row);
        InstanceCombo->setObjectName("instanceCombo");
        InstanceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        InstanceCombo->setMinimumWidth(180);
        RL->addWidget(InstanceCombo);
        InstanceMenuButton = new QToolButton(Row);
        InstanceMenuButton->setObjectName("instanceMenu");
        InstanceMenuButton->setText("⋯");
        InstanceMenuButton->setToolTip("New, duplicate, rename or delete an instance");
        InstanceMenuButton->setPopupMode(QToolButton::InstantPopup);
        InstanceMenuButton->setAutoRaise(true);
        QMenu* M = new QMenu(InstanceMenuButton);
        M->addAction("New instance…",       this, &PreLaunchWindow::NewInstance);
        M->addAction("Duplicate…",          this, &PreLaunchWindow::DuplicateInstance);
        M->addAction("Rename…",             this, &PreLaunchWindow::RenameInstance);
        M->addSeparator();
        M->addAction("Open its folder",     this, [this]{
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(
                InstanceStore::InstanceDir(*this->GlobalConfigJSON, PackageUID, InstanceName).string()))); });
        M->addSeparator();
        M->addAction("Delete…",             this, &PreLaunchWindow::DeleteInstance);
        InstanceMenuButton->setMenu(M);
        RL->addWidget(InstanceMenuButton);
        InstanceInfo = new QLabel(Row);
        InstanceInfo->setProperty("role", "dim");
        RL->addSpacing(6);
        RL->addWidget(InstanceInfo, 1);
        PickerForm->addRow(FieldLabel("Instance"), Row);
        FillInstances();
        connect(InstanceCombo, &QComboBox::currentIndexChanged, this, &PreLaunchWindow::onInstanceChanged);
    }

    // Variant combo (the group's launchable nodes) — hidden when there's only one.
    VariantCombo = new QComboBox(ControlWidget);
    VariantCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    PickerForm->addRow(FieldLabel("Version"), VariantCombo);
    VariantLabel = PickerForm->labelForField(VariantCombo);
    FillVariantCombo();
    {
        //The row named LaunchNodeId (a variant, or a graft that is a version: its rows carry it as their row id).
        int Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole + 1);
        if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
        if (Sel >= 0) VariantCombo->setCurrentIndex(Sel);
        if (VariantCombo->currentIndex() >= 0) TakeRow(VariantCombo->currentData().toString().toStdString());
        bool Multi = VariantCombo->count() > 1;
        VariantCombo->setVisible(Multi);
        if (VariantLabel) VariantLabel->setVisible(Multi);
    }

    // Runner daisy-chain — one compact combo per step, in a row (innermost→outermost); a hint only when it is broken.
    {
        QWidget*     Row = new QWidget(ControlWidget);
        QVBoxLayout* RV  = new QVBoxLayout(Row);
        RV->setContentsMargins(0, 0, 0, 0);
        RV->setSpacing(2);
        ChainContainer = new QWidget(Row);
        ChainLayout = new QHBoxLayout(ChainContainer);
        ChainLayout->setContentsMargins(0, 0, 0, 0);
        ChainLayout->setSpacing(6);
        RV->addWidget(ChainContainer, 0, Qt::AlignLeft);
        ChainHint = new QLabel(Row);
        ChainHint->setStyleSheet("QLabel { color: #e0a040; }");
        ChainHint->setWordWrap(true);
        RV->addWidget(ChainHint);
        PickerForm->addRow(FieldLabel("Runs with"), Row);
    }

    // ----- Tabs: Options | Add-ons | Advanced | Log -----
    Tabs = new QTabWidget(RightWidget);
    Tabs->setObjectName("prelaunchTabs");
    Tabs->setDocumentMode(false);
    RightLayout->addWidget(Tabs, 1);

    // Options: a tree of collapsible SECTION rows ("UserPatch/Interface": '/' nests), each option a row with its label
    // in column 0 and its control in column 1. The tree scrolls itself.
    OptionsPage = new QWidget();
    {
        auto* OL = new QVBoxLayout(OptionsPage);
        OL->setContentsMargins(6, 6, 6, 6);
        CustomVarGroup = OptionsPage;
        OptionsTree = new QTreeWidget(OptionsPage);
        OptionsTree->setObjectName("optionsTree");
        OptionsTree->setColumnCount(2);
        OptionsTree->setHeaderHidden(true);
        OptionsTree->setRootIsDecorated(true);
        OptionsTree->setIndentation(16);
        OptionsTree->setSelectionMode(QAbstractItemView::NoSelection);
        OptionsTree->setFocusPolicy(Qt::NoFocus);
        OptionsTree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        OptionsTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        OptionsTree->header()->setStretchLastSection(true);
        OL->addWidget(OptionsTree);
    }
    Tabs->addTab(OptionsPage, "Options");

    // Add-ons: the grafts this version offers, in the order they apply (the selected ticked one can be moved).
    AddonsPage = new QWidget();
    {
        ModuleGroup = AddonsPage;
        auto* ML = new QVBoxLayout(AddonsPage);
        ML->setContentsMargins(6, 6, 6, 6);
        ML->setSpacing(6);
        ModuleTree = new QTreeWidget(AddonsPage);
        ModuleTree->setObjectName("graftList");
        ModuleTree->setHeaderHidden(true);
        ModuleTree->setRootIsDecorated(true);   // grafts sit in collapsible SECTION rows
        ModuleTree->setIndentation(16);
        //Single selection: the selected TICKED graft can be moved — the list order is the order grafts apply in.
        ModuleTree->setSelectionMode(QAbstractItemView::SingleSelection);
        ML->addWidget(ModuleTree, 1);
        auto* Row = new QHBoxLayout();
        Row->setSpacing(6);
        auto* OrderHint = new QLabel("Ticked add-ons apply top to bottom.", AddonsPage);
        OrderHint->setObjectName("graftOrderHint");
        OrderHint->setProperty("role", "dim");
        Row->addWidget(OrderHint, 1);
        GraftUp   = new QPushButton("▲ Earlier", AddonsPage);
        GraftDown = new QPushButton("▼ Later", AddonsPage);
        GraftUp->setToolTip("Apply the selected add-on earlier (those below it fold over it).");
        GraftDown->setToolTip("Apply the selected add-on later (it folds over those above it).");
        GraftUp->setObjectName("graftUp");
        GraftDown->setObjectName("graftDown");
        GraftUp->setFlat(true); GraftDown->setFlat(true);
        Row->addWidget(GraftUp); Row->addWidget(GraftDown);
        ML->addLayout(Row);
        GraftNote = new QLabel(AddonsPage);
        GraftNote->setObjectName("graftNote");
        GraftNote->setWordWrap(true);
        GraftNote->setStyleSheet("QLabel { color: #e0a040; }");
        GraftNote->setVisible(false);
        ML->addWidget(GraftNote);
        connect(GraftUp,   &QPushButton::clicked, this, [this]{ MoveSelectedGraft(-1); });
        connect(GraftDown, &QPushButton::clicked, this, [this]{ MoveSelectedGraft(+1); });
        auto Enable = [this]{
            const QTreeWidgetItem* It = ModuleTree->currentItem();
            const bool Ticked = It && It->data(0, Qt::UserRole + 2).toBool();
            const std::vector<std::string> T = CollectGrafts();
            const auto At = It ? std::find(T.begin(), T.end(), It->data(0, Qt::UserRole).toString().toStdString()) : T.end();
            GraftUp->setEnabled(Ticked && At != T.end() && At != T.begin());
            GraftDown->setEnabled(Ticked && At != T.end() && At + 1 != T.end());
        };
        connect(ModuleTree, &QTreeWidget::currentItemChanged, this, Enable);
        connect(ModuleTree, &QTreeWidget::itemChanged, this, Enable);
        GraftUp->setEnabled(false); GraftDown->setEnabled(false);
    }
    Tabs->addTab(AddonsPage, "Add-ons");
    //A toggle rebuilds the list — deleting the very row whose setData emitted itemChanged, while Qt is still inside it
    //(a use-after-free: ticking a graft's box could crash). Take the row's key and state now, rebuild after it returns.
    connect(ModuleTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* It, int){
        if (!It) return;
        const QString Key = It->data(0, Qt::UserRole).toString();
        if (Key.isEmpty()) return;   // a section row
        const bool Checked = It->checkState(0) == Qt::Checked;
        QMetaObject::invokeMethod(this, [this, Key, Checked]{ PropagateModuleToggle(Key, Checked); }, Qt::QueuedConnection);
    });

    // Advanced: how this window and the launch behave.
    AdvancedPage = new QWidget();
    {
        auto* AL = new QVBoxLayout(AdvancedPage);
        AL->setContentsMargins(14, 12, 14, 12);
        AL->setSpacing(8);
        RememberCheck         = new QCheckBox("Skip this window next time (launch straight away)", AdvancedPage);
        CloseAfterLaunchCheck = new QCheckBox("Close this window once the game has started",      AdvancedPage);
        DryRunCheck           = new QCheckBox("Dry run: discard everything the game writes",       AdvancedPage);
        PreserveRuntimeCheck  = new QCheckBox("Pause after exit to inspect the runtime",           AdvancedPage);
        DryRunCheck->setToolTip("The run's writable layer is deleted on cleanup: saves and settings from this run are not kept.");
        PreserveRuntimeCheck->setToolTip("After the game exits, pause with a dialog while this run's runtime (mounts + "
                                         "files) is still in place so you can inspect it. It's cleaned up (unmounted + "
                                         "deleted) as soon as you close that dialog — never left dangling.");
        for (QCheckBox* C : {RememberCheck, CloseAfterLaunchCheck, DryRunCheck, PreserveRuntimeCheck}) AL->addWidget(C);
        AL->addStretch(1);
    }
    Tabs->addTab(AdvancedPage, "Advanced");

    // Log: everything the launch says (the footer carries the latest line).
    LogPage = new QWidget();
    {
        auto* LL = new QVBoxLayout(LogPage);
        LL->setContentsMargins(0, 0, 0, 0);
        ConsoleEdit = new QTextEdit(LogPage);
        ConsoleEdit->setReadOnly(true);
        ConsoleEdit->setFrameShape(QFrame::NoFrame);
        ConsoleEdit->setPlaceholderText("Nothing yet: what the launch does appears here.");
        QFont MonoFont("Monospace");
        MonoFont.setStyleHint(QFont::Monospace);
        MonoFont.setPointSize(9);
        ConsoleEdit->setFont(MonoFont);
        ConsoleEdit->setStyleSheet("QTextEdit { background-color: #15171a; color: #d8dade; padding: 6px; }");
        //Unbounded before: a long session accumulated every line ever logged, and each append relaid the whole
        //document. 5000 blocks is ~20 resolves' worth of context — plenty to scroll back through a failure.
        ConsoleEdit->document()->setMaximumBlockCount(5000);
        LL->addWidget(ConsoleEdit);
    }
    Tabs->addTab(LogPage, "Log");
    ConsoleFlushTimer = new QTimer(this);
    ConsoleFlushTimer->setSingleShot(true);
    ConsoleFlushTimer->setInterval(60);
    connect(ConsoleFlushTimer, &QTimer::timeout, this, &PreLaunchWindow::flushConsole);

    // ----- Footer: status + progress on the left, the actions on the right -----
    QWidget*     Footer    = new QWidget(this);
    Footer->setObjectName("footer");
    Footer->setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout* BtnLayout = new QHBoxLayout(Footer);
    BtnLayout->setContentsMargins(18, 10, 18, 10);
    BtnLayout->setSpacing(8);
    Outer->addWidget(Footer);
    {
        QWidget*     StatusBox = new QWidget(Footer);
        QVBoxLayout* SL        = new QVBoxLayout(StatusBox);
        SL->setContentsMargins(0, 0, 0, 0);
        SL->setSpacing(4);
        StatusLabel = new QLabel(StatusBox);
        StatusLabel->setProperty("role", "dim");
        StatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        StatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // a long line never widens the window
        SL->addWidget(StatusLabel);
        ProgressBar = new QProgressBar(StatusBox);
        ProgressBar->setRange(0, 100);
        ProgressBar->setValue(0);
        ProgressBar->setTextVisible(false);
        ProgressBar->setVisible(false);
        SL->addWidget(ProgressBar);
        BtnLayout->addWidget(StatusBox, 1);
    }

    QPushButton* PackageEditorButton = new QPushButton("Edit package", Footer);   // a button like Close; Play is the one that stands out
    PackageEditorButton->setToolTip("Open this package in the package editor");
    BtnLayout->addWidget(PackageEditorButton);
    connect(PackageEditorButton, &QPushButton::clicked, this, [this]()
    {
        PackageEditor::OpenFor(this->GlobalConfigJSON, this, QString::fromStdString(this->BundleDir));
    });

    CloseButton = new QPushButton("Close", Footer);
    BtnLayout->addWidget(CloseButton);
    KillButton = new QPushButton("■  Stop", Footer);
    KillButton->setObjectName("stopButton");
    KillButton->setEnabled(false);
    KillButton->setVisible(false);
    BtnLayout->addWidget(KillButton);
    LaunchButton = new QPushButton("▶  Play", Footer);
    LaunchButton->setObjectName("playButton");
    LaunchButton->setDefault(true);
    BtnLayout->addWidget(LaunchButton);

    connect(VariantCombo, &QComboBox::currentIndexChanged, this, &PreLaunchWindow::onVariantChanged);
    connect(LaunchButton, &QPushButton::clicked, this, &PreLaunchWindow::onLaunchClicked);
    connect(KillButton,   &QPushButton::clicked, this, &PreLaunchWindow::onKillClicked);
    connect(CloseButton,  &QPushButton::clicked, this, &QDialog::accept);

    RebuildCover();
    RebuildRunnerChain();
    RebuildModuleTree();
    RebuildCustomVarPickers();
    RefreshInstanceInfo();
    if (StatusLabel->text().isEmpty()) StatusLabel->setText("Ready");
    Tabs->setCurrentWidget(Tabs->isTabVisible(Tabs->indexOf(OptionsPage)) ? OptionsPage : AdvancedPage);

    // A comfortable default, capped to the screen so it never opens off-screen.
    int W = 1000, H = 680;
    if (QScreen* Scr = QGuiApplication::primaryScreen())
    {
        const QRect A = Scr->availableGeometry();
        W = std::min(W, A.width()  - 80);
        H = std::min(H, A.height() - 80);
    }
    resize(std::max(W, 760), std::max(H, 520));
}

PreLaunchWindow::~PreLaunchWindow()
{
    if (LaunchWorker && LaunchWorker->isRunning())
    {
        LaunchWorker->kill();
        LaunchWorker->wait();
    }
    delete LaunchWorker;
}

const Node* PreLaunchWindow::CurrentLaunch() const
{
    return (Index && !LaunchNodeId.empty()) ? Index->Find(LaunchNodeId) : nullptr;
}

void PreLaunchWindow::RebuildCover()
{
    // Drop any cover still pending from a PREVIOUS variant: otherwise switching to a variant with no cover (or a
    // different one) leaves the old CID pending, and when it lands the handler paints the old variant's cover onto
    // this one. Re-set below iff THIS variant has a not-yet-local cover.
    PendingCoverCid.clear();
    const Node* L = CurrentLaunch();
    if (!L) return;
    //The window presents its tile (a version may present several).
    const nlohmann::ordered_json* T = Index ? Index->Tile(Face(L)) : nullptr;
    const nlohmann::ordered_json& Meta = T ? *T : L->Meta;
    const std::string Title = Meta.is_object() ? Meta.value("TITLE", LaunchNodeId) : LaunchNodeId;
    setWindowTitle("Launch " + QString::fromStdString(Title));
    if (TitleLabel) TitleLabel->setText(QString::fromStdString(Title));
    if (MetaLabel)
    {
        //Developer · year · edition, from the tile's META — whatever of it is there.
        QStringList Bits;
        const nlohmann::ordered_json& M = Meta.is_object() && Meta.contains("META") && Meta["META"].is_object() ? Meta["META"] : Meta;
        auto Str = [&](const char* K) { return M.is_object() && M.contains(K) && M[K].is_string() ? QString::fromStdString(M[K].get<std::string>()) : QString(); };
        if (const QString D = Str("DEVELOPER"); !D.isEmpty()) Bits << D;
        if (const QString R = Str("RELEASEDATE"); R.size() >= 4) Bits << R.left(4);
        if (const QString E = Str("EDITION"); !E.isEmpty() && E != "Original Release") Bits << E;
        MetaLabel->setText(Bits.join("  ·  "));
        MetaLabel->setVisible(!Bits.isEmpty());
        RefreshDetails(M);
    }

    CoverLabel->clear();
    CoverPixmap = QPixmap();
    if (!Meta.is_object() || !Meta.contains("COVER")) return;
    const nlohmann::ordered_json& CoverNode = Meta["COVER"];
    const QString Pkg = QString::fromStdString(L->BundleDir.string());
    auto setFrom = [this](const QString& Path){
        QPixmap Pix(Path);
        if (!Pix.isNull()) { CoverPixmap = Pix; UpdateCoverScaled(); }
    };
    const QString Now = CoverCache::instance()->resolve(CoverNode, Pkg);
    if (!Now.isEmpty()) { setFrom(Now); return; }
    QString F, Cid; CoverCache::Locate(CoverNode, F, Cid);
    if (!Cid.isEmpty())
    {
        //ONE coverReady connection for the window's lifetime, keyed by the member state below. The old code
        //connected a fresh lambda on EVERY RebuildCover (each variant switch), stacking live connections to
        //the CoverCache singleton — a slow leak plus redundant resolve work per ready cover.
        PendingCoverCid  = Cid;
        PendingCoverNode = CoverNode;
        PendingCoverPkg  = Pkg;
        if (!CoverReadyConnected)
        {
            CoverReadyConnected = true;
            connect(CoverCache::instance(), &CoverCache::coverReady, this, [this](const QString &Ready){
                // An empty Ready is the broadcast nudge (e.g. the offline→online transition cleared the negative
                // cache): re-resolve our pending cover. A non-empty Ready must match the one we're waiting for.
                if (PendingCoverCid.isEmpty()) return;
                if (!Ready.isEmpty() && Ready != PendingCoverCid) return;
                const QString P = CoverCache::instance()->resolve(PendingCoverNode, PendingCoverPkg);
                if (!P.isEmpty())
                {
                    QPixmap Pix(P);
                    if (!Pix.isNull()) { CoverPixmap = Pix; UpdateCoverScaled(); }
                    PendingCoverCid.clear();   // painted — stop listening
                }
                // If P is empty the cover isn't here yet (the online nudge only just kicked the fetch, or it is
                // still in flight): KEEP PendingCoverCid set so the later coverReady(cid) can still paint it.
            });
        }
    }
}

// Scale the full-res cover to fit CoverLabel's current content area, preserving aspect ratio. AlignCenter on the
// label then centers it vertically (and horizontally) in its column. No-op until the label has a real size.
void PreLaunchWindow::UpdateCoverScaled()
{
    if (!CoverLabel) return;
    if (CoverPixmap.isNull()) { CoverLabel->clear(); CoverLabel->setFixedHeight(0); return; }
    const QPixmap P = RoundedCover(CoverPixmap, CoverLabel->width(), devicePixelRatioF());
    CoverLabel->setPixmap(P);
    CoverLabel->setFixedHeight((int)(P.height() / P.devicePixelRatio()));
}

void PreLaunchWindow::resizeEvent(QResizeEvent* Event)
{
    QDialog::resizeEvent(Event);
    UpdateCoverScaled();
}

bool PreLaunchWindow::eventFilter(QObject* Obj, QEvent* Event)
{
    if (Obj == CoverLabel && Event->type() == QEvent::Resize)
        UpdateCoverScaled();
    return QDialog::eventFilter(Obj, Event);
}

void PreLaunchWindow::RefreshDetails(const nlohmann::ordered_json &M)
{
    if (!DetailsLabel) return;
    auto Str = [&](const char* K) { return M.is_object() && M.contains(K) && M[K].is_string() ? QString::fromStdString(M[K].get<std::string>()) : QString(); };
    QStringList Rows;
    const auto Row = [&Rows](const QString &K, const QString &V) {
        if (!V.isEmpty()) Rows << "<tr><td style='color:" + Theme::Dim + "; padding-right:10px'>" + K + "</td><td>" + V.toHtmlEscaped() + "</td></tr>";
    };
    Row("Publisher", Str("PUBLISHER"));
    Row("Series", Str("SERIES"));
    QStringList Together;
    if (Str("LANMULTIPLAYER") == "Yes") Together << "LAN";
    if (Str("ONLINEMULTIPLAYER") == "Yes") Together << "online";
    if (Str("DIRECTCONNECT") == "Yes") Together << "direct IP";
    if (Str("LOCALMULTIPLAYER") == "Yes") Together << "same screen";
    if (Str("LOCALCOOP") == "Yes" || Str("NETWORKCOOP") == "Yes") Together << "co-op";
    Row("Multiplayer", Together.join(", "));
    Row("Versions", VariantCombo && VariantCombo->count() > 1 ? QString::number(VariantCombo->count()) : QString());
    const QString Last = PlayedWhen(QString::fromStdString(InstanceStore::LastRun(*GlobalConfigJSON, PackageUID, InstanceName)));
    Row("Last played", Last.isEmpty() ? QString("never") : Last);
    if (const std::string Since = PackageCatalog::GoldenSince(*GlobalConfigJSON, PackageUID); !Since.empty()) Row("Golden since", QString::fromStdString(Since));
    DetailsHtml = Rows;
    DetailsLabel->setText("<table>" + Rows.join("") + "</table>");
    //Its size: a walk of the package folder, off the GUI thread; added when it lands (if this is still the package).
    if (BundleDir.empty()) return;
    const std::string Dir = BundleDir;
    auto Bytes = std::make_shared<std::uintmax_t>(0);
    QPointer<PreLaunchWindow> Self(this);
    AsyncWork::Run(this, [Dir, Bytes]{
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(Dir, std::filesystem::directory_options::skip_permission_denied, Ec);
                 !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
                if (std::error_code E2; It->is_regular_file(E2)) *Bytes += It->file_size(E2);
        },
        [Self, Dir, Bytes]{
            if (!Self || Self->BundleDir != Dir || !Self->DetailsLabel) return;
            QStringList With = Self->DetailsHtml;
            With.insert(std::min<qsizetype>(With.size(), 3), "<tr><td style='color:" + Theme::Dim + "; padding-right:10px'>Size</td><td>"
                                                               + HumanBytesQ(static_cast<long long>(*Bytes)) + "</td></tr>");
            Self->DetailsLabel->setText("<table>" + With.join("") + "</table>");
        });
}

void PreLaunchWindow::onVariantChanged()
{
    if (VariantCombo->currentIndex() < 0) return;
    //Combo data = "<node key>\x1f<entrypoint label>": a variant is (node, entrypoint).
    //Combo data = "<variant key>\x1f<entry label>[\x1f<graft key>]": a row is (variant, entry) — of the variant's own
    //effective entries, or of a ticked graft that carries entries (a mod loader: "run Forge").
    const std::string PrevLaunch = LaunchNodeId + "\x1f" + RowGraft;
    TakeRow(VariantCombo->currentData().toString().toStdString());
    RebuildCover();
    RebuildRunnerChain();
    RebuildModuleTree();
    RebuildCustomVarPickers();
    if (PrevLaunch != LaunchNodeId + "\x1f" + RowGraft) RefreshGraftEntryRows();
}

void PreLaunchWindow::ReloadAndRebuild(std::shared_ptr<const NodeIndex> Fresh)
{
    if (Fresh && Fresh != IndexOwned)
    {
        const auto Follow = [&](const std::string &Id) -> std::string {
            if (Id.empty() || Fresh->Find(Id)) return Id;
            const Node *Old = Index ? Index->Find(Id) : nullptr;
            const std::string H = Old && !Old->Handle.empty() ? Old->Handle : Id;
            for (const auto &[K, N] : Fresh->Nodes) if (N.Handle == H || K == H) return K;
            return Id;   // gone: dropped below
        };
        for (std::string &Id : GroupNodeIds) Id = Follow(Id);
        LaunchNodeId = Follow(LaunchNodeId);
        RowGraft = Follow(RowGraft);
        IndexOwned = std::move(Fresh);
        Index = IndexOwned.get();
    }
    // Drop editions that no longer exist (e.g. after an edit) and rebuild everything from the index.
    std::vector<std::string> Live;
    for (const std::string& Id : GroupNodeIds) if (Index && Index->Find(Id)) Live.push_back(Id);
    GroupNodeIds = Live;
    QSignalBlocker B(VariantCombo);
    const QString Was = VariantCombo->currentData().toString();   // the exact row: a graft version's names its graft
    FillVariantCombo();
    int Sel = Was.isEmpty() ? -1 : VariantCombo->findData(Was);
    if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId + "\x1f" + Entrypoint));
    if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
    VariantCombo->setCurrentIndex(Sel >= 0 ? Sel : 0);
    onVariantChanged();
}

// ---- instances ----------------------------------------------------------------------------------------------------

void PreLaunchWindow::SetTabShown(QWidget* Page, bool Shown)
{
    if (!Tabs || !Page) return;
    const int I = Tabs->indexOf(Page);
    if (I < 0 || Tabs->isTabVisible(I) == Shown) return;
    Tabs->setTabVisible(I, Shown);
}

void PreLaunchWindow::UpdateLogTabTitle()
{
    if (!Tabs || !LogPage) return;
    const int I = Tabs->indexOf(LogPage);
    const int N = LogErrors + LogWarnings;
    Tabs->setTabText(I, N ? QString("Log  ⚠ %1").arg(N) : QString("Log"));
}

void PreLaunchWindow::TakeRow(const std::string& Data)
{
    std::vector<std::string> F;
    for (size_t A = 0;;)
    {
        const size_t B = Data.find('\x1f', A);
        F.push_back(Data.substr(A, B == std::string::npos ? std::string::npos : B - A));
        if (B == std::string::npos) break;
        A = B + 1;
    }
    LaunchNodeId = F[0];
    Entrypoint = F.size() > 1 ? F[1] : std::string();
    RowGraft = F.size() > 3 && (F[2] == "row" || F[2] == "graft") ? F[3] : std::string();   // a graft entry's row keeps it
    //The row's game: a graft that is a version names it (its tile, its family); else the variant does.
    const Node* G = RowGraft.empty() ? nullptr : Index->Find(RowGraft);
    if (const Node* L = CurrentLaunch()) { BundleDir = (G ? G : L)->BundleDir.string(); PackageUID = (G && !G->PackageUid.empty() ? G : L)->GameKey(); }
}

void PreLaunchWindow::FillVariantCombo()
{
    QSignalBlocker B(VariantCombo);
    VariantCombo->clear();
    struct E { std::string Id; QString Lbl; bool Rec; std::string Row = {}; };   // Row: the row's node (a graft version)
    std::vector<E> Es;
    for (const std::string& Id : GroupNodeIds)
    {
        const Node* N = Index ? Index->Find(Id) : nullptr;
        if (!N) continue;
        if (N->IsGraft)
        {
            //A graft that is a version: a row per version it applies onto (named by it, and by its base when it has
            //several), run by the entries presenting this card with it applied.
            const std::vector<std::string> Bases = Shelf::GraftBases(*Index, Id);
            const bool Rec = std::find(N->Recommended.begin(), N->Recommended.end(), Face(N)) != N->Recommended.end();
            const std::string Name = !N->Variant.empty() ? N->Variant : N->NodeId;
            for (const std::string& Base : Bases)
            {
                std::vector<std::string> With;
                if (!TileGraft.empty()) With.push_back(TileGraft);
                With.push_back(Id);
                const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), Base, {}, {}, With);
                const Node* BN = Index->Find(Base);
                const std::string On = Bases.size() > 1 && BN ? " (on " + (!BN->Variant.empty() ? BN->Variant : BN->NodeId) + ")" : std::string();
                std::vector<std::string> Mine;
                for (const auto& [Lb, Ep] : P.Exec.items())
                {
                    if (!Ep.is_object() || (Ep.contains("GUEST") && Ep["GUEST"].is_array() && !Ep["GUEST"].empty())) continue;
                    const bool Here = FaceUid.empty() || (Ep.contains("TILE") && Ep["TILE"].is_object() && Ep["TILE"].value("UID", std::string()) == FaceUid);
                    if (Here) Mine.push_back(Lb);
                }
                for (const std::string& Lb : Mine)
                    Es.push_back({ Base + "\x1f" + Lb + "\x1frow\x1f" + Id, QString::fromStdString(Name + On + (Mine.size() > 1 ? " - " + Lb : std::string())), Rec && Lb == Mine.front(), Id });
            }
            continue;
        }
        if (!TileGraft.empty())
        {
            //A graft's card: each variant it applies onto, run by the graft's entries presenting this card.
            const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), Id, {}, {}, { TileGraft });
            std::vector<std::string> Mine;
            for (const auto& [Lb, Ep] : P.Exec.items())
                if (Ep.is_object() && Ep.contains("TILE") && Ep["TILE"].is_object() && Ep["TILE"].value("UID", std::string()) == FaceUid)
                    Mine.push_back(Lb);
            const std::string NodeName = !N->Variant.empty() ? N->Variant : (!N->NodeId.empty() ? N->NodeId : Id);
            for (const std::string& Lb : Mine)
                Es.push_back({ Id + "\x1f" + Lb, QString::fromStdString(Mine.size() == 1 ? NodeName : NodeName + " - " + Lb), false });
            continue;
        }
        //A row is (variant, entry): one per EFFECTIVE entry of this tile (own, else inherited from beneath) — an
        //entry presenting ANOTHER tile belongs to that tile's card; an entry with no tile (a mod loader's) is an
        //extra way to run this one. A single-entry variant reads as its VARIANT name; a multi-entry one names each.
        std::vector<std::string> Labels;
        for (const std::string& Lb : N->EntrypointLabels())
        {
            const auto& Ep = N->Entries[Lb];
            const bool OtherTile = !FaceUid.empty() && Ep.is_object() && Ep.contains("TILE") && Ep["TILE"].is_object()
                                && Ep["TILE"].value("UID", std::string()) != FaceUid;
            if (!OtherTile) Labels.push_back(Lb);
        }
        const bool Rec = std::find(N->Recommended.begin(), N->Recommended.end(), Face(N)) != N->Recommended.end();
        const std::string NodeName = !N->Variant.empty() ? N->Variant : (!N->NodeId.empty() ? N->NodeId : Id);
        for (size_t I = 0; I < Labels.size(); ++I)
        {
            const auto& Ep = N->Entries[Labels[I]];
            if (Ep.is_object() && Ep.contains("GUEST") && Ep["GUEST"].is_array() && !Ep["GUEST"].empty()) continue;   // a runner entry
            const std::string EpLabel = Ep.is_object() ? Ep.value("LABEL", std::string()) : std::string();
            //One way to run: the version's name says it. Several: each is the version's name and the entry's.
            const std::string Shown = (Labels.size() == 1 || EpLabel.empty() || EpLabel == NodeName) ? NodeName : NodeName + " - " + EpLabel;
            Es.push_back({ Id + "\x1f" + Labels[I], QString::fromStdString(Shown), Rec && I == 0 });
        }
    }
    // Recommended first, then NATURAL version order (1.9 < 1.10 < 1.10.2, NaturalLess) — with hundreds of variants
    // (903 Minecraft versions) lexicographic order scatters versions and makes the combo unusable.
    std::stable_sort(Es.begin(), Es.end(), [](const E& Ea, const E& Eb){
        if (Ea.Rec != Eb.Rec) return Ea.Rec;
        return NaturalLess(Ea.Lbl, Eb.Lbl);
    });
    for (const E& X : Es)
    {
        VariantCombo->addItem((X.Rec ? QStringLiteral("⭐ ") : QString()) + X.Lbl, QString::fromStdString(X.Id));
        VariantCombo->setItemData(VariantCombo->count() - 1, QString::fromStdString(X.Row.empty() ? X.Id.substr(0, X.Id.find('\x1f')) : X.Row), Qt::UserRole + 1);
    }
    // Type-to-find once the list is big: an editable combo with a contains-matching completer over its own model
    // (the popup list view is virtualized by Qt, so the item count itself is a non-issue).
    const bool Big = VariantCombo->count() > 12;
    VariantCombo->setEditable(Big);
    if (Big)
    {
        VariantCombo->setInsertPolicy(QComboBox::NoInsert);
        if (QCompleter* C = VariantCombo->completer())
        {
            C->setCompletionMode(QCompleter::PopupCompletion);
            C->setFilterMode(Qt::MatchContains);
            C->setCaseSensitivity(Qt::CaseInsensitive);
        }
    }
}

void PreLaunchWindow::persistGlobalConfig()
{
    JSONOps::SaveConfig(*GlobalConfigJSON, AppPaths::DataRoot());
}

void PreLaunchWindow::onLaunchClicked()
{
    if (LaunchNodeId.empty()) return;
    // The chosen runner daisy-chain (innermost→outermost). Persisted as RUNNER_CHAIN and passed to the worker.
    const std::vector<std::string>& SelectedChain = CurrentChain;

    // Collect the editable CustomVar control values (bare KEY -> value), secrets included: persisting a secret's
    // first pool draw is what makes it stable across launches. Hidden (WHEN=false) rows are still collected and
    // persisted (so a value typed in one mode is remembered when you switch back) — but they cannot leak into the
    // launch: the resolver re-evaluates each var's WHEN against the resolved map and gates a false one to "",
    // OVERRIDING even this picker value. So a stale join address can never reach a host-mode launch's args.
    std::map<std::string, std::string> PickerVars = CollectVarValues(CustomVarGroup);

    // Persist prefs (keyed by the bundle UID, so the engine's GetPackageUserSettings sees them).
    if (!PackageUID.empty())
    {
        InstanceStore::MergeVariables(*GlobalConfigJSON, PackageUID, PickerVars, InstanceName);
        SaveGrafts();
        //The chain is kept (RUNNER_CHAIN, which the resolver honours and the chain UI pre-selects) only when it is not
        //the default: a saved default is a hard pin, and would hold the instance to today's runners after a better
        //one is installed or the game starts recommending one (RECOMMENDEDRUNNER).
        { nlohmann::ordered_json ChainJson = nlohmann::ordered_json::array();
          for (const std::string& Id : SelectedChain) ChainJson.push_back(Id);
          std::vector<std::string> Default;
          if (const Node* L = CurrentLaunch())
          {
              ContainerParams Cp{std::filesystem::path(BundleDir)};
              Cp.NodeIdx = Index; Cp.LaunchNodeId = LaunchNodeId; Cp.Entrypoint = Entrypoint; Cp.PackageUID = PackageUID;
              Cp.InstanceName = InstanceName; Cp.Platform = ChainStepInput(0); Cp.IgnoreSavedChain = true;
              Default = LaunchResolver::ResolveChainIds(*Index, *L, Cp, *GlobalConfigJSON);
          }
          InstanceStore::SetSetting(*GlobalConfigJSON, PackageUID, "RUNNER_CHAIN",
                                                SelectedChain == Default ? nlohmann::ordered_json() : ChainJson, InstanceName); }
        if (RememberCheck->isChecked())
            InstanceStore::SetSetting(*GlobalConfigJSON, PackageUID, "SKIP_LAUNCH_DIALOG", true, InstanceName);
        persistGlobalConfig();
    }

    // Disable controls + show progress.
    ChainContainer->setEnabled(false); VariantCombo->setEnabled(false); CustomVarGroup->setEnabled(false);
    ModuleGroup->setEnabled(false); AdvancedPage->setEnabled(false);
    InstanceCombo->setEnabled(false); InstanceMenuButton->setEnabled(false);
    LaunchButton->setVisible(false); KillButton->setVisible(true); CloseButton->setEnabled(false);
    ProgressBar->setValue(0); ProgressBar->setVisible(true);
    StatusLabel->setStyleSheet(QString());
    StatusLabel->setText("Starting…");
    LogErrors = LogWarnings = 0;
    UpdateLogTabTitle();

    // Build + start the worker — native node launch via LaunchNodeId. The last launch's has finished (Launch is shown
    // only then): it goes, rather than one more thread object per launch for the dialog's life.
    if (LaunchWorker) { LaunchWorker->wait(); delete LaunchWorker; }
    LaunchWorker = new LaunchThread();
    LaunchWorker->GlobalConfigJSON = *GlobalConfigJSON;
    LaunchWorker->Index            = IndexOwned;                              // the graph this dialog shows, not a re-scan
    //A graft that is a version is launched as itself, on its base (the engine applies it and names the game by it).
    LaunchWorker->LaunchNodeId     = RowGraft.empty() ? LaunchNodeId : RowGraft;
    LaunchWorker->GraftBase        = RowGraft.empty() ? std::string() : LaunchNodeId;
    LaunchWorker->Entrypoint       = Entrypoint;
    LaunchWorker->Face             = FaceUid;
    LaunchWorker->InstanceName     = InstanceName;
    LaunchWorker->VariableOverrides = PickerVars;
    LaunchWorker->Grafts            = CollectGrafts();
    LaunchWorker->RunnerChain       = SelectedChain;                          // the full daisy-chain (innermost→outermost)
    LaunchWorker->DryRun            = DryRunCheck->isChecked();
    LaunchWorker->PreserveRuntime   = PreserveRuntimeCheck->isChecked();
    if (QScreen* Scr = QGuiApplication::primaryScreen())
    {
        LaunchWorker->ScreenWidth  = std::to_string(Scr->geometry().width());
        LaunchWorker->ScreenHeight = std::to_string(Scr->geometry().height());
    }
    connect(LaunchWorker, &LaunchThread::logLine,         this, &PreLaunchWindow::onLogLine,         Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::statusChanged,   this, &PreLaunchWindow::onStatusChanged,   Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::progressChanged, this, &PreLaunchWindow::onProgressChanged, Qt::QueuedConnection);
    connect(LaunchWorker, &LaunchThread::launchFinished,  this, &PreLaunchWindow::onLaunchFinished,  Qt::QueuedConnection);
    //The launch's WARN/ERR tally. Deliberately intrusive when non-zero: the console already carried these lines
    //and nobody read them, which is exactly how a per-launch error survived for months looking like a cosmetic
    //glitch. A clean run says so quietly; a dirty one turns the status bar red and stays on screen.
    connect(LaunchWorker, &LaunchThread::diagnosticsReady, this, [this](int Errors, int Warnings){
        if (Errors == 0 && Warnings == 0)
        {
            StatusLabel->setStyleSheet(QString());
            StatusLabel->setText(StatusLabel->text() + "  ·  0 warnings");
            return;
        }
        StatusLabel->setStyleSheet("color:#e06c75;font-weight:bold;");
        StatusLabel->setText(QString("⚠ %1 error(s), %2 warning(s) during this launch — see the Log tab")
                                 .arg(Errors).arg(Warnings));
        if (Errors > 0) Tabs->setCurrentWidget(LogPage);
        //Keep the window up even when "close after launch" is set: closing it would throw away the only place
        //the detail is visible.
        if (Errors > 0) CloseAfterLaunchCheck->setChecked(false);
    }, Qt::QueuedConnection);
    LaunchWorker->start();
}

void PreLaunchWindow::onKillClicked()
{
    if (LaunchWorker) LaunchWorker->kill();
}

void PreLaunchWindow::onLogLine(int level, QString context, QString message)
{
    // BUFFER, don't append: every append() is an HTML parse + document edit + relayout + scrollbar poke, and a
    // single resolve emits hundreds of lines (it was 2036 before the trace gate) delivered as queued events. Doing
    // the widget work per line froze the UI for the duration of a resolve on the laptop; batching turns a burst
    // into one document edit per timer tick with identical visible output.
    QString color;
    switch (static_cast<LogLevel>(level))
    {
        case LogLevel::ERR:  color = "#e06c75"; ++LogErrors;   break;
        case LogLevel::WARN: color = "#e5c07b"; ++LogWarnings; break;
        case LogLevel::SUCC: color = "#98c379"; break;
        default:             color = "";        break;
    }
    if (static_cast<LogLevel>(level) == LogLevel::ERR || static_cast<LogLevel>(level) == LogLevel::WARN) UpdateLogTabTitle();
    QString Text = context.toHtmlEscaped() + " " + message.toHtmlEscaped();
    ConsolePending.append(color.isEmpty() ? Text : QString("<span style=\"color:%1\">%2</span>").arg(color, Text));
    if (!ConsoleFlushTimer->isActive()) ConsoleFlushTimer->start();
}

void PreLaunchWindow::flushConsole()
{
    if (ConsolePending.isEmpty()) return;
    // One append per batch: join with <br> so the whole burst is a single document edit. The block-count cap on
    // the document (set at construction) bounds memory and relayout cost for pathological runs.
    ConsoleEdit->append(ConsolePending.join(QStringLiteral("<br>")));
    ConsolePending.clear();
    QScrollBar* SB = ConsoleEdit->verticalScrollBar();
    SB->setValue(SB->maximum());
}

void PreLaunchWindow::onStatusChanged(QString status) { StatusLabel->setText(status); }

void PreLaunchWindow::onProgressChanged(int value)
{
    ProgressBar->setValue(value);
    if (value >= 95)
    {
        KillButton->setEnabled(true);
        if (CloseAfterLaunchCheck->isChecked()) accept();
    }
}

void PreLaunchWindow::onLaunchFinished(bool success, QString errorMsg)
{
    KillButton->setEnabled(false); KillButton->setVisible(false);
    LaunchButton->setVisible(true);
    CloseButton->setEnabled(true);
    //The game may have changed its settings while it ran: the window (which stays open across the run) shows what its
    //files say now, so the next Launch does not write the pre-run values back over them.
    ReadBackDoneFor.clear();
    RebuildCustomVarPickers();
    if (success) { if (!StatusLabel->text().startsWith("⚠")) StatusLabel->setText("Finished" + (StatusLabel->text().contains("0 warnings") ? QString("  ·  0 warnings") : QString())); }
    else
    {
        StatusLabel->setStyleSheet("color:#e06c75;font-weight:bold;");
        StatusLabel->setText("Launch failed: " + errorMsg);
        Tabs->setCurrentWidget(LogPage);
        QMessageBox::warning(this, "Launch failed", errorMsg);
    }

    ChainContainer->setEnabled(true); VariantCombo->setEnabled(true); CustomVarGroup->setEnabled(true);
    ModuleGroup->setEnabled(true); AdvancedPage->setEnabled(true);
    InstanceCombo->setEnabled(true); InstanceMenuButton->setEnabled(true);
    LaunchButton->setEnabled(true);
    ProgressBar->setValue(0); ProgressBar->setVisible(false);
    RefreshInstanceInfo();   // the launch stamped LASTRUN
}
