#include "packageeditor.h"
#include "packageeditormodel.h"
#include "jsonraweditor.h"
#include "validationpanel.h"
#include "commonutils.h"
#include "registrywrapper.h"
#include "covercache.h"
#include "processenv.h"   // RunCommand (zip/unzip the package archive)
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QBuffer>
#include <QImage>
#include <QInputDialog>
#include <QMessageBox>
#include <QAbstractButton>
#include <algorithm>
#include <set>
#include <map>
#include <functional>
#include <filesystem>

using json = nlohmann::ordered_json;

// ============================================================================
// Helpers
// ============================================================================

//Walks up `Sender`'s parent chain to the first widget carrying a non-empty "JSONPath" property — the owning
//node tab's "/NODES/<n>" pointer. (When an Add… slot fires from a QMenu action, sender() is a QAction whose
//parent chain is QAction → QMenu → QToolButton → node tab.)
static QString ResolveNodeJSONPath(QObject * Sender)
{
    QObject * Obj = Sender;
    while (Obj)
    {
        QVariant V = Obj->property("JSONPath");
        if (V.isValid() && !V.toString().isEmpty()) return V.toString();
        Obj = Obj->parent();
    }
    return QString();
}


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

    PackageEditorTabWidget->clear();

    // ---- JSON tab: raw edit of one node file at a time (its own widget; self-refreshes via the model) ----
    PackageEditorTabWidget->addTab(new JsonRawEditor(Model, PackageEditorTabWidget), "JSON");

    // ---- one tab per node ----
    static const std::vector<std::string> MetadataFields = {
        "RELEASEDATE", "EDITION", "EDITIONDATE", "DEVELOPER", "PUBLISHER",
        "TGDBID", "STEAMAPPID", "GOGPRODUCTID", "UMUID",
        "SERIES", "SERIESSORTNUMBER", "SUBSERIES", "SUBSERIESSORTNUMBER",
        "EDITOR", "ONLINEDRM",
        "NETWORKMULTIPLAYER", "DIRECTCONNECT", "LANMULTIPLAYER", "ONLINEMULTIPLAYER",
        "NETWORKCOOP", "LOCALMULTIPLAYER", "LOCALCOOP", "OTHERONLINEFEATURES"
    };

    for (int n = 0; n < (int)(*MANIFESTJSON)["NODES"].size(); n++)
    {
        auto &NodeRef = (*MANIFESTJSON)["NODES"][n];
        const std::string NodeIdStr = NodeRef.value("NODE_ID", std::string());
        const std::string NodePtr   = "/NODES/" + std::to_string(n);

        QWidget * NodeTab = new QWidget(PackageEditorTabWidget);
        NodeTab->setProperty("JSONPath", QString::fromStdString(NodePtr));   // owning-node pointer for Add… / authoring
        NodeTab->setProperty("NodeArrayIndex", n);
        QVBoxLayout * NodeTabLayout = new QVBoxLayout(NodeTab);
        NodeTab->setLayout(NodeTabLayout);

        // Toolbar: authoring tools + add-layer menu + remove/move.
        QHBoxLayout * Toolbar = new QHBoxLayout();
        NodeTabLayout->addLayout(Toolbar);

        QPushButton * RunExeBtn = new QPushButton("Run EXE", NodeTab);
        QPushButton * BrowseBtn = new QPushButton("Browse", NodeTab);
        QPushButton * RegBtn    = new QPushButton("Edit Registry", NodeTab);
        QPushButton * ExecBtn   = new QPushButton("Execute", NodeTab);
        QPushButton * AnalyzeBtn= new QPushButton("Analyze Registry", NodeTab);
        Toolbar->addWidget(RunExeBtn); Toolbar->addWidget(BrowseBtn);
        Toolbar->addWidget(RegBtn); Toolbar->addWidget(ExecBtn); Toolbar->addWidget(AnalyzeBtn);
        QObject::connect(RunExeBtn, &QPushButton::clicked, this, [this, NodeIdStr](){
            QString Exe = QFileDialog::getOpenFileName(this, "Select executable");
            if (!Exe.isEmpty()) Model->RunInNode(NodeIdStr, Exe.toStdString());
        });
        QObject::connect(BrowseBtn, &QPushButton::clicked, this, [this, NodeIdStr](){ Model->RunInNode(NodeIdStr, "explorer.exe"); });
        QObject::connect(RegBtn,    &QPushButton::clicked, this, [this, NodeIdStr](){ Model->RunInNode(NodeIdStr, "regedit.exe"); });
        QObject::connect(ExecBtn,   &QPushButton::clicked, this, [this, NodeIdStr](){ Model->RunInNode(NodeIdStr, ""); });
        QObject::connect(AnalyzeBtn,&QPushButton::clicked, this, [this, NodeIdStr](){ Model->AnalyzeNodeRegistry(NodeIdStr); });

        Toolbar->addStretch();

        QToolButton * AddLayerBtn = new QToolButton(NodeTab);
        AddLayerBtn->setText("+ Layer");
        AddLayerBtn->setPopupMode(QToolButton::InstantPopup);
        QMenu * LayerMenu = new QMenu(AddLayerBtn);
        LayerMenu->addAction("VFSZipLayer",   this, &PackageEditor::AddVFSZipLayer);
        LayerMenu->addAction("VFSDirLayer",   this, &PackageEditor::AddVFSDirLayer);
        LayerMenu->addAction("VFSFileLayer",  this, &PackageEditor::AddVFSFileLayer);
        LayerMenu->addSeparator();
        LayerMenu->addAction("RegEdit",       this, &PackageEditor::AddRegEdit);
        LayerMenu->addAction("DllOverride",   this, &PackageEditor::AddDllOverride);
        LayerMenu->addAction("FileEdit",      this, &PackageEditor::AddFileEdit);
        LayerMenu->addSeparator();
        LayerMenu->addAction("PersistDir",    this, &PackageEditor::AddPersistDir);
        LayerMenu->addAction("PersistFile",   this, &PackageEditor::AddPersistFile);
        LayerMenu->addAction("RegPersist",    this, &PackageEditor::AddRegPersist);
        LayerMenu->addAction("RegKeyPersist", this, &PackageEditor::AddRegKeyPersist);
        LayerMenu->addSeparator();
        LayerMenu->addAction("CustomVar",     this, &PackageEditor::AddCustomVar);
        AddLayerBtn->setMenu(LayerMenu);
        Toolbar->addWidget(AddLayerBtn);

        QPushButton * MoveUpBtn = new QPushButton("↑", NodeTab); MoveUpBtn->setFixedWidth(30); MoveUpBtn->setEnabled(n > 0);
        QPushButton * MoveDnBtn = new QPushButton("↓", NodeTab); MoveDnBtn->setFixedWidth(30);
        MoveDnBtn->setEnabled(n < (int)(*MANIFESTJSON)["NODES"].size() - 1);
        QPushButton * RemoveBtn = new QPushButton("Remove Node", NodeTab);
        Toolbar->addWidget(MoveUpBtn); Toolbar->addWidget(MoveDnBtn); Toolbar->addWidget(RemoveBtn);
        QObject::connect(MoveUpBtn, &QPushButton::clicked, this, [this, n](){
            if (n <= 0) return;
            std::swap((*MANIFESTJSON)["NODES"][n - 1], (*MANIFESTJSON)["NODES"][n]);
            SavedMainTab = (n - 1) + 1; Model->SaveNodes(); BuildUI();
        });
        QObject::connect(MoveDnBtn, &QPushButton::clicked, this, [this, n](){
            auto &A = (*MANIFESTJSON)["NODES"]; if (n + 1 >= (int)A.size()) return;
            std::swap(A[n], A[n + 1]); SavedMainTab = (n + 1) + 1; Model->SaveNodes(); BuildUI();
        });
        QObject::connect(RemoveBtn, &QPushButton::clicked, this, [this, n, NodeIdStr](){
            if (QMessageBox::question(this, "Remove node",
                    "Delete node “" + QString::fromStdString(NodeIdStr) + "” and its file?") != QMessageBox::Yes) return;
            (*MANIFESTJSON)["NODES"].erase(n); Model->SaveNodes(); BuildUI();
        });

        QScrollArea * Scroll = new QScrollArea(NodeTab);
        Scroll->setWidgetResizable(true);
        Scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        NodeTabLayout->addWidget(Scroll);
        QWidget * Contents = new QWidget();
        QVBoxLayout * Body = new QVBoxLayout(Contents);
        Contents->setLayout(Body);
        Scroll->setWidget(Contents);

        // ---- Identity (NODE_ID / ROLE / UID / GROUP / LABEL / RECOMMENDED) ----
        {
            QGroupBox * Box = new QGroupBox("Identity", Contents);
            QFormLayout * Form = new QFormLayout(Box);

            QLineEdit * IdField = new QLineEdit(Box);
            IdField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/NODE_ID"));
            IdField->setText(QString::fromStdString(NodeIdStr));
            QObject::connect(IdField, &QLineEdit::editingFinished, this, [this](){
                JSONQLineEditChanged(); BuildUI();   // renames re-file the node + relabel the tab
            });
            Form->addRow("NODE_ID", IdField);

            QComboBox * RoleCombo = new QComboBox(Box);
            RoleCombo->addItems({"content", "launchable", "runner"});
            RoleCombo->setCurrentText(QString::fromStdString(NodeRef.value("ROLE", std::string("content"))));
            QObject::connect(RoleCombo, &QComboBox::currentTextChanged, this, [this, NodePtr](const QString &T){
                (*MANIFESTJSON)[json::json_pointer(NodePtr + "/ROLE")] = T.toStdString();
                Model->SaveNodes(); BuildUI();   // re-render so role-specific sections (GROUP/LABEL/GUEST) adapt
            });
            Form->addRow("ROLE", RoleCombo);

            QLineEdit * UidField = new QLineEdit(Box);
            UidField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/UID"));
            if (NodeRef.contains("UID") && NodeRef["UID"].is_string()) UidField->setText(QString::fromStdString(std::string(NodeRef["UID"])));
            QObject::connect(UidField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
            Form->addRow("UID", UidField);

            const bool Launchable = NodeRef.value("ROLE", std::string()) == "launchable";
            if (Launchable)
            {
                QLineEdit * GroupField = new QLineEdit(Box);
                GroupField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/GROUP"));
                GroupField->setPlaceholderText("library-tile grouping key (defaults to NODE_ID)");
                if (NodeRef.contains("GROUP") && NodeRef["GROUP"].is_string()) GroupField->setText(QString::fromStdString(std::string(NodeRef["GROUP"])));
                QObject::connect(GroupField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                Form->addRow("GROUP", GroupField);

                QLineEdit * LabelField = new QLineEdit(Box);
                LabelField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/LABEL"));
                LabelField->setPlaceholderText("edition label for the picker dropdown");
                if (NodeRef.contains("LABEL") && NodeRef["LABEL"].is_string()) LabelField->setText(QString::fromStdString(std::string(NodeRef["LABEL"])));
                QObject::connect(LabelField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                Form->addRow("LABEL", LabelField);

                QCheckBox * RecChk = new QCheckBox("Recommended (default edition in its GROUP)", Box);
                RecChk->setChecked(NodeRef.value("RECOMMENDED", false));
                QObject::connect(RecChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                    (*MANIFESTJSON)[json::json_pointer(NodePtr + "/RECOMMENDED")] = On; Model->SaveNodes();
                });
                Form->addRow("", RecChk);
            }
            Body->addWidget(Box);
        }

        // ---- Selection (OPTIONAL / DEFAULT / EXCLUDE) ----
        {
            QGroupBox * Box = new QGroupBox("Selection (when referenced as a parent)", Contents);
            QVBoxLayout * BL = new QVBoxLayout(Box);

            QCheckBox * OptChk = new QCheckBox("OPTIONAL (user-toggleable add-on)", Box);
            OptChk->setChecked(NodeRef.value("OPTIONAL", false));
            QObject::connect(OptChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                (*MANIFESTJSON)[json::json_pointer(NodePtr + "/OPTIONAL")] = On; Model->SaveNodes();
            });
            BL->addWidget(OptChk);

            QCheckBox * DefChk = new QCheckBox("DEFAULT (enabled by default when optional)", Box);
            DefChk->setChecked(NodeRef.value("DEFAULT", true));
            QObject::connect(DefChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                (*MANIFESTJSON)[json::json_pointer(NodePtr + "/DEFAULT")] = On; Model->SaveNodes();
            });
            BL->addWidget(DefChk);

            QLineEdit * ExcludeField = new QLineEdit(Box);
            ExcludeField->setPlaceholderText("EXCLUDE — node ids mutually exclusive with this one (comma-separated)");
            { std::string Cur; if (NodeRef.contains("EXCLUDE") && NodeRef["EXCLUDE"].is_array())
                for (const auto &E : NodeRef["EXCLUDE"]) if (E.is_string()) Cur += (Cur.empty() ? "" : ", ") + std::string(E);
              ExcludeField->setText(QString::fromStdString(Cur)); }
            QObject::connect(ExcludeField, &QLineEdit::editingFinished, this, [this, NodePtr, ExcludeField](){
                json Arr = json::array();
                for (const QString &Part : ExcludeField->text().split(',', Qt::SkipEmptyParts))
                { std::string Id = Part.trimmed().toStdString(); if (!Id.empty()) Arr.push_back(Id); }
                if (Arr.empty()) (*MANIFESTJSON)[json::json_pointer(NodePtr)].erase("EXCLUDE");
                else             (*MANIFESTJSON)[json::json_pointer(NodePtr + "/EXCLUDE")] = Arr;
                Model->SaveNodes();
            });
            BL->addWidget(ExcludeField);
            Body->addWidget(Box);
        }

        // ---- PARENTS (global catalog-wide id picker; load order: later = higher priority) ----
        {
            const std::string PPtr = NodePtr + "/PARENTS";
            if (!NodeRef.contains("PARENTS") || !NodeRef["PARENTS"].is_array())
                (*MANIFESTJSON)[json::json_pointer(PPtr)] = json::array();
            auto &Parents = (*MANIFESTJSON)[json::json_pointer(PPtr)];

            QGroupBox * Box = new QGroupBox("PARENTS (load order — later overrides earlier)", Contents);
            QVBoxLayout * BL = new QVBoxLayout(Box);
            const std::vector<std::string> AllIds = Model->KnownNodeIds();

            for (int k = 0; k < (int)Parents.size(); k++)
            {
                QHBoxLayout * Row = new QHBoxLayout();
                QComboBox * Pick = new QComboBox(Box);
                Pick->setEditable(true);
                for (const auto &Id : AllIds) Pick->addItem(QString::fromStdString(Id));
                { QSignalBlocker B(Pick); Pick->setCurrentText(QString::fromStdString(Parents[k].is_string() ? std::string(Parents[k]) : "")); }
                const std::string ItemPtr = PPtr + "/" + std::to_string(k);
                QObject::connect(Pick, &QComboBox::currentTextChanged, this, [this, ItemPtr](const QString &T){
                    (*MANIFESTJSON)[json::json_pointer(ItemPtr)] = T.trimmed().toStdString(); Model->SaveNodes();
                });
                QPushButton * Up = new QPushButton("▲", Box); Up->setFixedWidth(28);
                QPushButton * Dn = new QPushButton("▼", Box); Dn->setFixedWidth(28);
                QPushButton * Del = new QPushButton("✕", Box); Del->setFixedWidth(28);
                QObject::connect(Up, &QPushButton::clicked, this, [this, PPtr, k](){
                    auto &A = (*MANIFESTJSON)[json::json_pointer(PPtr)]; if (k <= 0) return; std::swap(A[k - 1], A[k]); Model->SaveNodes(); BuildUI();
                });
                QObject::connect(Dn, &QPushButton::clicked, this, [this, PPtr, k](){
                    auto &A = (*MANIFESTJSON)[json::json_pointer(PPtr)]; if (k + 1 >= (int)A.size()) return; std::swap(A[k], A[k + 1]); Model->SaveNodes(); BuildUI();
                });
                QObject::connect(Del, &QPushButton::clicked, this, [this, PPtr, k](){
                    (*MANIFESTJSON)[json::json_pointer(PPtr)].erase(k); Model->SaveNodes(); BuildUI();
                });
                Row->addWidget(Pick, 1); Row->addWidget(Up); Row->addWidget(Dn); Row->addWidget(Del);
                BL->addLayout(Row);
            }
            QPushButton * AddParent = new QPushButton("+ Add Parent", Box);
            QObject::connect(AddParent, &QPushButton::clicked, this, [this, PPtr](){
                (*MANIFESTJSON)[json::json_pointer(PPtr)].push_back(""); Model->SaveNodes(); BuildUI();
            });
            BL->addWidget(AddParent);
            Body->addWidget(Box);
        }

        // ---- PLATFORM (HOST + GUEST for runner nodes) ----
        {
            QGroupBox * Box = new QGroupBox("Platform", Contents);
            QVBoxLayout * BL = new QVBoxLayout(Box);
            const std::vector<std::string> Plats = Model->KnownPlatforms();

            QHBoxLayout * HostRow = new QHBoxLayout();
            HostRow->addWidget(new QLabel("HOST:", Box));
            QComboBox * Host = new QComboBox(Box); Host->setEditable(true);
            Host->lineEdit()->setPlaceholderText("win32, win64, linux64, snes…");
            for (const auto &P : Plats) Host->addItem(QString::fromStdString(P));
            { QSignalBlocker B(Host);
              Host->setCurrentText(QString::fromStdString((NodeRef.contains("PLATFORM") && NodeRef["PLATFORM"].is_object()
                                   && NodeRef["PLATFORM"].value("HOST", std::string()).size()) ? std::string(NodeRef["PLATFORM"]["HOST"]) : "")); }
            QObject::connect(Host, &QComboBox::currentTextChanged, this, [this, NodePtr](const QString &T){
                const QString Tr = T.trimmed();
                if (Tr.isEmpty()) { if ((*MANIFESTJSON).contains(json::json_pointer(NodePtr + "/PLATFORM"))) (*MANIFESTJSON)[json::json_pointer(NodePtr + "/PLATFORM")].erase("HOST"); }
                else (*MANIFESTJSON)[json::json_pointer(NodePtr + "/PLATFORM/HOST")] = Tr.toStdString();
                Model->SaveNodes();
            });
            HostRow->addWidget(Host, 1);
            BL->addLayout(HostRow);

            // GUEST list (runner nodes serve these guest platforms).
            const std::string GPtr = NodePtr + "/PLATFORM/GUEST";
            const bool IsRunner = NodeRef.value("ROLE", std::string()) == "runner";
            if (IsRunner)
            {
                if (!(*MANIFESTJSON).contains(json::json_pointer(GPtr)) || !(*MANIFESTJSON)[json::json_pointer(GPtr)].is_array())
                    (*MANIFESTJSON)[json::json_pointer(GPtr)] = json::array();
                auto &Guests = (*MANIFESTJSON)[json::json_pointer(GPtr)];
                QGroupBox * GBox = new QGroupBox("GUEST (platforms this runner serves)", Box);
                QVBoxLayout * GBL = new QVBoxLayout(GBox);
                for (int k = 0; k < (int)Guests.size(); k++)
                {
                    QHBoxLayout * Row = new QHBoxLayout();
                    QComboBox * CB = new QComboBox(GBox); CB->setEditable(true);
                    for (const auto &P : Plats) CB->addItem(QString::fromStdString(P));
                    { QSignalBlocker B(CB); CB->setCurrentText(QString::fromStdString(Guests[k].is_string() ? std::string(Guests[k]) : "")); }
                    const std::string ItemPtr = GPtr + "/" + std::to_string(k);
                    QObject::connect(CB, &QComboBox::currentTextChanged, this, [this, ItemPtr](const QString &T){
                        (*MANIFESTJSON)[json::json_pointer(ItemPtr)] = T.trimmed().toStdString(); Model->SaveNodes();
                    });
                    QPushButton * Del = new QPushButton("✕", GBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, GPtr, k](){
                        (*MANIFESTJSON)[json::json_pointer(GPtr)].erase(k); Model->SaveNodes(); BuildUI();
                    });
                    Row->addWidget(CB, 1); Row->addWidget(Del); GBL->addLayout(Row);
                }
                QPushButton * AddG = new QPushButton("+ Add Guest", GBox);
                QObject::connect(AddG, &QPushButton::clicked, this, [this, GPtr](){
                    (*MANIFESTJSON)[json::json_pointer(GPtr)].push_back(""); Model->SaveNodes(); BuildUI();
                });
                GBL->addWidget(AddG);
                BL->addWidget(GBox);
            }
            Body->addWidget(Box);
        }

        // ---- EXEC (flat invocation/content block) ----
        {
            const std::string EPtr = NodePtr + "/EXEC";
            if (!NodeRef.contains("EXEC") || !NodeRef["EXEC"].is_object())
                (*MANIFESTJSON)[json::json_pointer(EPtr)] = json::object();
            auto &Exec = (*MANIFESTJSON)[json::json_pointer(EPtr)];

            QGroupBox * Box = new QGroupBox("EXEC (invocation / content)", Contents);
            QGridLayout * G = new QGridLayout(Box);
            int row = 0;
            for (const char *FK : {"CONTENTPATH", "EXEARGS", "WORKDIR", "EXECUTABLE", "CONTENT_ROOT"})
            {
                G->addWidget(new QLabel(QString(FK) + ":"), row, 0);
                QLineEdit * FE = new QLineEdit(Box);
                FE->setProperty("JSONPath", QString::fromStdString(EPtr + "/" + FK));
                if (Exec.contains(FK) && Exec[FK].is_string()) FE->setText(QString::fromStdString(std::string(Exec[FK])));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                G->addWidget(FE, row, 1, 1, 2); row++;
            }
            QCheckBox * PGen = new QCheckBox("PREFIX_GENERATE (build a wine prefix)", Box);
            PGen->setChecked(Exec.value("PREFIX_GENERATE", false));
            QObject::connect(PGen, &QCheckBox::toggled, this, [this, EPtr](bool On){
                (*MANIFESTJSON)[json::json_pointer(EPtr + "/PREFIX_GENERATE")] = On; Model->SaveNodes();
            });
            G->addWidget(PGen, row, 0, 1, 3); row++;

            // ARGS / REMOVE_ENV string-list editors.
            auto AddList = [&](const QString &Title, const std::string &Key){
                const std::string AP = EPtr + "/" + Key;
                if (!(*MANIFESTJSON).contains(json::json_pointer(AP)) || !(*MANIFESTJSON)[json::json_pointer(AP)].is_array())
                    (*MANIFESTJSON)[json::json_pointer(AP)] = json::array();
                auto &Arr = (*MANIFESTJSON)[json::json_pointer(AP)];
                QGroupBox * LBox = new QGroupBox(Title, Box);
                QVBoxLayout * LBL = new QVBoxLayout(LBox);
                for (int k = 0; k < (int)Arr.size(); k++)
                {
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * LE = new QLineEdit(LBox);
                    LE->setProperty("JSONPath", QString::fromStdString(AP + "/" + std::to_string(k)));
                    LE->setText(QString::fromStdString(Arr[k].is_string() ? std::string(Arr[k]) : ""));
                    QObject::connect(LE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    QPushButton * Del = new QPushButton("✕", LBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, AP, k](){
                        (*MANIFESTJSON)[json::json_pointer(AP)].erase(k); Model->SaveNodes(); BuildUI();
                    });
                    Row->addWidget(LE, 1); Row->addWidget(Del); LBL->addLayout(Row);
                }
                QPushButton * Add = new QPushButton("+ Add", LBox);
                QObject::connect(Add, &QPushButton::clicked, this, [this, AP](){
                    (*MANIFESTJSON)[json::json_pointer(AP)].push_back(""); Model->SaveNodes(); BuildUI();
                });
                LBL->addWidget(Add);
                G->addWidget(LBox, row, 0, 1, -1); row++;
            };
            AddList("ARGS", "ARGS");
            AddList("REMOVE_ENV", "REMOVE_ENV");

            // ENV key/value.
            {
                const std::string EnvPtr = EPtr + "/ENV";
                if (!(*MANIFESTJSON).contains(json::json_pointer(EnvPtr)) || !(*MANIFESTJSON)[json::json_pointer(EnvPtr)].is_object())
                    (*MANIFESTJSON)[json::json_pointer(EnvPtr)] = json::object();
                auto &EnvObj = (*MANIFESTJSON)[json::json_pointer(EnvPtr)];
                QGroupBox * EnvBox = new QGroupBox("ENV", Box);
                QVBoxLayout * EL = new QVBoxLayout(EnvBox);
                for (auto &[K, V] : EnvObj.items())
                {
                    std::string KeyCopy = K;
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * KeyField = new QLineEdit(EnvBox); KeyField->setText(QString::fromStdString(KeyCopy)); KeyField->setPlaceholderText("KEY");
                    QLineEdit * ValField = new QLineEdit(EnvBox);
                    ValField->setProperty("JSONPath", QString::fromStdString(EnvPtr + "/" + KeyCopy));
                    if (V.is_string()) ValField->setText(QString::fromStdString(std::string(V)));
                    QObject::connect(ValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    QObject::connect(KeyField, &QLineEdit::editingFinished, this, [this, EnvPtr, KeyCopy, KeyField](){
                        std::string NewKey = KeyField->text().toStdString();
                        if (NewKey == KeyCopy || NewKey.empty()) return;
                        auto &O = (*MANIFESTJSON)[json::json_pointer(EnvPtr)];
                        auto Val = O[KeyCopy]; O.erase(KeyCopy); O[NewKey] = Val; Model->SaveNodes(); BuildUI();
                    });
                    QPushButton * Del = new QPushButton("✕", EnvBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, EnvPtr, KeyCopy](){
                        (*MANIFESTJSON)[json::json_pointer(EnvPtr)].erase(KeyCopy); Model->SaveNodes(); BuildUI();
                    });
                    Row->addWidget(KeyField); Row->addWidget(ValField); Row->addWidget(Del); EL->addLayout(Row);
                }
                QPushButton * AddEnv = new QPushButton("+ Add Env Var", EnvBox);
                QObject::connect(AddEnv, &QPushButton::clicked, this, [this, EnvPtr](){
                    (*MANIFESTJSON)[json::json_pointer(EnvPtr)]["NEW_KEY"] = ""; Model->SaveNodes(); BuildUI();
                });
                EL->addWidget(AddEnv);
                G->addWidget(EnvBox, row, 0, 1, -1); row++;
            }
            Body->addWidget(Box);
        }

        // ---- META (presentable: cover + TITLE + metadata) ----
        {
            const std::string MPtr = NodePtr + "/META";
            if (!NodeRef.contains("META") || !NodeRef["META"].is_object())
                (*MANIFESTJSON)[json::json_pointer(MPtr)] = json::object();
            auto &Meta = (*MANIFESTJSON)[json::json_pointer(MPtr)];

            QGroupBox * Box = new QGroupBox("META (library presentation)", Contents);
            QVBoxLayout * BL = new QVBoxLayout(Box);

            // Cover drop area.
            QLabel * Cover = new QLabel(Box);
            Cover->setFixedSize(150, 225);
            Cover->setAlignment(Qt::AlignCenter);
            Cover->setWordWrap(true);
            Cover->setStyleSheet("QLabel { background-color:#1e1e1e; border:2px dashed #555; color:#777; border-radius:4px; font-size:11px; }");
            Cover->setText("Drop cover art\nhere\n\n2 : 3");
            Cover->setAcceptDrops(true);
            Cover->setProperty("NodeArrayIndex", n);
            if (Meta.contains("COVER"))
            {
                const QString P = CoverCache::instance()->resolve(Meta["COVER"], PackageDir->path());
                if (!P.isEmpty()) { QPixmap Pix(P); if (!Pix.isNull()) Cover->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
            }
            Cover->installEventFilter(this);
            QHBoxLayout * CoverRow = new QHBoxLayout(); CoverRow->addWidget(Cover); CoverRow->addStretch();
            BL->addLayout(CoverRow);

            QFormLayout * Form = new QFormLayout();
            BL->addLayout(Form);
            QLineEdit * TitleField = new QLineEdit(Box);
            TitleField->setProperty("JSONPath", QString::fromStdString(MPtr + "/TITLE"));
            if (Meta.contains("TITLE") && Meta["TITLE"].is_string()) TitleField->setText(QString::fromStdString(std::string(Meta["TITLE"])));
            QObject::connect(TitleField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
            Form->addRow("TITLE", TitleField);
            for (const std::string &FK : MetadataFields)
            {
                QLineEdit * FE = new QLineEdit(Box);
                FE->setProperty("JSONPath", QString::fromStdString(MPtr + "/" + FK));
                if (Meta.contains(FK) && !Meta[FK].is_null())
                    FE->setText(Meta[FK].is_string() ? QString::fromStdString(std::string(Meta[FK])) : QString::fromStdString(Meta[FK].dump()));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                Form->addRow(QString::fromStdString(FK), FE);
            }
            Body->addWidget(Box);
        }

        // ---- LAYERS (the per-TYPE sub-editors) ----
        {
            if (!NodeRef.contains("LAYERS") || !NodeRef["LAYERS"].is_array())
                (*MANIFESTJSON)[json::json_pointer(NodePtr + "/LAYERS")] = json::array();
            QGroupBox * Box = new QGroupBox("LAYERS", Contents);
            QFormLayout * Form = new QFormLayout(Box);

            auto &Layers = (*MANIFESTJSON)["NODES"][n]["LAYERS"];
            for (int j = 0; j < (int)Layers.size(); j++)
            {
                const QString LayerType = QString::fromStdString(Layers[j].value("TYPE", std::string()));
                QGroupBox * LBox = new QGroupBox(Box);
                QGridLayout * LG = new QGridLayout(LBox);
                LBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

                LG->addWidget(new QLabel("TYPE:"), 0, 0);
                LG->addWidget(new QLabel(LayerType), 0, 1);
                {
                    QPushButton * Rem = new QPushButton("✕", LBox); Rem->setFixedWidth(28);
                    QObject::connect(Rem, &QPushButton::clicked, this, [this, n, j](){
                        (*MANIFESTJSON)["NODES"][n]["LAYERS"].erase(j); Model->SaveNodes(); BuildUI();
                    });
                    LG->addWidget(Rem, 0, 2);
                }
                const QString LPath = QString("/NODES/%1/LAYERS/%2").arg(n).arg(j);

                if (LayerType == "VFSZipLayer" || LayerType == "VFSDirLayer" || LayerType == "VFSFileLayer")
                {
                    LG->addWidget(new QLabel("PATH:"), 1, 0);
                    QLineEdit * PathField = new QLineEdit(LBox);
                    PathField->setProperty("JSONPath", LPath + "/PATH");
                    if (Layers[j].contains("PATH") && Layers[j]["PATH"].is_string()) PathField->setText(QString::fromStdString(std::string(Layers[j]["PATH"])));
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(PathField, 1, 1);

                    if (LayerType == "VFSDirLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ ZIP", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j](){
                            std::string Path = (*MANIFESTJSON)["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path SrcDir = std::filesystem::path(PackageDir->path().toStdString()) / Path;
                            std::string ZipName = Path + ".zip";
                            std::filesystem::path ZipFile = std::filesystem::path(PackageDir->path().toStdString()) / ZipName;
                            RunCommand("zip", {"-0", "-r", "-X", ZipFile.string(), "."},
                                                         QProcessEnvironment::systemEnvironment(), SrcDir.string());
                            std::filesystem::remove_all(SrcDir);
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSZipLayer";
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["PATH"] = ZipName;
                            Model->SaveNodes(); BuildUI();
                        });
                        LG->addWidget(Conv, 1, 2);
                    }
                    else if (LayerType == "VFSZipLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ DIR", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j](){
                            std::string Path = (*MANIFESTJSON)["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path ZipFile = std::filesystem::path(PackageDir->path().toStdString()) / Path;
                            std::string DirName = (Path.size() > 4 && Path.substr(Path.size() - 4) == ".zip") ? Path.substr(0, Path.size() - 4) : Path + "_dir";
                            std::filesystem::path DirPath = std::filesystem::path(PackageDir->path().toStdString()) / DirName;
                            std::filesystem::create_directories(DirPath);
                            RunCommand("unzip", {ZipFile.string(), "-d", DirPath.string()});
                            std::filesystem::remove(ZipFile);
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSDirLayer";
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["PATH"] = DirName;
                            Model->SaveNodes(); BuildUI();
                        });
                        LG->addWidget(Conv, 1, 2);
                    }

                    if (LayerType == "VFSZipLayer" || LayerType == "VFSDirLayer")
                    {
                        auto &SubRef = (*MANIFESTJSON)["NODES"][n]["LAYERS"][j];
                        LG->addWidget(new QLabel("TARGET:"), 2, 0);
                        QLineEdit * TargetField = new QLineEdit(LBox);
                        TargetField->setProperty("JSONPath", LPath + "/TARGET");
                        if (SubRef.contains("TARGET") && SubRef["TARGET"].is_string()) TargetField->setText(QString::fromStdString(std::string(SubRef["TARGET"])));
                        QObject::connect(TargetField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        LG->addWidget(TargetField, 2, 1);

                        LG->addWidget(new QLabel("SUBMOUNTS:"), 3, 0);
                        QTextEdit * SubMounts = new QTextEdit(LBox);
                        SubMounts->setPlaceholderText("source/path:dest/path   (one per line; empty = whole source at TARGET)");
                        SubMounts->setFixedHeight(72);
                        if (SubRef.contains("SUBMOUNTS") && SubRef["SUBMOUNTS"].is_array())
                        { QString Txt; for (const auto &E : SubRef["SUBMOUNTS"]) if (E.is_string()) Txt += QString::fromStdString(std::string(E)) + "\n"; SubMounts->setPlainText(Txt.trimmed()); }
                        QObject::connect(SubMounts, &QTextEdit::textChanged, this, [this, LPath, SubMounts](){
                            json Arr = json::array();
                            for (const QString &Line : SubMounts->toPlainText().split('\n')) { QString T = Line.trimmed(); if (!T.isEmpty()) Arr.push_back(T.toStdString()); }
                            auto Ptr = json::json_pointer(LPath.toStdString());
                            if (Arr.empty()) (*MANIFESTJSON)[Ptr].erase("SUBMOUNTS"); else (*MANIFESTJSON)[Ptr]["SUBMOUNTS"] = Arr;
                            Model->SaveNodes();
                        });
                        LG->addWidget(SubMounts, 3, 1);
                    }
                }
                else if (LayerType == "RegEdit")
                {
                    LG->addWidget(new QLabel("REGPATH:"), 1, 0);
                    QLineEdit * RegPath = new QLineEdit(LBox);
                    RegPath->setProperty("JSONPath", LPath + "/REGPATH");
                    if (Layers[j].contains("REGPATH") && Layers[j]["REGPATH"].is_string()) RegPath->setText(QString::fromStdString(std::string(Layers[j]["REGPATH"])));
                    QObject::connect(RegPath, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(RegPath, 1, 1);

                    LG->addWidget(new QLabel("ARCHITECTURE:"), 2, 0);
                    QLineEdit * Arch = new QLineEdit(LBox);
                    Arch->setProperty("JSONPath", LPath + "/ARCHITECTURE");
                    if (Layers[j].contains("ARCHITECTURE") && Layers[j]["ARCHITECTURE"].is_string()) Arch->setText(QString::fromStdString(std::string(Layers[j]["ARCHITECTURE"])));
                    QObject::connect(Arch, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(Arch, 2, 1);

                    if (!Layers[j].contains("KEYVALUES") || !Layers[j]["KEYVALUES"].is_object())
                        (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["KEYVALUES"] = json::object();
                    QGroupBox * KeysBox = new QGroupBox("KEYS", LBox);
                    QGridLayout * KG = new QGridLayout(KeysBox);
                    KG->addWidget(new QLabel("Key"), 0, 0); KG->addWidget(new QLabel("Value"), 0, 1);
                    int k = 1;
                    for (auto Obj : (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["KEYVALUES"].items())
                    {
                        std::string OrigKey = Obj.key();
                        QLineEdit * KeyEdit = new QLineEdit(KeysBox);
                        KeyEdit->setText(QString::fromStdString(OrigKey));
                        KeyEdit->setProperty("OriginalKey", QString::fromStdString(OrigKey));
                        QObject::connect(KeyEdit, &QLineEdit::editingFinished, this, [this, n, j, KeyEdit](){
                            QString OldKey = KeyEdit->property("OriginalKey").toString(), NewKey = KeyEdit->text();
                            if (OldKey == NewKey || NewKey.isEmpty()) return;
                            auto &KV = (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["KEYVALUES"];
                            auto Val = KV[OldKey.toStdString()]; KV.erase(OldKey.toStdString()); KV[NewKey.toStdString()] = Val;
                            KeyEdit->setProperty("OriginalKey", NewKey); Model->SaveNodes();
                        });
                        KG->addWidget(KeyEdit, k, 0);
                        QLineEdit * ValField = new QLineEdit(KeysBox);
                        ValField->setProperty("JSONPath", LPath + "/KEYVALUES/" + QString::fromStdString(OrigKey));
                        if (Obj.value().is_string()) ValField->setText(QString::fromStdString(std::string(Obj.value())));
                        else if (!Obj.value().is_null()) ValField->setText(QString::fromStdString(Obj.value().dump()));
                        QObject::connect(ValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        KG->addWidget(ValField, k, 1);
                        QPushButton * KeyDel = new QPushButton("✕", KeysBox); KeyDel->setFixedWidth(28);
                        QObject::connect(KeyDel, &QPushButton::clicked, this, [this, n, j, OrigKey](){
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["KEYVALUES"].erase(OrigKey); Model->SaveNodes(); BuildUI();
                        });
                        KG->addWidget(KeyDel, k, 2); k++;
                    }
                    QPushButton * AddKey = new QPushButton("+ Add Key", KeysBox);
                    QObject::connect(AddKey, &QPushButton::clicked, this, [this, n, j](){
                        (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["KEYVALUES"]["new_key"] = ""; Model->SaveNodes(); BuildUI();
                    });
                    KG->addWidget(AddKey, k, 0, 1, -1);
                    LG->addWidget(KeysBox, 3, 0, 1, -1);
                }
                else if (LayerType == "DllOverride")
                {
                    LG->addWidget(new QLabel("DLLOVERRIDE:"), 1, 0);
                    QLineEdit * DLL = new QLineEdit(LBox);
                    DLL->setProperty("JSONPath", LPath + "/DLLOVERRIDE");
                    if (Layers[j].contains("DLLOVERRIDE") && Layers[j]["DLLOVERRIDE"].is_string()) DLL->setText(QString::fromStdString(std::string(Layers[j]["DLLOVERRIDE"])));
                    QObject::connect(DLL, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(DLL, 1, 1);
                }
                else if (LayerType == "FileEdit")
                {
                    std::string Mode = Layers[j].value("MODE", std::string("ConfigWrite"));
                    QStringList Fields = (Mode == "Overwrite") ? QStringList{"MODE", "FILE", "VALUE"} : QStringList{"MODE", "FILE", "KEY", "VALUE"};
                    int ferow = 1;
                    for (const QString &Field : Fields)
                    {
                        LG->addWidget(new QLabel(Field + ":"), ferow, 0);
                        QLineEdit * FE = new QLineEdit(LBox);
                        FE->setProperty("JSONPath", LPath + "/" + Field);
                        if (Layers[j].contains(Field.toStdString()) && Layers[j][Field.toStdString()].is_string()) FE->setText(QString::fromStdString(std::string(Layers[j][Field.toStdString()])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        LG->addWidget(FE, ferow, 1); ferow++;
                    }
                }
                else if (LayerType == "PersistDir" || LayerType == "PersistFile")
                {
                    LG->addWidget(new QLabel("PATH:"), 1, 0);
                    QLineEdit * PathField = new QLineEdit(LBox);
                    PathField->setPlaceholderText(LayerType == "PersistDir" ? "drive_c/users/steamuser/Saved Games/<game>" : "drive_c/users/steamuser/Documents/<game>/config.ini");
                    PathField->setProperty("JSONPath", LPath + "/PATH");
                    if (Layers[j].contains("PATH") && Layers[j]["PATH"].is_string()) PathField->setText(QString::fromStdString(std::string(Layers[j]["PATH"])));
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(PathField, 1, 1, 1, 2);
                }
                else if (LayerType == "RegPersist")
                {
                    QLabel * Hint = new QLabel("Persists the whole prefix registry (user/system/userdef.reg). For a single key, use RegKeyPersist.", LBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;"); Hint->setWordWrap(true);
                    LG->addWidget(Hint, 1, 1, 1, 2);
                }
                else if (LayerType == "RegKeyPersist")
                {
                    LG->addWidget(new QLabel("REGPATH:"), 1, 0);
                    QLineEdit * RegPath = new QLineEdit(LBox);
                    RegPath->setPlaceholderText("HKCU\\Software\\<vendor>\\<game>\\Saves");
                    RegPath->setProperty("JSONPath", LPath + "/REGPATH");
                    if (Layers[j].contains("REGPATH") && Layers[j]["REGPATH"].is_string()) RegPath->setText(QString::fromStdString(std::string(Layers[j]["REGPATH"])));
                    QObject::connect(RegPath, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    LG->addWidget(RegPath, 1, 1, 1, 2);
                }
                else if (LayerType == "CustomVar")
                {
                    std::string VarType = Layers[j].value("VARTYPE", std::string("string"));
                    int cvrow = 1;
                    QLineEdit * DefaultField = nullptr;
                    for (const auto &FK : std::vector<std::string>{"KEY", "LABEL", "DEFAULT"})
                    {
                        LG->addWidget(new QLabel(QString::fromStdString(FK) + ":"), cvrow, 0);
                        QLineEdit * FE = new QLineEdit(LBox);
                        FE->setProperty("JSONPath", LPath + "/" + QString::fromStdString(FK));
                        if (Layers[j].contains(FK) && Layers[j][FK].is_string()) FE->setText(QString::fromStdString(std::string(Layers[j][FK])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        LG->addWidget(FE, cvrow, 1, 1, 2);
                        if (FK == "DEFAULT") DefaultField = FE;
                        cvrow++;
                    }
                    {
                        QLabel * Hint = new QLabel(LBox); Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                        std::string VT = VarType;
                        auto Update = [Hint, VT](const QString &Raw){
                            std::string Tr = VarSubst::TranslateCustomVarValue(Raw.toStdString(), VT);
                            Hint->setText(Tr == Raw.toStdString() ? QString() : QString("→ stored as  %1").arg(QString::fromStdString(Tr)));
                        };
                        if (DefaultField) { Update(DefaultField->text()); QObject::connect(DefaultField, &QLineEdit::textChanged, this, [Update](const QString &T){ Update(T); }); }
                        LG->addWidget(Hint, cvrow, 1, 1, 2); cvrow++;
                    }
                    LG->addWidget(new QLabel("VARTYPE:"), cvrow, 0);
                    QComboBox * VT = new QComboBox(LBox);
                    VT->addItems({"string", "number", "dword", "qword", "bool", "options", "random"});
                    VT->setCurrentText(QString::fromStdString(VarType));
                    QObject::connect(VT, &QComboBox::currentIndexChanged, this, [this, n, j, VT](){
                        (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["VARTYPE"] = VT->currentText().toStdString(); Model->SaveNodes(); BuildUI();
                    });
                    LG->addWidget(VT, cvrow, 1, 1, 2); cvrow++;

                    LG->addWidget(new QLabel("DISPLAY:"), cvrow, 0);
                    QCheckBox * Disp = new QCheckBox(LBox);
                    Disp->setChecked(Layers[j].value("DISPLAY", true));
                    QObject::connect(Disp, &QCheckBox::toggled, this, [this, n, j](bool On){
                        (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["DISPLAY"] = On; Model->SaveNodes();
                    });
                    LG->addWidget(Disp, cvrow, 1, 1, 2); cvrow++;

                    if (VarType == "options" || VarType == "random")
                    {
                        if (!Layers[j].contains("OPTIONS") || !Layers[j]["OPTIONS"].is_array())
                            (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["OPTIONS"] = json::array();
                        bool IsRandom = (VarType == "random");
                        QGroupBox * OptsBox = new QGroupBox(IsRandom ? "Value Pool" : "OPTIONS", LBox);
                        QVBoxLayout * OptsLayout = new QVBoxLayout(OptsBox);
                        auto &Opts = (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["OPTIONS"];
                        for (int oi = 0; oi < (int)Opts.size(); oi++)
                        {
                            QHBoxLayout * OptRow = new QHBoxLayout();
                            if (!IsRandom)
                            {
                                QLineEdit * OptLabel = new QLineEdit(OptsBox); OptLabel->setPlaceholderText("Label");
                                OptLabel->setProperty("JSONPath", QString("%1/OPTIONS/%2/LABEL").arg(LPath).arg(oi));
                                if (Opts[oi].contains("LABEL") && Opts[oi]["LABEL"].is_string()) OptLabel->setText(QString::fromStdString(std::string(Opts[oi]["LABEL"])));
                                QObject::connect(OptLabel, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                                OptRow->addWidget(OptLabel);
                            }
                            QLineEdit * OptValue = new QLineEdit(OptsBox); OptValue->setPlaceholderText("Value");
                            OptValue->setProperty("JSONPath", QString("%1/OPTIONS/%2/VALUE").arg(LPath).arg(oi));
                            if (Opts[oi].contains("VALUE") && Opts[oi]["VALUE"].is_string()) OptValue->setText(QString::fromStdString(std::string(Opts[oi]["VALUE"])));
                            QObject::connect(OptValue, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QPushButton * OptDel = new QPushButton("✕", OptsBox); OptDel->setFixedWidth(28);
                            QObject::connect(OptDel, &QPushButton::clicked, this, [this, n, j, oi](){
                                (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["OPTIONS"].erase(oi); Model->SaveNodes(); BuildUI();
                            });
                            OptRow->addWidget(OptValue); OptRow->addWidget(OptDel); OptsLayout->addLayout(OptRow);
                        }
                        QPushButton * AddOpt = new QPushButton(IsRandom ? "+ Add Value" : "+ Add Option", OptsBox);
                        QObject::connect(AddOpt, &QPushButton::clicked, this, [this, n, j, IsRandom](){
                            if (IsRandom) (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["OPTIONS"].push_back(json::object({{"VALUE", ""}}));
                            else          (*MANIFESTJSON)["NODES"][n]["LAYERS"][j]["OPTIONS"].push_back(json::object({{"LABEL", ""}, {"VALUE", ""}}));
                            Model->SaveNodes(); BuildUI();
                        });
                        OptsLayout->addWidget(AddOpt);
                        LG->addWidget(OptsBox, cvrow, 0, 1, -1); cvrow++;
                    }
                    QLabel * Hint = new QLabel("Reference as  %" + QString::fromStdString(Layers[j].value("KEY", std::string("KEY"))) + "%", LBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                    LG->addWidget(Hint, cvrow, 0, 1, -1);
                }
                else
                {
                    LG->addWidget(new QLabel("(unrecognised layer type)"), 1, 0, 1, -1);
                }
                Form->addRow(LBox);
            }
            Body->addWidget(Box);
        }

        Body->addStretch();

        const QString TabLabel = NodeIdStr.empty() ? QString("node %1").arg(n + 1) : QString::fromStdString(NodeIdStr);
        PackageEditorTabWidget->addTab(NodeTab, TabLabel);
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

void PackageEditor::JSONQLineEditChanged()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(QObject::sender());
    if (!Editor) return;
    nlohmann::ordered_json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer] = Editor->text().toStdString();
    Model->SaveNodes();
   
}



// ============================================================================
// LAYERS — add-actions
// ============================================================================

void PackageEditor::AppendLayer(QObject * Sender, const nlohmann::ordered_json & Layer)
{
    const QString JSONPath = ResolveNodeJSONPath(Sender);
    if (JSONPath.isEmpty()) return;
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    if (!(*MANIFESTJSON)[JSONPointer].contains("LAYERS") || !(*MANIFESTJSON)[JSONPointer]["LAYERS"].is_array())
        (*MANIFESTJSON)[JSONPointer]["LAYERS"] = json::array();
    (*MANIFESTJSON)[JSONPointer]["LAYERS"].push_back(Layer);
    Model->SaveNodes(); BuildUI();
}

QString PackageEditor::ImportLayerFile(const QString & Selected, bool IsDir)
{
    const QString Name = QFileInfo(Selected).fileName();
    const QString Dest = QDir::cleanPath(PackageDir->path() + QDir::separator() + Name);

    if (QFileInfo::exists(Dest))
    {
        QMessageBox::warning(this, "Name collision",
            "“" + Name + "” already exists in the bundle directory.\nRename or remove it first, then re-add.");
        return QString();
    }

    QMessageBox Box(this);
    Box.setWindowTitle("Add layer");
    Box.setText("Bring “" + Name + "” into the bundle?");
    Box.setInformativeText("Copy keeps the original where it is; Move relocates it into the bundle.");
    QPushButton * CopyBtn = Box.addButton("Copy", QMessageBox::AcceptRole);
    QPushButton * MoveBtn = Box.addButton("Move", QMessageBox::DestructiveRole);
    Box.addButton(QMessageBox::Cancel);
    Box.setDefaultButton(CopyBtn);
    Box.exec();
    QAbstractButton * Clicked = Box.clickedButton();
    if (Clicked != CopyBtn && Clicked != MoveBtn) return QString();

    std::error_code Ec;
    const std::filesystem::path Src(Selected.toStdString()), Dst(Dest.toStdString());
    if (Clicked == MoveBtn)
    {
        std::filesystem::rename(Src, Dst, Ec);
        if (Ec) { std::filesystem::copy(Src, Dst, std::filesystem::copy_options::recursive, Ec);
                  if (!Ec) std::filesystem::remove_all(Src, Ec); }
    }
    else
    {
        std::filesystem::copy(Src, Dst, IsDir ? std::filesystem::copy_options::recursive : std::filesystem::copy_options::none, Ec);
    }
    if (Ec) { QMessageBox::critical(this, "Add layer", "Could not bring the file in:\n" + QString::fromStdString(Ec.message())); return QString(); }
    return Name;
}

void PackageEditor::AddVFSDirLayer()
{
    QObject * Sender = QObject::sender();
    QString Selected = QFileDialog::getExistingDirectory(this, "Select directory to add as VFSDirLayer");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, true);
    if (Name.isEmpty()) return;
    AppendLayer(Sender, json::object({{"TYPE", "VFSDirLayer"}, {"PATH", Name.toStdString()}}));
}

void PackageEditor::AddVFSZipLayer()
{
    QObject * Sender = QObject::sender();
    QString Selected = QFileDialog::getOpenFileName(this, "Select ZIP file to add as VFSZipLayer", "", "ZIP files (*.zip)");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, false);
    if (Name.isEmpty()) return;
    AppendLayer(Sender, json::object({{"TYPE", "VFSZipLayer"}, {"PATH", Name.toStdString()}}));
}

void PackageEditor::AddVFSFileLayer()
{
    QObject * Sender = QObject::sender();
    QString Selected = QFileDialog::getOpenFileName(this, "Select file to add as VFSFileLayer");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, false);
    if (Name.isEmpty()) return;
    AppendLayer(Sender, json::object({{"TYPE", "VFSFileLayer"}, {"PATH", Name.toStdString()}}));
}

void PackageEditor::AddRegEdit()
{
    AppendLayer(QObject::sender(), json::object({ {"TYPE", "RegEdit"}, {"REGPATH", ""}, {"ARCHITECTURE", "32"}, {"KEYVALUES", json::object()} }));
}
void PackageEditor::AddDllOverride()
{
    AppendLayer(QObject::sender(), json::object({ {"TYPE", "DllOverride"}, {"DLLOVERRIDE", ""} }));
}
void PackageEditor::AddFileEdit()
{
    AppendLayer(QObject::sender(), json::object({ {"TYPE", "FileEdit"}, {"MODE", "ConfigWrite"}, {"FILE", ""}, {"KEY", ""}, {"VALUE", ""} }));
}
void PackageEditor::AddCustomVar()
{
    AppendLayer(QObject::sender(), json::object({ {"TYPE", "CustomVar"}, {"KEY", ""}, {"LABEL", ""}, {"DEFAULT", ""}, {"VARTYPE", "string"}, {"DISPLAY", true} }));
}
void PackageEditor::AddPersistDir()  { AppendLayer(QObject::sender(), json::object({ {"TYPE", "PersistDir"},  {"PATH", ""} })); }
void PackageEditor::AddPersistFile() { AppendLayer(QObject::sender(), json::object({ {"TYPE", "PersistFile"}, {"PATH", ""} })); }
void PackageEditor::AddRegPersist()  { AppendLayer(QObject::sender(), json::object({ {"TYPE", "RegPersist"} })); }
void PackageEditor::AddRegKeyPersist(){ AppendLayer(QObject::sender(), json::object({ {"TYPE", "RegKeyPersist"}, {"REGPATH", ""} })); }

// ============================================================================
// Authoring execute (native node engine)
// ============================================================================

std::string PackageEditor::NodeIdOfSender(QObject * Sender) const
{
    const QString NodePtr = ResolveNodeJSONPath(Sender);
    if (NodePtr.isEmpty()) return "";
    try {
        auto &N = MANIFESTJSON->at(nlohmann::ordered_json::json_pointer(NodePtr.toStdString()));
        return N.value("NODE_ID", std::string());
    } catch (...) { return ""; }
}




// ============================================================================
// META — cover drop
// ============================================================================

void PackageEditor::ApplyCoverImage(QLabel *CoverLabel, const QByteArray &Data, const QString &Extension, int NodeIndexInArray)
{
    if (Data.isEmpty() || NodeIndexInArray < 0 || NodeIndexInArray >= (int)(*MANIFESTJSON)["NODES"].size()) return;

    const std::string NodeId = (*MANIFESTJSON)["NODES"][NodeIndexInArray].value("NODE_ID", std::string());
    QString FileName = (NodeId.empty() ? QString("node%1").arg(NodeIndexInArray) : QString::fromStdString(NodeId)) + "_cover." + Extension.toLower();
    QString DestPath = QDir::cleanPath(PackageDir->path() + "/" + FileName);

    QFile OutFile(DestPath);
    if (!OutFile.open(QIODevice::WriteOnly)) { LogErr("PackageEditor", "Could not write cover: " + DestPath.toStdString()); return; }
    OutFile.write(Data); OutFile.close();

    (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(QString("/NODES/%1/META/COVER").arg(NodeIndexInArray).toStdString())] = FileName.toStdString();
    Model->SaveNodes();

    QPixmap Pix; Pix.loadFromData(Data);
    if (!Pix.isNull()) CoverLabel->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    LogSucc("PackageEditor", "Cover set: " + FileName.toStdString());
}

bool PackageEditor::eventFilter(QObject *obj, QEvent *event)
{
    QLabel *CoverLabel = qobject_cast<QLabel*>(obj);
    if (!CoverLabel || !CoverLabel->property("NodeArrayIndex").isValid())
        return QDialog::eventFilter(obj, event);

    if (event->type() == QEvent::DragEnter)
    {
        QDragEnterEvent *ev = static_cast<QDragEnterEvent*>(event);
        if (ev->mimeData()->hasImage() || ev->mimeData()->hasUrls() || ev->mimeData()->hasText()) ev->acceptProposedAction();
        return true;
    }
    if (event->type() == QEvent::DragMove) { static_cast<QDragMoveEvent*>(event)->acceptProposedAction(); return true; }

    if (event->type() == QEvent::Drop)
    {
        QDropEvent *ev = static_cast<QDropEvent*>(event);
        int Idx = CoverLabel->property("NodeArrayIndex").toInt();
        const QMimeData *Mime = ev->mimeData();

        if (Mime->hasImage())
        {
            QImage Img = qvariant_cast<QImage>(Mime->imageData());
            QByteArray Data; QBuffer Buf(&Data); Buf.open(QIODevice::WriteOnly); Img.save(&Buf, "PNG");
            ApplyCoverImage(CoverLabel, Data, "png", Idx);
            return true;
        }
        if (Mime->hasUrls())
        {
            QUrl Url = Mime->urls().first();
            if (Url.isLocalFile())
            {
                QFile F(Url.toLocalFile());
                if (F.open(QIODevice::ReadOnly))
                {
                    QByteArray Data = F.readAll();
                    QString Ext = QFileInfo(Url.toLocalFile()).suffix(); if (Ext.isEmpty()) Ext = "png";
                    ApplyCoverImage(CoverLabel, Data, Ext, Idx);
                }
            }
            else
            {
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply *Reply = NetMgr->get(QNetworkRequest(Url));
                QEventLoop Loop; QObject::connect(Reply, &QNetworkReply::finished, &Loop, &QEventLoop::quit); Loop.exec();
                if (Reply->error() == QNetworkReply::NoError)
                {
                    QByteArray Data = Reply->readAll();
                    QString Ext = QFileInfo(Url.path()).suffix();
                    if (Ext.isEmpty())
                    {
                        QString CT = Reply->header(QNetworkRequest::ContentTypeHeader).toString();
                        if (CT.contains("jpeg") || CT.contains("jpg")) Ext = "jpg";
                        else if (CT.contains("webp")) Ext = "webp"; else Ext = "png";
                    }
                    ApplyCoverImage(CoverLabel, Data, Ext, Idx);
                }
                else LogErr("PackageEditor", "Failed to download cover: " + Reply->errorString().toStdString());
                Reply->deleteLater();
            }
            return true;
        }
        if (Mime->hasText())
        {
            QUrl Url(Mime->text().trimmed());
            if (Url.isValid() && (Url.scheme() == "http" || Url.scheme() == "https"))
            {
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply *Reply = NetMgr->get(QNetworkRequest(Url));
                QEventLoop Loop; QObject::connect(Reply, &QNetworkReply::finished, &Loop, &QEventLoop::quit); Loop.exec();
                if (Reply->error() == QNetworkReply::NoError)
                {
                    QByteArray Data = Reply->readAll();
                    QString Ext = QFileInfo(Url.path()).suffix(); if (Ext.isEmpty()) Ext = "png";
                    ApplyCoverImage(CoverLabel, Data, Ext, Idx);
                }
                Reply->deleteLater();
            }
            return true;
        }
        return true;
    }
    return QDialog::eventFilter(obj, event);
}


