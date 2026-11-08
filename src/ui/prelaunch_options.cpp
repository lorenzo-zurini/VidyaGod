#include "guiformat.h"   // PlainLabel
#include "installed.h"
#include "theme.h"   // the named colours
#include "shelf.h"
#include "prelaunchwindow.h"
#include "fold.h"   // a row's options and entries are its resolution's
#include "apppaths.h"
#include "packageeditor.h"
#include "mainwindow.h"
#include "covercache.h"
#include "packagecatalog.h"
#include "containerwrapper.h"   // StringVariableSubstitution / ContainerParams (CustomVar preview substitution)
#include "launchsession.h"     // ResolveTakingBack — what the game changed, read back before the values show
#include "launchresolver.h"     // ResolveChainIds / ResolveChainTail / kNativeTerminalId (runner daisy-chain UI)
#include "jsonoperations.h"
#include "ipfswrapper.h"     // LanPeers/SetLanExcluded/LanLaunchVars — the Virtual LAN panel
#include "variantpicker.h"     // NaturalLess — deterministic version-aware variant ordering
#include "instancestore.h"     // the instance picker
#include "asyncwork.h"        // the size under the cover, counted off the GUI thread

#include <set>
#include <algorithm>

#include <QBrush>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFont>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QDoubleSpinBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QGridLayout>
#include <QTreeWidgetItemIterator>
#include <QHeaderView>
#include <QGuiApplication>
#include <QApplication>
#include <QScrollBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include "keycapture.h"
#include <QPainterPath>
#include <QUrl>
#include <QDateTime>
#include <QLocale>
#include <QPointer>

#include "prelaunchwindow_p.h"

namespace {
//A section with nothing visible in it (every option gated off by its WHEN) hides too.
bool HideEmptySections(QTreeWidgetItem* I)
{
    bool Any = false;
    for (int c = 0; c < I->childCount(); ++c)
    {
        QTreeWidgetItem* C = I->child(c);
        if (C->data(0, Qt::UserRole + 10).isValid()) { const bool V = HideEmptySections(C); C->setHidden(!V); Any |= V; }
        else Any |= !C->isHidden();
    }
    return Any;
}
}

std::map<std::string, std::string> PreLaunchUi::CollectVarValues(QObject* Group)
{
    std::map<std::string, std::string> M;
    for (QWidget* W : Group->findChildren<QWidget*>())
    {
        const QString K = W->property("CVKey").toString();
        if (K.isEmpty()) continue;
        std::string V;
        if (auto* Kc = qobject_cast<KeyCaptureField*>(W))   V = Kc->value();
        else if (auto* C = qobject_cast<QComboBox*>(W))     V = C->currentData().toString().toStdString();
        else if (auto* S = qobject_cast<QSpinBox*>(W))      V = std::to_string(S->value());
        else if (auto* D = qobject_cast<QDoubleSpinBox*>(W))V = QString::number(D->value()).toStdString();
        else if (auto* B = qobject_cast<QCheckBox*>(W))     V = B->isChecked() ? "1" : "0";
        else if (auto* E = qobject_cast<QLineEdit*>(W))     V = E->text().toStdString();
        else continue;
        M[K.toStdString()] = V;
    }
    return M;
}

// Which var KEYs are ACTIVE (their WHEN holds) given the current control values. A gated-off var contributes an
// empty value, so a chain (NETMODE gates ADDR gates X) is resolved to a fixpoint — mirrors the resolver exactly,
// via the same VarSubst::EvaluateCondition. Rows without a CVWhen are always active.
static std::set<std::string> ActiveVarKeys(QObject* Group, const std::map<std::string, std::string>& Fixed)
{
    // Each gated row's WHEN, by its KEY (the row's label and control both carry them).
    std::map<std::string, std::string> KeyWhen;
    for (QWidget* Cell : Group->findChildren<QWidget*>())
    {
        const QVariant W = Cell->property("CVWhen");
        const QString K = Cell->property("CVRowKey").toString();
        if (W.isValid() && !K.isEmpty()) KeyWhen[K.toStdString()] = W.toString().toStdString();
    }
    //The options' values over the package's fixed facts (vars with no control): a WHEN reading `%h3_game%` used to
    //see it empty, and Horn of the Abyss's own option hid on its own card.
    std::map<std::string, std::string> Vals = Fixed;
    for (const auto& [K, V] : CollectVarValues(Group)) Vals[K] = V;
    std::set<std::string> Active;
    for (const auto& [K, V] : Vals) Active.insert(K);
    for (int Pass = 0; Pass < 8; ++Pass)   // fixpoint: a gated-off var reads as empty for the next round
    {
        bool Changed = false;
        std::map<std::string, std::string> Eff = Vals;
        for (const auto& K : Vals) if (!Active.count(K.first)) Eff[K.first] = "";
        for (const auto& [K, When] : KeyWhen)
        {
            const bool On = VarSubst::EvaluateCondition(When, Eff);
            if (!On && Active.count(K)) { Active.erase(K); Changed = true; }
            else if (On && !Active.count(K)) { Active.insert(K); Changed = true; }
        }
        if (!Changed) break;
    }
    return Active;
}

void PreLaunchWindow::EvaluateVarConditions()
{
    const std::set<std::string> Active = ActiveVarKeys(CustomVarGroup, FixedVarValues);
    for (QWidget* Cell : CustomVarGroup->findChildren<QWidget*>())
    {
        if (!Cell->property("CVWhen").isValid()) continue;                      // a gated row's control carries it
        const std::string Key = Cell->property("CVRowKey").toString().toStdString();
        if (auto* Row = reinterpret_cast<QTreeWidgetItem*>(Cell->property("CVItem").value<quintptr>()))
            Row->setHidden(!(Key.empty() || Active.count(Key) > 0));
    }
    if (OptionsTree) HideEmptySections(OptionsTree->invisibleRootItem());
}

//What the game changed in its own settings since the last launch becomes the instance's value before the controls show
//it (OptionReadBack, through the launch's own resolve): the window opens on the game's settings, and Launch writes them
//back unchanged. Nothing is mounted or launched.
void PreLaunchWindow::ReadBackFromFiles()
{
    if (PackageUID.empty() || !Index) return;
    //Once per row and instance shown: a graft tick or a chain change does not make the game's files say anything new.
    const std::string Key = InstanceName + "|" + LaunchNodeId + "|" + RowGraft;
    if (Key == ReadBackDoneFor) return;
    ReadBackDoneFor = Key;
    ContainerParams Cp{std::filesystem::path(BundleDir)};
    Cp.NodeIdx         = Index;
    Cp.LaunchNodeId    = RowGraft.empty() ? LaunchNodeId : RowGraft;
    Cp.GraftBase       = RowGraft.empty() ? std::string() : LaunchNodeId;
    Cp.Entrypoint      = Entrypoint;
    Cp.LaunchFace      = FaceUid;
    Cp.InstanceName    = InstanceName;
    Cp.RunnerChainIds  = CurrentChain;
    Cp.Grafts          = CollectGrafts();
    (void)LaunchSession::ResolveTakingBack(Cp, *GlobalConfigJSON, /*Reresolve=*/false);
}

void PreLaunchWindow::RebuildCustomVarPickers()
{
    //clear() frees the rows at once but only deleteLater()s their controls — which still carry CVWhen/CVItem (a
    //pointer to the freed row) until the event loop runs, and the condition pass below walks them in this very call.
    //So the controls go first, synchronously (their pending deleteLater is dropped with them).
    std::vector<QWidget*> OldControls;
    for (QTreeWidgetItemIterator It(OptionsTree); *It; ++It)
        if (QWidget* W = OptionsTree->itemWidget(*It, 1)) OldControls.push_back(W);
    OptionsTree->clear();
    FixedVarValues.clear();
    for (QWidget* W : OldControls) delete W;
    const Node* L = CurrentLaunch();
    if (!L) { SetTabShown(OptionsPage, false); return; }
    ReadBackFromFiles();

    // The knobs are the FOLDED declarations: the row's (with the ticked grafts), then every chain runner's.
    const Fold::Library Lib = ManifestModel::LibraryOf(*Index);
    std::vector<std::pair<nlohmann::ordered_json, bool>> Decls;   // (a CustomVar-shaped declaration, isRunnerKnob)
    auto AddDecls = [&](const Fold::Plan& P, bool IsRunner) {
        for (const auto& [K, D] : P.Decls.items())
        {
            nlohmann::ordered_json CV = { {"TYPE", "CustomVar"}, {"KEY", K} };
            if (D.is_object()) for (const auto& [F, X] : D.items()) CV[F] = X;
            Decls.push_back({ std::move(CV), IsRunner });
        }
    };
    AddDecls(Fold::Resolve(Lib, L->Key(), {}, {}, CollectGrafts()), false);
    for (const std::string& Rid : CurrentChain)
    {
        if (Rid == LaunchResolver::kNativeTerminalId || !Index->Find(Rid)) continue;
        AddDecls(Fold::Resolve(Lib, Rid), true);
    }

    const nlohmann::ordered_json SavedVars = InstanceStore::Variables(*GlobalConfigJSON, PackageUID, InstanceName);

    // Sections (UI.SECTION paths; a runner's options under "Runner"), created as their first option appears.
    std::map<std::string, QTreeWidgetItem*> Sections;

    bool AnyVisible = false, AnyCond = false;
    struct PendingRow { std::string Section; QString Label; QWidget* Field; };
    std::vector<PendingRow> Rows;     // placed once all are known: loose options above the sections
    std::set<std::string> SeenKeys;   // a KEY surfaces once; package nodes walked first so they win on collision

    for (const auto& [CVj, IsRunner] : Decls)
    {
        {
            const nlohmann::ordered_json& CV = CVj;
            if (!CV.is_object() || CV.value("TYPE", std::string()) != "CustomVar") continue;
            const std::string Key = CV.value("KEY", std::string());
            if (!CV.contains("UI") || !CV["UI"].is_object())                  // only UI-facet vars are shown...
            {   //...but a fixed fact (the game a package is, aoe2_game, h3_game) is what an option's WHEN may read
                if (!Key.empty() && !FixedVarValues.count(Key)) FixedVarValues[Key] = CV.value("DEFAULT", std::string());
                continue;
            }
            if (Key.empty() || SeenKeys.count(Key)) continue;
            const nlohmann::ordered_json& UI = CV["UI"];
            const std::string Control = UI.value("CONTROL", std::string("text"));

            std::string Initial = CV.value("DEFAULT", std::string());
            if (SavedVars.contains(Key) && SavedVars[Key].is_string()) Initial = std::string(SavedVars[Key]);
            //Kept in the format's spelling (%% stays %%): the value is saved and passed as an override, both read as
            //format text again — written out here, each launch would strip one more level.
            { ContainerParams TmpP; VarSubst::StringVariableSubstitution(Initial, TmpP.GetVariablesMap(), /*KeepEscapes=*/true); }

            const QString Label = QString::fromStdString(UI.value("LABEL", Key));
            QWidget* Field = nullptr;       // the value control (carries CVKey); null for an uneditable secret pool

            if (Control == "enum" && UI.contains("CHOICES") && UI["CHOICES"].is_array())
            {
                QComboBox* Combo = new QComboBox(CustomVarGroup);
                //A choice is either {LABEL, VALUE} or the bare-string shorthand, where the string is both.
                //`Opt.value(...)` on a string THROWS, and this runs inside a dialog with no catch above it,
                //so the shorthand — the obvious thing to write — took the whole app down for every game whose
                //closure pulled that shared CustomVar in.
                for (const auto& Opt : UI["CHOICES"])
                {
                    if (Opt.is_string())
                    {
                        const QString S = QString::fromStdString(Opt.get<std::string>());
                        Combo->addItem(S, S);
                    }
                    else if (Opt.is_object())
                        Combo->addItem(QString::fromStdString(Opt.value("LABEL", std::string())),
                                       QString::fromStdString(Opt.value("VALUE", std::string())));
                }
                for (int k = 0; k < Combo->count(); k++)
                    if (Combo->itemData(k).toString().toStdString() == Initial) { Combo->setCurrentIndex(k); break; }
                connect(Combo, &QComboBox::currentIndexChanged, this, [this](int){ EvaluateVarConditions(); });
                Field = Combo;
            }
            else if (Control == "bool")
            {
                QCheckBox* Check = new QCheckBox(CustomVarGroup);
                std::string lo = Initial; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                Check->setChecked(lo == "1" || lo == "true" || lo == "yes");
                connect(Check, &QCheckBox::toggled, this, [this](bool){ EvaluateVarConditions(); });
                Field = Check;
            }
            else if (Control == "int")
            {
                QSpinBox* Spin = new QSpinBox(CustomVarGroup);
                Spin->setRange(UI.value("MIN", -2147483647), UI.value("MAX", 2147483647));
                int IV = 0; try { IV = std::stoi(Initial); } catch (...) { IV = Spin->minimum(); }
                Spin->setValue(IV);
                connect(Spin, &QSpinBox::valueChanged, this, [this](int){ EvaluateVarConditions(); });
                Field = Spin;
            }
            else if (Control == "float")
            {
                QDoubleSpinBox* Spin = new QDoubleSpinBox(CustomVarGroup);
                Spin->setRange(UI.value("MIN", -1e12), UI.value("MAX", 1e12));
                double DV = 0.0; try { DV = std::stod(Initial); } catch (...) { DV = Spin->minimum(); }
                Spin->setValue(DV);
                connect(Spin, &QDoubleSpinBox::valueChanged, this, [this](double){ EvaluateVarConditions(); });
                Field = Spin;
            }
            else if (Control == "secret")
            {
                // Always editable and always collected — a POOL is only a SEED for the first launch. `Initial` is
                // already the persisted value when there is one; otherwise draw one pool entry so the field is
                // pre-filled and gets persisted on launch like any other value. The user can overwrite it (their
                // own CD key, account name, …) and that choice then sticks.
                QLineEdit* Edit = new QLineEdit(CustomVarGroup);
                Edit->setEchoMode(QLineEdit::Password);
                if (Initial.empty() && UI.contains("POOL") && UI["POOL"].is_array() && !UI["POOL"].empty())
                {
                    const auto& Pool = UI["POOL"];
                    const size_t Pick = static_cast<size_t>(QRandomGenerator::global()->bounded(int(Pool.size())));
                    if (Pool[Pick].is_string()) Initial = Pool[Pick].get<std::string>();
                    Edit->setPlaceholderText("from pool — edit to use your own");
                }
                Edit->setText(QString::fromStdString(Initial));
                Field = Edit;
            }
            else if (Control == "key")
            {
                std::string Default = CV.value("DEFAULT", std::string());
                { ContainerParams TmpP; VarSubst::StringVariableSubstitution(Default, TmpP.GetVariablesMap()); }
                auto* KeyField = new KeyCaptureField(Initial, Default, CustomVarGroup);
                connect(KeyField, &KeyCaptureField::changed, this, [this] { EvaluateVarConditions(); MarkKeyConflicts(); });
                Field = KeyField;
            }
            else // text
            {
                QLineEdit* Edit = new QLineEdit(CustomVarGroup);
                Edit->setText(QString::fromStdString(Initial));
                if (UI.contains("PATTERN") && UI["PATTERN"].is_string())
                    Edit->setValidator(new QRegularExpressionValidator(
                        QRegularExpression(QString::fromStdString(std::string(UI["PATTERN"]))), Edit));
                connect(Edit, &QLineEdit::textChanged, this, [this](const QString&){ EvaluateVarConditions(); });
                Field = Edit;
            }

            if (!Field) continue;
            if (!qobject_cast<QLabel*>(Field))                                   // editable controls are collected
                Field->setProperty("CVKey", QString::fromStdString(Key));
            SeenKeys.insert(Key); AnyVisible = true;

            std::string Section = UI.value("SECTION", std::string());
            if (IsRunner) Section = Section.empty() ? std::string("Runner") : "Runner/" + Section;
            Field->setProperty("CVSection", QString::fromStdString(Section));
            Field->setProperty("CVLabel", Label);
            //A value control keeps a readable width rather than spanning the window; a tick box is as wide as itself.
            if (!qobject_cast<QCheckBox*>(Field)) Field->setMaximumWidth(340);
            //WHEN gates this row's visibility (and, in the resolver, its value); the control carries it with the row's
            //key. Top-level preferred; UI.WHEN is a UI-era alias.
            std::string When = CV.value("WHEN", std::string());
            if (When.empty()) When = UI.value("WHEN", std::string());
            if (!When.empty())
            {
                Field->setProperty("CVWhen", QString::fromStdString(When));
                Field->setProperty("CVRowKey", QString::fromStdString(Key));
                AnyCond = true;
            }
            Rows.push_back({ Section, Label, Field });
        }
    }
    //Loose options first, then the sections (each in the order its first option appeared).
    std::stable_sort(Rows.begin(), Rows.end(), [](const PendingRow& A, const PendingRow& B) { return A.Section.empty() && !B.Section.empty(); });
    for (const PendingRow& R : Rows)
    {
        auto* Row = new QTreeWidgetItem(SectionRow(OptionsTree, Sections, R.Section));
        Row->setText(0, R.Label);
        Row->setFlags(Qt::ItemIsEnabled);
        OptionsTree->setItemWidget(Row, 1, R.Field);
        R.Field->setProperty("CVItem", QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(Row)));
    }

    if (AnyCond) EvaluateVarConditions();                                       // apply initial WHEN visibility
    MarkKeyConflicts();
    SetTabShown(OptionsPage, AnyVisible);
    //The label column fits the longest label at its depth — measured over every row, open or folded, so opening a
    //section never truncates what it shows — within reason; the controls take the rest.
    {
        const QFontMetrics Fm(OptionsTree->font());
        QFont Bold = OptionsTree->font(); Bold.setBold(true);
        const QFontMetrics FmB(Bold);
        int Need = 0;
        for (QTreeWidgetItemIterator It(OptionsTree); *It; ++It)
        {
            int Depth = 0;
            for (QTreeWidgetItem* P = (*It)->parent(); P; P = P->parent()) ++Depth;
            const bool Sec = (*It)->data(0, Qt::UserRole + 10).isValid();
            Need = std::max(Need, (Depth + 1) * OptionsTree->indentation() + (Sec ? FmB : Fm).horizontalAdvance((*It)->text(0)) + 24);
        }
        OptionsTree->setColumnWidth(0, std::clamp(Need, 160, 440));
    }
}

// Two key options of one section bound to the same key: one press would do both. Each is marked with the other's label.
void PreLaunchWindow::MarkKeyConflicts()
{
    std::map<std::pair<QString, std::string>, std::vector<KeyCaptureField*>> Bound;    // (section, key) → its fields
    const auto Fields = OptionsTree->findChildren<KeyCaptureField*>();
    for (KeyCaptureField* F : Fields)
        if (!F->value().empty()) Bound[{ F->property("CVSection").toString(), F->value() }].push_back(F);
    for (KeyCaptureField* F : Fields)
    {
        QStringList Others;
        if (!F->value().empty())
            for (KeyCaptureField* O : Bound[{ F->property("CVSection").toString(), F->value() }])
                if (O != F) Others << O->property("CVLabel").toString();
        F->setConflict(Others.join(", "));
    }
}
