#include "runnerspage.h"
#include "installed.h"
#include "appmodel.h"          // AppModel — catalog + importRunner
#include "libraryview.h"       // IpfsFetchReady()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "asyncwork.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPointer>
#include <QTreeWidget>
#include <QRegularExpression>
#include <QLabel>
#include <QPushButton>
#include <QFrame>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
//"ares_v148_linux64_exec" -> family "ares", shown as "ares 148 (linux64)"; "geproton_10_34" -> "geproton", "geproton 10.34".
//The words up to the first number are the family; the numbers its version; what follows (a platform) in brackets.
std::pair<QString, QString> FamilyAndName(const std::string &Id)
{
    QStringList W = QString::fromStdString(Id).split('_', Qt::SkipEmptyParts);
    if (!W.isEmpty() && W.back() == "exec") W.removeLast();
    QStringList Family, Version, Rest;
    static const QRegularExpression Platform("^(linux|win|mac|darwin)(32|64)?$");
    for (int I = 0; I < W.size(); ++I)
    {
        const QString &X = W[I];
        const bool Num = !X.isEmpty() && (X[0].isDigit() || (X.size() > 1 && X[0] == 'v' && X[1].isDigit()));
        if (I == 0) Family << X;                                          // "2ship": a name, though it starts with a digit
        else if (Platform.match(X).hasMatch()) Rest << X;                 // the platform is never part of the family
        else if (Num && Rest.isEmpty()) Version << (X[0] == 'v' ? X.mid(1) : X);
        else if (Version.isEmpty() && Rest.isEmpty()) Family << X;
        else Rest << X;
    }
    if (Family.isEmpty()) Family = W.mid(0, 1);
    QString Name = Family.join(' ');
    if (!Version.isEmpty()) Name += " " + Version.join('.');
    if (!Rest.isEmpty()) Name += " (" + Rest.join(' ') + ")";
    return { Family.join(' '), Name };
}
} // namespace

RunnersPage::RunnersPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(8,8,8,8);
    QLabel * intro = new QLabel(
        "Runners are packages from your repositories. A runner that ships its own build (e.g. Proton over "
        "IPFS) must be downloaded before games can use it.", this);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(intro);
    Search = new QLineEdit(this);
    Search->setPlaceholderText("Search runners (name, family, platform)…");
    Search->setClearButtonEnabled(true);
    Search->setAccessibleName("Search runners");
    connect(Search, &QLineEdit::textChanged, this, &RunnersPage::filter);
    pl->addWidget(Search);
    Table = new QTreeWidget(this);
    Table->setAccessibleName("Runners");
    Table->setColumnCount(5);
    Table->setHeaderLabels({ "Runner", "Runs", "Status", "Games", "" });
    Table->setRootIsDecorated(true);
    Table->setUniformRowHeights(true);
    Table->setAlternatingRowColors(true);
    Table->setSelectionMode(QAbstractItemView::NoSelection);
    Table->header()->setStretchLastSection(false);
    Table->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    Table->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    Table->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    Table->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    pl->addWidget(Table, 1);

    connect(&Model, &AppModel::catalogChanged, this, &RunnersPage::rebuild);
    rebuild();
}

void RunnersPage::filter(const QString &Text)
{
    for (int F = 0; F < Table->topLevelItemCount(); ++F)
    {
        QTreeWidgetItem * Fam = Table->topLevelItem(F);
        int Shown = 0;
        for (int I = 0; I < Fam->childCount(); ++I)
        {
            QTreeWidgetItem * R = Fam->child(I);
            const bool Hit = Text.isEmpty() || Fam->text(0).contains(Text, Qt::CaseInsensitive) || R->text(0).contains(Text, Qt::CaseInsensitive)
                             || R->text(1).contains(Text, Qt::CaseInsensitive) || R->data(0, Qt::UserRole).toString().contains(Text, Qt::CaseInsensitive);
            R->setHidden(!Hit);
            Shown += Hit;
        }
        Fam->setHidden(Shown == 0);
    }
}

void RunnersPage::rebuild()
{
    if (!Table) return;
    Table->clear();
    const unsigned Gen = ++Generation;

    //Runners are the DeclareRunner-carrying nodes of the global catalog graph (Node::IsRunner() — there is no ROLE
    //field). An EMBEDDED runner (bundled inside a game package) is hidden until installed — it's offered in that
    //game's download dialog instead; global runners always show (installed, or to download from here).
    std::shared_ptr<const NodeIndex> Snap = Model.catalogSnapshot();
    const NodeIndex & Idx = *Snap;
    std::vector<const Node*> Runners;
    for (const auto & [Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        if (Installed::IsEmbeddedRunner(Idx, N.NodeId) && !Installed::RunnerInstalled(Idx, N.NodeId)) continue;
        Runners.push_back(&N);
    }
    std::sort(Runners.begin(), Runners.end(), [](const Node* A, const Node* B){ return A->NodeId < B->NodeId; });
    // SAME fetch-based hydration model as the Catalog/Library tabs — a runner is no different from a game package:
    // its state is read from the hydration map (build content fetched?), not a bespoke runner-only predicate.
    const auto Hyd = Model.hydration();   // once per rebuild, shared with the Library and the Catalog
    if (Runners.empty())
    {
        auto * None = new QTreeWidgetItem(Table, { "No runners found in your repositories." });
        None->setForeground(0, QColor(0x8f, 0x98, 0xa0));
        return;
    }
    std::map<QString, QTreeWidgetItem *> Families;
    std::map<std::string, QTreeWidgetItem *> Rows;   // runner key -> its row (for the games count)
    for (const Node * R : Runners)
    {
        const std::string rid = R->NodeId;
        const auto [Family, Name] = FamilyAndName(rid);
        QTreeWidgetItem *& Fam = Families[Family];
        if (!Fam)
        {
            Fam = new QTreeWidgetItem(Table, { Family });
            QFont F = Fam->font(0); F.setBold(true); Fam->setFont(0, F);
            Fam->setFirstColumnSpanned(false);
        }
        QStringList Guests;
        for (const auto & P : R->GuestPlatform) Guests << (P == "custom" ? QString("its own game files") : QString::fromStdString(P));
        QString Runs = QString::fromStdString(R->HostPlatform) + " → ";
        Runs += Guests.size() > 6 ? Guests.mid(0, 6).join(", ") + QString(", … (%1 in all)").arg(Guests.size()) : Guests.join(", ");

        const bool Avail = RunnerWrapper::ExecutableAvailable(R->Exec);
        auto Hit = Hyd.find(R->Key());
        const bool HasContent = (Hit != Hyd.end()) && Hit->second.HasContent;   // ships a downloadable build (== a game's content)
        const bool Hydrated   = (Hit == Hyd.end()) || Hit->second.Hydrated;     // build fully fetched (fetch-based, same as games)
        // A runner is READY once its build content is hydrated — identical to a game being in the Library rather than the
        // Catalog. Build not yet fetched → offer Download (the same download pump). A runner with no downloadable build
        // is a system/built-in runner.
        QString Status; QColor Colour;
        if (!HasContent && Avail)   { Status = "built-in";                 Colour = QColor(0x8f, 0x98, 0xa0); }
        else if (!HasContent)       { Status = "not available on system";  Colour = QColor(0xc0, 0x72, 0x6a); }
        else if (Hydrated)          { Status = "Ready";                    Colour = QColor(0x5f, 0xb5, 0x5f); }
        else                        { Status = "Not downloaded";           Colour = QColor(0xc0, 0x72, 0x6a); }
        auto * Row = new QTreeWidgetItem(Fam, { Name, Runs, Status, "…" });
        Row->setData(0, Qt::UserRole, QString::fromStdString(rid));   // its id: searchable, and its tooltip
        Row->setToolTip(0, QString::fromStdString(rid));
        Row->setToolTip(1, Guests.join(", "));
        Row->setForeground(2, Colour);
        Row->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        Rows[R->Key()] = Row;
        if (HasContent && !Hydrated)
        {
            QPushButton * btn = new QPushButton("Download", Table);
            btn->setAccessibleName("Download " + Name);
            connect(btn, &QPushButton::clicked, this, [this, rid, btn, Row]{
                if (!IpfsFetchReady(this)) return;              // need the embedded node online to fetch
                btn->setEnabled(false); btn->setText("Downloading…");
                Row->setText(2, "Downloading… (see IPFS tab)");
                Row->setForeground(2, QColor(0xc6, 0xa1, 0x5f));
                Model.importRunner(QString::fromStdString(rid));   // → download pump; catalogChanged rebuilds this page
            });
            Table->setItemWidget(Row, 4, btn);
        }
    }
    for (auto & [F, Item] : Families) Item->setText(1, QString("%1 build%2").arg(Item->childCount()).arg(Item->childCount() == 1 ? "" : "s"));
    Table->expandAll();
    filter(Search->text());

    //How many games each can run (CompatibleRunners of every version on the shelf): a pass over the library, off the
    //GUI thread; shown when it lands, if this is still the latest table.
    auto Count = std::make_shared<std::map<std::string, int>>();
    QPointer<RunnersPage> Self(this);
    AsyncWork::Run(this, [Snap, Count]{
            for (const auto & [Id, N] : Snap->Nodes)
            {
                (void)Id;
                if (!N.IsVariant() || N.IsGraft || N.Received) continue;
                for (const Node * R : Installed::CompatibleRunners(*Snap, N)) ++(*Count)[R->Key()];
            }
        },
        [Self, Gen, Count, Rows]{
            if (!Self || Gen != Self->Generation) return;
            for (const auto & [Key, Row] : Rows)
            {
                const auto It = Count->find(Key);
                Row->setText(3, QString::number(It == Count->end() ? 0 : It->second));
            }
        });
}
