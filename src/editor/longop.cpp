#include "longop.h"
#include "asyncwork.h"
#include "commonutils.h"   // HumanBytes

#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>
#include <condition_variable>
#include <set>
#include <memory>
#include <mutex>

namespace {
std::atomic<int> g_RanOnWorker{0};
std::multiset<const QWidget *> g_Running;   // GUI thread: the parents of the Runs going on

//Escape and the title bar's close ask to stop, as Cancel does: the dialog stays until the work has.
class Waiting : public QDialog
{
public:
    using QDialog::QDialog;
    std::function<void()> AskStop;
    void reject() override { if (AskStop) AskStop(); }
};
} // namespace

QString LongOp::Describe(const QString &Item, int Done, int Total, unsigned long long Bytes, unsigned long long TotalBytes)
{
    QString T = Item;
    if (Total > 0) T += (T.isEmpty() ? QString() : QString(" — ")) + QString("%1 of %2").arg(Done).arg(Total);
    if (TotalBytes > 0)
        T += QString(" · %1 of %2").arg(QString::fromStdString(HumanBytes((long long)Bytes)),
                                         QString::fromStdString(HumanBytes((long long)TotalBytes)));
    return T;
}

bool LongOp::RunningIn(const QWidget *Parent) { return g_Running.count(Parent) != 0; }

bool LongOp::Blocks(const QWidget *W)
{
    if (!W) return false;
    for (const QWidget *P : g_Running)
        if (P && P->window() == W->window()) return true;
    return false;
}

bool LongOp::Run(QWidget *Parent, const QString &Title, const std::function<void(const Step &)> &Work)
{
    g_Running.insert(Parent);
    struct Out { const QWidget *P; ~Out() { g_Running.erase(g_Running.find(P)); } } Leave{ Parent };
    Waiting Dlg(Parent);
    Dlg.setObjectName("longOpProgress");
    Dlg.setWindowTitle(Title);
    Dlg.setWindowModality(Qt::WindowModal);
    auto *L = new QVBoxLayout(&Dlg);
    auto *What = new QLabel(Title + "…", &Dlg);
    What->setObjectName("longOpWhat");
    What->setMinimumWidth(420);
    auto *Bar = new QProgressBar(&Dlg);
    Bar->setRange(0, 0);                      // busy until the work first says where it is
    auto *Stop = new QPushButton("Cancel", &Dlg);
    Stop->setObjectName("longOpCancel");
    L->addWidget(What);
    L->addWidget(Bar);
    L->addWidget(Stop, 0, Qt::AlignRight);
    auto Cancel = std::make_shared<std::atomic<bool>>(false);
    const auto AskStop = [Cancel, What, Stop] {
        if (!Stop->isEnabled() || Cancel->exchange(true)) return;
        Stop->setEnabled(false);
        What->setText("Stopping…");
    };
    Dlg.AskStop = AskStop;
    QObject::connect(Stop, &QPushButton::clicked, &Dlg, AskStop);

    //The work's end as the worker says it (its completion is queued, and a quitting app runs no more of the queue).
    struct Ended { std::mutex M; std::condition_variable Cv; bool Yes = false; };
    auto Over = std::make_shared<Ended>();
    QEventLoop Loop;
    bool Finished = false;
    AsyncWork::Run(&Dlg, [Work, Cancel, What, Bar, Stop, Over](const AsyncWork::Job &J) {
        Step S;
        S.Report = [J, Cancel, What, Bar](const QString &Item, int Done, int Total, unsigned long long Bytes, unsigned long long TotalBytes) {
            J.Post([Cancel, What, Bar, T = Describe(Item, Done, Total, Bytes, TotalBytes), Done, Total, Bytes, TotalBytes] {
                if (Cancel->load()) return;   // "Stopping…" stays
                What->setText(T);
                Bar->setRange(0, 1000);
                Bar->setValue(TotalBytes > 0 ? int(1000.0 * double(Bytes) / double(TotalBytes))
                                             : Total > 0 ? int(1000.0 * double(Done) / double(Total)) : 0);
            });
        };
        S.Cancelled = [Cancel] { return Cancel->load(); };
        S.CanCancel = [J, Stop, Cancel](bool On) { J.Post([Stop, On, Cancel] { Stop->setEnabled(On && !Cancel->load()); }); };
        Work(S);
        if (QThread::currentThread() != qApp->thread()) ++g_RanOnWorker;
        { std::lock_guard<std::mutex> G(Over->M); Over->Yes = true; }
        Over->Cv.notify_all();
    }, [&Finished, &Loop] { Finished = true; Loop.quit(); });
    Dlg.show();
    if (!Finished) Loop.exec();
    if (!Finished)
    {   //The app is quitting (QCoreApplication::exit ends every loop, and none runs again): the work is asked to stop
        //at its next safe point and waited for here, the window no longer painted. It runs on what the caller lent it,
        //and a save stopped halfway leaves nodes naming nodes not written; the quit goes on once Run has returned.
        Cancel->store(true);
        std::unique_lock<std::mutex> G(Over->M);
        Over->Cv.wait(G, [&] { return Over->Yes; });
    }
    Dlg.hide();
    return !Cancel->load();
}

int LongOp::DebugRanOnWorker() { return g_RanOnWorker.load(); }
