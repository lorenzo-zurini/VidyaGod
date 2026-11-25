#include "activitystrip.h"
#include "activitylog.h"
#include "appmodel.h"
#include "ipfsmodel.h"
#include "downloadmanager.h"
#include "packagecatalog.h"   // HasMissingSources
#include "updateservice.h"
#include "theme.h"

#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace {
//A download's card is named by its game key: its title from the catalog, else the key.
QString TitleOf(const AppModel & Model, const QString & Key)
{
    for (const auto & [Id, N] : Model.catalogIndex().Nodes)
        if (QString::fromStdString(N.GameKey()) == Key && N.Meta.is_object() && N.Meta.contains("TITLE") && N.Meta["TITLE"].is_string())
            return QString::fromStdString(N.Meta["TITLE"].get<std::string>());
    return Key;
}
} // namespace

void ActivityStrip::Watch(AppModel & Model, IpfsModel & Ipfs, DownloadManager & Downloads)
{
    ActivityLog & L = ActivityLog::Get();
    struct State
    {
        QHash<QString, int> Download;            // game key → its job
        QSet<QString> Cancelled;                 // downloads cancelled from the list
        int Publish = 0, Seed = 0, Recheck = 0, Sync = 0, RecheckTotal = 0, Update = 0;
    };
    auto S = std::make_shared<State>();

    QObject::connect(&Downloads, &DownloadManager::downloadStarted, &L, [&L, &Model, &Downloads, S](const QString & Key) {
        if (S->Download.contains(Key)) return;
        S->Download[Key] = L.start("Downloads", TitleOf(Model, Key), [&Downloads, S, Key] { S->Cancelled.insert(Key); Downloads.cancelKey(Key); });
    });
    QObject::connect(&Downloads, &DownloadManager::downloadProgress, &L, [&L, S](const QString & Key, double Pct) {
        if (const int Id = S->Download.value(Key)) L.progress(Id, Pct < 0 ? -1 : Pct / 100.0, Pct < 0 ? QString() : QString("%1%").arg(int(Pct)));
    });
    QObject::connect(&Downloads, &DownloadManager::downloadFinished, &L, [&L, S](const QString & Key) {
        const int Id = S->Download.take(Key);
        if (Id) L.finish(Id, true, S->Cancelled.remove(Key) ? "Cancelled" : "Downloaded");
    });

    QObject::connect(&Model, &AppModel::publishStarted, &L, [&L, S](const QString & What) { S->Publish = L.start("Publish", "Publish " + What); });
    QObject::connect(&Model, &AppModel::libraryPublished, &L, [&L, S](const QString &, const QString &, const QString & Note) {
        const QString Said = Note.isEmpty() ? QString("Published — peers see it now") : "Published, with a note: " + Note;
        if (S->Publish) L.finish(std::exchange(S->Publish, 0), true, Said); else L.note("Publish", "Publish", true, Said);
    });
    QObject::connect(&Model, &AppModel::libraryPublishFailed, &L, [&L, S](const QString & Why) {
        if (S->Publish) L.finish(std::exchange(S->Publish, 0), false, Why); else L.note("Publish", "Publish", false, Why);
    });

    QObject::connect(&Ipfs, &IpfsModel::seedStarted, &L, [&L, &Ipfs, S](const QString & Dir) {
        S->Seed = L.start("Seed folder", "Seed " + QDir(Dir).dirName(), [&Ipfs] { Ipfs.stopSeed(); });
    });
    QObject::connect(&Ipfs, &IpfsModel::seedProgress, &L, [&L, S](int Done, int Total) {
        if (S->Seed) L.progress(S->Seed, Total > 0 ? double(Done) / Total : -1, QString("%1 of %2 files").arg(Done).arg(Total));
    });
    QObject::connect(&Ipfs, &IpfsModel::seedFinished, &L, [&L, S](int Seeded, int Mismatched) {
        if (!S->Seed) return;
        QString Said = QString("Seeded %1 referenced file%2.").arg(Seeded).arg(Seeded == 1 ? "" : "s");
        if (Mismatched > 0) Said += QString(" %1 file%2 changed since publish (or could not be added): their recorded CID could not "
                                            "be seeded again.").arg(Mismatched).arg(Mismatched == 1 ? "" : "s");
        L.finish(std::exchange(S->Seed, 0), Mismatched == 0, Said);
    });

    QObject::connect(&Ipfs, &IpfsModel::recheckStarted, &L, [&L, &Ipfs, S](int Total) {
        S->RecheckTotal = Total;
        S->Recheck = L.start("Force recheck", QString("Recheck %1 item%2").arg(Total).arg(Total == 1 ? "" : "s"), [&Ipfs] { Ipfs.stopRecheck(); });
    });
    QObject::connect(&Ipfs, &IpfsModel::recheckProgress, &L, [&L, S](int Done, int Total) {
        if (S->Recheck) L.progress(S->Recheck, Total > 0 ? double(Done) / Total : -1, QString("%1 of %2 read").arg(Done).arg(Total));
    });
    QObject::connect(&Ipfs, &IpfsModel::recheckFinished, &L, [&L, S](int Checked, int Failed) {
        if (!S->Recheck) return;
        QString Said = Failed == 0
            ? QString("Verified %1 item%2 — every block reads back correctly and can be served.").arg(Checked).arg(Checked == 1 ? "" : "s")
            : QString("%1 of %2 item%3 FAILED verification and cannot be served as published: their rows in Downloads say why. "
                      "Download them again, or seed them again from a good copy.").arg(Failed).arg(Checked).arg(Checked == 1 ? "" : "s");
        if (Checked < S->RecheckTotal) Said += QString(" Stopped after %1 of %2.").arg(Checked).arg(S->RecheckTotal);
        L.finish(std::exchange(S->Recheck, 0), Failed == 0, Said);
    });

    QObject::connect(&Model, &AppModel::sourcesSyncStarted, &L, [&L, S] {
        if (!S->Sync) S->Sync = L.start("Sources", "Sync package sources");
    });
    //A source not fetched (unreachable, the node's tries bounded) is not up to date: the sync tries again in a minute.
    QObject::connect(&Model, &AppModel::sourcesSyncFinished, &L, [&L, &Model, S](bool Changed) {
        if (!S->Sync) return;
        const bool Missing = Model.config() && PackageCatalog::HasMissingSources(*Model.config());
        L.finish(std::exchange(S->Sync, 0), !Missing,
                 Missing ? QString("A package source could not be fetched yet: it is tried again in a minute while the node runs")
                         : Changed ? "The library changed: new or updated packages are listed" : "Up to date");
    });
    QObject::connect(&Model, &AppModel::packageSourceFailed, &L, [&L](const QString & Why) { L.note("Sources", "Package source", false, Why); });
    QObject::connect(&Model, &AppModel::packageRemoveFailed, &L, [&L](const QString & Why) {
        L.note("Uninstall", "Uninstall", false, "The package was not uninstalled: " + Why);
    });

    //A switch to a peer's version: its steps, its Cancel (stops the fetch), and what came of it. A switch the person
    //declined at the confirm leaves no entry: they were there.
    UpdateService & Up = Model.updates();
    QObject::connect(&Up, &UpdateService::started, &L, [&L, &Up, S](const QString & Pkg, const QString & Peer) {
        S->Update = L.start("Update", "Switch " + Pkg + " to " + Peer + "'s version", [&Up] { Up.cancel(); });
    });
    QObject::connect(&Up, &UpdateService::progress, &L, [&L, S](double F, const QString & Detail) {
        if (S->Update) L.progress(S->Update, F, Detail);
    });
    QObject::connect(&Up, &UpdateService::finished, &L, [&L, S](UpdateService::Outcome O, const QString & Result) {
        const int Id = std::exchange(S->Update, 0);
        if (!Id) return;
        if (O == UpdateService::Outcome::Declined) { L.finish(Id, true, Result); L.dismiss(Id); return; }
        L.finish(Id, O != UpdateService::Outcome::Failed, O == UpdateService::Outcome::Cancelled ? QString("Cancelled") : Result);
    });
}

ActivityStrip::ActivityStrip(QWidget * Parent) : QWidget(Parent)
{
    setObjectName("activityStrip");
    auto * H = new QHBoxLayout(this);
    H->setContentsMargins(8, 2, 8, 2);
    H->addStretch(1);
    Spin = new QLabel(this);
    Spin->setFixedWidth(16);
    H->addWidget(Spin);
    Button = new QToolButton(this);
    Button->setObjectName("activityButton");
    Button->setAutoRaise(true);
    Button->setAccessibleName("Activity");
    Button->setToolTip("What is running in the background, and what it came to");
    H->addWidget(Button);
    Anim = new QTimer(this);
    Anim->setInterval(120);
    connect(Anim, &QTimer::timeout, this, [this] {
        static const char * F[] = { "◐", "◓", "◑", "◒" };
        Spin->setText(QString::fromUtf8(F[Frame++ % 4]));
    });
    connect(Button, &QToolButton::clicked, this, &ActivityStrip::openList);
    connect(&ActivityLog::Get(), &ActivityLog::changed, this, [this] { refresh(); if (Popup && Popup->isVisible()) fillList(); });
    refresh();
}

QString ActivityStrip::summary() const { return Button->text(); }

void ActivityStrip::refresh()
{
    const ActivityLog & L = ActivityLog::Get();
    const int Running = L.running(), Failed = L.failedUnseen();
    const auto All = L.entries();
    QString T;
    if (Running > 0) T = QString("%1 task%2").arg(Running).arg(Running == 1 ? "" : "s");
    else if (!All.isEmpty()) T = All.back().Title + ": " + (All.back().Failed ? "failed" : "done");
    else T = "No tasks";
    if (Failed > 0) T += QString("  ⚠ %1 failed").arg(Failed);
    Button->setText(T);
    Button->setEnabled(!All.isEmpty());
    if (Running > 0) { if (!Anim->isActive()) Anim->start(); }
    else { Anim->stop(); Spin->clear(); }
}

void ActivityStrip::openList()
{
    if (!Popup)
    {
        Popup = new QFrame(this, Qt::Popup);
        Popup->setObjectName("activityList");
        Popup->setFrameShape(QFrame::StyledPanel);
        Popup->setMinimumWidth(460);
    }
    fillList();
    Popup->adjustSize();
    const QPoint At = Button->mapToGlobal(QPoint(Button->width() - Popup->width(), -Popup->height()));
    Popup->move(At);
    Popup->show();
    ActivityLog::Get().markSeen();
}

void ActivityStrip::fillList()
{
    if (!Popup) return;
    delete Popup->layout();
    for (QWidget * W : Popup->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) W->deleteLater();
    auto * V = new QVBoxLayout(Popup);
    ActivityLog & L = ActivityLog::Get();
    const auto All = L.entries();
    for (int I = (int)All.size() - 1; I >= 0; --I)   // newest first
    {
        const ActivityLog::Entry & E = All[I];
        auto * Row = new QFrame(Popup);
        Row->setObjectName("activityRow");
        auto * G = new QVBoxLayout(Row);
        G->setContentsMargins(4, 4, 4, 4);
        auto * Top = new QHBoxLayout();
        auto * Title = new QLabel(QString("<b>%1</b> <span style='color:%2'>· %3</span>")
                                      .arg(E.Title.toHtmlEscaped(), Theme::Dim, E.Kind.toHtmlEscaped()), Row);
        Top->addWidget(Title, 1);
        const int Id = E.Id;
        if (E.Running && E.CanCancel)
        {
            auto * C = new QPushButton("Cancel", Row);
            C->setAccessibleName("Cancel " + E.Title);
            connect(C, &QPushButton::clicked, Row, [Id] { ActivityLog::Get().cancel(Id); });
            Top->addWidget(C);
        }
        else if (!E.Running)
        {
            auto * X = new QToolButton(Row);
            X->setText("✕");
            X->setAutoRaise(true);
            X->setAccessibleName("Dismiss " + E.Title);
            connect(X, &QToolButton::clicked, Row, [Id] { ActivityLog::Get().dismiss(Id); });
            Top->addWidget(X);
        }
        G->addLayout(Top);
        if (E.Running)
        {
            auto * B = new QProgressBar(Row);
            B->setRange(0, E.Fraction < 0 ? 0 : 1000);
            if (E.Fraction >= 0) B->setValue(int(E.Fraction * 1000));
            B->setTextVisible(false);
            B->setMaximumHeight(8);
            G->addWidget(B);
        }
        if (!E.Detail.isEmpty())
        {
            auto * D = new QLabel((E.Failed ? QString("⚠ ") : QString()) + E.Detail, Row);
            D->setWordWrap(true);
            D->setTextInteractionFlags(Qt::TextSelectableByMouse);
            G->addWidget(D);
        }
        V->addWidget(Row);
    }
    if (std::any_of(All.begin(), All.end(), [](const ActivityLog::Entry & E) { return !E.Running; }))
    {
        auto * Clear = new QPushButton("Clear finished", Popup);
        connect(Clear, &QPushButton::clicked, Popup, [] { ActivityLog::Get().dismissFinished(); });
        V->addWidget(Clear, 0, Qt::AlignRight);
    }
    Popup->adjustSize();
}
