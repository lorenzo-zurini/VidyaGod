#include "packageeditor.h"
#include "commonutils.h"
#include "registrywrapper.h"
#include <QPushButton>
#include <iostream>
#include <QBuffer>
#include <QImage>
#include <QInputDialog>
#include <functional>
#include <set>

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
    QPushButton * AddRunnerBtn    = new QPushButton("Add Runner",    this);
    Toolbar->addWidget(AddSubGameBtn);
    Toolbar->addWidget(AddComponentBtn);
    Toolbar->addWidget(AddRunnerBtn);
    Toolbar->addStretch();
    MainLayout->addLayout(Toolbar);

    connect(AddSubGameBtn,   &QPushButton::clicked, this, &PackageEditor::on_AddSubGameButton_clicked);
    connect(AddComponentBtn, &QPushButton::clicked, this, &PackageEditor::on_AddComponentButton_clicked);
    //Add Runner lives on the top toolbar (the RUNNERS tab only appears once runners exist).
    connect(AddRunnerBtn, &QPushButton::clicked, this, [this](){
        QString File = PromptTargetFile("Add Runner");
        if (File.isEmpty()) return;
        if (!(*MANIFESTJSON).contains("RUNNERS") || !(*MANIFESTJSON)["RUNNERS"].is_array())
            (*MANIFESTJSON)["RUNNERS"] = json::array();
        (*MANIFESTJSON)["RUNNERS"].push_back(json::object({
            {"RUNNER_ID", "runner"}, {"NAME", ""},
            {"VARIANTS", json::array({ json::object({
                {"VARIANT_ID", "default"}, {"HOST_PLATFORM", "linux64"}, {"GUEST_PLATFORM", json::array()},
                {"TYPE", "custom"}, {"EXECUTABLE", ""}, {"ARGS", json::array()}, {"ENV", json::object()},
                {"MODULES", json::array()}, {"RECOMMENDED", true}
            }) })},
            {"__FILE__", File.toStdString()}
        }));
        SaveManifestJSON(); BuildUI();
    });

    // Tab widget (populated by BuildUI)
    PackageEditorTabWidget = new QTabWidget(this);
    MainLayout->addWidget(PackageEditorTabWidget, 1);

    // Validation panel — a persistent box docked beneath all tabs, refreshed by UpdateValidationBox().
    ValidationBox = new QGroupBox("Validation", this);
    QVBoxLayout * ValBoxLayout = new QVBoxLayout(ValidationBox);
    ValBoxLayout->setContentsMargins(6, 2, 6, 6);
    ValidationView = new QTextEdit(ValidationBox);
    ValidationView->setReadOnly(true);
    ValidationView->setMaximumHeight(150);
    ValBoxLayout->addWidget(ValidationView);
    MainLayout->addWidget(ValidationBox, 0);

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
        //Seed a brand-new package with empty identity fields. The manifest validator flags these
        //as missing until the author fills them in (no dependency on GlobalConfig schema).
        (*PackageEditor::MANIFESTJSON) = json::object({
            {"PACKAGENAME", ""}, {"PACKAGEUID", ""}, {"PACKAGEVERSION", ""}
        });
    }
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    QString File = PromptTargetFile("Add Subgame");
    if (File.isEmpty()) return;
    json NewSubGameObject;
    NewSubGameObject["GAMEID"] = nullptr;
    NewSubGameObject["__FILE__"]  = File.toStdString();
    (*MANIFESTJSON)[json::json_pointer("/GAMES")].push_back(NewSubGameObject);
    SaveManifestJSON();
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    QString File = PromptTargetFile("Add Component");
    if (File.isEmpty()) return;
    (*MANIFESTJSON)[json::json_pointer("/COMPONENTS")].push_back(json::object({{"COMPONENTID", ""}, {"NAME", ""}, {"SUBCOMPONENTS", json::array()}, {"__FILE__", File.toStdString()}}));
    SaveManifestJSON();
    BuildUI();
    RefreshJSONView();
}


//Decomposes the tagged assembled MANIFESTJSON back into one document per fragment file.
//Routing: each subgame/component/customvar/runner and each variant carries a "__FILE__" tag.
//Top-level identity (PACKAGE* + HOST_PLATFORM) goes to IdentityFile. (Persistence is no longer a
//top-level object — it lives in PersistDir/PersistFile/RegPersist subcomponents.)
std::map<QString, nlohmann::ordered_json> PackageEditor::DecomposeByFile()
{
    auto Strip = [](nlohmann::ordered_json E) { E.erase("__FILE__"); return E; };
    auto FileOf = [](const nlohmann::ordered_json &E, const QString &Fallback) {
        return (E.contains("__FILE__") && E["__FILE__"].is_string())
            ? QString::fromStdString(std::string(E["__FILE__"])) : Fallback;
    };

    std::map<QString, nlohmann::ordered_json> Docs;
    // Ensure every known file exists in the map (so emptied files still get written).
    for (const QString &F : FragmentFiles) Docs[F] = nlohmann::ordered_json::object();
    QString Ident = IdentityFile.isEmpty() ? (FragmentFiles.isEmpty() ? QString("MANIFEST.json") : FragmentFiles.first()) : IdentityFile;
    Docs[Ident]; // ensure present

    auto &M = *MANIFESTJSON;

    // Identity (incl. HOST_PLATFORM) → identity file.
    for (const char *Key : {"PACKAGENAME", "PACKAGEUID", "PACKAGEVERSION", "HOST_PLATFORM"})
        if (M.contains(Key)) Docs[Ident][Key] = M[Key];

    // RUNNERS (flat array of runner objects, each tagged).
    if (M.contains("RUNNERS") && M["RUNNERS"].is_array())
        for (const auto &R : M["RUNNERS"])
            Docs[FileOf(R, Ident)]["RUNNERS"].push_back(Strip(R));

    // COMPONENTS.
    if (M.contains("COMPONENTS") && M["COMPONENTS"].is_array())
        for (const auto &C : M["COMPONENTS"])
            Docs[FileOf(C, Ident)]["COMPONENTS"].push_back(Strip(C));

    // SUBGAMES — owner file gets scalars+metadata; each variant routes to its own file.
    if (M.contains("GAMES") && M["GAMES"].is_array())
        for (const auto &SG : M["GAMES"])
        {
            QString OwnerFile = FileOf(SG, Ident);
            std::string SGID = SG.value("GAMEID", std::string());
            // group this subgame's variants by their file
            std::map<QString, nlohmann::ordered_json> VarsByFile;
            if (SG.contains("VARIANTS") && SG["VARIANTS"].is_array())
                for (const auto &V : SG["VARIANTS"])
                    VarsByFile[FileOf(V, OwnerFile)].push_back(Strip(V));

            // Owner file: full subgame (scalars + metadata) with its own variants.
            nlohmann::ordered_json Owner = SG;
            Owner.erase("__FILE__");
            Owner["VARIANTS"] = VarsByFile.count(OwnerFile) ? VarsByFile[OwnerFile] : nlohmann::ordered_json::array();
            Docs[OwnerFile]["GAMES"].push_back(Owner);

            // Other files: a stub {SUBGAMEID, VARIANTS:[…]} for variants they own.
            for (auto &[F, Vars] : VarsByFile)
            {
                if (F == OwnerFile) continue;
                nlohmann::ordered_json Stub = nlohmann::ordered_json::object();
                Stub["GAMEID"] = SGID;
                Stub["VARIANTS"] = Vars;
                Docs[F]["GAMES"].push_back(Stub);
            }
        }

    return Docs;
}

bool PackageEditor::SaveManifestJSON()
{
    bool Ok = true;
    auto Docs = DecomposeByFile();
    for (auto &[FileName, Doc] : Docs)
    {
        QFile F(PackageDir->filePath(FileName));
        if (!JSONOps::SaveJSON(&Doc, &F)) Ok = false;
    }
    //Notify open game cards / prelaunch dialogs to reload and re-render from the freshly-written files.
    if (PackageDir) emit packageSaved(PackageDir->path());
    return Ok;
}

//Builds a small "File: [dropdown]" row bound to the element's "__FILE__" provenance tag.
QWidget * PackageEditor::MakeFileTagWidget(const std::string &ElementPointer)
{
    QWidget * W = new QWidget();
    QHBoxLayout * L = new QHBoxLayout(W);
    L->setContentsMargins(0, 0, 0, 0);
    QLabel * Lbl = new QLabel("File:", W);
    QComboBox * Combo = new QComboBox(W);
    Combo->addItems(FragmentFiles);
    std::string Cur;
    try {
        auto &El = MANIFESTJSON->at(nlohmann::ordered_json::json_pointer(ElementPointer));
        if (El.contains("__FILE__") && El["__FILE__"].is_string()) Cur = std::string(El["__FILE__"]);
    } catch (...) {}
    { int i = Combo->findText(QString::fromStdString(Cur)); if (i >= 0) { QSignalBlocker B(Combo); Combo->setCurrentIndex(i); } }
    std::string PtrCopy = ElementPointer;
    QObject::connect(Combo, &QComboBox::currentIndexChanged, this, [this, Combo, PtrCopy](){
        (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(PtrCopy + "/__FILE__")] = Combo->currentText().toStdString();
        SaveManifestJSON(); BuildUI();
    });
    L->addWidget(Lbl);
    L->addWidget(Combo, 1);
    W->setLayout(L);
    return W;
}

//Re-runs validation on a tag-stripped copy of the assembled manifest.
void PackageEditor::RevalidateManifest()
{
    ValErrors.clear(); ValWarnings.clear();
    nlohmann::ordered_json Clean = *MANIFESTJSON;
    // Strip __FILE__ tags before validating (cosmetic; ValidateManifest ignores them anyway).
    std::function<void(nlohmann::ordered_json&)> StripAll = [&](nlohmann::ordered_json &J){
        if (J.is_object()) { J.erase("__FILE__"); for (auto &E : J) StripAll(E); }
        else if (J.is_array()) for (auto &E : J) StripAll(E);
    };
    StripAll(Clean);
    JSONOps::ValidateManifest(Clean, ValErrors, ValWarnings);
}

//Refreshes the persistent validation box beneath the tabs. Title summarises counts; body lists each
//error (red) then warning (amber). Green tick when the manifest is clean.
void PackageEditor::UpdateValidationBox()
{
    if (!ValidationView || !ValidationBox) return;

    if (ValErrors.empty() && ValWarnings.empty())
        ValidationBox->setTitle("Validation  —  ✓ OK");
    else if (ValErrors.empty())
        ValidationBox->setTitle(QString("Validation  —  %1 warning(s)").arg(ValWarnings.size()));
    else
        ValidationBox->setTitle(QString("⚠ Validation  —  %1 error(s), %2 warning(s)")
                                    .arg(ValErrors.size()).arg(ValWarnings.size()));

    QString Html;
    if (ValErrors.empty() && ValWarnings.empty())
        Html = "<span style='color:#3fae5a'>✓ No problems found.</span>";
    else
    {
        for (const auto &E : ValErrors)
            Html += "<div style='color:#d9534f'><b>ERROR:</b> " + QString::fromStdString(E).toHtmlEscaped() + "</div>";
        for (const auto &W : ValWarnings)
            Html += "<div style='color:#c9a227'>warning: " + QString::fromStdString(W).toHtmlEscaped() + "</div>";
    }
    ValidationView->setHtml(Html);
    //Errors block launch — make the box visually assertive when present.
    ValidationBox->setStyleSheet(ValErrors.empty() ? QString()
                                                   : "QGroupBox{border:1px solid #d9534f;border-radius:4px;margin-top:6px;}"
                                                     "QGroupBox::title{subcontrol-origin:margin;left:8px;color:#d9534f;}");
}

//Prompts for which fragment file a new element should go in. Auto-returns the sole file if there
//is only one; otherwise shows a dropdown of existing files plus a "+ New file…" option.
QString PackageEditor::PromptTargetFile(const QString &Title)
{
    if (FragmentFiles.size() <= 1)
        return FragmentFiles.isEmpty() ? QString("MANIFEST.json") : FragmentFiles.first();

    QDialog D(this);
    D.setWindowTitle(Title);
    QVBoxLayout *L = new QVBoxLayout(&D);
    L->addWidget(new QLabel("Which file should this go in?", &D));
    QComboBox *Combo = new QComboBox(&D);
    for (const QString &F : FragmentFiles) Combo->addItem(F);
    Combo->addItem("+ New file…");
    L->addWidget(Combo);
    QHBoxLayout *BR = new QHBoxLayout(); L->addLayout(BR); BR->addStretch();
    QPushButton *Ok = new QPushButton("OK", &D); Ok->setDefault(true);
    QPushButton *Ca = new QPushButton("Cancel", &D);
    BR->addWidget(Ca); BR->addWidget(Ok);
    QObject::connect(Ok, &QPushButton::clicked, &D, &QDialog::accept);
    QObject::connect(Ca, &QPushButton::clicked, &D, &QDialog::reject);
    if (D.exec() != QDialog::Accepted) return QString();

    if (Combo->currentIndex() == Combo->count() - 1)
    {
        QString Name = QInputDialog::getText(this, "New file", "Filename (e.g. mymod.json):").trimmed();
        if (Name.isEmpty()) return QString();
        if (!Name.endsWith(".json")) Name += ".json";
        if (!FragmentFiles.contains(Name)) FragmentFiles.append(Name);
        return Name;
    }
    return Combo->currentText();
}

void PackageEditor::RefreshJSONView()
{
    if (!JSONTextEdit) return;
    QString File = (JSONFileCombo && JSONFileCombo->currentIndex() >= 0) ? JSONFileCombo->currentText() : IdentityFile;
    auto Docs = DecomposeByFile();
    QSignalBlocker B(JSONTextEdit); // don't trigger the validation handler on programmatic refresh
    JSONTextEdit->setText(Docs.count(File) ? QString::fromStdString(Docs[File].dump(4)) : QString("{}"));
}

void PackageEditor::InitPackage(const QString &PreselectedPath)
{
    QString ChosenPath = PreselectedPath.isEmpty()
        ? QFileDialog::getExistingDirectory(this, "Select package directory...")
        : PreselectedPath;
    this->PackageDir = new QDir(ChosenPath);

    // Modular manifests: assemble every *.json in the dir into the tagged MANIFESTJSON.
    PackageEditor::MANIFESTJSON = new nlohmann::ordered_json;
    LoadFragmentsAndAssemble();
}

//Loads every *.json fragment, tags each routable element with its source file, merges into the
//assembled MANIFESTJSON, and records FragmentFiles + IdentityFile. See header.
void PackageEditor::LoadFragmentsAndAssemble()
{
    FragmentFiles.clear();
    IdentityFile.clear();
    *MANIFESTJSON = nlohmann::ordered_json::object();

    QStringList Files = PackageDir->entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    std::vector<std::string> Warnings;

    auto TagArray = [](nlohmann::ordered_json &Arr, const QString &File) {
        if (!Arr.is_array()) return;
        for (auto &E : Arr) if (E.is_object()) E["__FILE__"] = File.toStdString();
    };

    for (const QString &FileName : Files)
    {
        nlohmann::ordered_json Frag;
        QFile F(PackageDir->filePath(FileName));
        if (JSONOps::LoadJSON(&F, &Frag) || !Frag.is_object()) continue;
        FragmentFiles.append(FileName);

        // Tag routable elements with their origin file.
        if (Frag.contains("GAMES"))
        {
            TagArray(Frag["GAMES"], FileName);
            for (auto &SG : Frag["GAMES"]) if (SG.is_object() && SG.contains("VARIANTS")) TagArray(SG["VARIANTS"], FileName);
        }
        if (Frag.contains("COMPONENTS")) TagArray(Frag["COMPONENTS"], FileName);
        if (Frag.contains("RUNNERS")) TagArray(Frag["RUNNERS"], FileName); // flat top-level array

        if (IdentityFile.isEmpty() && Frag.contains("PACKAGEUID") && !Frag["PACKAGEUID"].is_null())
            IdentityFile = FileName;

        JSONOps::MergeManifestInto(*MANIFESTJSON, Frag, FileName.toStdString(), Warnings);
    }

    // New/empty package: seed a single MANIFEST.json fragment with placeholder identity.
    if (FragmentFiles.isEmpty())
    {
        FragmentFiles.append("MANIFEST.json");
        IdentityFile = "MANIFEST.json";
        InitMANIFESTJSON();
    }
    if (IdentityFile.isEmpty()) IdentityFile = FragmentFiles.first();

    // Reuse AssembleManifest's identity-conflict detector and carry its errors onto MANIFESTJSON
    // so RevalidateManifest/the Validation tab surface them (zero logic duplication).
    {
        nlohmann::ordered_json Tmp; std::vector<std::string> TmpWarn;
        JSONOps::AssembleManifest(PackageDir->path(), Tmp, TmpWarn);
        if (Tmp.contains("__VG_ERRORS__")) (*MANIFESTJSON)["__VG_ERRORS__"] = Tmp["__VG_ERRORS__"];
        else MANIFESTJSON->erase("__VG_ERRORS__");
    }

    for (const auto &W : Warnings) LogWarn("PackageEditor", "Assemble: " + W);
    RevalidateManifest();
    LogSucc("PackageEditor", "Assembled " + std::to_string(FragmentFiles.size()) + " fragment(s).");
}

//Reusable MODULES editor for any variant (game or runner), addressed by a JSON pointer to the variant.
//Each module = { COMPONENT, REQUIRED (default true), DEFAULT (default true), LABEL }. Lets a runner attach
//components (e.g. its build) exactly like a game variant.
QWidget * PackageEditor::BuildModulesEditor(const std::string &VariantPtr)
{
    using json = nlohmann::ordered_json;
    const json::json_pointer ModPtr(VariantPtr + "/MODULES");
    if (!(*MANIFESTJSON).contains(ModPtr) || !(*MANIFESTJSON).at(ModPtr).is_array())
        (*MANIFESTJSON)[ModPtr] = json::array();
    auto &ModArr = (*MANIFESTJSON).at(ModPtr);

    QGroupBox * ModBox = new QGroupBox("MODULES (components; load order — later overrides earlier)");
    QVBoxLayout * ModLayout = new QVBoxLayout(ModBox); ModBox->setLayout(ModLayout);

    for (int ek = 0; ek < (int)ModArr.size(); ek++)
    {
        const auto &M = ModArr[ek];
        const std::string CurComp  = M.is_object() ? M.value("COMPONENT", std::string()) : std::string();
        const bool        CurReq   = M.is_object() ? M.value("REQUIRED", true) : true;
        const bool        CurDef   = M.is_object() ? M.value("DEFAULT", true)  : true;
        const std::string CurLabel = M.is_object() ? M.value("LABEL", std::string()) : std::string();
        const std::string MPtr = VariantPtr + "/MODULES/" + std::to_string(ek);

        QHBoxLayout * Row = new QHBoxLayout();
        QComboBox * CompPicker = new QComboBox(ModBox);
        CompPicker->addItem("(none)", QString());
        if ((*MANIFESTJSON).contains("COMPONENTS"))
            for (auto &Comp : (*MANIFESTJSON)["COMPONENTS"])
            {
                std::string CID = Comp.value("COMPONENTID", std::string()), CName = Comp.value("NAME", std::string());
                CompPicker->addItem(CName.empty() ? QString::fromStdString(CID) : QString::fromStdString(CName + " (" + CID + ")"),
                                    QString::fromStdString(CID));
            }
        { int idx = CompPicker->findData(QString::fromStdString(CurComp)); if (idx >= 0) { QSignalBlocker B(CompPicker); CompPicker->setCurrentIndex(idx); } }
        QObject::connect(CompPicker, &QComboBox::currentIndexChanged, this, [this, MPtr, CompPicker](){
            (*MANIFESTJSON)[json::json_pointer(MPtr + "/COMPONENT")] = CompPicker->currentData().toString().toStdString();
            SaveManifestJSON(); RefreshJSONView();
        });
        QCheckBox * ReqChk = new QCheckBox("Required", ModBox); ReqChk->setChecked(CurReq);
        QObject::connect(ReqChk, &QCheckBox::toggled, this, [this, MPtr](bool On){
            (*MANIFESTJSON)[json::json_pointer(MPtr + "/REQUIRED")] = On; SaveManifestJSON(); RefreshJSONView();
        });
        QCheckBox * DefChk = new QCheckBox("Default", ModBox); DefChk->setChecked(CurDef);
        QObject::connect(DefChk, &QCheckBox::toggled, this, [this, MPtr](bool On){
            (*MANIFESTJSON)[json::json_pointer(MPtr + "/DEFAULT")] = On; SaveManifestJSON(); RefreshJSONView();
        });
        QLineEdit * LabelField = new QLineEdit(QString::fromStdString(CurLabel), ModBox);
        LabelField->setPlaceholderText("Label (optional)");
        QObject::connect(LabelField, &QLineEdit::editingFinished, this, [this, MPtr, LabelField](){
            (*MANIFESTJSON)[json::json_pointer(MPtr + "/LABEL")] = LabelField->text().toStdString(); SaveManifestJSON(); RefreshJSONView();
        });
        QPushButton * UpBtn = new QPushButton("▲", ModBox); UpBtn->setFixedWidth(28);
        QPushButton * DnBtn = new QPushButton("▼", ModBox); DnBtn->setFixedWidth(28);
        QPushButton * DelBtn = new QPushButton("✕", ModBox); DelBtn->setFixedWidth(28);
        const std::string ArrPtr = VariantPtr + "/MODULES";
        QObject::connect(UpBtn, &QPushButton::clicked, this, [this, ArrPtr, ek](){
            auto &A = (*MANIFESTJSON)[json::json_pointer(ArrPtr)]; if (ek <= 0) return;
            std::swap(A[ek - 1], A[ek]); SaveManifestJSON(); BuildUI();
        });
        QObject::connect(DnBtn, &QPushButton::clicked, this, [this, ArrPtr, ek](){
            auto &A = (*MANIFESTJSON)[json::json_pointer(ArrPtr)]; if (ek + 1 >= (int)A.size()) return;
            std::swap(A[ek], A[ek + 1]); SaveManifestJSON(); BuildUI();
        });
        QObject::connect(DelBtn, &QPushButton::clicked, this, [this, ArrPtr, ek](){
            (*MANIFESTJSON)[json::json_pointer(ArrPtr)].erase(ek); SaveManifestJSON(); BuildUI();
        });
        Row->addWidget(CompPicker, 1); Row->addWidget(ReqChk); Row->addWidget(DefChk);
        Row->addWidget(LabelField, 1); Row->addWidget(UpBtn); Row->addWidget(DnBtn); Row->addWidget(DelBtn);
        ModLayout->addLayout(Row);
    }
    QPushButton * AddModBtn = new QPushButton("+ Add Component", ModBox);
    const std::string ArrPtr = VariantPtr + "/MODULES";
    QObject::connect(AddModBtn, &QPushButton::clicked, this, [this, ArrPtr](){
        (*MANIFESTJSON)[json::json_pointer(ArrPtr)].push_back(nlohmann::ordered_json{{"COMPONENT", ""}});
        SaveManifestJSON(); BuildUI();
    });
    ModLayout->addWidget(AddModBtn);
    return ModBox;
}

bool PackageEditor::BuildUI()
{
    //Keep validation current for the docked validation box (UpdateValidationBox, called below).
    RevalidateManifest();

    //Save current tab positions so we can restore them after the UI is rebuilt.
    SavedMainTab    = PackageEditorTabWidget->currentIndex();
    SavedSubgameTab = SubGamesTabWidget ? SubGamesTabWidget->currentIndex() : 0;

    //JSON TAB
    PackageEditorTabWidget->clear();
    SubGamesTabWidget = nullptr; // Reset stale pointer — widgets are orphaned but alive until parent dies

    PackageEditor::JSONTabWidget = new QWidget(PackageEditorTabWidget);
    QVBoxLayout * JSONTabWidgetLayout = new QVBoxLayout(JSONTabWidget);
    JSONTabWidget->setLayout(JSONTabWidgetLayout);

        // Modular manifests: the raw JSON tab edits ONE fragment file at a time.
        QHBoxLayout * JSONFileRow = new QHBoxLayout();
        JSONFileRow->addWidget(new QLabel("File:", JSONTabWidget));
        JSONFileCombo = new QComboBox(JSONTabWidget);
        JSONFileCombo->addItems(FragmentFiles);
        { int ii = JSONFileCombo->findText(IdentityFile); if (ii >= 0) JSONFileCombo->setCurrentIndex(ii); }
        JSONFileRow->addWidget(JSONFileCombo, 1);
        QPushButton * NewFileBtn = new QPushButton("+ New File", JSONTabWidget);
        QPushButton * DelFileBtn = new QPushButton("Delete File", JSONTabWidget);
        JSONFileRow->addWidget(NewFileBtn);
        JSONFileRow->addWidget(DelFileBtn);
        JSONTabWidgetLayout->addLayout(JSONFileRow);

        PackageEditor::JSONTextEdit = new QTextEdit(JSONTabWidget);
        JSONTabWidgetLayout->addWidget(JSONTextEdit);
        QObject::connect(JSONTextEdit, &QTextEdit::textChanged, this, &PackageEditor::JSONQTextEditChanged);

        PackageEditor::SaveJSONButton = new QPushButton(JSONTabWidget);
        SaveJSONButton->setText("Save File");
        JSONTabWidgetLayout->addWidget(SaveJSONButton);
        QObject::connect(SaveJSONButton, &QPushButton::clicked, this, &PackageEditor::SaveJSONButtonPressed);

        // Switching the file re-dumps that fragment's decomposed content.
        QObject::connect(JSONFileCombo, &QComboBox::currentIndexChanged, this, [this](){ RefreshJSONView(); });

        // New file: create an empty fragment and switch to it.
        QObject::connect(NewFileBtn, &QPushButton::clicked, this, [this](){
            QString Name = QInputDialog::getText(this, "New file", "Filename (e.g. mymod.json):").trimmed();
            if (Name.isEmpty()) return;
            if (!Name.endsWith(".json")) Name += ".json";
            if (FragmentFiles.contains(Name)) return;
            FragmentFiles.append(Name);
            QFile F(PackageDir->filePath(Name));
            nlohmann::ordered_json Empty = nlohmann::ordered_json::object();
            JSONOps::SaveJSON(&Empty, &F);
            BuildUI();
        });
        // Delete file: only if it owns no elements (would orphan data otherwise).
        QObject::connect(DelFileBtn, &QPushButton::clicked, this, [this](){
            QString File = JSONFileCombo->currentText();
            if (File.isEmpty()) return;
            auto Docs = DecomposeByFile();
            if (Docs.count(File) && !Docs[File].empty())
            { QMessageBox::warning(this, "Delete File", "This file still owns manifest elements. Move or delete them first."); return; }
            if (File == IdentityFile) { QMessageBox::warning(this, "Delete File", "Cannot delete the identity file."); return; }
            PackageDir->remove(File);
            FragmentFiles.removeAll(File);
            BuildUI();
        });

        RefreshJSONView();

    PackageEditorTabWidget->addTab(JSONTabWidget, "JSON");

    //Validation now lives in a persistent box below the tabs (see UpdateValidationBox), refreshed here.
    UpdateValidationBox();

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
            //This section edits only top-level SCALAR fields (identity). Structured keys
            //(GAMES/COMPONENTS/RUNNERS) have their own tabs, and internal markers
            //(e.g. __VG_ERRORS__) are hidden — skip all of them.
            if (Item.value().is_structured()) continue;
            if (Item.key().rfind("__", 0) == 0) continue;

            LogOut("PackageEditor", "Adding parameter editor: " + Item.key());
            QLineEdit * NewParamField = new QLineEdit(PackageDataGroupBox);
            QString JSONPath = QString::fromStdString(Item.key()).prepend("/");
            nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
            NewParamField->setProperty("JSONPath", JSONPath);

            const auto &PVal = (*PackageEditor::MANIFESTJSON)[JSONPointer];
            if (!PVal.is_null())
            {
                NewParamField->setText(PVal.is_string() ? QString::fromStdString(std::string(PVal))
                                                        : QString::fromStdString(PVal.dump()));
            }

            QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
            PackageDataGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
        }

        //HOST_PLATFORM is no longer a package-level field — it lives on each game/runner VARIANT now
        //(edited in the variant cards under the GAMES/RUNNERS tabs).
        LogOut("PackageEditor", "Manifest tab done!");

        //GAMES TABS WIDGET
        SubGamesTabWidget = new QTabWidget(ManifestTabWidget); // assign to member for position save/restore
        ManifestTabWidgetLayout->addWidget(SubGamesTabWidget);

        //TITLE and GAMEUID stay flat — they are the game's primary identifiers. Platform is no longer
        //per-game; it is package identity (HOST_PLATFORM, edited above). Everything else nests under METADATA.
        static const std::vector<std::string> IdentityFields = {
            "GAMEID", "TITLE", "GAMEUID"
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
            (void)CurrentPlatform; //platform is package-level (HOST_PLATFORM) now; kept for call-site compatibility
            for (const std::string &FieldKey : Fields)
            {
                QString JSONPath = SubPath.empty()
                    ? QString("/GAMES/%1/%2").arg(i).arg(QString::fromStdString(FieldKey))
                    : QString("/GAMES/%1/%2/%3").arg(i).arg(QString::fromStdString(SubPath)).arg(QString::fromStdString(FieldKey));

                //Resolve the JSON object to read current values from.
                auto &ValSource = (SubPath.empty() || !SubgameRef.contains(SubPath))
                                  ? SubgameRef
                                  : SubgameRef[SubPath];

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

        for (int i = 0; i < (int)(*PackageEditor::MANIFESTJSON)["GAMES"].size(); i++)
        {
            auto &SubgameRef = (*PackageEditor::MANIFESTJSON)["GAMES"][i];

            std::string SubgameIDStr;
            if (SubgameRef.contains("GAMEID") && !SubgameRef["GAMEID"].is_null() && SubgameRef["GAMEID"].is_string())
                SubgameIDStr = std::string(SubgameRef["GAMEID"]);

            std::string CurrentPlatform;
            if (SubgameRef.contains("PLATFORM") && !SubgameRef["PLATFORM"].is_null() && SubgameRef["PLATFORM"].is_string())
                CurrentPlatform = std::string(SubgameRef["PLATFORM"]);

            QWidget * SubGameTabWidget = new QWidget(SubGamesTabWidget);
            SubGameTabWidget->setProperty("JSONPath", QString("/GAMES/%1").arg(i));
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

            //Which fragment file this subgame's scalar fields/metadata live in (per-element routing).
            SubGameScrollLayout->addWidget(MakeFileTagWidget("/GAMES/" + std::to_string(i)));

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
                    QPixmap Pix(QDir::cleanPath(PackageDir->path() + "/" + QString::fromStdString(CoverFile)));
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


            // VARIANTS group — one card per variant in SUBGAMES[i].VARIANTS
            {
                QGroupBox * EPBox = new QGroupBox("Variants", SubGameScrollContents);
                QVBoxLayout * EPBoxLayout = new QVBoxLayout(EPBox);
                EPBox->setLayout(EPBoxLayout);

                if (!SubgameRef.contains("VARIANTS") || !SubgameRef["VARIANTS"].is_array())
                    (*MANIFESTJSON)["GAMES"][i]["VARIANTS"] = nlohmann::ordered_json::array();

                auto &VariantArr = (*MANIFESTJSON)["GAMES"][i]["VARIANTS"];
                for (int epj = 0; epj < (int)VariantArr.size(); epj++)
                {
                    auto &EP = VariantArr[epj];
                    std::string CurEPID = EP.value("VARIANT_ID", std::string());

                    QString EPBoxTitle = CurEPID.empty()
                        ? QString("Variant %1").arg(epj + 1)
                        : QString::fromStdString(CurEPID);
                    QGroupBox * EPCard = new QGroupBox(EPBoxTitle, EPBox);
                    QGridLayout * EPCardLayout = new QGridLayout(EPCard);
                    EPCard->setLayout(EPCardLayout);
                    int eprow = 0;

                    // VARIANT_ID
                    EPCardLayout->addWidget(new QLabel("VARIANT_ID:"), eprow, 0);
                    QLineEdit * EPIDField = new QLineEdit(EPCard);
                    EPIDField->setProperty("JSONPath", QString("/GAMES/%1/VARIANTS/%2/VARIANT_ID").arg(i).arg(epj));
                    EPIDField->setText(QString::fromStdString(CurEPID));
                    QObject::connect(EPIDField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    EPCardLayout->addWidget(EPIDField, eprow, 1);
                    QPushButton * EPRemBtn = new QPushButton("✕", EPCard);
                    EPRemBtn->setFixedWidth(28);
                    QObject::connect(EPRemBtn, &QPushButton::clicked, this, [this, i, epj](){
                        (*MANIFESTJSON)["GAMES"][i]["VARIANTS"].erase(epj);
                        SaveManifestJSON(); BuildUI();
                    });
                    EPCardLayout->addWidget(EPRemBtn, eprow, 2);
                    eprow++;

                    // Which fragment file this variant lives in (per-element routing).
                    EPCardLayout->addWidget(MakeFileTagWidget("/GAMES/" + std::to_string(i) + "/VARIANTS/" + std::to_string(epj)), eprow, 0, 1, 3);
                    eprow++;

                    // RECOMMENDED checkbox — marks this as the default variant (mutually exclusive).
                    {
                        bool IsRec = EP.value("RECOMMENDED", false);
                        QCheckBox * RecCheck = new QCheckBox("Recommended (default)", EPCard);
                        RecCheck->setChecked(IsRec);
                        QObject::connect(RecCheck, &QCheckBox::toggled, this, [this, i, epj, RecCheck](){
                            if (RecCheck->isChecked())
                            {
                                // Clear RECOMMENDED on all other variants in this subgame.
                                auto &EPArr = (*MANIFESTJSON)["GAMES"][i]["VARIANTS"];
                                for (int k = 0; k < (int)EPArr.size(); k++)
                                    EPArr[k]["RECOMMENDED"] = (k == epj);
                            }
                            else
                            {
                                (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["RECOMMENDED"] = false;
                            }
                            SaveManifestJSON(); RefreshJSONView();
                        });
                        EPCardLayout->addWidget(RecCheck, eprow, 0, 1, 3);
                        eprow++;
                    }

                    // HOST_PLATFORM — per-variant (the platform this variant targets, e.g. "win32"/"linux64").
                    {
                        EPCardLayout->addWidget(new QLabel("HOST_PLATFORM:"), eprow, 0);
                        QLineEdit * HP = new QLineEdit(EPCard);
                        HP->setProperty("JSONPath", QString("/GAMES/%1/VARIANTS/%2/HOST_PLATFORM").arg(i).arg(epj));
                        HP->setPlaceholderText("e.g. win32, win64, linux64, snes");
                        if (EP.contains("HOST_PLATFORM") && EP["HOST_PLATFORM"].is_string())
                            HP->setText(QString::fromStdString(std::string(EP["HOST_PLATFORM"])));
                        QObject::connect(HP, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        EPCardLayout->addWidget(HP, eprow, 1, 1, 2);
                        eprow++;
                    }

                    // MODULES — toggleable component references (reusable editor, shared with runner variants).
                    EPCardLayout->addWidget(BuildModulesEditor("/GAMES/" + std::to_string(i) + "/VARIANTS/" + std::to_string(epj)), eprow, 0, 1, -1);
                    eprow++;

                    // EXEPATH, EXEARGS, WORKDIR
                    for (const auto &FieldKey : std::vector<std::string>{"EXEPATH", "EXEARGS", "WORKDIR"})
                    {
                        EPCardLayout->addWidget(new QLabel(QString::fromStdString(FieldKey) + ":"), eprow, 0);
                        QLineEdit * FE = new QLineEdit(EPCard);
                        FE->setProperty("JSONPath", QString("/GAMES/%1/VARIANTS/%2/%3").arg(i).arg(epj).arg(QString::fromStdString(FieldKey)));
                        if (EP.contains(FieldKey) && EP[FieldKey].is_string())
                            FE->setText(QString::fromStdString(std::string(EP[FieldKey])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        EPCardLayout->addWidget(FE, eprow, 1, 1, 2);
                        eprow++;
                    }

                    // RUNNER_ID pin (optional) — forces a specific runner; blank = platform default.
                    {
                        EPCardLayout->addWidget(new QLabel("RUNNER_ID (pin):"), eprow, 0);
                        QComboBox * RPin = new QComboBox(EPCard);
                        RPin->addItem("(platform default)", QString());
                        // Offer this package's own runners (embedded bundle) and every GLOBAL_RUNNERS registry runner.
                        auto AddPin = [&](const nlohmann::ordered_json &R){
                            QString Rid = QString::fromStdString(R.value("RUNNER_ID", std::string()));
                            if (!Rid.isEmpty() && RPin->findData(Rid) < 0) RPin->addItem(Rid, Rid);
                        };
                        if ((*MANIFESTJSON).contains("RUNNERS") && (*MANIFESTJSON)["RUNNERS"].is_array())
                            for (auto &R : (*MANIFESTJSON)["RUNNERS"]) AddPin(R);
                        for (auto &R : ContainerWrapper::RegistryRunners(*GlobalConfigJSON)) AddPin(R);
                        { int idx = RPin->findData(QString::fromStdString(EP.value("RUNNER_ID", std::string()))); if (idx >= 0) { QSignalBlocker B(RPin); RPin->setCurrentIndex(idx); } }
                        QObject::connect(RPin, &QComboBox::currentIndexChanged, this, [this, i, epj, RPin](){
                            QString Rid = RPin->currentData().toString();
                            if (Rid.isEmpty()) (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj].erase("RUNNER_ID");
                            else (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["RUNNER_ID"] = Rid.toStdString();
                            SaveManifestJSON(); RefreshJSONView();
                        });
                        EPCardLayout->addWidget(RPin, eprow, 1, 1, 2);
                        eprow++;
                    }

                    // FORCEVARS — per-variant variable seeds (key → value pairs)
                    {
                        if (!EP.contains("FORCEVARS") || !EP["FORCEVARS"].is_object())
                            (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"] = nlohmann::ordered_json::object();
                        auto &FVObj = (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"];

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
                            FVValField->setProperty("JSONPath", QString("/GAMES/%1/VARIANTS/%2/FORCEVARS/%3").arg(i).arg(epj).arg(QString::fromStdString(KeyCopy)));
                            if (FVVal.is_string()) FVValField->setText(QString::fromStdString(std::string(FVVal)));
                            QObject::connect(FVValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QObject::connect(FVKeyField, &QLineEdit::editingFinished, this, [this, i, epj, KeyCopy, FVKeyField, FVValField](){
                                QString NewKey = FVKeyField->text();
                                if (NewKey.isEmpty() || NewKey.toStdString() == KeyCopy) return;
                                auto Val = (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"][KeyCopy];
                                (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"].erase(KeyCopy);
                                (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"][NewKey.toStdString()] = Val;
                                FVValField->setProperty("JSONPath", QString("/GAMES/%1/VARIANTS/%2/FORCEVARS/%3").arg(i).arg(epj).arg(NewKey));
                                SaveManifestJSON(); BuildUI();
                            });
                            QPushButton * FVDel = new QPushButton("✕", FVBox);
                            FVDel->setFixedWidth(28);
                            QObject::connect(FVDel, &QPushButton::clicked, this, [this, i, epj, KeyCopy](){
                                (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"].erase(KeyCopy);
                                SaveManifestJSON(); BuildUI();
                            });
                            FVRow->addWidget(FVKeyField); FVRow->addWidget(FVValField); FVRow->addWidget(FVDel);
                            FVLayout->addLayout(FVRow);
                        }
                        QPushButton * FVAddBtn = new QPushButton("+ Add Force Var", FVBox);
                        QObject::connect(FVAddBtn, &QPushButton::clicked, this, [this, i, epj](){
                            (*MANIFESTJSON)["GAMES"][i]["VARIANTS"][epj]["FORCEVARS"]["NEW_KEY"] = "";
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
                    bool         EPHasEndpoints = EP.contains("MODULES") && EP["MODULES"].is_array() && !EP["MODULES"].empty();
                    QObject::connect(EPExecBtn, &QPushButton::clicked, this,
                    [this, EPSubgameID, EPEntryID, EPHasEndpoints]()
                    {
                        if (!EPHasEndpoints)
                        { QMessageBox::warning(this, "Execute", "Variant has no MODULES set."); return; }
                        if (EPEntryID.empty())
                        { QMessageBox::warning(this, "Execute", "Variant has no VARIANT_ID set."); return; }

                        // Candidate runners: the package's own (embedded bundle) + the GLOBAL_RUNNERS
                        // registry, filtered by GUEST_PLATFORM ∋ the package's HOST_PLATFORM.
                        std::vector<std::pair<QString, QString>> Runners; // (label, RUNNER_ID)
                        std::string SGPlatform = SubgamePlatform(EPSubgameID);
                        auto CollectR = [&](const nlohmann::ordered_json &R)
                        {
                            bool Match = SGPlatform.empty();
                            if (R.contains("GUEST_PLATFORM") && R["GUEST_PLATFORM"].is_array())
                                for (auto &P : R["GUEST_PLATFORM"]) if (P.is_string() && std::string(P) == SGPlatform) { Match = true; break; }
                            if (!Match) return;
                            QString RID = QString::fromStdString(R.value("RUNNER_ID", std::string()));
                            std::string Name = (R.contains("NAME") && R["NAME"].is_string()) ? std::string(R["NAME"]) : R.value("RUNNER_ID", std::string("(unnamed)"));
                            Runners.push_back({QString::fromStdString(Name), RID});
                        };
                        if ((*MANIFESTJSON).contains("RUNNERS") && (*MANIFESTJSON)["RUNNERS"].is_array())
                            for (auto &R : (*MANIFESTJSON)["RUNNERS"]) CollectR(R);
                        for (auto &R : ContainerWrapper::RegistryRunners(*GlobalConfigJSON)) CollectR(R);
                        if (Runners.empty()) { QMessageBox::warning(this, "Execute", "No runners for this platform."); return; }

                        QDialog D(this);
                        D.setWindowTitle(QString("Execute: %1").arg(QString::fromStdString(EPEntryID)));
                        D.setMinimumWidth(380);
                        QVBoxLayout * DL = new QVBoxLayout(&D);
                        QFormLayout * DF = new QFormLayout(); DL->addLayout(DF);
                        QComboBox * RC = new QComboBox(&D);
                        for (auto &[Label, Rid] : Runners) RC->addItem(Label, Rid);
                        DF->addRow("Runner:", RC);
                        QHBoxLayout * BR = new QHBoxLayout(); DL->addLayout(BR); BR->addStretch();
                        QPushButton * OkBtn = new QPushButton("Execute", &D); OkBtn->setDefault(true);
                        QPushButton * CaBtn = new QPushButton("Cancel",  &D);
                        BR->addWidget(CaBtn); BR->addWidget(OkBtn);
                        QObject::connect(CaBtn, &QPushButton::clicked, &D, &QDialog::reject);
                        QObject::connect(OkBtn, &QPushButton::clicked, &D, &QDialog::accept);
                        if (D.exec() != QDialog::Accepted) return;

                        // Set VariantID + RunnerID pre-construction; DeriveContainerParams/CreateRecipe resolve
                        // the runner, its ENDPOINTS, and all paths.
                        ContainerParams Params(PackageDir->path().toStdString(), EPSubgameID, "");
                        Params.VariantID = EPEntryID;
                        Params.RunnerID  = RC->currentData().toString().toStdString();
                        ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

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

                QPushButton * AddEPBtn = new QPushButton("+ Add Variant", EPBox);
                QObject::connect(AddEPBtn, &QPushButton::clicked, this, [this, i](){
                    QString File = PromptTargetFile("Add Variant");
                    if (File.isEmpty()) return;
                    (*MANIFESTJSON)["GAMES"][i]["VARIANTS"].push_back(json::object({
                        {"VARIANT_ID", "main"}, {"MODULES", json::array()}, {"EXEPATH", ""}, {"EXEARGS", ""}, {"WORKDIR", ""}, {"__FILE__", File.toStdString()}
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


    // Persistence is no longer a top-level tab — it is declared at specific points as
    // PersistDir / PersistFile / RegPersist subcomponents inside components (see the component
    // "+ Subcomponent" menu). When a package declares none, the whole runtime overlay persists.

    // RUNNERS TAB — flat array of first-class runners (each routed to its fragment file).
    // Only shown when the package actually defines runners; add new ones from the top toolbar.
    if ((*MANIFESTJSON).contains("RUNNERS") && (*MANIFESTJSON)["RUNNERS"].is_array() && !(*MANIFESTJSON)["RUNNERS"].empty())
    {
        QWidget * RTabWidget = new QWidget(PackageEditorTabWidget);
        QVBoxLayout * RTabLayout = new QVBoxLayout(RTabWidget);
        RTabWidget->setLayout(RTabLayout);
        QScrollArea * RScroll = new QScrollArea(RTabWidget);
        RScroll->setWidgetResizable(true);
        RTabLayout->addWidget(RScroll);
        QWidget * RContents = new QWidget();
        QVBoxLayout * RLayout = new QVBoxLayout(RContents);
        RContents->setLayout(RLayout);
        RScroll->setWidget(RContents);

        auto &RunnerArr = (*MANIFESTJSON)["RUNNERS"];

        for (int ri = 0; ri < (int)RunnerArr.size(); ri++)
        {
            auto &R = RunnerArr[ri];
            std::string CurRID = R.value("RUNNER_ID", std::string());
            QGroupBox * RCard = new QGroupBox(CurRID.empty() ? QString("Runner %1").arg(ri + 1) : QString::fromStdString(CurRID), RContents);
            QGridLayout * RG = new QGridLayout(RCard);
            RCard->setLayout(RG);
            int rrow = 0;

            // Remove button + file tag.
            QPushButton * RemBtn = new QPushButton("✕", RCard); RemBtn->setFixedWidth(28);
            QObject::connect(RemBtn, &QPushButton::clicked, this, [this, ri](){
                (*MANIFESTJSON)["RUNNERS"].erase(ri); SaveManifestJSON(); BuildUI();
            });
            RG->addWidget(RemBtn, 0, 2);
            RG->addWidget(MakeFileTagWidget("/RUNNERS/" + std::to_string(ri)), rrow, 0, 1, 2); rrow++;

            // Entity-level fields (RUNNER_ID, NAME). Everything platform/exec lives on the VARIANTS below.
            for (const auto &FK : std::vector<std::string>{"RUNNER_ID", "NAME"})
            {
                RG->addWidget(new QLabel(QString::fromStdString(FK) + ":"), rrow, 0);
                QLineEdit * FE = new QLineEdit(RCard);
                FE->setProperty("JSONPath", QString("/RUNNERS/%1/%2").arg(ri).arg(QString::fromStdString(FK)));
                if (R.contains(FK) && R[FK].is_string()) FE->setText(QString::fromStdString(std::string(R[FK])));
                QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                RG->addWidget(FE, rrow, 1, 1, 2); rrow++;
            }

            // VARIANTS — runner variants mirror game variants: each carries HOST_PLATFORM, the guests it
            // serves (GUEST_PLATFORM), its exec params (TYPE/EXECUTABLE/PREFIX_SUBPATH/ARGS/ENV) and MODULES
            // (its build components, via the shared editor).
            {
                if (!R.contains("VARIANTS") || !R["VARIANTS"].is_array())
                    (*MANIFESTJSON)["RUNNERS"][ri]["VARIANTS"] = nlohmann::ordered_json::array();
                auto &VArr = (*MANIFESTJSON)["RUNNERS"][ri]["VARIANTS"];

                QGroupBox * VBox = new QGroupBox("Variants", RCard);
                QVBoxLayout * VBL = new QVBoxLayout(VBox); VBox->setLayout(VBL);

                for (int vj = 0; vj < (int)VArr.size(); vj++)
                {
                    auto &V = VArr[vj];
                    const std::string VPtr = "/RUNNERS/" + std::to_string(ri) + "/VARIANTS/" + std::to_string(vj);
                    QGroupBox * VCard = new QGroupBox(QString::fromStdString(V.value("VARIANT_ID", std::string("Variant"))), VBox);
                    QGridLayout * VG = new QGridLayout(VCard); VCard->setLayout(VG);
                    int vrow = 0;

                    VG->addWidget(new QLabel("VARIANT_ID:"), vrow, 0);
                    QLineEdit * VIDF = new QLineEdit(VCard);
                    VIDF->setProperty("JSONPath", QString::fromStdString(VPtr + "/VARIANT_ID"));
                    VIDF->setText(QString::fromStdString(V.value("VARIANT_ID", std::string())));
                    QObject::connect(VIDF, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    VG->addWidget(VIDF, vrow, 1);
                    QPushButton * VRem = new QPushButton("✕", VCard); VRem->setFixedWidth(28);
                    QObject::connect(VRem, &QPushButton::clicked, this, [this, ri, vj](){
                        (*MANIFESTJSON)["RUNNERS"][ri]["VARIANTS"].erase(vj); SaveManifestJSON(); BuildUI();
                    });
                    VG->addWidget(VRem, vrow, 2); vrow++;

                    QCheckBox * Rec = new QCheckBox("Recommended (default variant)", VCard);
                    Rec->setChecked(V.value("RECOMMENDED", false));
                    QObject::connect(Rec, &QCheckBox::toggled, this, [this, ri, vj, Rec](){
                        auto &A = (*MANIFESTJSON)["RUNNERS"][ri]["VARIANTS"];
                        if (Rec->isChecked()) for (int k = 0; k < (int)A.size(); k++) A[k]["RECOMMENDED"] = (k == vj);
                        else A[vj]["RECOMMENDED"] = false;
                        SaveManifestJSON(); RefreshJSONView();
                    });
                    VG->addWidget(Rec, vrow, 0, 1, 3); vrow++;

                    for (const auto &FK : std::vector<std::string>{"HOST_PLATFORM", "EXECUTABLE", "PREFIX_SUBPATH"})
                    {
                        VG->addWidget(new QLabel(QString::fromStdString(FK) + ":"), vrow, 0);
                        QLineEdit * FE = new QLineEdit(VCard);
                        FE->setProperty("JSONPath", QString::fromStdString(VPtr + "/" + FK));
                        if (V.contains(FK) && V[FK].is_string()) FE->setText(QString::fromStdString(std::string(V[FK])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        VG->addWidget(FE, vrow, 1, 1, 2); vrow++;
                    }

                    VG->addWidget(new QLabel("TYPE:"), vrow, 0);
                    QComboBox * TC = new QComboBox(VCard); TC->addItems({"wine", "emulator", "native", "custom"});
                    TC->setCurrentText(QString::fromStdString(V.value("TYPE", std::string("custom"))));
                    QObject::connect(TC, &QComboBox::currentIndexChanged, this, [this, VPtr, TC](){
                        (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(VPtr + "/TYPE")] = TC->currentText().toStdString();
                        SaveManifestJSON(); RefreshJSONView();
                    });
                    VG->addWidget(TC, vrow, 1, 1, 2); vrow++;

                    // String-list editor (GUEST_PLATFORM, ARGS) addressed at the variant path.
                    auto AddVarList = [&](const QString &Title, const std::string &Key){
                        const std::string AP = VPtr + "/" + Key;
                        const nlohmann::ordered_json::json_pointer APtr(AP);
                        if (!(*MANIFESTJSON).contains(APtr) || !(*MANIFESTJSON).at(APtr).is_array())
                            (*MANIFESTJSON)[APtr] = nlohmann::ordered_json::array();
                        auto &Arr = (*MANIFESTJSON).at(APtr);
                        QGroupBox * Box = new QGroupBox(Title, VCard);
                        QVBoxLayout * BL = new QVBoxLayout(Box); Box->setLayout(BL);
                        for (int k = 0; k < (int)Arr.size(); k++)
                        {
                            QHBoxLayout * Row = new QHBoxLayout();
                            QLineEdit * LE = new QLineEdit(Box);
                            LE->setProperty("JSONPath", QString::fromStdString(AP + "/" + std::to_string(k)));
                            LE->setText(QString::fromStdString(Arr[k].is_string() ? std::string(Arr[k]) : ""));
                            QObject::connect(LE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QPushButton * Del = new QPushButton("✕", Box); Del->setFixedWidth(28);
                            QObject::connect(Del, &QPushButton::clicked, this, [this, AP, k](){
                                (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(AP)].erase(k); SaveManifestJSON(); BuildUI();
                            });
                            Row->addWidget(LE, 1); Row->addWidget(Del); BL->addLayout(Row);
                        }
                        QPushButton * Add = new QPushButton("+ Add", Box);
                        QObject::connect(Add, &QPushButton::clicked, this, [this, AP](){
                            (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(AP)].push_back(""); SaveManifestJSON(); BuildUI();
                        });
                        BL->addWidget(Add);
                        VG->addWidget(Box, vrow, 0, 1, -1); vrow++;
                    };
                    AddVarList("GUEST_PLATFORM", "GUEST_PLATFORM");
                    AddVarList("ARGS", "ARGS");

                    // ENV (key/value) at the variant path.
                    {
                        const std::string EPth = VPtr + "/ENV";
                        const nlohmann::ordered_json::json_pointer EPtr(EPth);
                        if (!(*MANIFESTJSON).contains(EPtr) || !(*MANIFESTJSON).at(EPtr).is_object())
                            (*MANIFESTJSON)[EPtr] = nlohmann::ordered_json::object();
                        auto &EnvObj = (*MANIFESTJSON).at(EPtr);
                        QGroupBox * EnvBox = new QGroupBox("ENV", VCard);
                        QVBoxLayout * EL = new QVBoxLayout(EnvBox); EnvBox->setLayout(EL);
                        for (auto &[EnvKey, EnvVal] : EnvObj.items())
                        {
                            std::string KeyCopy = EnvKey;
                            QHBoxLayout * Row = new QHBoxLayout();
                            QLineEdit * KeyField = new QLineEdit(EnvBox); KeyField->setText(QString::fromStdString(KeyCopy)); KeyField->setPlaceholderText("KEY");
                            QLineEdit * ValField = new QLineEdit(EnvBox);
                            ValField->setProperty("JSONPath", QString::fromStdString(EPth + "/" + KeyCopy));
                            if (EnvVal.is_string()) ValField->setText(QString::fromStdString(std::string(EnvVal)));
                            QObject::connect(ValField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QObject::connect(KeyField, &QLineEdit::editingFinished, this, [this, EPth, KeyCopy, KeyField](){
                                std::string NewKey = KeyField->text().toStdString();
                                if (NewKey == KeyCopy || NewKey.empty()) return;
                                auto &O = (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(EPth)];
                                auto Val = O[KeyCopy]; O.erase(KeyCopy); O[NewKey] = Val; SaveManifestJSON(); BuildUI();
                            });
                            QPushButton * Del = new QPushButton("✕", EnvBox); Del->setFixedWidth(28);
                            QObject::connect(Del, &QPushButton::clicked, this, [this, EPth, KeyCopy](){
                                (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(EPth)].erase(KeyCopy); SaveManifestJSON(); BuildUI();
                            });
                            Row->addWidget(KeyField); Row->addWidget(ValField); Row->addWidget(Del); EL->addLayout(Row);
                        }
                        QPushButton * AddEnv = new QPushButton("+ Add Env Var", EnvBox);
                        QObject::connect(AddEnv, &QPushButton::clicked, this, [this, EPth](){
                            (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer(EPth)]["NEW_KEY"] = ""; SaveManifestJSON(); BuildUI();
                        });
                        EL->addWidget(AddEnv);
                        VG->addWidget(EnvBox, vrow, 0, 1, -1); vrow++;
                    }

                    // MODULES — the variant's build components (reusable editor; this is "add components to a runner").
                    VG->addWidget(BuildModulesEditor(VPtr), vrow, 0, 1, -1); vrow++;

                    VBL->addWidget(VCard);
                }
                QPushButton * AddVar = new QPushButton("+ Add Variant", VBox);
                QObject::connect(AddVar, &QPushButton::clicked, this, [this, ri](){
                    (*MANIFESTJSON)["RUNNERS"][ri]["VARIANTS"].push_back(nlohmann::ordered_json{
                        {"VARIANT_ID", "default"}, {"HOST_PLATFORM", "linux64"}, {"GUEST_PLATFORM", nlohmann::ordered_json::array()},
                        {"TYPE", "custom"}, {"EXECUTABLE", ""}, {"ARGS", nlohmann::ordered_json::array()},
                        {"ENV", nlohmann::ordered_json::object()}, {"MODULES", nlohmann::ordered_json::array()}});
                    SaveManifestJSON(); BuildUI();
                });
                VBL->addWidget(AddVar);
                RG->addWidget(VBox, rrow, 0, 1, -1); rrow++;
            }

            RLayout->addWidget(RCard);
        }

        RLayout->addStretch();
        PackageEditorTabWidget->addTab(RTabWidget, "RUNNERS");
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

            //Which fragment file this component lives in (per-element routing).
            ComponentTabWidgetLayout->addWidget(MakeFileTagWidget("/COMPONENTS/" + std::to_string(i)));

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
                AddSubComponentMenu->addSeparator();
                AddSubComponentMenu->addAction("PersistDir",         this, &PackageEditor::AddPersistDir);
                AddSubComponentMenu->addAction("PersistFile",        this, &PackageEditor::AddPersistFile);
                AddSubComponentMenu->addAction("RegPersist",         this, &PackageEditor::AddRegPersist);
                AddSubComponentMenu->addAction("RegKeyPersist",      this, &PackageEditor::AddRegKeyPersist);
                AddSubComponentMenu->addSeparator();
                AddSubComponentMenu->addAction("CustomVar",          this, &PackageEditor::AddCustomVar);
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
                            std::filesystem::path SrcDir  = std::filesystem::path(PackageDir->path().toStdString()) / Path;
                            std::string ZipName           = Path + ".zip";
                            std::filesystem::path ZipFile = std::filesystem::path(PackageDir->path().toStdString()) / ZipName;
                            //zip -0 -r -X from INSIDE SrcDir, archiving "." — so entries are RELATIVE to the
                            //source dir's contents (e.g. "Characters/x"), not the absolute host path. Mounted
                            //at the layer's TARGET they then land at the right place. STORE (-0) keeps the
                            //layer zero-copy (vidyagodfs preads it; ZIP64 auto past 4 GB).
                            ContainerWrapper::RunCommand("zip", {"-0", "-r", "-X", ZipFile.string(), "."},
                                                         QProcessEnvironment::systemEnvironment(), SrcDir.string());
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
                            std::filesystem::path ZipFile = std::filesystem::path(PackageDir->path().toStdString()) / Path;
                            //Strip .zip extension for the directory name.
                            std::string DirName = (Path.size() > 4 && Path.substr(Path.size() - 4) == ".zip")
                                                  ? Path.substr(0, Path.size() - 4) : Path + "_dir";
                            std::filesystem::path DirPath = std::filesystem::path(PackageDir->path().toStdString()) / DirName;
                            std::filesystem::create_directories(DirPath);
                            ContainerWrapper::RunCommand("unzip", {ZipFile.string(), "-d", DirPath.string()});
                            std::filesystem::remove(ZipFile);
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["TYPE"] = "VFSDirLayer";
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["PATH"] = DirName;
                            SaveManifestJSON(); BuildUI();
                        });
                        IndividualSubComponentGroupBoxLayout->addWidget(ConvertBtn, 1, 2);
                    }

                    //TARGET + SUBPATH fields — for VFSZipLayer and VFSDirLayer only.
                    if (SubComponentType == "VFSZipLayer" || SubComponentType == "VFSDirLayer")
                    {
                        auto &SubRef = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];

                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("TARGET:"), 2, 0);
                        QLineEdit * TargetField = new QLineEdit(IndividualSubComponentGroupBox);
                        QString TargetJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/TARGET").arg(i).arg(j);
                        TargetField->setProperty("JSONPath", TargetJSONPath);
                        if (SubRef.contains("TARGET") && SubRef["TARGET"].is_string())
                            TargetField->setText(QString::fromStdString(std::string(SubRef["TARGET"])));
                        QObject::connect(TargetField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(TargetField, 2, 1);

                        //SUBPATH — mount only a path WITHIN the source at TARGET (empty = whole source). For a
                        //single file, point SUBPATH at it and make TARGET the full destination path (incl. filename).
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("SUBPATH:"), 3, 0);
                        QLineEdit * SubpathField = new QLineEdit(IndividualSubComponentGroupBox);
                        SubpathField->setPlaceholderText("optional: path inside the source (e.g. MS/x86/D3D9.dll)");
                        QString SubpathJSONPath = QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/SUBPATH").arg(i).arg(j);
                        SubpathField->setProperty("JSONPath", SubpathJSONPath);
                        if (SubRef.contains("SUBPATH") && SubRef["SUBPATH"].is_string())
                            SubpathField->setText(QString::fromStdString(std::string(SubRef["SUBPATH"])));
                        QObject::connect(SubpathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(SubpathField, 3, 1);
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
                else if (SubComponentType == "PersistDir" || SubComponentType == "PersistFile")
                {
                    // Runtime-root-relative PATH of the directory / file that should survive a session.
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("PATH:"), 1, 0);
                    QLineEdit * PathField = new QLineEdit(IndividualSubComponentGroupBox);
                    PathField->setPlaceholderText(SubComponentType == "PersistDir"
                        ? "drive_c/users/steamuser/Saved Games/<game>"
                        : "drive_c/users/steamuser/Documents/<game>/config.ini");
                    PathField->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/PATH").arg(i).arg(j));
                    auto &PSub = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    if (PSub.contains("PATH") && PSub["PATH"].is_string())
                        PathField->setText(QString::fromStdString(std::string(PSub["PATH"])));
                    QObject::connect(PathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(PathField, 1, 1, 1, 2);

                    QLabel * Hint = new QLabel(SubComponentType == "PersistDir"
                        ? "Bind-mounted live from the package's USERDATA store."
                        : "Seeded before launch and captured on exit (copy, like the registry).",
                        IndividualSubComponentGroupBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                    Hint->setWordWrap(true);
                    IndividualSubComponentGroupBoxLayout->addWidget(Hint, 2, 1, 1, 2);
                }
                else if (SubComponentType == "RegPersist")
                {
                    // No fields — its mere presence in the recipe makes the whole Wine registry persist.
                    QLabel * Hint = new QLabel("Persists the whole prefix registry (user/system/userdef.reg). For a single key, use RegKeyPersist instead.", IndividualSubComponentGroupBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                    Hint->setWordWrap(true);
                    IndividualSubComponentGroupBoxLayout->addWidget(Hint, 1, 1, 1, 2);
                }
                else if (SubComponentType == "RegKeyPersist")
                {
                    // REGPATH of the registry key whose subtree should survive a session.
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("REGPATH:"), 1, 0);
                    QLineEdit * RegPathField = new QLineEdit(IndividualSubComponentGroupBox);
                    RegPathField->setPlaceholderText("HKCU\\Software\\<vendor>\\<game>\\Saves");
                    RegPathField->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/REGPATH").arg(i).arg(j));
                    auto &RSub = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    if (RSub.contains("REGPATH") && RSub["REGPATH"].is_string())
                        RegPathField->setText(QString::fromStdString(std::string(RSub["REGPATH"])));
                    QObject::connect(RegPathField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                    IndividualSubComponentGroupBoxLayout->addWidget(RegPathField, 1, 1, 1, 2);

                    QLabel * Hint = new QLabel("Persists just this key's subtree across sessions (seeded into the prefix at launch, captured on exit).", IndividualSubComponentGroupBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                    Hint->setWordWrap(true);
                    IndividualSubComponentGroupBoxLayout->addWidget(Hint, 2, 1, 1, 2);
                }
                else if (SubComponentType == "CustomVar")
                {
                    auto &CV = (*PackageEditor::MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j];
                    std::string VarType = CV.value("VARTYPE", std::string("string"));
                    int cvrow = 1;

                    // KEY / LABEL / DEFAULT text fields.
                    QLineEdit * DefaultField = nullptr;
                    for (const auto &FieldKey : std::vector<std::string>{"KEY", "LABEL", "DEFAULT"})
                    {
                        IndividualSubComponentGroupBoxLayout->addWidget(new QLabel(QString::fromStdString(FieldKey) + ":"), cvrow, 0);
                        QLineEdit * FE = new QLineEdit(IndividualSubComponentGroupBox);
                        FE->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/%3").arg(i).arg(j).arg(QString::fromStdString(FieldKey)));
                        if (CV.contains(FieldKey) && CV[FieldKey].is_string())
                            FE->setText(QString::fromStdString(std::string(CV[FieldKey])));
                        QObject::connect(FE, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                        IndividualSubComponentGroupBoxLayout->addWidget(FE, cvrow, 1, 1, 2);
                        if (FieldKey == "DEFAULT") DefaultField = FE;
                        cvrow++;
                    }

                    // DEFAULT processed-value hint — live preview of what DEFAULT becomes after the
                    // VARTYPE Layer-2 translation (e.g. bool "true" → dword:00000001, dword 1920 →
                    // dword:00000780). Updates as the author types or changes VARTYPE (rebuild).
                    {
                        QLabel * DefaultHint = new QLabel(IndividualSubComponentGroupBox);
                        DefaultHint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                        std::string VT = VarType;
                        auto UpdateHint = [DefaultHint, VT](const QString &Raw){
                            std::string Translated = ContainerWrapper::TranslateCustomVarValue(Raw.toStdString(), VT);
                            DefaultHint->setText(Translated == Raw.toStdString()
                                                 ? QString()
                                                 : QString("→ stored as  %1").arg(QString::fromStdString(Translated)));
                        };
                        if (DefaultField)
                        {
                            UpdateHint(DefaultField->text());
                            QObject::connect(DefaultField, &QLineEdit::textChanged, this, [UpdateHint](const QString &T){ UpdateHint(T); });
                        }
                        IndividualSubComponentGroupBoxLayout->addWidget(DefaultHint, cvrow, 1, 1, 2);
                        cvrow++;
                    }

                    // VARTYPE combo (rebuilds to show/hide the OPTIONS editor).
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("VARTYPE:"), cvrow, 0);
                    QComboBox * VTCombo = new QComboBox(IndividualSubComponentGroupBox);
                    VTCombo->addItems({"string", "number", "dword", "qword", "bool", "options", "random"});
                    VTCombo->setCurrentText(QString::fromStdString(VarType));
                    QObject::connect(VTCombo, &QComboBox::currentIndexChanged, this, [this, i, j, VTCombo](){
                        (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["VARTYPE"] = VTCombo->currentText().toStdString();
                        SaveManifestJSON(); BuildUI();
                    });
                    IndividualSubComponentGroupBoxLayout->addWidget(VTCombo, cvrow, 1, 1, 2);
                    cvrow++;

                    // DISPLAY checkbox — whether the var is shown as a picker in the PreLaunch window.
                    IndividualSubComponentGroupBoxLayout->addWidget(new QLabel("DISPLAY:"), cvrow, 0);
                    QCheckBox * DisplayCheck = new QCheckBox(IndividualSubComponentGroupBox);
                    DisplayCheck->setChecked(CV.value("DISPLAY", true));
                    QObject::connect(DisplayCheck, &QCheckBox::toggled, this, [this, i, j](bool On){
                        (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["DISPLAY"] = On;
                        SaveManifestJSON(); RefreshJSONView();
                    });
                    IndividualSubComponentGroupBoxLayout->addWidget(DisplayCheck, cvrow, 1, 1, 2);
                    cvrow++;

                    // OPTIONS (options: label+value) / value pool (random: value-only).
                    if (VarType == "options" || VarType == "random")
                    {
                        if (!CV.contains("OPTIONS") || !CV["OPTIONS"].is_array())
                            (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["OPTIONS"] = nlohmann::ordered_json::array();
                        bool IsRandom = (VarType == "random");
                        QGroupBox * OptsBox = new QGroupBox(IsRandom ? "Value Pool (random pick on launch)" : "OPTIONS", IndividualSubComponentGroupBox);
                        QVBoxLayout * OptsLayout = new QVBoxLayout(OptsBox); OptsBox->setLayout(OptsLayout);
                        auto &Opts = (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["OPTIONS"];
                        for (int oi = 0; oi < (int)Opts.size(); oi++)
                        {
                            QHBoxLayout * OptRow = new QHBoxLayout();
                            if (!IsRandom)
                            {
                                QLineEdit * OptLabel = new QLineEdit(OptsBox); OptLabel->setPlaceholderText("Label");
                                OptLabel->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/OPTIONS/%3/LABEL").arg(i).arg(j).arg(oi));
                                if (Opts[oi].contains("LABEL") && Opts[oi]["LABEL"].is_string())
                                    OptLabel->setText(QString::fromStdString(std::string(Opts[oi]["LABEL"])));
                                QObject::connect(OptLabel, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                                OptRow->addWidget(OptLabel);
                            }
                            QLineEdit * OptValue = new QLineEdit(OptsBox); OptValue->setPlaceholderText("Value");
                            OptValue->setProperty("JSONPath", QString("/COMPONENTS/%1/SUBCOMPONENTS/%2/OPTIONS/%3/VALUE").arg(i).arg(j).arg(oi));
                            if (Opts[oi].contains("VALUE") && Opts[oi]["VALUE"].is_string())
                                OptValue->setText(QString::fromStdString(std::string(Opts[oi]["VALUE"])));
                            QObject::connect(OptValue, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                            QPushButton * OptDel = new QPushButton("✕", OptsBox); OptDel->setFixedWidth(28);
                            QObject::connect(OptDel, &QPushButton::clicked, this, [this, i, j, oi](){
                                (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["OPTIONS"].erase(oi);
                                SaveManifestJSON(); BuildUI();
                            });
                            OptRow->addWidget(OptValue); OptRow->addWidget(OptDel);
                            OptsLayout->addLayout(OptRow);
                        }
                        QPushButton * AddOptBtn = new QPushButton(IsRandom ? "+ Add Value" : "+ Add Option", OptsBox);
                        QObject::connect(AddOptBtn, &QPushButton::clicked, this, [this, i, j, IsRandom](){
                            if (IsRandom) (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["OPTIONS"].push_back(json::object({{"VALUE", ""}}));
                            else          (*MANIFESTJSON)["COMPONENTS"][i]["SUBCOMPONENTS"][j]["OPTIONS"].push_back(json::object({{"LABEL", ""}, {"VALUE", ""}}));
                            SaveManifestJSON(); BuildUI();
                        });
                        OptsLayout->addWidget(AddOptBtn);
                        IndividualSubComponentGroupBoxLayout->addWidget(OptsBox, cvrow, 0, 1, -1);
                        cvrow++;
                    }

                    // Token hint — how to reference this var elsewhere (bare global %KEY%, no component prefix).
                    std::string Bare   = CV.value("KEY", std::string("KEY"));
                    QLabel * Hint = new QLabel("Reference as  %" + QString::fromStdString(Bare) + "%", IndividualSubComponentGroupBox);
                    Hint->setStyleSheet("color:#8f98a0;font-size:9pt;");
                    IndividualSubComponentGroupBoxLayout->addWidget(Hint, cvrow, 0, 1, -1);
                    cvrow++;
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

std::string PackageEditor::SubgamePlatform(const std::string &SubgameID)
{
    //Platform is now package identity (HOST_PLATFORM), not per-game. Game id is ignored.
    (void)SubgameID;
    if ((*MANIFESTJSON).contains("HOST_PLATFORM") && (*MANIFESTJSON)["HOST_PLATFORM"].is_string())
        return std::string((*MANIFESTJSON)["HOST_PLATFORM"]);
    return "";
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
    // Writes the raw editor text to the SELECTED fragment file, then re-assembles from disk.
    QString File = (JSONFileCombo && JSONFileCombo->currentIndex() >= 0) ? JSONFileCombo->currentText() : IdentityFile;
    if (File.isEmpty()) return;
    QByteArray Data = JSONTextEdit->toPlainText().toUtf8();
    if (!nlohmann::ordered_json::accept(Data))
    { QMessageBox::warning(this, "Save File", "Invalid JSON — not saved."); return; }
    nlohmann::ordered_json Doc = nlohmann::ordered_json::parse(Data);
    QFile F(PackageDir->filePath(File));
    JSONOps::SaveJSON(&Doc, &F);
    LoadFragmentsAndAssemble(); // re-read all fragments, re-tag, re-merge
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

    //Compute the ancestor chain for this component so we can match variants by their ENDPOINTS.
    ContainerParams TmpParams(PackageDir->path().toStdString(), "", ComponentID);
    ContainerWrapper::CreateRecipe(*MANIFESTJSON, TmpParams);
    std::unordered_set<std::string> RecipeSet(TmpParams.Recipe.begin(), TmpParams.Recipe.end());

    //Collect variants from all subgames that have ANY endpoint in this component's ancestor chain.
    //Store (display label, subgame_id, variant JSON) triples.
    struct EPEntry { QString Label; std::string SubgameID; nlohmann::ordered_json EP; };
    std::vector<EPEntry> EPList;
    if ((*MANIFESTJSON).contains("GAMES"))
        for (auto &SG : (*MANIFESTJSON)["GAMES"])
        {
            std::string SGID = SG.value("GAMEID", std::string());
            if (!SG.contains("VARIANTS") || !SG["VARIANTS"].is_array()) continue;
            for (auto &V : SG["VARIANTS"])
            {
                std::vector<std::string> Endpoints;
                if (V.contains("MODULES") && V["MODULES"].is_array())
                    for (auto &M : V["MODULES"])
                        if (M.is_object() && M.contains("COMPONENT") && M["COMPONENT"].is_string())
                            Endpoints.push_back(std::string(M["COMPONENT"]));
                bool Matches = false;
                for (const auto &E : Endpoints) if (RecipeSet.count(E)) { Matches = true; break; }
                if (!Matches) continue;
                QString Joined; for (size_t k = 0; k < Endpoints.size(); k++) Joined += (k ? ", " : "") + QString::fromStdString(Endpoints[k]);
                QString EPLabel = QString("(%1)  %2  →  [%3]")
                    .arg(QString::fromStdString(SGID))
                    .arg(QString::fromStdString(V.value("VARIANT_ID", std::string("?"))))
                    .arg(Joined);
                EPList.push_back({EPLabel, SGID, V});
            }
        }

    if (EPList.empty())
    {
        QMessageBox::warning(this, "Execute Component", "No variants found with an endpoint in this component's chain.\nAdd a variant to a subgame first.");
        return;
    }

    //Collect candidate runners (flat arrays). The component's variants belong to a subgame whose
    //platform filters the runners; if multiple subgames match, accept any of their platforms.
    std::set<std::string> WantPlatforms;
    for (auto &EPE : EPList) { std::string p = SubgamePlatform(EPE.SubgameID); if (!p.empty()) WantPlatforms.insert(p); }
    std::vector<std::pair<QString, QString>> Runners; // (label, RUNNER_ID)
    auto CollectRunner = [&](const nlohmann::ordered_json &Runner, const QString &SourceLabel)
    {
        bool Match = WantPlatforms.empty();
        if (Runner.contains("GUEST_PLATFORM") && Runner["GUEST_PLATFORM"].is_array())
            for (auto &P : Runner["GUEST_PLATFORM"]) if (P.is_string() && WantPlatforms.count(std::string(P))) { Match = true; break; }
        if (!Match) return;
        QString Name    = QString::fromStdString(Runner.value("NAME", Runner.value("RUNNER_ID", std::string("(unnamed)"))));
        QString Display = Name + "  (" + SourceLabel + ")";
        Runners.push_back({Display, QString::fromStdString(Runner.value("RUNNER_ID", std::string()))});
    };
    if ((*MANIFESTJSON).contains("RUNNERS") && (*MANIFESTJSON)["RUNNERS"].is_array())
        for (auto &R : (*MANIFESTJSON)["RUNNERS"]) CollectRunner(R, "bundle");
    for (auto &R : ContainerWrapper::RegistryRunners(*GlobalConfigJSON)) CollectRunner(R, "registry");

    if (Runners.empty())
    {
        QMessageBox::warning(this, "Execute Component", "No runners for this component's platform(s).");
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
    Form->addRow("Variant:", EPPicker);

    QComboBox * RunnerPicker = new QComboBox(&Dialog);
    for (auto &[Label, Rid] : Runners) RunnerPicker->addItem(Label, Rid);
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

    EPEntry &SelectedEP = EPList[EPIdx];
    std::string SelectedEPID = SelectedEP.EP.value("VARIANT_ID", std::string());

    //Set VariantID + RunnerID pre-construction; DeriveContainerParams/CreateRecipe resolve the runner,
    //its ENDPOINTS, and all paths.
    ContainerParams Params(PackageDir->path().toStdString(), SelectedEP.SubgameID, "");
    Params.VariantID = SelectedEPID;
    Params.RunnerID  = RunnerPicker->currentData().toString().toStdString();
    ContainerWrapper Container(*GlobalConfigJSON, *MANIFESTJSON, Params);

    if (!ContainerWrapper::ResolveExecutableDefinition(*MANIFESTJSON, Container.ContainerParams))
    {
        QMessageBox::critical(this, "Execute Component", "Failed to resolve variant.\nCheck VARIANT_ID and MODULES.");
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
    std::string PackageUID = MANIFESTJSON->value("PACKAGEUID", std::string());

    // All ephemeral session state lives under ~/.VidyaGod/TEMP/PackageUID.
    std::filesystem::path SessionTemp = std::filesystem::path(QDir::homePath().toStdString())
                                        / ".VidyaGod" / "TEMP" / PackageUID;

    // Build the comparator — the baseline state of oldcomponent (or bare defprefix if 0) in readonly mode.
    // Custom TempPath/RuntimePath so it doesn't collide with the normal RUNTIME/WRITELAYER.
    ContainerParams ComparatorParams(PackagePath, "", oldcomponent_id);
    ContainerWrapper ComparatorContainer(*GlobalConfigJSON, *MANIFESTJSON, ComparatorParams);
    ComparatorContainer.ContainerParams.TempPath       = SessionTemp / "COMPARATOR_TEMP";
    ComparatorContainer.ContainerParams.DefPrefixPath  = SessionTemp / "COMPARATOR_TEMP" / "DEFPREFIX";
    ComparatorContainer.ContainerParams.RuntimePath    = SessionTemp / "COMPARATOR";
    ComparatorContainer.ContainerParams.ReadOnlyVFS    = true;
    ComparatorContainer.Cleanup();
    ComparatorContainer.BuildContainerRuntime();

    LogOut("PackageEditor::CompareComponentsRegistry", "Comparator initialized.");

    // Snapshot the baseline registry (HKLM+HKCU) from the mounted comparator VFS — before unmount.
    RegistryWrapper Baseline;
    Baseline.LoadPrefix(ComparatorContainer.ContainerParams.RuntimePath);

    ComparatorContainer.Cleanup();

    // Snapshot the "after" registry from WRITELAYER — changes COW'd there by a previous Execute().
    RegistryWrapper After;
    After.LoadPrefix(SessionTemp / "WRITELAYER");

    // One diff over both hives, already in manifest RegEdit shape (ignore-list + Wow6432Node→ARCH
    // applied inside DiffToRegEdits). Add/replace only — deletions are ignored.
    LogOut("PackageEditor::CompareComponentsRegistry", "Diffing registries.");
    nlohmann::ordered_json Delta = After.DiffToRegEdits(Baseline);
    LogOut("PackageEditor::CompareComponentsRegistry", "Delta: " + std::to_string(Delta.size()) + " RegEdit subcomponent(s).");

    if (!Delta.empty())
        MergeRegistryDeltaInComponent(&Delta, newcomponent_id);

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
    QString Dest = QDir::cleanPath(PackageDir->path() + QDir::separator() + DirName);
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
    QString Dest = QDir::cleanPath(PackageDir->path() + QDir::separator() + ZipName);
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
    QString Dest = QDir::cleanPath(PackageDir->path() + QDir::separator() + FileName);
    QFile::rename(Selected, Dest);
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "VFSFileLayer"}, {"PATH", FileName.toStdString()}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddEntrypoint()
{
    // The per-subgame "+ Add Variant" button is wired directly to an inline lambda that
    // captures `i` and pushes to SUBGAMES[i].VARIANTS. This slot is kept for header
    // consistency but should not normally be called.
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

//Adds a CustomVar subcomponent. CustomVars travel with their component but resolve into a bare global
//%KEY% namespace (no COMPONENTID prefix; resolved in recipe order, last wins — see
//ContainerWrapper::ResolveCustomVariables). The same KEY across version components is one shared knob.
void PackageEditor::AddCustomVar()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({
        {"TYPE", "CustomVar"}, {"KEY", ""}, {"LABEL", ""}, {"DEFAULT", ""}, {"VARTYPE", "string"}, {"DISPLAY", true}
    }));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

//Persistence subcomponents — declared at the point where a component introduces the state that
//should survive. Any one of these switches the package from whole-runtime persist (the default when
//none are present) to selective persistence (see ContainerWrapper::DerivePersistence).
void PackageEditor::AddPersistDir()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "PersistDir"}, {"PATH", ""}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddPersistFile()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "PersistFile"}, {"PATH", ""}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddRegPersist()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "RegPersist"}}));
    SaveManifestJSON(); RefreshJSONView(); BuildUI();
}

void PackageEditor::AddRegKeyPersist()
{
    QString JSONPath = ResolveComponentJSONPath(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
    (*MANIFESTJSON)[JSONPointer]["SUBCOMPONENTS"].push_back(json::object({{"TYPE", "RegKeyPersist"}, {"REGPATH", ""}}));
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
    if (SubgameIndex < (int)(*MANIFESTJSON)["GAMES"].size())
    {
        auto &IDField = (*MANIFESTJSON)["GAMES"][SubgameIndex]["GAMEID"];
        if (!IDField.is_null() && IDField.is_string()) SubgameID = std::string(IDField);
    }
    QString FileName = (SubgameID.empty() ? QString("subgame%1").arg(SubgameIndex) : QString::fromStdString(SubgameID))
                       + "_cover." + Extension.toLower();
    QString DestPath = QDir::cleanPath(PackageDir->path() + "/" + FileName);

    //Write the image file.
    QFile OutFile(DestPath);
    if (!OutFile.open(QIODevice::WriteOnly)) { LogErr("PackageEditor", "Could not write cover: " + DestPath.toStdString()); return; }
    OutFile.write(Data);
    OutFile.close();

    //Update MANIFEST METADATA.COVER.
    nlohmann::ordered_json::json_pointer CoverPtr(QString("/GAMES/%1/METADATA/COVER").arg(SubgameIndex).toStdString());
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
