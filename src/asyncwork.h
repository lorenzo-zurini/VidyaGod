#ifndef ASYNCWORK_H
#define ASYNCWORK_H

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
namespace AsyncWork {

inline void Run(QObject *Ctx, std::function<void()> Work, std::function<void()> Done = {})
{
    auto Alive = std::make_shared<std::atomic<bool>>(true);
    QObject::connect(Ctx, &QObject::destroyed, Ctx, [Alive]() { Alive->store(false); }, Qt::DirectConnection);
    std::thread([Ctx, Alive, W = std::move(Work), D = std::move(Done)]() mutable {
        W();
        if (D && Alive->load())
            QMetaObject::invokeMethod(Ctx, [Alive, D2 = std::move(D)]() { if (Alive->load()) D2(); },
                                      Qt::QueuedConnection);
    }).detach();
}

} // namespace AsyncWork

#endif // ASYNCWORK_H
