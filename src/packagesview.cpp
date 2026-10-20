#include "packagesview.h"
#include "appmodel.h"          // AppModel — catalog/config + removePackage
#include "mainwindow.h"        // MainWindow::RefreshPackage (app-wide "package edited" hook)
#include "packageeditor.h"
#include "filesystemoperations.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "commonutils.h"
#include "asyncwork.h"
#include "ipfswrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <QPointer>

PackagesView::PackagesView(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    buildUi();
    connect(&Model, &AppModel::catalogChanged, this, &PackagesView::rebuildList);
    connect(&Model, &AppModel::nodeReady, this, &PackagesView::rebuildUpdates);   // versions need the node up
    rebuildList();
}

void PackagesView::buildUi()
{
    QVBoxLayout * layout = new QVBoxLayout(this);

    // (The "Add Local Package" + "Package Editor" actions moved to the Library tab's toolbar — see LibraryTab — so the
    // authoring entry points are discoverable on the main screen instead of buried in this settings subpage.)

    UpdatesBox = new QWidget(this);
    UpdatesBox->setLayout(new QVBoxLayout());
    UpdatesBox->layout()->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(UpdatesBox);

    PackagesScrollArea = new QScrollArea(this);
    PackagesScrollArea->setWidgetResizable(true);
    layout->addWidget(PackagesScrollArea);
}

void PackagesView::rebuildUpdates()
{
    //Rebuilt whole each time: a stale row (a switch applied meanwhile) must not linger, and only the LATEST refresh's
    //result is shown (two in flight would otherwise add two lists).
    const unsigned Gen = ++UpdatesGeneration;
    QLayout *L = UpdatesBox->layout();
    while (QLayoutItem *It = L->takeAt(0)) { if (It->widget()) It->widget()->deleteLater(); delete It; }
    if (!IpfsWrapper::Available() || !Model.config()) return;
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    auto Found = std::make_shared<std::vector<PackageCatalog::PackageUpdate>>();
    QPointer<QWidget> Box = UpdatesBox;
    AsyncWork::Run(this, [Cfg, Found]{ *Found = PackageCatalog::AvailableUpdates(*Cfg); },
        [this, Box, Found, Gen]{
            if (!Box || Gen != UpdatesGeneration || Found->empty()) return;
            QGroupBox *G = new QGroupBox("Other versions", Box);
            QGridLayout *Grid = new QGridLayout(G);
            int Row = 0;
            for (const auto &U : *Found)
            {
                Grid->addWidget(new QLabel(QString::fromStdString(U.Pkg + "  (" + U.Lib + ") — " + U.Nick + " has a different version"), G), Row, 0);
                QPushButton *B = new QPushButton(QString::fromStdString("Use " + U.Nick + "'s version"), G);
                B->setToolTip("Switch this package to " + QString::fromStdString(U.Nick) + "'s version of it. A part other "
                              "installed games contain stays for them; what goes is listed before anything changes.");
                B->setEnabled(!Switching);
                connect(B, &QPushButton::clicked, this, [this, U, B]{ offerUpdate(U, B); });
                Grid->addWidget(B, Row++, 1);
            }
            Box->layout()->addWidget(G);
        });
}

void PackagesView::offerUpdate(const PackageCatalog::PackageUpdate &U, QPushButton *Button)
{
    if (!Model.config() || Switching) return;
    Switching = true;
    for (auto *B : UpdatesBox->findChildren<QPushButton *>()) B->setEnabled(false);
    Button->setText("Fetching " + QString::fromStdString(U.Nick) + "'s version…");
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    auto S = std::make_shared<PackageCatalog::StagedUpdate>();
    auto Err = std::make_shared<std::string>();
    auto Ok = std::make_shared<bool>(false);
    auto Finish = [this]{ Switching = false; rebuildUpdates(); };
    AsyncWork::Run(this, [Cfg, U, S, Err, Ok]{ *Ok = PackageCatalog::StageUpdate(*Cfg, U, S.get(), Err.get()); },
        [this, U, S, Err, Ok, Finish]{
            if (!*Ok) { QMessageBox::warning(this, "Switch version", QString::fromStdString(*Err)); Finish(); return; }
            QString Q = QString("Switch %1 to %2's version?\n\n%3 node(s) come in and %4 go.")
                            .arg(QString::fromStdString(U.Pkg), QString::fromStdString(U.Nick))
                            .arg(S->Added.size()).arg(S->Removed.size());
            if (!S->Kept.empty())
                Q += QString(" %1 node(s) of the current version stay, because other installed games still use them.").arg(S->Kept.size());
            if (!S->Deletes.empty())
            {
                Q += QString("\n\nThese files are deleted (%1 MB); they are needed again only to switch back:")
                         .arg(S->DeleteBytes / (1024.0 * 1024.0), 0, 'f', 1);
                for (size_t I = 0; I < S->Deletes.size() && I < 12; ++I)
                    Q += "\n  " + QString::fromStdString(S->Deletes[I].File.filename().string());
                if (S->Deletes.size() > 12) Q += QString("\n  … and %1 more").arg(S->Deletes.size() - 12);
            }
            Q += "\n\nSaves and the files you keep are not touched. Choices saved for removed versions go back to their "
                 "defaults. New content downloads the next time it is needed.";
            if (QMessageBox::question(this, "Switch version", Q) != QMessageBox::Yes)
            {
                AsyncWork::Run(this, [S]{ PackageCatalog::DiscardStagedUpdate(*S); }, Finish);
                return;
            }
            auto Done = std::make_shared<bool>(false);
            auto Now = std::make_shared<const nlohmann::ordered_json>(*Model.config());   // the library as it is now
            AsyncWork::Run(this, [Now, S, Err, Done]{ *Done = PackageCatalog::ApplyStagedUpdate(*Now, *S, Err.get()); },
                [this, Err, Done, Finish]{
                    if (!*Done) QMessageBox::warning(this, "Switch version", QString::fromStdString(*Err));
                    Switching = false;
                    Model.rebuildCatalog();                                    // the package's nodes changed → rebuildList
                    (void)Finish;
                });
        });
}

void PackagesView::rebuildList()
{
    rebuildUpdates();
    if (PackagesScrollArea->widget()) PackagesScrollArea->widget()->deleteLater();
    QWidget * w = new QWidget(PackagesScrollArea);
    PackagesScrollArea->setWidget(w);
    QGridLayout * g = new QGridLayout(w); w->setLayout(g);
    int row = 0;
    auto & Lib = (*Model.config())["LIBRARY"];

    //Which bundle dirs have a HYDRATED launchable node — computed in ONE pass over the already-built catalog index.
    //Previously this re-parsed every package's JSON from disk per LIBRARY entry (ScanBundleNodes in the loop), which
    //was O(packages × nodes) of parsing on the main thread and a real chunk of the launch freeze. The index already
    //holds every node with its BundleDir, so filter that instead. Paths are lexically normalized so a trailing-slash /
    //separator difference between a node's BundleDir and the LIBRARY PATH doesn't cause a false miss.
    const NodeIndex & Idx = Model.catalogIndex();
    auto Norm = [](const std::filesystem::path & P){ return P.lexically_normal().generic_string(); };
    // ONE HydrationMap pass (O(N+E), stat-cached) — the per-launchable NodeHasContent+NodeHydrated pair re-derived
    // CustomVarDefaults over the WHOLE graph and re-walked the closure per node: O(N²) + per-node disk stats ON THE
    // GUI THREAD, measured as a top contributor to the startup freeze. Same map the Library/Catalog tabs use.
    const auto Hyd = PackageCatalog::HydrationMap(Idx);
    std::set<std::string> HydratedBundles;
    for (const auto & [NodeId, N] : Idx.Nodes)
    {
        if (!N.IsVariant() || N.BundleDir.empty()) continue;
        const auto It = Hyd.find(NodeId);
        if (It != Hyd.end() && It->second.HasContent && It->second.Hydrated)
            HydratedBundles.insert(Norm(N.BundleDir));
    }

    for (int i = 0; i < (int)Lib.size(); i++) {
        //Only HYDRATED packages (content present locally) are listed — synced-but-not-downloaded repo entries
        //live in the Available tab, not here. Content-less packages (a malformed game with no layers, or a
        //PATH-only runner) are vacuously hydrated, so require real content to exclude them.
        const std::string LibPath = Lib[i].value("PATH", std::string());
        //Only HYDRATED bundles with a launchable node appear here (a runner-only bundle has none).
        if (LibPath.empty() || !HydratedBundles.count(Norm(LibPath))) continue;   // require REAL content present
        g->addWidget(new QLabel(QString::fromStdString(Lib[i].value("PACKAGENAME", std::string())),w),row,0);
        g->addWidget(new QLabel(QString::fromStdString(Lib[i].value("PACKAGEUID", std::string())),w),row,1);
        QPushButton * rb = new QPushButton("Remove", w);
        // Key removal by PACKAGEUID, resolved at click time — robust against the array shifting under us.
        const QString Uid = QString::fromStdString(Lib[i].value("PACKAGEUID", std::string()));
        connect(rb, &QPushButton::clicked, this, [this, Uid]{ Model.removePackage(Uid); });   // model deletes + rebuilds + emits
        g->addWidget(rb, row, 2);
        row++;
    }
    if (row == 0)
    {
        QLabel * none = new QLabel("No installed packages yet - download packages from the Catalog tab or add local packages.", w);
        none->setStyleSheet("color:#8f98a0;");
        g->addWidget(none, 0, 0, 1, 3);
    }
    g->setRowStretch(g->rowCount(), 1);
}

// (addLocalPackages moved to AppModel::importPackagesFromDir, invoked from the Library tab's toolbar.)
