#include "packageeditor.h"
#include "ui_packageeditor.h"
#include "commonutils.h"
#include <iostream>
#include <QBuffer>
#include <QImage>

using json = nlohmann::ordered_json;

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent, const QString &PreselectedPath)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;

    InitPackage(PreselectedPath);
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
    ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component %1").arg(ui->PackageEditorTabWidget->count() - 1));
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

void PackageEditor::InitPackage(const QString &PreselectedPath)
{
    QString ChosenPath = PreselectedPath.isEmpty()
        ? QFileDialog::getExistingDirectory(this, "Select package directory...")
        : PreselectedPath;
    this->PackageDir = new QDir(ChosenPath);
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
    //Save current tab positions so we can restore them after the UI is rebuilt.
    SavedMainTab    = ui->PackageEditorTabWidget->currentIndex();
    SavedSubgameTab = SubGamesTabWidget ? SubGamesTabWidget->currentIndex() : 0;

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
            if ((Item.key() == "SUBGAMES") || (Item.key() == "COMPONENTS") || (Item.key() == "RUNNERS"))
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
        SubGamesTabWidget = new QTabWidget(ManifestTabWidget); // assign to member for position save/restore
        ManifestTabWidgetLayout->addWidget(SubGamesTabWidget);

        //TITLE and GAMEUID stay flat — they are the subgame's primary identifiers.
        //Everything else lives under a nested METADATA object.
        static const std::vector<std::string> IdentityFields = {
            "SUBGAMEID", "TITLE", "PLATFORM", "GAMEUID"
        };
        static const std::vector<std::string> MetadataFields = {
            "TGDBID", "STEAMAPPID", "GOGPRODUCTID", "UMUID",
            "COVER", "RELEASEDATE", "EDITION", "EDITIONDATE",
            "DEVELOPER", "PUBLISHER",
            "SERIES", "SERIESSORTNUMBER", "SUBSERIES", "SUBSERIESSORTNUMBER",
            "EDITOR", "ONLINEDRM",
            "NETWORKMULTIPLAYER", "DIRECTCONNECT", "LANMULTIPLAYER", "ONLINEMULTIPLAYER",
            "NETWORKCOOP", "LOCALMULTIPLAYER", "LOCALCOOP", "OTHERONLINEFEATURES"
        };
        static const std::vector<std::string> WineExecFields = {
            "EXECUTABLE_ID", "DEFAULT_VARIANT_ID", "RECOMMENDED_RUNNER"
        };
        static const std::vector<std::string> EmulatorExecFields = {
            "EXECUTABLE_ID", "DEFAULT_VARIANT_ID"
        };
        static const std::vector<std::string> NativeExecFields = {
            "EXECUTABLE_ID", "DEFAULT_VARIANT_ID"
        };
        static const std::vector<std::string> CustomExecFields = {
            "EXECUTABLE_ID", "DEFAULT_VARIANT_ID"
        };

        //SubPath: when non-empty, fields are read/written under SUBGAMES[i][SubPath][field].
        //Used for the Metadata section which nests its fields under a METADATA object.
        auto BuildSubgameFields = [&](QFormLayout *Layout, const std::vector<std::string> &Fields, int i, auto &SubgameRef, const std::string &CurrentPlatform, const std::string &SubPath = "")
        {
            for (const std::string &FieldKey : Fields)
            {
                QString JSONPath = SubPath.empty()
                    ? QString("/SUBGAMES/%1/%2").arg(i).arg(QString::fromStdString(FieldKey))
                    : QString("/SUBGAMES/%1/%2/%3").arg(i).arg(QString::fromStdString(SubPath)).arg(QString::fromStdString(FieldKey));

                //Resolve the JSON object to read current values from.
                auto &ValSource = (SubPath.empty() || !SubgameRef.contains(SubPath))
                                  ? SubgameRef
                                  : SubgameRef[SubPath];

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

                QLineEdit * NewParamField = new QLineEdit();
                NewParamField->setProperty("JSONPath", JSONPath);
                if (ValSource.contains(FieldKey) && !ValSource[FieldKey].is_null())
                {
                    auto &Val = ValSource[FieldKey];
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

            //Cover drop area — 2:3 aspect ratio matching SteamGridDB vertical art standard.
            QLabel * CoverDropLabel = new QLabel(SubGameScrollContents);
            CoverDropLabel->setFixedSize(150, 225);
            CoverDropLabel->setAlignment(Qt::AlignCenter);
            CoverDropLabel->setWordWrap(true);
            CoverDropLabel->setStyleSheet(
                "QLabel { background-color: #1e1e1e; border: 2px dashed #555555; "
                "color: #777777; border-radius: 4px; font-size: 11px; }");
            CoverDropLabel->setText("Drop cover art\nhere\n\n2 : 3");
            CoverDropLabel->setAcceptDrops(true);
            CoverDropLabel->setProperty("SubgameIndex", i);

            //Load existing cover if set in metadata.
            {
                std::string CoverFile;
                if (SubgameRef.contains("METADATA") && SubgameRef["METADATA"].is_object()
                    && SubgameRef["METADATA"].contains("COVER") && SubgameRef["METADATA"]["COVER"].is_string())
                    CoverFile = std::string(SubgameRef["METADATA"]["COVER"]);
                else if (SubgameRef.contains("COVER") && SubgameRef["COVER"].is_string())
                    CoverFile = std::string(SubgameRef["COVER"]);
                if (!CoverFile.empty())
                {
                    QPixmap Pix(QDir::cleanPath(MetadataDir->path() + "/" + QString::fromStdString(CoverFile)));
                    if (!Pix.isNull())
                        CoverDropLabel->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
            }
            CoverDropLabel->installEventFilter(this);

            QHBoxLayout * CoverRowLayout = new QHBoxLayout();
            CoverRowLayout->addWidget(CoverDropLabel);
            CoverRowLayout->addStretch();
            SubGameScrollLayout->addLayout(CoverRowLayout);

            auto MakeSection = [&](const QString &Title, const std::vector<std::string> &Fields, const std::string &SubPath = "")
            {
                QGroupBox * Box = new QGroupBox(Title, SubGameScrollContents);
                QFormLayout * Form = new QFormLayout(Box);
                Box->setLayout(Form);
                BuildSubgameFields(Form, Fields, i, SubgameRef, CurrentPlatform, SubPath);
                SubGameScrollLayout->addWidget(Box);
            };

            MakeSection("Identity",  IdentityFields);
            if (!CurrentPlatform.empty())
                MakeSection("Execution", *ExecFields);

            //For custom-platform subgames, show an inline runner definition editor.
            //Reads/writes MANIFEST["RUNNERS"][Platform][0].
            if (RunnerType == "custom" && !CurrentPlatform.empty())
            {
                //Ensure RUNNERS[Platform][0] exists so json_pointer writes land in an array, not an object.
                auto &RunnerArray = (*MANIFESTJSON)["RUNNERS"][CurrentPlatform];
                if (!RunnerArray.is_array() || RunnerArray.empty())
                    RunnerArray = nlohmann::ordered_json::array({nlohmann::ordered_json::object()});

                //Ensure sub-fields exist with correct types.
                auto &Runner0 = (*MANIFESTJSON)["RUNNERS"][CurrentPlatform][0];
                if (!Runner0.contains("ARGS")       || !Runner0["ARGS"].is_array())       Runner0["ARGS"]       = nlohmann::ordered_json::array();
                if (!Runner0.contains("ENV")        || !Runner0["ENV"].is_object())        Runner0["ENV"]        = nlohmann::ordered_json::object();
                if (!Runner0.contains("REMOVE_ENV") || !Runner0["REMOVE_ENV"].is_array()) Runner0["REMOVE_ENV"] = nlohmann::ordered_json::array();

                std::string Platform = CurrentPlatform; //capture by value for lambdas

                QGroupBox * RunnerBox = new QGroupBox("Runner Definition", SubGameScrollContents);
                QVBoxLayout * RunnerBoxLayout = new QVBoxLayout(RunnerBox);
                RunnerBox->setLayout(RunnerBoxLayout);

                //--- NAME and EXECUTABLE ---
                QFormLayout * RunnerTopForm = new QFormLayout();
                RunnerBoxLayout->addLayout(RunnerTopForm);

                auto AddSimpleField = [&](const QString &Label, const QString &Key)
                {
                    QLineEdit * Field = new QLineEdit(RunnerBox);
                    QString Path = QString("/RUNNERS/%1/0/%2").arg(QString::fromStdString(Platform)).arg(Key);
                    Field->setProperty("JSONPath", Path);
                    auto &Val = (*MANIFESTJSON)["RUNNERS"][Platform][0][Key.toStdString()];
                    if (!Val.is_null() && Val.is_string())
                        Field->setText(QString::fromStdString(std::string(Val)));
                    QObject::connect(Field, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    RunnerTopForm->addRow(Label, Field);
                };
                AddSimpleField("NAME",       "NAME");
                AddSimpleField("EXECUTABLE", "EXECUTABLE");

                //--- ARGS (array of strings) ---
                QGroupBox * ArgsBox = new QGroupBox("ARGS", RunnerBox);
                QVBoxLayout * ArgsLayout = new QVBoxLayout(ArgsBox);
                ArgsBox->setLayout(ArgsLayout);
                auto &ArgsArr = (*MANIFESTJSON)["RUNNERS"][Platform][0]["ARGS"];
                for (int k = 0; k < (int)ArgsArr.size(); k++)
                {
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * ArgField = new QLineEdit(ArgsBox);
                    ArgField->setProperty("JSONPath", QString("/RUNNERS/%1/0/ARGS/%2").arg(QString::fromStdString(Platform)).arg(k));
                    if (ArgsArr[k].is_string()) ArgField->setText(QString::fromStdString(std::string(ArgsArr[k])));
                    QObject::connect(ArgField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    QPushButton * DelBtn = new QPushButton("✕", ArgsBox);
                    DelBtn->setFixedWidth(28);
                    QObject::connect(DelBtn, &QPushButton::clicked, this, [this, Platform, k](){
                        (*MANIFESTJSON)["RUNNERS"][Platform][0]["ARGS"].erase(k);
                        SaveManifestJSON(); BuildUI();
                    });
                    Row->addWidget(ArgField); Row->addWidget(DelBtn);
                    ArgsLayout->addLayout(Row);
                }
                QPushButton * AddArgBtn = new QPushButton("+ Add Arg", ArgsBox);
                QObject::connect(AddArgBtn, &QPushButton::clicked, this, [this, Platform](){
                    (*MANIFESTJSON)["RUNNERS"][Platform][0]["ARGS"].push_back("");
                    SaveManifestJSON(); BuildUI();
                });
                ArgsLayout->addWidget(AddArgBtn);
                RunnerBoxLayout->addWidget(ArgsBox);

                //--- ENV (object: key → value) ---
                QGroupBox * EnvBox = new QGroupBox("ENV", RunnerBox);
                QVBoxLayout * EnvLayout = new QVBoxLayout(EnvBox);
                EnvBox->setLayout(EnvLayout);
                auto &EnvObj = (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"];
                for (auto &[EnvKey, EnvVal] : EnvObj.items())
                {
                    std::string KeyCopy = EnvKey;
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * KeyField = new QLineEdit(EnvBox);
                    KeyField->setText(QString::fromStdString(KeyCopy));
                    KeyField->setPlaceholderText("KEY");
                    QLineEdit * ValField = new QLineEdit(EnvBox);
                    ValField->setProperty("JSONPath", QString("/RUNNERS/%1/0/ENV/%2").arg(QString::fromStdString(Platform)).arg(QString::fromStdString(KeyCopy)));
                    if (EnvVal.is_string()) ValField->setText(QString::fromStdString(std::string(EnvVal)));
                    QObject::connect(ValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    //Key rename: erase old key, insert new key with same value.
                    QObject::connect(KeyField, &QLineEdit::editingFinished, this, [this, Platform, KeyCopy, KeyField, ValField](){
                        std::string NewKey = KeyField->text().toStdString();
                        if (NewKey == KeyCopy || NewKey.empty()) return;
                        auto Val = (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"][KeyCopy];
                        (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"].erase(KeyCopy);
                        (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"][NewKey] = Val;
                        ValField->setProperty("JSONPath", QString("/RUNNERS/%1/0/ENV/%2").arg(QString::fromStdString(Platform)).arg(QString::fromStdString(NewKey)));
                        SaveManifestJSON(); BuildUI();
                    });
                    QPushButton * DelBtn = new QPushButton("✕", EnvBox);
                    DelBtn->setFixedWidth(28);
                    QObject::connect(DelBtn, &QPushButton::clicked, this, [this, Platform, KeyCopy](){
                        (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"].erase(KeyCopy);
                        SaveManifestJSON(); BuildUI();
                    });
                    Row->addWidget(KeyField); Row->addWidget(ValField); Row->addWidget(DelBtn);
                    EnvLayout->addLayout(Row);
                }
                QPushButton * AddEnvBtn = new QPushButton("+ Add Env Var", EnvBox);
                QObject::connect(AddEnvBtn, &QPushButton::clicked, this, [this, Platform](){
                    (*MANIFESTJSON)["RUNNERS"][Platform][0]["ENV"]["NEW_KEY"] = "";
                    SaveManifestJSON(); BuildUI();
                });
                EnvLayout->addWidget(AddEnvBtn);
                RunnerBoxLayout->addWidget(EnvBox);

                //--- REMOVE_ENV (array of strings) ---
                QGroupBox * RemEnvBox = new QGroupBox("REMOVE_ENV", RunnerBox);
                QVBoxLayout * RemEnvLayout = new QVBoxLayout(RemEnvBox);
                RemEnvBox->setLayout(RemEnvLayout);
                auto &RemEnvArr = (*MANIFESTJSON)["RUNNERS"][Platform][0]["REMOVE_ENV"];
                for (int k = 0; k < (int)RemEnvArr.size(); k++)
                {
                    QHBoxLayout * Row = new QHBoxLayout();
                    QLineEdit * RemField = new QLineEdit(RemEnvBox);
                    RemField->setProperty("JSONPath", QString("/RUNNERS/%1/0/REMOVE_ENV/%2").arg(QString::fromStdString(Platform)).arg(k));
                    if (RemEnvArr[k].is_string()) RemField->setText(QString::fromStdString(std::string(RemEnvArr[k])));
                    QObject::connect(RemField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    QPushButton * DelBtn = new QPushButton("✕", RemEnvBox);
                    DelBtn->setFixedWidth(28);
                    QObject::connect(DelBtn, &QPushButton::clicked, this, [this, Platform, k](){
                        (*MANIFESTJSON)["RUNNERS"][Platform][0]["REMOVE_ENV"].erase(k);
                        SaveManifestJSON(); BuildUI();
                    });
                    Row->addWidget(RemField); Row->addWidget(DelBtn);
                    RemEnvLayout->addLayout(Row);
                }
                QPushButton * AddRemEnvBtn = new QPushButton("+ Add", RemEnvBox);
                QObject::connect(AddRemEnvBtn, &QPushButton::clicked, this, [this, Platform](){
                    (*MANIFESTJSON)["RUNNERS"][Platform][0]["REMOVE_ENV"].push_back("");
                    SaveManifestJSON(); BuildUI();
                });
                RemEnvLayout->addWidget(AddRemEnvBtn);
                RunnerBoxLayout->addWidget(RemEnvBox);

                SubGameScrollLayout->addWidget(RunnerBox);
            }

            MakeSection("Metadata",  MetadataFields, "METADATA");
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

                //Move up/down buttons reorder components in the JSON array.
                QPushButton * MoveUpBtn = new QPushButton("↑", ComponentTabWidget);
                MoveUpBtn->setFixedWidth(30);
                MoveUpBtn->setEnabled(i > 0);
                SubComponentsToolbarLayout->addWidget(MoveUpBtn);
                QObject::connect(MoveUpBtn, &QPushButton::clicked, this, &PackageEditor::MoveComponentUp);

                QPushButton * MoveDownBtn = new QPushButton("↓", ComponentTabWidget);
                MoveDownBtn->setFixedWidth(30);
                MoveDownBtn->setEnabled(i < (int)(*MANIFESTJSON)["COMPONENTS"].size() - 1);
                SubComponentsToolbarLayout->addWidget(MoveDownBtn);
                QObject::connect(MoveDownBtn, &QPushButton::clicked, this, &PackageEditor::MoveComponentDown);

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

                QPushButton * AddVFSDirLayerButton = new QPushButton("Add VFSDirLayer", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AddVFSDirLayerButton);
                QObject::connect(AddVFSDirLayerButton, &QPushButton::clicked, this, &PackageEditor::AddVFSDirLayer);

                QPushButton * AddVFSZipLayerButton = new QPushButton("Add VFSZipLayer", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AddVFSZipLayerButton);
                QObject::connect(AddVFSZipLayerButton, &QPushButton::clicked, this, &PackageEditor::AddVFSZipLayer);

                QPushButton * AddVFSFileLayerButton = new QPushButton("Add VFSFileLayer", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AddVFSFileLayerButton);
                QObject::connect(AddVFSFileLayerButton, &QPushButton::clicked, this, &PackageEditor::AddVFSFileLayer);

                QPushButton * AddVariantDefButton = new QPushButton("Add VariantDefinition", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AddVariantDefButton);
                QObject::connect(AddVariantDefButton, &QPushButton::clicked, this, &PackageEditor::AddVariantDefinition);

                QPushButton * AddCustomVarButton = new QPushButton("Add CustomVar", ComponentTabWidget);
                SubComponentsToolbarLayout->addWidget(AddCustomVarButton);
                QObject::connect(AddCustomVarButton, &QPushButton::clicked, this, &PackageEditor::AddCustomVar);

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

                if (SubComponentType == "VariantDefinition")
                {
                    //Fields: EXECUTABLE_ID, VARIANT_ID, then the exe path (EXEPATH / ROM / DATAPATH), EXEARGS, WORKDIR.
                    static const QStringList ExeDefFields = {"EXECUTABLE_ID", "VARIANT_ID", "EXEPATH", "ROM", "DATAPATH", "EXEARGS", "WORKDIR"};
                    int row = 1;
                    for (const QString &Field : ExeDefFields)
                    {
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(Field + ":"), row, 0);
                        QLineEdit * FieldEdit = new QLineEdit(IndividualSubComponentGroupBox);
                        QString FieldPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/%3").arg(i).arg(j).arg(Field);
                        FieldEdit->setProperty("JSONPath", FieldPath);
                        auto &Val = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j][Field.toStdString()];
                        if (!Val.is_null() && Val.is_string())
                            FieldEdit->setText(QString::fromStdString(std::string(Val)));
                        QObject::connect(FieldEdit, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(FieldEdit, row, 1);
                        row++;
                    }

                    //Execute button — launches this specific variant directly.
                    QPushButton * VDExecBtn = new QPushButton("▶ Execute", IndividualSubComponentGroupBox);
                    IndividualSubComponentGroupBoxLayout->addWidget(VDExecBtn, row, 0, 1, -1);

                    //Capture by value so the lambda is valid after BuildUI rebuilds.
                    std::string CompID   = ((*MANIFESTJSON)["COMPONENTS"][i].contains("COMPONENTID") &&
                                            !(*MANIFESTJSON)["COMPONENTS"][i]["COMPONENTID"].is_null())
                                           ? std::string((*MANIFESTJSON)["COMPONENTS"][i]["COMPONENTID"]) : "";
                    auto &VDSub          = (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    std::string ExeID    = (VDSub.contains("EXECUTABLE_ID") && VDSub["EXECUTABLE_ID"].is_string())
                                           ? std::string(VDSub["EXECUTABLE_ID"]) : "";
                    std::string VariantID= (VDSub.contains("VARIANT_ID")    && VDSub["VARIANT_ID"].is_string())
                                           ? std::string(VDSub["VARIANT_ID"]) : "";

                    QObject::connect(VDExecBtn, &QPushButton::clicked, this,
                    [this, CompID, ExeID, VariantID]()
                    {
                        //Guards
                        if (CompID.empty())  { QMessageBox::warning(this, "Execute", "Component has no COMPONENTID."); return; }
                        if (ExeID.empty())   { QMessageBox::warning(this, "Execute", "VariantDefinition has no EXECUTABLE_ID."); return; }

                        //Collect runners for the runner picker.
                        std::vector<std::pair<QString, nlohmann::ordered_json>> Runners;
                        auto CollectR = [&](const nlohmann::ordered_json &Src)
                        {
                            if (!Src.contains("RUNNERS")) return;
                            for (auto &[Plat, PlatRunners] : Src["RUNNERS"].items())
                                for (auto &R : PlatRunners)
                                {
                                    std::string Name = (R.contains("NAME") && R["NAME"].is_string()) ? std::string(R["NAME"]) : "(unnamed)";
                                    Runners.push_back({QString::fromStdString(Name), R});
                                }
                        };
                        CollectR(*GlobalConfigJSON);
                        CollectR(*MANIFESTJSON);
                        if (Runners.empty()) { QMessageBox::warning(this, "Execute", "No runners defined."); return; }

                        //Runner picker dialog.
                        QDialog D(this);
                        D.setWindowTitle(QString("Execute: %1 [%2]")
                            .arg(QString::fromStdString(ExeID))
                            .arg(VariantID.empty() ? "(no variant)" : QString::fromStdString(VariantID)));
                        D.setMinimumWidth(380);
                        QVBoxLayout * DL = new QVBoxLayout(&D);
                        QFormLayout * DF = new QFormLayout(); DL->addLayout(DF);
                        QComboBox * RC = new QComboBox(&D);
                        for (auto &[Label, _] : Runners) RC->addItem(Label);
                        DF->addRow("Runner:", RC);
                        QHBoxLayout * BR = new QHBoxLayout(); DL->addLayout(BR); BR->addStretch();
                        QPushButton * OkBtn = new QPushButton("Execute", &D); OkBtn->setDefault(true);
                        QPushButton * CaBtn = new QPushButton("Cancel",  &D);
                        BR->addWidget(CaBtn); BR->addWidget(OkBtn);
                        QObject::connect(CaBtn, &QPushButton::clicked, &D, &QDialog::reject);
                        QObject::connect(OkBtn, &QPushButton::clicked, &D, &QDialog::accept);
                        if (D.exec() != QDialog::Accepted) return;

                        int RIdx = RC->currentIndex();
                        if (RIdx < 0 || RIdx >= (int)Runners.size()) return;
                        nlohmann::ordered_json SelectedRunner = Runners[RIdx].second;

                        //Build container with the component containing this VariantDefinition.
                        ContainerParams Params(PackageDir->path().toStdString(), "", CompID);
                        ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

                        //Apply selected runner.
                        Container.ContainerParams.RunnerName       = (SelectedRunner.contains("NAME")       && SelectedRunner["NAME"].is_string())       ? std::string(SelectedRunner["NAME"])       : "";
                        Container.ContainerParams.RunnerExecutable = (SelectedRunner.contains("EXECUTABLE") && SelectedRunner["EXECUTABLE"].is_string()) ? std::string(SelectedRunner["EXECUTABLE"]) : "";
                        std::string TypeStr = (SelectedRunner.contains("TYPE") && SelectedRunner["TYPE"].is_string()) ? std::string(SelectedRunner["TYPE"]) : "custom";
                        if      (TypeStr == "wine")     Container.ContainerParams.RunnerTypeEnum = RunnerType::Wine;
                        else if (TypeStr == "emulator") Container.ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
                        else if (TypeStr == "custom")   Container.ContainerParams.RunnerTypeEnum = RunnerType::Custom;
                        else                            Container.ContainerParams.RunnerTypeEnum = RunnerType::Native;
                        Container.ContainerParams.RunnerEnv.clear();
                        Container.ContainerParams.RunnerRemoveEnv.clear();
                        Container.ContainerParams.RunnerArgs.clear();
                        if (SelectedRunner.contains("ENV"))        Container.ContainerParams.RunnerEnv = SelectedRunner["ENV"];
                        if (SelectedRunner.contains("REMOVE_ENV")) for (auto &E : SelectedRunner["REMOVE_ENV"]) Container.ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
                        if (SelectedRunner.contains("ARGS"))       for (auto &A : SelectedRunner["ARGS"])       Container.ContainerParams.RunnerArgs.push_back(std::string(A));

                        //Re-derive paths for Wine vs non-Wine.
                        if (Container.ContainerParams.RunnerTypeEnum == RunnerType::Wine)
                        {
                            Container.ContainerParams.ProgramPath   = Container.ContainerParams.RuntimePath / "drive_c" / Container.ContainerParams.PackageUID;
                            Container.ContainerParams.DefPrefixPath = Container.ContainerParams.TempPath / "DEFPREFIX";
                            Container.ContainerParams.WindowsProgramPath = "C:\\" + Container.ContainerParams.PackageUID;
                            Container.ContainerParams.WindowsProgramPathDoubleBackSlash = "C:\\\\" + Container.ContainerParams.PackageUID;
                            Container.ContainerParams.WorkDirPathComplete = Container.ContainerParams.ProgramPath;
                        }
                        else Container.ContainerParams.ProgramPath = Container.ContainerParams.WorkDirPathComplete = Container.ContainerParams.RuntimePath;

                        //ExecutableID tells ResolveExecutableDefinition which VariantDefinition to use.
                        //The component_id constraint already ensures only one VariantDefinition with ExeID exists.
                        Container.ContainerParams.ExecutableID = ExeID;

                        if (!ContainerWrapper::ResolveExecutableDefinition(Container.ContainerParams))
                        { QMessageBox::critical(this, "Execute", "ResolveExecutableDefinition failed."); return; }

                        Container.Cleanup();
                        if (!Container.BuildContainerRuntime())
                        { QMessageBox::critical(this, "Execute", "BuildContainerRuntime failed."); Container.Cleanup(); return; }

                        if (!Container.Execute())
                            QMessageBox::warning(this, "Execute", "Process exited with an error.");

                        Container.Cleanup();
                    });
                }
                else if (SubComponentType == "VFSZipLayer" || SubComponentType == "VFSDirLayer" || SubComponentType == "VFSFileLayer")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("PATH:"), 1, 0);
                    QLineEdit * PathField = new QLineEdit(IndividualSubComponentGroupBox);
                    QString PathJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/PATH").arg(i).arg(j);
                    PathField->setProperty("JSONPath", PathJSONPath);
                    if (!(*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"].is_null())
                        PathField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"]));
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(PathField, 1, 1);

                    //Convert buttons — only for Dir↔Zip since FileLayer is a single file.
                    if (SubComponentType == "VFSDirLayer")
                    {
                        QPushButton * ConvertBtn = new QPushButton("→ ZIP", IndividualSubComponentGroupBox);
                        QObject::connect(ConvertBtn, &QPushButton::clicked, this, [this, i, j]()
                        {
                            std::string Path = (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j].value("PATH", std::string());
                            std::filesystem::path SrcDir  = std::filesystem::path(PackageFilesDir->path().toStdString()) / Path;
                            std::string ZipName           = Path + ".zip";
                            std::filesystem::path ZipFile = std::filesystem::path(PackageFilesDir->path().toStdString()) / ZipName;
                            //zip -r archive.zip srcdir — runs from PACKAGEFILES so the zip root equals Path.
                            ContainerWrapper::RunCommand("zip", {"-r", ZipFile.string(), SrcDir.string()});
                            std::filesystem::remove_all(SrcDir);
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["TYPE"] = "VFSZipLayer";
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"] = ZipName;
                            SaveManifestJSON(); BuildUI();
                        });
                        IndividualSubComponentGroupBoxLayout->addWidget(ConvertBtn, 1, 2);
                    }
                    else if (SubComponentType == "VFSZipLayer")
                    {
                        QPushButton * ConvertBtn = new QPushButton("→ DIR", IndividualSubComponentGroupBox);
                        QObject::connect(ConvertBtn, &QPushButton::clicked, this, [this, i, j]()
                        {
                            std::string Path = (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j].value("PATH", std::string());
                            std::filesystem::path ZipFile = std::filesystem::path(PackageFilesDir->path().toStdString()) / Path;
                            //Strip .zip extension for the directory name.
                            std::string DirName = (Path.size() > 4 && Path.substr(Path.size() - 4) == ".zip")
                                                  ? Path.substr(0, Path.size() - 4) : Path + "_dir";
                            std::filesystem::path DirPath = std::filesystem::path(PackageFilesDir->path().toStdString()) / DirName;
                            std::filesystem::create_directories(DirPath);
                            ContainerWrapper::RunCommand("unzip", {ZipFile.string(), "-d", DirPath.string()});
                            std::filesystem::remove(ZipFile);
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["TYPE"] = "VFSDirLayer";
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"] = DirName;
                            SaveManifestJSON(); BuildUI();
                        });
                        IndividualSubComponentGroupBoxLayout->addWidget(ConvertBtn, 1, 2);
                    }
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
                else if (SubComponentType == "CustomVar")
                {
                    static const QStringList CustomVarFields = {"KEY", "LABEL", "DEFAULT", "VARTYPE", "OPTIONS"};
                    int row = 1;
                    for (const QString &Field : CustomVarFields)
                    {
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(Field + ":"), row, 0);
                        QLineEdit * FieldEdit = new QLineEdit(IndividualSubComponentGroupBox);
                        QString FieldPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/%3").arg(i).arg(j).arg(Field);
                        FieldEdit->setProperty("JSONPath", FieldPath);
                        auto &Val = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j][Field.toStdString()];
                        if (!Val.is_null() && Val.is_string())
                            FieldEdit->setText(QString::fromStdString(std::string(Val)));
                        QObject::connect(FieldEdit, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(FieldEdit, row, 1);
                        row++;
                    }
                }
                else
                {
                    LogErr("PackageEditor", QString("Component %1, subcomponent %2 has unrecognised type (%3).").arg(i).arg(j).arg(SubComponentType).toStdString());
                    continue;
                }
                SubComponentsGroupBoxLayout->addWidget(IndividualSubComponentGroupBox);
            }
        QString ComponentTabLabel = ComponentIDStr.empty() ? QString("Component %1").arg(i + 1) : QString::fromStdString(ComponentIDStr);
        ui->PackageEditorTabWidget->addTab(ComponentTabWidget, ComponentTabLabel);
    }

    //Restore saved tab positions (clamped to valid range).
    int MainCount = ui->PackageEditorTabWidget->count();
    ui->PackageEditorTabWidget->setCurrentIndex(qBound(0, SavedMainTab, MainCount - 1));
    if (SubGamesTabWidget && SubGamesTabWidget->count() > 0)
        SubGamesTabWidget->setCurrentIndex(qBound(0, SavedSubgameTab, SubGamesTabWidget->count() - 1));

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
    if (runners.contains(platform))
    {
        //Non-empty array: read TYPE from the first runner definition.
        if (!runners[platform].empty())
            return runners[platform][0].value("TYPE", std::string("wine"));
        //Empty array: platform is reserved for manifest-defined runners (e.g. "Custom").
        return "custom";
    }
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
    std::string ComponentID = ComponentTabWidget->property("ComponentID").toString().toStdString();
    LogOut("PackageEditor", "REMOVE COMPONENT " + ComponentID);

    int Idx = ContainerWrapper::FindComponentIndex(*MANIFESTJSON, ComponentID);
    if (Idx == -1) { LogErr("PackageEditor", "Component not found: " + ComponentID); return; }
    (*PackageEditor::MANIFESTJSON)["COMPONENTS"].erase(Idx);
    LogOut("PackageEditor", "Deleted component: " + ComponentID);
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

    if (ComponentID.empty())
    {
        QMessageBox::warning(this, "Execute Component", "This component has no COMPONENTID set.");
        return;
    }

    //Build the container for this component to get SubComponentsArray populated.
    ContainerParams Params(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

    //Collect VariantDefinitions from the entire component chain.
    //Use a map keyed by (EXECUTABLE_ID|VARIANT_ID) so that each unique definition is shown once.
    std::map<std::string, nlohmann::ordered_json> ExeDefMap;
    for (auto &Sub : Container.ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "VariantDefinition") continue;
        std::string Key = Sub.value("EXECUTABLE_ID", std::string()) + "|" + Sub.value("VARIANT_ID", std::string());
        ExeDefMap[Key] = Sub;
    }

    std::vector<std::pair<QString, nlohmann::ordered_json>> ExeDefs;
    for (auto &[Key, Sub] : ExeDefMap)
    {
        QString ID        = QString::fromStdString(Sub.value("EXECUTABLE_ID", std::string("(no id)")));
        QString VariantID = QString::fromStdString(Sub.value("VARIANT_ID",    std::string()));
        QString Label     = VariantID.isEmpty() ? ID : ID + "  [" + VariantID + "]";
        ExeDefs.push_back({Label, Sub});
    }

    if (ExeDefs.empty())
    {
        QMessageBox::warning(this, "Execute Component", "No VariantDefinition subcomponents found in this component chain.\nAdd a VariantDefinition subcomponent first.");
        return;
    }

    //Collect all runners from GlobalConfig and MANIFEST into a flat list.
    //Stored as (display label, runner JSON) pairs; index matches the combobox.
    std::vector<std::pair<QString, nlohmann::ordered_json>> Runners;
    auto CollectRunners = [&](const nlohmann::ordered_json &Source, const QString &SourceLabel)
    {
        if (!Source.contains("RUNNERS")) return;
        for (auto &[Platform, PlatformRunners] : Source["RUNNERS"].items())
        {
            if (!PlatformRunners.is_array()) continue;
            for (auto &Runner : PlatformRunners)
            {
                QString Name    = QString::fromStdString(Runner.value("NAME", std::string("(unnamed)")));
                QString Display = Name + "  [" + QString::fromStdString(Platform) + "]  (" + SourceLabel + ")";
                Runners.push_back({Display, Runner});
            }
        }
    };
    CollectRunners(*GlobalConfigJSON, "global");
    CollectRunners(*MANIFESTJSON,     "manifest");

    if (Runners.empty())
    {
        QMessageBox::warning(this, "Execute Component", "No runners defined in GlobalConfig or the manifest.");
        return;
    }

    //Build the runner picker dialog.
    QDialog Dialog(this);
    Dialog.setWindowTitle(QString("Execute: %1").arg(QString::fromStdString(ComponentID)));
    Dialog.setMinimumWidth(500);
    QVBoxLayout * DLayout = new QVBoxLayout(&Dialog);
    QFormLayout * Form = new QFormLayout();
    DLayout->addLayout(Form);

    QComboBox * ExeDefPicker = new QComboBox(&Dialog);
    for (auto &[Label, _] : ExeDefs)
        ExeDefPicker->addItem(Label);
    Form->addRow("Executable:", ExeDefPicker);

    QComboBox * RunnerPicker = new QComboBox(&Dialog);
    for (auto &[Label, _] : Runners)
        RunnerPicker->addItem(Label);
    Form->addRow("Runner:", RunnerPicker);

    QHBoxLayout * BtnRow = new QHBoxLayout();
    DLayout->addLayout(BtnRow);
    BtnRow->addStretch();
    QPushButton * CancelBtn  = new QPushButton("Cancel",  &Dialog);
    QPushButton * ExecuteBtn = new QPushButton("Execute", &Dialog);
    ExecuteBtn->setDefault(true);
    BtnRow->addWidget(CancelBtn);
    BtnRow->addWidget(ExecuteBtn);
    QObject::connect(CancelBtn,  &QPushButton::clicked, &Dialog, &QDialog::reject);
    QObject::connect(ExecuteBtn, &QPushButton::clicked, &Dialog, &QDialog::accept);

    if (Dialog.exec() != QDialog::Accepted) return;

    int RunnerIdx = RunnerPicker->currentIndex();
    int ExeDefIdx = ExeDefPicker->currentIndex();
    if (RunnerIdx < 0 || RunnerIdx >= (int)Runners.size()) return;
    if (ExeDefIdx < 0 || ExeDefIdx >= (int)ExeDefs.size()) return;

    nlohmann::ordered_json SelectedRunner = Runners[RunnerIdx].second;
    nlohmann::ordered_json SelectedExeDef = ExeDefs[ExeDefIdx].second;

    //Container was already built above to populate SubComponentsArray/ExeDefs.
    //Now apply runner and resolve exe definition.

    //Set ExecutableID from the chosen VariantDefinition, then resolve.
    Container.ContainerParams.ExecutableID = SelectedExeDef.value("EXECUTABLE_ID", std::string());

    //Override all runner fields from the user's selection.
    //Must be done after construction since DeriveContainerParams has already run.
    Container.ContainerParams.RunnerName       = SelectedRunner.value("NAME",       std::string());
    Container.ContainerParams.RunnerExecutable = SelectedRunner.value("EXECUTABLE", std::string());
    std::string TypeStr                        = SelectedRunner.value("TYPE",        std::string("wine"));
    if      (TypeStr == "wine")      Container.ContainerParams.RunnerTypeEnum = RunnerType::Wine;
    else if (TypeStr == "emulator")  Container.ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
    else if (TypeStr == "custom")    Container.ContainerParams.RunnerTypeEnum = RunnerType::Custom;
    else                             Container.ContainerParams.RunnerTypeEnum = RunnerType::Native;
    Container.ContainerParams.RunnerEnv       = SelectedRunner.contains("ENV")        ? SelectedRunner["ENV"]        : nlohmann::ordered_json::object();
    Container.ContainerParams.RunnerRemoveEnv.clear();
    Container.ContainerParams.RunnerArgs.clear();
    if (SelectedRunner.contains("REMOVE_ENV")) for (auto &E : SelectedRunner["REMOVE_ENV"]) Container.ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
    if (SelectedRunner.contains("ARGS"))       for (auto &A : SelectedRunner["ARGS"])       Container.ContainerParams.RunnerArgs.push_back(std::string(A));

    //Re-derive path layout — only ProgramPath, DefPrefixPath and WorkDir differ between Wine and non-Wine.
    //RuntimePath and TempPath are already set correctly by DeriveContainerParams.
    if (Container.ContainerParams.RunnerTypeEnum == RunnerType::Wine)
    {
        Container.ContainerParams.ProgramPath                        = Container.ContainerParams.RuntimePath / "drive_c" / Container.ContainerParams.PackageUID;
        Container.ContainerParams.DefPrefixPath                      = Container.ContainerParams.TempPath / "DEFPREFIX";
        Container.ContainerParams.WindowsProgramPath                 = "C:\\" + Container.ContainerParams.PackageUID;
        Container.ContainerParams.WindowsProgramPathDoubleBackSlash  = "C:\\\\" + Container.ContainerParams.PackageUID;
        Container.ContainerParams.WorkDirPathComplete                = Container.ContainerParams.ProgramPath;
    }
    else
    {
        Container.ContainerParams.ProgramPath         = Container.ContainerParams.RuntimePath;
        Container.ContainerParams.WorkDirPathComplete = Container.ContainerParams.RuntimePath;
    }

    if (!ContainerWrapper::ResolveExecutableDefinition(Container.ContainerParams))
    {
        QMessageBox::critical(this, "Execute Component", "Failed to resolve VariantDefinition.\nCheck EXECUTABLE_ID and VARIANT_ID.");
        return;
    }

    Container.Cleanup(); //Remove any leftover RUNTIME/TEMP from a previous run.

    if (!Container.BuildContainerRuntime())
    {
        QMessageBox::critical(this, "Execute Component", "Failed to build container runtime.\nCheck the log for details.");
        Container.Cleanup();
        return;
    }

    if (!Container.Execute())
        QMessageBox::warning(this, "Execute Component", "Process exited with an error.\nCheck the log for details.");

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

void PackageEditor::AddVFSDirLayer()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QString Selected = QFileDialog::getExistingDirectory(this, "Select directory to add as VFSDirLayer");
    if (Selected.isEmpty()) return;
    QString DirName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + DirName);
    QDir().rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSDirLayer"}, {"PATH", DirName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddVFSZipLayer()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QString Selected = QFileDialog::getOpenFileName(this, "Select ZIP file to add as VFSZipLayer", "", "ZIP files (*.zip)");
    if (Selected.isEmpty()) return;
    QString ZipName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + ZipName);
    QFile::rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSZipLayer"}, {"PATH", ZipName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddVFSFileLayer()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    QString Selected = QFileDialog::getOpenFileName(this, "Select file to add as VFSFileLayer");
    if (Selected.isEmpty()) return;
    QString FileName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + FileName);
    QFile::rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSFileLayer"}, {"PATH", FileName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddVariantDefinition()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "VariantDefinition"}, {"EXECUTABLE_ID", "main"}, {"VARIANT_ID", ""}, {"EXEPATH", ""}
    }));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddCustomVar()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "CustomVar"}, {"KEY", ""}, {"LABEL", ""}, {"DEFAULT", ""}, {"VARTYPE", "string"}
    }));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::MoveComponentUp()
{
    std::string ComponentID = qobject_cast<QPushButton*>(sender())->parentWidget()->property("ComponentID").toString().toStdString();
    int Idx = ContainerWrapper::FindComponentIndex(*MANIFESTJSON, ComponentID);
    if (Idx <= 0) return;
    auto Tmp = (*MANIFESTJSON)["COMPONENTS"][Idx];
    (*MANIFESTJSON)["COMPONENTS"][Idx]     = (*MANIFESTJSON)["COMPONENTS"][Idx - 1];
    (*MANIFESTJSON)["COMPONENTS"][Idx - 1] = Tmp;
    SaveManifestJSON();
    SavedMainTab = (Idx - 1) + 2; // +2 for JSON and MANIFEST tabs
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::MoveComponentDown()
{
    std::string ComponentID = qobject_cast<QPushButton*>(sender())->parentWidget()->property("ComponentID").toString().toStdString();
    int Idx = ContainerWrapper::FindComponentIndex(*MANIFESTJSON, ComponentID);
    int Total = (int)(*MANIFESTJSON)["COMPONENTS"].size();
    if (Idx < 0 || Idx >= Total - 1) return;
    auto Tmp = (*MANIFESTJSON)["COMPONENTS"][Idx];
    (*MANIFESTJSON)["COMPONENTS"][Idx]     = (*MANIFESTJSON)["COMPONENTS"][Idx + 1];
    (*MANIFESTJSON)["COMPONENTS"][Idx + 1] = Tmp;
    SaveManifestJSON();
    SavedMainTab = (Idx + 1) + 2;
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::FinalizeComponent()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    LogOut("PackageEditor", "Finalizing component " + Button->parentWidget()->property("ComponentID").toString().toStdString());
}

//Saves image data to METADATA/<SUBGAMEID>_cover.<ext>, sets COVER in manifest, and updates the label.
void PackageEditor::ApplyCoverImage(QLabel *CoverLabel, const QByteArray &Data, const QString &Extension, int SubgameIndex)
{
    if (Data.isEmpty()) return;

    //Build filename from SUBGAMEID or fallback to index.
    std::string SubgameID;
    if (SubgameIndex < (int)(*MANIFESTJSON)["SUBGAMES"].size())
    {
        auto &IDField = (*MANIFESTJSON)["SUBGAMES"][SubgameIndex]["SUBGAMEID"];
        if (!IDField.is_null() && IDField.is_string()) SubgameID = std::string(IDField);
    }
    QString FileName = (SubgameID.empty() ? QString("subgame%1").arg(SubgameIndex) : QString::fromStdString(SubgameID))
                       + "_cover." + Extension.toLower();
    QString DestPath = QDir::cleanPath(MetadataDir->path() + "/" + FileName);

    //Write the image file.
    QFile OutFile(DestPath);
    if (!OutFile.open(QIODevice::WriteOnly)) { LogErr("PackageEditor", "Could not write cover: " + DestPath.toStdString()); return; }
    OutFile.write(Data);
    OutFile.close();

    //Update MANIFEST METADATA.COVER.
    nlohmann::ordered_json::json_pointer CoverPtr(QString("/SUBGAMES/%1/METADATA/COVER").arg(SubgameIndex).toStdString());
    (*MANIFESTJSON)[CoverPtr] = FileName.toStdString();
    SaveManifestJSON();

    //Update the label pixmap.
    QPixmap Pix;
    Pix.loadFromData(Data);
    if (!Pix.isNull())
        CoverLabel->setPixmap(Pix.scaled(150, 225, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    LogSucc("PackageEditor", "Cover set: " + FileName.toStdString());
}

bool PackageEditor::eventFilter(QObject *obj, QEvent *event)
{
    QLabel *CoverLabel = qobject_cast<QLabel*>(obj);
    if (!CoverLabel || !CoverLabel->property("SubgameIndex").isValid())
        return QDialog::eventFilter(obj, event);

    if (event->type() == QEvent::DragEnter)
    {
        QDragEnterEvent *ev = static_cast<QDragEnterEvent*>(event);
        if (ev->mimeData()->hasImage() || ev->mimeData()->hasUrls() || ev->mimeData()->hasText())
            ev->acceptProposedAction();
        return true;
    }

    if (event->type() == QEvent::DragMove)
    {
        static_cast<QDragMoveEvent*>(event)->acceptProposedAction();
        return true;
    }

    if (event->type() == QEvent::Drop)
    {
        QDropEvent *ev = static_cast<QDropEvent*>(event);
        int Idx = CoverLabel->property("SubgameIndex").toInt();
        const QMimeData *Mime = ev->mimeData();

        //1. Direct image data (drag from image viewer, browser image, etc.)
        if (Mime->hasImage())
        {
            QImage Img = qvariant_cast<QImage>(Mime->imageData());
            QByteArray Data;
            QBuffer Buf(&Data);
            Buf.open(QIODevice::WriteOnly);
            Img.save(&Buf, "PNG");
            ApplyCoverImage(CoverLabel, Data, "png", Idx);
            return true;
        }

        //2. URL(s) — could be local file or http(s)
        if (Mime->hasUrls())
        {
            QUrl Url = Mime->urls().first();
            if (Url.isLocalFile())
            {
                //Local file — read and copy.
                QString FilePath = Url.toLocalFile();
                QFile F(FilePath);
                if (F.open(QIODevice::ReadOnly))
                {
                    QByteArray Data = F.readAll();
                    QString Ext = QFileInfo(FilePath).suffix();
                    if (Ext.isEmpty()) Ext = "png";
                    ApplyCoverImage(CoverLabel, Data, Ext, Idx);
                }
            }
            else
            {
                //Remote URL — download synchronously.
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply *Reply = NetMgr->get(QNetworkRequest(Url));
                QEventLoop Loop;
                QObject::connect(Reply, &QNetworkReply::finished, &Loop, &QEventLoop::quit);
                Loop.exec();
                if (Reply->error() == QNetworkReply::NoError)
                {
                    QByteArray Data = Reply->readAll();
                    //Detect extension from URL or Content-Type.
                    QString Ext = QFileInfo(Url.path()).suffix();
                    if (Ext.isEmpty())
                    {
                        QString CT = Reply->header(QNetworkRequest::ContentTypeHeader).toString();
                        if (CT.contains("jpeg") || CT.contains("jpg")) Ext = "jpg";
                        else if (CT.contains("webp")) Ext = "webp";
                        else Ext = "png";
                    }
                    ApplyCoverImage(CoverLabel, Data, Ext, Idx);
                }
                else { LogErr("PackageEditor", "Failed to download cover: " + Reply->errorString().toStdString()); }
                Reply->deleteLater();
            }
            return true;
        }

        //3. Plain text that looks like a URL (some browsers drop as text/plain)
        if (Mime->hasText())
        {
            QString Text = Mime->text().trimmed();
            QUrl Url(Text);
            if (Url.isValid() && (Url.scheme() == "http" || Url.scheme() == "https"))
            {
                if (!NetMgr) NetMgr = new QNetworkAccessManager(this);
                QNetworkReply *Reply = NetMgr->get(QNetworkRequest(Url));
                QEventLoop Loop;
                QObject::connect(Reply, &QNetworkReply::finished, &Loop, &QEventLoop::quit);
                Loop.exec();
                if (Reply->error() == QNetworkReply::NoError)
                {
                    QByteArray Data = Reply->readAll();
                    QString Ext = QFileInfo(Url.path()).suffix();
                    if (Ext.isEmpty()) Ext = "png";
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
