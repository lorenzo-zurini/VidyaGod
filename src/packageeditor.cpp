#include "packageeditor.h"
#include "ui_packageeditor.h"
#include <iostream>

using json = nlohmann::ordered_json;

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
        std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Save successful." << std::endl;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[ERR] Save failed." << std::endl;
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

    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[ERR] Parser returned nullptr, creating new JSON." << std::endl;
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

            std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Adding parameter editor:" << Item.key() << std::endl;
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
        std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Manifest tab done!" << std::endl;

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
                std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Adding parameter editor:" << Item.key() << std::endl;
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

                QPushButton * ExecuteComponentButton = new QPushButton(ComponentTabWidget);
                ExecuteComponentButton->setText("Execute Component");
                SubComponentsToolbarLayout->addWidget(ExecuteComponentButton);
                QObject::connect(ExecuteComponentButton, &QPushButton::clicked, this, &PackageEditor::ExecuteComponent);

                SubComponentsToolbarLayout->addStretch();

                QPushButton * AnalyzeButton = new QPushButton("Analyze Registry", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AnalyzeButton);
                QObject::connect(AnalyzeButton, &QPushButton::clicked, this,
                [this, i, ParentComponentJSONPointer, ParentComponentJSONPath]
                {
                    //qDebug() << "lambda entered";
                    //qDebug() << "i + 1=" << i + 1;
                    //qDebug() << "ParentComponentJSONPointer=" << ParentComponentJSONPath.toStdString();
                    //CAUTION!!!! THIS CRASHES IF PARENTCOMPONENT IS NULL!! SET DEFAULT TO 0!!
                    //qDebug() << "QString::fromStdString((*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer]).toInt() =" << QString::fromStdString((*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer]).toInt();
                    this->CompareComponentsRegistry(QString::fromStdString((*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer]).toInt(), i + 1);
                });

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
                    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << QString("[ERR] Component %1, subcomponent %2 has invalid type (%3).").arg(i).arg(j).arg(SubComponentType).toStdString() << std::endl;
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
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] JSON value:" << Editor->text().toStdString() << "Submitted to:" << Editor->property("JSONPath").toString().toStdString() << std::endl;
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}
void PackageEditor::JSONQTextEditChanged()
{
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] JSON TEXT EDITOR CHANGED" << std::endl;
    if (nlohmann::ordered_json::accept(PackageEditor::JSONTextEdit->toPlainText().toUtf8()))
    {
        std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Valid JSON!" << std::endl;
        PackageEditor::JSONTextEdit->setStyleSheet("");
        PackageEditor::SaveJSONButton->setDisabled(false);
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[ERR] Invalid JSON!" << std::endl;
        PackageEditor::JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
        PackageEditor::SaveJSONButton->setDisabled(true);
    }
}

void PackageEditor::ParentComponentChanged()
{
    QComboBox * ParentComponentPicker = qobject_cast<QComboBox *>(QObject::sender());
    QString Index = QString::number(ParentComponentPicker->currentIndex());
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] PARENT COMPONENT CHANGED!" << ParentComponentPicker->currentIndex() << ParentComponentPicker->currentText().toStdString() << std::endl;

    nlohmann::ordered_json::json_pointer JSONPointer(ParentComponentPicker->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = Index.toStdString();
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] JSON value:" << ParentComponentPicker->currentIndex() << "Submitted to:" << ParentComponentPicker->property("JSONPath").toString().toStdString() << std::endl;
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}

void PackageEditor::SaveJSONButtonPressed()
{
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] SAVE TRIGGERED!" << std::endl;
    (*PackageEditor::MANIFESTJSON) = nlohmann::ordered_json::parse(PackageEditor::JSONTextEdit->toPlainText().toUtf8());
    PackageEditor::SaveManifestJSON();
    BuildUI();
}

void PackageEditor::RemoveSubgame()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * SubgameTabWidget = Button->parentWidget();
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] REMOVE SUBGAME" << SubgameTabWidget->property("JSONPath").toString().toStdString() << std::endl;
}

void PackageEditor::RemoveComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * ComponentTabWidget = Button->parentWidget();
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] REMOVE COMPONENT" << ComponentTabWidget->property("JSONPath").toString().toStdString() << std::endl;

    nlohmann::ordered_json::json_pointer JSONPointer(ComponentTabWidget->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON).at(JSONPointer.parent_pointer()).erase(ComponentTabWidget->property("Index").toInt());
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[OUT] Deleted component:" << ComponentTabWidget->property("Index").toString().toStdString() << std::endl;
    delete ComponentTabWidget;
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

void PackageEditor::RunExeInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] PackageEditor:" << "Running EXE in component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
    Runner * ExeRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ExeRunner->Cleanup();
    ExeRunner->BuildRuntime();
    ExeRunner->Run(QFileDialog::getOpenFileName(this, "Select executable"), QStringList());
    ExeRunner->Cleanup();
}

void PackageEditor::BrowseInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] PackageEditor:" << "Browsing in component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
    Runner * ExplorerRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ExplorerRunner->Cleanup();
    ExplorerRunner->BuildRuntime();
    ExplorerRunner->Run("explorer.exe");
    ExplorerRunner->Cleanup();
}

void PackageEditor::RegeditInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Editing registry in component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
    Runner * RegeditRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    RegeditRunner->Cleanup();
    RegeditRunner->BuildRuntime();
    RegeditRunner->Run("regedit.exe");
    RegeditRunner->Cleanup();
}

void PackageEditor::ExecuteComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Executing component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
    Runner * ComponentRunner = new Runner(this->PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ComponentRunner->Cleanup();
    ComponentRunner->BuildRuntime();
    ComponentRunner->Run();
    ComponentRunner->Cleanup();
}

void PackageEditor::AnalyzeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] PackageEditor:" << "Executing component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
    Runner * ComponentRunner = new Runner(this->PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    ComponentRunner->Cleanup();
    ComponentRunner->BuildRuntime();
    ComponentRunner->Run();
    ComponentRunner->Cleanup();


}

void PackageEditor::CompareComponentsRegistry(const int oldcomponent, const int newcomponent)
{
    //The component numbers to be compared are passed as friendly number, not array index!
    //If oldcomponent is 0, the newcomponent will be comapred against DEFPREFIX,
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Comparing component" << newcomponent << "against" << oldcomponent << std::endl;

    //A runner object is created for each of the two states.
    QString ComparatorPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "COMPARATOR");
    Runner * ComparatorRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, oldcomponent);
    ComparatorRunner->Cleanup(ComparatorPath);
    ComparatorRunner->BuildRuntime(ComparatorPath, "READONLY");
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Comparator initialzed!" << std::endl;

    QString UserDataPath = QDir::cleanPath(PackageDir->path() + QDir::separator() + "USERDATA");
    //Runner * NewComponentRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, newcomponent);
    //NewComponentRunner->Cleanup();
    //NewComponentRunner->BuildRuntime();
    //std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Runtime initialized!";

    //The registry files are read into memory and converted to JSON for easy processing.
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Starting REG to JSON conversion" << std::endl;
    nlohmann::ordered_json OldSysRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((ComparatorPath + QDir::separator() + "system.reg"))));
    nlohmann::ordered_json NewSysRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((UserDataPath + QDir::separator() + "system.reg"))));
    //std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "OldSysRegJSON:" << OldSysRegJSON.dump();
    //std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "NewSysRegJSON:" << NewSysRegJSON.dump();

    nlohmann::ordered_json OldUserRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((ComparatorPath + QDir::separator() + "user.reg"))));
    nlohmann::ordered_json NewUserRegJSON = PackageEditor::RegFileToJSON(QFile(QDir::cleanPath((UserDataPath + QDir::separator() + "user.reg"))));

    //A delta JSON is generated for each pair using the SubtractJSON function.
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Starting JSON Diff subtraction." << std::endl;
    nlohmann::ordered_json SysDeltaJSON = SubtractJSON(OldSysRegJSON, NewSysRegJSON);
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "SYSDELTAJSON:" << SysDeltaJSON.dump() << std::endl;
    nlohmann::ordered_json UserDeltaJSON = SubtractJSON(OldUserRegJSON, NewUserRegJSON);
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "USERDELTAJSON:" << UserDeltaJSON.dump() << std::endl;

    //The DeltaJSON is then converted into RegEdit subcomponents.
    //The Registry Delta objects are merged into MANIFESTJSON.
    std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor: [OUT]" << "Converting deltas to SubComponentArray and merging deltas to MANIFESTJSON." << std::endl;
    if (!SysDeltaJSON.is_null()){
        nlohmann::ordered_json SysDeltaSubComponentArray = RegDeltaToSubComponentArray(SysDeltaJSON, "HKLM");
        MergeRegistryDeltaInComponent(&SysDeltaSubComponentArray, newcomponent);
    }
    if (!UserDeltaJSON.is_null()){
        nlohmann::ordered_json UserDeltaSubComponentArray = RegDeltaToSubComponentArray(UserDeltaJSON, "HKCU");
        MergeRegistryDeltaInComponent(&UserDeltaSubComponentArray, newcomponent);
    }

    //The runners perform cleanup and are then deleted.
    //NewComponentRunner->Cleanup();
    ComparatorRunner->Cleanup(ComparatorPath);

    //delete NewComponentRunner;
    delete ComparatorRunner;

    //The changes are saved to ManifestJSON and the UI is refreshed.
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

//CAUTION! VIBE RE-CODED TO MAKE COMPATIBLE WITH GCC 12!! MUST TEST!!
void PackageEditor::MergeRegistryDeltaInComponent(nlohmann::ordered_json *DeltaSubComponentArray, const int targetcomponent)
{
    // Iterate through all new subcomponents
    for (int i = 0; i < DeltaSubComponentArray->size(); i++)
    {
        bool merged = false; // flag to track if the subcomponent was merged

        auto &deltaSub = (*DeltaSubComponentArray)[i];
        auto &targetSubComponents = (*MANIFESTJSON)["COMPONENTS"][targetcomponent - 1]["SUBCOMPONENTS"];

        for (int j = 0; j < targetSubComponents.size(); j++)
        {
            auto &existingSub = targetSubComponents[j];

            // Skip if not a RegEdit type
            if (!(QString::fromStdString(existingSub["TYPE"]) == "RegEdit"))
                continue;

            // Merge if REGPATH matches
            if (deltaSub["REGPATH"] == existingSub["REGPATH"])
            {
                for (auto KeyValueObject : deltaSub["KEYVALUES"].items())
                {
                    existingSub["KEYVALUES"][KeyValueObject.key()] = KeyValueObject.value();
                }
                merged = true;
                break; // stop searching; already merged
            }
        }

        // If not merged, append new subcomponent
        if (!merged)
        {
            targetSubComponents.push_back(deltaSub);
        }
    }
}

//CAUTION! VIBE RE-CODED TO MAKE COMPATIBLE WITH GCC 12!! MUST TEST!!
nlohmann::ordered_json PackageEditor::RegDeltaToSubComponentArray(nlohmann::ordered_json RegDeltaJSON, QString Hive)
{
    nlohmann::ordered_json SubComponentArray = nlohmann::ordered_json::array();
    RegDeltaJSON = RegDeltaJSON.flatten();

    for (auto Item : RegDeltaJSON.items())
    {
        QString KeyPath = QString::fromStdString(Item.key());
        if (KeyPath.contains("Software/Microsoft/Windows") ||
            KeyPath.contains("Software/Microsoft/Cryptography") ||
            KeyPath.contains("Software/Wow6432Node/Microsoft/Windows") ||
            KeyPath.contains("Software/Classes") ||
            KeyPath.contains("Software/Microsoft/ActiveMovie") ||
            KeyPath.contains("Software/Wine"))
        {
            continue;
        }

        QString REGPATH = Hive + KeyPath;
        QString ARCHITECTURE = REGPATH.contains("Wow6432Node") ? "32" : "64";

        QStringList REGPATHTokens = REGPATH.split("/");
        QString KEY = REGPATHTokens.takeLast(); // removes last element
        REGPATH.clear();
        for (const QString &Token : REGPATHTokens)
        {
            if (Token != "Wow6432Node")
            {
                REGPATH.append(Token + "\\");
            }
        }
        if (!REGPATH.isEmpty()) REGPATH.chop(1); // remove trailing backslash

        nlohmann::ordered_json KeyValueObject;
        QString VALUE = QString::fromStdString(Item.value());

        if (VALUE.startsWith("dword:"))
        {
            KeyValueObject["TYPE"] = "REG_DWORD";
            VALUE.remove(0, 6);
            VALUE = QString::number(VALUE.toUInt(nullptr, 16));
        }
        else if (VALUE.startsWith("hex:"))
        {
            KeyValueObject["TYPE"] = "REG_BINARY";
            VALUE.remove(0, 4);
        }
        else if (VALUE.startsWith("hex(b):") || VALUE.startsWith("str(7):") || VALUE.startsWith("str(2):"))
        {
            continue; // skip unsupported types
        }
        else
        {
            KeyValueObject["TYPE"] = "REG_SZ";
            VALUE.replace("\\\\", "\\");
        }

        KeyValueObject["VALUE"] = VALUE.toStdString();

        // Check if subcomponent with same REGPATH exists
        bool merged = false;
        for (auto &SubComponent : SubComponentArray)
        {
            if (SubComponent["REGPATH"] == REGPATH.toStdString())
            {
                SubComponent["KEYVALUES"][KEY.toStdString()] = KeyValueObject;
                merged = true;
                break;
            }
        }

        if (!merged)
        {
            nlohmann::ordered_json NewSubComponentObject;
            NewSubComponentObject["TYPE"] = "RegEdit";
            NewSubComponentObject["REGPATH"] = REGPATH.toStdString();
            NewSubComponentObject["ARCHITECTURE"] = ARCHITECTURE.toStdString();
            NewSubComponentObject["KEYVALUES"][KEY.toStdString()] = KeyValueObject;
            SubComponentArray.push_back(NewSubComponentObject);
        }
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
        //qDebug() << "beforeif" << "i" << i;
        //qDebug() << DiffJSON[i].dump();
        if((QString::fromStdString(DiffJSON[i]["op"]) == "add") || (QString::fromStdString(DiffJSON[i]["op"]) == "replace"))
        {
            QString KeyPath = QString::fromStdString(DiffJSON[i]["path"]);
            //qDebug().noquote() << "KeyPath:" << KeyPath;

            //Ensure the DeltaJSON parent object is an object so that it doesn't get turned into an array by paths that end in numbers (HKLM/SOFTWARE/FOO/BAR/1234).
            if (!DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString()).parent_pointer()].is_object())
            {
                DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString()).parent_pointer()] = nlohmann::ordered_json::object();
            }

            //Make this a switch!
            if (DiffJSON[i]["value"].is_object())
            {
                for (auto ValueItem : DiffJSON[i]["value"].items())
                {
                    DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString())][ValueItem.key()] = ValueItem.value();
                }
            }
            else if (DiffJSON[i]["value"].is_string())
            {
                DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString())] = DiffJSON[i]["value"];
            }
            else
            {
                std::cout << QTime::currentTime().toString().toStdString() << "PackageEditor:" << "[ERR] DiffJSON[i][\"value\"] is unknown type! Dump:" << DiffJSON[i]["value"].dump() << std::endl;
            }
        }
    }
    //qDebug() << "AFTERFOR";
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
            ////qDebug().noquote() << "KEY:" << KeyPathString;
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
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] PackageEditor:" << "Adding DirLayer in component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;

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
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] PackageEditor:" << "Finalizing component" << Button->parentWidget()->property("Index").toInt() + 1 << std::endl;
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
