#include "asyncwork.h"
#include "nodesections.h"
#include "packageeditormodel.h"
#include "covercache.h"
#include "manifestmodel.h"   // ManifestModel::ZipFullyStored (STORE-zip check for the re-store action)
#include "procenv.h"   // RunCommand (LAYERS zip<->dir convert)
#include "commonutils.h"

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// The META metadata field list lives in ManifestModel::MetaEditableFields (package-format knowledge).

NodeIdentitySection::NodeIdentitySection(PackageEditorModel * model, int nodeIndex, QWidget * parent)
    : NodeSection("Identity", model, nodeIndex, parent)
{
    auto & NodeRef = Model->doc()["NODES"][N];
    const std::string NodePtr = "/NODES/" + std::to_string(N);
            QFormLayout * Form = new QFormLayout(this);

            QLineEdit * IdField = new QLineEdit(this);
            IdField->setProperty("JSONPath", QString::fromStdString(NodePtr + "/NODE_ID"));
            IdField->setText(QString::fromStdString(NodeRef.value("NODE_ID", std::string())));
            QObject::connect(IdField, &QLineEdit::editingFinished, this, [this](){
                onFieldEdited(); Model->requestReload();   // renames re-file the node + relabel the tab
            });
            Form->addRow("NODE_ID", IdField);

            // Identity is DERIVED from the node's Declare* layers (no ROLE field): a DeclareExec ⇒ launchable, a
            // DeclareRunner ⇒ runner, a DeclareLibraryItem ⇒ a library tile; none ⇒ content. Show it as a read-only
            // badge so the node's nature stays legible — add the matching Declare* layer below to give it an identity.
            bool HasExec = false, HasRunner = false, HasLib = false;
            if (NodeRef.contains("LAYERS") && NodeRef["LAYERS"].is_array())
                for (const auto & L : NodeRef["LAYERS"])
                {
                    const std::string T = L.is_object() ? L.value("TYPE", std::string()) : std::string();
                    if      (T == "DeclareExec")        HasExec = true;
                    else if (T == "DeclareRunner")      HasRunner = true;
                    else if (T == "DeclareLibraryItem") HasLib = true;
                }
            QStringList Tags;
            if (HasRunner) Tags << "runner";
            if (HasExec)   Tags << "launchable";
            if (HasLib)    Tags << "library tile";
            if (Tags.isEmpty()) Tags << "content";
            QLabel * Badge = new QLabel(Tags.join(" · "), this);
            Badge->setStyleSheet("font-weight:bold;color:#8fbcd4;");
            Form->addRow("Identity", Badge);
}

NodeSelectionSection::NodeSelectionSection(PackageEditorModel * model, int nodeIndex, QWidget * parent)
    : NodeSection("Selection (when referenced as a parent)", model, nodeIndex, parent)
{
    auto & NodeRef = Model->doc()["NODES"][N];
    const std::string NodePtr = "/NODES/" + std::to_string(N);
            QVBoxLayout * BL = new QVBoxLayout(this);

            QCheckBox * OptChk = new QCheckBox("OPTIONAL (user-toggleable add-on)", this);
            OptChk->setChecked(NodeRef.value("OPTIONAL", false));
            QObject::connect(OptChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                Model->doc()[json::json_pointer(NodePtr + "/OPTIONAL")] = On; Model->SaveNodes();
            });
            BL->addWidget(OptChk);

            QCheckBox * DefChk = new QCheckBox("DEFAULT (enabled by default when optional)", this);
            DefChk->setChecked(NodeRef.value("DEFAULT", true));
            QObject::connect(DefChk, &QCheckBox::toggled, this, [this, NodePtr](bool On){
                Model->doc()[json::json_pointer(NodePtr + "/DEFAULT")] = On; Model->SaveNodes();
            });
            BL->addWidget(DefChk);

            QLineEdit * ExcludeField = new QLineEdit(this);
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
            BL->addWidget(ExcludeField);}

NodeParentsSection::NodeParentsSection(PackageEditorModel * model, int nodeIndex, QWidget * parent)
    : NodeSection("PARENTS (load order — later overrides earlier)", model, nodeIndex, parent)
{
    auto & NodeRef = Model->doc()["NODES"][N];
    const std::string NodePtr = "/NODES/" + std::to_string(N);
            const std::string PPtr = NodePtr + "/PARENTS";
            if (!NodeRef.contains("PARENTS") || !NodeRef["PARENTS"].is_array())
                Model->doc()[json::json_pointer(PPtr)] = json::array();
            auto &Parents = Model->doc()[json::json_pointer(PPtr)];

            QVBoxLayout * BL = new QVBoxLayout(this);
            const std::vector<std::string> AllIds = Model->KnownNodeIds();

            for (int k = 0; k < (int)Parents.size(); k++)
            {
                QHBoxLayout * Row = new QHBoxLayout();
                QComboBox * Pick = new QComboBox(this);
                Pick->setEditable(true);
                for (const auto &Id : AllIds) Pick->addItem(QString::fromStdString(Id));
                { QSignalBlocker B(Pick); Pick->setCurrentText(QString::fromStdString(Parents[k].is_string() ? std::string(Parents[k]) : "")); }
                const std::string ItemPtr = PPtr + "/" + std::to_string(k);
                QObject::connect(Pick, &QComboBox::currentTextChanged, this, [this, ItemPtr](const QString &T){
                    Model->doc()[json::json_pointer(ItemPtr)] = T.trimmed().toStdString(); Model->SaveNodes();
                });
                QPushButton * Up = new QPushButton("▲", this); Up->setFixedWidth(28);
                QPushButton * Dn = new QPushButton("▼", this); Dn->setFixedWidth(28);
                QPushButton * Del = new QPushButton("✕", this); Del->setFixedWidth(28);
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
            QPushButton * AddParent = new QPushButton("+ Add Parent", this);
            QObject::connect(AddParent, &QPushButton::clicked, this, [this, PPtr](){
                Model->doc()[json::json_pointer(PPtr)].push_back(""); Model->SaveNodes(); Model->requestReload();
            });
            BL->addWidget(AddParent);}

NodeLayersSection::NodeLayersSection(PackageEditorModel * model, int nodeIndex, QWidget * parent)
    : NodeSection("LAYERS", model, nodeIndex, parent)
{
    const int n = N;
    auto & NodeRef = Model->doc()["NODES"][N];
    const std::string NodePtr = "/NODES/" + std::to_string(N);
            if (!NodeRef.contains("LAYERS") || !NodeRef["LAYERS"].is_array())
                Model->doc()[json::json_pointer(NodePtr + "/LAYERS")] = json::array();
            QFormLayout * Form = new QFormLayout(this);

            auto &Layers = Model->doc()["NODES"][n]["LAYERS"];
            for (int j = 0; j < (int)Layers.size(); j++)
            {
                const QString LayerType = QString::fromStdString(Layers[j].value("TYPE", std::string()));
                QGroupBox * LBox = new QGroupBox(this);
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
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                    LG->addWidget(PathField, 1, 1);

                    if (LayerType == "VFSDirLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ ZIP", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j, Conv](){
                            std::string Path = Model->doc()["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path SrcDir = std::filesystem::path(Model->packageDir()->path().toStdString()) / Path;
                            std::string ZipName = Path + ".zip";
                            std::filesystem::path ZipFile = std::filesystem::path(Model->packageDir()->path().toStdString()) / ZipName;
                            //Multi-GB zip authoring must not freeze the editor: heavy work off-thread, model
                            //mutation back on the GUI thread (AsyncWork guards against teardown mid-flight).
                            Conv->setEnabled(false); Conv->setText("zipping…");
                            AsyncWork::Run(this,
                                [SrcDir, ZipFile]() {
                                    RunCommand("zip", {"-0", "-r", "-X", ZipFile.string(), "."},
                                               QProcessEnvironment::systemEnvironment(), SrcDir.string());
                                    std::filesystem::remove_all(SrcDir);
                                },
                                [this, n, j, ZipName]() {
                                    Model->doc()["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSZipLayer";
                                    Model->doc()["NODES"][n]["LAYERS"][j]["PATH"] = ZipName;
                                    Model->SaveNodes(); Model->requestReload();
                                });
                        });
                        LG->addWidget(Conv, 1, 2);
                    }
                    else if (LayerType == "VFSZipLayer")
                    {
                        QPushButton * Conv = new QPushButton("→ DIR", LBox);
                        QObject::connect(Conv, &QPushButton::clicked, this, [this, n, j, Conv](){
                            std::string Path = Model->doc()["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                            std::filesystem::path ZipFile = std::filesystem::path(Model->packageDir()->path().toStdString()) / Path;
                            std::string DirName = (Path.size() > 4 && Path.substr(Path.size() - 4) == ".zip") ? Path.substr(0, Path.size() - 4) : Path + "_dir";
                            std::filesystem::path DirPath = std::filesystem::path(Model->packageDir()->path().toStdString()) / DirName;
                            Conv->setEnabled(false); Conv->setText("unzipping…");
                            AsyncWork::Run(this,
                                [ZipFile, DirPath]() {
                                    std::filesystem::create_directories(DirPath);
                                    RunCommand("unzip", {ZipFile.string(), "-d", DirPath.string()});
                                    std::filesystem::remove(ZipFile);
                                },
                                [this, n, j, DirName]() {
                                    Model->doc()["NODES"][n]["LAYERS"][j]["TYPE"] = "VFSDirLayer";
                                    Model->doc()["NODES"][n]["LAYERS"][j]["PATH"] = DirName;
                                    Model->SaveNodes(); Model->requestReload();
                                });
                        });
                        LG->addWidget(Conv, 1, 2);

                        //If the zip is DEFLATE-compressed it cannot mount (VidyaGodFS reads entries at their
                        //backing offset and can't inflate). Offer a one-click fix that unzips + re-zips STORE.
                        //The STORE probe scans the zip's central directory — done OFF-thread (it used to run
                        //synchronously per layer while building this tab, stalling the editor on big packages);
                        //the button materializes only if the probe finds DEFLATE entries.
                        {
                            std::string ZipPathStr = Layers[j].value("PATH", std::string());
                            std::filesystem::path ZipFile = std::filesystem::path(Model->packageDir()->path().toStdString()) / ZipPathStr;
                            if (!ZipPathStr.empty() && std::filesystem::exists(ZipFile))
                            {
                                auto FullyStored = std::make_shared<bool>(true);
                                AsyncWork::Run(this,
                                    [ZipFile, FullyStored]() { *FullyStored = ManifestModel::ZipFullyStored(ZipFile.string()); },
                                    [this, LG, LBox, n, j, FullyStored]() {
                                        if (*FullyStored) return;
                                        QPushButton * Fix = new QPushButton("⚠ Re-store", LBox);
                                        Fix->setToolTip("This zip is DEFLATE-compressed and will not mount — re-pack it uncompressed (STORE).");
                                        QObject::connect(Fix, &QPushButton::clicked, this, [this, n, j, Fix](){
                                            std::string Path = Model->doc()["NODES"][n]["LAYERS"][j].value("PATH", std::string());
                                            std::filesystem::path PkgDir = std::filesystem::path(Model->packageDir()->path().toStdString());
                                            std::filesystem::path ZipTarget = PkgDir / Path;
                                            const QString Msg =
                                                "“" + QString::fromStdString(Path) + "” is DEFLATE-compressed.\n\n"
                                                "VidyaGod mounts zip layers by reading each file at its position inside the zip — it "
                                                "cannot decompress on the fly, so a compressed zip mounts to garbage and the game "
                                                "won't launch.\n\nRe-pack it uncompressed (STORE) now? The contents are unchanged; "
                                                "only the on-disk packing differs (the file may get larger).";
                                            if (QMessageBox::question(this, "Re-pack zip as STORE", Msg,
                                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
                                                return;
                                            Fix->setEnabled(false); Fix->setText("re-packing…");
                                            //Unzip to a sibling temp dir, then re-zip STORE (`-0`) with files at the root (`.`).
                                            AsyncWork::Run(this,
                                                [PkgDir, Path, ZipTarget]() {
                                                    std::filesystem::path Tmp = PkgDir / (Path + ".restore.tmp");
                                                    std::error_code Ec; std::filesystem::remove_all(Tmp, Ec);
                                                    std::filesystem::create_directories(Tmp);
                                                    RunCommand("unzip", {"-o", ZipTarget.string(), "-d", Tmp.string()});
                                                    std::filesystem::remove(ZipTarget, Ec);
                                                    RunCommand("zip", {"-0", "-r", "-X", ZipTarget.string(), "."},
                                                               QProcessEnvironment::systemEnvironment(), Tmp.string());
                                                    std::filesystem::remove_all(Tmp, Ec);
                                                },
                                                [this]() { Model->SaveNodes(); Model->requestReload(); });   // re-runs validation → error clears
                                        });
                                        LG->addWidget(Fix, 1, 3);
                                    });
                            }
                        }
                    }

                    if (LayerType == "VFSZipLayer" || LayerType == "VFSDirLayer")
                    {
                        auto &SubRef = Model->doc()["NODES"][n]["LAYERS"][j];
                        LG->addWidget(new QLabel("TARGET:"), 2, 0);
                        QLineEdit * TargetField = new QLineEdit(LBox);
                        TargetField->setProperty("JSONPath", LPath + "/TARGET");
                        if (SubRef.contains("TARGET") && SubRef["TARGET"].is_string()) TargetField->setText(QString::fromStdString(std::string(SubRef["TARGET"])));
                        QObject::connect(TargetField, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
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
                    QObject::connect(RegPath, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                    LG->addWidget(RegPath, 1, 1);

                    LG->addWidget(new QLabel("ARCHITECTURE:"), 2, 0);
                    QLineEdit * Arch = new QLineEdit(LBox);
                    Arch->setProperty("JSONPath", LPath + "/ARCHITECTURE");
                    if (Layers[j].contains("ARCHITECTURE") && Layers[j]["ARCHITECTURE"].is_string()) Arch->setText(QString::fromStdString(std::string(Layers[j]["ARCHITECTURE"])));
                    QObject::connect(Arch, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
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
                        QObject::connect(ValField, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
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
                    QObject::connect(DLL, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                    LG->addWidget(DLL, 1, 1);
                }
                else if (LayerType == "FileEdit")
                {
                    std::string Mode = Layers[j].value("MODE", std::string("ConfigWrite"));
                    // ConfigWrite needs a KEY (prefix-replace); Overwrite/AppendLine just take a VALUE.
                    QStringList Fields = (Mode == "Overwrite" || Mode == "AppendLine")
                        ? QStringList{"MODE", "FILE", "VALUE"} : QStringList{"MODE", "FILE", "KEY", "VALUE"};
                    int ferow = 1;
                    for (const QString &Field : Fields)
                    {
                        LG->addWidget(new QLabel(Field + ":"), ferow, 0);
                        QLineEdit * FE = new QLineEdit(LBox);
                        FE->setProperty("JSONPath", LPath + "/" + Field);
                        if (Layers[j].contains(Field.toStdString()) && Layers[j][Field.toStdString()].is_string()) FE->setText(QString::fromStdString(std::string(Layers[j][Field.toStdString()])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                        LG->addWidget(FE, ferow, 1); ferow++;
                    }
                }
                else if (LayerType == "Persist")
                {
                    // One self-describing, purely-additive persistence primitive: KEEP (persist a target) + DROP (make a
                    // path ephemeral). No mode — the dir/file/registry kind of a KEEP target is derived, not picked.
                    int prow = 1;

                    // KEEP — persist a target. Self-describing: %RuntimePath% → the whole runtime; a path → dir or single
                    // file; HKxx… → a registry hive or subtree; "registry" → all hives.
                    LG->addWidget(new QLabel("KEEP:"), prow, 0);
                    QLineEdit * Keep = new QLineEdit(LBox);
                    Keep->setPlaceholderText("drive_c/users/steamuser/Saves   ·   HKCU\\Software\\<vendor>\\<game>   ·   %RuntimePath%");
                    Keep->setProperty("JSONPath", LPath + "/KEEP");
                    if (Layers[j].contains("KEEP") && Layers[j]["KEEP"].is_string()) Keep->setText(QString::fromStdString(std::string(Layers[j]["KEEP"])));
                    QObject::connect(Keep, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                    LG->addWidget(Keep, prow, 1, 1, 2); prow++;

                    // DROP — make a runtime path ephemeral (writes discarded), e.g. a cache dir inside a kept tree. Paths only.
                    LG->addWidget(new QLabel("DROP:"), prow, 0);
                    QLineEdit * Drop = new QLineEdit(LBox);
                    Drop->setPlaceholderText("drive_c/<game>/cache   (optional — paths only)");
                    Drop->setProperty("JSONPath", LPath + "/DROP");
                    if (Layers[j].contains("DROP") && Layers[j]["DROP"].is_string()) Drop->setText(QString::fromStdString(std::string(Layers[j]["DROP"])));
                    QObject::connect(Drop, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                    LG->addWidget(Drop, prow, 1, 1, 2); prow++;

                    QLabel * Hint = new QLabel("The runtime is pristine by default — only KEEPs persist (and a runner ships a "
                                               "keep-set for its platform's user-state, so most games need nothing here). "
                                               "KEEP %RuntimePath% to persist everything; DROP carves an ephemeral hole.", LBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;"); Hint->setWordWrap(true);
                    LG->addWidget(Hint, prow, 1, 1, 2);
                }
                else if (LayerType == "DeclareExec" || LayerType == "DeclareLibraryItem" || LayerType == "DeclareRunner")
                {
                    // Identity layers: a simple field form per kind (lists/objects on a runner go through the raw JSON tab).
                    auto AddText = [&](int Row, const char *FK, const char *Hint = ""){
                        LG->addWidget(new QLabel(QString(FK) + ":"), Row, 0);
                        QLineEdit * FE = new QLineEdit(LBox);
                        FE->setProperty("JSONPath", LPath + "/" + FK);
                        if (Hint[0]) FE->setPlaceholderText(Hint);
                        if (Layers[j].contains(FK) && Layers[j][FK].is_string()) FE->setText(QString::fromStdString(std::string(Layers[j][FK])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                        LG->addWidget(FE, Row, 1, 1, 2);
                    };
                    int r = 1;
                    if (LayerType == "DeclareExec")
                    {
                        AddText(r++, "PLATFORM", "win32 / linux64 / snes …  (the content's platform)");
                        AddText(r++, "CONTENTPATH", "exe/ROM path relative to the content mount");
                        AddText(r++, "EXEARGS"); AddText(r++, "WORKDIR");
                        AddText(r++, "LABEL", "variant label for the picker (when grouped under a game)");
                        QCheckBox * Rec = new QCheckBox("RECOMMENDED (default variant of its game)", LBox);
                        Rec->setChecked(Layers[j].value("RECOMMENDED", false));
                        QObject::connect(Rec, &QCheckBox::toggled, this, [this, n, j](bool On){
                            Model->doc()["NODES"][n]["LAYERS"][j]["RECOMMENDED"] = On; Model->SaveNodes(); });
                        LG->addWidget(Rec, r, 0, 1, 3);
                    }
                    else if (LayerType == "DeclareLibraryItem")
                    {
                        AddText(r++, "TITLE", "the game's display title (the library tile)");
                        AddText(r++, "UID", "numeric package id");

                        // Cover drop-target: drag an image / local file / URL onto it → COVER inside THIS layer.
                        LG->addWidget(new QLabel("COVER:"), r, 0);
                        QLabel * Cover = new QLabel(LBox);
                        Cover->setFixedSize(150, 225);
                        Cover->setAlignment(Qt::AlignCenter);
                        Cover->setWordWrap(true);
                        Cover->setStyleSheet("QLabel { background-color:#1e1e1e; border:2px dashed #555; color:#777; border-radius:4px; font-size:11px; }");
                        Cover->setText("Drop cover art\nhere\n\n2 : 3");
                        Cover->setAcceptDrops(true);
                        Cover->setProperty("NodeArrayIndex", n);
                        Cover->setProperty("LayerArrayIndex", j);
                        if (Layers[j].contains("COVER"))
                        {
                            const QString P = CoverCache::instance()->resolve(Layers[j]["COVER"], Model->packageDir()->path());
                            if (!P.isEmpty()) { QPixmap Pix(P); if (!Pix.isNull()) Cover->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
                        }
                        Cover->installEventFilter(this);
                        LG->addWidget(Cover, r++, 1, 1, 2);

                        QLabel * H = new QLabel("Variants of one game are nodes with a DeclareExec that PARENT this tile node. "
                                                "Extra metadata (DEVELOPER, SERIES, …) is editable in the raw JSON tab.", LBox);
                        H->setStyleSheet("color:#8f98a0;font-size:9pt;"); H->setWordWrap(true);
                        LG->addWidget(H, r, 0, 1, 3);
                    }
                    else // DeclareRunner
                    {
                        AddText(r++, "HOST", "the platform the runner runs ON (e.g. linux64)");
                        AddText(r++, "EXECUTABLE"); AddText(r++, "CONTENT_ROOT"); AddText(r++, "GUEST_PATH");
                        for (const char *CK : {"PREFIX_GENERATE", "UNIFIED_RUNTIME"})
                        {
                            QCheckBox * C = new QCheckBox(CK, LBox);
                            C->setChecked(Layers[j].value(CK, false));
                            const std::string Key = CK;
                            QObject::connect(C, &QCheckBox::toggled, this, [this, n, j, Key](bool On){
                                Model->doc()["NODES"][n]["LAYERS"][j][Key] = On; Model->SaveNodes(); });
                            LG->addWidget(C, r++, 0, 1, 3);
                        }
                        QLabel * H = new QLabel("GUEST (platforms served), ARGS, ENV, REMOVE_ENV are lists/objects — edit them in the raw JSON tab.", LBox);
                        H->setStyleSheet("color:#8f98a0;font-size:9pt;"); H->setWordWrap(true);
                        LG->addWidget(H, r, 0, 1, 3);
                    }
                }
                else if (LayerType == "CustomVar")
                {
                    int cvrow = 1;
                    // Value identity: KEY (the %token%) + DEFAULT (its value — fixed for a binding, the initial for a knob).
                    for (const auto &FK : std::vector<std::string>{"KEY", "DEFAULT"})
                    {
                        LG->addWidget(new QLabel(QString::fromStdString(FK) + ":"), cvrow, 0);
                        QLineEdit * FE = new QLineEdit(LBox);
                        FE->setProperty("JSONPath", LPath + "/" + QString::fromStdString(FK));
                        if (Layers[j].contains(FK) && Layers[j][FK].is_string()) FE->setText(QString::fromStdString(std::string(Layers[j][FK])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                        LG->addWidget(FE, cvrow, 1, 1, 2); cvrow++;
                    }

                    // A `UI` facet makes it a user-facing option (shown in prelaunch); absent ⇒ an internal binding.
                    const bool HasUI = Layers[j].contains("UI") && Layers[j]["UI"].is_object();
                    LG->addWidget(new QLabel("User option:"), cvrow, 0);
                    QCheckBox * UiToggle = new QCheckBox("show a control in the launch dialog", LBox);
                    UiToggle->setChecked(HasUI);
                    QObject::connect(UiToggle, &QCheckBox::toggled, this, [this, n, j](bool On){
                        if (On) Model->doc()["NODES"][n]["LAYERS"][j]["UI"] = json::object({{"LABEL", ""}, {"CONTROL", "text"}});
                        else    Model->doc()["NODES"][n]["LAYERS"][j].erase("UI");
                        Model->SaveNodes(); Model->requestReload();
                    });
                    LG->addWidget(UiToggle, cvrow, 1, 1, 2); cvrow++;

                    if (HasUI)
                    {
                        const json UI = Layers[j]["UI"];
                        const std::string Control = UI.value("CONTROL", std::string("text"));
                        const QString UPath = LPath + "/UI";

                        for (const auto &FK : std::vector<std::string>{"LABEL", "GROUP", "WHEN"})
                        {
                            LG->addWidget(new QLabel(QString::fromStdString(FK) + ":"), cvrow, 0);
                            QLineEdit * FE = new QLineEdit(LBox);
                            FE->setProperty("JSONPath", UPath + "/" + QString::fromStdString(FK));
                            if (FK == "WHEN")  FE->setPlaceholderText("%OTHER_KEY% == value   (optional — show only when true)");
                            if (FK == "GROUP") FE->setPlaceholderText("Graphics   (optional — UI section)");
                            if (UI.contains(FK) && UI[FK].is_string()) FE->setText(QString::fromStdString(std::string(UI[FK])));
                            QObject::connect(FE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                            LG->addWidget(FE, cvrow, 1, 1, 2); cvrow++;
                        }

                        LG->addWidget(new QLabel("CONTROL:"), cvrow, 0);
                        QComboBox * CC = new QComboBox(LBox);
                        CC->addItems({"text", "bool", "int", "float", "enum", "secret"});
                        CC->setCurrentText(QString::fromStdString(Control));
                        QObject::connect(CC, &QComboBox::currentIndexChanged, this, [this, n, j, CC](){
                            Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["CONTROL"] = CC->currentText().toStdString();
                            Model->SaveNodes(); Model->requestReload();
                        });
                        LG->addWidget(CC, cvrow, 1, 1, 2); cvrow++;

                        if (Control == "int" || Control == "float")   // numeric constraints
                        {
                            LG->addWidget(new QLabel("MIN / MAX:"), cvrow, 0);
                            QHBoxLayout * MM = new QHBoxLayout();
                            QSpinBox * MinS = new QSpinBox(LBox); MinS->setRange(-2147483647, 2147483647); MinS->setValue(UI.value("MIN", 0));
                            QSpinBox * MaxS = new QSpinBox(LBox); MaxS->setRange(-2147483647, 2147483647); MaxS->setValue(UI.value("MAX", 0));
                            QObject::connect(MinS, &QSpinBox::valueChanged, this, [this, n, j](int v){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["MIN"] = v; Model->SaveNodes(); });
                            QObject::connect(MaxS, &QSpinBox::valueChanged, this, [this, n, j](int v){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["MAX"] = v; Model->SaveNodes(); });
                            MM->addWidget(MinS); MM->addWidget(MaxS);
                            QWidget * MMW = new QWidget(LBox); MMW->setLayout(MM); LG->addWidget(MMW, cvrow, 1, 1, 2); cvrow++;
                        }
                        else if (Control == "text")   // input pattern
                        {
                            LG->addWidget(new QLabel("PATTERN:"), cvrow, 0);
                            QLineEdit * PF = new QLineEdit(LBox); PF->setPlaceholderText("^[A-Z0-9]+$   (optional regex)");
                            PF->setProperty("JSONPath", UPath + "/PATTERN");
                            if (UI.contains("PATTERN") && UI["PATTERN"].is_string()) PF->setText(QString::fromStdString(std::string(UI["PATTERN"])));
                            QObject::connect(PF, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                            LG->addWidget(PF, cvrow, 1, 1, 2); cvrow++;
                        }
                        else if (Control == "enum")   // CHOICES (label/value pairs)
                        {
                            if (!Layers[j]["UI"].contains("CHOICES") || !Layers[j]["UI"]["CHOICES"].is_array())
                                Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["CHOICES"] = json::array();
                            QGroupBox * CB = new QGroupBox("CHOICES", LBox); QVBoxLayout * CL = new QVBoxLayout(CB);
                            auto &Ch = Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["CHOICES"];
                            for (int oi = 0; oi < (int)Ch.size(); oi++)
                            {
                                QHBoxLayout * Row = new QHBoxLayout();
                                QLineEdit * LblE = new QLineEdit(CB); LblE->setPlaceholderText("Label");
                                LblE->setProperty("JSONPath", QString("%1/UI/CHOICES/%2/LABEL").arg(LPath).arg(oi));
                                if (Ch[oi].contains("LABEL") && Ch[oi]["LABEL"].is_string()) LblE->setText(QString::fromStdString(std::string(Ch[oi]["LABEL"])));
                                QObject::connect(LblE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                                QLineEdit * ValE = new QLineEdit(CB); ValE->setPlaceholderText("Value");
                                ValE->setProperty("JSONPath", QString("%1/UI/CHOICES/%2/VALUE").arg(LPath).arg(oi));
                                if (Ch[oi].contains("VALUE") && Ch[oi]["VALUE"].is_string()) ValE->setText(QString::fromStdString(std::string(Ch[oi]["VALUE"])));
                                QObject::connect(ValE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                                QPushButton * Del = new QPushButton("✕", CB); Del->setFixedWidth(28);
                                QObject::connect(Del, &QPushButton::clicked, this, [this, n, j, oi](){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["CHOICES"].erase(oi); Model->SaveNodes(); Model->requestReload(); });
                                Row->addWidget(LblE); Row->addWidget(ValE); Row->addWidget(Del); CL->addLayout(Row);
                            }
                            QPushButton * Add = new QPushButton("+ Add choice", CB);
                            QObject::connect(Add, &QPushButton::clicked, this, [this, n, j](){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["CHOICES"].push_back(json::object({{"LABEL", ""}, {"VALUE", ""}})); Model->SaveNodes(); Model->requestReload(); });
                            CL->addWidget(Add); LG->addWidget(CB, cvrow, 0, 1, -1); cvrow++;
                        }
                        else if (Control == "secret")   // POOL (plain values rotated each launch)
                        {
                            if (!Layers[j]["UI"].contains("POOL") || !Layers[j]["UI"]["POOL"].is_array())
                                Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["POOL"] = json::array();
                            QGroupBox * PB = new QGroupBox("POOL (one picked per launch; empty ⇒ user enters their own)", LBox); QVBoxLayout * PL = new QVBoxLayout(PB);
                            auto &Pool = Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["POOL"];
                            for (int oi = 0; oi < (int)Pool.size(); oi++)
                            {
                                QHBoxLayout * Row = new QHBoxLayout();
                                QLineEdit * ValE = new QLineEdit(PB); ValE->setPlaceholderText("value");
                                ValE->setProperty("JSONPath", QString("%1/UI/POOL/%2").arg(LPath).arg(oi));
                                if (Pool[oi].is_string()) ValE->setText(QString::fromStdString(std::string(Pool[oi])));
                                QObject::connect(ValE, &QLineEdit::editingFinished, this, &NodeSection::onFieldEdited);
                                QPushButton * Del = new QPushButton("✕", PB); Del->setFixedWidth(28);
                                QObject::connect(Del, &QPushButton::clicked, this, [this, n, j, oi](){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["POOL"].erase(oi); Model->SaveNodes(); Model->requestReload(); });
                                Row->addWidget(ValE); Row->addWidget(Del); PL->addLayout(Row);
                            }
                            QPushButton * Add = new QPushButton("+ Add value", PB);
                            QObject::connect(Add, &QPushButton::clicked, this, [this, n, j](){ Model->doc()["NODES"][n]["LAYERS"][j]["UI"]["POOL"].push_back(std::string()); Model->SaveNodes(); Model->requestReload(); });
                            PL->addWidget(Add); LG->addWidget(PB, cvrow, 0, 1, -1); cvrow++;
                        }
                    }

                    QLabel * Hint = new QLabel("Reference as  %" + QString::fromStdString(Layers[j].value("KEY", std::string("KEY")))
                                               + "%   ·   encode at the use site:  %KEY:dword%  %KEY:bool%  %KEY:winpath%", LBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;"); Hint->setWordWrap(true);
                    LG->addWidget(Hint, cvrow, 0, 1, -1);
                }
                else
                {
                    LG->addWidget(new QLabel("(unrecognised layer type)"), 1, 0, 1, -1);
                }
                Form->addRow(LBox);
            }}

// Write a dropped cover image to the bundle and point the DeclareLibraryItem layer's COVER at it (bare filename =
// authoring form; publishing later wraps it with a SOURCE/CID, exactly like a content layer — see CoverCache).
void NodeLayersSection::ApplyCoverImage(QLabel * CoverLabel, const QByteArray & Data, const QString & Extension, int NodeIdx, int LayerIdx)
{
    if (Data.isEmpty() || NodeIdx < 0 || NodeIdx >= (int)Model->doc()["NODES"].size()) return;

    const std::string NodeId = Model->doc()["NODES"][NodeIdx].value("NODE_ID", std::string());
    QString FileName = (NodeId.empty() ? QString("node%1").arg(NodeIdx) : QString::fromStdString(NodeId)) + "_cover." + Extension.toLower();
    QString DestPath = QDir::cleanPath(Model->packageDir()->path() + "/" + FileName);

    QFile OutFile(DestPath);
    if (!OutFile.open(QIODevice::WriteOnly)) { LogErr("NodeEditor", "Could not write cover: " + DestPath.toStdString()); return; }
    OutFile.write(Data); OutFile.close();

    Model->doc()[json::json_pointer(QString("/NODES/%1/LAYERS/%2/COVER").arg(NodeIdx).arg(LayerIdx).toStdString())] = FileName.toStdString();
    Model->SaveNodes();

    QPixmap Pix; Pix.loadFromData(Data);
    if (!Pix.isNull()) CoverLabel->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    LogSucc("NodeEditor", "Cover set: " + FileName.toStdString());
}

bool NodeLayersSection::eventFilter(QObject * obj, QEvent * event)
{
    QLabel * CoverLabel = qobject_cast<QLabel*>(obj);
    if (!CoverLabel || !CoverLabel->property("NodeArrayIndex").isValid() || !CoverLabel->property("LayerArrayIndex").isValid())
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
        int NodeIdx  = CoverLabel->property("NodeArrayIndex").toInt();
        int LayerIdx = CoverLabel->property("LayerArrayIndex").toInt();
        const QMimeData * Mime = ev->mimeData();

        if (Mime->hasImage())
        {
            QImage Img = qvariant_cast<QImage>(Mime->imageData());
            QByteArray Data; QBuffer Buf(&Data); Buf.open(QIODevice::WriteOnly); Img.save(&Buf, "PNG");
            ApplyCoverImage(CoverLabel, Data, "png", NodeIdx, LayerIdx);
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
                    ApplyCoverImage(CoverLabel, Data, Ext, NodeIdx, LayerIdx);
                }
            }
            else
            {
                //Async download — the old QEventLoop::exec() here blocked (and re-entered) the GUI event loop
                //for the whole HTTP round-trip. The reply lands via a queued signal; `this` as the connection
                //context auto-disconnects if the section is torn down before the reply arrives.
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply * Reply = NetMgr->get(QNetworkRequest(Url));
                QObject::connect(Reply, &QNetworkReply::finished, this, [this, Reply, CoverLabel, Url, NodeIdx, LayerIdx]() {
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
                        ApplyCoverImage(CoverLabel, Data, Ext, NodeIdx, LayerIdx);
                    }
                    else LogErr("NodeEditor", "Failed to download cover: " + Reply->errorString().toStdString());
                    Reply->deleteLater();
                });
            }
            return true;
        }
        if (Mime->hasText())
        {
            QUrl Url(Mime->text().trimmed());
            if (Url.isValid() && (Url.scheme() == "http" || Url.scheme() == "https"))
            {
                //Async — same rationale as the URL-drop branch above.
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply * Reply = NetMgr->get(QNetworkRequest(Url));
                QObject::connect(Reply, &QNetworkReply::finished, this, [this, Reply, CoverLabel, Url, NodeIdx, LayerIdx]() {
                    if (Reply->error() == QNetworkReply::NoError)
                    {
                        QByteArray Data = Reply->readAll();
                        QString Ext = QFileInfo(Url.path()).suffix(); if (Ext.isEmpty()) Ext = "png";
                        ApplyCoverImage(CoverLabel, Data, Ext, NodeIdx, LayerIdx);
                    }
                    Reply->deleteLater();
                });
            }
            return true;
        }
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

