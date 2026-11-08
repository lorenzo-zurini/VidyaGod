// test_asyncwork.cpp — the one off-thread idiom (base/asyncwork.h): work on a worker, everything that reaches the
// context posted through the GUI thread and dropped once the context is gone. Every model's workers rely on it.

#include <QtTest/QtTest>

#include "asyncwork.h"

#include <condition_variable>
#include <mutex>

class AsyncWorkTest : public QObject
{
    Q_OBJECT
private slots:
    // What a worker posts runs on the GUI thread, and the completion after it. Teeth: run the post on the worker.
    void a_post_runs_on_the_gui_thread_while_its_context_lives()
    {
        QObject Ctx;
        std::atomic<bool> Posted{false}, Done{false};
        QThread *PostedOn = nullptr;
        AsyncWork::Run(&Ctx, [&](const AsyncWork::Job &J) {
            J.Post([&] { PostedOn = QThread::currentThread(); Posted = true; });
        }, [&] { Done = true; });
        QTRY_VERIFY(Done.load());
        QVERIFY(Posted.load());
        QCOMPARE(PostedOn, QCoreApplication::instance()->thread());
    }

    // A context destroyed while its worker runs: the worker sees it (Live), and neither what it posts nor the
    // completion runs. Teeth: run a post whatever became of the context; never flip Live.
    void a_post_after_its_context_died_is_dropped()
    {
        auto *Ctx = new QObject;
        std::mutex M;
        std::condition_variable Cv;
        bool Go = false;
        std::atomic<bool> SawDead{false}, WorkEnded{false}, Posted{false}, Done{false};
        AsyncWork::Run(Ctx, [&](const AsyncWork::Job &J) {
            { std::unique_lock<std::mutex> L(M); Cv.wait(L, [&] { return Go; }); }
            SawDead = !J.Live();
            J.Post([&] { Posted = true; });
            WorkEnded = true;
        }, [&] { Done = true; });
        delete Ctx;
        { std::lock_guard<std::mutex> L(M); Go = true; }
        Cv.notify_all();
        QTRY_VERIFY(WorkEnded.load());
        QTest::qWait(50);   // anything posted has had its turn
        QVERIFY(SawDead.load());
        QVERIFY(!Posted.load());
        QVERIFY(!Done.load());
    }
};

QTEST_MAIN(AsyncWorkTest)
#include "test_asyncwork.moc"
