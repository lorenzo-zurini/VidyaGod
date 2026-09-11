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

inline void Run(QObject *Ctx, std::function<void()> Work, std::function<void()> Done = {})
{
    auto Alive = std::make_shared<std::atomic<bool>>(true);
    QObject::connect(Ctx, &QObject::destroyed, Ctx, [Alive]() { Alive->store(false); }, Qt::DirectConnection);
    std::thread([Alive, W = std::move(Work), D = std::move(Done)]() mutable {
        W();
        if (D)
            QMetaObject::invokeMethod(qApp, [Alive, D2 = std::move(D)]() { if (Alive->load()) D2(); },
                                      Qt::QueuedConnection);
    }).detach();
}

} // namespace AsyncWork

#endif // ASYNCWORK_H
