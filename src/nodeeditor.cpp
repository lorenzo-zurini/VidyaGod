#include "nodeeditor.h"
#include "packageeditormodel.h"
#include "nodesections.h"   // the per-node section widgets this composes
#include "authoringsessionwindow.h"   // "Author in runtime…" → the held-open authoring session

#include <QAbstractButton>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <filesystem>
#include <string>

using json = nlohmann::ordered_json;

// The META metadata fields rendered as a flat form under TITLE (same set the old BuildUI used).
static const std::vector<std::string> MetadataFields = {
    "RELEASEDATE", "EDITION", "EDITIONDATE", "DEVELOPER", "PUBLISHER",
    "TGDBID", "STEAMAPPID", "GOGPRODUCTID", "UMUID",
    "SERIES", "SERIESSORTNUMBER", "SUBSERIES", "SUBSERIESSORTNUMBER",
    "EDITOR", "ONLINEDRM",
    "NETWORKMULTIPLAYER", "DIRECTCONNECT", "LANMULTIPLAYER", "ONLINEMULTIPLAYER",
    "NETWORKCOOP", "LOCALMULTIPLAYER", "LOCALCOOP", "OTHERONLINEFEATURES"
};

NodeEditor::NodeEditor(PackageEditorModel * model, int nodeArrayIndex, QWidget * parent)
    : QWidget(parent), Model(model), N(nodeArrayIndex)
{
    const int n = N;   // the toolbar lambdas below were written against `n`; alias it to this node's index
        const std::string NodeIdStr = Model->doc()["NODES"][n].value("NODE_ID", std::string());

        QVBoxLayout * NodeTabLayout = new QVBoxLayout(this);
        this->setLayout(NodeTabLayout);

        // Toolbar: authoring tools + add-layer menu + remove/move.
        QHBoxLayout * Toolbar = new QHBoxLayout();
        NodeTabLayout->addLayout(Toolbar);

        // Author in runtime: open a held-open AuthoringSession on this node's closure (run installers / regedit and
        // capture the write-delta + registry into the bundle). Replaces the old fire-and-forget Run EXE / Browse /
        // Edit Registry / Analyze (which the pristine PERSIST default broke — those wiped before capture).
        QPushButton * AuthorBtn = new QPushButton("Author in runtime…", this);
        QPushButton * ExecBtn   = new QPushButton("Test launch", this);
        Toolbar->addWidget(AuthorBtn); Toolbar->addWidget(ExecBtn);
        QObject::connect(AuthorBtn, &QPushButton::clicked, this, [this, NodeIdStr](){
            (new AuthoringSessionWindow(Model, NodeIdStr, this))->show();
        });
        QObject::connect(ExecBtn, &QPushButton::clicked, this, [this, NodeIdStr](){ Model->RunInNode(NodeIdStr, ""); });

        Toolbar->addStretch();

        QToolButton * AddLayerBtn = new QToolButton(this);
        AddLayerBtn->setText("+ Layer");
        AddLayerBtn->setPopupMode(QToolButton::InstantPopup);
        QMenu * LayerMenu = new QMenu(AddLayerBtn);
        LayerMenu->addAction("VFSZipLayer",   this, &NodeEditor::AddVFSZipLayer);
        LayerMenu->addAction("VFSDirLayer",   this, &NodeEditor::AddVFSDirLayer);
        LayerMenu->addAction("VFSFileLayer",  this, &NodeEditor::AddVFSFileLayer);
        LayerMenu->addSeparator();
        LayerMenu->addAction("RegEdit",       this, &NodeEditor::AddRegEdit);
        LayerMenu->addAction("DllOverride",   this, &NodeEditor::AddDllOverride);
        LayerMenu->addAction("FileEdit",      this, &NodeEditor::AddFileEdit);
        LayerMenu->addSeparator();
        LayerMenu->addAction("Persist",       this, &NodeEditor::AddPersist);
        LayerMenu->addSeparator();
        LayerMenu->addAction("CustomVar",     this, &NodeEditor::AddCustomVar);
        AddLayerBtn->setMenu(LayerMenu);
        Toolbar->addWidget(AddLayerBtn);

        QPushButton * MoveUpBtn = new QPushButton("↑", this); MoveUpBtn->setFixedWidth(30); MoveUpBtn->setEnabled(n > 0);
        QPushButton * MoveDnBtn = new QPushButton("↓", this); MoveDnBtn->setFixedWidth(30);
        MoveDnBtn->setEnabled(n < (int)Model->doc()["NODES"].size() - 1);
        QPushButton * RemoveBtn = new QPushButton("Remove Node", this);
        Toolbar->addWidget(MoveUpBtn); Toolbar->addWidget(MoveDnBtn); Toolbar->addWidget(RemoveBtn);
        QObject::connect(MoveUpBtn, &QPushButton::clicked, this, [this, n](){
            if (n <= 0) return;
            std::swap(Model->doc()["NODES"][n - 1], Model->doc()["NODES"][n]);
            Model->SaveNodes(); Model->requestReload();
        });
        QObject::connect(MoveDnBtn, &QPushButton::clicked, this, [this, n](){
            auto &A = Model->doc()["NODES"]; if (n + 1 >= (int)A.size()) return;
            std::swap(A[n], A[n + 1]); Model->SaveNodes(); Model->requestReload();
        });
        QObject::connect(RemoveBtn, &QPushButton::clicked, this, [this, n, NodeIdStr](){
            if (QMessageBox::question(this, "Remove node",
                    "Delete node “" + QString::fromStdString(NodeIdStr) + "” and its file?") != QMessageBox::Yes) return;
            Model->doc()["NODES"].erase(n); Model->SaveNodes(); Model->requestReload();
        });

        QScrollArea * Scroll = new QScrollArea(this);
        Scroll->setWidgetResizable(true);
        Scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        NodeTabLayout->addWidget(Scroll);
        QWidget * Contents = new QWidget();
        QVBoxLayout * Body = new QVBoxLayout(Contents);
        Contents->setLayout(Body);
        Scroll->setWidget(Contents);

        // ---- sections (each its own NodeSection widget; all edit through the model) ----
        Body->addWidget(new NodeIdentitySection(Model, n, Contents));
        Body->addWidget(new NodeSelectionSection(Model, n, Contents));
        Body->addWidget(new NodeParentsSection(Model, n, Contents));
        Body->addWidget(new NodePlatformSection(Model, n, Contents));
        Body->addWidget(new NodeExecSection(Model, n, Contents));
        Body->addWidget(new NodeMetaSection(Model, n, Contents));
        Body->addWidget(new NodeLayersSection(Model, n, Contents));
        Body->addStretch();
}

// ============================================================================
// Field edit + LAYERS add-actions
// ============================================================================


void NodeEditor::appendLayer(const json & Layer)
{
    auto & Node = Model->doc()["NODES"][N];
    if (!Node.contains("LAYERS") || !Node["LAYERS"].is_array()) Node["LAYERS"] = json::array();
    Node["LAYERS"].push_back(Layer);
    Model->SaveNodes();
    Model->requestReload();
}

QString NodeEditor::ImportLayerFile(const QString & Selected, bool IsDir)
{
    const QString Name = QFileInfo(Selected).fileName();
    const QString Dest = QDir::cleanPath(Model->packageDir()->path() + QDir::separator() + Name);

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

void NodeEditor::AddVFSDirLayer()
{
    QString Selected = QFileDialog::getExistingDirectory(this, "Select directory to add as VFSDirLayer");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, true);
    if (Name.isEmpty()) return;
    appendLayer(json::object({{"TYPE", "VFSDirLayer"}, {"PATH", Name.toStdString()}}));
}

void NodeEditor::AddVFSZipLayer()
{
    QString Selected = QFileDialog::getOpenFileName(this, "Select ZIP file to add as VFSZipLayer", "", "ZIP files (*.zip)");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, false);
    if (Name.isEmpty()) return;
    appendLayer(json::object({{"TYPE", "VFSZipLayer"}, {"PATH", Name.toStdString()}}));
}

void NodeEditor::AddVFSFileLayer()
{
    QString Selected = QFileDialog::getOpenFileName(this, "Select file to add as VFSFileLayer");
    if (Selected.isEmpty()) return;
    QString Name = ImportLayerFile(Selected, false);
    if (Name.isEmpty()) return;
    appendLayer(json::object({{"TYPE", "VFSFileLayer"}, {"PATH", Name.toStdString()}}));
}

void NodeEditor::AddRegEdit()      { appendLayer(json::object({ {"TYPE", "RegEdit"}, {"REGPATH", ""}, {"ARCHITECTURE", "32"}, {"KEYVALUES", json::object()} })); }
void NodeEditor::AddDllOverride()  { appendLayer(json::object({ {"TYPE", "DllOverride"}, {"DLLOVERRIDE", ""} })); }
void NodeEditor::AddFileEdit()     { appendLayer(json::object({ {"TYPE", "FileEdit"}, {"MODE", "ConfigWrite"}, {"FILE", ""}, {"KEY", ""}, {"VALUE", ""} })); }
void NodeEditor::AddCustomVar()    { appendLayer(json::object({ {"TYPE", "CustomVar"}, {"KEY", ""}, {"DEFAULT", ""} })); }   // a binding by default; tick "User option" to add a UI facet
void NodeEditor::AddPersist()      { appendLayer(json::object({ {"TYPE", "Persist"}, {"KEEP", ""} })); }

// ============================================================================
// META — cover drop
// ============================================================================


