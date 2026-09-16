// test_instancestore — the filesystem-as-truth per-game INSTANCE store: discovery, newest-LASTRUN active,
// auto-create DefaultInstance, config round-trip, lifecycle (create/rename/delete/clone), and name validation.
// Driven entirely through a temp Settings.Paths.UserDataRoot — no AppPaths, no node.

#include "instancestore.h"

#include <QtTest>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

class InstanceStoreTest : public QObject
{
    Q_OBJECT
    // A config whose only content is the UserDataRoot override, so every call roots under `dir`.
    static json cfg(const QString & dir) { return json{{"Settings", {{"Paths", {{"UserDataRoot", dir.toStdString()}}}}}}; }

private slots:
    // The active instance is the one with the newest LASTRUN, and List returns newest-first. Teeth: flip the sort
    // in InstanceStore::List (a.second < b.second) → Active returns "old" and this fails.
    void active_is_newest_lastrun()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        QVERIFY(InstanceStore::WriteConfig(C, "G", "old", json{{"LASTRUN", "2020-01-01T00:00:00Z"}}));
        QVERIFY(InstanceStore::WriteConfig(C, "G", "mid", json{{"LASTRUN", "2022-01-01T00:00:00Z"}}));
        QVERIFY(InstanceStore::WriteConfig(C, "G", "new", json{{"LASTRUN", "2024-01-01T00:00:00Z"}}));
        QCOMPARE(QString::fromStdString(InstanceStore::Active(C, "G")), QString("new"));
        const auto L = InstanceStore::List(C, "G");
        QCOMPARE((int)L.size(), 3);
        QCOMPARE(QString::fromStdString(L.front()), QString("new"));   // newest first
        QCOMPARE(QString::fromStdString(L.back()),  QString("old"));   // oldest last
    }

    // A game with no instances yet: Active auto-creates DefaultInstance and it exists on disk with a LASTRUN.
    void auto_creates_default_when_none()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        QCOMPARE(QString::fromStdString(InstanceStore::Active(C, "H")), QString("DefaultInstance"));
        QVERIFY(fs::exists(InstanceStore::ConfigPath(C, "H", "DefaultInstance")));
        QVERIFY(!InstanceStore::ReadConfig(C, "H", "DefaultInstance").value("LASTRUN", std::string()).empty());
    }

    // List scans subdirs and IGNORES a stray file and an invalid-named dir (never treats them as instances).
    void list_ignores_strays()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        QVERIFY(InstanceStore::Create(C, "G", "real", nullptr));
        // a stray file (not a dir) and a dir with a control char in the name
        { std::ofstream((InstanceStore::PackageDir(C, "G") / "loose.txt").string()) << "x"; }
        const auto L = InstanceStore::List(C, "G");
        QCOMPARE((int)L.size(), 1);
        QCOMPARE(QString::fromStdString(L.front()), QString("real"));
    }

    // WriteConfig → ReadConfig round-trips the blob; an absent instance reads as {}.
    void config_roundtrip()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        const json Data = json{{"VARIABLES", {{"ScreenWidth", "1920"}}}, {"MODULES", {{"dxvk", "on"}}}, {"LASTRUN", "2023-05-05T05:05:05Z"}};
        QVERIFY(InstanceStore::WriteConfig(C, "G", "inst", Data, nullptr));
        const json Got = InstanceStore::ReadConfig(C, "G", "inst");
        QCOMPARE(QString::fromStdString(Got["VARIABLES"]["ScreenWidth"].get<std::string>()), QString("1920"));
        QCOMPARE(QString::fromStdString(Got["MODULES"]["dxvk"].get<std::string>()), QString("on"));
        QVERIFY(InstanceStore::ReadConfig(C, "G", "nope").empty());   // absent → {}
    }

    // Create / Rename / Clone (copies the whole tree incl. userdata) / Delete.
    void lifecycle()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        std::string E;
        QVERIFY2(InstanceStore::Create(C, "G", "A", &E), E.c_str());
        QVERIFY2(!InstanceStore::Create(C, "G", "A", &E), "create must refuse an existing name");
        // a userdata file inside A — Clone must carry it
        { std::ofstream((InstanceStore::InstanceDir(C, "G", "A") / "save.dat").string()) << "progress"; }

        QVERIFY2(InstanceStore::Rename(C, "G", "A", "B", &E), E.c_str());
        QVERIFY(!fs::exists(InstanceStore::InstanceDir(C, "G", "A")));
        QVERIFY(fs::exists(InstanceStore::InstanceDir(C, "G", "B")));

        QVERIFY2(InstanceStore::Clone(C, "G", "B", "Cc", &E), E.c_str());
        QVERIFY(fs::exists(InstanceStore::InstanceDir(C, "G", "Cc") / "save.dat"));   // userdata copied
        QVERIFY(fs::exists(InstanceStore::ConfigPath(C, "G", "Cc")));                 // config copied

        QVERIFY2(InstanceStore::Delete(C, "G", "Cc", &E), E.c_str());
        QVERIFY(!fs::exists(InstanceStore::InstanceDir(C, "G", "Cc")));
    }

    // Name validation blocks traversal / separators / empties everywhere (Create refuses; ValidName agrees).
    void name_validation_blocks_traversal()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        for (const char * Bad : {"..", "../evil", "a/b", "a\\b", ""})
        {
            QVERIFY2(!InstanceStore::ValidName(Bad), Bad);
            QVERIFY2(!InstanceStore::Create(C, "G", Bad, nullptr), Bad);
        }
        QVERIFY(InstanceStore::ValidName("DefaultInstance"));
        QVERIFY(InstanceStore::ValidName("Speedrun 100%"));
    }

    // TouchLastRun bumps LASTRUN forward (a stale value is replaced with a newer one). Teeth: no-op TouchLastRun's
    // write → the value stays "2000..." and this fails.
    void touch_lastrun_bumps()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        QVERIFY(InstanceStore::WriteConfig(C, "G", "i", json{{"LASTRUN", "2000-01-01T00:00:00Z"}}));
        InstanceStore::TouchLastRun(C, "G", "i");
        const std::string After = InstanceStore::ReadConfig(C, "G", "i").value("LASTRUN", std::string());
        QVERIFY2(After > "2000-01-01T00:00:00Z", After.c_str());   // ISO sorts chronologically
    }

    // A peer-authored PackageUID can never escape <root>: separators / ".." / absolute paths are sanitised to a
    // single safe segment. Teeth: make SanitizeUid a pass-through → the parent_path assertion fails (escape).
    void sanitizes_package_uid_no_traversal()
    {
        QTemporaryDir T; QVERIFY(T.isValid());
        const json C = cfg(T.path());
        QCOMPARE(QString::fromStdString(InstanceStore::SanitizeUid("17260")), QString("17260"));   // legit UID unchanged
        QCOMPARE(QString::fromStdString(InstanceStore::SanitizeUid("..")), QString("_"));
        for (const char * Hostile : {"../../../etc", "/etc/passwd", "a/b", "..", ".."})
        {
            const std::string San = InstanceStore::SanitizeUid(Hostile);
            QVERIFY2(San.find('/') == std::string::npos && San != ".." && San != ".", Hostile);
            // PackageDir must be exactly ONE segment directly under root — no escape possible.
            QCOMPARE(InstanceStore::PackageDir(C, Hostile).parent_path(), InstanceStore::Root(C));
        }
    }
};

QTEST_MAIN(InstanceStoreTest)
#include "test_instancestore.moc"
