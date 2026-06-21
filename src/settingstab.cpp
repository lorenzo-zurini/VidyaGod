#include "settingstab.h"
#include "mainwindow.h"        // friend access: Mw.CatalogIndex / GlobalConfigJSON / PackagesTabWidget / rebuilds
#include "ipfstab.h"          // Mw.IpfsTabPtr->refresh()
#include "catalogtab.h"       // Mw.CatalogTabPtr->rebuild()
#include "libraryview.h"       // IpfsFetchReady()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"
#include "commonutils.h"

#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QFrame>
#include <QDir>

#include <nlohmann/json.hpp>

#include <thread>
#include <string>
#include <vector>

SettingsTab::SettingsTab(MainWindow &Owner, QWidget *parent)
    : QWidget(parent), Mw(Owner) { buildUi(); }

void SettingsTab::buildUi()
{
    QHBoxLayout * outer = new QHBoxLayout(this);
    outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);

    // Left: category sidebar.
    SettingsCategoryList = new QListWidget(this);
    SettingsCategoryList->setFixedWidth(170);
    SettingsCategoryList->setFrameShape(QFrame::NoFrame);
    SettingsCategoryList->addItem("Installed Packages");
    SettingsCategoryList->addItem("Runners");
    SettingsCategoryList->addItem("Repositories");
    SettingsCategoryList->addItem("Downloads");
    SettingsCategoryList->addItem("Storage & Paths");
    outer->addWidget(SettingsCategoryList);

    // Right: stacked category forms.
    SettingsStack = new QStackedWidget(this);
    outer->addWidget(SettingsStack, 1);

    // Page 0 — Installed Packages (built in BuildStaticUI; the +Add Local Package / Package Editor / list).
    SettingsStack->addWidget(Mw.PackagesTabWidget);

    // Page 1 — Runners (a scroll area we rebuild in place on structural edits).
    {
        QWidget * page = new QWidget(SettingsStack);
        QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
        pl->setContentsMargins(8,8,8,8);
        SettingsRunnersScroll = new QScrollArea(page);
        SettingsRunnersScroll->setWidgetResizable(true);
        SettingsRunnersScroll->setFrameShape(QFrame::NoFrame);
        pl->addWidget(SettingsRunnersScroll);
        SettingsStack->addWidget(page);
        rebuildRunnersPage();
    }

    // Page 2 — Repositories (a scroll area we rebuild in place on add/remove).
    {
        QWidget * page = new QWidget(SettingsStack);
        QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
        pl->setContentsMargins(8,8,8,8);
        SettingsReposScroll = new QScrollArea(page);
        SettingsReposScroll->setWidgetResizable(true);
        SettingsReposScroll->setFrameShape(QFrame::NoFrame);
        pl->addWidget(SettingsReposScroll);
        SettingsStack->addWidget(page);
        rebuildReposPage();
    }

    // Page 3 — Downloads.
    SettingsStack->addWidget(buildDownloadsPage());

    // Page 4 — Storage & Paths.
    SettingsStack->addWidget(buildPathsPage());

    QObject::connect(SettingsCategoryList, &QListWidget::currentRowChanged,
                     SettingsStack, &QStackedWidget::setCurrentIndex);
    SettingsCategoryList->setCurrentRow(0);
}

void SettingsTab::rebuildRunnersPage()
{
    if (!SettingsRunnersScroll) return;
    if (SettingsRunnersScroll->widget()) SettingsRunnersScroll->widget()->deleteLater();

    QWidget * contents = new QWidget(SettingsRunnersScroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    SettingsRunnersScroll->setWidget(contents);

    // Runners are packages from the configured repositories. One that ships its own build (e.g. Proton over
    // IPFS) must be imported (build fetched + DEFPREFIX generated) before games can use it.
    QLabel * intro = new QLabel(
        "Runners are packages from your repositories. A runner that ships its own build (e.g. Proton over "
        "IPFS) must be imported before games can use it.", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    //Runners are the ROLE:"runner" nodes of the global catalog graph — one card per runner node. An EMBEDDED runner
    //(bundled inside a game package) is hidden until installed — it's offered in that game's download dialog instead;
    //global runners always show (installed or installable via the Import button below).
    std::vector<const Node*> Runners;
    for (const auto & [Id, N] : Mw.CatalogIndex.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        if (PackageCatalog::IsEmbeddedRunner(Mw.CatalogIndex, N.NodeId)
            && !PackageCatalog::RunnerInstalled(Mw.CatalogIndex, N.NodeId)) continue;
        Runners.push_back(&N);
    }
    std::sort(Runners.begin(), Runners.end(), [](const Node* A, const Node* B){ return A->NodeId < B->NodeId; });
    if (Runners.empty())
    {
        QLabel * none = new QLabel("No runners found in your repositories.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (const Node * R : Runners)
    {
        const std::string rid = R->NodeId;
        QGroupBox * card = new QGroupBox(QString::fromStdString(rid), contents);
        QVBoxLayout * cv = new QVBoxLayout(card); card->setLayout(cv);

        QString guest;
        for (const auto & P : R->GuestPlatform) guest += (guest.isEmpty() ? "" : ", ") + QString::fromStdString(P);
        const QString Desc = QString::fromStdString(R->HostPlatform) + " → [" + guest + "]";

        const bool Avail    = RunnerWrapper::ExecutableAvailable(R->Exec);
        const bool Ships    = !PackageCatalog::NodeContentCids(Mw.CatalogIndex, rid).empty();
        const bool Imported = ContainerWrapper::RunnerNodeImported(Mw.CatalogIndex, rid);
        QHBoxLayout * row = new QHBoxLayout();
        row->addWidget(new QLabel(Desc, card), 1);
        QLabel * st = new QLabel(card);
        if (!Ships && Avail) st->setText("<span style='color:#8f98a0;'>built-in</span>");
        else if (!Ships)    st->setText("<span style='color:#c0726a;'>not installed on system</span>");
        else if (Imported)  st->setText("<span style='color:#5fb55f;'>Imported</span>");
        else                 st->setText("<span style='color:#c0726a;'>Not imported</span>");
        row->addWidget(st);
        if (Ships && !Imported && IpfsWrapper::Available())
        {
            QPushButton * btn = new QPushButton("Import", card);
            connect(btn, &QPushButton::clicked, this, [this, rid, btn, st]{
                if (!IpfsFetchReady(&Mw)) return;              // need the embedded node online to fetch
                btn->setEnabled(false); btn->setText("Importing…");
                st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                // The worker reads config + index; hand it private copies so it never races the GUI thread (which
                // owns the live GlobalConfigJSON/Mw.CatalogIndex and may mutate them concurrently).
                auto Cfg = std::make_shared<nlohmann::ordered_json>(*Mw.GlobalConfigJSON);
                NodeIndex Idx = Mw.CatalogIndex;
                std::thread([this, rid, Cfg, Idx = std::move(Idx)]{
                    std::string Err;
                    bool Ok = ContainerWrapper::ImportRunnerNode(*Cfg, Idx, rid, &Err);
                    QMetaObject::invokeMethod(this, [this, Ok, Err]{
                        if (!Ok) LogErr("MainWindow", "Runner import failed: " + Err);   // no dialog — see the IPFS tab
                        rebuildRunnersPage(); Mw.IpfsTabPtr->refresh();
                    }, Qt::QueuedConnection);
                }).detach();
            });
            row->addWidget(btn);
        }
        else if (Ships && !Imported)
        {
            QLabel * need = new QLabel("IPFS unavailable", card);
            need->setStyleSheet("color:#8f98a0;font-style:italic;");
            row->addWidget(need);
        }
        cv->addLayout(row);
        v->addWidget(card);
    }
    v->addStretch(1);
}

// Repositories settings page — add/remove the git repos that share dehydrated packages.
// Each Settings.Repositories[] entry is {NAME, PATH:<git url>}, cloned to ~/.VidyaGod/LIBRARY/<name>
// and indexed. Adding one clones it (off-thread); removing drops the reference (the disposable clone is
// left on disk). After any change the Store + Runners pages are rebuilt so the new catalog shows.
void SettingsTab::rebuildReposPage()
{
    if (!SettingsReposScroll) return;
    if (SettingsReposScroll->widget()) SettingsReposScroll->widget()->deleteLater();

    QWidget * contents = new QWidget(SettingsReposScroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    SettingsReposScroll->setWidget(contents);

    QLabel * intro = new QLabel(
        "Repositories are git repos that share dehydrated packages (manifests + IPFS CIDs, no bundled content). "
        "Cloned into your LIBRARY, where packages hydrate their content in place; private repos work if your git is "
        "set up to authenticate non-interactively (SSH key or a stored token).", contents);
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8f98a0;font-size:9pt;");
    v->addWidget(intro);

    // ── Sync now ── pull all repos again to pick up packages added/updated upstream (no restart needed).
    {
        QHBoxLayout * syncRow = new QHBoxLayout();
        QPushButton * syncBtn = new QPushButton("Sync now", contents);
        syncRow->addWidget(syncBtn); syncRow->addStretch(1);
        v->addLayout(syncRow);
        connect(syncBtn, &QPushButton::clicked, this, [this, syncBtn]{
            syncBtn->setEnabled(false); syncBtn->setText("Syncing…");
            // Sync on a private config copy (it git-pulls + rebuilds LIBRARY), then apply just LIBRARY back on the
            // GUI thread — so the worker never mutates the live GlobalConfigJSON the GUI may be reading/writing.
            auto Cfg = std::make_shared<nlohmann::ordered_json>(*Mw.GlobalConfigJSON);
            std::thread([this, Cfg]{
                PackageCatalog::SyncRepositories(*Cfg);   // git pull each repo + reindex LIBRARY (into the copy)
                QMetaObject::invokeMethod(this, [this, Cfg]{
                    (*Mw.GlobalConfigJSON)["LIBRARY"] = (*Cfg)["LIBRARY"];   // SyncRepositories only writes LIBRARY
                    Mw.SaveGlobalConfigJSON();
                    Mw.RebuildDynamicUI(); rebuildReposPage(); rebuildRunnersPage(); Mw.CatalogTabPtr->rebuild();
                }, Qt::QueuedConnection);
            }).detach();
        });
    }

    auto & S = (*Mw.GlobalConfigJSON)["Settings"];
    if (!S.contains("Repositories") || !S["Repositories"].is_array())
        S["Repositories"] = nlohmann::ordered_json::array();

    if (S["Repositories"].empty())
    {
        QLabel * none = new QLabel("No repositories configured.", contents);
        none->setStyleSheet("color:#8f98a0;");
        v->addWidget(none);
    }
    for (int i = 0; i < int(S["Repositories"].size()); ++i)
    {
        const auto & R = S["Repositories"][i];
        const std::string Url  = R.is_object() ? R.value("PATH", std::string()) : std::string();
        std::string Name = R.is_object() ? R.value("NAME", std::string()) : std::string();
        if (Name.empty()) { QString b = QString::fromStdString(Url); b = b.section('/', -1); if (b.endsWith(".git")) b.chop(4); Name = b.toStdString(); }

        QGroupBox * card = new QGroupBox(QString::fromStdString(Name), contents);
        QHBoxLayout * row = new QHBoxLayout(card); card->setLayout(row);
        QLabel * urlLbl = new QLabel("<span style='color:#8f98a0;'>" + QString::fromStdString(Url).toHtmlEscaped() + "</span>", card);
        urlLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(urlLbl, 1);
        QPushButton * rm = new QPushButton("Remove", card);
        connect(rm, &QPushButton::clicked, this, [this, i]{
            auto & SS = (*Mw.GlobalConfigJSON)["Settings"];
            if (SS.contains("Repositories") && SS["Repositories"].is_array() && i < int(SS["Repositories"].size()))
                SS["Repositories"].erase(SS["Repositories"].begin() + i);
            Mw.SaveGlobalConfigJSON();
            rebuildReposPage(); rebuildRunnersPage(); Mw.CatalogTabPtr->rebuild();
        });
        row->addWidget(rm);
        v->addWidget(card);
    }

    // ── Add-repository row ──
    QGroupBox * add = new QGroupBox("Add repository", contents);
    QFormLayout * af = new QFormLayout(add); add->setLayout(af);
    QLineEdit * nameEdit = new QLineEdit(add);
    nameEdit->setPlaceholderText("(optional — defaults to the URL's basename)");
    QLineEdit * urlEdit = new QLineEdit(add);
    urlEdit->setPlaceholderText("https://github.com/you/Repo.git  or  git@github.com:you/Repo.git");
    af->addRow("Name:", nameEdit);
    af->addRow("Git URL:", urlEdit);
    QPushButton * addBtn = new QPushButton("Add + sync", add);
    af->addRow("", addBtn);
    connect(addBtn, &QPushButton::clicked, this, [this, nameEdit, urlEdit, addBtn]{
        const QString Url = urlEdit->text().trimmed();
        if (Url.isEmpty()) { QMessageBox::warning(this, "Add repository", "Enter a git URL."); return; }
        nlohmann::ordered_json Entry = nlohmann::ordered_json::object();
        const QString Nm = nameEdit->text().trimmed();
        if (!Nm.isEmpty()) Entry["NAME"] = Nm.toStdString();
        Entry["PATH"] = Url.toStdString();
        auto & SS = (*Mw.GlobalConfigJSON)["Settings"];
        if (!SS.contains("Repositories") || !SS["Repositories"].is_array()) SS["Repositories"] = nlohmann::ordered_json::array();
        SS["Repositories"].push_back(Entry);
        Mw.SaveGlobalConfigJSON();
        addBtn->setEnabled(false); addBtn->setText("Cloning…");
        // Clone/pull + index off-thread on a private config copy (includes the entry just added above), then apply
        // LIBRARY back on the GUI thread — no worker mutation of the live GlobalConfigJSON.
        auto Cfg = std::make_shared<nlohmann::ordered_json>(*Mw.GlobalConfigJSON);
        std::thread([this, Cfg]{
            PackageCatalog::SyncRepositories(*Cfg);   // git clone/pull + reindex LIBRARY (into the copy)
            QMetaObject::invokeMethod(this, [this, Cfg]{
                (*Mw.GlobalConfigJSON)["LIBRARY"] = (*Cfg)["LIBRARY"];
                Mw.SaveGlobalConfigJSON();
                Mw.RebuildDynamicUI(); rebuildReposPage(); rebuildRunnersPage(); Mw.CatalogTabPtr->rebuild();
            }, Qt::QueuedConnection);
        }).detach();
    });
    v->addWidget(add);

    v->addStretch(1);
}

QWidget * SettingsTab::buildDownloadsPage()
{
    QWidget * page = new QWidget(SettingsStack);
    QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    // Max simultaneous downloads — Settings.MaxConcurrentDownloads. Caps how many files fetch at once across ALL
    // packages, so one slow/stalled file no longer holds up the rest of the queue.
    QSpinBox * maxDl = new QSpinBox(page);
    maxDl->setRange(1, 32);
    maxDl->setValue(IpfsWrapper::MaxConcurrentDownloads());
    {
        auto & S = (*Mw.GlobalConfigJSON)["Settings"];
        if (S.contains("MaxConcurrentDownloads") && S["MaxConcurrentDownloads"].is_number_integer())
            maxDl->setValue(int(S["MaxConcurrentDownloads"]));
    }
    QObject::connect(maxDl, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        IpfsWrapper::SetMaxConcurrentDownloads(v);
        (*Mw.GlobalConfigJSON)["Settings"]["MaxConcurrentDownloads"] = v;
        Mw.SaveGlobalConfigJSON();
    });
    form->addRow("Max simultaneous downloads:", maxDl);

    QLabel * note = new QLabel("Files download in parallel up to this limit. A stalled download keeps retrying in the "
                               "background without blocking the others; raise this to fetch more at once.", page);
    note->setWordWrap(true);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
    return page;
}

QWidget * SettingsTab::buildPathsPage()
{
    QWidget * page = new QWidget(SettingsStack);
    QVBoxLayout * pl = new QVBoxLayout(page); page->setLayout(pl);
    pl->setContentsMargins(12,12,12,12);
    QFormLayout * form = new QFormLayout();
    pl->addLayout(form);

    const QString defaultTempRoot = QDir::cleanPath(QDir::homePath() + "/.VidyaGod/TEMP");

    // Temporary / runtime root — Settings.Paths.TempRoot (empty = default).
    QWidget * rootRow = new QWidget(page);
    QHBoxLayout * rl = new QHBoxLayout(rootRow); rl->setContentsMargins(0,0,0,0);
    rootRow->setLayout(rl);
    QLineEdit * rootEdit = new QLineEdit(rootRow);
    rootEdit->setPlaceholderText(defaultTempRoot + "  (default)");
    {
        auto & S = (*Mw.GlobalConfigJSON)["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string())
            rootEdit->setText(QString::fromStdString(std::string(S["Paths"]["TempRoot"])));
    }
    auto writeTempRoot = [this,rootEdit]{
        auto & S = (*Mw.GlobalConfigJSON)["Settings"];
        QString t = rootEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("TempRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["TempRoot"] = t.toStdString();
        }
        Mw.SaveGlobalConfigJSON();
    };
    QObject::connect(rootEdit, &QLineEdit::editingFinished, this, writeTempRoot);
    rl->addWidget(rootEdit, 1);
    QPushButton * browse = new QPushButton("Browse…", rootRow);
    QObject::connect(browse, &QPushButton::clicked, this, [this,rootEdit,writeTempRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select temporary / runtime root");
        if (!d.isEmpty()) { rootEdit->setText(d); writeTempRoot(); }
    });
    rl->addWidget(browse);
    form->addRow("Temporary / runtime root:", rootRow);

    // Library folder — Settings.Paths.LibraryRoot (empty = default). Imported packages are hydrated here,
    // one subfolder per repo.
    const QString defaultLibRoot = QDir::cleanPath(QDir::homePath() + "/.VidyaGod/library");
    QWidget * libRow = new QWidget(page);
    QHBoxLayout * ll = new QHBoxLayout(libRow); ll->setContentsMargins(0,0,0,0);
    libRow->setLayout(ll);
    QLineEdit * libEdit = new QLineEdit(libRow);
    libEdit->setPlaceholderText(defaultLibRoot + "  (default)");
    {
        auto & S = (*Mw.GlobalConfigJSON)["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("LibraryRoot") && S["Paths"]["LibraryRoot"].is_string())
            libEdit->setText(QString::fromStdString(std::string(S["Paths"]["LibraryRoot"])));
    }
    auto writeLibRoot = [this,libEdit]{
        auto & S = (*Mw.GlobalConfigJSON)["Settings"];
        QString t = libEdit->text().trimmed();
        if (t.isEmpty()) {
            if (S.contains("Paths") && S["Paths"].is_object()) S["Paths"].erase("LibraryRoot");
        } else {
            if (!S.contains("Paths") || !S["Paths"].is_object()) S["Paths"] = nlohmann::ordered_json::object();
            S["Paths"]["LibraryRoot"] = t.toStdString();
        }
        Mw.SaveGlobalConfigJSON();
    };
    QObject::connect(libEdit, &QLineEdit::editingFinished, this, writeLibRoot);
    ll->addWidget(libEdit, 1);
    QPushButton * libBrowse = new QPushButton("Browse…", libRow);
    QObject::connect(libBrowse, &QPushButton::clicked, this, [this,libEdit,writeLibRoot]{
        QString d = QFileDialog::getExistingDirectory(this, "Select library folder");
        if (!d.isEmpty()) { libEdit->setText(d); writeLibRoot(); }
    });
    ll->addWidget(libBrowse);
    form->addRow("Library folder:", libRow);

    // AppData dir (read-only, informational).
    QLineEdit * appDataLbl = new QLineEdit(Mw.AppDataDir->path(), page);
    appDataLbl->setReadOnly(true);
    form->addRow("App data directory:", appDataLbl);

    QLabel * note = new QLabel("Path changes apply to future launches.", page);
    note->setStyleSheet("color:#8f98a0;font-size:9pt;");
    pl->addWidget(note);
    pl->addStretch(1);
    return page;
}
