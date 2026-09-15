#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>

#include "downloadqueue.h"

// DownloadQueue's ENQUEUE-side contracts, exercised with no node: a dest already on disk satisfies a new CID
// without a fetch, and joining a Done job at a NEW dest materializes (hard-link/copy) from an existing one.
// The queue is a process-global keyed by CID, so every case uses its own CID.
//
// The review finding these teeth pin: Materialize() failures were LOGGED but the job stayed Done, so a batch —
// and --download-all on top of it — reported "full mirror complete" over a dest that was never written.
class DownloadQueueTest : public QObject
{
    Q_OBJECT

private slots:
    void aDestAlreadyOnDiskSatisfiesTheBatchWithoutANode()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        auto H = IpfsWrapper::EnqueueBatch({{"CID_DQ_SAT", A, /*Optional=*/false}});
        std::string Err;
        QVERIFY(IpfsWrapper::WaitBatch(H, &Err));
        QVERIFY(Err.empty());
    }

    void joiningADoneJobAtANewDestMaterializesFromTheExistingOne()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        const std::string B = (dir.path() + "/sub/b.bin").toStdString();   // parent created by Materialize
        { std::ofstream(A) << "the same bytes at every dest"; }
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_MAT", A, false}})));
        std::string Err;
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_MAT", B, false}}), &Err));
        std::error_code Ec;
        QVERIFY(std::filesystem::exists(B, Ec));
        QCOMPARE(std::filesystem::file_size(B, Ec), std::filesystem::file_size(A, Ec));
    }

    // THE review finding. Teeth (each caught): drop the `J.State = Job::Failed` on the EnqueueBatch materialize
    // → WaitBatch returns true over the hole; make Materialize void again → same.
    void aMaterializeFailureFailsTheBatchInsteadOfReportingAMirrorOverAHole()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        // The unwritable parent is a regular FILE — ENOTDIR on every platform and for every euid (a read-only
        // DIRECTORY is ignored by Windows and by root, which made the first version of this test Linux-non-root-only).
        const std::string RO = (dir.path() + "/ro").toStdString();
        { std::ofstream(RO) << "a file where a directory would be needed"; }
        const std::string B = RO + "/sub/b.bin";                           // create_directories under a FILE must fail
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_HOLE", A, false}})));
        std::string Err;
        const bool Ok = IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_HOLE", B, false}}), &Err);
        QVERIFY2(!Ok, "a dest that could not be written must FAIL the batch, not vanish into a log line");
        QVERIFY(!Err.empty());
        std::error_code Ec;
        QVERIFY(!std::filesystem::exists(B, Ec));
    }

    // The fetch BOUND rides the job, and unbounded is STICKY: a cover's 30 s must never cut short a game layer
    // that wants the same CID; once any requester is unbounded the job stays unbounded. This is the guard against
    // the reviewed CRITICAL: a bounded cover attempt frees its DownloadSlot at the deadline instead of a dead
    // cover holding one of the 3 slots forever. Teeth: make MergeTimeout keep the max / drop the NewJob.TimeoutMs
    // assignment → the matching compare fails.
    void theFetchBoundRidesTheJobAndUnboundedIsSticky()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string D1 = (dir.path() + "/x/a.bin").toStdString();
        (void)IpfsWrapper::EnqueueBatch({{"CID_DQ_BOUND", D1, true, 30000}});
        QCOMPARE(IpfsWrapper::DebugJobTimeoutMs("CID_DQ_BOUND"), 30000);              // a bounded cover job
        (void)IpfsWrapper::EnqueueBatch({{"CID_DQ_BOUND", (dir.path() + "/y/a.bin").toStdString(), false, 0}});
        QCOMPARE(IpfsWrapper::DebugJobTimeoutMs("CID_DQ_BOUND"), 0);                  // a game joined → unbounded
        (void)IpfsWrapper::EnqueueBatch({{"CID_DQ_BOUND", (dir.path() + "/z/a.bin").toStdString(), true, 30000}});
        QCOMPARE(IpfsWrapper::DebugJobTimeoutMs("CID_DQ_BOUND"), 0);                  // …and stays unbounded
    }

    // An OPTIONAL requester of the same holed CID must still tolerate the failure — the per-batch view.
    void anOptionalRequesterToleratesTheSameHole()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string A = (dir.path() + "/a.bin").toStdString();
        { std::ofstream(A) << "bytes"; }
        const std::string RO = (dir.path() + "/ro").toStdString();
        { std::ofstream(RO) << "a file where a directory would be needed"; }
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_OPT", A, false}})));
        std::string Err;
        const bool Ok = IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({{"CID_DQ_OPT", RO + "/sub/b.bin", /*Optional=*/true}}), &Err);
        QVERIFY2(Ok, "an optional job's failure must not abort the batch");
    }
};

QTEST_MAIN(DownloadQueueTest)
#include "test_downloadqueue.moc"
