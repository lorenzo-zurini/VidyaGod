// The command layer as a caller meets it: a ui action that opens a dialog answers at once with the dialog (not when
// someone closes it); a JSON value of the wrong shape says what shape it should have; a failure says what it logged;
// a node is found by any unique part of its CID, as a table shortens it; and a result reads as a table only when it
// is one.
#include <QtTest>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <filesystem>
#include <fstream>

#include "appmodel.h"
#include "cliapp.h"
#include "command.h"
#include "sessions.h"
#include "cli/cliapp.h"
#include "command.h"
#include "commands/cmdutil.h"
#include "commandserver.h"
#include <atomic>
#include <thread>
#include "commonutils.h"
#include "launchresolver.h"
#include "manifestmodel.h"

using json = nlohmann::ordered_json;

class CommandsTest : public QObject
{
    Q_OBJECT
private slots:
    // A path argument is the caller's: made absolute where the command is typed (a forwarded command runs in the app,
    // in the app's folder). A name-or-path one only when it names something there; "", "-" and other args as given.
    // Teeth: no absolutising (the app opened its own folder's file); MaybePath absolutising a UID.
    void a_path_argument_is_made_absolute_where_it_is_typed()
    {
        QTemporaryDir Here; QVERIFY(Here.isValid());
        const std::filesystem::path Was = std::filesystem::current_path();
        std::filesystem::current_path(Here.path().toStdString());
        { std::ofstream("there.json") << "{}"; }
        Command::Spec S;
        S.Name = "test paths";
        S.Args = { Command::AsPath(Command::Pos("file", "a file")), Command::MaybeAPath(Command::Pos("package", "a package")),
                   Command::AsPath(Command::Opt("out", "out")), Command::Opt("name", "a name") };
        const auto A = CliApp::ParseCommandArgs(S, { "sub/../x.txt", "749", "--out", "-", "--name", "y.txt" });
        const auto B = CliApp::ParseCommandArgs(S, { "x.txt", "there.json" });
        const std::string Abs = std::filesystem::current_path().lexically_normal().string();   // as the shell's folder reads
        std::filesystem::current_path(Was);
        QCOMPARE(QString::fromStdString(A["file"].get<std::string>()), QString::fromStdString(Abs + "/x.txt"));
        QCOMPARE(QString::fromStdString(A["package"].get<std::string>()), QStringLiteral("749"));   // a UID: as given
        QCOMPARE(QString::fromStdString(A["out"].get<std::string>()), QStringLiteral("-"));         // stdout
        QCOMPARE(QString::fromStdString(A["name"].get<std::string>()), QStringLiteral("y.txt"));    // not a path arg
        QCOMPARE(QString::fromStdString(B["package"].get<std::string>()), QString::fromStdString(Abs + "/there.json"));
    }

    void initTestCase()
    {
        SetLogEcho(false);
        Command::Register({ "test shape", "a test command taking JSON", "", {
            Command::WithSample(Command::Opt("v", "an object", Command::Type::Json), "{\"x\":\"text\"}") }, {}, {}, Command::Needs::Config, false,
            [](Command::Context &, const json &A) { return json{{"x", A["v"]["x"].get<std::string>()}}; }, {} });
        Command::Register({ "test check", "a test command whose check does not pass", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &, const json &) -> json { throw Command::CheckFailed(json{{"checked", 3}, {"bad", 1}}, "1 of 3 is broken"); }, {} });
        Command::Register({ "test fails", "a test command that logs why it fails", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &, const json &) -> json {
                LogErr("Test", "the helper said: no such mount");
                throw std::runtime_error("it did not mount");
            }, {} });
    }

    // A command the app runs off its thread (a fetch, a health check, a seed: minutes on a Deck, the window frozen for
    // all of them) reads a config of its own — the app goes on changing its config meanwhile — and cannot save it: the
    // copy written over the file would undo the app's changes. Teeth: hand it the app's config (it reads the change
    // made under it); let it save (the call answers ok).
    void a_command_off_the_apps_thread_reads_its_own_config_and_cannot_save()
    {
        static std::atomic<bool> Started{false}, Go{false};
        Started = false; Go = false;
        Command::Register({ "test bg read", "a background test command", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &C, const json &) {
                const std::string Before = (*C.Config).value("V", std::string());
                Started = true;
                while (!Go) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                return json{{"before", Before}, {"after", (*C.Config).value("V", std::string())}};
            }, {}, true });
        Command::Register({ "test bg save", "a background test command that saves", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &C, const json &) { CmdUtil::Save(C); return json{{"saved", true}}; }, {}, true });
        QTemporaryDir D; QVERIFY(D.isValid());
        QDir Dir(D.path());
        json Cfg = json{{"Settings", json::object()}, {"V", "as it was"}};
        AppModel Model(&Cfg, &Dir);
        Command::Context Ctx;
        Ctx.Config = &Cfg;
        Ctx.AppDataDir = &Dir;
        Ctx.Model = &Model;
        Ctx.Live = true;
        CommandServer Server(Ctx);
        std::string Err;
        QVERIFY2(Server.Start(D.path().toStdString(), &Err), Err.c_str());
        json ReadReply, SaveReply;
        std::atomic<bool> Done{false};
        std::thread Client([&] {
            CommandServer::Send(D.path().toStdString(), "test bg read", json::object(), ReadReply, 10000);
            CommandServer::Send(D.path().toStdString(), "test bg save", json::object(), SaveReply, 10000);
            Done = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(Started.load(), 5000);
        Cfg["V"] = "changed by the app";                                 // the app's thread, while the command runs
        Go = true;
        QTRY_VERIFY_WITH_TIMEOUT(Done.load(), 10000);
        Client.join();
        QCOMPARE(ReadReply["result"].value("before", std::string()), std::string("as it was"));
        QCOMPARE(ReadReply["result"].value("after", std::string()), std::string("as it was"));
        QVERIFY2(!SaveReply.value("ok", true), SaveReply.dump().c_str());
    }

    // The commands that take minutes (fetches, health and seed walks, an index build) run off the app's thread: in
    // the app, each froze the window and every other command for as long as it ran. Teeth: unmark any of them.
    void the_long_commands_run_off_the_apps_thread()
    {
        for (const char *N : { "ipfs fetch", "ipfs fetch-many", "queue enqueue", "ipfs health", "launch mirror",
                               "packages seed", "packages heal", "packages held", "catalog fresh", "catalog validate" })
        {
            const Command::Spec *S = Command::Find(N);
            QVERIFY2(S, N);
            QVERIFY2(S->Background, N);
        }
        //Off the app's thread a command has no model and no windows (only a copy of the config): one that needs them
        //would refuse to run there at all ('catalog validate' did). Teeth: declare one of them Needs::Model.
        for (const auto &[Name, S] : Command::All())
            if (S.Background) QVERIFY2(S.Requires == Command::Needs::Config, Name.c_str());
    }

    // Teeth: drop Act's watch (run the click bare) and Early is never called — the call would wait for the dialog,
    // which here the fallback timer closes, so the test fails instead of hanging.
    void a_click_that_opens_a_dialog_answers_with_it()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        QDir Dir(D.path());
        json Cfg = json{{"Settings", json::object()}};
        AppModel Model(&Cfg, &Dir);
        QWidget Win;
        auto *L = new QVBoxLayout(&Win);
        auto *Ask = new QPushButton("Ask", &Win);
        Ask->setObjectName("AskBtn");
        L->addWidget(Ask);
        bool Answered = false;
        QObject::connect(Ask, &QPushButton::clicked, &Win, [&] {
            Answered = QMessageBox::question(&Win, "Remove it?", "Remove the thing?") == QMessageBox::Yes;
        });
        Win.show();
        QTimer Fallback;   // never let a broken Act hang the test: close any dialog after 5 s
        Fallback.setSingleShot(true);
        QObject::connect(&Fallback, &QTimer::timeout, [] { if (QWidget *M = QApplication::activeModalWidget()) M->close(); });
        Fallback.start(5000);

        Command::Context C;
        C.Config = &Cfg;
        C.AppDataDir = &Dir;
        C.Model = &Model;
        C.Window = &Win;
        json Early;
        C.Early = [&](const json &R) {
            Early = R;
            // What `ui press Yes` does, from the next call: answer the dialog that is waiting.
            QTimer::singleShot(0, [] {
                if (auto *M = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) M->button(QMessageBox::Yes)->click();
            });
        };
        const json R = Command::Run("ui click", {{"control", "AskBtn"}}, C);
        QVERIFY2(Early.is_object() && Early.value("waiting", false), "the call was answered while the dialog was open");
        QCOMPARE(Early["opened"][0]["title"].get<std::string>(), std::string("Remove it?"));
        QVERIFY(Early["opened"][0]["buttons"].dump().find("Yes") != std::string::npos);
        QVERIFY(R.value("ok", false));
        QVERIFY2(Answered, "pressing Yes reached the action that asked");
    }

    // Teeth: drop the shape check and the parser's own words come back ("cannot use operator[] with a string
    // argument with array"); drop the exception rewording and they do for the number too.
    void a_wrong_json_value_says_what_it_should_be()
    {
        Command::Context C;
        const json Arr = Command::Run("test shape", {{"v", json::array({1, 2})}}, C);
        QVERIFY(!Arr.value("ok", true));
        QVERIFY2(Arr["error"].get<std::string>().find("--v takes a JSON object, like {\"x\":\"text\"}") != std::string::npos,
                 Arr["error"].get<std::string>().c_str());
        const json Num = Command::Run("test shape", {{"v", {{"x", 3}}}}, C);
        QVERIFY(!Num.value("ok", true));
        const std::string E = Num["error"].get<std::string>();
        QVERIFY2(E.find("[json.exception") == std::string::npos && E.find("is not what 'test shape' reads") != std::string::npos, E.c_str());
    }

    // Teeth: drop the recorder in Command::Run and the error is only "it did not mount".
    void a_failure_says_what_it_logged()
    {
        Command::Context C;
        const json R = Command::Run("test fails", json::object(), C);
        QCOMPARE(R["error"].get<std::string>(), std::string("it did not mount\n  the helper said: no such mount"));
    }

    // A check that did not pass fails the call and still answers with what it found. Teeth: let CheckFailed fall to the
    // generic catch and the rows are gone, only the message left.
    void a_failed_check_fails_and_keeps_its_rows()
    {
        Command::Context C;
        const json R = Command::Run("test check", json::object(), C);
        QVERIFY(!R.value("ok", true));
        QCOMPARE(R["error"].get<std::string>(), std::string("1 of 3 is broken"));
        QCOMPARE(R["result"].dump(), json({{"checked", 3}, {"bad", 1}}).dump());
    }

    // Teeth: drop the prefix match in NodeOf and the short form (as tables print it) names nothing.
    void a_node_is_found_by_any_unique_part_of_its_cid()
    {
        NodeIndex Idx;
        Node A, B;
        A.Cid = "bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka"; A.NodeId = "Patch";
        B.Cid = "bafkreiaoiaihurug456ee3yix7cqcqtmio3qago6o6j7cdcnkxr7n52jty"; B.NodeId = "Other";
        Idx.Nodes.emplace(A.Cid, A);
        Idx.Nodes.emplace(B.Cid, B);
        QCOMPARE(CmdUtil::NodeOf(Idx, "bafkrei…myteka")->NodeId, std::string("Patch"));
        QCOMPARE(CmdUtil::NodeOf(Idx, "bafkreiank4")->NodeId, std::string("Patch"));
        bool Ambiguous = false;
        try { CmdUtil::NodeOf(Idx, "bafkreia"); } catch (const Command::BadArgs &E) { Ambiguous = std::string(E.what()).find("2 nodes") != std::string::npos; }
        QVERIFY2(Ambiguous, "a start two CIDs share names neither, and says so");
    }

    // Teeth: render every list of objects as a table and the layers' "—" columns come back; drop the duplicate-column
    // pass and "summary" is printed beside "title"; drop the short form and the whole CID is in the terminal's table;
    // shorten piped output too and the piped table loses it.
    void a_result_is_a_table_only_when_it_is_one()
    {
        const json Rows = json::array({ {{"title", "Patch 2.0a"}, {"summary", "Patch 2.0a"}, {"cid", "bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka"}},
                                        {{"title", "The Conquerors"}, {"summary", "The Conquerors"}, {"cid", "bafkreiaoiaihurug456ee3yix7cqcqtmio3qago6o6j7cdcnkxr7n52jty"}} });
        CliApp::SetOutputWidth(120);   // as on a terminal: short CIDs
        const std::string T = CliApp::RenderGeneric(Rows, false);
        QVERIFY2(T.find("summary") == std::string::npos, T.c_str());
        QVERIFY2(T.find("bafkrei…myteka") != std::string::npos && T.find("ank43ketmdy") == std::string::npos, T.c_str());
        CliApp::SetOutputWidth(0);     // piped: every CID whole
        const std::string P = CliApp::RenderGeneric(Rows, false);
        QVERIFY2(P.find("bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka") != std::string::npos, P.c_str());
        const json Layers = json::array({ {{"ZIP", "a.zip"}, {"TARGET", "%GameDir%"}}, {{"REG", {{"HKLM", {{"x", 1}}}}}, {"ARCH", {"32"}}},
                                          {{"VARS", {{"a", 1}}}}, {{"NODE", "x"}}, {{"EXEC", json::array({ {{"LABEL", "Play"}} })}} });
        const std::string O = CliApp::RenderGeneric(Layers, false);
        QVERIFY2(O.find("- ZIP") != std::string::npos && O.find(" rows") == std::string::npos, O.c_str());
    }

    // Teeth: sweep the data dir (as `launch running --sweep` once did) and the package folder and the save are removed
    // as a dead session's leftovers, and the session left under the session root stays.
    void a_sweep_clears_dead_sessions_and_never_the_data_dir()
    {
        QTemporaryDir D, T; QVERIFY(D.isValid() && T.isValid());
        qputenv("TMPDIR", T.path().toUtf8());
        const std::filesystem::path Data = D.path().toStdString(), Sessions = Sessions::Root();
        QVERIFY(Sessions.string().rfind(T.path().toStdString(), 0) == 0);
        std::filesystem::create_directories(Data / "LIBRARY" / "[749][v1.0] Age of Empires II");
        std::filesystem::create_directories(Data / "USERDATA" / "749" / "default");
        std::ofstream(Data / "USERDATA" / "749" / "default" / "save.dat") << "a save";
        std::filesystem::create_directories(Sessions / "749" / "default" / "WRITELAYER");   // a launch that died
        QDir Dir(D.path());
        json Cfg = json{{"Settings", json::object()}};
        Command::Context C;
        C.Config = &Cfg;
        C.AppDataDir = &Dir;
        const json R = Command::Run("launch running", {{"sweep", true}}, C);
        qunsetenv("TMPDIR");
        QVERIFY2(R.value("ok", false), R.dump().c_str());
        QVERIFY(std::filesystem::exists(Data / "USERDATA" / "749" / "default" / "save.dat"));
        QVERIFY(std::filesystem::is_directory(Data / "LIBRARY" / "[749][v1.0] Age of Empires II"));
        QVERIFY2(!std::filesystem::exists(Sessions / "749" / "default"), "the dead session is cleared");
    }
};

QTEST_MAIN(CommandsTest)
#include "test_commands.moc"
