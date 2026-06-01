#include "packageeditor.h"
#include "commonutils.h"
#include <QPushButton>
#include <iostream>
#include <QBuffer>
#include <QImage>

using json = nlohmann::ordered_json;

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent, const QString &PreselectedPath)
    : QDialog(parent)
{
    setWindowTitle("VidyaGod Package Editor");
    setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    setWindowState(Qt::WindowMaximized);

    // Main layout: toolbar on top, tab widget below.
    QVBoxLayout * MainLayout = new QVBoxLayout(this);
    MainLayout->setSpacing(1);
    MainLayout->setContentsMargins(0, 0, 0, 0);
    setLayout(MainLayout);

    // Toolbar
    QHBoxLayout * Toolbar = new QHBoxLayout();
    Toolbar->setSpacing(1);
    QPushButton * AddSubGameBtn   = new QPushButton("Add SubGame",   this);
    QPushButton * AddComponentBtn = new QPushButton("Add Component", this);
    QPushButton * SaveBtn         = new QPushButton("Save",          this);
    Toolbar->addWidget(AddSubGameBtn);
    Toolbar->addWidget(AddComponentBtn);
    Toolbar->addStretch();
    Toolbar->addWidget(SaveBtn);
    MainLayout->addLayout(Toolbar);

    connect(AddSubGameBtn,   &QPushButton::clicked, this, &PackageEditor::on_AddSubGameButton_clicked);
    connect(AddComponentBtn, &QPushButton::clicked, this, &PackageEditor::on_AddComponentButton_clicked);
    connect(SaveBtn,         &QPushButton::clicked, this, &PackageEditor::on_SaveButton_clicked);

    // Tab widget (populated by BuildUI)
    PackageEditorTabWidget = new QTabWidget(this);
    MainLayout->addWidget(PackageEditorTabWidget);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;

    InitPackage(PreselectedPath);
    InitMANIFESTJSON();
    BuildUI();
    RefreshJSONView();
}

PackageEditor::~PackageEditor() = default;

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
    PackageEditorTabWidget->addTab(NewTabWidget, QString("Component %1").arg(PackageEditorTabWidget->count() - 1));
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
    SavedMainTab    = PackageEditorTabWidget->currentIndex();
    SavedSubgameTab = SubGamesTabWidget ? SubGamesTabWidget->currentIndex() : 0;

    //JSON TAB
    PackageEditorTabWidget->clear();
    SubGamesTabWidget = nullptr; // Reset stale pointer — widgets are orphaned but alive until parent dies

    PackageEditor::JSONTabWidget = new QWidget(PackageEditorTabWidget);
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

    PackageEditorTabWidget->addTab(JSONTabWidget, "JSON");

    //MANIFEST TAB
    PackageEditor::ManifestTabWidget = new QWidget(PackageEditorTabWidget);
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
        // Execution section removed — the recommended entrypoint is now marked per-entrypoint.

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
                        if (idx >= 0) { QSignalBlocker Blocker(PlatformPicker); PlatformPicker->setCurrentIndex(idx); }
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

            MakeSection("Identity", IdentityFields);

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

            // ENTRYPOINTS group — one card per entrypoint in SUBGAMES[i].ENTRYPOINTS
            {
                QGroupBox * EPBox = new QGroupBox("Entrypoints", SubGameScrollContents);
                QVBoxLayout * EPBoxLayout = new QVBoxLayout(EPBox);
                EPBox->setLayout(EPBoxLayout);

                if (!SubgameRef.contains("ENTRYPOINTS") || !SubgameRef["ENTRYPOINTS"].is_array())
                    (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"] = nlohmann::ordered_json::array();

                auto &EntrypointArr = (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"];
                for (int epj = 0; epj < (int)EntrypointArr.size(); epj++)
                {
                    auto &EP = EntrypointArr[epj];
                    std::string CurEPID    = EP.value("ENTRYPOINT_ID", std::string());
                    std::string CurLastComp = EP.value("LASTCOMPONENT", std::string());

                    QString EPBoxTitle = CurEPID.empty()
                        ? QString("Entrypoint %1").arg(epj + 1)
                        : QString::fromStdString(CurEPID);
                    QGroupBox * EPCard = new QGroupBox(EPBoxTitle, EPBox);
                    QGridLayout * EPCardLayout = new QGridLayout(EPCard);
                    EPCard->setLayout(EPCardLayout);
                    int eprow = 0;

                    // ENTRYPOINT_ID
                    EPCardLayout->addWidget(new QLabel("ENTRYPOINT_ID:"), eprow, 0);
                    QLineEdit * EPIDField = new QLineEdit(EPCard);
                    EPIDField->setProperty("JSONPath", QString("/SUBGAMES/%1/ENTRYPOINTS/%2/ENTRYPOINT_ID").arg(i).arg(epj));
                    EPIDField->setText(QString::fromStdString(CurEPID));
                    QObject::connect(EPIDField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    EPCardLayout->addWidget(EPIDField, eprow, 1);
                    QPushButton * EPRemBtn = new QPushButton("✕", EPCard);
                    EPRemBtn->setFixedWidth(28);
                    QObject::connect(EPRemBtn, &QPushButton::clicked, this, [this, i, epj](){
                        (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"].erase(epj);
                        SaveManifestJSON(); BuildUI();
                    });
                    EPCardLayout->addWidget(EPRemBtn, eprow, 2);
                    eprow++;

                    // RECOMMENDED checkbox — marks this as the default entrypoint (mutually exclusive).
                    {
                        bool IsRec = EP.value("RECOMMENDED", false);
                        QCheckBox * RecCheck = new QCheckBox("Recommended (default)", EPCard);
                        RecCheck->setChecked(IsRec);
                        QObject::connect(RecCheck, &QCheckBox::toggled, this, [this, i, epj, RecCheck](){
                            if (RecCheck->isChecked())
                            {
                                // Clear RECOMMENDED on all other entrypoints in this subgame.
                                auto &EPArr = (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"];
                                for (int k = 0; k < (int)EPArr.size(); k++)
                                    EPArr[k]["RECOMMENDED"] = (k == epj);
                            }
                            else
                            {
                                (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["RECOMMENDED"] = false;
                            }
                            SaveManifestJSON(); RefreshJSONView();
                        });
                        EPCardLayout->addWidget(RecCheck, eprow, 0, 1, 3);
                        eprow++;
                    }

                    // LASTCOMPONENT — combobox from all components
                    EPCardLayout->addWidget(new QLabel("LASTCOMPONENT:"), eprow, 0);
                    QComboBox * LastCompPicker = new QComboBox(EPCard);
                    LastCompPicker->addItem("(none)", QString());
                    if ((*MANIFESTJSON).contains("COMPONENTS"))
                        for (auto &Comp : (*MANIFESTJSON)["COMPONENTS"])
                        {
                            std::string CID   = Comp.value("COMPONENTID", std::string());
                            std::string CName = Comp.value("NAME", std::string());
                            QString Label = CName.empty()
                                ? QString::fromStdString(CID)
                                : QString::fromStdString(CName + " (" + CID + ")");
                            LastCompPicker->addItem(Label, QString::fromStdString(CID));
                        }
                    {
                        int idx = LastCompPicker->findData(QString::fromStdString(CurLastComp));
                        if (idx >= 0) { QSignalBlocker B(LastCompPicker); LastCompPicker->setCurrentIndex(idx); }
                    }
                    QObject::connect(LastCompPicker, &QComboBox::currentIndexChanged, this, [this, i, epj, LastCompPicker](){
                        QString Sel = LastCompPicker->currentData().toString();
                        nlohmann::ordered_json::json_pointer P(QString("/SUBGAMES/%1/ENTRYPOINTS/%2/LASTCOMPONENT").arg(i).arg(epj).toStdString());
                        (*MANIFESTJSON)[P] = Sel.isEmpty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(Sel.toStdString());
                        SaveManifestJSON(); RefreshJSONView();
                    });
                    EPCardLayout->addWidget(LastCompPicker, eprow, 1, 1, 2);
                    eprow++;

                    // EXEPATH, EXEARGS, WORKDIR
                    for (const auto &FieldKey : std::vector<std::string>{"EXEPATH", "EXEARGS", "WORKDIR"})
                    {
                        EPCardLayout->addWidget(new QLabel(QString::fromStdString(FieldKey) + ":"), eprow, 0);
                        QLineEdit * FE = new QLineEdit(EPCard);
                        FE->setProperty("JSONPath", QString("/SUBGAMES/%1/ENTRYPOINTS/%2/%3").arg(i).arg(epj).arg(QString::fromStdString(FieldKey)));
                        if (EP.contains(FieldKey) && EP[FieldKey].is_string())
                            FE->setText(QString::fromStdString(std::string(EP[FieldKey])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        EPCardLayout->addWidget(FE, eprow, 1, 1, 2);
                        eprow++;
                    }

                    // FORCEVARS — per-entrypoint variable seeds (key → value pairs)
                    {
                        if (!EP.contains("FORCEVARS") || !EP["FORCEVARS"].is_object())
                            (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"] = nlohmann::ordered_json::object();
                        auto &FVObj = (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"];

                        QGroupBox * FVBox = new QGroupBox("FORCEVARS", EPCard);
                        QVBoxLayout * FVLayout = new QVBoxLayout(FVBox);
                        FVBox->setLayout(FVLayout);

                        for (auto &[FVKey, FVVal] : FVObj.items())
                        {
                            std::string KeyCopy = FVKey;
                            QHBoxLayout * FVRow = new QHBoxLayout();
                            QLineEdit * FVKeyField = new QLineEdit(FVBox);
                            FVKeyField->setText(QString::fromStdString(KeyCopy));
                            FVKeyField->setPlaceholderText("KEY");
                            QLineEdit * FVValField = new QLineEdit(FVBox);
                            FVValField->setProperty("JSONPath", QString("/SUBGAMES/%1/ENTRYPOINTS/%2/FORCEVARS/%3").arg(i).arg(epj).arg(QString::fromStdString(KeyCopy)));
                            if (FVVal.is_string()) FVValField->setText(QString::fromStdString(std::string(FVVal)));
                            QObject::connect(FVValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QObject::connect(FVKeyField, &QLineEdit::editingFinished, this, [this, i, epj, KeyCopy, FVKeyField, FVValField](){
                                QString NewKey = FVKeyField->text();
                                if (NewKey.isEmpty() || NewKey.toStdString() == KeyCopy) return;
                                auto Val = (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"][KeyCopy];
                                (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"].erase(KeyCopy);
                                (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"][NewKey.toStdString()] = Val;
                                FVValField->setProperty("JSONPath", QString("/SUBGAMES/%1/ENTRYPOINTS/%2/FORCEVARS/%3").arg(i).arg(epj).arg(NewKey));
                                SaveManifestJSON(); BuildUI();
                            });
                            QPushButton * FVDel = new QPushButton("✕", FVBox);
                            FVDel->setFixedWidth(28);
                            QObject::connect(FVDel, &QPushButton::clicked, this, [this, i, epj, KeyCopy](){
                                (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"].erase(KeyCopy);
                                SaveManifestJSON(); BuildUI();
                            });
                            FVRow->addWidget(FVKeyField); FVRow->addWidget(FVValField); FVRow->addWidget(FVDel);
                            FVLayout->addLayout(FVRow);
                        }
                        QPushButton * FVAddBtn = new QPushButton("+ Add Force Var", FVBox);
                        QObject::connect(FVAddBtn, &QPushButton::clicked, this, [this, i, epj](){
                            (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"][epj]["FORCEVARS"]["NEW_KEY"] = "";
                            SaveManifestJSON(); BuildUI();
                        });
                        FVLayout->addWidget(FVAddBtn);
                        EPCardLayout->addWidget(FVBox, eprow, 0, 1, -1);
                        eprow++;
                    }

                    // ▶ Execute button
                    QPushButton * EPExecBtn = new QPushButton("▶ Execute", EPCard);
                    EPCardLayout->addWidget(EPExecBtn, eprow, 0, 1, -1);
                    std::string EPSubgameID  = SubgameIDStr;
                    std::string EPEntryID    = CurEPID;
                    std::string EPLastComp   = CurLastComp;
                    QObject::connect(EPExecBtn, &QPushButton::clicked, this,
                    [this, EPSubgameID, EPEntryID, EPLastComp]()
                    {
                        if (EPLastComp.empty())
                        { QMessageBox::warning(this, "Execute", "Entrypoint has no LASTCOMPONENT set."); return; }
                        if (EPEntryID.empty())
                        { QMessageBox::warning(this, "Execute", "Entrypoint has no ENTRYPOINT_ID set."); return; }

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

                        QDialog D(this);
                        D.setWindowTitle(QString("Execute: %1").arg(QString::fromStdString(EPEntryID)));
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

                        ContainerParams Params(PackageDir->path().toStdString(), EPSubgameID, EPLastComp);
                        ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

                        Container.ContainerParams.RunnerName       = SelectedRunner.value("NAME",       std::string());
                        Container.ContainerParams.RunnerExecutable = SelectedRunner.value("EXECUTABLE", std::string());
                        std::string TypeStr = SelectedRunner.value("TYPE", std::string("wine"));
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

                        if (Container.ContainerParams.RunnerTypeEnum == RunnerType::Wine)
                        {
                            Container.ContainerParams.ProgramPath   = Container.ContainerParams.RuntimePath / "drive_c" / Container.ContainerParams.PackageUID;
                            Container.ContainerParams.DefPrefixPath = Container.ContainerParams.TempPath / "DEFPREFIX";
                            Container.ContainerParams.WindowsProgramPath = "C:\\" + Container.ContainerParams.PackageUID;
                            Container.ContainerParams.WindowsProgramPathDoubleBackSlash = "C:\\\\" + Container.ContainerParams.PackageUID;
                            Container.ContainerParams.WorkDirPathComplete = Container.ContainerParams.ProgramPath;
                        }
                        else Container.ContainerParams.ProgramPath = Container.ContainerParams.WorkDirPathComplete = Container.ContainerParams.RuntimePath;

                        Container.ContainerParams.ExecutableID = EPEntryID;

                        if (!ContainerWrapper::ResolveExecutableDefinition(*MANIFESTJSON, Container.ContainerParams))
                        { QMessageBox::critical(this, "Execute", "ResolveExecutableDefinition failed."); return; }

                        Container.Cleanup();
                        if (!Container.BuildContainerRuntime())
                        { QMessageBox::critical(this, "Execute", "BuildContainerRuntime failed."); Container.Cleanup(); return; }

                        if (!Container.Execute())
                            QMessageBox::warning(this, "Execute", "Process exited with an error.");

                        Container.Cleanup();
                    });

                    EPBoxLayout->addWidget(EPCard);
                }

                QPushButton * AddEPBtn = new QPushButton("+ Add Entrypoint", EPBox);
                QObject::connect(AddEPBtn, &QPushButton::clicked, this, [this, i](){
                    (*MANIFESTJSON)["SUBGAMES"][i]["ENTRYPOINTS"].push_back(json::object({
                        {"ENTRYPOINT_ID", "main"}, {"LASTCOMPONENT", ""}, {"EXEPATH", ""}, {"EXEARGS", ""}, {"WORKDIR", ""}
                    }));
                    SaveManifestJSON(); BuildUI();
                });
                EPBoxLayout->addWidget(AddEPBtn);
                SubGameScrollLayout->addWidget(EPBox);
            }

            MakeSection("Metadata",  MetadataFields, "METADATA");
            SubGameScrollLayout->addStretch();

            QString TabLabel = SubgameIDStr.empty() ? QString("Subgame %1").arg(i + 1) : QString::fromStdString(SubgameIDStr);
            SubGamesTabWidget->addTab(SubGameTabWidget, TabLabel);
        }
        PackageEditorTabWidget->addTab(ManifestTabWidget, "MANIFEST");

    // CUSTOMVARS TAB
    {
        QWidget * CVTabWidget = new QWidget(PackageEditorTabWidget);
        QVBoxLayout * CVTabLayout = new QVBoxLayout(CVTabWidget);
        CVTabWidget->setLayout(CVTabLayout);

        QScrollArea * CVScroll = new QScrollArea(CVTabWidget);
        CVScroll->setWidgetResizable(true);
        CVScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        CVTabLayout->addWidget(CVScroll);

        QWidget * CVScrollContents = new QWidget();
        QVBoxLayout * CVScrollLayout = new QVBoxLayout(CVScrollContents);
        CVScrollContents->setLayout(CVScrollLayout);
        CVScroll->setWidget(CVScrollContents);

        if (!(*MANIFESTJSON).contains("CUSTOMVARS") || !(*MANIFESTJSON)["CUSTOMVARS"].is_array())
            (*MANIFESTJSON)["CUSTOMVARS"] = nlohmann::ordered_json::array();

        auto &CVArr = (*MANIFESTJSON)["CUSTOMVARS"];
        for (int cvi = 0; cvi < (int)CVArr.size(); cvi++)
        {
            auto &CV = CVArr[cvi];
            std::string CVKey   = CV.value("KEY", std::string());
            std::string VarType = CV.value("VARTYPE", std::string("string"));
            bool Display        = CV.value("DISPLAY", true);

            QString CardTitle = CVKey.empty() ? QString("CustomVar %1").arg(cvi + 1) : QString::fromStdString(CVKey);
            QGroupBox * CVCard = new QGroupBox(CardTitle, CVScrollContents);
            QGridLayout * CVCardLayout = new QGridLayout(CVCard);
            CVCard->setLayout(CVCardLayout);
            int cvrow = 0;

            // ✕ Remove button (top-right)
            QPushButton * CVRemBtn = new QPushButton("✕", CVCard);
            CVRemBtn->setFixedWidth(28);
            QObject::connect(CVRemBtn, &QPushButton::clicked, this, [this, cvi](){
                (*MANIFESTJSON)["CUSTOMVARS"].erase(cvi);
                SaveManifestJSON(); BuildUI();
            });
            CVCardLayout->addWidget(CVRemBtn, 0, 2);

            // Simple text fields: KEY, LABEL, DEFAULT
            for (const auto &FieldKey : std::vector<std::string>{"KEY", "LABEL", "DEFAULT"})
            {
                CVCardLayout->addWidget(new QLabel(QString::fromStdString(FieldKey) + ":"), cvrow, 0);
                QLineEdit * FE = new QLineEdit(CVCard);
                FE->setProperty("JSONPath", QString("/CUSTOMVARS/%1/%2").arg(cvi).arg(QString::fromStdString(FieldKey)));
                if (CV.contains(FieldKey) && CV[FieldKey].is_string())
                    FE->setText(QString::fromStdString(std::string(CV[FieldKey])));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                CVCardLayout->addWidget(FE, cvrow, 1, 1, 2);
                cvrow++;
            }

            // VARTYPE combobox
            CVCardLayout->addWidget(new QLabel("VARTYPE:"), cvrow, 0);
            QComboBox * VTCombo = new QComboBox(CVCard);
            VTCombo->addItems({"string", "number", "dword", "qword", "bool", "options", "random"});
            VTCombo->setCurrentText(QString::fromStdString(VarType));
            QObject::connect(VTCombo, &QComboBox::currentIndexChanged, this, [this, cvi, VTCombo](){
                nlohmann::ordered_json::json_pointer P(QString("/CUSTOMVARS/%1/VARTYPE").arg(cvi).toStdString());
                (*MANIFESTJSON)[P] = VTCombo->currentText().toStdString();
                SaveManifestJSON(); BuildUI();
            });
            CVCardLayout->addWidget(VTCombo, cvrow, 1, 1, 2);
            cvrow++;

            // DISPLAY checkbox
            CVCardLayout->addWidget(new QLabel("DISPLAY:"), cvrow, 0);
            QCheckBox * DisplayCheck = new QCheckBox(CVCard);
            DisplayCheck->setChecked(Display);
            QObject::connect(DisplayCheck, &QCheckBox::toggled, this, [this, cvi, DisplayCheck](){
                nlohmann::ordered_json::json_pointer P(QString("/CUSTOMVARS/%1/DISPLAY").arg(cvi).toStdString());
                (*MANIFESTJSON)[P] = DisplayCheck->isChecked();
                SaveManifestJSON(); RefreshJSONView();
            });
            CVCardLayout->addWidget(DisplayCheck, cvrow, 1, 1, 2);
            cvrow++;

            // OPTIONS section — shown for "options" (label+value pairs) and "random" (value-only pool)
            if (VarType == "options" || VarType == "random")
            {
                if (!CV.contains("OPTIONS") || !CV["OPTIONS"].is_array())
                    (*MANIFESTJSON)["CUSTOMVARS"][cvi]["OPTIONS"] = nlohmann::ordered_json::array();

                bool IsRandom = (VarType == "random");
                QString BoxTitle = IsRandom ? "Value Pool (random pick on launch)" : "OPTIONS";
                QGroupBox * OptsBox = new QGroupBox(BoxTitle, CVCard);
                QVBoxLayout * OptsLayout = new QVBoxLayout(OptsBox);
                OptsBox->setLayout(OptsLayout);

                auto &Opts = (*MANIFESTJSON)["CUSTOMVARS"][cvi]["OPTIONS"];
                for (int oi = 0; oi < (int)Opts.size(); oi++)
                {
                    QHBoxLayout * OptRow = new QHBoxLayout();

                    if (!IsRandom)
                    {
                        // options type: show Label + Value
                        QLineEdit * OptLabel = new QLineEdit(OptsBox);
                        OptLabel->setPlaceholderText("Label");
                        OptLabel->setProperty("JSONPath", QString("/CUSTOMVARS/%1/OPTIONS/%2/LABEL").arg(cvi).arg(oi));
                        if (Opts[oi].contains("LABEL") && Opts[oi]["LABEL"].is_string())
                            OptLabel->setText(QString::fromStdString(std::string(Opts[oi]["LABEL"])));
                        QObject::connect(OptLabel, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        OptRow->addWidget(OptLabel);
                    }

                    // Both types: show Value field
                    QLineEdit * OptValue = new QLineEdit(OptsBox);
                    OptValue->setPlaceholderText(IsRandom ? "Key / Value" : "Value");
                    OptValue->setProperty("JSONPath", QString("/CUSTOMVARS/%1/OPTIONS/%2/VALUE").arg(cvi).arg(oi));
                    if (Opts[oi].contains("VALUE") && Opts[oi]["VALUE"].is_string())
                        OptValue->setText(QString::fromStdString(std::string(Opts[oi]["VALUE"])));
                    QObject::connect(OptValue, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);

                    QPushButton * OptDel = new QPushButton("✕", OptsBox);
                    OptDel->setFixedWidth(28);
                    QObject::connect(OptDel, &QPushButton::clicked, this, [this, cvi, oi](){
                        (*MANIFESTJSON)["CUSTOMVARS"][cvi]["OPTIONS"].erase(oi);
                        SaveManifestJSON(); BuildUI();
                    });
                    OptRow->addWidget(OptValue); OptRow->addWidget(OptDel);
                    OptsLayout->addLayout(OptRow);
                }
                QString AddBtnLabel = IsRandom ? "+ Add Value" : "+ Add Option";
                QPushButton * AddOptBtn = new QPushButton(AddBtnLabel, OptsBox);
                QObject::connect(AddOptBtn, &QPushButton::clicked, this, [this, cvi, IsRandom](){
                    if (IsRandom)
                        (*MANIFESTJSON)["CUSTOMVARS"][cvi]["OPTIONS"].push_back(json::object({{"VALUE", ""}}));
                    else
                        (*MANIFESTJSON)["CUSTOMVARS"][cvi]["OPTIONS"].push_back(json::object({{"LABEL", ""}, {"VALUE", ""}}));
                    SaveManifestJSON(); BuildUI();
                });
                OptsLayout->addWidget(AddOptBtn);
                CVCardLayout->addWidget(OptsBox, cvrow, 0, 1, -1);
                cvrow++;
            }

            CVScrollLayout->addWidget(CVCard);
        }

        QPushButton * AddCVBtn = new QPushButton("+ Add CustomVar", CVScrollContents);
        QObject::connect(AddCVBtn, &QPushButton::clicked, this, [this](){
            (*MANIFESTJSON)["CUSTOMVARS"].push_back(json::object({
                {"KEY", ""}, {"LABEL", ""}, {"DEFAULT", ""}, {"VARTYPE", "string"}, {"DISPLAY", true}
            }));
            SaveManifestJSON(); BuildUI();
        });
        CVScrollLayout->addWidget(AddCVBtn);
        CVScrollLayout->addStretch();

        PackageEditorTabWidget->addTab(CVTabWidget, "CUSTOMVARS");
    }

    //INDIVIDUAL COMPONENTS TABS
    for (int i = 0; i < (int)(*PackageEditor::MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        // Resolve COMPONENTID string for this component
        std::string ComponentIDStr;
        auto &CIDField = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["COMPONENTID"];
        if (!CIDField.is_null() && CIDField.is_string())
            ComponentIDStr = std::string(CIDField);

        QWidget * ComponentTabWidget = new QWidget(PackageEditorTabWidget);
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

                QToolButton * AddSubComponentButton = new QToolButton(ComponentTabWidget);
                AddSubComponentButton->setText("+ Subcomponent");
                AddSubComponentButton->setPopupMode(QToolButton::InstantPopup);
                QMenu * AddSubComponentMenu = new QMenu(AddSubComponentButton);
                AddSubComponentMenu->addAction("VFSZipLayer",        this, &PackageEditor::AddVFSZipLayer);
                AddSubComponentMenu->addAction("VFSDirLayer",        this, &PackageEditor::AddVFSDirLayer);
                AddSubComponentMenu->addAction("VFSFileLayer",       this, &PackageEditor::AddVFSFileLayer);
                AddSubComponentMenu->addSeparator();
                AddSubComponentMenu->addSeparator();
                AddSubComponentMenu->addAction("RegEdit",            this, &PackageEditor::AddRegEdit);
                AddSubComponentMenu->addAction("DllOverride",        this, &PackageEditor::AddDllOverride);
                AddSubComponentMenu->addAction("FileEdit",           this, &PackageEditor::AddFileEdit);
                AddSubComponentButton->setMenu(AddSubComponentMenu);
                SubComponentsToolbarLayout->addWidget(AddSubComponentButton);

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

                {
                    QPushButton * RemoveBtn = new QPushButton("✕", IndividualSubComponentGroupBox);
                    RemoveBtn->setFixedWidth(28);
                    QObject::connect(RemoveBtn, &QPushButton::clicked, this, [this, i, j]()
                    {
                        (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"].erase(j);
                        SaveManifestJSON(); BuildUI();
                    });
                    IndividualSubComponentGroupBoxLayout->addWidget(RemoveBtn, 0, 2);
                }

                if (SubComponentType == "VFSZipLayer" || SubComponentType == "VFSDirLayer" || SubComponentType == "VFSFileLayer")
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

                    //TARGET field — for VFSZipLayer and VFSDirLayer only.
                    if (SubComponentType == "VFSZipLayer" || SubComponentType == "VFSDirLayer")
                    {
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("TARGET:"), 2, 0);
                        QLineEdit * TargetField = new QLineEdit(IndividualSubComponentGroupBox);
                        QString TargetJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/TARGET").arg(i).arg(j);
                        TargetField->setProperty("JSONPath", TargetJSONPath);
                        auto &SubRef = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                        if (SubRef.contains("TARGET") && SubRef["TARGET"].is_string())
                            TargetField->setText(QString::fromStdString(std::string(SubRef["TARGET"])));
                        QObject::connect(TargetField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(TargetField, 2, 1);
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
                            std::string OrigKey = Object.key();

                            //Editable key name field.
                            QLineEdit * KeyEdit = new QLineEdit(RegKeysGroupBox);
                            KeyEdit->setText(QString::fromStdString(OrigKey));
                            KeyEdit->setProperty("OriginalKey", QString::fromStdString(OrigKey));
                            QObject::connect(KeyEdit, &QLineEdit::editingFinished, this, [this, i, j, KeyEdit]()
                            {
                                QString OldKey = KeyEdit->property("OriginalKey").toString();
                                QString NewKey = KeyEdit->text();
                                if (OldKey == NewKey || NewKey.isEmpty()) return;
                                std::string OldKeyStd = OldKey.toStdString();
                                std::string NewKeyStd = NewKey.toStdString();
                                auto Val = (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"][OldKeyStd];
                                (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"].erase(OldKeyStd);
                                (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"][NewKeyStd] = Val;
                                KeyEdit->setProperty("OriginalKey", NewKey);
                                SaveManifestJSON(); RefreshJSONView();
                            });
                            RegKeysGroupBoxLayout->addWidget(KeyEdit, k, 0);

                            //Editable value field.
                            QLineEdit * KeyValueField = new QLineEdit(RegKeysGroupBox);
                            QString KeyValueJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/KEYVALUES/%3").arg(i).arg(j).arg(QString::fromStdString(OrigKey));
                            KeyValueField->setProperty("JSONPath", KeyValueJSONPath);
                            if (Object.value().is_string())
                                KeyValueField->setText(QString::fromStdString(std::string(Object.value())));
                            else if (!Object.value().is_null())
                                KeyValueField->setText(QString::fromStdString(Object.value().dump()));
                            QObject::connect(KeyValueField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            RegKeysGroupBoxLayout->addWidget(KeyValueField, k, 1);

                            //Per-key remove button.
                            QPushButton * KeyDelBtn = new QPushButton("✕", RegKeysGroupBox);
                            KeyDelBtn->setFixedWidth(28);
                            QObject::connect(KeyDelBtn, &QPushButton::clicked, this, [this, i, j, OrigKey]()
                            {
                                (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"].erase(OrigKey);
                                SaveManifestJSON(); BuildUI();
                            });
                            RegKeysGroupBoxLayout->addWidget(KeyDelBtn, k, 2);

                            k++;
                        }

                        //Add Key button.
                        QPushButton * AddKeyBtn = new QPushButton("+ Add Key", RegKeysGroupBox);
                        QObject::connect(AddKeyBtn, &QPushButton::clicked, this, [this, i, j]()
                        {
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["KEYVALUES"]["new_key"] = "";
                            SaveManifestJSON(); BuildUI();
                        });
                        RegKeysGroupBoxLayout->addWidget(AddKeyBtn, k, 0, 1, -1);

                    IndividualSubComponentGroupBoxLayout->addWidget(RegKeysGroupBox, 3, 0, 1, -1);
                }
                else if (SubComponentType == "DllOverride")
                {
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("DLLOVERRIDE:"), 1, 0);
                    QLineEdit * DLLField = new QLineEdit(IndividualSubComponentGroupBox);
                    QString DLLPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/DLLOVERRIDE").arg(i).arg(j);
                    DLLField->setProperty("JSONPath", DLLPath);
                    auto &Sub = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    if (Sub.contains("DLLOVERRIDE") && Sub["DLLOVERRIDE"].is_string())
                        DLLField->setText(QString::fromStdString(std::string(Sub["DLLOVERRIDE"])));
                    QObject::connect(DLLField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(DLLField, 1, 1);
                }
                else if (SubComponentType == "FileEdit")
                {
                    auto &FESub = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    std::string FEMode = FESub.value("MODE", std::string("ConfigWrite"));
                    // Overwrite mode has no KEY field — only MODE, FILE, VALUE.
                    QStringList FEFields = (FEMode == "Overwrite")
                        ? QStringList{"MODE", "FILE", "VALUE"}
                        : QStringList{"MODE", "FILE", "KEY", "VALUE"};
                    int ferow = 1;
                    for (const QString &Field : FEFields)
                    {
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(Field + ":"), ferow, 0);
                        QLineEdit * FEField = new QLineEdit(IndividualSubComponentGroupBox);
                        FEField->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/%3").arg(i).arg(j).arg(Field));
                        if (FESub.contains(Field.toStdString()) && FESub[Field.toStdString()].is_string())
                            FEField->setText(QString::fromStdString(std::string(FESub[Field.toStdString()])));
                        QObject::connect(FEField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(FEField, ferow, 1);
                        ferow++;
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
        PackageEditorTabWidget->addTab(ComponentTabWidget, ComponentTabLabel);
    }

    //Restore saved tab positions (clamped to valid range).
    int MainCount = PackageEditorTabWidget->count();
    PackageEditorTabWidget->setCurrentIndex(qBound(0, SavedMainTab, MainCount - 1));
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

    //Compute the ancestor chain for this component so we can match entrypoints by LASTCOMPONENT.
    ContainerParams TmpParams(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper::CreateRecipe(*MANIFESTJSON, TmpParams);
    std::unordered_set<std::string> RecipeSet(TmpParams.Recipe.begin(), TmpParams.Recipe.end());

    //Collect entrypoints from all subgames whose LASTCOMPONENT is in this component's ancestor chain.
    //Store (display label, subgame_id, entrypoint JSON) triples.
    struct EPEntry { QString Label; std::string SubgameID; nlohmann::ordered_json EP; };
    std::vector<EPEntry> EPList;
    if ((*MANIFESTJSON).contains("SUBGAMES"))
        for (auto &SG : (*MANIFESTJSON)["SUBGAMES"])
        {
            std::string SGID = SG.value("SUBGAMEID", std::string());
            if (!SG.contains("ENTRYPOINTS") || !SG["ENTRYPOINTS"].is_array()) continue;
            for (auto &EP : SG["ENTRYPOINTS"])
            {
                std::string LastComp = EP.value("LASTCOMPONENT", std::string());
                if (RecipeSet.count(LastComp) == 0) continue;
                QString EPLabel = QString("(%1)  %2  →  %3")
                    .arg(QString::fromStdString(SGID))
                    .arg(QString::fromStdString(EP.value("ENTRYPOINT_ID", std::string("?"))))
                    .arg(QString::fromStdString(LastComp));
                EPList.push_back({EPLabel, SGID, EP});
            }
        }

    if (EPList.empty())
    {
        QMessageBox::warning(this, "Execute Component", "No entrypoints found whose LASTCOMPONENT is in this component's chain.\nAdd an entrypoint to a subgame first.");
        return;
    }

    //Collect all runners from GlobalConfig and MANIFEST.
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

    //Build the picker dialog.
    QDialog Dialog(this);
    Dialog.setWindowTitle(QString("Execute: %1").arg(QString::fromStdString(ComponentID)));
    Dialog.setMinimumWidth(500);
    QVBoxLayout * DLayout = new QVBoxLayout(&Dialog);
    QFormLayout * Form = new QFormLayout();
    DLayout->addLayout(Form);

    QComboBox * EPPicker = new QComboBox(&Dialog);
    for (auto &EPE : EPList) EPPicker->addItem(EPE.Label);
    Form->addRow("Entrypoint:", EPPicker);

    QComboBox * RunnerPicker = new QComboBox(&Dialog);
    for (auto &[Label, _] : Runners) RunnerPicker->addItem(Label);
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
    int EPIdx     = EPPicker->currentIndex();
    if (RunnerIdx < 0 || RunnerIdx >= (int)Runners.size()) return;
    if (EPIdx     < 0 || EPIdx     >= (int)EPList.size())  return;

    nlohmann::ordered_json SelectedRunner = Runners[RunnerIdx].second;
    EPEntry &SelectedEP = EPList[EPIdx];
    std::string SelectedLastComp = SelectedEP.EP.value("LASTCOMPONENT", std::string());
    std::string SelectedEPID     = SelectedEP.EP.value("ENTRYPOINT_ID", std::string());

    //Build container with the entrypoint's LASTCOMPONENT and its subgame.
    ContainerParams Params(PackageDir->path().toStdString(), SelectedEP.SubgameID, SelectedLastComp);
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

    //Override runner from picker.
    Container.ContainerParams.RunnerName       = SelectedRunner.value("NAME",       std::string());
    Container.ContainerParams.RunnerExecutable = SelectedRunner.value("EXECUTABLE", std::string());
    std::string TypeStr                        = SelectedRunner.value("TYPE",        std::string("wine"));
    if      (TypeStr == "wine")      Container.ContainerParams.RunnerTypeEnum = RunnerType::Wine;
    else if (TypeStr == "emulator")  Container.ContainerParams.RunnerTypeEnum = RunnerType::Emulator;
    else if (TypeStr == "custom")    Container.ContainerParams.RunnerTypeEnum = RunnerType::Custom;
    else                             Container.ContainerParams.RunnerTypeEnum = RunnerType::Native;
    Container.ContainerParams.RunnerEnv       = SelectedRunner.contains("ENV") ? SelectedRunner["ENV"] : nlohmann::ordered_json::object();
    Container.ContainerParams.RunnerRemoveEnv.clear();
    Container.ContainerParams.RunnerArgs.clear();
    if (SelectedRunner.contains("REMOVE_ENV")) for (auto &E : SelectedRunner["REMOVE_ENV"]) Container.ContainerParams.RunnerRemoveEnv.push_back(std::string(E));
    if (SelectedRunner.contains("ARGS"))       for (auto &A : SelectedRunner["ARGS"])       Container.ContainerParams.RunnerArgs.push_back(std::string(A));

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

    Container.ContainerParams.ExecutableID = SelectedEPID;

    if (!ContainerWrapper::ResolveExecutableDefinition(*MANIFESTJSON, Container.ContainerParams))
    {
        QMessageBox::critical(this, "Execute Component", "Failed to resolve entrypoint.\nCheck ENTRYPOINT_ID and LASTCOMPONENT.");
        return;
    }

    Container.Cleanup();

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

//Walks up the sender's parent chain to find the first QWidget with a non-empty JSONPath property.
//When the Add* slots are triggered from a QMenu action, sender() is a QAction whose parent chain is:
//  QAction → QMenu → QToolButton → ComponentTabWidget (has JSONPath).
static QString ResolveComponentJSONPath(QObject * Sender)
{
    //Walk up the parent chain until we find a widget with a non-empty JSONPath property.
    QObject * Obj = Sender;
    while (Obj)
    {
        QVariant V = Obj->property("JSONPath");
        if (V.isValid() && !V.toString().isEmpty())
            return V.toString();
        Obj = Obj->parent();
    }
    return QString();
}

void PackageEditor::AddVFSDirLayer()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    QString Selected = QFileDialog::getExistingDirectory(this, "Select directory to add as VFSDirLayer");
    if (Selected.isEmpty()) return;
    QString DirName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + DirName);
    QDir().rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSDirLayer"}, {"PATH", DirName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddVFSZipLayer()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    QString Selected = QFileDialog::getOpenFileName(this, "Select ZIP file to add as VFSZipLayer", "", "ZIP files (*.zip)");
    if (Selected.isEmpty()) return;
    QString ZipName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + ZipName);
    QFile::rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSZipLayer"}, {"PATH", ZipName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddVFSFileLayer()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    QString Selected = QFileDialog::getOpenFileName(this, "Select file to add as VFSFileLayer");
    if (Selected.isEmpty()) return;
    QString FileName = QFileInfo(Selected).fileName();
    QString Dest = QDir::cleanPath(PackageFilesDir->path() + QDir::separator() + FileName);
    QFile::rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSFileLayer"}, {"PATH", FileName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddEntrypoint()
{
    // AddEntrypoint is wired to the per-subgame "+ Add Entrypoint" button directly,
    // which captures `i` and pushes to SUBGAMES[i].ENTRYPOINTS inline via lambda.
    // This slot is kept for header consistency but should not normally be called.
    LogErr("PackageEditor", "AddEntrypoint() called unexpectedly — use the inline lambda in BuildUI instead.");
}


void PackageEditor::AddRegEdit()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "RegEdit"}, {"REGPATH", ""}, {"ARCHITECTURE", "32"}, {"KEYVALUES", json::object()}
    }));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddDllOverride()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "DllOverride"}, {"DLLOVERRIDE", ""}
    }));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddFileEdit()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "FileEdit"}, {"MODE", "ConfigWrite"}, {"FILE", ""}, {"KEY", ""}, {"VALUE", ""}
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
