#include "packagesview.h"
#include "appmodel.h"          // AppModel — catalog/config + removePackage
#include "mainwindow.h"        // MainWindow::RefreshPackage (app-wide "package edited" hook)
#include "packageeditor.h"
#include "filesystemoperations.h"
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "commonutils.h"

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
#include <string>

PackagesView::PackagesView(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    buildUi();
    connect(&Model, &AppModel::catalogChanged, this, &PackagesView::rebuildList);
    rebuildList();
}

void PackagesView::buildUi()
{
    QVBoxLayout * layout = new QVBoxLayout(this);

    // (The "Add Local Package" + "Package Editor" actions moved to the Library tab's toolbar — see LibraryTab — so the
    // authoring entry points are discoverable on the main screen instead of buried in this settings subpage.)

    PackagesScrollArea = new QScrollArea(this);
    PackagesScrollArea->setWidgetResizable(true);
    layout->addWidget(PackagesScrollArea);
}

void PackagesView::rebuildList()
{
    if (PackagesScrollArea->widget()) PackagesScrollArea->widget()->deleteLater();
    QWidget * w = new QWidget(PackagesScrollArea);
    PackagesScrollArea->setWidget(w);
    QGridLayout * g = new QGridLayout(w); w->setLayout(g);
    int row = 0;
    auto & Lib = (*Model.config())["LIBRARY"];
    for (int i = 0; i < (int)Lib.size(); i++) {
        //Only HYDRATED packages (content present locally) are listed — synced-but-not-downloaded repo entries
        //live in the Available tab, not here. Content-less packages (a malformed game with no layers, or a
        //PATH-only runner) are vacuously hydrated, so require real content to exclude them.
        const std::string LibPath = Lib[i].value("PATH", std::string());
        //Only HYDRATED bundles with a launchable node appear here (a runner-only bundle has none).
        NodeIndex BIdx; ManifestModel::ScanBundleNodes(LibPath, BIdx);
        bool AnyHydratedLaunchable = false;
        for (const auto & [NodeId, N] : BIdx.Nodes)
            if (N.IsLaunchable() && PackageCatalog::NodeHasContent(Model.catalogIndex(), NodeId)
                && PackageCatalog::NodeHydrated(Model.catalogIndex(), NodeId)) { AnyHydratedLaunchable = true; break; }
        if (!AnyHydratedLaunchable) continue;   // require REAL content present (excludes content-less/malformed)
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
