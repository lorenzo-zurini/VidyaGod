#ifndef ASYNCWORK_H
#define ASYNCWORK_H

#include <QCoreApplication>
#include <QObject>
#include <QMetaObject>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

//Detached-worker helper with the lifetime guard DownloadManager pioneered (its async CID-size updater),
//extracted so every "run this off the GUI thread, then touch widgets" site shares ONE audited pattern
//instead of hand-rolling std::thread(...).detach() with a captured `this`.
//
//  AsyncWork::Run(this,
//      []            { /* heavy work — NO widget/model access here */ },
//      [this, ...]   { /* completion — runs queued on Ctx's thread, only if Ctx still alive */ });
//
//The guard flips in Ctx's destructor (destroyed signal, direct connection, fires on Ctx's thread), so
//a completion queued after teardown is dropped instead of dereferencing a dead widget.
//
//The completion is posted to qApp, NOT to Ctx. Posting to Ctx has a race the guard cannot close from the
//worker thread: invokeMethod dereferences its target to read the thread affinity, and ~Ctx can run between
//the `Alive->load()` check and that dereference. qApp outlives every Ctx, and the re-check INSIDE the lambda
//then happens on the GUI thread, where the destructor cannot be concurrent with it. This was a narrow window
//while the only users were quick jobs; a package conversion runs for MINUTES on an editor the user can close.
namespace AsyncWork {

//What a worker gets to report along the way (progress, rows as they land): Post queues F onto the GUI thread, run
//there only while Ctx lives — the completion's own guard; Live says whether Ctx still does, so a long job stops
//early. The work itself never touches Ctx: whatever reaches it goes through Post.
class Job
{
public:
    explicit Job(std::shared_ptr<std::atomic<bool>> A) : Alive(std::move(A)) {}
    bool Live() const { return Alive->load(); }
    void Post(std::function<void()> F) const
    {
        QMetaObject::invokeMethod(qApp, [A = Alive, F = std::move(F)]() { if (A->load()) F(); }, Qt::QueuedConnection);
    }
private:
    std::shared_ptr<std::atomic<bool>> Alive;
};

inline void Run(QObject *Ctx, std::function<void(const Job &)> Work, std::function<void()> Done = {})
{
    auto Alive = std::make_shared<std::atomic<bool>>(true);
    QObject::connect(Ctx, &QObject::destroyed, Ctx, [Alive]() { Alive->store(false); }, Qt::DirectConnection);
    std::thread([J = Job(Alive), W = std::move(Work), D = std::move(Done)]() mutable {
        W(J);
        if (D) J.Post(std::move(D));
    }).detach();
}

inline void Run(QObject *Ctx, std::function<void()> Work, std::function<void()> Done = {})
{
    Run(Ctx, std::function<void(const Job &)>([W = std::move(Work)](const Job &) { W(); }), std::move(Done));
}

} // namespace AsyncWork

#endif // ASYNCWORK_H
