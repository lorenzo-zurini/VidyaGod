#include "packageeditor.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"   // the state/signal hub
#include "pkgcanvaspanel.h"       // the blueprint canvas (the editing surface)
#include "pkgactions.h"           // performs the node actions the canvas asks for
#include "jsonraweditor.h"        // raw-JSON view
#include "validationpanel.h"      // docked validation panel
#include "packagecatalog.h"       // PackageCatalog::PublishPackage (the Publish button)
#include "commonutils.h"

#include <QGuiApplication>
#include <QScreen>
#include <QFileDialog>
#include <QDir>
#include <QMessageBox>
#include <filesystem>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>

using json = nlohmann::ordered_json;

// ============================================================================
// Helpers
// ============================================================================



// ============================================================================
// Construction / teardown
// ============================================================================

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent, const QString &PreselectedPath)
    : QDialog(parent)
{
    // The state/signal hub: owns the working document, node I/O, validation, and the authoring runs. Created first
    // so the toolbar/Publish lambdas below can reach it; it parents its modal dialogs on this editor.
    Model = new PackageEditorModel(GlobalConfigJSON, this, this);
    //Registered HERE, not in OpenFor, so every construction path counts - the canvas's single Dear ImGui
    //context is a process-wide resource and a second editor cannot render regardless of who built it.
    if (!Live) Live = this;
    else LogErr("PackageEditor", "A second package editor was constructed; its canvas cannot render. "
                                   "Open the editor through PackageEditor::OpenFor().");
    // A first-class top-level window: a parented QDialog gets _NET_WM_WINDOW_TYPE_DIALOG (no taskbar entry, rides the
    // parent's minimize/tray). Qt::Window makes it a normal window — its own taskbar button + ordinary minimize.
    setWindowFlags(Qt::Window);
    setWindowTitle("VidyaGod Package Editor");
    setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    setWindowState(Qt::WindowMaximized);

    QVBoxLayout * MainLayout = new QVBoxLayout(this);
    MainLayout->setSpacing(1);
    MainLayout->setContentsMargins(0, 0, 0, 0);
    setLayout(MainLayout);

    // Toolbar: add a node / publish the bundle.
    QHBoxLayout * Toolbar = new QHBoxLayout();
    Toolbar->setSpacing(1);
    QPushButton * ValidateBtn  = new QPushButton("Check Package Validity", this);
    QPushButton * FixCaseBtn   = new QPushButton("Fix Case Conflicts", this);
    QPushButton * PublishBtn   = new QPushButton("Publish",  this);
    ValidateBtn->setToolTip("Validate the node graph on demand: dangling/cyclic PARENTS, layer paths, runner resolution,\n"
                            "cross-layer case collisions. Authoring-only — the regular launcher assumes packages are valid.");
    FixCaseBtn->setToolTip("Resolve cross-layer case conflicts: rename case-colliding zip entries in the higher-priority\n"
                           "layers to the base layer's case (unpack→rename→repackage) so patches/add-ons override cleanly.");
    Toolbar->addStretch();
    Toolbar->addWidget(ValidateBtn);
    Toolbar->addWidget(FixCaseBtn);
    Toolbar->addWidget(PublishBtn);
    MainLayout->addLayout(Toolbar);

    //Publish (dehydrate): flush edits, seed each layer's content over IPFS + record its CID into the node files in
    //place, and export a manifest-only copy to a chosen folder (ready to commit into a sharing repo).
    connect(PublishBtn, &QPushButton::clicked, this, [this](){
        if (!PackageDir) return;
        Model->SaveNodes();
        const QString Dest = QFileDialog::getExistingDirectory(
            this, "Export dehydrated copy to… (cancel to dehydrate in place only)");
        std::string Err;
        const bool Ok = PackageCatalog::PublishPackage(PackageDir->path().toStdString(), Dest.toStdString(), &Err);
        if (Ok)
        {
            Model->LoadNodes(); BuildUI();
            QMessageBox::information(this, "Publish",
                Dest.isEmpty() ? "Bundle dehydrated (content seeded, CIDs written into the node files)."
                               : ("Bundle published.\nManifest-only copy exported to:\n" + Dest));
        }
        else QMessageBox::critical(this, "Publish", "Publish failed:\n" + QString::fromStdString(Err));
    });

    //Check package validity: on-demand node-graph validation (the regular launcher no longer validates per-launch).
    //Can be slow for very large packages (deep chains) — that's fine as an explicit authoring action.
    connect(ValidateBtn, &QPushButton::clicked, this, [this](){
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        Model->Revalidate();
        QGuiApplication::restoreOverrideCursor();
        const int Errs = (int)Model->validationErrors().size(), Warns = (int)Model->validationWarnings().size();
        if (!Errs && !Warns) QMessageBox::information(this, "Check Package Validity", "✓ No problems found.");
        else QMessageBox::warning(this, "Check Package Validity",
            QString("%1 error(s), %2 warning(s) — see the Validation panel below.").arg(Errs).arg(Warns));
    });

    //Fix case conflicts: canonicalize case-colliding zip entries in this bundle's higher-priority layers to the
    //base layer's case (unpack→rename→repackage STORE) so patches/add-ons override cleanly on the case-sensitive mount.
    connect(FixCaseBtn, &QPushButton::clicked, this, [this](){
        if (!PackageDir) return;
        Model->SaveNodes();
        std::vector<std::string> Log;
        const std::filesystem::path Scope = PackageDir->path().toStdString();   // only rewrite THIS bundle's zips
        const int Fixed = ManifestModel::FixCaseConflicts(Model->BuildExecIndex(), Log, &Scope);
        Model->Revalidate();
        if (Fixed == 0) { QMessageBox::information(this, "Fix Case Conflicts", "No cross-layer case conflicts found in this package."); return; }
        QString Report; for (const auto &L : Log) Report += QString::fromStdString(L) + "\n";
        QMessageBox::information(this, "Fix Case Conflicts",
            QString("Rewrote %1 zip(s) — case-colliding entries were renamed to the base layer's case.\n\n%2").arg(Fixed).arg(Report));
    });

    // The editing surface IS the graph. A node is one layer of one TYPE, so it draws as one box with pins and
    // its payload inline — which is exactly what the two-tier model could not be rendered as (a node containing
    // an ordered array has no pins to wire). The old tab-strip of per-node forms is gone.
    //The save hook persists BOTH: the package (node files) and the canvas layout (LAYOUT.vglayout).
    //They are deliberately separate files — a drag must never rewrite package bytes.
    Canvas = new PkgCanvasPanel(&Model->doc(), [this]{ Model->SaveNodes(); Model->SaveLayout(); }, this,
                                &Model->layout());
    Canvas->canvas()->setKnownIds([this]{ return Model->KnownNodeIds(); });

    // The canvas draws action buttons and reports clicks; PkgActions does everything that touches disk, spawns
    // a process, opens a dialog or builds a runtime — which is what keeps the canvas headlessly testable.
    Actions = new PkgActions(Model, Canvas->canvas(), this, this);
    // QUEUED: an action opens file dialogs and confirmations. Even dispatched after the ImGui frame closes,
    // running it inline would still sit inside paintGL's call stack; a queued connection puts it on a clean
    // turn of the event loop instead.
    connect(Canvas->canvas(), &PkgCanvas::nodeAction, Actions, &PkgActions::perform, Qt::QueuedConnection);
    connect(Canvas->canvas(), &PkgCanvas::cancelRequested, Actions, &PkgActions::cancel);

    // Right: the raw JSON of the selected node (unchanged — it is the escape hatch and it works), with the
    // validation panel docked beneath it.
    QWidget * RightPanel = new QWidget(this);
    QVBoxLayout * RightLayout = new QVBoxLayout(RightPanel);
    RightLayout->setContentsMargins(0, 0, 0, 0);
    RightLayout->setSpacing(1);
    RightLayout->addWidget(new JsonRawEditor(Model, RightPanel), 1);
    RightLayout->addWidget(new ValidationPanel(Model, RightPanel), 0);

    QSplitter * Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(Canvas);
    Split->addWidget(RightPanel);
    Split->setStretchFactor(0, 1);
    Split->setStretchFactor(1, 0);
    Split->setSizes({ 1250, 470 });
    MainLayout->addWidget(Split, 1);

    // Open the bundle (pick a dir if none was preselected), then point our working-doc/PackageDir aliases at the
    // model's owned state so the existing BuildUI machinery reads them unchanged.
    Model->initPackage(PreselectedPath, this);
    MANIFESTJSON = &Model->doc();
    PackageDir   = Model->packageDir();

    // React to the model: structural change → rebuild tabs; validation update → repaint the panel; a disk write →
    // relay to packageSaved (so library tiles / prelaunch dialogs reload).
    connect(Model, &PackageEditorModel::documentReloaded, this, [this]{ BuildUI(); });
    connect(Model, &PackageEditorModel::savedToDisk, this, &PackageEditor::packageSaved);

    BuildUI();

}

PackageEditor::~PackageEditor()
{
    if (Live == this) Live = nullptr;
}

PackageEditor *PackageEditor::Live = nullptr;

QString PackageEditor::bundleDir() const
{
    return Model ? Model->packagePath() : QString();
}

PackageEditor *PackageEditor::OpenFor(nlohmann::ordered_json *GlobalConfigJSON, QWidget *parent,
                                      const QString &PackagePath, bool *Created)
{
    if (Created) *Created = false;
    if (Live)
    {
        const QString Open = Live->bundleDir();
        if (!PackagePath.isEmpty() && !Open.isEmpty() && QDir(Open) != QDir(PackagePath))
            QMessageBox::information(parent, "Package Editor",
                "The package editor is already open on:\n\n" + Open +
                "\n\nClose it before opening another bundle - the blueprint canvas can only run one at a time.");
        Live->show();
        Live->raise();
        Live->activateWindow();
        return Live;
    }
    auto *Ed = new PackageEditor(GlobalConfigJSON, parent, PackagePath);
    Live = Ed;
    if (Created) *Created = true;
    Ed->show();
    return Ed;
}











// ============================================================================
// UI
// ============================================================================

bool PackageEditor::BuildUI()
{
    Model->Revalidate();
    if (!Canvas) return true;
    // Validation findings are attached to the node they are ABOUT, so a problem is drawn on the node that is
    // wrong instead of in a report you have to correlate by hand. Messages are tagged "node '<id>': …".
    std::vector<std::pair<std::string, std::string>> Issues;
    auto Attach = [&Issues](const std::vector<std::string> &Msgs) {
        for (const std::string &M : Msgs)
        {
            const size_t A = M.find("node '");
            if (A == std::string::npos) continue;
            const size_t B = M.find('\'', A + 6);
            if (B == std::string::npos) continue;
            const std::string Id = M.substr(A + 6, B - (A + 6));
            std::string Text = M.substr(B + 1);
            if (Text.rfind(": ", 0) == 0) Text = Text.substr(2);
            Issues.emplace_back(Id, Text);
        }
    };
    Attach(Model->validationErrors());
    Attach(Model->validationWarnings());
    Canvas->canvas()->invalidateGraph();   // the document may have changed under the cached graph
    Canvas->canvas()->setIssues(Issues);
    if (Actions) Actions->refreshHints();
    Canvas->update();
    return true;
}

//Selects the node with this NODE_ID on the canvas.
void PackageEditor::SelectNodeTab(const std::string & NodeId)
{
    if (!Canvas) return;
    const int I = Canvas->canvas()->indexOf(NodeId);
    if (I >= 0) { Canvas->canvas()->selectNode(I); Canvas->update(); }
}

// ============================================================================
// Field-change slots
// ============================================================================




// ============================================================================
// LAYERS — add-actions
// ============================================================================







// ============================================================================
// Authoring execute (native node engine)
// ============================================================================





// ============================================================================
// META — cover drop
// ============================================================================




