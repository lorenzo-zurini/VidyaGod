// Headless tests for AuthoringSession's UI-agnostic file-capture mechanism (no container needed): churn
// filtering, write-delta enumeration, re-rooting copy, and the VFSDirLayer json shape.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <string>

#include "authoringsession.h"

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {
void touch(const fs::path &P, const std::string &Body = "x")
{
    fs::create_directories(P.parent_path());
    std::ofstream(P) << Body;
}
}

class AuthoringSessionTest : public QObject
{
    Q_OBJECT
private slots:

    // IsChurn: registry hives + wine-managed trees are filtered; real content is not.
    void is_churn()
    {
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/user.reg"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/windows/system32/foo.dll"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/Windows/win.ini"));
        QVERIFY(AuthoringSession::IsChurn("pfx/dosdevices/c:"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/Temp/setup.log"));
        QVERIFY(AuthoringSession::IsChurn("pfx/.update-timestamp"));
        // Real game content survives.
        QVERIFY(!AuthoringSession::IsChurn("pfx/drive_c/Morrowind/Morrowind.exe"));
        QVERIFY(!AuthoringSession::IsChurn("pfx/drive_c/Morrowind/Data Files/Morrowind.esm"));
    }

    // EnumerateDelta walks a writelayer-like tree, returns sorted content-file rel paths, churn excluded.
    void enumerate_delta_excludes_churn()
    {
        QTemporaryDir Tmp; QVERIFY(Tmp.isValid());
        const fs::path WL = Tmp.path().toStdString();
        touch(WL / "pfx/drive_c/Morrowind/Morrowind.exe");
        touch(WL / "pfx/drive_c/Morrowind/Data Files/Morrowind.esm");
        touch(WL / "pfx/drive_c/user.reg");                    // churn
        touch(WL / "pfx/drive_c/windows/system32/d3d8.dll");   // churn

        const std::vector<std::string> Got = AuthoringSession::EnumerateDelta(WL);
        QCOMPARE((int)Got.size(), 2);
        QCOMPARE(Got[0], std::string("pfx/drive_c/Morrowind/Data Files/Morrowind.esm"));
        QCOMPARE(Got[1], std::string("pfx/drive_c/Morrowind/Morrowind.exe"));
    }

    // CopySelection re-roots by stripping the content-root prefix, so a capture lands under the layer's TARGET.
    void copy_selection_restrips_prefix()
    {
        QTemporaryDir Tmp; QVERIFY(Tmp.isValid());
        const fs::path WL = fs::path(Tmp.path().toStdString()) / "wl";
        const fs::path Dest = fs::path(Tmp.path().toStdString()) / "dest";
        touch(WL / "pfx/drive_c/Morrowind/Morrowind.exe");
        touch(WL / "pfx/drive_c/Morrowind/Data Files/Morrowind.esm");

        // Capture the Morrowind dir, stripping "pfx/drive_c" so files land at Morrowind/… under the layer.
        const int N = AuthoringSession::CopySelection(WL, {"pfx/drive_c/Morrowind"}, Dest, "pfx/drive_c");
        QCOMPARE(N, 2);
        QVERIFY(fs::exists(Dest / "Morrowind/Morrowind.exe"));
        QVERIFY(fs::exists(Dest / "Morrowind/Data Files/Morrowind.esm"));
        QVERIFY(!fs::exists(Dest / "pfx"));   // the prefix path was stripped
    }

    // MakeDirLayer emits the VFSDirLayer shape; TARGET omitted when empty.
    void make_dir_layer_shape()
    {
        const json A = AuthoringSession::MakeDirLayer("morrowind_files", "drive_c/Morrowind");
        QCOMPARE(A.value("TYPE", std::string()), std::string("VFSDirLayer"));
        QCOMPARE(A.value("PATH", std::string()), std::string("morrowind_files"));
        QCOMPARE(A.value("TARGET", std::string()), std::string("drive_c/Morrowind"));

        const json B = AuthoringSession::MakeDirLayer("morrowind_files", "");
        QVERIFY(!B.contains("TARGET"));
    }
};

QTEST_MAIN(AuthoringSessionTest)
#include "test_authoringsession.moc"
