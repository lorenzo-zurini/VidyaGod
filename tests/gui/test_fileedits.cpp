// test_fileedits.cpp — the FileEdit apply layer.
//
// This is where the Worms 4 Mayhem bug actually lived: a ConfigWrite scheduled in the BASE pass, targeting a file
// that only exists once the runtime is mounted, so the resolution was NEVER written and the game ran at its
// shipped 800x600 — visible only as a wrong aspect ratio. Every silent-failure mode this layer has is pinned here.

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fileedits.h"
#include "commonutils.h"

namespace {

std::string ReadAll(const std::string &Path)
{
    std::ifstream In(Path);
    std::stringstream SS;
    SS << In.rdbuf();
    return SS.str();
}

void Write(const std::string &Path, const std::string &Text)
{
    std::ofstream Out(Path);
    Out << Text;
}

// Collect the WARN/ERR a call produces, so "did it complain?" is itself testable.
struct Complaints
{
    std::vector<std::string> Lines;
    Complaints()  { SetLogCallback([this](LogLevel L, const std::string &C, const std::string &M){
                        if (L == LogLevel::WARN || L == LogLevel::ERR) Lines.push_back(C + "|" + M); }); }
    ~Complaints() { ClearLogCallback(); }
    bool Mentions(const std::string &Needle) const
    {
        for (const auto &L : Lines) if (L.find(Needle) != std::string::npos) return true;
        return false;
    }
};

} // namespace

class FileEditsTest : public QObject
{
    Q_OBJECT
private slots:

    // ---- ConfigWrite: the mode that broke Worms 4 ----

    void configwrite_replaces_the_matching_line_and_preserves_the_rest()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/Default.cfg").toStdString();
        Write(P, "/W:800\n/H:600\n/FS\n");
        QVERIFY(FileEdits::ConfigWrite("/W:", "1920", P));
        QCOMPARE(ReadAll(P), std::string("/W:1920\n/H:600\n/FS\n"));
    }

    // The file must EXIST — it is read, rewritten, and written back. This is the exact Worms 4 failure: a
    // base-pass ConfigWrite targets content that is not mounted yet.
    void configwrite_on_a_missing_file_fails_loudly()
    {
        QTemporaryDir Dir;
        Complaints C;
        const std::string P = (Dir.path() + "/does-not-exist.cfg").toStdString();
        QVERIFY2(!FileEdits::ConfigWrite("/W:", "1920", P), "editing a missing file must FAIL, not silently pass");
        QVERIFY2(C.Mentions("Could not open"), "the failure must be reported");
    }

    // A key that matches nothing rewrites the file unchanged and returns success. That is a typo, or a config
    // whose format moved on, and it must not pass in silence — it is the same class of bug as the base-pass one.
    void configwrite_with_no_matching_key_warns()
    {
        QTemporaryDir Dir;
        Complaints C;
        const std::string P = (Dir.path() + "/patch.ini").toStdString();
        Write(P, "ResX = 0\nResY = 0\n");
        QVERIFY(FileEdits::ConfigWrite("ResZ = ", "1", P));       // no such key
        QCOMPARE(ReadAll(P), std::string("ResX = 0\nResY = 0\n")); // unchanged
        QVERIFY2(C.Mentions("matched NO line"), "a key that matches nothing must warn — it silently did nothing");
    }

    // The key is a literal line PREFIX, spaces included. "ResX=" does not match "ResX = 0" — the kind of detail
    // that silently costs an evening.
    void configwrite_key_is_a_literal_prefix_including_spaces()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/patch.ini").toStdString();
        Write(P, "ResX = 0\n");
        {
            Complaints C;
            QVERIFY(FileEdits::ConfigWrite("ResX=", "1920", P));   // wrong spacing
            QVERIFY2(C.Mentions("matched NO line"), "wrong spacing must be reported, not silently ignored");
        }
        QCOMPARE(ReadAll(P), std::string("ResX = 0\n"));
        QVERIFY(FileEdits::ConfigWrite("ResX = ", "1920", P));     // right spacing
        QCOMPARE(ReadAll(P), std::string("ResX = 1920\n"));
    }

    void configwrite_replaces_every_matching_line()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/dup.cfg").toStdString();
        Write(P, "K:a\nother\nK:b\n");
        QVERIFY(FileEdits::ConfigWrite("K:", "z", P));
        QCOMPARE(ReadAll(P), std::string("K:z\nother\nK:z\n"));
    }

    void configwrite_accepts_an_empty_value()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/e.cfg").toStdString();
        Write(P, "Name=Player\n");
        QVERIFY(FileEdits::ConfigWrite("Name=", "", P));
        QCOMPARE(ReadAll(P), std::string("Name=\n"));
    }

    // A file with no trailing newline must not lose its last line.
    void configwrite_handles_a_file_without_a_trailing_newline()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/nonl.cfg").toStdString();
        Write(P, "A:1\nB:2");
        QVERIFY(FileEdits::ConfigWrite("A:", "9", P));
        QCOMPARE(ReadAll(P), std::string("A:9\nB:2\n"));
    }

    void configwrite_leaves_an_empty_file_empty_and_warns()
    {
        QTemporaryDir Dir;
        Complaints C;
        const std::string P = (Dir.path() + "/empty.cfg").toStdString();
        Write(P, "");
        QVERIFY(FileEdits::ConfigWrite("K:", "v", P));
        QCOMPARE(ReadAll(P), std::string(""));
        QVERIFY(C.Mentions("matched NO line"));
    }

    // ---- Overwrite / AppendLine: the modes that CREATE, which is why they work in the base pass ----

    void overwrite_creates_a_file_that_does_not_exist()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/new/deep/marker").toStdString();
        QVERIFY2(FileEdits::FileOverwrite("hello", P),
                 "Overwrite must create the file (that is why it is safe in the base pass, unlike ConfigWrite)");
        QCOMPARE(ReadAll(P), std::string("hello"));
    }

    void overwrite_replaces_existing_content_entirely()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/f").toStdString();
        Write(P, "old content that is longer");
        QVERIFY(FileEdits::FileOverwrite("new", P));
        QCOMPARE(ReadAll(P), std::string("new"));
    }

    void appendline_creates_then_appends_without_clobbering()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/log.txt").toStdString();
        QVERIFY(FileEdits::AppendLine("first", P));
        QVERIFY(FileEdits::AppendLine("second", P));
        const std::string Got = ReadAll(P);
        QVERIFY2(Got.find("first") != std::string::npos, "the first line must survive the second append");
        QVERIFY2(Got.find("second") != std::string::npos, "the appended line must be present");
    }

    // A path that cannot be written (a directory where a file should be) must fail rather than silently no-op.
    void writing_over_a_directory_fails_loudly()
    {
        QTemporaryDir Dir;
        const std::string P = (Dir.path() + "/adir").toStdString();
        std::filesystem::create_directory(P);
        Complaints C;
        QVERIFY2(!FileEdits::FileOverwrite("x", P), "writing a file over a directory must fail");
        QVERIFY(!C.Lines.empty());
    }
};

QTEST_MAIN(FileEditsTest)
#include "test_fileedits.moc"
