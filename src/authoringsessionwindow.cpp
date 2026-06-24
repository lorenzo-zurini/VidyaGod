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
#include <QVBoxLayout>

AuthoringSessionWindow::AuthoringSessionWindow(PackageEditorModel * Editor, const std::string & TargetNodeId, QWidget * parent)
    : QWidget(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QString::fromStdString("Authoring session — " + TargetNodeId));
    resize(840, 660);

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

    // ── The write-delta tree (every level checkable) ──
    auto * DeltaBox = new QGroupBox("Changed files (the session's write-delta) — check what to capture", this);
    auto * DeltaLay = new QVBoxLayout(DeltaBox);
    Tree = new DeltaTree(DeltaBox);
    DeltaLay->addWidget(Tree);
    auto * SelRow = new QHBoxLayout();
    auto * SelAll = new QPushButton("Check all", DeltaBox);
    auto * SelNone= new QPushButton("Uncheck all", DeltaBox);
    SelRow->addWidget(SelAll); SelRow->addWidget(SelNone); SelRow->addStretch();
    DeltaLay->addLayout(SelRow);
    Root->addWidget(DeltaBox, 1);
    connect(SelAll,  &QPushButton::clicked, this, [this]{ Tree->checkAll(true); });
    connect(SelNone, &QPushButton::clicked, this, [this]{ Tree->checkAll(false); });

    // ── Capture into the package ──
    auto * CapBox = new QGroupBox("Capture into the package", this);
    auto * CapForm = new QFormLayout(CapBox);
    TargetCombo = new QComboBox(CapBox);
    for (const QString & Id : Model->bundleNodeIds()) TargetCombo->addItem(Id);
    TargetCombo->setCurrentText(Model->targetNode());
    CapForm->addRow("Target node", TargetCombo);
    DestNameEdit = new QLineEdit(QString::fromStdString(TargetNodeId + "_files"), CapBox);
    CapForm->addRow("Captured dir (in bundle)", DestNameEdit);
    StripEdit = new QLineEdit(CapBox);
    StripEdit->setToolTip("Path prefix removed from each captured file (default: the content root, so a content-root "
                          "install re-mounts exactly where it installed).");
    CapForm->addRow("Strip prefix", StripEdit);
    TargetEdit = new QLineEdit(CapBox);
    TargetEdit->setToolTip("Where the captured layer mounts, relative to the content root ('' = at the content root).");
    CapForm->addRow("Mount TARGET", TargetEdit);
    auto * CapBtnRow = new QHBoxLayout();
    CaptureFilesBtn = new QPushButton("Capture files →", CapBox);
    CaptureRegBtn   = new QPushButton("Capture registry →", CapBox);
    CapBtnRow->addWidget(CaptureFilesBtn); CapBtnRow->addWidget(CaptureRegBtn); CapBtnRow->addStretch();
    CapForm->addRow(CapBtnRow);
    Root->addWidget(CapBox);

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
    connect(CaptureFilesBtn, &QPushButton::clicked, this, [this]{
        const QStringList Sel = Tree->checkedFiles();
        if (Sel.isEmpty()) { QMessageBox::information(this, "Capture files", "Check at least one changed file to capture."); return; }
        if (DestNameEdit->text().trimmed().isEmpty()) { QMessageBox::warning(this, "Capture files", "Give the captured directory a name."); return; }
        Model->captureFiles(Sel, TargetCombo->currentText(), DestNameEdit->text().trimmed(), StripEdit->text(), TargetEdit->text().trimmed());
    });
    connect(CaptureRegBtn, &QPushButton::clicked, this, [this]{ Model->captureRegistry(TargetCombo->currentText()); });
    connect(EndBtn, &QPushButton::clicked, this, [this]{ close(); });

    // ── model → UI ──
    connect(Model, &AuthoringSessionModel::busyChanged, this, [this](bool Busy, const QString & What){
        for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn, RefreshBtn, CaptureFilesBtn, CaptureRegBtn, EndBtn})
            B->setEnabled(!Busy);
        RunnerCombo->setEnabled(!Busy);
        if (Busy && !What.isEmpty()) StatusLabel->setText(What);
        else if (!Busy)             StatusLabel->setText("Session live.");
    });
    connect(Model, &AuthoringSessionModel::sessionReady, this, [this](const QString & Rt, const QString & Cr, bool Wine){
        StripEdit->setText(Cr);
        TargetEdit->setText("");
        for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn, CaptureRegBtn}) B->setVisible(Wine);
        InfoLabel->setText("Live runtime: " + Rt +
            (Wine ? "\nContent root: " + Cr + "  — install the game into this location (the wine C: drive) so the "
                    "capture re-mounts exactly where it installed."
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
    connect(Model, &AuthoringSessionModel::captured, this, [this](const QString & Msg){ StatusLabel->setText(Msg); });
    connect(Model, &AuthoringSessionModel::failed, this, [this](const QString & Msg){
        QMessageBox::critical(this, "Authoring session", Msg);
        close();
    });

    Model->start();
}
