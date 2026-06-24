#include "authoringsessionwindow.h"
#include "packageeditormodel.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <filesystem>

namespace fs = std::filesystem;

AuthoringSessionWindow::AuthoringSessionWindow(PackageEditorModel * M, const std::string & NodeId, QWidget * parent)
    : QDialog(parent), Model(M), TargetNodeId(NodeId)
{
    setWindowFlags(Qt::Window);                  // first-class window (own taskbar entry, normal minimize)
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QString::fromStdString("Authoring session — " + TargetNodeId));
    resize(820, 640);

    auto * Root = new QVBoxLayout(this);

    StatusLabel = new QLabel("Starting session…", this);
    StatusLabel->setStyleSheet("font-weight:bold;");
    Root->addWidget(StatusLabel);
    InfoLabel = new QLabel(this);
    InfoLabel->setWordWrap(true);
    InfoLabel->setStyleSheet("color:#8f98a0;font-size:9pt;");
    Root->addWidget(InfoLabel);

    // ── Run tools (wine-gated) + the runner picker ──
    auto * ToolRow = new QHBoxLayout();
    RunExeBtn = new QPushButton("Run EXE…", this);
    BrowseBtn = new QPushButton("Open Explorer", this);
    RegBtn    = new QPushButton("Edit Registry", this);
    RefreshBtn= new QPushButton("Refresh changes", this);
    ToolRow->addWidget(RunExeBtn); ToolRow->addWidget(BrowseBtn); ToolRow->addWidget(RegBtn);
    ToolRow->addWidget(new QLabel("Runner:", this));
    RunnerCombo = new QComboBox(this);
    RunnerCombo->setToolTip("The runner this authoring session runs under. Switching rebuilds the runtime "
                            "(discarding any uncaptured changes).");
    ToolRow->addWidget(RunnerCombo);
    ToolRow->addStretch(); ToolRow->addWidget(RefreshBtn);
    Root->addLayout(ToolRow);
    // textActivated fires only on a user pick (not on setCurrentText), so reflecting the auto-resolved runner won't loop.
    QObject::connect(RunnerCombo, &QComboBox::textActivated, this, [this](const QString & Rid){
        startSession({ Rid.toStdString() });
    });

    // ── The WRITELAYER delta (what the runs changed) ──
    auto * DeltaBox = new QGroupBox("Changed files (the session's write-delta) — check what to capture", this);
    auto * DeltaLay = new QVBoxLayout(DeltaBox);
    DeltaList = new QListWidget(DeltaBox);
    DeltaList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    DeltaLay->addWidget(DeltaList);
    auto * SelRow = new QHBoxLayout();
    auto * SelAll = new QPushButton("Check all", DeltaBox);
    auto * SelNone= new QPushButton("Uncheck all", DeltaBox);
    SelRow->addWidget(SelAll); SelRow->addWidget(SelNone); SelRow->addStretch();
    DeltaLay->addLayout(SelRow);
    Root->addWidget(DeltaBox, 1);
    QObject::connect(SelAll,  &QPushButton::clicked, this, [this]{ for (int i=0;i<DeltaList->count();++i) DeltaList->item(i)->setCheckState(Qt::Checked); });
    QObject::connect(SelNone, &QPushButton::clicked, this, [this]{ for (int i=0;i<DeltaList->count();++i) DeltaList->item(i)->setCheckState(Qt::Unchecked); });

    // ── Capture → node ──
    auto * CapBox = new QGroupBox("Capture into the package", this);
    auto * CapForm = new QFormLayout(CapBox);
    TargetCombo = new QComboBox(CapBox);
    CapForm->addRow("Target node", TargetCombo);
    DestNameEdit = new QLineEdit(CapBox);
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

    // ── End ──
    auto * EndRow = new QHBoxLayout();
    EndRow->addStretch();
    EndBtn = new QPushButton("End session (unmount + wipe)", this);
    EndRow->addWidget(EndBtn);
    Root->addLayout(EndRow);

    QObject::connect(RunExeBtn,  &QPushButton::clicked, this, &AuthoringSessionWindow::onRunExe);
    QObject::connect(BrowseBtn,  &QPushButton::clicked, this, [this]{ setBusy(true, "explorer"); Session->RunExe("explorer.exe"); setBusy(false); refreshDelta(); });
    QObject::connect(RegBtn,     &QPushButton::clicked, this, [this]{ setBusy(true, "regedit"); Session->RunExe("regedit.exe"); setBusy(false); refreshDelta(); });
    QObject::connect(RefreshBtn, &QPushButton::clicked, this, &AuthoringSessionWindow::refreshDelta);
    QObject::connect(CaptureFilesBtn, &QPushButton::clicked, this, &AuthoringSessionWindow::onCaptureFiles);
    QObject::connect(CaptureRegBtn,   &QPushButton::clicked, this, &AuthoringSessionWindow::onCaptureRegistry);
    QObject::connect(EndBtn, &QPushButton::clicked, this, [this]{ close(); });

    setBusy(true, "building runtime");
    // Paint the window first, then build the runtime (blocking, like the editor's existing authoring runs).
    QTimer::singleShot(0, this, &AuthoringSessionWindow::initialStart);
}

AuthoringSessionWindow::~AuthoringSessionWindow() = default;

void AuthoringSessionWindow::closeEvent(QCloseEvent * e)
{
    if (Session && Session->Live()) { setBusy(true, "ending session"); Session->End(); }
    QDialog::closeEvent(e);
}

void AuthoringSessionWindow::setBusy(bool On, const QString & What)
{
    if (On) { QApplication::setOverrideCursor(Qt::WaitCursor); if (!What.isEmpty()) StatusLabel->setText(What + "…"); }
    else    { QApplication::restoreOverrideCursor(); StatusLabel->setText("Session live."); }
    for (QPushButton * B : {RunExeBtn, BrowseBtn, RegBtn, RefreshBtn, CaptureFilesBtn, CaptureRegBtn})
        if (B) B->setEnabled(!On);
    QApplication::processEvents();
}

void AuthoringSessionWindow::initialStart()
{
    populateRunnerCombo();
    startSession({});   // auto-resolve the runner for the first build
}

void AuthoringSessionWindow::populateRunnerCombo()
{
    // Runners that can run this node: those serving the node's platform on this machine. (Direct runners; daisy-chained
    // multi-hop runners are auto-resolved when none is pinned.)
    const NodeIndex Idx = Model->BuildExecIndex();
    const Node * N = Idx.Find(TargetNodeId);
    const std::string Plat = N ? N->HostPlatform : std::string();
    const std::string Machine = ManifestModel::MachinePlatform();
    RunnerCombo->clear();
    for (const auto & [Id, R] : Idx.Nodes)
    {
        if (!R.IsRunner() || R.HostPlatform != Machine) continue;
        for (const std::string & G : R.GuestPlatform)
            if (G == Plat) { RunnerCombo->addItem(QString::fromStdString(Id)); break; }
    }
}

void AuthoringSessionWindow::startSession(const std::vector<std::string> & RunnerChainIds)
{
    setBusy(true, RunnerChainIds.empty() ? "building runtime" : "switching runner");
    if (Session) Session->End();
    Session = std::make_unique<AuthoringSession>(*Model->globalConfig(), QDir(Model->packagePath()));
    const bool Ok = Session->Begin(Model->BuildExecIndex(), TargetNodeId, {}, RunnerChainIds);
    QApplication::restoreOverrideCursor();
    if (!Ok)
    {
        QMessageBox::critical(this, "Authoring session",
            "Couldn't build the runtime for '" + QString::fromStdString(TargetNodeId) +
            "'.\n\nThe node needs a PLATFORM.HOST that a runner serves (or pick a runner). Check the log.");
        close();
        return;
    }

    // One-time: target-node picker (default: the node we started on) + capture dest name.
    if (!DefaultsDone)
    {
        TargetCombo->clear();
        for (const std::string & Id : Model->bundleNodeIds()) TargetCombo->addItem(QString::fromStdString(Id));
        TargetCombo->setCurrentText(QString::fromStdString(TargetNodeId));
        DestNameEdit->setText(QString::fromStdString(TargetNodeId + "_files"));
        DefaultsDone = true;
    }
    // Reflect the resolved runner (no rebuild loop — textActivated ignores setCurrentText).
    RunnerCombo->setCurrentText(QString::fromStdString(Session->RunnerId()));

    // Runner-dependent: the content root (and so the strip-prefix default) + wine gating.
    const bool Wine = Session->PrefixGenerate();
    StripEdit->setText(QString::fromStdString(Session->ContentRoot()));
    TargetEdit->setText("");
    RunExeBtn->setVisible(Wine); BrowseBtn->setVisible(Wine); RegBtn->setVisible(Wine);
    CaptureRegBtn->setVisible(Wine);

    const QString CR = QString::fromStdString(Session->ContentRoot());
    InfoLabel->setText(
        "Live runtime: " + QString::fromStdString(Session->RuntimePath().string()) +
        (Wine ? "\nContent root: " + CR + "  — install the game into this location (the wine C: drive) so the capture "
                "re-mounts exactly where it installed."
              : "\nContent root: " + CR));
    setBusy(false);
    refreshDelta();
}

void AuthoringSessionWindow::refreshDelta()
{
    if (!Session || !Session->Live()) return;
    DeltaList->clear();
    for (const std::string & Rel : AuthoringSession::EnumerateDelta(Session->WriteLayerPath()))
    {
        auto * It = new QListWidgetItem(QString::fromStdString(Rel), DeltaList);
        It->setFlags(It->flags() | Qt::ItemIsUserCheckable);
        It->setCheckState(Qt::Unchecked);
    }
    StatusLabel->setText("Session live — " + QString::number(DeltaList->count()) + " changed file(s).");
}

void AuthoringSessionWindow::onRunExe()
{
    if (!Session || !Session->Live()) return;
    const QString Exe = QFileDialog::getOpenFileName(this, "Select an executable to run in the prefix (installer, …)");
    if (Exe.isEmpty()) return;
    setBusy(true, "running");
    const bool Ok = Session->RunExe(Exe.toStdString());
    setBusy(false);
    if (!Ok) QMessageBox::warning(this, "Run EXE", "The process exited with an error. Check the log.");
    refreshDelta();
}

void AuthoringSessionWindow::onCaptureFiles()
{
    if (!Session || !Session->Live()) return;
    std::vector<std::string> Rels;
    for (int i = 0; i < DeltaList->count(); ++i)
        if (DeltaList->item(i)->checkState() == Qt::Checked) Rels.push_back(DeltaList->item(i)->text().toStdString());
    if (Rels.empty()) { QMessageBox::information(this, "Capture files", "Check at least one changed file to capture."); return; }

    const QString TargetNode = TargetCombo->currentText();
    const std::string DestName = DestNameEdit->text().trimmed().toStdString();
    if (DestName.empty()) { QMessageBox::warning(this, "Capture files", "Give the captured directory a name."); return; }
    const fs::path DestDir = fs::path(Model->packagePath().toStdString()) / DestName;

    setBusy(true, "capturing files");
    const int N = AuthoringSession::CopySelection(Session->WriteLayerPath(), Rels, DestDir, StripEdit->text().toStdString());
    setBusy(false);
    if (N == 0) { QMessageBox::warning(this, "Capture files", "Nothing was copied (check the strip prefix)."); return; }

    Model->appendLayerToNode(TargetNode.toStdString(),
                             AuthoringSession::MakeDirLayer(DestName, TargetEdit->text().trimmed().toStdString()));
    QMessageBox::information(this, "Capture files",
        QString("Captured %1 file(s) into '%2' and added a VFSDirLayer to '%3'.").arg(N).arg(QString::fromStdString(DestName)).arg(TargetNode));
}

void AuthoringSessionWindow::onCaptureRegistry()
{
    if (!Session || !Session->Live()) return;
    setBusy(true, "diffing registry");
    nlohmann::ordered_json Delta = Session->CaptureRegistryDelta();
    setBusy(false);
    if (Delta.empty()) { QMessageBox::information(this, "Capture registry", "No registry changes since the session started."); return; }
    const QString TargetNode = TargetCombo->currentText();
    Model->mergeRegEditsIntoNode(TargetNode.toStdString(), Delta);
    QMessageBox::information(this, "Capture registry",
        QString("Captured %1 RegEdit(s) into '%2'.").arg((int)Delta.size()).arg(TargetNode));
}
