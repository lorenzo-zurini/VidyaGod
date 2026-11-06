#include "packageeditor.h"
#include "packageeditormodel.h"
#include "pkgcanvaspanel.h"
#include "pkgactions.h"
#include "jsonraweditor.h"
#include "validationpanel.h"
#include "packagecatalog.h"
#include "pkginsight.h"
#include "pkggraph.h"
#include "commonutils.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QRegularExpressionValidator>
#include <QPushButton>
#include <QLineEdit>
#include <QFormLayout>
#include <QFileInfo>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QGuiApplication>
#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QInputDialog>
#include <QMetaMethod>
#include <QScreen>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <filesystem>

using json = nlohmann::ordered_json;

PackageEditor * PackageEditor::Live = nullptr;

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent, const QString & PreselectedPath)
    : QDialog(parent)
{
    Model = new PackageEditorModel(GlobalConfigJSON, this, this);
    if (!Live) Live = this;
    else LogErr("PackageEditor", "A second package editor was constructed; its canvas cannot render. Open it through PackageEditor::OpenFor().");
    setWindowFlags(Qt::Window);
    resize(QGuiApplication::primaryScreen()->availableGeometry().size() * 0.9);
    setWindowState(Qt::WindowMaximized);

    QVBoxLayout * Main = new QVBoxLayout(this);
    Main->setSpacing(0);
    Main->setContentsMargins(0, 0, 0, 0);

    // ---- toolbar ----
    QToolBar * Bar = new QToolBar(this);
    Bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    Bar->setIconSize(QSize(16, 16));
    QAction * NewAct = Bar->addAction("New package...");
    NewAct->setShortcut(QKeySequence::New);
    NewAct->setToolTip("Start a package for a new game (Ctrl+N): a folder in your library and its tile, ready for its files");
    QAction * OpenAct = Bar->addAction("Open package...");
    OpenAct->setShortcut(QKeySequence::Open);
    OpenAct->setToolTip("Open another package folder (Ctrl+O)");
    SaveAct = Bar->addAction("Save");
    SaveAct->setShortcut(QKeySequence::Save);
    SaveAct->setToolTip("Save the package (Ctrl+S). Every edited node is renamed to the CID of its new content,\n"
                        "and every node that contains it - here or in other packages - follows.");
    Bar->addSeparator();
    UndoAct = Bar->addAction("Undo");
    UndoAct->setToolTip("Undo the last edit (Ctrl+Z on the canvas)");
    RedoAct = Bar->addAction("Redo");
    RedoAct->setToolTip("Redo (Ctrl+Shift+Z or Ctrl+Y on the canvas)");
    Bar->addSeparator();
    QAction * ValidateAct = Bar->addAction("Validate");
    ValidateAct->setToolTip("Check the package as it is now (unsaved edits included): references, layer paths,\n"
                            "runner resolution, case collisions. Problems appear on the nodes and in the list.");
    QAction * FixCaseAct = Bar->addAction("Fix case collisions");
    FixCaseAct->setToolTip("Rename zip entries that differ from a lower layer's only by case to that layer's case,\n"
                           "so patches and add-ons override cleanly (rewrites this package's zips; saves first).");
    QAction * IdAct = Bar->addAction("Game ID...");
    IdAct->setToolTip("Change the game's ID: its folder, its tiles, the tiles nested under it and its saves follow.\n"
                      "The ID is its TheGamesDB ID - or, for a game TheGamesDB has no entry for, its release date as\n"
                      "YYYYMMDD followed by a serial (0001).");
    connect(IdAct, &QAction::triggered, this, [this] {
        if (bundleDir().isEmpty()) return;
        bool Ok = false;
        const QString New = QInputDialog::getText(this, "Game ID", "The game's ID (TheGamesDB ID, or release date YYYYMMDD + 0001):",
                                                  QLineEdit::Normal, QString::fromStdString(Model->gameId()), &Ok).trimmed();
        if (!Ok || New.toStdString() == Model->gameId()) return;
        QString Err;
        if (!Model->changeGameId(New.toStdString(), &Err)) QMessageBox::warning(this, "Game ID", Err);
        else updateTitle();
    });
    QAction * SeedAct = Bar->addAction("Seed && share");
    SeedAct->setToolTip("Put this package's content (zips, files, covers) on IPFS, record each file's CID in its node,\n"
                        "and save - so friends can fetch it.");
    QWidget * Spacer = new QWidget(this);
    Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    Bar->addWidget(Spacer);
    StatusText = new QLabel(this);
    StatusText->setContentsMargins(8, 0, 12, 0);
    Bar->addWidget(StatusText);
    Main->addWidget(Bar);

    // ---- canvas | JSON + validation ----
    Canvas = new PkgCanvasPanel(&Model->doc(), this);
    PkgCanvas * C = Canvas->canvas();
    C->setExternalLookup([this](const std::string & Cid) {
        const auto E = Model->externalInfo(Cid);
        return PkgCanvas::External{E.Label, E.Package, E.PackageDir, E.Cover};
    });
    C->setOffers([this] {
        std::vector<PkgCanvas::Offer> Out;
        for (const auto & O : Model->offers()) Out.push_back({O.Cid, O.Label, O.Package});
        return Out;
    });
    C->setPlatforms([this] { return Model->KnownPlatforms(); });
    Actions = new PkgActions(Model, C, this, this);
    connect(Actions, &PkgActions::status, this, [this](const QString & T) { status(T, 8000); });
    //Files from the file manager: onto a node they are added to it; onto the empty canvas they get a node of their own.
    connect(Canvas, &PkgCanvasPanel::filesDropped, this, [this](QString H, const QStringList & Paths, float Wx, float Wy) {
        if (bundleDir().isEmpty()) { status("Open or create a package first"); return; }
        if (H.isEmpty())
        {
            const int I = Canvas->canvas()->addNode("", Wx, Wy);
            Model->doc().Commit();
            Model->noteEdited();
            H = QString::fromStdString(Model->doc().Handle(I));
            Canvas->canvas()->select(H.toStdString(), false);
        }
        Actions->dropFiles(H, Paths);
        Canvas->requestFrame();
    });

    QWidget * Right = new QWidget(this);
    QVBoxLayout * RL = new QVBoxLayout(Right);
    RL->setContentsMargins(0, 0, 0, 0);
    QSplitter * RightSplit = new QSplitter(Qt::Vertical, Right);
    //The selected node: its JSON, and what it amounts to - the files the game sees, its fold, its card.
    QTabWidget * NodeTabs = new QTabWidget(RightSplit);
    Json = new JsonRawEditor(Model, NodeTabs);
    NodeTabs->addTab(Json, "JSON");
    NodeTabs->setUsesScrollButtons(false);                // every tab in view: none hidden behind arrows
    Insight = new PkgInsight(Model, NodeTabs);
    History = new QListWidget(NodeTabs);
    History->setToolTip("Every change since the package was opened. Click one to go back to just after it; the ones after it stay, greyed, for redo.");
    NodeTabs->addTab(History, "History");
    NodeTabs->setTabToolTip(0, "The selected node as JSON - edit it directly");
    NodeTabs->setTabToolTip(1, "Every file the selected node gives the game when it runs");
    NodeTabs->setTabToolTip(2, "The selected node's layers in the order they are applied - later wins");
    NodeTabs->setTabToolTip(3, "How the selected version shows in the Library");
    NodeTabs->setTabToolTip(4, "Every change since the package was opened - click one to go back to it");
    ValidationPanel * Val = new ValidationPanel(Model, RightSplit);
    RightSplit->addWidget(NodeTabs);
    RightSplit->addWidget(Val);
    RightSplit->setStretchFactor(0, 1);
    RL->addWidget(RightSplit);
    QSplitter * Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Canvas);
    Split->addWidget(Right);
    Split->setStretchFactor(0, 1);
    Split->setStretchFactor(1, 0);
    Split->setSizes({1300, 420});
    Split->setCollapsible(0, false);
    Main->addWidget(Split, 1);

    // ---- wiring ----
    auto UpdateUndo = [this] {
        UndoAct->setEnabled(Model->doc().CanUndo());
        RedoAct->setEnabled(Model->doc().CanRedo());
    };
    connect(NewAct, &QAction::triggered, this, [this] { newPackage(); });
    connect(OpenAct, &QAction::triggered, this, [this] { openPackagePicker(); });
    //The empty canvas's buttons; queued, since they are pressed in the middle of drawing a frame.
    connect(C, &PkgCanvas::newPackageRequested, this, [this] { newPackage(); }, Qt::QueuedConnection);
    connect(C, &PkgCanvas::openPackagePickerRequested, this, [this] { openPackagePicker(); }, Qt::QueuedConnection);
    connect(SaveAct, &QAction::triggered, this, [this] { save(); });
    connect(UndoAct, &QAction::triggered, this, [this, UpdateUndo] { Canvas->canvas()->undo(); Canvas->requestFrame(); UpdateUndo(); refreshHistory(); });
    connect(RedoAct, &QAction::triggered, this, [this, UpdateUndo] { Canvas->canvas()->redo(); Canvas->requestFrame(); UpdateUndo(); refreshHistory(); });
    connect(ValidateAct, &QAction::triggered, this, [this] { validate(); });
    connect(FixCaseAct, &QAction::triggered, this, [this] { fixCase(); });
    connect(SeedAct, &QAction::triggered, this, [this] { seedAndShare(); });

    connect(C, &PkgCanvas::selectionChanged, Json, &JsonRawEditor::showNode);
    connect(C, &PkgCanvas::selectionChanged, Insight, &PkgInsight::showNode);
    connect(C, &PkgCanvas::documentEdited, this, [this, UpdateUndo] { Model->noteEdited(); Json->refresh(); Insight->refresh(); UpdateUndo(); refreshHistory(); });
    connect(C, &PkgCanvas::saveRequested, this, [this] { save(); });
    connect(C, &PkgCanvas::statusMessage, this, [this](const QString & T) { status(T); });
    connect(C, &PkgCanvas::openPackageRequested, this, [this](const QString & D) { openPackage(D); });
    connect(C, &PkgCanvas::nodeAction, Actions, &PkgActions::perform, Qt::QueuedConnection);
    connect(C, &PkgCanvas::cancelRequested, Actions, &PkgActions::cancel);
    connect(Val, &ValidationPanel::fixClicked, this, [this](const QString & A) {
        if (A == "fix_case") fixCase();
        else if (A == "delete_unused")
        {
            const QStringList F = Model->unusedFiles();
            if (F.isEmpty()) return;
            if (QMessageBox::question(this, "Unused files", "No node of this package uses:\n\n" + F.join("\n")
                                      + QString("\n\nDelete %1 from the package folder?").arg(F.size() == 1 ? "it" : "them")) != QMessageBox::Yes) return;
            status(QString("Deleted %1 unused file(s)").arg(Model->deleteUnusedFiles()));
            validate();
        }
    });
    connect(Val, &ValidationPanel::nodeClicked, this, [this](const QString & H) {
        Canvas->canvas()->select(H.toStdString(), true);
        Canvas->setFocus();
        Canvas->requestFrame();
    });
    connect(Model, &PackageEditorModel::documentReloaded, this, [this, UpdateUndo] {
        Canvas->canvas()->setPackageOpen(!bundleDir().isEmpty());
        Canvas->canvas()->documentReset();
        Canvas->canvas()->setIssues({});
        Actions->refreshHints();
        updateTitle();
        UpdateUndo();
        Canvas->requestFrame();
    });
    connect(Model, &PackageEditorModel::nodeContentChanged, this, [this, UpdateUndo] { Canvas->requestFrame(); Insight->refresh(); UpdateUndo(); refreshHistory(); });
    connect(Model, &PackageEditorModel::handlesRenamed, this, [this] { Canvas->canvas()->applyRenames(Model->lastRenames()); });
    connect(Model, &PackageEditorModel::validationChanged, this, [this] {
        Canvas->canvas()->setIssues(Model->validated() ? Model->issuesByHandle() : std::map<std::string, std::vector<std::string>>());
        Canvas->requestFrame();
        if (!Model->validated()) return;
        //A step the author is about to take (a layer waiting for its files) is not counted as a problem.
        int E = 0, Waiting = 0;
        for (const std::string & M : Model->validationErrors()) (PkgGraph::FixFor(M).Waiting ? Waiting : E)++;
        const int W = (int)Model->validationWarnings().size();
        const QString Said = E || W ? QString("%1 error(s), %2 warning(s) - see the nodes marked !").arg(E).arg(W)
                           : Waiting ? QString("Waiting for the game's files - see the node") : QString("No problems found");
        if (Said != LastVerdict || ValidationAsked) status(Said);   // a live re-check that found the same says nothing
        LastVerdict = Said;
        ValidationAsked = false;
    });
    connect(Model, &PackageEditorModel::dirtyChanged, this, [this, UpdateUndo] { updateTitle(); UpdateUndo(); });
    connect(Model, &PackageEditorModel::savedToDisk, this, &PackageEditor::packageSaved);
    connect(Model, &PackageEditorModel::savedToDisk, this, [this] { Model->dropDraft(); refreshHistory(); });
    //The history: click a step to stand just after it (undoing the later ones, or redoing up to it).
    connect(History, &QListWidget::itemClicked, this, [this, UpdateUndo](QListWidgetItem * It) {
        const int Want = It->data(Qt::UserRole).toInt();   // steps applied after the click
        int Now = (int)Model->doc().Steps().Done.size();
        while (Now > Want && Model->doc().Undo()) --Now;
        while (Now < Want && Model->doc().Redo()) ++Now;
        Model->noteEdited();
        Canvas->canvas()->documentReset();
        Json->refresh();
        Insight->refresh();
        Canvas->requestFrame();
        UpdateUndo();
        refreshHistory();
    });
    //Unsaved work is written aside now and then, so a crash loses at most a few seconds of it.
    QTimer * DraftTimer = new QTimer(this);
    connect(DraftTimer, &QTimer::timeout, this, [this] { if (Model->isDirty()) Model->writeDraft(); });
    DraftTimer->start(10000);

    Model->initPackage(PreselectedPath, this);
    Canvas->canvas()->setPackageOpen(!bundleDir().isEmpty());
    updateTitle();
    UpdateUndo();
    QTimer::singleShot(0, this, [this] { offerDraft(); });
    //The first time the editor opens on this machine, a short tour; never again once finished or skipped.
    connect(C, &PkgCanvas::tourFinished, this, [this] { Model->markTourSeen(); }, Qt::QueuedConnection);
    if (!Model->tourSeen()) { C->startTour(); Canvas->requestFrame(); }
    //Checked once on open, so problems are on the nodes from the start (it reads the library index, which the canvas
    //needs for its external chips anyway).
    QTimer::singleShot(0, this, [this] { validate(); });
}

PackageEditor::~PackageEditor()
{
    if (Live == this) Live = nullptr;
}

QString PackageEditor::bundleDir() const { return Model ? Model->packagePath() : QString(); }

PackageEditor * PackageEditor::OpenFor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent,
                                       const QString & PackagePath, bool * Created)
{
    if (Created) *Created = false;
    if (Live)
    {
        //One canvas at a time: an open editor switches to the asked-for package (asking about unsaved edits first).
        if (!PackagePath.isEmpty() && QDir(Live->bundleDir()) != QDir(PackagePath)) Live->openPackage(PackagePath);
        Live->show();
        Live->raise();
        Live->activateWindow();
        return Live;
    }
    auto * Ed = new PackageEditor(GlobalConfigJSON, parent, PackagePath);
    Ed->setAttribute(Qt::WA_DeleteOnClose);
    Live = Ed;
    if (Created) *Created = true;
    if (OnCreated) OnCreated(Ed);
    Ed->show();
    return Ed;
}

void PackageEditor::updateTitle()
{
    const QString Dir = bundleDir();
    const QString Name = Dir.isEmpty() ? QString("no package") : QDir(Dir).dirName();
    setWindowTitle((Model->isDirty() ? QString("● ") : QString()) + Name + " - Package Editor");
    SaveAct->setEnabled(!Dir.isEmpty());
    SaveAct->setText(Model->isDirty() ? "Save ●" : "Save");
}

void PackageEditor::status(const QString & Text, int Ms)
{
    StatusText->setText(Text);
    QTimer::singleShot(Ms, StatusText, [L = StatusText, Text] { if (L->text() == Text) L->clear(); });
}

bool PackageEditor::save()
{
    if (bundleDir().isEmpty()) return false;
    if (Model->saveEmptiesPackage()
        && QMessageBox::question(this, "Save", "Every node of this package has been deleted. Saving now leaves the package with no "
                                 "nodes (its files stay in the folder).\n\nSave anyway? (Undo brings the nodes back.)",
                                 QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
        return false;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString Err;
    const bool Ok = Model->Save(&Err);
    QGuiApplication::restoreOverrideCursor();
    if (!Ok) { QMessageBox::warning(this, "Save", "The package was not saved:\n\n" + Err); return false; }
    status("Saved");
    if (!Model->saveNotes().empty())
    {
        QString Notes;
        for (const std::string & N : Model->saveNotes()) Notes += "\u2022 " + QString::fromStdString(N) + "\n\n";
        QMessageBox::information(this, "Saved", "The package was saved. Note:\n\n" + Notes.trimmed());
    }
    Canvas->requestFrame();
    validate();
    return true;
}

bool PackageEditor::maybeSave(const QString & Why)
{
    if (!Model->isDirty()) return true;
    const auto B = QMessageBox::question(this, "Unsaved changes",
        "This package has unsaved changes. Save them before " + Why + "?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (B == QMessageBox::Cancel) return false;
    if (B == QMessageBox::Save) return save();
    Model->dropDraft();                                   // discarded on purpose: not offered back next time
    return true;
}

void PackageEditor::refreshHistory()
{
    if (!History) return;
    const auto H = Model->doc().Steps();
    History->clear();
    auto *Start = new QListWidgetItem("(as opened)", History);
    Start->setData(Qt::UserRole, 0);
    for (int I = 0; I < (int)H.Done.size(); ++I)
    {
        auto *It = new QListWidgetItem(QString::fromStdString(H.Done[(size_t)I]), History);
        It->setData(Qt::UserRole, I + 1);
    }
    History->setCurrentRow((int)H.Done.size());
    if (H.Done.empty() && H.Undone.empty())   // an empty list read as broken: say what will be here
    {
        auto *Hint = new QListWidgetItem("No changes yet — each edit is listed here; click one to go back to just after it.", History);
        Hint->setFlags(Qt::NoItemFlags);
        Hint->setForeground(QColor(140, 140, 150));
    }
    for (int I = 0; I < (int)H.Undone.size(); ++I)
    {
        auto *It = new QListWidgetItem(QString::fromStdString(H.Undone[(size_t)I]) + "   (undone)", History);
        It->setData(Qt::UserRole, (int)H.Done.size() + I + 1);
        It->setForeground(QColor(140, 140, 150));
    }
}

void PackageEditor::offerDraft()
{
    const qint64 T = Model->draftTime();
    if (T <= 0) return;
    const auto B = QMessageBox::question(this, "Unsaved changes found",
        "This package has changes that were never saved, from " + QDateTime::fromMSecsSinceEpoch(T).toString("d MMM, HH:mm")
        + " (the editor closed before they were).\n\nRestore them?", QMessageBox::Yes | QMessageBox::Discard, QMessageBox::Yes);
    if (B == QMessageBox::Yes)
    {
        QString Err;
        if (!Model->restoreDraft(&Err)) QMessageBox::warning(this, "Unsaved changes found", Err);
        else status("Restored the unsaved changes - save to keep them");
    }
    else Model->dropDraft();
    refreshHistory();
}

void PackageEditor::closeEvent(QCloseEvent * E)
{
    if (!maybeSave("closing")) { E->ignore(); return; }
    E->accept();
}

void PackageEditor::reject()
{
    if (maybeSave("closing")) QDialog::reject();
}

void PackageEditor::openPackage(const QString & Dir)
{
    if (QDir(Dir) == QDir(bundleDir())) return;
    if (!maybeSave("opening another package")) return;
    Model->initPackage(Dir, this);
    QTimer::singleShot(0, this, [this] { offerDraft(); refreshHistory(); });
    QTimer::singleShot(0, this, [this] { validate(); });
}

void PackageEditor::openPackagePicker()
{
    //Start where packages live: the open package's library, else this machine's LIBRARY.
    auto Browse = [this] {
        QString Start = bundleDir();
        if (Start.isEmpty() && Model->globalConfig()) Start = QString::fromStdString(PackageCatalog::LibraryRootDir(*Model->globalConfig()));
        else if (!Start.isEmpty()) Start = QFileInfo(Start).absolutePath();
        return QFileDialog::getExistingDirectory(this, "Open a package folder", Start);
    };
    nlohmann::ordered_json * Cfg = Model->globalConfig();
    if (!Cfg) { const QString D = Browse(); if (!D.isEmpty()) openPackage(D); return; }
    //The packages themselves, by title and searchable: hundreds of "[uid][version] Title" folders are not browsed.
    QDialog Dlg(this);
    Dlg.setWindowTitle("Open package");
    Dlg.resize(620, 560);
    QVBoxLayout * L = new QVBoxLayout(&Dlg);
    QLineEdit * Filter = new QLineEdit(&Dlg);
    Filter->setPlaceholderText("Search your packages by title or library");
    Filter->setClearButtonEnabled(true);
    QListWidget * List = new QListWidget(&Dlg);
    for (const auto & P : PackageCatalog::AuthoredPackages(*Cfg))
    {
        auto * It = new QListWidgetItem(QString::fromStdString(P.Title) + "    "
                                        + (P.Library.empty() ? QString("(outside the library)") : QString::fromStdString(P.Library)), List);
        It->setData(Qt::UserRole, QString::fromStdString(P.Dir.string()));
        It->setToolTip(QString::fromStdString(P.Dir.string()));
    }
    QDialogButtonBox * Box = new QDialogButtonBox(QDialogButtonBox::Cancel, &Dlg);
    QPushButton * OpenBtn = Box->addButton("Open", QDialogButtonBox::AcceptRole);
    QPushButton * Other = Box->addButton("Another folder...", QDialogButtonBox::ActionRole);
    L->addWidget(Filter);
    L->addWidget(List);
    L->addWidget(Box);
    auto FirstShown = [List] {
        for (int I = 0; I < List->count(); ++I) if (!List->item(I)->isHidden()) { List->setCurrentRow(I); return; }
        List->setCurrentRow(-1);
    };
    connect(Filter, &QLineEdit::textChanged, &Dlg, [List, FirstShown](const QString & T) {
        for (int I = 0; I < List->count(); ++I) List->item(I)->setHidden(!List->item(I)->text().contains(T, Qt::CaseInsensitive));
        FirstShown();
    });
    connect(List, &QListWidget::currentRowChanged, &Dlg, [OpenBtn, List](int R) { OpenBtn->setEnabled(R >= 0 && !List->item(R)->isHidden()); });
    connect(List, &QListWidget::itemActivated, &Dlg, &QDialog::accept);
    connect(Filter, &QLineEdit::returnPressed, &Dlg, [&Dlg, List] { if (List->currentItem()) Dlg.accept(); });
    connect(Box, &QDialogButtonBox::accepted, &Dlg, &QDialog::accept);
    connect(Box, &QDialogButtonBox::rejected, &Dlg, &QDialog::reject);
    QString Picked;
    connect(Other, &QPushButton::clicked, &Dlg, [&] { Picked = Browse(); if (!Picked.isEmpty()) Dlg.done(QDialog::Accepted); });
    FirstShown();
    OpenBtn->setEnabled(List->currentItem() != nullptr);
    Filter->setFocus();
    if (Dlg.exec() != QDialog::Accepted) return;
    if (Picked.isEmpty() && List->currentItem()) Picked = List->currentItem()->data(Qt::UserRole).toString();
    if (!Picked.isEmpty()) openPackage(Picked);
}

void PackageEditor::newPackage()
{
    nlohmann::ordered_json * Cfg = Model->globalConfig();
    if (!Cfg || !maybeSave("starting a new package")) return;
    QDialog Dlg(this);
    Dlg.setWindowTitle("New package");
    QFormLayout * Form = new QFormLayout(&Dlg);
    QLabel * Intro = new QLabel("A package is one game's folder in your library: its files, how it runs, and its tile on the shelf.", &Dlg);
    Intro->setWordWrap(true);
    Form->addRow(Intro);
    QLineEdit * Title = new QLineEdit(&Dlg);
    Title->setPlaceholderText("e.g. Black & White 2");
    Form->addRow("Game title", Title);
    QLineEdit * Uid = new QLineEdit(&Dlg);
    Uid->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{1,16}"), Uid));
    Uid->setPlaceholderText("the number in thegamesdb.net/game.php?id=...");
    Form->addRow("TheGamesDB ID", Uid);
    QLabel * Find = new QLabel("<a href=\"https://thegamesdb.net/search.php\">Find it on TheGamesDB</a> - the game's ID "
                               "everywhere: its folder, its tile, its saves.", &Dlg);
    Find->setOpenExternalLinks(true);
    Find->setWordWrap(true);
    Form->addRow(QString(), Find);
    QComboBox * Lib = new QComboBox(&Dlg);
    for (const std::string & L : PackageCatalog::LibraryNames(*Cfg)) Lib->addItem(QString::fromStdString(L));
    Lib->setCurrentText("VidyaGod");
    Form->addRow("Library", Lib);
    //What it is decides the first node: what runs it, and what is left to find out from its files.
    QComboBox * Kind = new QComboBox(&Dlg);
    Kind->addItem("A Windows game", (int)PkgGraph::Recipe::Windows);
    Kind->addItem("A DOS game", (int)PkgGraph::Recipe::Dos);
    Kind->addItem("A console / emulated game", (int)PkgGraph::Recipe::Console);
    Kind->addItem("A Linux-native game", (int)PkgGraph::Recipe::Linux);
    Kind->addItem("A library games use (a runtime's DLLs, fonts...)", (int)PkgGraph::Recipe::Library);
    Form->addRow("What is it", Kind);
    QComboBox * Plat = new QComboBox(&Dlg);
    for (const std::string & P : Model->KnownPlatforms())
        if (P != "win32" && P != "win64" && P != "linux64" && P != "dos" && P != "custom") Plat->addItem(QString::fromStdString(P));
    Plat->setEditable(true);                               // a platform no runner here plays yet can still be named
    Plat->setToolTip("The console or system it runs on - the runners installed here are listed");
    Form->addRow("Platform", Plat);
    //A library has no game ID (nothing of it is on the shelf): it is named, and lives in the libraries' library.
    auto IsLibrary = [Kind] { return Kind->currentData().toInt() == (int)PkgGraph::Recipe::Library; };
    auto KindChanged = [Form, Plat, Kind, Uid, Find, Lib, Title, IsLibrary] {
        Form->setRowVisible(Plat, Kind->currentData().toInt() == (int)PkgGraph::Recipe::Console);
        Form->setRowVisible(Uid, !IsLibrary());
        Form->setRowVisible(Find, !IsLibrary());
        if (auto * L = qobject_cast<QLabel *>(Form->labelForField(Title))) L->setText(IsLibrary() ? "Name" : "Game title");
        Title->setPlaceholderText(IsLibrary() ? "e.g. mfc42" : "e.g. Black & White 2");
        if (IsLibrary() && Lib->findText("VidyaGodLibraries") >= 0) Lib->setCurrentText("VidyaGodLibraries");
    };
    connect(Kind, &QComboBox::currentIndexChanged, &Dlg, KindChanged);
    KindChanged();
    QDialogButtonBox * Box = new QDialogButtonBox(QDialogButtonBox::Cancel, &Dlg);
    QPushButton * Create = Box->addButton("Create", QDialogButtonBox::AcceptRole);   //its own button: named "Create", not "OK"
    Create->setDefault(true);
    Form->addRow(Box);
    auto Ready = [&] { Create->setEnabled(!Title->text().trimmed().isEmpty() && (IsLibrary() || !Uid->text().isEmpty())); };
    connect(Title, &QLineEdit::textChanged, &Dlg, Ready);
    connect(Uid, &QLineEdit::textChanged, &Dlg, Ready);
    connect(Kind, &QComboBox::currentIndexChanged, &Dlg, Ready);
    connect(Box, &QDialogButtonBox::rejected, &Dlg, &QDialog::reject);
    std::filesystem::path Dir;
    connect(Box, &QDialogButtonBox::accepted, &Dlg, [&] {
        std::string Err;
        Dir = IsLibrary() ? PackageCatalog::CreateLibraryFolder(*Cfg, Lib->currentText().toStdString(), Title->text().trimmed().toStdString(), Err)
                          : PackageCatalog::CreatePackageFolder(*Cfg, Lib->currentText().toStdString(), Uid->text().toStdString(),
                                  Title->text().trimmed().toStdString(), Err);
        if (Dir.empty()) { QMessageBox::warning(&Dlg, "New package", QString::fromStdString(Err)); return; }
        Dlg.accept();
    });
    Ready();
    if (Dlg.exec() != QDialog::Accepted) return;
    Model->initPackage(QString::fromStdString(Dir.string()), this);
    //Its first version, carrying the game's tile: a game is on the shelf as its versions (VARIANT), and another version
    //later contains this one (its files, its entry and its tile) and changes what differs.
    const std::string T = Title->text().trimmed().toStdString(), U = IsLibrary() ? std::string() : Uid->text().toStdString();
    json Tile = PkgGraph::RecipeNode((PkgGraph::Recipe)Kind->currentData().toInt(), T, U, Plat->currentText().trimmed().toStdString());
    PkgDoc::Document & D = Model->doc();
    D.Add(std::move(Tile));
    D.Commit();
    Canvas->canvas()->documentReset();
    Model->noteEdited();
    //Saved at once: Create makes a package, folder and tile, not a folder that stays empty if the editor closes.
    if (!save()) return;
    const std::string Saved = D.Count() ? D.Handle(0) : std::string();
    Canvas->canvas()->select(Saved, true);
    Json->showNode(QString::fromStdString(Saved));
    updateTitle();
    Canvas->requestFrame();
    status("Created " + QDir(QString::fromStdString(Dir.string())).dirName());   // the canvas says what comes next
}

void PackageEditor::validate()
{
    if (bundleDir().isEmpty()) return;
    status("Checking the package...");
    ValidationAsked = true;
    Model->validateNow();                                 // the result arrives as validationChanged
}

void PackageEditor::fixCase()
{
    if (bundleDir().isEmpty()) return;
    if (Model->isDirty() && !save()) return;
    std::vector<std::string> Log;
    const std::filesystem::path Scope = bundleDir().toStdString();
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const int Fixed = ManifestModel::FixCaseConflicts(Model->BuildExecIndex(), Log, &Scope);
    QGuiApplication::restoreOverrideCursor();
    validate();
    if (Fixed == 0) { QMessageBox::information(this, "Fix case collisions", "No cross-layer case collisions in this package."); return; }
    QString Report;
    for (const auto & L : Log) Report += QString::fromStdString(L) + "\n";
    QMessageBox::information(this, "Fix case collisions",
        QString("Rewrote %1 zip(s): entries that collided only by case now use the lower layer's case.\n\n%2").arg(Fixed).arg(Report));
}

void PackageEditor::seedAndShare()
{
    if (bundleDir().isEmpty()) return;
    const std::filesystem::path Pkg = bundleDir().toStdString();
    PackageCatalog::SeedReport R;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    PkgDoc::Document & D = Model->doc();
    for (int I = 0; I < D.Count(); ++I)
    {
        json N = D.Node(I);
        bool Changed = false;
        std::string Err;
        if (!PackageCatalog::SeedNodeContent(N, Pkg, R, Changed, &Err))
        {
            QGuiApplication::restoreOverrideCursor();
            QMessageBox::warning(this, "Seed & share", QString::fromStdString(Err));
            return;
        }
        if (Changed) D.Replace(I, std::move(N));
    }
    D.Commit();
    Model->notifyNodeChanged();
    QGuiApplication::restoreOverrideCursor();
    if (!save()) return;
    QString Summary = QString("%1 of %2 file(s) newly seeded, %3 cover(s).").arg(R.Seeded).arg(R.Walked).arg(R.Covers);
    if (!R.Unfetchable.empty()) Summary += QString("\n\n%1 layer(s) name a file that is not here - they cannot be fetched.").arg(R.Unfetchable.size());
    if (!R.Unshareable.empty()) Summary += QString("\n\n%1 DIR layer(s) are local only - convert them to zips to share them.").arg(R.Unshareable.size());
    nlohmann::ordered_json * Cfg = Model->globalConfig();
    const bool OwnLibrary = Cfg && PackageCatalog::IsPackageSourcePath(*Cfg, Pkg);
    const bool CanPublish = isSignalConnected(QMetaMethod::fromSignal(&PackageEditor::publishToLibraryRequested));
    if (OwnLibrary && CanPublish &&
        QMessageBox::question(this, "Seed & share", Summary + "\n\nPublish your library now, so friends see the update?",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes)
    {
        emit publishToLibraryRequested();
        status("Publishing in the background - the Sharing tab shows the result", 8000);
    }
    else QMessageBox::information(this, "Seed & share", Summary);
}
