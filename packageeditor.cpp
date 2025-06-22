#include "packageeditor.h"
#include "ui_packageeditor.h"
#include <iostream>

using json = nlohmann::ordered_json;
//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;

    InitPackage();
    InitMANIFESTJSON();
    BuildUI();
    RefreshJSONView();
}

PackageEditor::~PackageEditor()
{
    delete ui;
}

bool PackageEditor::InitMANIFESTJSON()
{
    if (PackageEditor::MANIFESTJSON->empty())
    {
        json RootJSONObject;
        for (auto Item : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].items())
        {
            if (!(Item.key() == "PATH"))
            {
                RootJSONObject[Item.key()] = nullptr;
            }
        }
        (*PackageEditor::MANIFESTJSON) = RootJSONObject;
    }
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    json NewSubGameObject;
    for (auto Item : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].items())
    {
        if (!(Item.key() == "PARENTPACKAGE"))
        {
            NewSubGameObject[Item.key()] = nullptr;
        }
    }
    (*MANIFESTJSON)[json::json_pointer("/SUBGAMES")].push_back(NewSubGameObject);
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    (*MANIFESTJSON)[json::json_pointer("/COMPONENTS")].push_back(json::object({{"NAME", nullptr}}));
    RefreshJSONView();

    QWidget * NewTabWidget = new QWidget(this);
    ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component " + QString::number(ui->PackageEditorTabWidget->count() - 1)));
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::on_SaveButton_clicked()
{
    if (PackageEditor::SaveManifestJSON())
    {
        qDebug().noquote() << QTime::currentTime() << "PackageEditor:" << "[OUT] Save successful.";
    }
    else
    {
        qDebug().noquote() << QTime::currentTime() << "PackageEditor:" << "[ERR] Save failed.";
    }
}


bool PackageEditor::SaveManifestJSON()
{
    return JSONOps::SaveJSON(PackageEditor::MANIFESTJSON, new QFile(MetadataDir->filePath("MANIFEST.json")));
}

void PackageEditor::RefreshJSONView()
{
    JSONTextEdit->setText(QString::fromStdString(MANIFESTJSON->dump(4)));
}

void PackageEditor::InitPackage()
{
    this->PackageDir = new QDir(QFileDialog::getExistingDirectory(this, "Select package directory..."));
    this->MetadataDir = new QDir(FSOps::SubPath((*PackageDir).path(), "METADATA"));
    this->PackageFilesDir = new QDir(FSOps::SubPath((*PackageDir).path(), "PACKAGEFILES"));

    if (!FSOps::CheckPackageValid(PackageDir))
    {
        MetadataDir->mkdir(MetadataDir->path());
        PackageFilesDir->mkdir(PackageFilesDir->path());
    }

    PackageEditor::MANIFESTJSON = JSONOps::LoadJSON(new QFile(MetadataDir->filePath("MANIFEST.json")));
    if (!(PackageEditor::MANIFESTJSON == nullptr))
    {
        return;
    }

    qDebug().noquote() << QTime::currentTime() << "PackageEditor:" << "[ERR] Parser returned nullptr, creating new JSON.";
    PackageEditor::MANIFESTJSON = new nlohmann::ordered_json;
}

bool PackageEditor::BuildUI()
{
    //JSON TAB
    ui->PackageEditorTabWidget->clear();

    PackageEditor::JSONTabWidget = new QWidget(ui->PackageEditorTabWidget);
    QVBoxLayout * JSONTabWidgetLayout = new QVBoxLayout(JSONTabWidget);
    JSONTabWidget->setLayout(JSONTabWidgetLayout);

        PackageEditor::JSONTextEdit = new QTextEdit(JSONTabWidget);
        JSONTabWidgetLayout->addWidget(JSONTextEdit);
        JSONTextEdit->setText(QString::fromStdString(MANIFESTJSON->dump(4)));
        QObject::connect(JSONTextEdit, &QTextEdit::textChanged, this, &PackageEditor::JSONQTextEditChanged);

        PackageEditor::SaveJSONButton = new QPushButton(JSONTabWidget);
        SaveJSONButton->setText("Save JSON");
        JSONTabWidgetLayout->addWidget(SaveJSONButton);
        QObject::connect(SaveJSONButton, &QPushButton::clicked, this, &PackageEditor::SaveJSONButtonPressed);

    ui->PackageEditorTabWidget->addTab(JSONTabWidget, "JSON");

    //MANIFEST TAB
    PackageEditor::ManifestTabWidget = new QWidget(ui->PackageEditorTabWidget);
    QVBoxLayout * ManifestTabWidgetLayout = new QVBoxLayout(ManifestTabWidget);
    ManifestTabWidget->setLayout(ManifestTabWidgetLayout);

        QGroupBox * PackageDataGroupBox = new QGroupBox(ManifestTabWidget);
        ManifestTabWidgetLayout->addWidget(PackageDataGroupBox);
        QFormLayout * PackageDataGroupBoxLayout = new QFormLayout(PackageDataGroupBox);
        PackageDataGroupBox->setLayout(PackageDataGroupBoxLayout);

        for (auto Item : (*PackageEditor::MANIFESTJSON).items())
        {
            if ((Item.key() == "SUBGAMES") || (Item.key() == "COMPONENTS"))
            {
                continue;
            }

            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Adding parameter editor:" << Item.key();
            QLineEdit * NewParamField = new QLineEdit(PackageDataGroupBox);
            QString JSONPath = QString::fromStdString(Item.key()).prepend("/");
            nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
            NewParamField->setProperty("JSONPath", JSONPath);

            if(!(*PackageEditor::MANIFESTJSON)[JSONPointer].is_null())
            {
                NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
            }

            QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
            PackageDataGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
        }
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Manifest tab done!";

        //SUBGAMES TABS WIDGET
        QTabWidget * SubGamesTabWidget = new QTabWidget(ManifestTabWidget);
        ManifestTabWidgetLayout->addWidget(SubGamesTabWidget);

        for (int i = 0; i < (*PackageEditor::MANIFESTJSON)["SUBGAMES"].size(); i++)
        {
            QWidget * SubGameTabWidget = new QWidget(SubGamesTabWidget);
            SubGameTabWidget->setProperty("JSONPath", QString("/SUBGAMES/%1").arg(QString::number(i)));
            QVBoxLayout * SubGameTabLayout = new QVBoxLayout(SubGameTabWidget);
            SubGameTabWidget->setLayout(SubGameTabLayout);

            QScrollArea * SubGameScrollArea = new QScrollArea(SubGameTabWidget);
            QVBoxLayout * SubGameScrollAreaLayout = new QVBoxLayout(SubGameScrollArea);
            SubGameScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            SubGameScrollArea->setWidgetResizable(1);
            SubGameScrollArea->setLayout(SubGameScrollAreaLayout);
            SubGameTabLayout->addWidget(SubGameScrollArea);

            QGroupBox * SubGameGroupBox = new QGroupBox(SubGameScrollArea);
            SubGameScrollAreaLayout->addWidget(SubGameGroupBox);
            SubGameScrollArea->setWidget(SubGameGroupBox);
            QFormLayout * SubGameGroupBoxLayout = new QFormLayout(SubGameGroupBox);
            SubGameGroupBox->setLayout(SubGameGroupBoxLayout);

            for (auto Item : (*PackageEditor::MANIFESTJSON)["SUBGAMES"][i].items())
            {
                qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Adding parameter editor:" << QString::fromStdString(Item.key());
                QLineEdit * NewParamField = new QLineEdit(SubGameGroupBox);
                QString JSONPath = QString("/SUBGAMES/%1/%2").arg(QString::number(i)).arg(QString::fromStdString(Item.key()));
                nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
                NewParamField->setProperty("JSONPath", JSONPath);

                if(!(*PackageEditor::MANIFESTJSON)[JSONPointer].is_null())
                {
                    NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
                }

                QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                SubGameGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
            }
            SubGamesTabWidget->addTab(SubGameTabWidget, QString("Subgame %1").arg(QString::number(i + 1)));
        }
        ui->PackageEditorTabWidget->addTab(ManifestTabWidget, "MANIFEST");

    //INDIVIDUAL COMPONENTS TABS
    for (int i = 0; i < (*PackageEditor::MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        QWidget * ComponentTabWidget = new QWidget(ui->PackageEditorTabWidget);
        ComponentTabWidget->setProperty("JSONPath", QString("/COMPONENTS/%1").arg(QString::number(i)));
        ComponentTabWidget->setProperty("Index", i);
        QVBoxLayout * ComponentTabWidgetLayout = new QVBoxLayout(ComponentTabWidget);
        ComponentTabWidget->setLayout(ComponentTabWidgetLayout);

            QGroupBox * ComponentNameGroupBox = new QGroupBox(ComponentTabWidget);
            ComponentTabWidgetLayout->addWidget(ComponentNameGroupBox);
            QFormLayout * ComponentNameGroupBoxLayout = new QFormLayout(ComponentNameGroupBox);
            ComponentNameGroupBox->setLayout(ComponentNameGroupBoxLayout);

                QLineEdit * ComponentNameField = new QLineEdit(ComponentNameGroupBox);
                QString ComponentNameFieldJSONPath = QString("/COMPONENTS/%1/NAME").arg(QString::number(i));
                nlohmann::ordered_json::json_pointer ComponentNameFieldJSONPointer(ComponentNameFieldJSONPath.toStdString());
                ComponentNameField->setProperty("JSONPath", ComponentNameFieldJSONPath);
                if(!(*PackageEditor::MANIFESTJSON)[ComponentNameFieldJSONPointer].is_null())
                {
                    ComponentNameField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[ComponentNameFieldJSONPointer]));
                }
                QObject::connect(ComponentNameField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                ComponentNameGroupBoxLayout->addRow("NAME", ComponentNameField);

            QHBoxLayout * SubComponentsToolbarLayout = new QHBoxLayout(ComponentTabWidget);
            ComponentTabWidgetLayout->addLayout(SubComponentsToolbarLayout);
            SubComponentsToolbarLayout->setSizeConstraint(QLayout::SetMinimumSize);

                QLabel * ParentComponentPickerLabel = new QLabel(ComponentTabWidget);
                ParentComponentPickerLabel->setText("Parent Component:");
                SubComponentsToolbarLayout->addWidget(ParentComponentPickerLabel);

                QComboBox * ParentComponentPicker = new QComboBox(ComponentTabWidget);
                ParentComponentPicker->addItem("None");
                for (int j = 1; j <= i; j++)
                {
                    ParentComponentPicker->addItem(QString::number(j));
                }
                QString ParentComponentJSONPath = QString("/COMPONENTS/%1/PARENTCOMPONENT").arg(QString::number(i));
                nlohmann::ordered_json::json_pointer ParentComponentJSONPointer(ParentComponentJSONPath.toStdString());
                ParentComponentPicker->setProperty("JSONPath", ParentComponentJSONPath);
                if(!(*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer].is_null())
                {
                    ParentComponentPicker->setCurrentIndex(QString::fromStdString((*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer]).toInt());
                }
                QObject::connect(ParentComponentPicker, &QComboBox::currentIndexChanged, this, &PackageEditor::ParentComponentChanged);
                SubComponentsToolbarLayout->addWidget(ParentComponentPicker);

                SubComponentsToolbarLayout->addStretch();

                QPushButton * RunExeButton = new QPushButton(ComponentTabWidget);
                RunExeButton->setText("Run EXE");
                SubComponentsToolbarLayout->addWidget(RunExeButton);
                QObject::connect(RunExeButton, &QPushButton::clicked, this, &PackageEditor::RunExeInComponent);

                QPushButton * BrowseFilesystemButton = new QPushButton(ComponentTabWidget);
                BrowseFilesystemButton->setText("Browse");
                SubComponentsToolbarLayout->addWidget(BrowseFilesystemButton);
                QObject::connect(BrowseFilesystemButton, &QPushButton::clicked, this, &PackageEditor::BrowseInComponent);

                QPushButton * EditRegistryButton = new QPushButton(ComponentTabWidget);
                EditRegistryButton->setText("Edit Registry");
                SubComponentsToolbarLayout->addWidget(EditRegistryButton);
                QObject::connect(EditRegistryButton, &QPushButton::clicked, this, &PackageEditor::RegeditInComponent);

                SubComponentsToolbarLayout->addStretch();

                QPushButton * AnalyzeButton = new QPushButton(ComponentTabWidget);
                AnalyzeButton->setText("Analyze Registry");
                SubComponentsToolbarLayout->addWidget(AnalyzeButton);
                QObject::connect(AnalyzeButton, &QPushButton::clicked, this, &PackageEditor::AnalyzeComponent);

                QPushButton * AddFileLayerButton = new QPushButton(ComponentTabWidget);
                AddFileLayerButton->setText("Add File Layer");
                SubComponentsToolbarLayout->addWidget(AddFileLayerButton);
                QObject::connect(AddFileLayerButton, &QPushButton::clicked, this, &PackageEditor::AddFileLayer);

                QPushButton * FinalizeButton = new QPushButton(ComponentTabWidget);
                FinalizeButton->setText("Finalize");
                SubComponentsToolbarLayout->addWidget(FinalizeButton);
                QObject::connect(FinalizeButton, &QPushButton::clicked, this, &PackageEditor::FinalizeComponent);

                QPushButton * RemoveComponentButton = new QPushButton(ComponentTabWidget);
                RemoveComponentButton->setText("Remove Component");
                SubComponentsToolbarLayout->addWidget(RemoveComponentButton);
                QObject::connect(RemoveComponentButton, &QPushButton::clicked, this, &PackageEditor::RemoveComponent);

                SubComponentsToolbarLayout->addStretch();

            QScrollArea * SubComponentsScrollArea = new QScrollArea(ComponentTabWidget);
            QVBoxLayout * SubComponentsScrollAreaLayout = new QVBoxLayout(SubComponentsScrollArea);
            SubComponentsScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            SubComponentsScrollArea->setWidgetResizable(1);
            SubComponentsScrollArea->setLayout(SubComponentsScrollAreaLayout);
            ComponentTabWidgetLayout->addWidget(SubComponentsScrollArea);

            QGroupBox * SubComponentsGroupBox = new QGroupBox(SubComponentsScrollArea);
            SubComponentsGroupBox->setTitle("Sub-Components");
            SubComponentsScrollAreaLayout->addWidget(SubComponentsGroupBox);
            SubComponentsScrollArea->setWidget(SubComponentsGroupBox);
            QFormLayout * SubComponentsGroupBoxLayout = new QFormLayout(SubComponentsGroupBox);
            SubComponentsGroupBox->setLayout(SubComponentsGroupBoxLayout);

            for (int j = 0; j < (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].size(); j++)
            {
                QString SubComponentType = QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["TYPE"]);
                QGroupBox * IndividualSubComponentGroupBox = new QGroupBox(SubComponentsGroupBox);
                QGridLayout * IndividualSubComponentGroupBoxLayout = new QGridLayout(IndividualSubComponentGroupBox);
                IndividualSubComponentGroupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                IndividualSubComponentGroupBox->setLayout(IndividualSubComponentGroupBoxLayout);

                IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("TYPE:"), 0, 0);
                IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(SubComponentType), 0, 1);

                if (SubComponentType == "ZipFileLayer")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("PATH:"), 1, 0);
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"])), 1, 1);
                }
                else if (SubComponentType == "DirLayer")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("PATH:"), 1, 0);
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"])), 1, 1);
                }
                else if (SubComponentType == "RegEdit")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("REGPATH:"), 1, 0);
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["REGPATH"])), 1, 1);
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("ARCHITECTURE:"), 2, 0);
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["ARCHITECTURE"])), 2, 1);

                        QGroupBox * RegKeysGroupBox = new QGroupBox(IndividualSubComponentGroupBox);
                        QGridLayout * RegKeysGroupBoxLayout = new QGridLayout(RegKeysGroupBox);
                        //RegKeysGroupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                        RegKeysGroupBox->setLayout(RegKeysGroupBoxLayout);
                        RegKeysGroupBox->setTitle("KEYS");

                        RegKeysGroupBoxLayout->addWidget(new QLabel("Key"), 0, 0);
                        RegKeysGroupBoxLayout->addWidget(new QLabel("Value"), 0, 1);
                        RegKeysGroupBoxLayout->addWidget(new QLabel("Type"), 0, 2);

                        int k = 1;
                        for (auto Object : (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"].items())
                        {
                            RegKeysGroupBoxLayout->addWidget(new QLabel(QString::fromStdString(Object.key())), k, 0);
                            RegKeysGroupBoxLayout->addWidget(new QLabel(QString::fromStdString(Object.value()["VALUE"])), k, 1);
                            RegKeysGroupBoxLayout->addWidget(new QLabel(QString::fromStdString(Object.value()["TYPE"])), k, 2);
                            k++;
                        }

                    IndividualSubComponentGroupBoxLayout->addWidget(RegKeysGroupBox, 3, 0, 1, -1);
                }
                else
                {
                    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << QString("[ERR] Component %1, subcomponent %2 has invalid type (%3).").arg(i).arg(j).arg(SubComponentType);
                    continue;
                }
                SubComponentsGroupBoxLayout->addWidget(IndividualSubComponentGroupBox);
            }
        ui->PackageEditorTabWidget->addTab(ComponentTabWidget, QString("Component %1").arg(QString::number(i + 1)));
    }

    return true;
}

void PackageEditor::JSONQLineEditChanged()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(QObject::sender());
    QString String = Editor->text();
    nlohmann::ordered_json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = String.toStdString();
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] JSON value:" << Editor->text() << "Submitted to:" << Editor->property("JSONPath").toString();
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}
void PackageEditor::JSONQTextEditChanged()
{
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] JSON TEXT EDITOR CHANGED";
    if (nlohmann::ordered_json::accept(PackageEditor::JSONTextEdit->toPlainText().toUtf8()))
    {
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Valid JSON!";
        PackageEditor::JSONTextEdit->setStyleSheet("");
        PackageEditor::SaveJSONButton->setDisabled(false);
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[ERR] Invalid JSON!";
        PackageEditor::JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
        PackageEditor::SaveJSONButton->setDisabled(true);
    }
}

void PackageEditor::ParentComponentChanged()
{
    QComboBox * ParentComponentPicker = qobject_cast<QComboBox *>(QObject::sender());
    QString Index = QString::number(ParentComponentPicker->currentIndex());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PARENT COMPONENT CHANGED!" << ParentComponentPicker->currentIndex() << ParentComponentPicker->currentText();

    nlohmann::ordered_json::json_pointer JSONPointer(ParentComponentPicker->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = Index.toStdString();
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] JSON value:" << ParentComponentPicker->currentIndex() << "Submitted to:" << ParentComponentPicker->property("JSONPath").toString();
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}

void PackageEditor::SaveJSONButtonPressed()
{
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] SAVE TRIGGERED!";
    (*PackageEditor::MANIFESTJSON) = nlohmann::ordered_json::parse(PackageEditor::JSONTextEdit->toPlainText().toUtf8());
    PackageEditor::SaveManifestJSON();
    BuildUI();
}

void PackageEditor::RemoveSubgame()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * SubgameTabWidget = Button->parentWidget();
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] REMOVE SUBGAME" << SubgameTabWidget->property("JSONPath");
}

void PackageEditor::RemoveComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * ComponentTabWidget = Button->parentWidget();
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] REMOVE COMPONENT" << ComponentTabWidget->property("JSONPath");

    nlohmann::ordered_json::json_pointer JSONPointer(ComponentTabWidget->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON).at(JSONPointer.parent_pointer()).erase(ComponentTabWidget->property("Index").toInt());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Deleted component:" << ComponentTabWidget->property("Index").toString();
    delete ComponentTabWidget;
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

void PackageEditor::RunExeInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Running EXE in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * ExeRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ExeRunner->Cleanup();
    ExeRunner->BuildRuntime();
    ExeRunner->Run(QFileDialog::getOpenFileName(this, "Select executable"), QStringList());
    ExeRunner->Cleanup();
}

void PackageEditor::BrowseInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Browsing in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * ExplorerRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ExplorerRunner->Cleanup();
    ExplorerRunner->BuildRuntime();
    ExplorerRunner->Run("explorer.exe");
    ExplorerRunner->Cleanup();
}

void PackageEditor::RegeditInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Editing registry in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * RegeditRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    RegeditRunner->Cleanup();
    RegeditRunner->BuildRuntime();
    RegeditRunner->Run("regedit.exe");
    RegeditRunner->Cleanup();
}

void PackageEditor::AnalyzeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Analyzing new component." << Button->parentWidget()->property("Index").toInt() + 1;

    int component = Button->parentWidget()->property("Index").toInt() + 1;
    int parentcomponent = 0;
    if ((!(*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"].is_null()) && (QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]) != "0"))
    {
        parentcomponent = QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component - 1]["PARENTCOMPONENT"]).toInt();
    }

    //QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    //QString NewComponentRuntimePath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RUNTIME");
    Runner * NewComponentRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, component - 1);
    NewComponentRunner->Cleanup();
    NewComponentRunner->BuildRuntime();

    //QString NewComponentComparatorUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "COMPARATORUSERDATA");
    QString ComparatorPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "COMPARATOR");
    Runner * ComparatorRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, parentcomponent);
    ComparatorRunner->Cleanup(ComparatorPath);
    ComparatorRunner->BuildRuntime(ComparatorPath, "READONLY");

    //The registry files are read into memory and compared. A delta JSON is generated for each pair.
    //The DeltaJSON is then converted into RegEdit subcomponents.
    //All existing RegEdit subcomponents of the current component are iterated through.
    //If a subcomponent with the same REGPATH already exists, the SUBKEYS are merged.
    //Else, a new subcomponent is created.
    nlohmann::ordered_json OldSysRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((ComparatorPath + QDir::separator() + "system.reg"))));
    nlohmann::ordered_json NewSysRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((NewComponentRunner->Paths["RuntimePath"] + QDir::separator() + "system.reg"))));
    //nlohmann::ordered_json SysRegDeltaJSON = SubtractJSON(OldSysRegJSON, NewSysRegJSON);
    nlohmann::ordered_json SysDeltaSubComponentArray = RegDeltaToSubComponentArray(SubtractJSON(OldSysRegJSON, NewSysRegJSON), "HKLM");
    //qDebug() << SysDeltaSubComponentArray.dump();

    for (int i = 0; i < SysDeltaSubComponentArray.size(); i++)
    {
        qDebug().noquote() << SysDeltaSubComponentArray[i].dump();
        for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"].size(); j++)
        {
            //If the component is not a RegEdit type, skip.
            if (!((*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][j]["TYPE"] == "RegEdit"))
            {
                qDebug().noquote() << "NOT REGEDIT TYPE";
                continue;
            }

            //qDebug().noquote() << "COMPARING: ================================================================================";
            //qDebug().noquote() << QString::fromStdString(SysDeltaSubComponentArray[i]["REGPATH"]);
            //qDebug().noquote() << QString::fromStdString(((*MANIFESTJSON)["COMPONENTS"][component]["SUBCOMPONENTS"][j]["REGPATH"]));
            //Potential problem if two subcomponents have the same path but different architecture!
            if (SysDeltaSubComponentArray[i]["REGPATH"] == ((*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][j]["REGPATH"]))
            {
                for (auto KeyValueObject : SysDeltaSubComponentArray[i]["KEYVALUES"].items())
                {
                    (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"]["KEYVALUES"][KeyValueObject.key()] = KeyValueObject.value();
                }
                goto SkipMergeNewSubComponent;
            }
        }

        //The current subcomponent is merged into MANIFESTJSON.
        (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][(*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"].size()] = SysDeltaSubComponentArray[i];
        SkipMergeNewSubComponent:
    }

    //JSONOps::SaveJSON(&SysRegDeltaJSON, new QFile("SYSDELTA.json"));
    //nlohmann::ordered_json SysRegDeltaFLATJSON = SysRegDeltaJSON.flatten();
    //JSONOps::SaveJSON(&SysRegDeltaFLATJSON, new QFile("SYSDELTAFLAT.json"));
    //qDebug().noquote() << RegDeltaToSubComponentArray(SysRegDeltaJSON, "HKLM").dump();

    nlohmann::ordered_json OldUsrRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((ComparatorPath + QDir::separator() + "user.reg"))));
    nlohmann::ordered_json NewUsrRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((NewComponentRunner->Paths["RuntimePath"] + QDir::separator() + "user.reg"))));
    //nlohmann::ordered_json UsrRegDeltaJSON = SubtractJSON(OldUsrRegJSON, NewUsrRegJSON);
    nlohmann::ordered_json UsrDeltaSubComponentArray = RegDeltaToSubComponentArray(SubtractJSON(OldUsrRegJSON, NewUsrRegJSON), "HKCU");

    for (int i = 0; i < UsrDeltaSubComponentArray.size(); i++)
    {
        for (int j = 0; j < (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"].size(); j++)
        {
            //If the component is not a RegEdit type, skip.
            if (!((*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][j]["TYPE"] == "RegEdit"))
            {
                continue;
            }
            //Potential problem if two subcomponents have the same path but different architecture!
            if (UsrDeltaSubComponentArray[i]["REGPATH"] == ((*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][j]["REGPATH"]))
            {
                for (auto KeyValueObject : UsrDeltaSubComponentArray[i]["KEYVALUES"].items())
                {
                    (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"]["KEYVALUES"][KeyValueObject.key()] = KeyValueObject.value();
                }
                goto SkipMergeNewSubComponent2;
            }
        }

        //The current subcomponent is merged into MANIFESTJSON.
        (*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"][(*MANIFESTJSON)["COMPONENTS"][component - 1]["SUBCOMPONENTS"].size()] = UsrDeltaSubComponentArray[i];
    SkipMergeNewSubComponent2:
    }

    //JSONOps::SaveJSON(&UsrRegDeltaJSON, new QFile("USRDELTA.json"));
    //nlohmann::ordered_json UsrRegDeltaFLATJSON = UsrRegDeltaJSON.flatten();
    //JSONOps::SaveJSON(&UsrRegDeltaFLATJSON, new QFile("USRDELTAFLAT.json"));
    //qDebug().noquote() << RegDeltaToSubComponentArray(UsrRegDeltaJSON, "HKCU").dump();

    //nlohmann::ordered_json OldUsrDefRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((ComparatorPath + QDir::separator() + "userdef.reg"))));
    //nlohmann::ordered_json NewUsrDefRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((NewComponentRunner->Paths["RuntimePath"] + QDir::separator() + "userdef.reg"))));
    //nlohmann::ordered_json UsrDefRegDeltaJSON = SubtractJSON(OldUsrDefRegJSON, NewUsrDefRegJSON);
    //JSONOps::SaveJSON(&UsrDefRegDeltaJSON, new QFile("USRDEFDELTA.json"));

    NewComponentRunner->Cleanup();
    ComparatorRunner->Cleanup(ComparatorPath);

    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

nlohmann::ordered_json PackageEditor::RegDeltaToSubComponentArray(nlohmann::ordered_json RegDeltaJSON, QString Hive)
{
    nlohmann::ordered_json SubComponentArray = nlohmann::ordered_json::array();
    RegDeltaJSON = RegDeltaJSON.flatten();
    for (auto Item : RegDeltaJSON.items())
    {
        //Exclude useless windows and wine registry keys form further processing.
        //Excluded paths must be exposed in settings.
        //Use a nice loop for those values, not 100x if's.
        if (QString::fromStdString(Item.key()).contains("Software/Microsoft/Windows"))
        {
            continue;
        }

        if (QString::fromStdString(Item.key()).contains("Software/Microsoft/Cryptography"))
        {
            continue;
        }

        if (QString::fromStdString(Item.key()).contains("Software/Wow6432Node/Microsoft/Windows"))
        {
            continue;
        }

        if (QString::fromStdString(Item.key()).contains("Software/Classes"))
        {
            continue;
        }

        if (QString::fromStdString(Item.key()).contains("Software/Microsoft/ActiveMovie"))
        {
            continue;
        }

        if (QString::fromStdString(Item.key()).contains("Software/Wine"))
        {
            continue;
        }

        QString REGPATH = QString::fromStdString(Item.key()).prepend(Hive);
        //Determine 64/32 bit architecture based on presence of "Wow6432Node" in the path string.
        QString ARCHITECTURE = "64";
        if (REGPATH.contains("Wow6432Node"))
        {
            ARCHITECTURE = "32";
        }

        //Split the REGPATH by separator.
        QStringList REGPATHTokens = REGPATH.split("/");
        //Extract the key names (the last item in the path).
        QString KEY = REGPATHTokens.last();
        //Remove the key name form the path object.
        REGPATHTokens.removeLast();
        //Reconstruct the REGPATH with "\\" separators.
        //The "Wow6432Node item is skipped.
        REGPATH.clear();
        for (QString Token : REGPATHTokens)
        {
            if (Token == "Wow6432Node")
            {
                continue;
            }
            REGPATH.append(Token);
            REGPATH.append("\\");
        }
        //Remove the last 2 chars (extrenuous "\\").
        REGPATH.chop(1); //No idea why using chop(1) actually chops 2 chars ?!
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Interpreting registry key:" << "REGPATH:" << REGPATH << "KEY:" << KEY;

        nlohmann::ordered_json KeyValueObject = nlohmann::ordered_json::object();
        QString VALUE = QString::fromStdString(Item.value());
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Raw value:" << VALUE;
        //Processing of value based on value type.
        if (VALUE.first(6) == "dword:")
        {
            KeyValueObject["TYPE"] = "REG_DWORD";
            VALUE.remove(0, 6);
            VALUE = QString::number(VALUE.toUInt(nullptr, 16));
            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Processed value:" << VALUE;
        }
        else if (VALUE.first(4) == "hex:")
        {
            KeyValueObject["TYPE"] = "REG_BINARY";
            VALUE.remove(0, 4);
            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Processed value:" << VALUE;
            //No further processing needed, the binary value is passed as comma separated string.
        }
        else if (VALUE.first(7) == "hex(b):")
        {
            KeyValueObject["TYPE"] = "REG_QWORD";
            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[ERR] Unsupported REG_QWORD value type!";
            continue;
            //VALUE.remove(0, 7);
            //VALUE.remove(",");
            //qDebug() << "QBYTEARRAY" << QByteArray::fromHex(VALUE.toUtf8());
            //VALUE = QString::number(QByteArray::fromHex(VALUE.toUtf8()).toUInt(nullptr, 16));
            //qDebug().noquote() << "VALUE PROCESSED:" << VALUE;
        }
        else if (VALUE.first(7) == "str(7):")
        {
            KeyValueObject["TYPE"] = "REG_MULTI_SZ";
            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[ERR] Unsupported REG_MULTI_SZ value type!";
            continue;
            //VALUE.remove(0, 6);
            //qDebug() << "VALUE CHOPPED:" << VALUE;
            //VALUE = QString::number(VALUE.toUInt(nullptr, 16));
            //qDebug().noquote() << "VALUE PROCESSED:" << VALUE;
        }
        else if (VALUE.first(7) == "str(2):")
        {
            KeyValueObject["TYPE"] = "REG_EXPAND_SZ";
            qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[ERR] Unsupported REG_EXPAND_SZ value type!";
            continue;
            //VALUE.remove(0, 7);
            //qDebug() << "VALUE CHOPPED:" << VALUE;
            //VALUE = QString::number(VALUE.toUInt(nullptr, 16));
            //qDebug().noquote() << "VALUE PROCESSED:" << VALUE;
        }
        else
        {
            //REG_SZ values must be cleaned of extrenuous backslashes introduced along the way.
            KeyValueObject["TYPE"] = "REG_SZ";
            VALUE = VALUE.replace("\\\\", "\\");
        }
        KeyValueObject["VALUE"] = VALUE.toStdString();

        //If there is another subcomponent with the same REGPATH, add the current item as a KEYVALUE.
        //If there is not, create a new subcomponent and add KEYVALUE.

        nlohmann::ordered_json NewSubComponentObject = nlohmann::ordered_json::object();
        //Make this a function to avoid nested for loop shenanigains.

        for (int i = 0; i < SubComponentArray.size(); i++)
        {
            if (SubComponentArray[i]["REGPATH"] == REGPATH.toStdString())
            {
                qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Subcomponent with same REGPATH exists. Merging.";
                SubComponentArray[i]["KEYVALUES"][KEY.toStdString()] = KeyValueObject;
                goto SkipNewSubComponentObjectContruct;
            }
        }

        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] Constructing new RegEdit subcomponent.";
        NewSubComponentObject["TYPE"] = "RegEdit";
        NewSubComponentObject["REGPATH"] = REGPATH.toStdString();
        NewSubComponentObject["ARCHITECTURE"] = ARCHITECTURE.toStdString();
        NewSubComponentObject["KEYVALUES"][KEY.toStdString()] = KeyValueObject;
        qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] New subcomponent:" << NewSubComponentObject.dump();
        SubComponentArray[SubComponentArray.size()] = NewSubComponentObject;
        SkipNewSubComponentObjectContruct:
    }
    return SubComponentArray;
}

nlohmann::ordered_json PackageEditor::SubtractJSON(nlohmann::ordered_json OldJSON, nlohmann::ordered_json NewJSON)
{
    nlohmann::ordered_json DiffJSON = nlohmann::ordered_json(nlohmann::ordered_json::diff(OldJSON, NewJSON));
    nlohmann::ordered_json DeltaJSON = nlohmann::ordered_json();

    for (int i = 0; i < DiffJSON.size(); i++)
    {
        //All items in the DiffJSON are iterated over.
        //The DeltaJSON is recreating by using the "path" as json pointers.
        DeltaJSON[nlohmann::ordered_json::json_pointer(DiffJSON.at(i)["path"])] = DiffJSON.at(i)["value"];
    }

    return DeltaJSON;
}

nlohmann::ordered_json PackageEditor::RegFileToJSON(QFile RegFile)
{

    nlohmann::ordered_json RegJSON = nlohmann::ordered_json();
    if (RegFile.open(QFile::ReadOnly | QFile::Text))
    {
        QString RegFileString = RegFile.readAll();

        QRegularExpression KeysRegex("\\[(Software.+?)\\] [0-9]{7,15}(.+?)(?=\\[.+?\\])", QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatchIterator KeysRegexExtractor = KeysRegex.globalMatch(RegFileString);
        while (KeysRegexExtractor.hasNext())
        {
            QRegularExpressionMatch ExtractedKey = KeysRegexExtractor.next();
            QStringList KeyPathTokens(ExtractedKey.captured(1).split("\\\\"));
            QString KeyPathString;
            for (int i = 0; i < KeyPathTokens.size(); ++i)
            {
                //If the path contains numerical tokens, they need to be quoted, so they are not interpreted as JSON array indices.
                if (QRegularExpression("^\\d+$").match(KeyPathTokens[i]).hasMatch())
                {
                    KeyPathTokens[i] = "\"" + KeyPathTokens[i] + "\"";
                }
                KeyPathString.append("/").append(KeyPathTokens[i]);
            }
            //qDebug().noquote() << "KEY:" << KeyPathString;
            nlohmann::ordered_json::json_pointer JSONPointer(KeyPathString.toStdString());

            //The regex should match the entire contents of the key, including all subkeys.
            //In order to account for multi-line keys (like hex values), the specific sequence "\\\n  " is removed, thus making them single line.
            //Since all subkeys should now be single line, the entite match is then split by line to get the infividual key-value pairs.
            QStringList SubKeys(ExtractedKey.captured(2).remove("\\\n  ").split("\n"));
            for (int i = 0; i < SubKeys.size(); i++)
            {
                if (SubKeys.at(i).isEmpty())
                {
                    SubKeys.removeAt(i);
                    i--;
                    continue;
                }
                if (SubKeys.at(i).left(5) == "#time")
                {
                    SubKeys.removeAt(i);
                    i--;
                    continue;
                }
                if (!SubKeys.at(i).contains("="))
                {
                    SubKeys.removeAt(i);
                    i--;
                    continue;
                }

                QString Subkey(PackageEditor::UnquoteString(SubKeys.at(i).split("=").at(0)));
                if (SubKeys.at(i).split("=").size() == 1)
                {
                    RegJSON[JSONPointer][Subkey.toStdString()] = "";
                    continue;
                }

                QString Value(PackageEditor::UnquoteString(SubKeys.at(i).split("=").at(1)));
                RegJSON[JSONPointer][Subkey.toStdString()] = Value.toStdString();
            }
        }
    }
    return RegJSON;
}

void PackageEditor::AddFileLayer()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageEditor:" << "Adding DirLayer in component" << Button->parentWidget()->property("Index").toInt() + 1;

    QString UserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "USERDATA");
    QString SelectedProgramDir = QFileDialog::getExistingDirectory(this, "Select ProgramDir", UserDataPath);
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    QString ComponentName = QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]["NAME"]);

    (*PackageEditor::MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "DirLayer"}, {"PATH", ComponentName.toStdString()}}));

    QDir DirMover;
    DirMover.mkdir(PackageFilesDir->path());
    DirMover.rename(SelectedProgramDir, QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + ComponentName));

    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

void PackageEditor::FinalizeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageEditor:" << "Finalizing component" << Button->parentWidget()->property("Index").toInt() + 1;
}

QString PackageEditor::UnquoteString(QString InputString)
{
    if(InputString.startsWith('"') && InputString.endsWith('"') && InputString.size() > 1)
    {
        return InputString.mid(1, InputString.length() - 2);
    }
    else
    {
        return InputString;
    }
};
