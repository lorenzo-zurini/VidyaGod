#include "prelaunchwindow.h"
#include "fold.h"   // a row's options and entries are its resolution's
#include "apppaths.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "launchresolver.h"     // ResolveChainIds / ResolveChainTail / kNativeTerminalId (runner daisy-chain UI)
#include "jsonoperations.h"
#include "ipfswrapper.h"     // LanPeers/SetLanExcluded/LanLaunchVars — the Virtual LAN panel
#include "variantpicker.h"     // NaturalLess — deterministic version-aware variant ordering
#include "instancestore.h"     // the instance picker

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
#include <QPainterPath>
#include <QUrl>
#include <QDateTime>

namespace {
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

//A section with nothing visible in it (every option gated off by its WHEN) hides too.
bool HideEmptySections(QTreeWidgetItem* I)
{
    bool Any = false;
    for (int c = 0; c < I->childCount(); ++c)
    {
        QTreeWidgetItem* C = I->child(c);
        if (C->data(0, Qt::UserRole + 10).isValid()) { const bool V = HideEmptySections(C); C->setHidden(!V); Any |= V; }
        else Any |= !C->isHidden();
    }
    return Any;
}

}

namespace {
//A runner's id as a person reads it: "geproton_11_3" → "Geproton 11.3", "native-passthrough_exec" → "Native". The id
//stays in the tooltip; the data names runners by their node label, which is an identifier, not a title.
QString RunnerDisplayName(const std::string& Id)
{
    if (Id == LaunchResolver::kNativeTerminalId) return "Native (built-in)";
    std::string S = Id;
    if (S.size() > 5 && S.compare(S.size() - 5, 5, "_exec") == 0) S.resize(S.size() - 5);
    if (S.rfind("native-passthrough", 0) == 0) return "Native";
    for (size_t I = 1; I + 1 < S.size(); ++I)
        if (S[I] == '_') S[I] = (std::isdigit((unsigned char)S[I - 1]) && std::isdigit((unsigned char)S[I + 1])) ? '.' : ' ';
    if (!S.empty()) S[0] = (char)std::toupper((unsigned char)S[0]);
    return QString::fromStdString(S);
}

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
    const NodeIndex*         Index,
    std::vector<std::string> GroupNodeIds,
    std::string              FaceUid,
    QWidget*                 parent)
    : QDialog(parent)
    , GlobalConfigJSON(GlobalConfigJSON)
    , Index(Index)
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
        int Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId));
        if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
        if (Sel >= 0) VariantCombo->setCurrentIndex(Sel);
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

    QPushButton* PackageEditorButton = new QPushButton("Edit package", Footer);
    PackageEditorButton->setFlat(true);
    PackageEditorButton->setToolTip("Open this package in the package editor");
    BtnLayout->addWidget(PackageEditorButton);
    connect(PackageEditorButton, &QPushButton::clicked, this, [this]()
    {
        bool Created = false;
        PackageEditor* Editor = PackageEditor::OpenFor(this->GlobalConfigJSON, this,
                                                       QString::fromStdString(this->BundleDir), &Created);
        if (Editor && Created) connect(Editor, &PackageEditor::packageSaved, &MainWindow::RefreshPackage);
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

std::string PreLaunchWindow::ChainStepInput(int Step) const
{
    if (Step <= 0)
    {
        const Node* L = CurrentLaunch();
        if (!L) return ManifestModel::MachinePlatform();
        //The entry may be one a ticked graft adds: read it off the row's resolution with the grafts applied.
        if (!Entrypoint.empty() && !L->Entries.contains(Entrypoint))
        {
            const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), L->Key(), {}, {}, CollectGrafts());
            if (P.Exec.contains(Entrypoint)) return P.Exec[Entrypoint].value("HOST", L->HostPlatform);
        }
        return L->HostFor(Entrypoint);
    }
    if (Step - 1 >= (int)CurrentChain.size()) return ManifestModel::MachinePlatform();
    const std::string& Prev = CurrentChain[Step - 1];
    if (Prev == LaunchResolver::kNativeTerminalId) return ManifestModel::MachinePlatform();
    const Node* R = Index ? Index->Find(Prev) : nullptr;
    return R ? R->HostPlatform : ManifestModel::MachinePlatform();
}

bool PreLaunchWindow::ChainIdIsTerminal(const std::string& Id) const
{
    if (Id == LaunchResolver::kNativeTerminalId) return true;
    const Node* R = Index ? Index->Find(Id) : nullptr;
    if (!R || R->HostPlatform != ManifestModel::MachinePlatform()) return false;
    for (const auto& G : R->GuestPlatform) if (G == ManifestModel::MachinePlatform()) return true;
    return false;
}

void PreLaunchWindow::RebuildRunnerChain()
{
    // Resolve the DEFAULT chain for this variant (honours a persisted RUNNER_CHAIN), then render the per-step combos.
    CurrentChain.clear();
    const Node* L = CurrentLaunch();
    if (L)
    {
        ContainerParams Cp(std::filesystem::path(BundleDir), LaunchNodeId, std::string());
        Cp.NodeIdx = Index; Cp.LaunchNodeId = LaunchNodeId; Cp.Entrypoint = Entrypoint; Cp.PackageUID = PackageUID;
        Cp.InstanceName = InstanceName;
        Cp.Platform = ChainStepInput(0);
        CurrentChain = LaunchResolver::ResolveChainIds(*Index, *L, Cp, *GlobalConfigJSON);
    }
    RenderChainCombos();
}

void PreLaunchWindow::RenderChainCombos()
{
    static const QString NoRunnerMsg = "No installed runner chain — download a compatible runner from the Catalog.";

    // Tear down the previous step rows.
    ChainCombos.clear();
    QLayoutItem* Item;
    while ((Item = ChainLayout->takeAt(0)) != nullptr)
    {
        if (QWidget* W = Item->widget()) W->deleteLater();
        delete Item;
    }

    const std::string Machine = ManifestModel::MachinePlatform();
    for (int i = 0; i < (int)CurrentChain.size(); ++i)
    {
        const std::string Input = ChainStepInput(i);
        if (i > 0)
        {
            QLabel* Sep = new QLabel("›", ChainContainer);
            Sep->setProperty("role", "dim");
            ChainLayout->addWidget(Sep);
        }
        QComboBox* Combo = new QComboBox(ChainContainer);
        Combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        {
            QSignalBlocker B(Combo);
            for (const Node* R : PackageCatalog::CandidateRunners(*Index, Input))
            {
                Combo->addItem(RunnerDisplayName(R->NodeId), QString::fromStdString(R->Key()));   // keyed by the index key
                Combo->setItemData(Combo->count() - 1, QString::fromStdString(R->NodeId), Qt::ToolTipRole);
            }
            // The native terminal step also offers the built-in passthrough (used when no native runner is authored).
            if (Input == Machine && Combo->findData(QString::fromStdString(LaunchResolver::kNativeTerminalId)) < 0)
                Combo->addItem("Native (built-in)", QString::fromStdString(LaunchResolver::kNativeTerminalId));
            int Sel = Combo->findData(QString::fromStdString(CurrentChain[i]));
            if (Sel < 0 && Combo->count() > 0)
            {
                // The resolved id isn't an installed candidate (e.g. synthesized terminal absent here) — show it anyway
                // so the chain is faithful and selectable.
                Combo->addItem(RunnerDisplayName(CurrentChain[i]), QString::fromStdString(CurrentChain[i]));
                Sel = Combo->count() - 1;
            }
            if (Sel >= 0) Combo->setCurrentIndex(Sel);
        }
        Combo->setToolTip(QString("Runs %1 programs").arg(QString::fromStdString(Input)));
        const int Step = i;
        connect(Combo, &QComboBox::currentIndexChanged, this, [this, Step](int){ onChainStepChanged(Step); });
        ChainLayout->addWidget(Combo);
        ChainCombos.push_back(Combo);
    }

    // Validity: a chain is runnable when it ends on the machine platform (the native terminal). Say so only when not.
    const bool Reaches = !CurrentChain.empty() && ChainIdIsTerminal(CurrentChain.back());
    if (ChainHint)
    {
        if (CurrentChain.empty())
            ChainHint->setText(QString("⚠ Nothing installed runs this on %1 — download a runner from the Catalog.").arg(QString::fromStdString(Machine)));
        else if (!Reaches)
            ChainHint->setText(QString("⚠ This chain does not reach %1.").arg(QString::fromStdString(Machine)));
        ChainHint->setVisible(!Reaches);
    }
    if (LaunchButton) LaunchButton->setEnabled(Reaches);
    if (StatusLabel)
    {
        if (!Reaches) StatusLabel->setText(NoRunnerMsg);
        else if (StatusLabel->text() == NoRunnerMsg) StatusLabel->clear();
    }
}

void PreLaunchWindow::onChainStepChanged(int Step)
{
    if (Step < 0 || Step >= (int)ChainCombos.size()) return;
    const std::string Chosen = ChainCombos[Step]->currentData().toString().toStdString();
    if (Chosen.empty()) return;

    // Adopt the choice as the new step, drop everything downstream, then BFS-re-resolve the tail from its HOST.
    CurrentChain.resize(Step + 1);
    CurrentChain[Step] = Chosen;
    if (!ChainIdIsTerminal(Chosen))
    {
        const Node* R = Index ? Index->Find(Chosen) : nullptr;
        const std::string Host = R ? R->HostPlatform : ManifestModel::MachinePlatform();
        const Node* L = CurrentLaunch();
        std::vector<std::string> Tail = L ? LaunchResolver::ResolveChainTail(*Index, Host, *L, *GlobalConfigJSON)
                                          : std::vector<std::string>{};
        for (const std::string& Id : Tail) CurrentChain.push_back(Id);
    }
    RenderChainCombos();
    RebuildCustomVarPickers();
}

void PreLaunchWindow::RebuildModuleTree()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    ModuleTree->clear();

    const Node* L = CurrentLaunch();
    if (!L) { SetTabShown(AddonsPage, false); return; }

    //The GRAFTS this row offers (a node whose list begins with ANY naming something the row contains), ticked by the
    //instance's saved graft list — else the ones RECOMMENDED under this tile (a fresh instance).
    const PackageCatalog::GraftChoice Saved = SavedGrafts();
    //Offered with the ticked ones applied: a graft on a graft appears once the graft it needs is ticked.
    std::vector<std::string> PreTicked;
    std::vector<std::string> Offered = PackageCatalog::OfferedGrafts(*Index, LaunchNodeId, &PreTicked, Saved, FaceUid);
    //Ticked = what the launch applies: a saved graft it would drop (one another ticked graft excludes) shows unticked.
    std::vector<std::string> Ticked = PreTicked;
    if (Saved)
    {
        const auto [Instance, Builtins] = GraftJudging();
        Ticked = PackageCatalog::AppliedGrafts(*Index, LaunchNodeId, *Saved, Instance, Builtins);
    }
    Ticked.erase(std::remove_if(Ticked.begin(), Ticked.end(), [&](const std::string& G) {
        return std::find(Offered.begin(), Offered.end(), G) == Offered.end(); }), Ticked.end());
    //One stable list in the default order, so ticking never moves a row: the ticked rows' places hold the ticked grafts
    //in the order they apply (a move swaps two ticked rows; with no move made that is the default order itself).
    //Grafts sit in their SECTION's row (collapsed); sections in the order their first graft is offered. Within a
    //section the ticked rows' places hold that section's ticked grafts in the order they apply, and the tree read
    //depth-first is the order the launch applies them (CollectGrafts).
    const auto SectionOf = [&](const std::string& G) {
        const Node* N = Index->Find(G);
        return N && N->Json.contains("SECTION") && N->Json["SECTION"].is_string() ? N->Json["SECTION"].get<std::string>() : std::string();
    };
    std::vector<std::string> SectionOrder;
    std::map<std::string, std::vector<std::string>> BySection;
    for (const std::string& G : Offered)
    {
        const std::string S = SectionOf(G);
        if (!BySection.count(S)) SectionOrder.push_back(S);
        BySection[S].push_back(G);
    }
    AppliedOrder = Ticked;   // the order they apply in (CollectGrafts); the tree below only displays them
    std::map<std::string, QTreeWidgetItem*> Sections;
    std::vector<std::pair<std::string, QTreeWidgetItem*>> Rows;
    for (const std::string& S : SectionOrder)
    {
        std::vector<std::string> Mine;
        for (const std::string& G : Ticked) if (SectionOf(G) == S) Mine.push_back(G);
        size_t Next = 0;
        for (const std::string& G : BySection[S])
            Rows.push_back({ std::find(Mine.begin(), Mine.end(), G) != Mine.end() ? Mine[Next++] : G, SectionRow(ModuleTree, Sections, S) });
    }
    for (const auto& [G, Parent] : Rows)
    {
        const Node* N = Index->Find(G);
        if (!N) continue;
        QTreeWidgetItem* It = new QTreeWidgetItem(Parent);
        It->setText(0, QString::fromStdString(N->NodeId.empty() ? N->Key().substr(0, 12) : N->NodeId));
        It->setData(0, Qt::UserRole,     QString::fromStdString(N->Key()));
        It->setData(0, Qt::UserRole + 2, std::find(Ticked.begin(), Ticked.end(), G) != Ticked.end());
        It->setData(0, Qt::UserRole + 4, true);
    }
    SetTabShown(AddonsPage, !GraftRows(ModuleTree).empty());
    //Ordering means something only with two or more add-ons.
    const bool Several = GraftRows(ModuleTree).size() > 1;
    for (QWidget* X : { static_cast<QWidget*>(GraftUp), static_cast<QWidget*>(GraftDown),
                        static_cast<QWidget*>(AddonsPage->findChild<QLabel*>("graftOrderHint")) })
        if (X) X->setVisible(Several);
    RefreshModuleLocks();
}

void PreLaunchWindow::RefreshModuleLocks()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    for (QTreeWidgetItem* It : GraftRows(ModuleTree))
    {
        bool Desired = It->data(0, Qt::UserRole + 2).toBool();
        const bool Tickable = !It->data(0, Qt::UserRole + 4).isValid() || It->data(0, Qt::UserRole + 4).toBool();
        It->setFlags(Tickable ? (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable) : Qt::ItemIsUserCheckable);
        It->setCheckState(0, Desired ? Qt::Checked : Qt::Unchecked);
        It->setData(0, Qt::UserRole + 3, !Tickable);   // locked while its requirements are unselected
    }
}

void PreLaunchWindow::PropagateModuleToggle(const QString& Key, bool Checked)
{
    if (!ModuleTree) return;
    QTreeWidgetItem* Item = nullptr;
    for (QTreeWidgetItem* It : GraftRows(ModuleTree)) if (It->data(0, Qt::UserRole).toString() == Key) { Item = It; break; }
    if (!Item) return;
    //The toggle arrives queued, and a rebuild can come between: judge the row as it is NOW, and a row already recorded
    //in that state is a duplicate or stale event (one emitted from a row since rebuilt) — acting on it would untick a
    //graft the user just ticked.
    (void)Checked;
    const bool Now = Item->checkState(0) == Qt::Checked;
    if (Now == Item->data(0, Qt::UserRole + 2).toBool()) return;
    Checked = Now;
    Item->setData(0, Qt::UserRole + 2, Checked);
    if (Checked)
    {
        //The graft just ticked wins: those it excludes (either side's NOT) are unticked — exclusive grafts are a choice
        //of one — and it goes last in the ordered list.
        const std::string G = Key.toStdString();
        std::vector<std::string> Others = CollectGrafts();
        Others.erase(std::remove(Others.begin(), Others.end(), G), Others.end());
        const auto [Instance, Builtins] = GraftJudging();
        std::vector<std::string> List = PackageCatalog::TickGraft(*Index, LaunchNodeId, Others, G, nullptr, Instance, Builtins);
        //Keep its row where it is: it applies after the ticked rows above it — unless it needs one below (then last).
        List.pop_back();
        size_t Above = 0;
        for (QTreeWidgetItem* Row : GraftRows(ModuleTree))
        {
            const std::string K = Row->data(0, Qt::UserRole).toString().toStdString();
            if (K == G) break;
            if (std::find(List.begin(), List.end(), K) != List.end()) ++Above;
        }
        std::vector<std::string> InPlace = List;
        InPlace.insert(InPlace.begin() + (long)std::min(Above, InPlace.size()), G);
        const std::vector<std::string> A = PackageCatalog::AppliedGrafts(*Index, LaunchNodeId, InPlace, Instance, Builtins);
        if (std::find(A.begin(), A.end(), G) == A.end()) { InPlace = List; InPlace.push_back(G); }
        StoreGrafts(InPlace);
    }
    else SaveGrafts();
    RebuildModuleTree();
    RebuildCustomVarPickers();   // a graft brings its own options
    RefreshGraftEntryRows();     // a ticked mod loader adds a way to run; an unticked one takes it away
}

//The instance's graft lists are PER TILE: {"<tile UID>": [graft, …]}. Instance settings are the family's (RoC and TFT
//share one), so one list for the family meant a sibling tile's saved list replaced this tile's recommended pre-ticks,
//and saving on the sibling dropped the grafts only this tile offers.
std::optional<std::vector<std::string>> PreLaunchWindow::SavedGrafts() const
{
    const auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID, InstanceName);
    const std::string Tile = Face(CurrentLaunch());
    PackageCatalog::GraftChoice Saved;
    if (US.contains("GRAFTS") && US["GRAFTS"].is_object() && US["GRAFTS"].contains(Tile) && US["GRAFTS"][Tile].is_array())
    {
        Saved.emplace();
        for (const auto& G : US["GRAFTS"][Tile]) if (G.is_string()) Saved->push_back(G.get<std::string>());
    }
    return PackageCatalog::CurrentGraftChoice(*Index, Saved);
}

void PreLaunchWindow::StoreGrafts(const std::vector<std::string>& List)
{
    const auto US = PackageCatalog::GetPackageUserSettings(*GlobalConfigJSON, PackageUID, InstanceName);
    nlohmann::ordered_json ByTile = US.contains("GRAFTS") && US["GRAFTS"].is_object() ? US["GRAFTS"] : nlohmann::ordered_json::object();
    ByTile[Face(CurrentLaunch())] = List;
    PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "GRAFTS", ByTile, InstanceName);
}

void PreLaunchWindow::SaveGrafts()
{
    StoreGrafts(CollectGrafts());   //the tile's graft list = the ticked rows, in list order
}

static std::map<std::string, std::string> CollectVarValues(QObject* Group);   // below

//Grafts are judged as the launch judges them: the instance's values with what the pickers hold now merged over them
//(saved at launch), and the tile this window launches.
std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>> PreLaunchWindow::GraftJudging() const
{
    std::map<std::string, std::string> Instance;
    if (const auto Saved = PackageCatalog::GetPackageVariables(*GlobalConfigJSON, PackageUID, InstanceName); Saved.is_object())
        for (const auto &[K, V] : Saved.items()) if (V.is_string()) Instance[K] = V.get<std::string>();
    if (CustomVarGroup) for (const auto &[K, V] : CollectVarValues(CustomVarGroup)) Instance[K] = V;
    return { Instance, { {"UID", Face(Index->Find(LaunchNodeId))}, {"PackageUID", PackageUID} } };
}

void PreLaunchWindow::MoveSelectedGraft(int By)
{
    const QTreeWidgetItem* It = ModuleTree ? ModuleTree->currentItem() : nullptr;
    if (!It || !It->data(0, Qt::UserRole + 2).toBool()) return;
    const std::string Key = It->data(0, Qt::UserRole).toString().toStdString();
    std::vector<std::string> Ticked = CollectGrafts();
    const auto At = std::find(Ticked.begin(), Ticked.end(), Key);
    if (At == Ticked.end()) return;
    std::string Why;
    const auto [Instance, Builtins] = GraftJudging();
    if (!PackageCatalog::MoveGraft(*Index, LaunchNodeId, Ticked, (size_t)(At - Ticked.begin()), By, &Why, Instance, Builtins))
    {
        if (!Why.empty()) { GraftNote->setText(QString::fromStdString("Not moved: " + Why)); GraftNote->setVisible(true); }
        return;
    }
    GraftNote->setVisible(false);
    StoreGrafts(Ticked);
    RebuildModuleTree();                 // rows follow the saved order
    for (QTreeWidgetItem* Row : GraftRows(ModuleTree))
        if (Row->data(0, Qt::UserRole).toString().toStdString() == Key)
        {
            for (QTreeWidgetItem* P = Row->parent(); P; P = P->parent()) P->setExpanded(true);   // keep it in view
            ModuleTree->setCurrentItem(Row);
        }
    RebuildCustomVarPickers();
    RefreshGraftEntryRows();             // the entries grafts add follow the order too
}

//The ticked grafts in the order they APPLY — the list the last rebuild settled on (saved, or the tile's defaults),
//less any row unticked since. Not the tree's order: sections group grafts for display, and a graft in an earlier
//section can need one in a later section — read depth-first, it would be judged before what it needs and dropped.
std::vector<std::string> PreLaunchWindow::CollectGrafts() const
{
    std::vector<std::string> Out;
    if (!ModuleTree) return Out;
    std::set<std::string> TickedRows;
    for (QTreeWidgetItem* It : GraftRows(ModuleTree))
        if (It->data(0, Qt::UserRole + 2).toBool()) TickedRows.insert(It->data(0, Qt::UserRole).toString().toStdString());
    for (const std::string& G : AppliedOrder) if (TickedRows.count(G)) Out.push_back(G);
    return Out;
}

// Reads the current value of every var control (tagged CVKey) under `Group`, by widget type. Shared by the WHEN
// evaluator and the launch-time collector.
static std::map<std::string, std::string> CollectVarValues(QObject* Group)
{
    std::map<std::string, std::string> M;
    for (QWidget* W : Group->findChildren<QWidget*>())
    {
        const QString K = W->property("CVKey").toString();
        if (K.isEmpty()) continue;
        std::string V;
        if (auto* C = qobject_cast<QComboBox*>(W))          V = C->currentData().toString().toStdString();
        else if (auto* S = qobject_cast<QSpinBox*>(W))      V = std::to_string(S->value());
        else if (auto* D = qobject_cast<QDoubleSpinBox*>(W))V = QString::number(D->value()).toStdString();
        else if (auto* B = qobject_cast<QCheckBox*>(W))     V = B->isChecked() ? "1" : "0";
        else if (auto* E = qobject_cast<QLineEdit*>(W))     V = E->text().toStdString();
        else continue;
        M[K.toStdString()] = V;
    }
    return M;
}

// Which var KEYs are ACTIVE (their WHEN holds) given the current control values. A gated-off var contributes an
// empty value, so a chain (NETMODE gates ADDR gates X) is resolved to a fixpoint — mirrors the resolver exactly,
// via the same VarSubst::EvaluateCondition. Rows without a CVWhen are always active.
static std::set<std::string> ActiveVarKeys(QObject* Group)
{
    // Each gated row's WHEN, by its KEY (the row's label and control both carry them).
    std::map<std::string, std::string> KeyWhen;
    for (QWidget* Cell : Group->findChildren<QWidget*>())
    {
        const QVariant W = Cell->property("CVWhen");
        const QString K = Cell->property("CVRowKey").toString();
        if (W.isValid() && !K.isEmpty()) KeyWhen[K.toStdString()] = W.toString().toStdString();
    }
    std::map<std::string, std::string> Vals = CollectVarValues(Group);
    std::set<std::string> Active;
    for (const auto& [K, V] : Vals) Active.insert(K);
    for (int Pass = 0; Pass < 8; ++Pass)   // fixpoint: a gated-off var reads as empty for the next round
    {
        bool Changed = false;
        std::map<std::string, std::string> Eff = Vals;
        for (const auto& K : Vals) if (!Active.count(K.first)) Eff[K.first] = "";
        for (const auto& [K, When] : KeyWhen)
        {
            const bool On = VarSubst::EvaluateCondition(When, Eff);
            if (!On && Active.count(K)) { Active.erase(K); Changed = true; }
            else if (On && !Active.count(K)) { Active.insert(K); Changed = true; }
        }
        if (!Changed) break;
    }
    return Active;
}

void PreLaunchWindow::EvaluateVarConditions()
{
    const std::set<std::string> Active = ActiveVarKeys(CustomVarGroup);
    for (QWidget* Cell : CustomVarGroup->findChildren<QWidget*>())
    {
        if (!Cell->property("CVWhen").isValid()) continue;                      // a gated row's control carries it
        const std::string Key = Cell->property("CVRowKey").toString().toStdString();
        if (auto* Row = reinterpret_cast<QTreeWidgetItem*>(Cell->property("CVItem").value<quintptr>()))
            Row->setHidden(!(Key.empty() || Active.count(Key) > 0));
    }
    if (OptionsTree) HideEmptySections(OptionsTree->invisibleRootItem());
}

void PreLaunchWindow::RebuildCustomVarPickers()
{
    //clear() frees the rows at once but only deleteLater()s their controls — which still carry CVWhen/CVItem (a
    //pointer to the freed row) until the event loop runs, and the condition pass below walks them in this very call.
    //So the controls go first, synchronously (their pending deleteLater is dropped with them).
    std::vector<QWidget*> OldControls;
    for (QTreeWidgetItemIterator It(OptionsTree); *It; ++It)
        if (QWidget* W = OptionsTree->itemWidget(*It, 1)) OldControls.push_back(W);
    OptionsTree->clear();
    for (QWidget* W : OldControls) delete W;
    const Node* L = CurrentLaunch();
    if (!L) { SetTabShown(OptionsPage, false); return; }

    // The knobs are the FOLDED declarations: the row's (with the ticked grafts), then every chain runner's.
    const Fold::Library Lib = ManifestModel::LibraryOf(*Index);
    std::vector<std::pair<nlohmann::ordered_json, bool>> Decls;   // (a CustomVar-shaped declaration, isRunnerKnob)
    auto AddDecls = [&](const Fold::Plan& P, bool IsRunner) {
        for (const auto& [K, D] : P.Decls.items())
        {
            nlohmann::ordered_json CV = { {"TYPE", "CustomVar"}, {"KEY", K} };
            if (D.is_object()) for (const auto& [F, X] : D.items()) CV[F] = X;
            Decls.push_back({ std::move(CV), IsRunner });
        }
    };
    AddDecls(Fold::Resolve(Lib, L->Key(), {}, {}, CollectGrafts()), false);
    for (const std::string& Rid : CurrentChain)
    {
        if (Rid == LaunchResolver::kNativeTerminalId || !Index->Find(Rid)) continue;
        AddDecls(Fold::Resolve(Lib, Rid), true);
    }

    const nlohmann::ordered_json SavedVars = PackageCatalog::GetPackageVariables(*GlobalConfigJSON, PackageUID, InstanceName);

    // Sections (UI.SECTION paths; a runner's options under "Runner"), created as their first option appears.
    std::map<std::string, QTreeWidgetItem*> Sections;

    bool AnyVisible = false, AnyCond = false;
    struct PendingRow { std::string Section; QString Label; QWidget* Field; };
    std::vector<PendingRow> Rows;     // placed once all are known: loose options above the sections
    std::set<std::string> SeenKeys;   // a KEY surfaces once; package nodes walked first so they win on collision

    for (const auto& [CVj, IsRunner] : Decls)
    {
        {
            const nlohmann::ordered_json& CV = CVj;
            if (!CV.is_object() || CV.value("TYPE", std::string()) != "CustomVar") continue;
            if (!CV.contains("UI") || !CV["UI"].is_object()) continue;          // only UI-facet vars are shown
            const std::string Key = CV.value("KEY", std::string());
            if (Key.empty() || SeenKeys.count(Key)) continue;
            const nlohmann::ordered_json& UI = CV["UI"];
            const std::string Control = UI.value("CONTROL", std::string("text"));

            std::string Initial = CV.value("DEFAULT", std::string());
            if (SavedVars.contains(Key) && SavedVars[Key].is_string()) Initial = std::string(SavedVars[Key]);
            { ContainerParams TmpP("", "", ""); VarSubst::StringVariableSubstitution(Initial, TmpP.GetVariablesMap()); }

            const QString Label = QString::fromStdString(UI.value("LABEL", Key));
            QWidget* Field = nullptr;       // the value control (carries CVKey); null for an uneditable secret pool

            if (Control == "enum" && UI.contains("CHOICES") && UI["CHOICES"].is_array())
            {
                QComboBox* Combo = new QComboBox(CustomVarGroup);
                //A choice is either {LABEL, VALUE} or the bare-string shorthand, where the string is both.
                //`Opt.value(...)` on a string THROWS, and this runs inside a dialog with no catch above it,
                //so the shorthand — the obvious thing to write — took the whole app down for every game whose
                //closure pulled that shared CustomVar in.
                for (const auto& Opt : UI["CHOICES"])
                {
                    if (Opt.is_string())
                    {
                        const QString S = QString::fromStdString(Opt.get<std::string>());
                        Combo->addItem(S, S);
                    }
                    else if (Opt.is_object())
                        Combo->addItem(QString::fromStdString(Opt.value("LABEL", std::string())),
                                       QString::fromStdString(Opt.value("VALUE", std::string())));
                }
                for (int k = 0; k < Combo->count(); k++)
                    if (Combo->itemData(k).toString().toStdString() == Initial) { Combo->setCurrentIndex(k); break; }
                connect(Combo, &QComboBox::currentIndexChanged, this, [this](int){ EvaluateVarConditions(); });
                Field = Combo;
            }
            else if (Control == "bool")
            {
                QCheckBox* Check = new QCheckBox(CustomVarGroup);
                std::string lo = Initial; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                Check->setChecked(lo == "1" || lo == "true" || lo == "yes");
                connect(Check, &QCheckBox::toggled, this, [this](bool){ EvaluateVarConditions(); });
                Field = Check;
            }
            else if (Control == "int")
            {
                QSpinBox* Spin = new QSpinBox(CustomVarGroup);
                Spin->setRange(UI.value("MIN", -2147483647), UI.value("MAX", 2147483647));
                int IV = 0; try { IV = std::stoi(Initial); } catch (...) { IV = Spin->minimum(); }
                Spin->setValue(IV);
                connect(Spin, &QSpinBox::valueChanged, this, [this](int){ EvaluateVarConditions(); });
                Field = Spin;
            }
            else if (Control == "float")
            {
                QDoubleSpinBox* Spin = new QDoubleSpinBox(CustomVarGroup);
                Spin->setRange(UI.value("MIN", -1e12), UI.value("MAX", 1e12));
                double DV = 0.0; try { DV = std::stod(Initial); } catch (...) { DV = Spin->minimum(); }
                Spin->setValue(DV);
                connect(Spin, &QDoubleSpinBox::valueChanged, this, [this](double){ EvaluateVarConditions(); });
                Field = Spin;
            }
            else if (Control == "secret")
            {
                // Always editable and always collected — a POOL is only a SEED for the first launch. `Initial` is
                // already the persisted value when there is one; otherwise draw one pool entry so the field is
                // pre-filled and gets persisted on launch like any other value. The user can overwrite it (their
                // own CD key, account name, …) and that choice then sticks.
                QLineEdit* Edit = new QLineEdit(CustomVarGroup);
                Edit->setEchoMode(QLineEdit::Password);
                if (Initial.empty() && UI.contains("POOL") && UI["POOL"].is_array() && !UI["POOL"].empty())
                {
                    const auto& Pool = UI["POOL"];
                    const size_t Pick = static_cast<size_t>(QRandomGenerator::global()->bounded(int(Pool.size())));
                    if (Pool[Pick].is_string()) Initial = Pool[Pick].get<std::string>();
                    Edit->setPlaceholderText("from pool — edit to use your own");
                }
                Edit->setText(QString::fromStdString(Initial));
                Field = Edit;
            }
            else // text
            {
                QLineEdit* Edit = new QLineEdit(CustomVarGroup);
                Edit->setText(QString::fromStdString(Initial));
                if (UI.contains("PATTERN") && UI["PATTERN"].is_string())
                    Edit->setValidator(new QRegularExpressionValidator(
                        QRegularExpression(QString::fromStdString(std::string(UI["PATTERN"]))), Edit));
                connect(Edit, &QLineEdit::textChanged, this, [this](const QString&){ EvaluateVarConditions(); });
                Field = Edit;
            }

            if (!Field) continue;
            if (!qobject_cast<QLabel*>(Field))                                   // editable controls are collected
                Field->setProperty("CVKey", QString::fromStdString(Key));
            SeenKeys.insert(Key); AnyVisible = true;

            std::string Section = UI.value("SECTION", std::string());
            if (IsRunner) Section = Section.empty() ? std::string("Runner") : "Runner/" + Section;
            //A value control keeps a readable width rather than spanning the window; a tick box is as wide as itself.
            if (!qobject_cast<QCheckBox*>(Field)) Field->setMaximumWidth(340);
            //WHEN gates this row's visibility (and, in the resolver, its value); the control carries it with the row's
            //key. Top-level preferred; UI.WHEN is a UI-era alias.
            std::string When = CV.value("WHEN", std::string());
            if (When.empty()) When = UI.value("WHEN", std::string());
            if (!When.empty())
            {
                Field->setProperty("CVWhen", QString::fromStdString(When));
                Field->setProperty("CVRowKey", QString::fromStdString(Key));
                AnyCond = true;
            }
            Rows.push_back({ Section, Label, Field });
        }
    }
    //Loose options first, then the sections (each in the order its first option appeared).
    std::stable_sort(Rows.begin(), Rows.end(), [](const PendingRow& A, const PendingRow& B) { return A.Section.empty() && !B.Section.empty(); });
    for (const PendingRow& R : Rows)
    {
        auto* Row = new QTreeWidgetItem(SectionRow(OptionsTree, Sections, R.Section));
        Row->setText(0, R.Label);
        Row->setFlags(Qt::ItemIsEnabled);
        OptionsTree->setItemWidget(Row, 1, R.Field);
        R.Field->setProperty("CVItem", QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(Row)));
    }

    if (AnyCond) EvaluateVarConditions();                                       // apply initial WHEN visibility
    SetTabShown(OptionsPage, AnyVisible);
    //The label column fits the longest label at its depth — measured over every row, open or folded, so opening a
    //section never truncates what it shows — within reason; the controls take the rest.
    {
        const QFontMetrics Fm(OptionsTree->font());
        QFont Bold = OptionsTree->font(); Bold.setBold(true);
        const QFontMetrics FmB(Bold);
        int Need = 0;
        for (QTreeWidgetItemIterator It(OptionsTree); *It; ++It)
        {
            int Depth = 0;
            for (QTreeWidgetItem* P = (*It)->parent(); P; P = P->parent()) ++Depth;
            const bool Sec = (*It)->data(0, Qt::UserRole + 10).isValid();
            Need = std::max(Need, (Depth + 1) * OptionsTree->indentation() + (Sec ? FmB : Fm).horizontalAdvance((*It)->text(0)) + 24);
        }
        OptionsTree->setColumnWidth(0, std::clamp(Need, 160, 440));
    }
}

void PreLaunchWindow::BuildLanPanel(QVBoxLayout * LeftCol)
{
    LanGroup = new QGroupBox("Virtual LAN", this);
    QVBoxLayout * GL = new QVBoxLayout(LanGroup);
    GL->setContentsMargins(10, 6, 10, 8);
    GL->setSpacing(4);
    LanStatus = new QLabel(LanGroup);
    LanStatus->setStyleSheet("color:#8f98a0;font-size:9pt;");
    GL->addWidget(LanStatus);
    LanRows = new QVBoxLayout();
    LanRows->setSpacing(2);
    GL->addLayout(LanRows);

    // The virtual LAN is host-less: every accepted friend is simply present on the shared 10.66/16, discovered by
    // vIP with no join-a-specific-friend targeting. The panel below just shows each friend's live link status.
    LanGroup->setVisible(false);                 // shown once the first poll finds friends / a LAN
    LeftCol->addWidget(LanGroup);

    LanTimer = new QTimer(this);
    LanTimer->setInterval(2000);
    connect(LanTimer, &QTimer::timeout, this, &PreLaunchWindow::RefreshLanPanel);
    LanTimer->start();
    RefreshLanPanel();
}

void PreLaunchWindow::RefreshLanPanel()
{
    const std::vector<IpfsWrapper::LanPeer> Peers = IpfsWrapper::LanPeers();

    // Own status line: our vIP comes from the LAN launch vars (cheap; empty when the node/LAN is down).
    const auto Vars = IpfsWrapper::LanLaunchVars();
    const auto VipIt = Vars.find("VIDYAGOD_SELF_VIP");
    if (VipIt != Vars.end())
        LanStatus->setText("Your vIP: " + QString::fromStdString(VipIt->second) + " — LAN ready");
    else
        LanStatus->setText("LAN offline (node down)");

    // The excluded set (GLOBAL roster) from config — ticks reflect it, toggles rewrite it.
    std::set<std::string> Excluded;
    {
        const auto & S = (*GlobalConfigJSON)["Settings"];
        if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
            for (const auto & P : S["LanExcludedPeers"])
                if (P.is_string()) Excluded.insert(P.get<std::string>());
    }

    // Reconcile rows: upsert every reported friend, drop rows for friends that vanished.
    std::set<std::string> Seen;
    for (const IpfsWrapper::LanPeer & P : Peers)
    {
        Seen.insert(P.Peer);
        auto It = LanRowByPeer.find(P.Peer);
        if (It == LanRowByPeer.end())
        {
            QWidget * Row = new QWidget(LanGroup);
            Row->setObjectName(QString::fromStdString("lanrow_" + P.Peer));
            QHBoxLayout * RL = new QHBoxLayout(Row);
            RL->setContentsMargins(0, 0, 0, 0);
            RL->setSpacing(6);
            QCheckBox * Cb = new QCheckBox(QString::fromStdString(P.Nick.empty() ? P.Peer.substr(0, 8) : P.Nick), Row);
            Cb->setChecked(!Excluded.count(P.Peer));
            const std::string Peer = P.Peer;
            connect(Cb, &QCheckBox::toggled, this, [this, Peer](bool On){
                // Rewrite Settings.LanExcludedPeers (the GLOBAL roster) + push it into the node — live, mid-game too.
                auto & S = (*GlobalConfigJSON)["Settings"];
                std::vector<std::string> Ex;
                if (S.contains("LanExcludedPeers") && S["LanExcludedPeers"].is_array())
                    for (const auto & E : S["LanExcludedPeers"])
                        if (E.is_string() && E.get<std::string>() != Peer) Ex.push_back(E.get<std::string>());
                if (!On) Ex.push_back(Peer);
                nlohmann::ordered_json Arr = nlohmann::ordered_json::array();
                for (const std::string & E : Ex) Arr.push_back(E);
                S["LanExcludedPeers"] = Arr;
                persistGlobalConfig();
                IpfsWrapper::SetLanExcluded(Ex);
            });
            RL->addWidget(Cb);
            RL->addStretch();
            QLabel * Badge = new QLabel(Row);
            Badge->setStyleSheet("font-size:9pt;");
            RL->addWidget(Badge);
            LanRows->addWidget(Row);
            It = LanRowByPeer.emplace(P.Peer, std::make_pair(Cb, Badge)).first;
        }
        // Badge: link quality at a glance — green direct (+RTT), amber relayed, grey otherwise.
        QLabel * Badge = It->second.second;
        if (P.Link == "direct")
        {
            QString T = "● direct";
            if (P.RttMs >= 0) T += QString(" · %1 ms").arg(P.RttMs);
            Badge->setText(T);
            Badge->setStyleSheet("font-size:9pt;color:#6dbf6d;");
        }
        else if (P.Link == "relayed") { Badge->setText("● relayed");    Badge->setStyleSheet("font-size:9pt;color:#c9a227;"); }
        else if (P.Link == "connecting") { Badge->setText("○ connecting…"); Badge->setStyleSheet("font-size:9pt;color:#8f98a0;"); }
        else                          { Badge->setText("○ offline");    Badge->setStyleSheet("font-size:9pt;color:#8f98a0;"); }
    }
    for (auto It = LanRowByPeer.begin(); It != LanRowByPeer.end();)
    {
        if (!Seen.count(It->first))
        {
            if (QWidget * Row = It->second.first->parentWidget()) Row->deleteLater();
            It = LanRowByPeer.erase(It);
        }
        else ++It;
    }
    LanGroup->setVisible(!Peers.empty());
}

void PreLaunchWindow::onVariantChanged()
{
    if (VariantCombo->currentIndex() < 0) return;
    //Combo data = "<node key>\x1f<entrypoint label>": a variant is (node, entrypoint).
    //Combo data = "<variant key>\x1f<entry label>[\x1f<graft key>]": a row is (variant, entry) — of the variant's own
    //effective entries, or of a ticked graft that carries entries (a mod loader: "run Forge").
    const std::string Data = VariantCombo->currentData().toString().toStdString();
    const size_t Sep = Data.find('\x1f');
    const std::string PrevLaunch = LaunchNodeId;
    LaunchNodeId = Data.substr(0, Sep);
    std::string Rest = Sep == std::string::npos ? std::string() : Data.substr(Sep + 1);
    Entrypoint = Rest.substr(0, Rest.find('\x1f'));
    if (const Node* L = CurrentLaunch()) { BundleDir = L->BundleDir.string(); PackageUID = L->GameKey(); }
    RebuildCover();
    RebuildRunnerChain();
    RebuildModuleTree();
    RebuildCustomVarPickers();
    if (PrevLaunch != LaunchNodeId) RefreshGraftEntryRows();
}

void PreLaunchWindow::RefreshGraftEntryRows()
{
    //Rows a ticked graft with entries adds for the CURRENT variant — appended after the variant rows (data with a
    //third field), removed and re-added on every tick change or variant change. Selection is preserved by data.
    QSignalBlocker B(VariantCombo);
    const QString Keep = VariantCombo->currentData().toString();
    for (int i = VariantCombo->count() - 1; i >= 0; --i)
        if (VariantCombo->itemData(i).toString().count(QChar(0x1f)) >= 2) VariantCombo->removeItem(i);
    const Node* L = CurrentLaunch();
    if (L && Index)
    {
        //The entries the ticked grafts ADD to this row's fold (a mod loader), beyond the row's own.
        const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), L->Key(), {}, {}, CollectGrafts());
        for (const auto& [Label, E] : P.Exec.items())
        {
            if (L->Entries.contains(Label)) continue;
            if (E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) continue;   // a runner entry
            VariantCombo->addItem(QString::fromStdString("run " + Label),
                                  QString::fromStdString(LaunchNodeId + "\x1f" + Label + "\x1fgraft"));
        }
    }
    int Sel = VariantCombo->findData(Keep);
    bool Vanished = false;
    if (Sel < 0)
    {
        // The selected row was a graft's entry and that graft was just unticked: fall back to the variant's own
        // rows and RE-READ the selection, or Entrypoint keeps naming a loader the mount no longer holds.
        Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId + "\x1f" + Entrypoint));
        if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
        Vanished = Sel >= 0;
    }
    if (Sel >= 0) VariantCombo->setCurrentIndex(Sel);
    if (Vanished) onVariantChanged();
}

void PreLaunchWindow::ReloadAndRebuild()
{
    // Drop editions that no longer exist (e.g. after an edit) and rebuild everything from the (shared) index.
    std::vector<std::string> Live;
    for (const std::string& Id : GroupNodeIds) if (Index && Index->Find(Id)) Live.push_back(Id);
    GroupNodeIds = Live;
    QSignalBlocker B(VariantCombo);
    FillVariantCombo();
    int Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId + "\x1f" + Entrypoint));
    if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
    VariantCombo->setCurrentIndex(Sel >= 0 ? Sel : 0);
    onVariantChanged();
}

// ---- instances ----------------------------------------------------------------------------------------------------

void PreLaunchWindow::FillInstances()
{
    if (!InstanceCombo) return;
    QSignalBlocker B(InstanceCombo);
    InstanceCombo->clear();
    std::vector<std::string> Names = PackageUID.empty() ? std::vector<std::string>{}
                                                        : InstanceStore::List(*GlobalConfigJSON, PackageUID);
    //A game never played has no instance yet: its default one is created by the first launch.
    if (std::find(Names.begin(), Names.end(), InstanceName) == Names.end() && !InstanceName.empty()) Names.push_back(InstanceName);
    for (const std::string& N : Names)
        InstanceCombo->addItem(N == InstanceStore::DefaultInstance ? QString("Default") : QString::fromStdString(N),
                               QString::fromStdString(N));
    InstanceCombo->setCurrentIndex(std::max(0, InstanceCombo->findData(QString::fromStdString(InstanceName))));
}

void PreLaunchWindow::onInstanceChanged()
{
    const std::string Picked = InstanceCombo->currentData().toString().toStdString();
    if (Picked.empty() || Picked == InstanceName) return;
    InstanceName = Picked;
    //Everything the instance holds: its runner chain, its add-ons, its option values.
    RebuildRunnerChain();
    RebuildModuleTree();
    RebuildCustomVarPickers();
    RefreshGraftEntryRows();
    RefreshInstanceInfo();
}

void PreLaunchWindow::RefreshInstanceInfo()
{
    if (!InstanceInfo) return;
    const auto Cfg = PackageUID.empty() ? nlohmann::ordered_json() : InstanceStore::ReadConfig(*GlobalConfigJSON, PackageUID, InstanceName);
    const QString Last = Cfg.is_object() && Cfg.contains("LASTRUN") && Cfg["LASTRUN"].is_string()
                       ? QString::fromStdString(Cfg["LASTRUN"].get<std::string>()) : QString();
    const QDateTime At = QDateTime::fromString(Last, Qt::ISODate);
    if (!At.isValid()) { InstanceInfo->setText("Never played"); return; }
    const qint64 Days = At.toLocalTime().date().daysTo(QDate::currentDate());
    InstanceInfo->setText(Days <= 0 ? "Last played today" : Days == 1 ? "Last played yesterday"
                          : Days < 30 ? QString("Last played %1 days ago").arg(Days)
                          : "Last played " + At.toLocalTime().date().toString("d MMM yyyy"));
    InstanceInfo->setToolTip(At.toLocalTime().toString(Qt::TextDate));
}

namespace {
//A name for a new instance, or "" when cancelled; an invalid or taken name is refused and asked again.
std::string AskInstanceName(QWidget* Parent, const QString& Title, const QString& Suggest,
                            const nlohmann::ordered_json& Cfg, const std::string& Uid)
{
    QString Name = Suggest;
    for (;;)
    {
        bool Ok = false;
        Name = QInputDialog::getText(Parent, Title, "Name:", QLineEdit::Normal, Name, &Ok).trimmed();
        if (!Ok || Name.isEmpty()) return {};
        const std::string N = Name.toStdString();
        const auto Have = InstanceStore::List(Cfg, Uid);
        if (!InstanceStore::ValidName(N))
            QMessageBox::warning(Parent, Title, "An instance name is one folder name: no slashes, up to 64 characters.");
        else if (std::find(Have.begin(), Have.end(), N) != Have.end())
            QMessageBox::warning(Parent, Title, "There is already an instance called \"" + Name + "\".");
        else return N;
    }
}
}

void PreLaunchWindow::NewInstance()
{
    const std::string N = AskInstanceName(this, "New instance", "New instance", *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    if (!InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)) { QMessageBox::warning(this, "New instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

void PreLaunchWindow::DuplicateInstance()
{
    const QString Shown = InstanceCombo->currentText();
    const std::string N = AskInstanceName(this, "Duplicate instance", Shown + " copy", *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    const bool Exists = std::find(Have.begin(), Have.end(), InstanceName) != Have.end();
    //An instance never launched has nothing on disk to copy: the duplicate starts fresh, like it.
    if (!(Exists ? InstanceStore::Clone(*GlobalConfigJSON, PackageUID, InstanceName, N, &Err)
                 : InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)))
    { QMessageBox::warning(this, "Duplicate instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

void PreLaunchWindow::RenameInstance()
{
    const std::string N = AskInstanceName(this, "Rename instance", InstanceCombo->currentText(), *GlobalConfigJSON, PackageUID);
    if (N.empty()) return;
    std::string Err;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    const bool Exists = std::find(Have.begin(), Have.end(), InstanceName) != Have.end();
    if (!(Exists ? InstanceStore::Rename(*GlobalConfigJSON, PackageUID, InstanceName, N, &Err)
                 : InstanceStore::Create(*GlobalConfigJSON, PackageUID, N, &Err)))
    { QMessageBox::warning(this, "Rename instance", QString::fromStdString(Err)); return; }
    InstanceName = N;
    FillInstances();
    RefreshInstanceInfo();
}

void PreLaunchWindow::DeleteInstance()
{
    const QString Shown = InstanceCombo->currentText();
    QMessageBox Ask(QMessageBox::Warning, "Delete instance",
                    "Delete the instance \"" + Shown + "\"?\n\nIts saves and settings are deleted with it. This cannot be undone.",
                    QMessageBox::Cancel, this);
    QPushButton* Del = Ask.addButton("Delete", QMessageBox::DestructiveRole);
    Ask.setDefaultButton(QMessageBox::Cancel);
    Ask.exec();
    if (Ask.clickedButton() != Del) return;
    const auto Have = InstanceStore::List(*GlobalConfigJSON, PackageUID);
    if (std::find(Have.begin(), Have.end(), InstanceName) != Have.end())
    {
        std::string Err;
        if (!InstanceStore::Delete(*GlobalConfigJSON, PackageUID, InstanceName, &Err))
        { QMessageBox::warning(this, "Delete instance", QString::fromStdString(Err)); return; }
    }
    InstanceName = InstanceStore::ResolveActive(*GlobalConfigJSON, PackageUID);
    FillInstances();
    RebuildRunnerChain(); RebuildModuleTree(); RebuildCustomVarPickers(); RefreshGraftEntryRows(); RefreshInstanceInfo();
}

// ---- tabs -----------------------------------------------------------------------------------------------------------

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

void PreLaunchWindow::FillVariantCombo()
{
    QSignalBlocker B(VariantCombo);
    VariantCombo->clear();
    struct E { std::string Id; QString Lbl; bool Rec; };
    std::vector<E> Es;
    for (const std::string& Id : GroupNodeIds)
    {
        const Node* N = Index ? Index->Find(Id) : nullptr;
        if (!N) continue;
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
        VariantCombo->addItem((X.Rec ? QStringLiteral("⭐ ") : QString()) + X.Lbl, QString::fromStdString(X.Id));
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
    QDir AppDataDir(QString::fromStdString(AppPaths::DataRoot().string()));
    JSONOps::SaveJSON(GlobalConfigJSON, new QFile(AppDataDir.filePath("GlobalConfig.JSON")));
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
        PackageCatalog::MergePackageVariables(*GlobalConfigJSON, PackageUID, PickerVars, InstanceName);
        SaveGrafts();
        //Persist the whole resolved chain (RUNNER_CHAIN supersedes the old single PREFERRED_RUNNER). The resolver
        //honours it on the next launch (and the chain UI pre-selects it).
        { nlohmann::ordered_json ChainJson = nlohmann::ordered_json::array();
          for (const std::string& Id : SelectedChain) ChainJson.push_back(Id);
          PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "RUNNER_CHAIN", ChainJson, InstanceName); }
        if (RememberCheck->isChecked())
            PackageCatalog::SetPackageUserSetting(*GlobalConfigJSON, PackageUID, "SKIP_LAUNCH_DIALOG", true, InstanceName);
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

    // Build + start the worker — native node launch via LaunchNodeId.
    LaunchWorker = new LaunchThread();
    LaunchWorker->GlobalConfigJSON = *GlobalConfigJSON;
    LaunchWorker->LaunchNodeId     = LaunchNodeId;
    LaunchWorker->Entrypoint       = Entrypoint;
    LaunchWorker->Face             = FaceUid;
    LaunchWorker->InstanceName     = InstanceName;
    LaunchWorker->VariableOverrides = PickerVars;
    LaunchWorker->Grafts            = CollectGrafts();
    LaunchWorker->RunnerChain       = SelectedChain;                          // the full daisy-chain (innermost→outermost)
    LaunchWorker->RunnerID          = SelectedChain.empty() ? std::string() : SelectedChain.front();  // back-compat
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
