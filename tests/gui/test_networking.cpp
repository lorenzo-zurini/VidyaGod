// End-to-end test of the networking opt-in: ticking Settings → IPFS "Enable networking" actually starts the
// embedded node (Available() flips true), and the IPFS tab is functional (its full UI is built, not the old
// "unavailable" stub). Uses VIDYAGOD_IPFS_OFFLINE so the node opens its repo locally without joining a swarm —
// fast + deterministic. Runs under offscreen QPA; the node repo lives in a temp dir (AppPaths::SetDataRoot).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QCheckBox>
#include <QLabel>

#include "mainwindow.h"
#include "ipfstab.h"
#include "ipfswrapper.h"
#include "apppaths.h"

using json = nlohmann::ordered_json;

class NetworkingTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("VIDYAGOD_IPFS_OFFLINE", "1"); }   // local node, no network

    // Enabling networking starts the node, and the IPFS tab has a live status label (not the dead stub).
    void enabling_networking_starts_node_and_makes_tab_live()
    {
        QVERIFY(!IpfsWrapper::Available());                          // off to start

        QTemporaryDir d; QVERIFY(d.isValid());
        QDir appDir(d.path());
        AppPaths::SetDataRoot(d.path().toStdString());               // node repo at <temp>/ipfs
        json cfg = json{{"Settings", json::object()}};

        MainWindow mw(&cfg, &appDir);
        mw.startup();                                               // networking off → no node started
        QApplication::processEvents();
        QVERIFY(!IpfsWrapper::Available());

        // The IPFS tab must have built its real UI (a status QLabel) even though the node is off — the old bug built
        // a stub with no controls when the node wasn't up at construction.
        IpfsTab * tab = mw.findChild<IpfsTab *>();
        QVERIFY(tab != nullptr);
        QVERIFY(!tab->findChildren<QLabel *>().isEmpty());

        // Tick Settings → IPFS "Enable networking" → drives the model → MainWindow starts the node off-thread.
        QCheckBox * netCb = nullptr;
        for (QCheckBox * cb : mw.findChildren<QCheckBox *>())
            if (cb->text().contains("Enable networking")) netCb = cb;
        QVERIFY(netCb != nullptr);
        netCb->setChecked(true);

        QTRY_VERIFY_WITH_TIMEOUT(IpfsWrapper::Available(), 15000);   // node came up

        IpfsWrapper::StopNode();                                     // clean up (don't leak the node into other tests)
    }
};

QTEST_MAIN(NetworkingTest)
#include "test_networking.moc"
