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

    QGroupBox * ptb = new QGroupBox(this);
    QHBoxLayout * ptbl = new QHBoxLayout(ptb); ptb->setLayout(ptbl);
    layout->addWidget(ptb);

    QPushButton * addBtn = new QPushButton("Add Local Package", ptb);
    addBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ptbl->addWidget(addBtn);
    connect(addBtn, &QPushButton::clicked, this, &PackagesView::addLocalPackages);

    QPushButton * edBtn = new QPushButton("Package Editor", ptb);
    edBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    ptbl->addWidget(edBtn);
    connect(edBtn, &QPushButton::clicked, this, [this]{
        auto * Ed = new PackageEditor(Model.config(), this);
        connect(Ed, &PackageEditor::packageSaved, &MainWindow::RefreshPackage);
        Ed->show();
    });

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
            if (N.IsLaunchable() && PackageCatalog::NodeHydrated(Model.catalogIndex(), NodeId)) { AnyHydratedLaunchable = true; break; }
        if (!AnyHydratedLaunchable) continue;
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

void PackagesView::addLocalPackages()
{
    QString sel = QFileDialog::getExistingDirectory(this, "Select package or directory...");
    if (sel.isEmpty()) return;
    QStringList paths;
    std::function<void(const QString &)> scan = [&](const QString & d) {
        QDir dir(d);
        if (FSOps::CheckPackageValid(&dir)) { paths.append(d); return; }
        for (const QString & s : dir.entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            scan(QDir::cleanPath(d + QDir::separator() + s));
    };
    scan(sel);
    if (paths.isEmpty()) { QMessageBox::warning(this,"No packages found","No valid packages found."); return; }
    int added=0, skipped=0;
    for (const QString & path : paths) {
        //Node-native identity: a library bundle must define a launchable node (runner-only bundles aren't games).
        NodeIndex BIdx; ManifestModel::ScanBundleNodes(path.toStdString(), BIdx);
        const Node * Rep = nullptr;
        for (const auto & [id, N] : BIdx.Nodes)
            if (N.IsLaunchable() && (!Rep || (N.Presentable() && !Rep->Presentable()))) Rep = &N;
        if (!Rep) {
            LogWarn("PackagesView", "Skipping " + path.toStdString() + ": no launchable node (not a library bundle).");
            skipped++; continue;
        }
        const std::string Uid  = Rep->Uid.empty() ? Rep->NodeId : Rep->Uid;
        const std::string Name = Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId;
        bool dup=false;
        for (auto & e : (*Model.config())["LIBRARY"])
            if (e.value("PACKAGEUID", std::string()) == Uid) { dup=true; skipped++; break; }
        if (dup) continue;
        nlohmann::ordered_json slim;
        slim["PACKAGEUID"]=Uid;
        slim["PACKAGENAME"]=Name;
        slim["PATH"]=path.toStdString();
        (*Model.config())["LIBRARY"].push_back(slim);
        added++;
    }
    if (added>0) { Model.save(); Model.rebuildCatalog(); }
    QMessageBox::information(this,"Done",
        QString("Added %1 package(s). %2 skipped.").arg(added).arg(skipped));
}
