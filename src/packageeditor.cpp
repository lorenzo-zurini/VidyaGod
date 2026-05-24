#include "packageeditor.h"
#include "ui_packageeditor.h"
#include "commonutils.h"
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
    NewSubGameObject["SUBGAMEID"] = nullptr;
    NewSubGameObject["PLATFORM"]  = nullptr;
    (*MANIFESTJSON)[json::json_pointer("/SUBGAMES")].push_back(NewSubGameObject);
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    (*MANIFESTJSON)[json::json_pointer("/COMPONENTS")].push_back(json::object({{"COMPONENTID", nullptr}, {"NAME", nullptr}, {"SUBCOMPONENTS", json::array()}}));
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
        LogSucc("PackageEditor", "Save successful.");
    }
    else
    {
        LogErr("PackageEditor", "Save failed.");
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

    PackageEditor::MANIFESTJSON = new nlohmann::ordered_json;
    if(!JSONOps::LoadJSON(new QFile(MetadataDir->filePath("MANIFEST.json")), PackageEditor::MANIFESTJSON))
    {
        //ADD ERROR HANDLING HERE
        LogSucc("packageeditor.cpp", "ManifestJSON successfully parsed.");
        return;
    }
    else
    {
        LogErr("packageeditor.cpp", "Parser returned nullptr, leaving MANIFESTJSON empty.");
    }
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

            LogOut("PackageEditor", "Adding parameter editor: " + Item.key());
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
        LogOut("PackageEditor", "Manifest tab done!");

        //SUBGAMES TABS WIDGET
        QTabWidget * SubGamesTabWidget = new QTabWidget(ManifestTabWidget);
        ManifestTabWidgetLayout->addWidget(SubGamesTabWidget);

        static const std::vector<std::string> IdentityFields = {
            "SUBGAMEID", "PLATFORM", "GAMEUID"
        };
        static const std::vector<std::string> MetadataFields = {
            "TITLE", "TGDBID", "COVER",
            "RELEASEDATE", "EDITION", "EDITIONDATE",
            "DEVELOPER", "PUBLISHER",
            "SERIES", "SERIESSORTNUMBER", "SUBSERIES", "SUBSERIESSORTNUMBER",
            "NETWORKMULTIPLAYER", "DIRECTCONNECT", "LANMULTIPLAYER", "ONLINEMULTIPLAYER",
            "NETWORKCOOP", "LOCALMULTIPLAYER", "LOCALCOOP", "OTHERONLINEFEATURES"
        };
        static const std::vector<std::string> WineExecFields = {
            "COMPONENT", "EXEPATH", "EXEARGS", "WORKDIR",
            "STEAMAPPID", "UMUID", "GOGPRODUCTID",
            "EDITOR", "ONLINEDRM",
            "VARIANTS", "RECOMMENDED_RUNNER"
        };
        static const std::vector<std::string> EmulatorExecFields = { "COMPONENT", "ROM" };
        static const std::vector<std::string> NativeExecFields = {
            "COMPONENT", "EXEPATH", "EXEARGS", "WORKDIR"
        };
        static const std::vector<std::string> CustomExecFields = {
            "COMPONENT", "DATAPATH", "EXEARGS", "WORKDIR"
        };

        auto BuildSubgameFields = [&](QFormLayout *Layout, const std::vector<std::string> &Fields, int i, auto &SubgameRef, const std::string &CurrentPlatform)
        {
            for (const std::string &FieldKey : Fields)
            {
                QString JSONPath = QString("/SUBGAMES/%1/%2").arg(i).arg(QString::fromStdString(FieldKey));

                if (FieldKey == "PLATFORM")
                {
                    QComboBox * PlatformPicker = new QComboBox();
                    PlatformPicker->addItem("(none)");
                    if ((*GlobalConfigJSON).contains("RUNNERS"))
                        for (auto &[PlatformKey, _] : (*GlobalConfigJSON)["RUNNERS"].items())
                            PlatformPicker->addItem(QString::fromStdString(PlatformKey));
                    PlatformPicker->setProperty("JSONPath", JSONPath);
                    if (!CurrentPlatform.empty())
                    {
                        int idx = PlatformPicker->findText(QString::fromStdString(CurrentPlatform));
                        if (idx >= 0) PlatformPicker->setCurrentIndex(idx);
                    }
                    QObject::connect(PlatformPicker, &QComboBox::currentIndexChanged, this, &PackageEditor::PlatformChanged);
                    Layout->addRow("PLATFORM", PlatformPicker);
                    continue;
                }

                if (FieldKey == "COMPONENT")
                {
                    QComboBox * ComponentPicker = new QComboBox();
                    ComponentPicker->addItem("None");
                    for (int j = 0; j < (int)(*PackageEditor::MANIFESTJSON)["COMPONENTS"].size(); j++)
                    {
                        auto &CIDField = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][j]["COMPONENTID"];
                        std::string CIDStr = (!CIDField.is_null() && CIDField.is_string()) ? std::string(CIDField) : ("Component " + std::to_string(j + 1));
                        ComponentPicker->addItem(QString::fromStdString(CIDStr));
                    }
                    ComponentPicker->setProperty("JSONPath", JSONPath);
                    if (SubgameRef.contains("COMPONENT") && !SubgameRef["COMPONENT"].is_null() && SubgameRef["COMPONENT"].is_string())
                    {
                        std::string CurrentComponent = std::string(SubgameRef["COMPONENT"]);
                        for (int k = 1; k < ComponentPicker->count(); k++)
                        {
                            if (ComponentPicker->itemText(k).toStdString() == CurrentComponent)
                            {
                                ComponentPicker->setCurrentIndex(k);
                                break;
                            }
                        }
                    }
                    QObject::connect(ComponentPicker, &QComboBox::currentIndexChanged, this, &PackageEditor::ParentComponentChanged);
                    Layout->addRow("COMPONENT", ComponentPicker);
                    continue;
                }

                QLineEdit * NewParamField = new QLineEdit();
                NewParamField->setProperty("JSONPath", JSONPath);
                if (SubgameRef.contains(FieldKey) && !SubgameRef[FieldKey].is_null())
                {
                    auto &Val = SubgameRef[FieldKey];
                    NewParamField->setText(Val.is_string() ? QString::fromStdString(std::string(Val)) : QString::fromStdString(Val.dump()));
                }
                QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                Layout->addRow(QString::fromStdString(FieldKey), NewParamField);
            }
        };

        for (int i = 0; i < (int)(*PackageEditor::MANIFESTJSON)["SUBGAMES"].size(); i++)
        {
            auto &SubgameRef = (*PackageEditor::MANIFESTJSON)["SUBGAMES"][i];

            std::string SubgameIDStr;
            if (SubgameRef.contains("SUBGAMEID") && !SubgameRef["SUBGAMEID"].is_null() && SubgameRef["SUBGAMEID"].is_string())
                SubgameIDStr = std::string(SubgameRef["SUBGAMEID"]);

            std::string CurrentPlatform;
            if (SubgameRef.contains("PLATFORM") && !SubgameRef["PLATFORM"].is_null() && SubgameRef["PLATFORM"].is_string())
                CurrentPlatform = std::string(SubgameRef["PLATFORM"]);
            std::string RunnerType = GetRunnerType(CurrentPlatform);

            const std::vector<std::string> *ExecFields = &WineExecFields;
            if      (RunnerType == "emulator") ExecFields = &EmulatorExecFields;
            else if (RunnerType == "native")   ExecFields = &NativeExecFields;
            else if (RunnerType == "custom")   ExecFields = &CustomExecFields;

            QWidget * SubGameTabWidget = new QWidget(SubGamesTabWidget);
            SubGameTabWidget->setProperty("JSONPath", QString("/SUBGAMES/%1").arg(i));
            SubGameTabWidget->setProperty("Index", i);
            QVBoxLayout * SubGameTabLayout = new QVBoxLayout(SubGameTabWidget);
            SubGameTabWidget->setLayout(SubGameTabLayout);

            QHBoxLayout * SubGameToolbarLayout = new QHBoxLayout();
            SubGameTabLayout->addLayout(SubGameToolbarLayout);
            SubGameToolbarLayout->addStretch();
            QPushButton * RemoveSubGameButton = new QPushButton("Remove Subgame", SubGameTabWidget);
            SubGameToolbarLayout->addWidget(RemoveSubGameButton);
            QObject::connect(RemoveSubGameButton, &QPushButton::clicked, this, &PackageEditor::RemoveSubgame);

            QScrollArea * SubGameScrollArea = new QScrollArea(SubGameTabWidget);
            SubGameScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            SubGameScrollArea->setWidgetResizable(true);
            SubGameTabLayout->addWidget(SubGameScrollArea);

            QWidget * SubGameScrollContents = new QWidget();
            QVBoxLayout * SubGameScrollLayout = new QVBoxLayout(SubGameScrollContents);
            SubGameScrollContents->setLayout(SubGameScrollLayout);
            SubGameScrollArea->setWidget(SubGameScrollContents);

            auto MakeSection = [&](const QString &Title, const std::vector<std::string> &Fields)
            {
                QGroupBox * Box = new QGroupBox(Title, SubGameScrollContents);
                QFormLayout * Form = new QFormLayout(Box);
                Box->setLayout(Form);
                BuildSubgameFields(Form, Fields, i, SubgameRef, CurrentPlatform);
                SubGameScrollLayout->addWidget(Box);
            };

            MakeSection("Identity",  IdentityFields);
            if (!CurrentPlatform.empty())
                MakeSection("Execution", *ExecFields);
            MakeSection("Metadata",  MetadataFields);
            SubGameScrollLayout->addStretch();

            QString TabLabel = SubgameIDStr.empty() ? QString("Subgame %1").arg(i + 1) : QString::fromStdString(SubgameIDStr);
            SubGamesTabWidget->addTab(SubGameTabWidget, TabLabel);
        }
        ui->PackageEditorTabWidget->addTab(ManifestTabWidget, "MANIFEST");

    //INDIVIDUAL COMPONENTS TABS
    for (int i = 0; i < (int)(*PackageEditor::MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        // Resolve COMPONENTID string for this component
        std::string ComponentIDStr;
        auto &CIDField = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["COMPONENTID"];
        if (!CIDField.is_null() && CIDField.is_string())
            ComponentIDStr = std::string(CIDField);

        QWidget * ComponentTabWidget = new QWidget(ui->PackageEditorTabWidget);
        ComponentTabWidget->setProperty("JSONPath", QString("/COMPONENTS/%1").arg(QString::number(i)));
        ComponentTabWidget->setProperty("Index", i);
        ComponentTabWidget->setProperty("ComponentID", QString::fromStdString(ComponentIDStr));
        QVBoxLayout * ComponentTabWidgetLayout = new QVBoxLayout(ComponentTabWidget);
        ComponentTabWidget->setLayout(ComponentTabWidgetLayout);

            QGroupBox * ComponentNameGroupBox = new QGroupBox(ComponentTabWidget);
            ComponentTabWidgetLayout->addWidget(ComponentNameGroupBox);
            QFormLayout * ComponentNameGroupBoxLayout = new QFormLayout(ComponentNameGroupBox);
            ComponentNameGroupBox->setLayout(ComponentNameGroupBoxLayout);

                QLineEdit * ComponentIDField = new QLineEdit(ComponentNameGroupBox);
                QString ComponentIDFieldJSONPath = QString("/COMPONENTS/%1/COMPONENTID").arg(QString::number(i));
                ComponentIDField->setProperty("JSONPath", ComponentIDFieldJSONPath);
                if (!CIDField.is_null())
                    ComponentIDField->setText(QString::fromStdString(ComponentIDStr));
                QObject::connect(ComponentIDField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                ComponentNameGroupBoxLayout->addRow("COMPONENTID", ComponentIDField);

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

                QString ParentComponentJSONPath = QString("/COMPONENTS/%1/PARENTCOMPONENT").arg(QString::number(i));
                nlohmann::ordered_json::json_pointer ParentComponentJSONPointer(ParentComponentJSONPath.toStdString());

                QComboBox * ParentComponentPicker = new QComboBox(ComponentTabWidget);
                ParentComponentPicker->addItem("None");
                for (int j = 0; j < i; j++)
                {
                    std::string ParentCIDStr;
                    auto &ParentCIDField = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][j]["COMPONENTID"];
                    if (!ParentCIDField.is_null() && ParentCIDField.is_string())
                        ParentCIDStr = std::string(ParentCIDField);
                    ParentComponentPicker->addItem(QString::fromStdString(ParentCIDStr.empty() ? ("Component " + std::to_string(j + 1)) : ParentCIDStr));
                }
                ParentComponentPicker->setProperty("JSONPath", ParentComponentJSONPath);
                if(!(*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer].is_null())
                {
                    std::string CurrentParentID = std::string((*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer]);
                    for (int k = 1; k < ParentComponentPicker->count(); k++)
                    {
                        if (ParentComponentPicker->itemText(k).toStdString() == CurrentParentID)
                        {
                            ParentComponentPicker->setCurrentIndex(k);
                            break;
                        }
                    }
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
                [this, ParentComponentJSONPointer, ComponentIDStr]
                {
                    std::string oldcomponent_id;
                    auto &ParentVal = (*PackageEditor::MANIFESTJSON)[ParentComponentJSONPointer];
                    if (!ParentVal.is_null() && ParentVal.is_string())
                        oldcomponent_id = std::string(ParentVal);
                    this->CompareComponentsRegistry(oldcomponent_id, ComponentIDStr);
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

                if (SubComponentType == "VFSZipLayer" || SubComponentType == "VFSDirLayer" || SubComponentType == "RawFile")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("PATH:"), 1, 0);
                    QLineEdit * PathField = new QLineEdit(IndividualSubComponentGroupBox);
                    QString PathJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/PATH").arg(i).arg(j);
                    PathField->setProperty("JSONPath", PathJSONPath);
                    if (!(*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"].is_null())
                        PathField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"]));
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(PathField, 1, 1);
                }
                else if (SubComponentType == "RegEdit")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("REGPATH:"), 1, 0);
                    QLineEdit * RegPathField = new QLineEdit(IndividualSubComponentGroupBox);
                    QString RegPathJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/REGPATH").arg(i).arg(j);
                    RegPathField->setProperty("JSONPath", RegPathJSONPath);
                    if (!(*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["REGPATH"].is_null())
                    {
                        RegPathField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["REGPATH"]));
                    }
                    QObject::connect(RegPathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(RegPathField, 1, 1);

                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("ARCHITECTURE:"), 2, 0);
                    QLineEdit * ArchField = new QLineEdit(IndividualSubComponentGroupBox);
                    QString ArchJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/ARCHITECTURE").arg(i).arg(j);
                    ArchField->setProperty("JSONPath", ArchJSONPath);
                    if (!(*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["ARCHITECTURE"].is_null())
                    {
                        ArchField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["ARCHITECTURE"]));
                    }
                    QObject::connect(ArchField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(ArchField, 2, 1);

                        QGroupBox * RegKeysGroupBox = new QGroupBox(IndividualSubComponentGroupBox);
                        QGridLayout * RegKeysGroupBoxLayout = new QGridLayout(RegKeysGroupBox);
                        RegKeysGroupBox->setLayout(RegKeysGroupBoxLayout);
                        RegKeysGroupBox->setTitle("KEYS");

                        RegKeysGroupBoxLayout->addWidget(new QLabel("Key"), 0, 0);
                        RegKeysGroupBoxLayout->addWidget(new QLabel("Value"), 0, 1);

                        int k = 1;
                        for (auto Object : (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"].items())
                        {
                            RegKeysGroupBoxLayout->addWidget(new QLabel(QString::fromStdString(Object.key())), k, 0);
                            QLineEdit * KeyValueField = new QLineEdit(RegKeysGroupBox);
                            QString KeyValueJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/KEYVALUES/%3").arg(i).arg(j).arg(QString::fromStdString(Object.key()));
                            KeyValueField->setProperty("JSONPath", KeyValueJSONPath);
                            KeyValueField->setText(QString::fromStdString(Object.value()));
                            QObject::connect(KeyValueField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            RegKeysGroupBoxLayout->addWidget(KeyValueField, k, 1);
                            k++;
                        }

                    IndividualSubComponentGroupBoxLayout->addWidget(RegKeysGroupBox, 3, 0, 1, -1);
                }
                else
                {
                    LogErr("PackageEditor", QString("Component %1, subcomponent %2 has invalid type (%3).").arg(i).arg(j).arg(SubComponentType).toStdString());
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
    LogOut("PackageEditor", "JSON value: " + Editor->text().toStdString() + " Submitted to: " + Editor->property("JSONPath").toString().toStdString());
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}

void PackageEditor::JSONQTextEditChanged()
{
    LogOut("PackageEditor", "JSON TEXT EDITOR CHANGED");
    if (nlohmann::ordered_json::accept(PackageEditor::JSONTextEdit->toPlainText().toUtf8()))
    {
        LogOut("PackageEditor", "Valid JSON!");
        PackageEditor::JSONTextEdit->setStyleSheet("");
        PackageEditor::SaveJSONButton->setDisabled(false);
    }
    else
    {
        LogErr("PackageEditor", "Invalid JSON!");
        PackageEditor::JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
        PackageEditor::SaveJSONButton->setDisabled(true);
    }
}

std::string PackageEditor::GetRunnerType(const std::string &platform)
{
    if (platform.empty()) return "wine";
    if (!(*GlobalConfigJSON).contains("RUNNERS")) return "wine";
    auto &runners = (*GlobalConfigJSON)["RUNNERS"];
    if (runners.contains(platform) && !runners[platform].empty())
        return runners[platform][0].value("TYPE", std::string("wine"));
    return "wine";
}

void PackageEditor::PlatformChanged()
{
    QComboBox * PlatformPicker = qobject_cast<QComboBox *>(QObject::sender());
    std::string SelectedPlatform = PlatformPicker->currentIndex() == 0 ? "" : PlatformPicker->currentText().toStdString();
    nlohmann::ordered_json::json_pointer JSONPointer(PlatformPicker->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = SelectedPlatform.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(SelectedPlatform);
    PackageEditor::SaveManifestJSON();
    PackageEditor::BuildUI();
    PackageEditor::RefreshJSONView();
}

void PackageEditor::ParentComponentChanged()
{
    QComboBox * ParentComponentPicker = qobject_cast<QComboBox *>(QObject::sender());
    std::string SelectedID = ParentComponentPicker->currentIndex() == 0 ? "" : ParentComponentPicker->currentText().toStdString();
    LogOut("PackageEditor", "PARENT COMPONENT CHANGED! SelectedID: " + SelectedID);

    nlohmann::ordered_json::json_pointer JSONPointer(ParentComponentPicker->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = SelectedID;
    LogOut("PackageEditor", "Submitted PARENTCOMPONENT: " + SelectedID + " to: " + ParentComponentPicker->property("JSONPath").toString().toStdString());
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}

void PackageEditor::SaveJSONButtonPressed()
{
    LogOut("PackageEditor", "SAVE TRIGGERED!");
    (*PackageEditor::MANIFESTJSON) = nlohmann::ordered_json::parse(PackageEditor::JSONTextEdit->toPlainText().toUtf8());
    PackageEditor::SaveManifestJSON();
    BuildUI();
}

void PackageEditor::RemoveSubgame()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * SubgameTabWidget = Button->parentWidget();
    LogOut("PackageEditor", "REMOVE SUBGAME " + SubgameTabWidget->property("JSONPath").toString().toStdString());
}

void PackageEditor::RemoveComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QWidget * ComponentTabWidget = Button->parentWidget();
    LogOut("PackageEditor", "REMOVE COMPONENT " + ComponentTabWidget->property("JSONPath").toString().toStdString());

    nlohmann::ordered_json::json_pointer JSONPointer(ComponentTabWidget->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON).at(JSONPointer.parent_pointer()).erase(ComponentTabWidget->property("Index").toInt());
    LogOut("PackageEditor", "Deleted component: " + ComponentTabWidget->property("Index").toString().toStdString());
    delete ComponentTabWidget;
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

void PackageEditor::RunExeInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::string ComponentID = Button->parentWidget()->property("ComponentID").toString().toStdString();
    LogOut("PackageEditor::RunExeInComponent", "Running EXE in component " + ComponentID);

    QString SelectedExe = QFileDialog::getOpenFileName(this, "Select executable");
    if (SelectedExe.isEmpty()) return;

    ContainerParams Params(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);
    Container.Cleanup();
    Container.BuildContainerRuntime();
    Container.Execute(SelectedExe.toStdString());
    Container.Cleanup();
}

void PackageEditor::BrowseInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::string ComponentID = Button->parentWidget()->property("ComponentID").toString().toStdString();
    LogOut("PackageEditor::BrowseInComponent", "Browsing in component " + ComponentID);

    ContainerParams Params(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);
    Container.Cleanup();
    Container.BuildContainerRuntime();
    Container.Execute("explorer.exe");
    Container.Cleanup();
}

void PackageEditor::RegeditInComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::string ComponentID = Button->parentWidget()->property("ComponentID").toString().toStdString();
    LogOut("PackageEditor::RegeditInComponent", "Editing registry in component " + ComponentID);

    ContainerParams Params(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);
    Container.Cleanup();
    Container.BuildContainerRuntime();
    Container.Execute("regedit.exe");
    Container.Cleanup();
}

void PackageEditor::ExecuteComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    std::string ComponentID = Button->parentWidget()->property("ComponentID").toString().toStdString();
    LogOut("PackageEditor::ExecuteComponent", "Executing component " + ComponentID);

    ContainerParams Params(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);
    Container.Cleanup();
    Container.BuildContainerRuntime();
    Container.Execute("cmd.exe");
    Container.Cleanup();
}

void PackageEditor::AnalyzeComponent()
{
    // Not yet implemented
}

void PackageEditor::CompareComponentsRegistry(const std::string &oldcomponent_id, const std::string &newcomponent_id)
{
    LogOut("PackageEditor::CompareComponentsRegistry", "Comparing component " + newcomponent_id + " against " + oldcomponent_id);

    std::filesystem::path PackagePath = PackageDir->path().toStdString();

    // Build the comparator — the baseline state of oldcomponent (or bare defprefix if 0) in readonly mode.
    // Custom TempPath/RuntimePath so it doesn't collide with the normal RUNTIME/TEMP.
    ContainerParams ComparatorParams(PackagePath, "", oldcomponent_id);
    ContainerWrapper ComparatorContainer(*GlobalConfigJSON, *MANIFESTJSON, ComparatorParams);
    ComparatorContainer.ContainerParams.TempPath       = PackagePath / "COMPARATOR_TEMP";
    ComparatorContainer.ContainerParams.DefPrefixPath  = PackagePath / "COMPARATOR_TEMP" / "DEFPREFIX";
    ComparatorContainer.ContainerParams.RuntimePath    = PackagePath / "COMPARATOR";
    ComparatorContainer.ContainerParams.ReadOnlyVFS    = true;
    ComparatorContainer.Cleanup();
    ComparatorContainer.BuildContainerRuntime();

    LogOut("PackageEditor::CompareComponentsRegistry", "Comparator initialized.");

    // Read baseline registry from the mounted comparator VFS.
    std::filesystem::path ComparatorRuntime = ComparatorContainer.ContainerParams.RuntimePath;
    nlohmann::ordered_json OldSysRegJSON  = PackageEditor::RegFileToJSON(QFile(QString::fromStdString((ComparatorRuntime / "system.reg").string())));
    nlohmann::ordered_json OldUserRegJSON = PackageEditor::RegFileToJSON(QFile(QString::fromStdString((ComparatorRuntime / "user.reg").string())));

    ComparatorContainer.Cleanup();

    // Read the "after" registry from UserDataPath — changes COW'd there by a previous Execute().
    std::filesystem::path UserDataPath = PackagePath / "USERDATA";
    nlohmann::ordered_json NewSysRegJSON  = PackageEditor::RegFileToJSON(QFile(QString::fromStdString((UserDataPath / "system.reg").string())));
    nlohmann::ordered_json NewUserRegJSON = PackageEditor::RegFileToJSON(QFile(QString::fromStdString((UserDataPath / "user.reg").string())));

    LogOut("PackageEditor::CompareComponentsRegistry", "Starting JSON diff.");
    nlohmann::ordered_json SysDeltaJSON  = SubtractJSON(OldSysRegJSON,  NewSysRegJSON);
    nlohmann::ordered_json UserDeltaJSON = SubtractJSON(OldUserRegJSON, NewUserRegJSON);
    LogOut("PackageEditor::CompareComponentsRegistry", "SYSDELTA: " + SysDeltaJSON.dump());
    LogOut("PackageEditor::CompareComponentsRegistry", "USERDELTA: " + UserDeltaJSON.dump());

    if (!SysDeltaJSON.is_null())
    {
        nlohmann::ordered_json SysDelta = RegDeltaToSubComponentArray(SysDeltaJSON, "HKLM");
        MergeRegistryDeltaInComponent(&SysDelta, newcomponent_id);
    }
    if (!UserDeltaJSON.is_null())
    {
        nlohmann::ordered_json UserDelta = RegDeltaToSubComponentArray(UserDeltaJSON, "HKCU");
        MergeRegistryDeltaInComponent(&UserDelta, newcomponent_id);
    }

    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
    PackageEditor::BuildUI();
}

//CAUTION! VIBE RE-CODED TO MAKE COMPATIBLE WITH GCC 12!! MUST TEST!!
void PackageEditor::MergeRegistryDeltaInComponent(nlohmann::ordered_json *DeltaSubComponentArray, const std::string &targetcomponent_id)
{
    int targetcomponentIdx = ContainerWrapper::FindComponentIndex(*MANIFESTJSON, targetcomponent_id);
    if (targetcomponentIdx == -1) return;

    // Iterate through all new subcomponents
    for (int i = 0; i < (int)DeltaSubComponentArray->size(); i++)
    {
        bool merged = false; // flag to track if the subcomponent was merged

        auto &deltaSub = (*DeltaSubComponentArray)[i];
        auto &targetSubComponents = (*MANIFESTJSON)["COMPONENTS"][targetcomponentIdx]["SUBCOMPONENTS"];

        for (int j = 0; j < (int)targetSubComponents.size(); j++)
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

        // Check if subcomponent with same REGPATH exists
        bool merged = false;
        for (auto &SubComponent : SubComponentArray)
        {
            if (SubComponent["REGPATH"] == REGPATH.toStdString())
            {
                SubComponent["KEYVALUES"][KEY.toStdString()] = Item.value();
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
            NewSubComponentObject["KEYVALUES"][KEY.toStdString()] = Item.value();
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
        if((QString::fromStdString(DiffJSON[i]["op"]) == "add") || (QString::fromStdString(DiffJSON[i]["op"]) == "replace"))
        {
            QString KeyPath = QString::fromStdString(DiffJSON[i]["path"]);

            //Ensure the DeltaJSON parent object is an object so that it doesn't get turned into an array by paths that end in numbers (HKLM/SOFTWARE/FOO/BAR/1234).
            if (!DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString()).parent_pointer()].is_object())
            {
                DeltaJSON[nlohmann::ordered_json::json_pointer(KeyPath.toStdString()).parent_pointer()] = nlohmann::ordered_json::object();
            }

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
                LogErr("PackageEditor", "DiffJSON[i][\"value\"] is unknown type! Dump: " + DiffJSON[i]["value"].dump());
            }
        }
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
            nlohmann::ordered_json::json_pointer JSONPointer(KeyPathString.toStdString());

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
    LogOut("PackageEditor", "Adding DirLayer in component " + Button->parentWidget()->property("ComponentID").toString().toStdString());

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
    LogOut("PackageEditor", "Finalizing component " + std::to_string(Button->parentWidget()->property("Index").toInt() + 1));
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
