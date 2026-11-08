#include "packagesview.h"
#include "theme.h"   // the named colours
#include "guiformat.h"   // HumanBytesQ
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
#include <QDesktopServices>
#include <QUrl>

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
                Q += "\n\nThese files are deleted (" + HumanBytesQ((long long)S->DeleteBytes) + "); they are needed again only to switch back:";
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
    //What is installed comes from the index (the packages LIBRARY holds are its nodes; the config lists only those added
    //from outside it): found off the GUI thread — hydration stats every content file, sizes walk every folder — one
    //scan at a time.
    rebuildUpdates();
    if (Listing) { ListAgain = true; return; }   // a scan is under way: one more when it ends, not a pile of them
    Listing = true;
    if (!PackagesScrollArea->widget())
    {
        QLabel *Wait = new QLabel("Reading the library…", PackagesScrollArea);
        Wait->setStyleSheet("color:" + Theme::Dim + ";");
        PackagesScrollArea->setWidget(Wait);
    }
    if (!Model.config()) { Listing = false; return; }
    auto Cfg = std::make_shared<const nlohmann::ordered_json>(*Model.config());
    std::shared_ptr<const NodeIndex> Idx = Model.catalogSnapshot();
    auto Found = std::make_shared<std::vector<PackageCatalog::InstalledPackage>>();
    AsyncWork::Run(this, [Cfg, Idx, Found]{ *Found = PackageCatalog::InstalledPackages(*Cfg, *Idx); },
        [this, Found]{
            Listing = false;
            if (ListAgain) { ListAgain = false; rebuildList(); return; }
            showList(*Found);
        });
}

void PackagesView::showList(const std::vector<PackageCatalog::InstalledPackage> &Pkgs)
{
    if (PackagesScrollArea->widget()) PackagesScrollArea->widget()->deleteLater();
    QWidget *w = new QWidget(PackagesScrollArea);
    w->setObjectName("InstalledPackagesList");
    QGridLayout *g = new QGridLayout(w);
    g->setHorizontalSpacing(16);
    g->setColumnStretch(0, 1);
    int row = 0;
    if (Pkgs.empty())
    {
        QLabel *none = new QLabel("No installed packages yet - download packages from the Catalog tab or add local packages.", w);
        none->setStyleSheet("color:" + Theme::Dim + ";");
        g->addWidget(none, 0, 0, 1, 4);
    }
    else
    {
        std::uintmax_t Total = 0;
        for (const auto &P : Pkgs) Total += P.Bytes;
        QLabel *Sum = new QLabel(QString("%1 packages · %2").arg(Pkgs.size()).arg(QString::fromStdString(HumanBytes(static_cast<long long>(Total)))), w);
        Sum->setStyleSheet("color:" + Theme::Dim + ";");
        g->addWidget(Sum, row++, 0, 1, 4);
    }
    std::string Group = "\x01";   // none yet
    for (const auto &P : Pkgs)
    {
        const std::string Here = P.Local ? std::string("Added locally") : P.Library;
        if (Here != Group)
        {
            Group = Here;
            QLabel *H = new QLabel(QString::fromStdString(Here), w);
            H->setStyleSheet("font-weight:600; margin-top:10px;");
            g->addWidget(H, row++, 0, 1, 4);
        }
        //A few versions by name, then how many more (a game can have hundreds: the row would be as wide as all of them).
        QString Versions, All;
        for (size_t I = 0; I < P.Variants.size(); ++I)
        {
            const QString V = QString::fromStdString(P.Variants[I]);
            if (I < 4) Versions += (Versions.isEmpty() ? "" : ", ") + V;
            if (I < 40) All += (All.isEmpty() ? "" : "\n") + V;
        }
        if (P.Variants.size() > 4) Versions += QString(" and %1 more").arg(P.Variants.size() - 4);
        if (P.Variants.size() > 40) All += QString("\n… and %1 more").arg(P.Variants.size() - 40);
        QLabel *Name = new QLabel(QString("<b>%1</b><br><span style='color:" + Theme::Dim + "'>%2 · %3</span>")
                                      .arg(QString::fromStdString(P.Title).toHtmlEscaped(), QString::fromStdString(P.Uid).toHtmlEscaped(),
                                           Versions.toHtmlEscaped()), w);
        Name->setToolTip(QString::fromStdString(P.Dir.string()) + (P.Variants.size() > 4 ? "\n\n" + All : QString()));
        Name->setWordWrap(true);
        Name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        g->addWidget(Name, row, 0);
        QLabel *Size = new QLabel(QString::fromStdString(HumanBytes(static_cast<long long>(P.Bytes))), w);
        Size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(Size, row, 1);
        const QString Dir = QString::fromStdString(P.Dir.string()), Title = QString::fromStdString(P.Title);
        QPushButton *Open = new QPushButton("Open folder", w);
        Open->setAccessibleName("Open folder of " + Title);
        connect(Open, &QPushButton::clicked, this, [Dir]{ QDesktopServices::openUrl(QUrl::fromLocalFile(Dir)); });
        g->addWidget(Open, row, 2);
        //Only a package added from outside LIBRARY is removed here — forgotten, its folder untouched. A package of LIBRARY
        //is not: its content can be shared with others and may have no source (AppModel::removePackage).
        //A package source's package (under LIBRARY, named by its config entry) keeps the Remove it always had.
        if (P.Local || P.HasEntry)
        {
            const bool Local = P.Local;
            QPushButton *rb = new QPushButton(Local ? "Forget" : "Remove", w);
            rb->setAccessibleName((Local ? "Forget " : "Remove ") + Title);
            rb->setToolTip(Local ? "Stop listing this package; its folder is not touched."
                                 : "Remove this source package's downloaded content; its nodes stay, to download again.");
            connect(rb, &QPushButton::clicked, this, [this, Dir, Title, Local]{
                const QString Q = Local ? QString("Forget %1? Its folder is not touched:\n%2").arg(Title, Dir)
                                        : QString("Remove %1's downloaded content? Its nodes stay, to download it again.").arg(Title);
                if (QMessageBox::question(this, Local ? "Forget package" : "Remove package", Q) != QMessageBox::Yes) return;
                if (const QString Why = Model.removePackage(Dir); !Why.isEmpty()) QMessageBox::warning(this, "Remove package", Why);
            });
            g->addWidget(rb, row, 3);
        }
        row++;
    }
    g->setRowStretch(g->rowCount(), 1);
    PackagesScrollArea->setWidget(w);
}

// (addLocalPackages moved to AppModel::importPackagesFromDir, invoked from the Library tab's toolbar.)
