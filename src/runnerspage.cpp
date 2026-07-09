#include "runnerspage.h"
#include "appmodel.h"          // AppModel — catalog + importRunner
#include "libraryview.h"       // IpfsFetchReady()
#include "packagecatalog.h"
#include "manifestmodel.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "runnerwrapper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QFrame>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

RunnersPage::RunnersPage(AppModel &model, QWidget *parent)
    : QWidget(parent), Model(model)
{
    QVBoxLayout * pl = new QVBoxLayout(this);
    pl->setContentsMargins(8,8,8,8);
    Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    pl->addWidget(Scroll);

    connect(&Model, &AppModel::catalogChanged, this, &RunnersPage::rebuild);
    rebuild();
}

void RunnersPage::rebuild()
{
    if (!Scroll) return;
    if (Scroll->widget()) Scroll->widget()->deleteLater();

    QWidget * contents = new QWidget(Scroll);
    QVBoxLayout * v = new QVBoxLayout(contents); contents->setLayout(v);
    Scroll->setWidget(contents);

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
    for (const auto & [Id, N] : Model.catalogIndex().Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        if (PackageCatalog::IsEmbeddedRunner(Model.catalogIndex(), N.NodeId)
            && !PackageCatalog::RunnerInstalled(Model.catalogIndex(), N.NodeId)) continue;
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

        const bool Avail      = RunnerWrapper::ExecutableAvailable(R->Exec);
        const bool Ships      = !PackageCatalog::NodeContentCids(Model.catalogIndex(), rid).empty();
        const bool PrefixGen  = R->Exec.is_object() && R->Exec.value("PREFIX_GENERATE", false);
        const bool BuildReady = RunnerInstall::RunnerBuildPresent(Model.catalogIndex(), rid);   // build layers hydrated
        const bool Imported   = RunnerInstall::RunnerNodeImported(Model.catalogIndex(), rid);   // build + DEFPREFIX

        // Two-stage state: (1) build fetched? (2) DEFPREFIX generated? — no longer conflated. Build present but prefix
        // missing (or a partial one from an interrupted wineboot) surfaces as its own state with a Generate button.
        QHBoxLayout * row = new QHBoxLayout();
        row->addWidget(new QLabel(Desc, card), 1);
        QLabel * st = new QLabel(card);
        if (!Ships && Avail)              st->setText("<span style='color:#8f98a0;'>built-in</span>");
        else if (!Ships)                  st->setText("<span style='color:#c0726a;'>not installed on system</span>");
        else if (Imported)                st->setText("<span style='color:#5fb55f;'>Imported</span>");
        else if (BuildReady && PrefixGen) st->setText("<span style='color:#c6a15f;'>Build ready — prefix needed</span>");
        else                              st->setText("<span style='color:#c0726a;'>Not imported</span>");
        row->addWidget(st);

        // Import (fetch build → auto-generate prefix) feeds the ONE download pump and needs the node online. The prefix
        // buttons re-run ONLY the DEFPREFIX step (local wineboot, no download) — the deliberate, retryable step.
        auto addPrefixButton = [&](const char * Label, const char * Busy, bool Force){
            QPushButton * btn = new QPushButton(Label, card);
            connect(btn, &QPushButton::clicked, this, [this, rid, btn, st, Busy, Force]{
                btn->setEnabled(false); btn->setText(Busy);
                st->setText(QString("<span style='color:#c6a15f;'>%1…</span>").arg(Busy));
                Model.generateRunnerDefPrefix(QString::fromStdString(rid), Force);   // off-thread; catalogChanged rebuilds
            });
            row->addWidget(btn);
        };
        if (Ships && !BuildReady && IpfsWrapper::Available())
        {
            QPushButton * btn = new QPushButton("Import", card);
            connect(btn, &QPushButton::clicked, this, [this, rid, btn, st]{
                if (!IpfsFetchReady(this)) return;              // need the embedded node online to fetch
                btn->setEnabled(false); btn->setText("Importing…");
                st->setText("<span style='color:#c6a15f;'>Importing… (see IPFS tab)</span>");
                Model.importRunner(QString::fromStdString(rid));   // → download pump; catalogChanged rebuilds this page
            });
            row->addWidget(btn);
        }
        else if (Ships && !BuildReady)
        {
            QLabel * need = new QLabel("IPFS unavailable", card);
            need->setStyleSheet("color:#8f98a0;font-style:italic;");
            row->addWidget(need);
        }
        else if (Ships && PrefixGen && !Imported) addPrefixButton("Generate prefix", "Generating", /*force*/false);
        else if (Ships && PrefixGen && Imported)  addPrefixButton("Rebuild prefix",  "Rebuilding",  /*force*/true);
        cv->addLayout(row);
        v->addWidget(card);
    }
    v->addStretch(1);
}
