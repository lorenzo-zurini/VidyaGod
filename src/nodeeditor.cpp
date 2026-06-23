#include "nodeeditor.h"
#include "packageeditormodel.h"
#include "covercache.h"
#include "processenv.h"   // RunCommand (zip/unzip layer conversions)
#include "varsubst.h"     // VarSubst::TranslateCustomVarValue (CustomVar "stored as" hint)
#include "commonutils.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <string>
#include <vector>

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
    const int n = N;   // the per-node loop body below was written against `n`; alias it to this node's index
        auto &NodeRef = Model->doc()["NODES"][n];
        const std::string NodeIdStr = NodeRef.value("NODE_ID", std::string());
        const std::string NodePtr   = "/NODES/" + std::to_string(n);

        QVBoxLayout * NodeTabLayout = new QVBoxLayout(this);
        this->setLayout(NodeTabLayout);

        // Toolbar: authoring tools + add-layer menu + remove/move.
        QHBoxLayout * Toolbar = new QHBoxLayout();
        NodeTabLayout->addLayout(Toolbar);

        QPushButton * RunExeBtn = new QPushButton("Run EXE", this);
        QPushButton * BrowseBtn = new QPushButton("Browse", this);
        QPushButton * RegBtn    = new QPushButton("Edit Registry", this);
        QPushButton * ExecBtn   = new QPushButton("Execute", this);
        QPushButton * AnalyzeBtn= new QPushButton("Analyze Registry", this);
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
        LayerMenu->addAction("PersistDir",    this, &NodeEditor::AddPersistDir);
        LayerMenu->addAction("PersistFile",   this, &NodeEditor::AddPersistFile);
        LayerMenu->addAction("RegPersist",    this, &NodeEditor::AddRegPersist);
        LayerMenu->addAction("RegKeyPersist", this, &NodeEditor::AddRegKeyPersist);
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

        // ---- Identity (NODE_ID / ROLE / UID / GROUP / LABEL / RECOMMENDED) ----
        {
            QGroupBox * Box = new QGroupBox("Identity", Contents);
            QFormLayout * Form = new QFormLayout(Box);

            QLineEdit * IdField = new QLineEdit(Box);
            IdField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/NODE_ID"));
            IdField->setText(QString::fromStdString(NodeIdStr));
            QObject::connect(IdField, &QLineEdit::editingFinished, this, [this](){
                onFieldEdited(); Model->requestReload();   // renames re-file the node + relabel the tab
            });
            Form->addRow("NODE_ID", IdField);

            QComboBox * RoleCombo = new QComboBox(Box);
            RoleCombo->addItems({"content", "launchable", "runner"});
            RoleCombo->setCurrentText(QString::fromStdString(NodeRef.value("ROLE", std::string("content"))));
            QObject::connect(RoleCombo, &QComboBox::currentTextChanged, this, [this, NodePtr](const QString &T){
                Model->doc()[json::json_pointer(NodePtr + "/ROLE")] = T.toStdString();
                Model->SaveNodes(); Model->requestReload();   // re-render so role-specific sections (GROUP/LABEL/GUEST) adapt
            });
            Form->addRow("ROLE", RoleCombo);

            QLineEdit * UidField = new QLineEdit(Box);
            UidField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/UID"));
            if (NodeRef.contains("UID") && NodeRef["UID"].is_string()) UidField->setText(QString::fromStdString(std::string(NodeRef["UID"])));
            QObject::connect(UidField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
            Form->addRow("UID", UidField);

            const bool Launchable = NodeRef.value("ROLE", std::string()) == "launchable";
            if (Launchable)
            {
                QLineEdit * GroupField = new QLineEdit(Box);
                GroupField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/GROUP"));
                GroupField->setPlaceholderText("library-tile grouping key (defaults to NODE_ID)");
                if (NodeRef.contains("GROUP") && NodeRef["GROUP"].is_string()) GroupField->setText(QString::fromStdString(std::string(NodeRef["GROUP"])));
                QObject::connect(GroupField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                Form->addRow("GROUP", GroupField);

                QLineEdit * LabelField = new QLineEdit(Box);
                LabelField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/LABEL"));
                LabelField->setPlaceholderText("edition label for the picker dropdown");
                if (NodeRef.contains("LABEL") && NodeRef["LABEL"].is_string()) LabelField->setText(QString::fromStdString(std::string(NodeRef["LABEL"])));
                QObject::connect(LabelField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                Form->addRow("LABEL", LabelField);

                QCheckBox * RecChk = new QCheckBox("Recommended (default edition in its GROUP)", Box);
                RecChk->setChecked(NodeRef.value("RECOMMENDED", false));
                QObject::connect(RecChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                    Model->doc()[json::json_pointer(NodePtr + "/RECOMMENDED")] = On; Model->SaveNodes();
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
                Model->doc()[json::json_pointer(NodePtr + "/OPTIONAL")] = On; Model->SaveNodes();
            });
            BL->addWidget(OptChk);

            QCheckBox * DefChk = new QCheckBox("DEFAULT (enabled by default when optional)", Box);
            DefChk->setChecked(NodeRef.value("DEFAULT", true));
            QObject::connect(DefChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                Model->doc()[json::json_pointer(NodePtr + "/DEFAULT")] = On; Model->SaveNodes();
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
                if (Arr.empty()) Model->doc()[json::json_pointer(NodePtr)].erase("EXCLUDE");
                else             Model->doc()[json::json_pointer(NodePtr + "/EXCLUDE")] = Arr;
                Model->SaveNodes();
            });
            BL->addWidget(ExcludeField);
            Body->addWidget(Box);
        }

        // ---- PARENTS (global catalog-wide id picker; load order: later = higher priority) ----
        {
            const std::string PPtr = NodePtr + "/PARENTS";
            if (!NodeRef.contains("PARENTS") || !NodeRef["PARENTS"].is_array())
                Model->doc()[json::json_pointer(PPtr)] = json::array();
            auto &Parents = Model->doc()[json::json_pointer(PPtr)];

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
                    Model->doc()[json::json_pointer(ItemPtr)] = T.trimmed().toStdString(); Model->SaveNodes();
                });
                QPushButton * Up = new QPushButton("▲", Box); Up->setFixedWidth(28);
                QPushButton * Dn = new QPushButton("▼", Box); Dn->setFixedWidth(28);
                QPushButton * Del = new QPushButton("✕", Box); Del->setFixedWidth(28);
                QObject::connect(Up, &QPushButton::clicked, this, [this, PPtr, k](){
                    auto &A = Model->doc()[json::json_pointer(PPtr)]; if (k <= 0) return; std::swap(A[k - 1], A[k]); Model->SaveNodes(); Model->requestReload();
                });
                QObject::connect(Dn, &QPushButton::clicked, this, [this, PPtr, k](){
                    auto &A = Model->doc()[json::json_pointer(PPtr)]; if (k + 1 >= (int)A.size()) return; std::swap(A[k], A[k + 1]); Model->SaveNodes(); Model->requestReload();
                });
                QObject::connect(Del, &QPushButton::clicked, this, [this, PPtr, k](){
                    Model->doc()[json::json_pointer(PPtr)].erase(k); Model->SaveNodes(); Model->requestReload();
                });
                Row->addWidget(Pick, 1); Row->addWidget(Up); Row->addWidget(Dn); Row->addWidget(Del);
                BL->addLayout(Row);
            }
            QPushButton * AddParent = new QPushButton("+ Add Parent", Box);
            QObject::connect(AddParent, &QPushButton::clicked, this, [this, PPtr](){
                Model->doc()[json::json_pointer(PPtr)].push_back(""); Model->SaveNodes(); Model->requestReload();
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
                if (Tr.isEmpty()) { if (Model->doc().contains(json::json_pointer(NodePtr + "/PLATFORM"))) Model->doc()[json::json_pointer(NodePtr + "/PLATFORM")].erase("HOST"); }
                else Model->doc()[json::json_pointer(NodePtr + "/PLATFORM/HOST")] = Tr.toStdString();
                Model->SaveNodes();
            });
            HostRow->addWidget(Host, 1);
            BL->addLayout(HostRow);

            // GUEST list (runner nodes serve these guest platforms).
            const std::string GPtr = NodePtr + "/PLATFORM/GUEST";
            const bool IsRunner = NodeRef.value("ROLE", std::string()) == "runner";
            if (IsRunner)
            {
                if (!Model->doc().contains(json::json_pointer(GPtr)) || !Model->doc()[json::json_pointer(GPtr)].is_array())
                    Model->doc()[json::json_pointer(GPtr)] = json::array();
                auto &Guests = Model->doc()[json::json_pointer(GPtr)];
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
                        Model->doc()[json::json_pointer(ItemPtr)] = T.trimmed().toStdString(); Model->SaveNodes();
                    });
                    QPushButton * Del = new QPushButton("✕", GBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, GPtr, k](){
                        Model->doc()[json::json_pointer(GPtr)].erase(k); Model->SaveNodes(); Model->requestReload();
                    });
                    Row->addWidget(CB, 1); Row->addWidget(Del); GBL->addLayout(Row);
                }
                QPushButton * AddG = new QPushButton("+ Add Guest", GBox);
                QObject::connect(AddG, &QPushButton::clicked, this, [this, GPtr](){
                    Model->doc()[json::json_pointer(GPtr)].push_back(""); Model->SaveNodes(); Model->requestReload();
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
                Model->doc()[json::json_pointer(EPtr)] = json::object();
            auto &Exec = Model->doc()[json::json_pointer(EPtr)];

            QGroupBox * Box = new QGroupBox("EXEC (invocation / content)", Contents);
            QGridLayout * G = new QGridLayout(Box);
            int row = 0;
            for (const char *FK : {"CONTENTPATH", "EXEARGS", "WORKDIR", "EXECUTABLE", "CONTENT_ROOT"})
            {
                G->addWidget(new QLabel(QString(FK) + ":"), row, 0);
                QLineEdit * FE = new QLineEdit(Box);
                FE->setProperty("JSONPath", QString::fromStdString(EPtr + "/" + FK));
                if (Exec.contains(FK) && Exec[FK].is_string()) FE->setText(QString::fromStdString(std::string(Exec[FK])));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                G->addWidget(FE, row, 1, 1, 2); row++;
            }
            QCheckBox * PGen = new QCheckBox("PREFIX_GENERATE (build a wine prefix)", Box);
            PGen->setChecked(Exec.value("PREFIX_GENERATE", false));
            QObject::connect(PGen, &QCheckBox::toggled, this, [this, EPtr](bool On){
                Model->doc()[json::json_pointer(EPtr + "/PREFIX_GENERATE")] = On; Model->SaveNodes();
            });
            G->addWidget(PGen, row, 0, 1, 3); row++;

            // ARGS / REMOVE_ENV string-list editors.
            auto AddList = [&](const QString &Title, const std::string &Key){
                const std::string AP = EPtr + "/" + Key;
                if (!Model->doc().contains(json::json_pointer(AP)) || !Model->doc()[json::json_pointer(AP)].is_array())
                    Model->doc()[json::json_pointer(AP)] = json::array();
                auto &Arr = Model->doc()[json::json_pointer(AP)];
                QGroupBox * LBox = new QGroupBox(Title, Box);
                QVBoxLayout * LBL = new QVBoxLayout(LBox);
                for (int k = 0; k < (int)Arr.size(); k++)
                {
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * LE = new QLineEdit(LBox);
                    LE->setProperty("JSONPath", QString::fromStdString(AP + "/" + std::to_string(k)));
                    LE->setText(QString::fromStdString(Arr[k].is_string() ? std::string(Arr[k]) : ""));
                    QObject::connect(LE, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                    QPushButton * Del = new QPushButton("✕", LBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, AP, k](){
                        Model->doc()[json::json_pointer(AP)].erase(k); Model->SaveNodes(); Model->requestReload();
                    });
                    Row->addWidget(LE, 1); Row->addWidget(Del); LBL->addLayout(Row);
                }
                QPushButton * Add = new QPushButton("+ Add", LBox);
                QObject::connect(Add, &QPushButton::clicked, this, [this, AP](){
                    Model->doc()[json::json_pointer(AP)].push_back(""); Model->SaveNodes(); Model->requestReload();
                });
                LBL->addWidget(Add);
                G->addWidget(LBox, row, 0, 1, -1); row++;
            };
            AddList("ARGS", "ARGS");
            AddList("REMOVE_ENV", "REMOVE_ENV");

            // ENV key/value.
            {
                const std::string EnvPtr = EPtr + "/ENV";
                if (!Model->doc().contains(json::json_pointer(EnvPtr)) || !Model->doc()[json::json_pointer(EnvPtr)].is_object())
                    Model->doc()[json::json_pointer(EnvPtr)] = json::object();
                auto &EnvObj = Model->doc()[json::json_pointer(EnvPtr)];
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
                    QObject::connect(ValField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                    QObject::connect(KeyField, &QLineEdit::editingFinished, this, [this, EnvPtr, KeyCopy, KeyField](){
                        std::string NewKey = KeyField->text().toStdString();
                        if (NewKey == KeyCopy || NewKey.empty()) return;
                        auto &O = Model->doc()[json::json_pointer(EnvPtr)];
                        auto Val = O[KeyCopy]; O.erase(KeyCopy); O[NewKey] = Val; Model->SaveNodes(); Model->requestReload();
                    });
                    QPushButton * Del = new QPushButton("✕", EnvBox); Del->setFixedWidth(28);
                    QObject::connect(Del, &QPushButton::clicked, this, [this, EnvPtr, KeyCopy](){
                        Model->doc()[json::json_pointer(EnvPtr)].erase(KeyCopy); Model->SaveNodes(); Model->requestReload();
                    });
                    Row->addWidget(KeyField); Row->addWidget(ValField); Row->addWidget(Del); EL->addLayout(Row);
                }
                QPushButton * AddEnv = new QPushButton("+ Add Env Var", EnvBox);
                QObject::connect(AddEnv, &QPushButton::clicked, this, [this, EnvPtr](){
                    Model->doc()[json::json_pointer(EnvPtr)]["NEW_KEY"] = ""; Model->SaveNodes(); Model->requestReload();
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
                Model->doc()[json::json_pointer(MPtr)] = json::object();
            auto &Meta = Model->doc()[json::json_pointer(MPtr)];

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
                const QString P = CoverCache::instance()->resolve(Meta["COVER"], Model->packageDir()->path());
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
            QObject::connect(TitleField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
            Form->addRow("TITLE", TitleField);
            for (const std::string &FK : MetadataFields)
            {
                QLineEdit * FE = new QLineEdit(Box);
                FE->setProperty("JSONPath", QString::fromStdString(MPtr + "/" + FK));
                if (Meta.contains(FK) && !Meta[FK].is_null())
                    FE->setText(Meta[FK].is_string() ? QString::fromStdString(std::string(Meta[FK])) : QString::fromStdString(Meta[FK].dump()));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                Form->addRow(QString::fromStdString(FK), FE);
            }
            Body->addWidget(Box);
        }

        // ---- LAYERS (the per-TYPE sub-editors) ----
        {
            if (!NodeRef.contains("LAYERS") || !NodeRef["LAYERS"].is_array())
                Model->doc()[json::json_pointer(NodePtr + "/LAYERS")] = json::array();
            QGroupBox * Box = new QGroupBox("LAYERS", Contents);
            QFormLayout * Form = new QFormLayout(Box);

            auto &Layers = Model->doc()["NODES"][n]["LAYERS"];
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
                        Model->doc()["NODES"][n]["LAYERS"].erase(j); Model->SaveNodes(); Model->requestReload();
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
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                    LG->addWidget(PathField, 1, 1);

                    if (LayerType == "VFSDirLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ ZIP", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j](){
                            std::string Path = Model->doc()["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path SrcDir = std::filesystem::path(Model->packageDir()->path().toStdString()) / Path;
                            std::string ZipName = Path + ".zip";
                            std::filesystem::path ZipFile = std::filesystem::path(Model->packageDir()->path().toStdString()) / ZipName;
                            RunCommand("zip", {"-0", "-r", "-X", ZipFile.string(), "."},
                                                         QProcessEnvironment::systemEnvironment(), SrcDir.string());
                            std::filesystem::remove_all(SrcDir);
                            Model->doc()["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSZipLayer";
                            Model->doc()["NODES"][n]["LAYERS"][j]["PATH"] = ZipName;
                            Model->SaveNodes(); Model->requestReload();
                        });
                        LG->addWidget(Conv, 1, 2);
                    }
                    else if (LayerType == "VFSZipLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ DIR", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j](){
                            std::string Path = Model->doc()["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path ZipFile = std::filesystem::path(Model->packageDir()->path().toStdString()) / Path;
                            std::string DirName = (Path.size() > 4 && Path.substr(Path.size() - 4) == ".zip") ? Path.substr(0, Path.size() - 4) : Path + "_dir";
                            std::filesystem::path DirPath = std::filesystem::path(Model->packageDir()->path().toStdString()) / DirName;
                            std::filesystem::create_directories(DirPath);
                            RunCommand("unzip", {ZipFile.string(), "-d", DirPath.string()});
                            std::filesystem::remove(ZipFile);
                            Model->doc()["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSDirLayer";
                            Model->doc()["NODES"][n]["LAYERS"][j]["PATH"] = DirName;
                            Model->SaveNodes(); Model->requestReload();
                        });
                        LG->addWidget(Conv, 1, 2);
                    }

                    if (LayerType == "VFSZipLayer" || LayerType == "VFSDirLayer")
                    {
                        auto &SubRef = Model->doc()["NODES"][n]["LAYERS"][j];
                        LG->addWidget(new QLabel("TARGET:"), 2, 0);
                        QLineEdit * TargetField = new QLineEdit(LBox);
                        TargetField->setProperty("JSONPath", LPath + "/TARGET");
                        if (SubRef.contains("TARGET") && SubRef["TARGET"].is_string()) TargetField->setText(QString::fromStdString(std::string(SubRef["TARGET"])));
                        QObject::connect(TargetField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                            if (Arr.empty()) Model->doc()[Ptr].erase("SUBMOUNTS"); else Model->doc()[Ptr]["SUBMOUNTS"] = Arr;
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
                    QObject::connect(RegPath, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                    LG->addWidget(RegPath, 1, 1);

                    LG->addWidget(new QLabel("ARCHITECTURE:"), 2, 0);
                    QLineEdit * Arch = new QLineEdit(LBox);
                    Arch->setProperty("JSONPath", LPath + "/ARCHITECTURE");
                    if (Layers[j].contains("ARCHITECTURE") && Layers[j]["ARCHITECTURE"].is_string()) Arch->setText(QString::fromStdString(std::string(Layers[j]["ARCHITECTURE"])));
                    QObject::connect(Arch, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                    LG->addWidget(Arch, 2, 1);

                    if (!Layers[j].contains("KEYVALUES") || !Layers[j]["KEYVALUES"].is_object())
                        Model->doc()["NODES"][n]["LAYERS"][j]["KEYVALUES"] = json::object();
                    QGroupBox * KeysBox = new QGroupBox("KEYS", LBox);
                    QGridLayout * KG = new QGridLayout(KeysBox);
                    KG->addWidget(new QLabel("Key"), 0, 0); KG->addWidget(new QLabel("Value"), 0, 1);
                    int k = 1;
                    for (auto Obj : Model->doc()["NODES"][n]["LAYERS"][j]["KEYVALUES"].items())
                    {
                        std::string OrigKey = Obj.key();
                        QLineEdit * KeyEdit = new QLineEdit(KeysBox);
                        KeyEdit->setText(QString::fromStdString(OrigKey));
                        KeyEdit->setProperty("OriginalKey", QString::fromStdString(OrigKey));
                        QObject::connect(KeyEdit, &QLineEdit::editingFinished, this, [this, n, j, KeyEdit](){
                            QString OldKey = KeyEdit->property("OriginalKey").toString(), NewKey = KeyEdit->text();
                            if (OldKey == NewKey || NewKey.isEmpty()) return;
                            auto &KV = Model->doc()["NODES"][n]["LAYERS"][j]["KEYVALUES"];
                            auto Val = KV[OldKey.toStdString()]; KV.erase(OldKey.toStdString()); KV[NewKey.toStdString()] = Val;
                            KeyEdit->setProperty("OriginalKey", NewKey); Model->SaveNodes();
                        });
                        KG->addWidget(KeyEdit, k, 0);
                        QLineEdit * ValField = new QLineEdit(KeysBox);
                        ValField->setProperty("JSONPath", LPath + "/KEYVALUES/" + QString::fromStdString(OrigKey));
                        if (Obj.value().is_string()) ValField->setText(QString::fromStdString(std::string(Obj.value())));
                        else if (!Obj.value().is_null()) ValField->setText(QString::fromStdString(Obj.value().dump()));
                        QObject::connect(ValField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                        KG->addWidget(ValField, k, 1);
                        QPushButton * KeyDel = new QPushButton("✕", KeysBox); KeyDel->setFixedWidth(28);
                        QObject::connect(KeyDel, &QPushButton::clicked, this, [this, n, j, OrigKey](){
                            Model->doc()["NODES"][n]["LAYERS"][j]["KEYVALUES"].erase(OrigKey); Model->SaveNodes(); Model->requestReload();
                        });
                        KG->addWidget(KeyDel, k, 2); k++;
                    }
                    QPushButton * AddKey = new QPushButton("+ Add Key", KeysBox);
                    QObject::connect(AddKey, &QPushButton::clicked, this, [this, n, j](){
                        Model->doc()["NODES"][n]["LAYERS"][j]["KEYVALUES"]["new_key"] = ""; Model->SaveNodes(); Model->requestReload();
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
                    QObject::connect(DLL, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                    QObject::connect(RegPath, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
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
                        Model->doc()["NODES"][n]["LAYERS"][j]["VARTYPE"] = VT->currentText().toStdString(); Model->SaveNodes(); Model->requestReload();
                    });
                    LG->addWidget(VT, cvrow, 1, 1, 2); cvrow++;

                    LG->addWidget(new QLabel("DISPLAY:"), cvrow, 0);
                    QCheckBox * Disp = new QCheckBox(LBox);
                    Disp->setChecked(Layers[j].value("DISPLAY", true));
                    QObject::connect(Disp, &QCheckBox::toggled, this, [this, n, j](bool On){
                        Model->doc()["NODES"][n]["LAYERS"][j]["DISPLAY"] = On; Model->SaveNodes();
                    });
                    LG->addWidget(Disp, cvrow, 1, 1, 2); cvrow++;

                    if (VarType == "options" || VarType == "random")
                    {
                        if (!Layers[j].contains("OPTIONS") || !Layers[j]["OPTIONS"].is_array())
                            Model->doc()["NODES"][n]["LAYERS"][j]["OPTIONS"] = json::array();
                        bool IsRandom = (VarType == "random");
                        QGroupBox * OptsBox = new QGroupBox(IsRandom ? "Value Pool" : "OPTIONS", LBox);
                        QVBoxLayout * OptsLayout = new QVBoxLayout(OptsBox);
                        auto &Opts = Model->doc()["NODES"][n]["LAYERS"][j]["OPTIONS"];
                        for (int oi = 0; oi < (int)Opts.size(); oi++)
                        {
                            QHBoxLayout * OptRow = new QHBoxLayout();
                            if (!IsRandom)
                            {
                                QLineEdit * OptLabel = new QLineEdit(OptsBox); OptLabel->setPlaceholderText("Label");
                                OptLabel->setProperty("JSONPath", QString("%1/OPTIONS/%2/LABEL").arg(LPath).arg(oi));
                                if (Opts[oi].contains("LABEL") && Opts[oi]["LABEL"].is_string()) OptLabel->setText(QString::fromStdString(std::string(Opts[oi]["LABEL"])));
                                QObject::connect(OptLabel, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                                OptRow->addWidget(OptLabel);
                            }
                            QLineEdit * OptValue = new QLineEdit(OptsBox); OptValue->setPlaceholderText("Value");
                            OptValue->setProperty("JSONPath", QString("%1/OPTIONS/%2/VALUE").arg(LPath).arg(oi));
                            if (Opts[oi].contains("VALUE") && Opts[oi]["VALUE"].is_string()) OptValue->setText(QString::fromStdString(std::string(Opts[oi]["VALUE"])));
                            QObject::connect(OptValue, &QLineEdit::editingFinished, this, &NodeEditor::onFieldEdited);
                            QPushButton * OptDel = new QPushButton("✕", OptsBox); OptDel->setFixedWidth(28);
                            QObject::connect(OptDel, &QPushButton::clicked, this, [this, n, j, oi](){
                                Model->doc()["NODES"][n]["LAYERS"][j]["OPTIONS"].erase(oi); Model->SaveNodes(); Model->requestReload();
                            });
                            OptRow->addWidget(OptValue); OptRow->addWidget(OptDel); OptsLayout->addLayout(OptRow);
                        }
                        QPushButton * AddOpt = new QPushButton(IsRandom ? "+ Add Value" : "+ Add Option", OptsBox);
                        QObject::connect(AddOpt, &QPushButton::clicked, this, [this, n, j, IsRandom](){
                            if (IsRandom) Model->doc()["NODES"][n]["LAYERS"][j]["OPTIONS"].push_back(json::object({{"VALUE", ""}}));
                            else          Model->doc()["NODES"][n]["LAYERS"][j]["OPTIONS"].push_back(json::object({{"LABEL", ""}, {"VALUE", ""}}));
                            Model->SaveNodes(); Model->requestReload();
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
}

// ============================================================================
// Field edit + LAYERS add-actions
// ============================================================================

void NodeEditor::onFieldEdited()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(QObject::sender());
    if (!Editor) return;
    json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    Model->doc()[JSONPointer] = Editor->text().toStdString();
    Model->SaveNodes();
}

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
void NodeEditor::AddCustomVar()    { appendLayer(json::object({ {"TYPE", "CustomVar"}, {"KEY", ""}, {"LABEL", ""}, {"DEFAULT", ""}, {"VARTYPE", "string"}, {"DISPLAY", true} })); }
void NodeEditor::AddPersistDir()   { appendLayer(json::object({ {"TYPE", "PersistDir"},  {"PATH", ""} })); }
void NodeEditor::AddPersistFile()  { appendLayer(json::object({ {"TYPE", "PersistFile"}, {"PATH", ""} })); }
void NodeEditor::AddRegPersist()   { appendLayer(json::object({ {"TYPE", "RegPersist"} })); }
void NodeEditor::AddRegKeyPersist(){ appendLayer(json::object({ {"TYPE", "RegKeyPersist"}, {"REGPATH", ""} })); }

// ============================================================================
// META — cover drop
// ============================================================================

void NodeEditor::ApplyCoverImage(QLabel * CoverLabel, const QByteArray & Data, const QString & Extension, int NodeIndexInArray)
{
    if (Data.isEmpty() || NodeIndexInArray < 0 || NodeIndexInArray >= (int)Model->doc()["NODES"].size()) return;

    const std::string NodeId = Model->doc()["NODES"][NodeIndexInArray].value("NODE_ID", std::string());
    QString FileName = (NodeId.empty() ? QString("node%1").arg(NodeIndexInArray) : QString::fromStdString(NodeId)) + "_cover." + Extension.toLower();
    QString DestPath = QDir::cleanPath(Model->packageDir()->path() + "/" + FileName);

    QFile OutFile(DestPath);
    if (!OutFile.open(QIODevice::WriteOnly)) { LogErr("NodeEditor", "Could not write cover: " + DestPath.toStdString()); return; }
    OutFile.write(Data); OutFile.close();

    Model->doc()[json::json_pointer(QString("/NODES/%1/META/COVER").arg(NodeIndexInArray).toStdString())] = FileName.toStdString();
    Model->SaveNodes();

    QPixmap Pix; Pix.loadFromData(Data);
    if (!Pix.isNull()) CoverLabel->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    LogSucc("NodeEditor", "Cover set: " + FileName.toStdString());
}

bool NodeEditor::eventFilter(QObject * obj, QEvent * event)
{
    QLabel * CoverLabel = qobject_cast<QLabel*>(obj);
    if (!CoverLabel || !CoverLabel->property("NodeArrayIndex").isValid())
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::DragEnter)
    {
        QDragEnterEvent * ev = static_cast<QDragEnterEvent*>(event);
        if (ev->mimeData()->hasImage() || ev->mimeData()->hasUrls() || ev->mimeData()->hasText()) ev->acceptProposedAction();
        return true;
    }
    if (event->type() == QEvent::DragMove) { static_cast<QDragMoveEvent*>(event)->acceptProposedAction(); return true; }

    if (event->type() == QEvent::Drop)
    {
        QDropEvent * ev = static_cast<QDropEvent*>(event);
        int Idx = CoverLabel->property("NodeArrayIndex").toInt();
        const QMimeData * Mime = ev->mimeData();

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
                QNetworkReply * Reply = NetMgr->get(QNetworkRequest(Url));
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
                else LogErr("NodeEditor", "Failed to download cover: " + Reply->errorString().toStdString());
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
                QNetworkReply * Reply = NetMgr->get(QNetworkRequest(Url));
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
    return QWidget::eventFilter(obj, event);
}
