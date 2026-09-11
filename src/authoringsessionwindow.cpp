#include "authoringsessionwindow.h"
#include "authoringsessionmodel.h"
#include "deltatree.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

// A check-all / uncheck-all row under a DeltaTree.
static QHBoxLayout * checkRow(DeltaTree * Tree, QWidget * parent)
{
    auto * Row = new QHBoxLayout();
    auto * All  = new QPushButton("Check all", parent);
    auto * None = new QPushButton("Uncheck all", parent);
    Row->addWidget(All); Row->addWidget(None); Row->addStretch();
    QObject::connect(All,  &QPushButton::clicked, Tree, [Tree]{ Tree->checkAll(true); });
    QObject::connect(None, &QPushButton::clicked, Tree, [Tree]{ Tree->checkAll(false); });
    return Row;
}

AuthoringSessionWindow::AuthoringSessionWindow(PackageEditorModel * Editor, const std::string & TargetNodeId,
                                               CaptureMode Mode, QWidget * parent)
    : QWidget(parent, Qt::Window), Mode(Mode)
{
    setAttribute(Qt::WA_DeleteOnClose);
    const char * Verb = Mode == CaptureMode::Files    ? "Browse files"
                      : Mode == CaptureMode::Registry ? "Edit registry"
                                                      : "Capture setup";
    //The title says where in the chain this is anchored: the runtime is built UP TO this node, and whatever is
    //captured becomes a new node parented here.
    setWindowTitle(QString("%1 at '%2'").arg(Verb).arg(QString::fromStdString(TargetNodeId)));
    resize(860, 700);

    Model = new AuthoringSessionModel(Editor, TargetNodeId, Mode, this);

    auto * Root = new QVBoxLayout(this);

    StatusLabel = new QLabel("Starting session…", this);
    StatusLabel->setStyleSheet("font-weight:bold;");
    InfoLabel = new QLabel(this);
    InfoLabel->setWordWrap(true);
    InfoLabel->setStyleSheet("color:#8f98a0;font-size:9pt;");
    Root->addWidget(StatusLabel);
    Root->addWidget(InfoLabel);

    // ── Tools (each carries its own environment) + the wine-tool's runner picker ──
    auto * ToolRow = new QHBoxLayout();
    RunExeBtn = new QPushButton("Run Windows program…", this);
    BrowseBtn = new QPushButton("Open Explorer", this);
    RegBtn    = new QPushButton("Edit Registry", this);
    ToolRow->addWidget(RunExeBtn); ToolRow->addWidget(BrowseBtn); ToolRow->addWidget(RegBtn);
    ToolRow->addWidget(new QLabel("Wine runner:", this));
    RunnerCombo = new QComboBox(this);
    RunnerCombo->setToolTip("The wine/proton runner the 'Run Windows program' tool uses. Independent of the package's "
                            "platform — the first Windows run builds the prefix around the current runtime.");
    ToolRow->addWidget(RunnerCombo);
    ToolRow->addStretch();
    RefreshBtn = new QPushButton("Refresh changes", this);
    ToolRow->addWidget(RefreshBtn);
    Root->addLayout(ToolRow);

    // A capture CREATES a node parented at the anchor — there is no target to pick any more. Say where it will
    // land instead, because "captured into thin air" is the thing an author needs to not wonder about.
    auto * AnchorLabel = new QLabel(
        QString("Captures become NEW nodes parented at <b>%1</b> — they apply exactly where this session is anchored.")
            .arg(QString::fromStdString(TargetNodeId)), this);
    AnchorLabel->setWordWrap(true);
    AnchorLabel->setStyleSheet("color:#7ec699;font-size:9pt;");
    Root->addWidget(AnchorLabel);

    auto * Tabs = new QTabWidget(this);
    Root->addWidget(Tabs, 1);

    // ── Files tab ──
    auto * FilesTab = new QWidget(Tabs);
    auto * FLay = new QVBoxLayout(FilesTab);
    FLay->addWidget(new QLabel("Changed files — check what to capture (a ticked folder is captured at its level, "
                               "without its parent folders).", FilesTab));
    Tree = new DeltaTree(FilesTab);
    FLay->addWidget(Tree, 1);
    FLay->addLayout(checkRow(Tree, FilesTab));
    FilesPreview = new QLabel(FilesTab);     // live "you picked this level" + where it mounts
    FilesPreview->setWordWrap(true);
    FilesPreview->setStyleSheet("color:#7ec699;font-size:9pt;");
    FLay->addWidget(FilesPreview);
    connect(Tree, &DeltaTree::checkedChanged, this, &AuthoringSessionWindow::updateCapturePreview);
    auto * FForm = new QFormLayout();
    DestNameEdit = new QLineEdit(QString::fromStdString(TargetNodeId + "_files"), FilesTab);
    FForm->addRow("Captured dir (in bundle)", DestNameEdit);
    TargetEdit = new QLineEdit(FilesTab);
    TargetEdit->setToolTip("Where the captured layer mounts, relative to the content root ('' = at the content root).");
    connect(TargetEdit, &QLineEdit::textChanged, this, [this]{ updateCapturePreview(); });
    FForm->addRow("Mount TARGET", TargetEdit);
    FLay->addLayout(FForm);
    CaptureFilesBtn = new QPushButton("Capture files →", FilesTab);
    FLay->addWidget(CaptureFilesBtn);
    Tabs->addTab(FilesTab, "Files");

    // ── Registry tab (mirror) ──
    auto * RegTab = new QWidget(Tabs);
    auto * RLay = new QVBoxLayout(RegTab);
    ScanRegBtn = new QPushButton("Scan registry changes", RegTab);
    RLay->addWidget(ScanRegBtn);
    RegTree = new DeltaTree(RegTab);
    RegTree->setSeparator('\\');
    RLay->addWidget(RegTree, 1);
    RLay->addLayout(checkRow(RegTree, RegTab));
    CaptureRegBtn = new QPushButton("Capture registry →", RegTab);
    RLay->addWidget(CaptureRegBtn);
    const int RegTabIdx = Tabs->addTab(RegTab, "Registry");

    // The mode decides what this session IS. Files/Registry open one tool on one tab so the window says exactly
    // one thing; Setup keeps both, because an installer writes both and you want to see the two deltas together.
    if (Mode == CaptureMode::Files)
    {
        Tabs->removeTab(RegTabIdx);
        RunExeBtn->setVisible(false); RegBtn->setVisible(false);
        RunnerCombo->setVisible(false);
        InfoLabel->setText("Add or change files in the window that opens; then check what you added and capture it.");
    }
    else if (Mode == CaptureMode::Registry)
    {
        Tabs->setCurrentIndex(RegTabIdx);
        Tabs->removeTab(0);
        RunExeBtn->setVisible(false); BrowseBtn->setVisible(false);
        RunnerCombo->setVisible(false);
        InfoLabel->setText("Make your changes in regedit; then Scan, check the keys you want, and capture them.");
    }

    auto * EndRow = new QHBoxLayout();
    EndRow->addStretch();
    EndBtn = new QPushButton("End session (unmount + wipe)", this);
    EndRow->addWidget(EndBtn);
    Root->addLayout(EndRow);

    // ── UI → model ──
    connect(RunExeBtn, &QPushButton::clicked, this, [this]{
        if (RunnerCombo->currentText().isEmpty())
        { QMessageBox::information(this, "Run Windows program", "No wine/proton runner is installed to run a Windows program."); return; }
        const QString Exe = QFileDialog::getOpenFileName(this, "Select a Windows program to run in the prefix (installer, editor, …)");
        if (!Exe.isEmpty()) Model->runWindows(Exe, RunnerCombo->currentText());
    });
    connect(BrowseBtn, &QPushButton::clicked, this, [this]{ Model->runGuest("explorer.exe"); });
    connect(RegBtn,    &QPushButton::clicked, this, [this]{ Model->runGuest("regedit.exe"); });
    connect(RefreshBtn,&QPushButton::clicked, Model, &AuthoringSessionModel::refreshDelta);
    connect(ScanRegBtn,&QPushButton::clicked, Model, &AuthoringSessionModel::scanRegistry);
    connect(CaptureFilesBtn, &QPushButton::clicked, this, [this]{
        const QStringList Sel = Tree->checkedRoots();
        if (Sel.isEmpty()) { QMessageBox::information(this, "Capture files", "Check at least one changed file/folder to capture."); return; }
        if (DestNameEdit->text().trimmed().isEmpty()) { QMessageBox::warning(this, "Capture files", "Give the captured directory a name."); return; }
        Model->captureFiles(Sel, DestNameEdit->text().trimmed(), TargetEdit->text().trimmed());
    });
    connect(CaptureRegBtn, &QPushButton::clicked, this, [this]{
        const QStringList Sel = RegTree->checkedEntries();
        if (Sel.isEmpty()) { QMessageBox::information(this, "Capture registry", "Check at least one changed key to capture."); return; }
        Model->captureSelectedRegistry(Sel);
    });
    connect(EndBtn, &QPushButton::clicked, this, [this]{ close(); });
    const QString AnchorId = QString::fromStdString(TargetNodeId);
    connect(Model, &AuthoringSessionModel::nodeCreated, this, [this, AnchorId](const QString & Id) {
        StatusLabel->setText(QString("Created node '%1' - it is on the canvas, parented at '%2'.")
                                 .arg(Id).arg(AnchorId));
    });

    // ── model → UI ──
    connect(Model, &AuthoringSessionModel::busyChanged, this, [this](bool Busy, const QString & What){
        for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn, RefreshBtn, ScanRegBtn, CaptureFilesBtn, CaptureRegBtn, EndBtn})
            B->setEnabled(!Busy);
        RunnerCombo->setEnabled(!Busy);
        if (Busy && !What.isEmpty()) StatusLabel->setText(What);
        else if (!Busy)             StatusLabel->setText("Session live.");
    });
    // In a single-purpose mode the tool IS the session: open it as soon as the runtime is live, rather than
    // making the author find the button that is the only thing they came here to press.
    connect(Model, &AuthoringSessionModel::sessionReady, this, [this](QString, QString, bool IsWine) {
        if (!IsWine) return;
        if (this->Mode == CaptureMode::Files)         Model->runGuest("explorer.exe");
        else if (this->Mode == CaptureMode::Registry) Model->runGuest("regedit.exe");
    });
    connect(Model, &AuthoringSessionModel::sessionReady, this, [this, Tabs, RegTabIdx](const QString & Rt, const QString & Cr, bool Wine){
        ContentRootStr = Cr;
        TargetEdit->setText("");
        updateCapturePreview();
        // The "Run Windows program" tool is always available (it establishes the prefix on first use); Explorer/regedit
        // + registry capture only make sense once a wine prefix exists.
        for (QPushButton * B : {BrowseBtn, RegBtn}) B->setVisible(Wine);
        Tabs->setTabVisible(RegTabIdx, Wine);   // registry capture is wine-only
        InfoLabel->setText("Live runtime: " + Rt +
            (Wine ? "\nContent root: " + Cr + "  — the Windows program installs into this location (the wine C: drive); "
                    "the capture re-mounts where it installed (or re-home it later via the layer TARGET)."
                  : "\nA bare, platform-agnostic runtime (no prefix). Run a Windows program to author under wine/proton, "
                    "or capture files dropped in directly."));
    });
    connect(Model, &AuthoringSessionModel::runnersChanged, this, [this](const QStringList & Runners, const QString & Current){
        const QSignalBlocker B(RunnerCombo);
        RunnerCombo->clear();
        RunnerCombo->addItems(Runners);
        RunnerCombo->setCurrentText(Current);
    });
    connect(Model, &AuthoringSessionModel::deltaChanged, this, [this](const QStringList & Paths){
        Tree->setPaths(Paths);
        StatusLabel->setText(QString("Session live — %1 changed file(s).").arg(Paths.size()));
    });
    connect(Model, &AuthoringSessionModel::registryTreeChanged, this, [this](const QStringList & Paths){
        RegTree->setPaths(Paths);
    });
    connect(Model, &AuthoringSessionModel::filesCaptured,    this, [this](const QStringList & Roots){ Tree->markCaptured(Roots); });
    connect(Model, &AuthoringSessionModel::registryCaptured, this, [this](const QStringList & Keys){ RegTree->markCaptured(Keys); });
    connect(Model, &AuthoringSessionModel::captured, this, [this](const QString & Msg){ StatusLabel->setText(Msg); });
    connect(Model, &AuthoringSessionModel::failed, this, [this](const QString & Msg){
        QMessageBox::critical(this, "Authoring session", Msg);
        close();
    });

    Model->start();
}

void AuthoringSessionWindow::updateCapturePreview()
{
    if (!FilesPreview) return;
    const QStringList Roots = Tree->checkedRoots();
    if (Roots.isEmpty()) { FilesPreview->setText("Nothing checked — tick a folder/file to capture it at that level."); return; }
    QString MountBase = ContentRootStr;
    const QString Target = TargetEdit->text().trimmed();
    if (!Target.isEmpty()) MountBase += "/" + Target;
    QStringList Lines;
    for (const QString & R : Roots)
    {
        const QString Base = R.section('/', -1);   // the ticked level's name (parents stripped)
        Lines << Base + "  →  " + MountBase + "/" + Base;
    }
    FilesPreview->setText("Captures at the ticked level (and where each mounts):\n  " + Lines.join("\n  "));
}
