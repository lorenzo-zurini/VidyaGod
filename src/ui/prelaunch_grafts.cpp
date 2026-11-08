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

void PreLaunchWindow::RebuildModuleTree()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    ModuleTree->clear();

    const Node* L = CurrentLaunch();
    if (!L) { SetTabShown(AddonsPage, false); return; }

    //The GRAFTS this row offers (a node whose list begins with ANY naming something the row contains), ticked by the
    //instance's saved graft list — else the ones RECOMMENDED under this tile (a fresh instance).
    const Shelf::GraftChoice Saved = SavedGrafts();
    //Offered with the ticked ones applied: a graft on a graft appears once the graft it needs is ticked.
    std::vector<std::string> PreTicked;
    std::vector<std::string> Offered = Shelf::OfferedGrafts(*Index, LaunchNodeId, &PreTicked, Saved, FaceUid);
    //A graft that presents a card of its own is not an add-on: on its card it is the game (always applied), and
    //elsewhere it is played from that card.
    const auto HasCard = [&](const std::string& G) {
        for (const auto& [U, Tg] : Index->TileGraft) if (Tg == G) return true;
        const Node* GN = Index->Find(G);
        return GN && !GN->Variant.empty();      // a graft that is a version is a row, not an add-on
    };
    Offered.erase(std::remove_if(Offered.begin(), Offered.end(), HasCard), Offered.end());
    //What the card's graft contains is always there with it (Forgotten Empires brings UserPatch): not a choice.
    for (const std::string& Forced : { TileGraft, RowGraft })
    {
        if (Forced.empty()) continue;
        const std::vector<std::string> In = ManifestModel::Closure(*Index, Forced);
        Offered.erase(std::remove_if(Offered.begin(), Offered.end(), [&](const std::string& G) {
            return std::find(In.begin(), In.end(), G) != In.end(); }), Offered.end());
    }
    //Ticked = what the launch applies: a saved graft it would drop (one another ticked graft excludes) shows unticked.
    std::vector<std::string> Ticked = PreTicked;
    if (Saved)
    {
        const auto [Instance, Builtins] = GraftJudging();
        Ticked = Shelf::AppliedGrafts(*Index, LaunchNodeId, *Saved, Instance, Builtins);
    }
    Ticked.erase(std::remove_if(Ticked.begin(), Ticked.end(), [&](const std::string& G) {
        return std::find(Offered.begin(), Offered.end(), G) == Offered.end(); }), Ticked.end());
    //One stable list in the default order, so ticking never moves a row: the ticked rows' places hold the ticked grafts
    //in the order they apply (a move swaps two ticked rows; with no move made that is the default order itself).
    //Grafts sit in their SECTION's row (collapsed); sections in the order their first graft is offered. Within a
    //section the ticked rows' places hold that section's ticked grafts in the order they apply, and the tree read
    //depth-first is the order the launch applies them (CollectGrafts).
    const auto SectionOf = [&](const std::string& G) {
        const Node* N = Index->Find(G);
        return N && N->Json->contains("SECTION") && (*N->Json)["SECTION"].is_string() ? (*N->Json)["SECTION"].get<std::string>() : std::string();
    };
    std::vector<std::string> SectionOrder;
    std::map<std::string, std::vector<std::string>> BySection;
    for (const std::string& G : Offered)
    {
        const std::string S = SectionOf(G);
        if (!BySection.count(S)) SectionOrder.push_back(S);
        BySection[S].push_back(G);
    }
    AppliedOrder = Ticked;   // the order they apply in (CollectGrafts); the tree below only displays them
    std::map<std::string, QTreeWidgetItem*> Sections;
    std::vector<std::pair<std::string, QTreeWidgetItem*>> Rows;
    for (const std::string& S : SectionOrder)
    {
        std::vector<std::string> Mine;
        for (const std::string& G : Ticked) if (SectionOf(G) == S) Mine.push_back(G);
        size_t Next = 0;
        for (const std::string& G : BySection[S])
            Rows.push_back({ std::find(Mine.begin(), Mine.end(), G) != Mine.end() ? Mine[Next++] : G, SectionRow(ModuleTree, Sections, S) });
    }
    for (const auto& [G, Parent] : Rows)
    {
        const Node* N = Index->Find(G);
        if (!N) continue;
        QTreeWidgetItem* It = new QTreeWidgetItem(Parent);
        It->setText(0, QString::fromStdString(N->NodeId.empty() ? N->Key().substr(0, 12) : N->NodeId));
        It->setData(0, Qt::UserRole,     QString::fromStdString(N->Key()));
        It->setData(0, Qt::UserRole + 2, std::find(Ticked.begin(), Ticked.end(), G) != Ticked.end());
    }
    SetTabShown(AddonsPage, !GraftRows(ModuleTree).empty());
    //Ordering means something only with two or more add-ons.
    const bool Several = GraftRows(ModuleTree).size() > 1;
    for (QWidget* X : { static_cast<QWidget*>(GraftUp), static_cast<QWidget*>(GraftDown),
                        static_cast<QWidget*>(AddonsPage->findChild<QLabel*>("graftOrderHint")) })
        if (X) X->setVisible(Several);
    SyncGraftChecks();
}

//Every row's check follows what is ticked (UserRole+2). Every row is tickable: what a graft needs is ticked with it.
void PreLaunchWindow::SyncGraftChecks()
{
    if (!ModuleTree) return;
    QSignalBlocker B(ModuleTree);
    for (QTreeWidgetItem* It : GraftRows(ModuleTree))
    {
        It->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        It->setCheckState(0, It->data(0, Qt::UserRole + 2).toBool() ? Qt::Checked : Qt::Unchecked);
    }
}

void PreLaunchWindow::PropagateModuleToggle(const QString& Key, bool Checked)
{
    if (!ModuleTree) return;
    QTreeWidgetItem* Item = nullptr;
    for (QTreeWidgetItem* It : GraftRows(ModuleTree)) if (It->data(0, Qt::UserRole).toString() == Key) { Item = It; break; }
    if (!Item) return;
    //The toggle arrives queued, and a rebuild can come between: judge the row as it is NOW, and a row already recorded
    //in that state is a duplicate or stale event (one emitted from a row since rebuilt) — acting on it would untick a
    //graft the user just ticked.
    const bool Now = Item->checkState(0) == Qt::Checked;
    if (Now == Item->data(0, Qt::UserRole + 2).toBool()) return;
    Checked = Now;
    Item->setData(0, Qt::UserRole + 2, Checked);
    if (Checked)
    {
        //The graft just ticked wins: those it excludes (either side's NOT) are unticked — exclusive grafts are a choice
        //of one — and it goes last in the ordered list.
        const std::string G = Key.toStdString();
        std::vector<std::string> Others = CollectGrafts();
        Others.erase(std::remove(Others.begin(), Others.end(), G), Others.end());
        const auto [Instance, Builtins] = GraftJudging();
        std::vector<std::string> List = Shelf::TickGraft(*Index, LaunchNodeId, Others, G, nullptr, Instance, Builtins);
        //Keep its row where it is: it applies after the ticked rows above it — unless it needs one below (then last).
        List.pop_back();
        size_t Above = 0;
        for (QTreeWidgetItem* Row : GraftRows(ModuleTree))
        {
            const std::string K = Row->data(0, Qt::UserRole).toString().toStdString();
            if (K == G) break;
            if (std::find(List.begin(), List.end(), K) != List.end()) ++Above;
        }
        std::vector<std::string> InPlace = List;
        InPlace.insert(InPlace.begin() + (long)std::min(Above, InPlace.size()), G);
        const std::vector<std::string> A = Shelf::AppliedGrafts(*Index, LaunchNodeId, InPlace, Instance, Builtins);
        if (std::find(A.begin(), A.end(), G) == A.end()) { InPlace = List; InPlace.push_back(G); }
        StoreGrafts(InPlace);
    }
    else SaveGrafts();
    RebuildModuleTree();
    RebuildCustomVarPickers();   // a graft brings its own options
    RefreshGraftEntryRows();     // a ticked mod loader adds a way to run; an unticked one takes it away
}

//The instance's graft lists are PER TILE: {"<tile UID>": [graft, …]}. Instance settings are the family's (RoC and TFT
//share one), so one list for the family meant a sibling tile's saved list replaced this tile's recommended pre-ticks,
//and saving on the sibling dropped the grafts only this tile offers.
std::optional<std::vector<std::string>> PreLaunchWindow::SavedGrafts() const
{
    const auto US = InstanceStore::ReadConfig(*GlobalConfigJSON, PackageUID, InstanceName);
    const std::string Tile = Face(CurrentLaunch());
    Shelf::GraftChoice Saved;
    if (US.contains("GRAFTS") && US["GRAFTS"].is_object() && US["GRAFTS"].contains(Tile) && US["GRAFTS"][Tile].is_array())
    {
        Saved.emplace();
        for (const auto& G : US["GRAFTS"][Tile]) if (G.is_string()) Saved->push_back(G.get<std::string>());
    }
    for (const std::string& Forced : { RowGraft, TileGraft })   // grafts on these are judged with them in place
        if (Saved && !Forced.empty() && std::find(Saved->begin(), Saved->end(), Forced) == Saved->end())
            Saved->insert(Saved->begin(), Forced);
    return Shelf::CurrentGraftChoice(*Index, Saved);
}

void PreLaunchWindow::StoreGrafts(const std::vector<std::string>& List)
{
    const auto US = InstanceStore::ReadConfig(*GlobalConfigJSON, PackageUID, InstanceName);
    nlohmann::ordered_json ByTile = US.contains("GRAFTS") && US["GRAFTS"].is_object() ? US["GRAFTS"] : nlohmann::ordered_json::object();
    ByTile[Face(CurrentLaunch())] = List;
    InstanceStore::SetSetting(*GlobalConfigJSON, PackageUID, "GRAFTS", ByTile, InstanceName);
}

void PreLaunchWindow::SaveGrafts()
{
    StoreGrafts(CollectGrafts());   //the tile's graft list = the ticked rows, in list order
}


//Grafts are judged as the launch judges them: the instance's values with what the pickers hold now merged over them
//(saved at launch), and the tile this window launches.
std::pair<std::map<std::string, std::string>, std::map<std::string, std::string>> PreLaunchWindow::GraftJudging() const
{
    std::map<std::string, std::string> Instance;
    if (const auto Saved = InstanceStore::Variables(*GlobalConfigJSON, PackageUID, InstanceName); Saved.is_object())
        for (const auto &[K, V] : Saved.items()) if (V.is_string()) Instance[K] = V.get<std::string>();
    if (CustomVarGroup) for (const auto &[K, V] : CollectVarValues(CustomVarGroup)) Instance[K] = V;
    return { Instance, { {"UID", Face(Index->Find(LaunchNodeId))}, {"PackageUID", PackageUID} } };
}

void PreLaunchWindow::MoveSelectedGraft(int By)
{
    const QTreeWidgetItem* It = ModuleTree ? ModuleTree->currentItem() : nullptr;
    if (!It || !It->data(0, Qt::UserRole + 2).toBool()) return;
    const std::string Key = It->data(0, Qt::UserRole).toString().toStdString();
    std::vector<std::string> Ticked = CollectGrafts();
    const auto At = std::find(Ticked.begin(), Ticked.end(), Key);
    if (At == Ticked.end()) return;
    std::string Why;
    const auto [Instance, Builtins] = GraftJudging();
    if (!Shelf::MoveGraft(*Index, LaunchNodeId, Ticked, (size_t)(At - Ticked.begin()), By, &Why, Instance, Builtins))
    {
        if (!Why.empty()) { GraftNote->setText(QString::fromStdString("Not moved: " + Why)); GraftNote->setVisible(true); }
        return;
    }
    GraftNote->setVisible(false);
    StoreGrafts(Ticked);
    RebuildModuleTree();                 // rows follow the saved order
    for (QTreeWidgetItem* Row : GraftRows(ModuleTree))
        if (Row->data(0, Qt::UserRole).toString().toStdString() == Key)
        {
            for (QTreeWidgetItem* P = Row->parent(); P; P = P->parent()) P->setExpanded(true);   // keep it in view
            ModuleTree->setCurrentItem(Row);
        }
    RebuildCustomVarPickers();
    RefreshGraftEntryRows();             // the entries grafts add follow the order too
}

//The ticked grafts in the order they APPLY — the list the last rebuild settled on (saved, or the tile's defaults),
//less any row unticked since. Not the tree's order: sections group grafts for display, and a graft in an earlier
//section can need one in a later section — read depth-first, it would be judged before what it needs and dropped.
std::vector<std::string> PreLaunchWindow::CollectGrafts() const
{
    std::vector<std::string> Out;
    if (!TileGraft.empty()) Out.push_back(TileGraft);      // the card's own graft applies first, always
    if (!RowGraft.empty()) Out.push_back(RowGraft);        // then the row's (a graft that is a version)
    if (!ModuleTree) return Out;
    std::set<std::string> TickedRows;
    for (QTreeWidgetItem* It : GraftRows(ModuleTree))
        if (It->data(0, Qt::UserRole + 2).toBool()) TickedRows.insert(It->data(0, Qt::UserRole).toString().toStdString());
    for (const std::string& G : AppliedOrder) if (TickedRows.count(G) && G != TileGraft && G != RowGraft) Out.push_back(G);
    return Out;
}

// Reads the current value of every var control (tagged CVKey) under `Group`, by widget type. Shared by the WHEN
// evaluator and the launch-time collector.
void PreLaunchWindow::RefreshGraftEntryRows()
{
    //Rows a ticked graft with entries adds for the CURRENT variant — appended after the variant rows (data with a
    //third field), removed and re-added on every tick change or variant change. Selection is preserved by data.
    QSignalBlocker B(VariantCombo);
    const QString Keep = VariantCombo->currentData().toString();
    for (int i = VariantCombo->count() - 1; i >= 0; --i)
        if (VariantCombo->itemData(i).toString().section(QChar(0x1f), 2, 2) == "graft") VariantCombo->removeItem(i);
    const Node* L = CurrentLaunch();
    if (L && Index)
    {
        //The entries the ticked grafts ADD to this row's fold (a mod loader), beyond the row's own.
        const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*Index), L->Key(), {}, {}, CollectGrafts());
        for (const auto& [Label, E] : P.Exec.items())
        {
            if (L->Entries.contains(Label)) continue;
            if (E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty()) continue;   // a runner entry
            if (E.contains("TILE")) continue;                                // it presents a card: played from there
            VariantCombo->addItem(QString::fromStdString("run " + Label),
                                  QString::fromStdString(LaunchNodeId + "\x1f" + Label + "\x1fgraft" + (RowGraft.empty() ? std::string() : "\x1f" + RowGraft)));
        }
    }
    int Sel = VariantCombo->findData(Keep);
    bool Vanished = false;
    if (Sel < 0)
    {
        // The selected row was a graft's entry and that graft was just unticked: fall back to the variant's own
        // rows and RE-READ the selection, or Entrypoint keeps naming a loader the mount no longer holds.
        Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId + "\x1f" + Entrypoint));
        if (Sel < 0) Sel = VariantCombo->findData(QString::fromStdString(LaunchNodeId), Qt::UserRole, Qt::MatchStartsWith);
        Vanished = Sel >= 0;
    }
    if (Sel >= 0) VariantCombo->setCurrentIndex(Sel);
    if (Vanished) onVariantChanged();
}
