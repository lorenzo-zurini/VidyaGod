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

AuthoringSessionWindow::AuthoringSessionWindow(PackageEditorModel * Editor, const std::string & TargetNodeId, QWidget * parent)
    : QWidget(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QString::fromStdString("Capture Setup — " + TargetNodeId));
    resize(860, 700);

    Model = new AuthoringSessionModel(Editor, TargetNodeId, this);

    auto * Root = new QVBoxLayout(this);

    StatusLabel = new QLabel("Starting session…", this);
    StatusLabel->setStyleSheet("font-weight:bold;");
    InfoLabel = new QLabel(this);
    InfoLabel->setWordWrap(true);
    InfoLabel->setStyleSheet("color:#8f98a0;font-size:9pt;");
    Root->addWidget(StatusLabel);
    Root->addWidget(InfoLabel);

    // ── Run tools + runner picker ──
    auto * ToolRow = new QHBoxLayout();
    RunExeBtn = new QPushButton("Run EXE…", this);
    BrowseBtn = new QPushButton("Open Explorer", this);
    RegBtn    = new QPushButton("Edit Registry", this);
    ToolRow->addWidget(RunExeBtn); ToolRow->addWidget(BrowseBtn); ToolRow->addWidget(RegBtn);
    ToolRow->addWidget(new QLabel("Runner:", this));
    RunnerCombo = new QComboBox(this);
    RunnerCombo->setToolTip("The runner this session runs under. Switching rebuilds the runtime (discards uncaptured changes).");
    ToolRow->addWidget(RunnerCombo);
    ToolRow->addStretch();
    RefreshBtn = new QPushButton("Refresh changes", this);
    ToolRow->addWidget(RefreshBtn);
    Root->addLayout(ToolRow);

    // ── Shared: which node captures land on ──
    auto * TargetRow = new QHBoxLayout();
    TargetRow->addWidget(new QLabel("Capture into node:", this));
    TargetCombo = new QComboBox(this);
    for (const QString & Id : Model->bundleNodeIds()) TargetCombo->addItem(Id);
    TargetCombo->setCurrentText(Model->targetNode());
    TargetRow->addWidget(TargetCombo); TargetRow->addStretch();
    Root->addLayout(TargetRow);

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
    auto * Preview = new QLabel(FilesTab);   // live "you picked this level" feedback
    Preview->setWordWrap(true);
    Preview->setStyleSheet("color:#7ec699;font-size:9pt;");
    FLay->addWidget(Preview);
    connect(Tree, &DeltaTree::checkedChanged, this, [this, Preview]{
        const QStringList Roots = Tree->checkedRoots();
        Preview->setText(Roots.isEmpty() ? "Nothing checked."
                                         : "Will capture at this level → " + Roots.join("   ·   "));
    });
    auto * FForm = new QFormLayout();
    DestNameEdit = new QLineEdit(QString::fromStdString(TargetNodeId + "_files"), FilesTab);
    FForm->addRow("Captured dir (in bundle)", DestNameEdit);
    TargetEdit = new QLineEdit(FilesTab);
    TargetEdit->setToolTip("Where the captured layer mounts, relative to the content root ('' = at the content root).");
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

    auto * EndRow = new QHBoxLayout();
    EndRow->addStretch();
    EndBtn = new QPushButton("End session (unmount + wipe)", this);
    EndRow->addWidget(EndBtn);
    Root->addLayout(EndRow);

    // ── UI → model ──
    connect(RunnerCombo, &QComboBox::textActivated, Model, &AuthoringSessionModel::switchRunner);
    connect(RunExeBtn, &QPushButton::clicked, this, [this]{
        const QString Exe = QFileDialog::getOpenFileName(this, "Select an executable to run in the prefix (installer, …)");
        if (!Exe.isEmpty()) Model->runExe(Exe);
    });
    connect(BrowseBtn, &QPushButton::clicked, this, [this]{ Model->runGuest("explorer.exe"); });
    connect(RegBtn,    &QPushButton::clicked, this, [this]{ Model->runGuest("regedit.exe"); });
    connect(RefreshBtn,&QPushButton::clicked, Model, &AuthoringSessionModel::refreshDelta);
    connect(ScanRegBtn,&QPushButton::clicked, Model, &AuthoringSessionModel::scanRegistry);
    connect(CaptureFilesBtn, &QPushButton::clicked, this, [this]{
        const QStringList Sel = Tree->checkedRoots();
        if (Sel.isEmpty()) { QMessageBox::information(this, "Capture files", "Check at least one changed file/folder to capture."); return; }
        if (DestNameEdit->text().trimmed().isEmpty()) { QMessageBox::warning(this, "Capture files", "Give the captured directory a name."); return; }
        Model->captureFiles(Sel, TargetCombo->currentText(), DestNameEdit->text().trimmed(), TargetEdit->text().trimmed());
    });
    connect(CaptureRegBtn, &QPushButton::clicked, this, [this]{
        const QStringList Sel = RegTree->checkedEntries();
        if (Sel.isEmpty()) { QMessageBox::information(this, "Capture registry", "Check at least one changed key to capture."); return; }
        Model->captureSelectedRegistry(Sel, TargetCombo->currentText());
    });
    connect(EndBtn, &QPushButton::clicked, this, [this]{ close(); });

    // ── model → UI ──
    connect(Model, &AuthoringSessionModel::busyChanged, this, [this](bool Busy, const QString & What){
        for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn, RefreshBtn, ScanRegBtn, CaptureFilesBtn, CaptureRegBtn, EndBtn})
            B->setEnabled(!Busy);
        RunnerCombo->setEnabled(!Busy);
        if (Busy && !What.isEmpty()) StatusLabel->setText(What);
        else if (!Busy)             StatusLabel->setText("Session live.");
    });
    connect(Model, &AuthoringSessionModel::sessionReady, this, [this, Tabs, RegTabIdx](const QString & Rt, const QString & Cr, bool Wine){
        TargetEdit->setText("");
        for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn}) B->setVisible(Wine);
        Tabs->setTabVisible(RegTabIdx, Wine);   // registry capture is wine-only
        InfoLabel->setText("Live runtime: " + Rt +
            (Wine ? "\nContent root: " + Cr + "  — install the game into this location (the wine C: drive) so the "
                    "capture re-mounts where it installed (or re-home it later via the layer TARGET)."
                  : "\nContent root: " + Cr));
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
