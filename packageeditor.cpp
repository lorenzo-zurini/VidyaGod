#include "packageeditor.h"
#include "ui_packageeditor.h"

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
                else if (SubComponentType == "FileLayer")
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
                        RegKeysGroupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
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

    QString NewComponentRuntimePath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RUNTIME");
    QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    ExeRunner->Cleanup(NewComponentRuntimePath);
    ExeRunner->BuildRuntime(NewComponentRuntimePath, FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "USERDATA"));
    ExeRunner->Run(QFileDialog::getOpenFileName(this, "Select executable"), QStringList(), NewComponentRuntimePath);
    ExeRunner->Cleanup(NewComponentRuntimePath);
}

void PackageEditor::BrowseInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Browsing in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * ExplorerRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    QString NewComponentRuntimePath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RUNTIME");
    QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    ExplorerRunner->Cleanup(NewComponentRuntimePath);
    ExplorerRunner->BuildRuntime(NewComponentRuntimePath, FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "USERDATA"));
    ExplorerRunner->Run("explorer.exe", QStringList(), NewComponentRuntimePath);
    ExplorerRunner->Cleanup(NewComponentRuntimePath);
}

void PackageEditor::RegeditInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Editing registry in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * RegeditRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    QString NewComponentRuntimePath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RUNTIME");
    QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    RegeditRunner->Cleanup(NewComponentRuntimePath);
    RegeditRunner->BuildRuntime(NewComponentRuntimePath, NewComponentUserDataPath);
    RegeditRunner->Run("regedit.exe", QStringList(), NewComponentRuntimePath);
    RegeditRunner->Cleanup(NewComponentRuntimePath);
}

void PackageEditor::AnalyzeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Analyzing new component."; // << Button->parentWidget()->property("Index").toInt() + 1;

    int component = Button->parentWidget()->property("Index").toInt();
    int parentcomponent = 0;
    if ((!(*MANIFESTJSON)["COMPONENTS"][component]["PARENTCOMPONENT"].is_null()) && (QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component]["PARENTCOMPONENT"]) != "0"))
    {
        parentcomponent = QString::fromStdString((*MANIFESTJSON)["COMPONENTS"][component]["PARENTCOMPONENT"]).toInt();
    }

    QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    QString NewComponentRuntimePath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RUNTIME");
    Runner * NewComponentRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, component);
    NewComponentRunner->Cleanup(NewComponentRuntimePath);
    NewComponentRunner->BuildRuntime(NewComponentRuntimePath, NewComponentUserDataPath);

    //QString NewComponentComparatorUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "COMPARATORUSERDATA");
    QString NewComponentComparatorPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "COMPARATOR");
    Runner * ComparatorRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, parentcomponent);
    ComparatorRunner->Cleanup(NewComponentComparatorPath);
    ComparatorRunner->BuildRuntime(NewComponentComparatorPath, "READONLY");

    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST COMPARATOR");


    nlohmann::ordered_json * OldSysReg = new nlohmann::ordered_json;

    QFile RegFile(QDir::cleanPath((NewComponentComparatorPath + QDir::separator() + "system.reg")));
    QStringList KeysList;

    if (RegFile.open(QFile::ReadOnly | QFile::Text))
    {
        //WorkingFileString.split(QRegularExpression("<tr><td class=\"tdnplus\">\\s*?PREZENTARE\\s[0-9]*?.*?<\\/td><\\/tr>", QRegularExpression::DotMatchesEverythingOption));
        QString RegFileString = RegFile.readAll();
        //qDebug() << RegFileString;


        QRegularExpression KeysRegex("(\\[.+?\\]) [0-9]{7,15}(.*?)(?=\\[.+?\\])", QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatchIterator KeysRegexExtractor = KeysRegex.globalMatch(RegFileString);
        while (KeysRegexExtractor.hasNext())
        {
            QRegularExpressionMatch ExtractedKey = KeysRegexExtractor.next();
            QString KeyPath(ExtractedKey.captured(1));
            QStringList SubKeys(ExtractedKey.captured(2).split("\n"));

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
                //DISCRIMINATE TYPES OF REGISTRY KEYS
            }

            qDebug().noquote() << "KEY:" << KeyPath;
            qDebug().noquote() << "CONTENTS:" << SubKeys;
        }


        //KeysList = QString(RegFile.readAll()).split(QRegularExpression("(\[.+\])"));//, QRegularExpression::DotMatchesEverythingOption));

        /*
        while (!RegFile.atEnd())
        {
            LinesList.append(RegFile.readLine());
        }
        */
    }


    for (QString Key : KeysList)
    {
        qDebug().noquote() << Key;
    }

    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST COMPARATOR");
    /*
    QString RegCmpPath(QDir::cleanPath(QCoreApplication::applicationDirPath() + QDir::separator() + "registrychangesview-x64" + QDir::separator() + "RegistryChangesView.exe"));

    ///////////////////////////////////////////////////////////////////////////////////////////////
    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TAKING OLDSNAP");

    QDir OldSnap(QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT") + QDir::separator() + "OLDREG");
    OldSnap.mkpath(OldSnap.path());
    QProcess * OldRegCmpProcess = new QProcess;
    OldRegCmpProcess->setProgram("umu-run");

    QProcessEnvironment OldRegCmpProcessEnvironment = QProcessEnvironment::systemEnvironment();
    OldRegCmpProcessEnvironment.insert("PROTONPATH", "/usr/share/steam/compatibilitytools.d/proton-ge-custom/");
    OldRegCmpProcessEnvironment.insert("WINEPREFIX", NewComponentComparatorPath);
    OldRegCmpProcessEnvironment.insert("GAMEID", 0);
    OldRegCmpProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    OldRegCmpProcess->setArguments(QStringList() << RegCmpPath << "/CreateSnapshot" << OldSnap.path());

    OldRegCmpProcess->setProcessEnvironment(OldRegCmpProcessEnvironment);
    OldRegCmpProcess->start();
    OldRegCmpProcess->waitForFinished(-1);
    qDebug().noquote() << OldRegCmpProcess->readAllStandardError();
    qDebug().noquote() << OldRegCmpProcess->readAllStandardOutput();

    delete OldRegCmpProcess;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST NEWSNAP");

    QDir NewSnap(QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT") + QDir::separator() + "NEWREG");
    NewSnap.mkpath(NewSnap.path());
    QProcess * NewRegCmpProcess = new QProcess;
    NewRegCmpProcess->setProgram("umu-run");

    QProcessEnvironment NewRegCmpProcessEnvironment = QProcessEnvironment::systemEnvironment();
    NewRegCmpProcessEnvironment.insert("PROTONPATH", "/usr/share/steam/compatibilitytools.d/proton-ge-custom/");
    NewRegCmpProcessEnvironment.insert("WINEPREFIX", NewComponentRuntimePath);
    NewRegCmpProcessEnvironment.insert("GAMEID", 0);
    NewRegCmpProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    NewRegCmpProcess->setArguments(QStringList() << RegCmpPath << "/CreateSnapshot" << NewSnap.path());

    NewRegCmpProcess->setProcessEnvironment(NewRegCmpProcessEnvironment);
    NewRegCmpProcess->start();
    NewRegCmpProcess->waitForFinished(-1);
    qDebug().noquote() << NewRegCmpProcess->readAllStandardError();
    qDebug().noquote() << NewRegCmpProcess->readAllStandardOutput();

    delete NewRegCmpProcess;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    QMessageBox::warning(nullptr, "TEST COMPARATOR", "COMPARING SNAPS");

    QProcess * RegCmpProcess = new QProcess;
    RegCmpProcess->setProgram("umu-run");

    QProcessEnvironment RegCmpProcessEnvironment = QProcessEnvironment::systemEnvironment();
    RegCmpProcessEnvironment.insert("PROTONPATH", "/usr/share/steam/compatibilitytools.d/proton-ge-custom/");
    RegCmpProcessEnvironment.insert("WINEPREFIX", NewComponentRuntimePath);
    RegCmpProcessEnvironment.insert("GAMEID", 0);
    RegCmpProcessEnvironment.insert("PROTON_VERB", "waitforexitandrun");

    RegCmpProcess->setArguments(QStringList() << RegCmpPath \
                                                 << "/DataSourceDirection" << "2" \
                                                 << "/DataSourceType1" << "2" \
                                                 << "/DataSourceType2" << "2" \
                                                 << "/RegSnapshotPath1" << OldSnap.path() \
                                                 << "/RegSnapshotPath2" << NewSnap.path() \
                                                 << "/sreg" << QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "RegDiff.reg"));

    RegCmpProcess->setProcessEnvironment(RegCmpProcessEnvironment);
    RegCmpProcess->start();
    RegCmpProcess->waitForFinished(-1);
    qDebug().noquote() << RegCmpProcess->readAllStandardError();
    qDebug().noquote() << RegCmpProcess->readAllStandardOutput();

    delete RegCmpProcess;

    ///////////////////////////////////////////////////////////////////////////////////////////////
    */

    //QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST COMPARATOR");
    //QProcess * DiffProcess = new QProcess(this);
    //DiffProcess->setProgram("diff");
    //DiffProcess->setArguments({QDir(NewComponentComparatorPath).filePath("system.reg"), QDir(NewComponentUserDataPath).filePath("system.reg")});
    //QStringList ExeArgs;
    //ExeArgs << QDir(NewComponentComparatorPath).filePath("system.reg") << QDir(NewComponentUserDataPath).filePath("system.reg");
    //qDebug() << ExeArgs;

    //DiffProcess->start("diff", ExeArgs);
    //DiffProcess->waitForFinished(-1);
    //qDebug().noquote() << DiffProcess->readAllStandardError();
    //QByteArray DiffOutput(DiffProcess->readAllStandardOutput());

    //qDebug() << DiffOutput.toStdString();

    //QSettings * NewSysReg = new QSettings(QDir(NewComponentUserDataPath).filePath("system.reg"), QSettings::IniFormat);
    //QSettings * OldSysReg = new QSettings(QDir(NewComponentComparatorPath).filePath("system.reg"), QSettings::IniFormat);

    //NewSysReg->beginGroup("System");
    //OldSysReg->beginGroup("System");

    //qDebug() << "CHILDGROUPS:" << NewSysReg->childGroups();

    //QSettings * NewUsrReg = new QSettings(QDir(NewComponentUserDataPath).filePath("user.reg"), QSettings::IniFormat);
    //QSettings * OldUsrReg = new QSettings(QDir(NewComponentComparatorPath).filePath("user.reg"), QSettings::IniFormat);

    //QSettings * NewUsrDefReg = new QSettings(QDir(NewComponentUserDataPath).filePath("userdef.reg"), QSettings::IniFormat);
    //QSettings * OldUsrDefReg = new QSettings(QDir(NewComponentComparatorPath).filePath("userdef.reg"), QSettings::IniFormat);

    //qDebug() << NewSysReg;


    /*
    //ATTEMPT TO MAKE FILESYSTEM DIFF BELOW, VERY HACKY AND SUPERFLUOUS
    QDir NewComponentRuntimeDir(QDir::cleanPath(NewComponentRuntimePath + QDir::separator() + "drive_c"));
    NewComponentRuntimeDir.setFilter(QDir::Files);
    QFileInfoList * NewComponentRuntimeFileList = new QFileInfoList;
    QDirIterator NewComponentRuntimeDirIterator(NewComponentRuntimeDir, QDirIterator::Subdirectories);
    while (NewComponentRuntimeDirIterator.hasNext())
    {
        NewComponentRuntimeFileList->append(NewComponentRuntimeDirIterator.nextFileInfo());
    }

    QDir NewComponentComparatorDir(QDir::cleanPath(NewComponentComparatorPath + QDir::separator() + "drive_c"));
    NewComponentComparatorDir.setFilter(QDir::Files);
    QFileInfoList * NewComponentComparatorDirFileList = new QFileInfoList;
    QDirIterator NewComponentComparatorDirIterator(NewComponentComparatorDir, QDirIterator::Subdirectories);
    while (NewComponentComparatorDirIterator.hasNext())
    {
        NewComponentComparatorDirFileList->append(NewComponentComparatorDirIterator.nextFileInfo());
    }

    QMessageBox::warning(nullptr, "TEST COMPARATOR", "RUNNING COMPARISON");
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageEditor:" << "COMPARATOR FILE COUNT:" << NewComponentComparatorDirFileList->count();
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageEditor:" << "RUNTIME FILE COUNT:" << NewComponentRuntimeFileList->count();

    QMessageBox::warning(nullptr, "TEST COMPARATOR", "RUNNING COMPARISON");

    for (int i = 0; i < NewComponentRuntimeFileList->count(); i++)
    {
        QFileInfo NewFile = NewComponentRuntimeFileList->at(i);
        for (int j = 0; j < NewComponentComparatorDirFileList->count(); j++)
        {
            QFileInfo OldFile = NewComponentComparatorDirFileList->at(j);
            if (NewFile.filePath().slice(NewComponentRuntimeDir.path().size()) == (OldFile.filePath().slice(NewComponentComparatorDir.path().size())))
            {
                if (NewFile.lastModified() == OldFile.lastModified())
                {
                    //qDebug() << QString("File %1 exists in both directories.").arg(NewFile.filePath().slice(NewComponentRuntimeDir.path().size()));
                    NewComponentRuntimeFileList->removeAt(i);
                    i--;
                }
                else
                {
                    qDebug() << QString("MODIFIED FILE %1").arg(NewFile.filePath().slice(NewComponentRuntimeDir.path().size()));
                }
                break;
            }
        }
    }

    for (int i = 0; i < NewComponentRuntimeFileList->count(); i++)
    {
        qDebug() << "NEWFILE:" << NewComponentRuntimeFileList->at(i).filePath();
    }

    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST COMPARATOR");

    delete NewComponentComparatorDirFileList;
    delete NewComponentRuntimeFileList;
    */


    NewComponentRunner->Cleanup(NewComponentRuntimePath);
    ComparatorRunner->Cleanup(NewComponentComparatorPath);
}

void PackageEditor::AddFileLayer()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] PackageEditor:" << "Adding FileLayer in component" << Button->parentWidget()->property("Index").toInt() + 1;

    QString NewComponentUserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "NEWCOMPONENT" + QDir::separator() + "USERDATA");
    QString SelectedProgramDir = QFileDialog::getExistingDirectory(this, "Select ProgramDir", NewComponentUserDataPath);
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    QString ComponentName = QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]["NAME"]);

    (*PackageEditor::MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "FileLayer"}, {"PATH", ComponentName.toStdString()}}));

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
