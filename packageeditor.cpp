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
                AnalyzeButton->setText("Analyze");
                SubComponentsToolbarLayout->addWidget(AnalyzeButton);
                QObject::connect(AnalyzeButton, &QPushButton::clicked, this, &PackageEditor::AnalyzeComponent);

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

                if(SubComponentType == "ZipFileLayer")
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
    ExeRunner->Cleanup(FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
    ExeRunner->BuildRuntime(FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"), FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "USERDATA"));
    ExeRunner->Run(QFileDialog::getOpenFileName(this, "Select executable"), QStringList(), FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
    ExeRunner->Cleanup(FSOps::SubPath(FSOps::SubPath(ExeRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
}

void PackageEditor::BrowseInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Browsing in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * ExplorerRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);
    ExplorerRunner->Cleanup(FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
    ExplorerRunner->BuildRuntime(FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"), FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "USERDATA"));
    ExplorerRunner->Run("explorer.exe", QStringList(), FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
    ExplorerRunner->Cleanup(FSOps::SubPath(FSOps::SubPath(ExplorerRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME"));
}

void PackageEditor::RegeditInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Editing registry in component" << Button->parentWidget()->property("Index").toInt() + 1;
    Runner * RegeditRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, Button->parentWidget()->property("Index").toInt() + 1);

    QString NewComponentRuntimePath = FSOps::SubPath(FSOps::SubPath(RegeditRunner->PackageDir->path(), "NEWCOMPONENT"), "RUNTIME");
    QString NewComponentUserDataPath = FSOps::SubPath(FSOps::SubPath(RegeditRunner->PackageDir->path(), "NEWCOMPONENT"), "USERDATA");
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

    Runner * ComparatorRunner = new Runner(PackageDir, MANIFESTJSON, GlobalConfigJSON, 0, parentcomponent);
    QString ComparatorPath = FSOps::SubPath(FSOps::SubPath(ComparatorRunner->PackageDir->path(), "NEWCOMPONENT"), "COMPARATOR");
    ComparatorRunner->Cleanup(ComparatorPath);
    ComparatorRunner->BuildRuntime(ComparatorPath, "READONLY");
    QMessageBox::warning(nullptr, "TEST COMPARATOR", "TEST COMPARATOR");
    ComparatorRunner->Cleanup(ComparatorPath);

    //QStringList * FileList = new QStringList;

    //QDirIterator Iterator(RuntimePath, QDirIterator::Subdirectories);
    //while (Iterator.hasNext()) {
    //    QString FilePath = Iterator.next().toLower();

    //}
    //delete FileList;
    //return NoConflict;




}

void PackageEditor::FinalizeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    qDebug().noquote() << QTime::currentTime().toString() << "PackageEditor:" << "[OUT] PackageEditor:" << "Finalizing component" << Button->parentWidget()->property("Index").toInt() + 1;
}
