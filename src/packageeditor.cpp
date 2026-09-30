#include "packageeditor.h"
#include "packageeditormodel.h"
#include "pkgcanvaspanel.h"
#include "pkgactions.h"
#include "jsonraweditor.h"
#include "validationpanel.h"
#include "packagecatalog.h"
#include "commonutils.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QMetaMethod>
#include <QScreen>
#include <QSplitter>
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
        return PkgCanvas::External{E.Label, E.Package, E.PackageDir};
    });
    C->setOffers([this] {
        std::vector<PkgCanvas::Offer> Out;
        for (const auto & O : Model->offers()) Out.push_back({O.Cid, O.Label, O.Package});
        return Out;
    });
    Actions = new PkgActions(Model, C, this, this);

    QWidget * Right = new QWidget(this);
    QVBoxLayout * RL = new QVBoxLayout(Right);
    RL->setContentsMargins(0, 0, 0, 0);
    QSplitter * RightSplit = new QSplitter(Qt::Vertical, Right);
    Json = new JsonRawEditor(Model, RightSplit);
    ValidationPanel * Val = new ValidationPanel(Model, RightSplit);
    RightSplit->addWidget(Json);
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
    connect(OpenAct, &QAction::triggered, this, [this] {
        const QString D = QFileDialog::getExistingDirectory(this, "Open a package folder", bundleDir());
        if (!D.isEmpty()) openPackage(D);
    });
    connect(SaveAct, &QAction::triggered, this, [this] { save(); });
    connect(UndoAct, &QAction::triggered, this, [this, UpdateUndo] { Canvas->canvas()->undo(); Canvas->requestFrame(); UpdateUndo(); });
    connect(RedoAct, &QAction::triggered, this, [this, UpdateUndo] { Canvas->canvas()->redo(); Canvas->requestFrame(); UpdateUndo(); });
    connect(ValidateAct, &QAction::triggered, this, [this] { validate(); });
    connect(FixCaseAct, &QAction::triggered, this, [this] { fixCase(); });
    connect(SeedAct, &QAction::triggered, this, [this] { seedAndShare(); });

    connect(C, &PkgCanvas::selectionChanged, Json, &JsonRawEditor::showNode);
    connect(C, &PkgCanvas::documentEdited, this, [this, UpdateUndo] { Model->noteEdited(); Json->refresh(); UpdateUndo(); });
    connect(C, &PkgCanvas::saveRequested, this, [this] { save(); });
    connect(C, &PkgCanvas::statusMessage, this, [this](const QString & T) { status(T); });
    connect(C, &PkgCanvas::openPackageRequested, this, [this](const QString & D) { openPackage(D); });
    connect(C, &PkgCanvas::nodeAction, Actions, &PkgActions::perform, Qt::QueuedConnection);
    connect(C, &PkgCanvas::cancelRequested, Actions, &PkgActions::cancel);
    connect(Val, &ValidationPanel::nodeClicked, this, [this](const QString & H) {
        Canvas->canvas()->select(H.toStdString(), true);
        Canvas->setFocus();
        Canvas->requestFrame();
    });
    connect(Model, &PackageEditorModel::documentReloaded, this, [this, UpdateUndo] {
        Canvas->canvas()->documentReset();
        Canvas->canvas()->setIssues({});
        Actions->refreshHints();
        updateTitle();
        UpdateUndo();
        Canvas->requestFrame();
    });
    connect(Model, &PackageEditorModel::nodeContentChanged, this, [this, UpdateUndo] { Canvas->requestFrame(); UpdateUndo(); });
    connect(Model, &PackageEditorModel::handlesRenamed, this, [this] { Canvas->canvas()->applyRenames(Model->lastRenames()); });
    connect(Model, &PackageEditorModel::validationChanged, this, [this] {
        Canvas->canvas()->setIssues(Model->validated() ? Model->issuesByHandle() : std::map<std::string, std::vector<std::string>>());
        Canvas->requestFrame();
        if (!Model->validated()) return;
        const int E = (int)Model->validationErrors().size(), W = (int)Model->validationWarnings().size();
        const QString Said = E || W ? QString("%1 error(s), %2 warning(s) - see the nodes marked ⚠").arg(E).arg(W) : QString("No problems found");
        if (Said != LastVerdict || ValidationAsked) status(Said);   // a live re-check that found the same says nothing
        LastVerdict = Said;
        ValidationAsked = false;
    });
    connect(Model, &PackageEditorModel::dirtyChanged, this, [this, UpdateUndo] { updateTitle(); UpdateUndo(); });
    connect(Model, &PackageEditorModel::savedToDisk, this, &PackageEditor::packageSaved);

    Model->initPackage(PreselectedPath, this);
    updateTitle();
    UpdateUndo();
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
    return true;
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
    QTimer::singleShot(0, this, [this] { validate(); });
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
