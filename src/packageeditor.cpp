#include "packageeditor.h"
#include "packageeditormodel.h"   // the state/signal hub
#include "nodeeditor.h"           // per-node tab widget
#include "jsonraweditor.h"        // raw-JSON tab widget
#include "validationpanel.h"      // docked validation panel
#include "packagecatalog.h"       // PackageCatalog::PublishPackage (the Publish button)
// NodeGraphView comes via packageeditor.h.

#include <QGuiApplication>
#include <QScreen>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

using json = nlohmann::ordered_json;

// ============================================================================
// Helpers
// ============================================================================



// ============================================================================
// Construction / teardown
// ============================================================================

PackageEditor::PackageEditor(const nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent, const QString &PreselectedPath)
    : QDialog(parent)
{
    // The state/signal hub: owns the working document, node I/O, validation, and the authoring runs. Created first
    // so the toolbar/Publish lambdas below can reach it; it parents its modal dialogs on this editor.
    Model = new PackageEditorModel(GlobalConfigJSON, this, this);
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
    QPushButton * AddNodeBtn = new QPushButton("Add Node", this);
    QPushButton * PublishBtn = new QPushButton("Publish",  this);
    Toolbar->addWidget(AddNodeBtn);
    Toolbar->addStretch();
    Toolbar->addWidget(PublishBtn);
    MainLayout->addLayout(Toolbar);

    connect(AddNodeBtn, &QPushButton::clicked, this, [this](){
        json NewNode = json::object({ {"NODE_ID", "new_node"}, {"ROLE", "content"}, {"LAYERS", json::array()} });
        (*MANIFESTJSON)["NODES"].push_back(NewNode);
        Model->SaveNodes(); BuildUI();
    });

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

    // Node graph overview — a clickable DAG sidebar on the LEFT of the editor. Flows top→down (launchables on top,
    // the PARENTS chain below, branches spreading right). Clicking a chip jumps to that node's editor tab.
    GraphView = new NodeGraphView(this);
    QScrollArea * GraphScroll = new QScrollArea(this);
    GraphScroll->setWidget(GraphView);
    GraphScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    GraphScroll->setFrameShape(QFrame::StyledPanel);
    GraphScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    GraphScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    GraphScroll->setMinimumWidth(150);
    connect(GraphView, &NodeGraphView::nodeClicked, this, [this](const QString & Id){ SelectNodeTab(Id.toStdString()); });

    // Right side: the node tabs, with the validation panel docked beneath them.
    QWidget * RightPanel = new QWidget(this);
    QVBoxLayout * RightLayout = new QVBoxLayout(RightPanel);
    RightLayout->setContentsMargins(0, 0, 0, 0);
    RightLayout->setSpacing(1);

    PackageEditorTabWidget = new QTabWidget(RightPanel);
    RightLayout->addWidget(PackageEditorTabWidget, 1);
    // Keep the graph's highlight in sync with the open tab.
    connect(PackageEditorTabWidget, &QTabWidget::currentChanged, this, [this](int Idx){
        if (!GraphView || !MANIFESTJSON) return;
        const auto & A = (*MANIFESTJSON)["NODES"];
        const int NodeI = Idx - 1;   // tab 0 = JSON
        GraphView->SetCurrent(NodeI >= 0 && NodeI < (int)A.size() ? A[NodeI].value("NODE_ID", std::string()) : std::string());
    });

    // Validation panel — a persistent box docked beneath the tabs; self-refreshes on the model's validationChanged.
    RightLayout->addWidget(new ValidationPanel(Model, RightPanel), 0);

    // Graph sidebar (left) | tabs+validation (right) — resizable.
    QSplitter * Split = new QSplitter(Qt::Horizontal, this);
    Split->addWidget(GraphScroll);
    Split->addWidget(RightPanel);
    Split->setStretchFactor(0, 0);
    Split->setStretchFactor(1, 1);
    Split->setSizes({ 300, 1200 });
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

PackageEditor::~PackageEditor() = default;

// ============================================================================
// Node I/O (one file per node)
// ============================================================================






// ============================================================================
// Validation
// ============================================================================



// ============================================================================
// Catalog queries (PARENTS picker / platform suggestions / exec index)
// ============================================================================




// ============================================================================
// UI
// ============================================================================

bool PackageEditor::BuildUI()
{
    Model->Revalidate();
    SavedMainTab = PackageEditorTabWidget->currentIndex();

    // Free the old tab widgets. QTabWidget::clear() detaches but does NOT delete the pages, so without this they'd
    // leak (and stale JsonRawEditors would keep reacting to the model). deleteLater (not delete) because a child
    // NodeEditor's own button lambda may have triggered this rebuild — it must outlive the current call.
    // Block the whole subtree's signals first: a focused QLineEdit fires editingFinished while losing focus during
    // ~QWidget, which would invoke onFieldEdited on the half-destroyed section (Qt assert/abort). Common trigger:
    // renaming a NODE_ID and pressing Enter, then the rebuild deletes the still-focused field.
    for (int i = PackageEditorTabWidget->count() - 1; i >= 0; --i)
    {
        QWidget * Page = PackageEditorTabWidget->widget(i);
        Page->blockSignals(true);
        for (QObject * Child : Page->findChildren<QObject *>()) Child->blockSignals(true);
        Page->deleteLater();
    }
    PackageEditorTabWidget->clear();

    // ---- JSON tab: raw edit of one node file at a time (its own widget; self-refreshes via the model) ----
    PackageEditorTabWidget->addTab(new JsonRawEditor(Model, PackageEditorTabWidget), "JSON");

    // ---- one tab per node (each its own NodeEditor widget; edits flow through the model) ----
    for (int n = 0; n < (int)(*MANIFESTJSON)["NODES"].size(); n++)
    {
        const std::string NodeIdStr = (*MANIFESTJSON)["NODES"][n].value("NODE_ID", std::string());
        const QString TabLabel = NodeIdStr.empty() ? QString("node %1").arg(n + 1) : QString::fromStdString(NodeIdStr);
        PackageEditorTabWidget->addTab(new NodeEditor(Model, n, PackageEditorTabWidget), TabLabel);
    }

    int MainCount = PackageEditorTabWidget->count();
    PackageEditorTabWidget->setCurrentIndex(qBound(0, SavedMainTab, MainCount - 1));

    // Refresh the node-graph overview and highlight the open node.
    if (GraphView && MANIFESTJSON)
    {
        GraphView->SetGraph((*MANIFESTJSON)["NODES"]);
        const auto & A = (*MANIFESTJSON)["NODES"];
        const int NodeI = PackageEditorTabWidget->currentIndex() - 1;
        GraphView->SetCurrent(NodeI >= 0 && NodeI < (int)A.size() ? A[NodeI].value("NODE_ID", std::string()) : std::string());
    }
    return true;
}

//Jumps the tab widget to the node with this NODE_ID (tab 0 = JSON, tabs 1.. = NODES in array order).
void PackageEditor::SelectNodeTab(const std::string & NodeId)
{
    if (!MANIFESTJSON || !PackageEditorTabWidget) return;
    const auto & A = (*MANIFESTJSON)["NODES"];
    for (int I = 0; I < (int)A.size(); ++I)
        if (A[I].value("NODE_ID", std::string()) == NodeId) { PackageEditorTabWidget->setCurrentIndex(I + 1); return; }
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




