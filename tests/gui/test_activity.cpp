// test_activity — the activity log and the main window's activity strip: background work reports its progress, its
// Cancel and its result in one place, and nothing it does pops a box from a page the user is not on.
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QFrame>
#include <QTemporaryDir>
#include <QTimer>

#include "activitylog.h"
#include "activitystrip.h"
#include "appmodel.h"
#include "downloadmanager.h"
#include "ipfsmodel.h"
#include "ipfstab.h"
#include "sourcespage.h"

#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

class ActivityTest : public QObject
{
    Q_OBJECT
    //Every message box that appears is counted and closed (a modal one would hold the test forever).
    int Boxes = 0;
    QTimer BoxWatch;
    const ActivityLog::Entry * Newest(const QString & Kind)
    {
        const auto All = ActivityLog::Get().entries();
        for (int I = (int)All.size() - 1; I >= 0; --I)
            if (All[I].Kind == Kind) return ActivityLog::Get().find(All[I].Id);
        return nullptr;
    }

private slots:
    void init()
    {
        ActivityLog::Get().dismissFinished();
        Boxes = 0;
        BoxWatch.setInterval(30);
        connect(&BoxWatch, &QTimer::timeout, this, [this] {
            for (QWidget * W : QApplication::topLevelWidgets())
                if (auto * B = qobject_cast<QMessageBox *>(W); B && B->isVisible()) { ++Boxes; B->done(QMessageBox::Ok); }
        }, Qt::UniqueConnection);
        BoxWatch.start();
    }

    // A job runs, says where it is, finishes with a result that stays until dismissed; a failure is counted until the
    // list is looked at; only a running job's Cancel is asked, and a finished one cannot be cancelled or dismissed twice.
    // Teeth: a finish that drops the result; a failure not counted; dismiss of a running job; cancel after the end.
    void a_job_reports_and_its_result_stays_until_dismissed()
    {
        ActivityLog & L = ActivityLog::Get();
        int Asked = 0;
        const int Id = L.start("Test", "Job", [&Asked] { ++Asked; });
        QCOMPARE(L.running(), 1);
        QVERIFY(L.find(Id)->CanCancel);
        L.progress(Id, 0.5, "half");
        QCOMPARE(L.find(Id)->Fraction, 0.5);
        QCOMPARE(L.find(Id)->Detail, QString("half"));
        L.dismiss(Id);                               // running: stays
        QVERIFY(L.find(Id));
        L.cancel(Id);
        QCOMPARE(Asked, 1);
        L.finish(Id, false, "it broke");
        QCOMPARE(L.running(), 0);
        QVERIFY(L.find(Id)->Failed && !L.find(Id)->CanCancel);
        QCOMPARE(L.find(Id)->Detail, QString("it broke"));
        QCOMPARE(L.failedUnseen(), 1);
        L.cancel(Id);
        QCOMPARE(Asked, 1);                          // its Cancel went with it
        L.progress(Id, 0.9, "late");
        QCOMPARE(L.find(Id)->Detail, QString("it broke"));
        L.markSeen();
        QCOMPARE(L.failedUnseen(), 0);
        L.dismiss(Id);
        QVERIFY(!L.find(Id));
        const int N = L.note("Test", "Note", true, "fine");
        QVERIFY(L.find(N) && !L.find(N)->Running && !L.find(N)->Failed);
    }

    // What the background does is in the log: a download (Cancel routed to it, its end said as cancelled), a seed folder
    // (n of m, mismatches a failure), a force recheck, a source sync and a source failure, a failed uninstall — and the
    // strip says what runs. No box opens, from the Downloads tab or the Sources page, for any of it. Teeth: a source
    // failure, a seed result or a recheck result shown as a box again; a download without its Cancel; a seed's
    // mismatches not a failure; the strip not following the log.
    void the_background_reports_into_the_log_and_opens_no_box()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (D.path() + "/LIBRARY").toStdString()}}}}}};
        QDir AppDir(D.path());
        AppModel Model(&Cfg, &AppDir);
        IpfsModel Ipfs(Model);
        DownloadManager Downloads(Model, Ipfs);
        ActivityStrip::Watch(Model, Ipfs, Downloads);
        ActivityStrip Strip;
        IpfsTab Tab(Ipfs);
        SourcesPage Sources(Model);
        Tab.show(); Sources.show(); Strip.show();
        ActivityLog & L = ActivityLog::Get();

        emit Downloads.downloadStarted("4242");
        const ActivityLog::Entry * Dl = Newest("Downloads");
        QVERIFY(Dl && Dl->Running && Dl->CanCancel);
        QCOMPARE(Strip.summary(), QString("1 task"));
        emit Downloads.downloadProgress("4242", 40.0);
        QCOMPARE(Newest("Downloads")->Fraction, 0.4);
        L.cancel(Newest("Downloads")->Id);
        emit Downloads.downloadFinished("4242");
        QCOMPARE(Newest("Downloads")->Detail, QString("Cancelled"));

        emit Ipfs.seedStarted(D.path() + "/Lib");
        QVERIFY(Newest("Seed folder")->CanCancel);
        emit Ipfs.seedProgress(1, 4);
        QCOMPARE(Newest("Seed folder")->Fraction, 0.25);
        QCOMPARE(Newest("Seed folder")->Detail, QString("1 of 4 files"));
        emit Ipfs.seedFinished(3, 1);
        QVERIFY(Newest("Seed folder")->Failed);
        QVERIFY2(Newest("Seed folder")->Detail.contains("changed since publish"), qPrintable(Newest("Seed folder")->Detail));
        QVERIFY2(Strip.summary().contains("1 failed"), qPrintable(Strip.summary()));

        emit Ipfs.recheckStarted(2);
        QVERIFY(Newest("Force recheck")->Running && Newest("Force recheck")->CanCancel);
        emit Ipfs.recheckFinished(1, 0);
        QVERIFY2(Newest("Force recheck")->Detail.contains("Stopped after 1 of 2"), qPrintable(Newest("Force recheck")->Detail));

        emit Model.sourcesSyncStarted();
        emit Model.sourcesSyncStarted();            // asked again while running: one job
        QCOMPARE(L.running(), 1);
        emit Model.sourcesSyncFinished(false);
        QCOMPARE(Newest("Sources")->Detail, QString("Up to date"));
        emit Model.packageSourceFailed("the node is offline");
        QVERIFY(Newest("Sources")->Failed && Newest("Sources")->Detail == "the node is offline");
        emit Model.packageRemoveFailed("no trash here");
        QVERIFY(Newest("Uninstall")->Failed);

        Strip.openList();
        QVERIFY(Strip.list() && Strip.list()->isVisible());
        bool Dismiss = false;
        for (auto * B : Strip.list()->findChildren<QToolButton *>()) if (B->accessibleName().startsWith("Dismiss")) Dismiss = true;
        QVERIFY(Dismiss);
        QCOMPARE(L.failedUnseen(), 0);               // looked at

        QTest::qWait(200);
        QCOMPARE(Boxes, 0);
    }
};

QTEST_MAIN(ActivityTest)
#include "test_activity.moc"
