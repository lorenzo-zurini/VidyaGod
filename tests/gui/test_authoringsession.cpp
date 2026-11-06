// Headless tests for AuthoringSession's UI-agnostic file-capture mechanism (no container needed): churn
// filtering, write-delta enumeration, re-rooting copy, and the VFSDirLayer json shape.

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QDir>

#include <filesystem>
#include <fstream>
#include <string>

#include "authoringsession.h"
#include "authoringsessionmodel.h"   // CapturePreview
#include "manifestmodel.h"
#include "apppaths.h"

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

    // A new package's node waits for its files (a ZIP with no file yet) - malformed, so no runtime would build from
    // it. The session authors it without the waiting layer, its entry kept; a node waiting for nothing is left as is.
    // Teeth: keep the waiting layer (the node stays malformed); drop every content layer.
    // A person picks a runner by its name, newest first (numbers as numbers: 11_7 > 11_3 > 10_34 > 9_27); the
    // session runs it by its id. The setup capture of a new package's folder goes where it waits (%GameDir%), named
    // after the game. Teeth: sort as text (9_27 first); list ids; default the target to "".
    void the_capture_window_speaks_names()
    {
        const QStringList R = CapturePreview::RunnerChoices({{"a", "geproton_9_27"}, {"b", "geproton_11_3"}, {"c", "geproton_10_34"}, {"d", "geproton_11_7"}});
        QCOMPARE(R, (QStringList{"geproton_11_7\td", "geproton_11_3\tb", "geproton_10_34\tc", "geproton_9_27\ta"}));
        const json N = json::parse(R"({"LABEL":"S.T.A.L.K.E.R.: Shadow of Chernobyl","LAYERS":[{"ZIP":"","TARGET":"FILES/%GameDir%"},{"EXEC":[{"LABEL":"Play"}]}]})");
        QCOMPARE(CapturePreview::WaitingTarget(N), QString("%GameDir%"));
        QCOMPARE(CapturePreview::WaitingTarget(json::parse(R"({"LAYERS":[{"ZIP":"g.zip","TARGET":"FILES/%GameDir%"}]})")), QString());
        QCOMPARE(CapturePreview::CaptureName(N, "x"), QString("S.T.A.L.K.E.R. - Shadow of Chernobyl"));
        QCOMPARE(CapturePreview::CaptureName(json::object(), "x"), QString("x_files"));
    }

    // An installer writes where it installed (C:\GOG Games\X) into the registry; once that folder is captured as the
    // game's files, the value names it by anchor. Only whole path components match, any case. Teeth: replace
    // case-sensitively; match a longer folder name ("X2"); drop the folder's own name when captured at its level.
    void captured_folders_are_named_by_anchor_in_registry_values()
    {
        QCOMPARE(CapturePreview::GuestPathOf("pfx/drive_c/GOG Games/S.T.A.L.K.E.R"), std::string("C:\\GOG Games\\S.T.A.L.K.E.R"));
        QCOMPARE(CapturePreview::GuestPathOf("config_info"), std::string());
        QCOMPARE(CapturePreview::AnchorWinpath("%GameDir%", "pfx/drive_c/GOG Games/X", true), std::string("%GameDir:winpath%"));
        QCOMPARE(CapturePreview::AnchorWinpath("FILES/%Documents%/Saves", "pfx/drive_c/users/u/Documents/Game", false),
                 std::string("%Documents:winpath%\\Saves\\Game"));
        const std::vector<std::pair<std::string, std::string>> M{{"C:\\GOG Games\\X", "%GameDir:winpath%"}};
        QCOMPARE(CapturePreview::Reanchor("c:\\gog games\\x", M), std::string("%GameDir:winpath%"));
        QCOMPARE(CapturePreview::Reanchor("C:\\GOG Games\\X\\bin\\x.exe", M), std::string("%GameDir:winpath%\\bin\\x.exe"));
        QCOMPARE(CapturePreview::Reanchor("C:\\GOG Games\\X2", M), std::string("C:\\GOG Games\\X2"));
        //A game's folder captured by ticking its parts: the folder they share is the target too.
        const auto G = CapturePreview::GuestAnchorsFor({"pfx/drive_c/GOG Games/VtMB/Bin", "pfx/drive_c/GOG Games/VtMB/Vampire.exe"}, "%GameDir%", false);
        QCOMPARE(CapturePreview::Reanchor("C:\\GOG Games\\VtMB", G), std::string("%GameDir:winpath%"));
        //Installed under an anchor of the runtime (Program Files), the session already wrote the value by THAT anchor:
        //it is the captured folder all the same. Teeth: match only the C:\ spelling; only the deepest anchor.
        const QMap<QString, QString> L{{"%ProgramFiles32%", "pfx/drive_c/Program Files (x86)"}, {"%SystemDrive%", "pfx/drive_c"}};
        const auto P = CapturePreview::GuestAnchorsFor({"pfx/drive_c/Program Files (x86)/KONAMI/SILENT HILL 3"}, "%GameDir%", true, L);
        QCOMPARE(CapturePreview::Reanchor("%ProgramFiles32%\\KONAMI\\SILENT HILL 3\\savedata\\", P), std::string("%GameDir:winpath%\\savedata\\"));
        QCOMPARE(CapturePreview::Reanchor("%SystemDrive%\\Program Files (x86)\\KONAMI\\SILENT HILL 3", P), std::string("%GameDir:winpath%"));
        QCOMPARE(CapturePreview::Reanchor("C:\\Program Files (x86)\\KONAMI\\SILENT HILL 3", P), std::string("%GameDir:winpath%"));
        QVERIFY(CapturePreview::AnchoredGuestsOf("pfx/drive_c/Program Files (x86)/X", {{"%ProgramFiles%", "pfx/drive_c/Program Files"}}).empty());
    }

    // A capture of many items previews the first few and how many more (the tree stays in view); a second capture's
    // folder name is one the package does not hold yet. Teeth: list every line; keep the taken name.
    void a_capture_previews_briefly_and_never_reuses_a_folder()
    {
        QStringList L;
        for (int I = 0; I < 30; ++I) L << QString("item%1").arg(I);
        const QString P = CapturePreview::PreviewLines(L);
        QCOMPARE(P.count('\n'), 5);
        QVERIFY(P.endsWith("... and 25 more"));
        QCOMPARE(CapturePreview::PreviewLines({"a", "b"}), QString("a\n  b"));
        QTemporaryDir D; QVERIFY(D.isValid());
        QCOMPARE(CapturePreview::FreeName("Game", D.path()), QString("Game"));
        QDir(D.path()).mkdir("Game");
        QDir(D.path()).mkdir("Game 2");
        QCOMPARE(CapturePreview::FreeName("Game", D.path()), QString("Game 3"));
    }

    void a_node_waiting_for_its_files_is_authored_without_them()
    {
        NodeIndex Idx;
        const json W = json::parse(R"({"LABEL":"G","VARIANT":"Original","LAYERS":[{"ZIP":"","TARGET":"FILES/%GameDir%"},{"ZIP":"base.zip"},
            {"EXEC":[{"LABEL":"Play","TILE":{"UID":"9"}}]}]})");
        Node N;
        QVERIFY(ManifestModel::ParseNode(W, "w.json", "/b", N));
        QVERIFY(!N.LowerError.empty());
        N.Cid = "w";
        Idx.Nodes["w"] = N;
        Node O;
        QVERIFY(ManifestModel::ParseNode(json::parse(R"({"LABEL":"O","LAYERS":[{"ZIP":"o.zip"}]})"), "o.json", "/b", O));
        O.Cid = "o";
        Idx.Nodes["o"] = O;
        ManifestModel::DeriveFacts(Idx);
        QVERIFY(AuthoringSession::WithoutWaiting(Idx, "w"));
        QVERIFY2(Idx.Nodes["w"].LowerError.empty(), Idx.Nodes["w"].LowerError.c_str());
        QCOMPARE((int)(*Idx.Nodes["w"].Json)["LAYERS"].size(), 2);
        QCOMPARE((*Idx.Nodes["w"].Json)["LAYERS"][0]["ZIP"], json("base.zip"));
        QCOMPARE(Idx.Nodes["w"].Cid, std::string("w"));
        QVERIFY(!AuthoringSession::WithoutWaiting(Idx, "o"));
    }

    // The capture preview says where a capture mounts: a TARGET naming an anchor under that anchor's layout in this
    // runtime (sent from the worker as "%Anchor%\tlayout" lines), any other under the content root. Teeth: drop the
    // insert in ParseAnchors (or the anchor branch of MountBase) and %GameDir%/data previews under the content root.
    void capture_preview_mounts_an_anchored_target_where_the_runtime_lays_it_out()
    {
        const QMap<QString, QString> A = CapturePreview::ParseAnchors(CapturePreview::EncodeAnchors(
            {{"%GameDir%", "pfx/drive_c/802"}, {"%ProgramFiles32%", "pfx/drive_c/Program Files (x86)"}}));
        QCOMPARE(CapturePreview::MountBase("FILES/%GameDir%/data", "pfx/drive_c", A), QString("pfx/drive_c/802/data"));
        QCOMPARE(CapturePreview::MountBase("%ProgramFiles32%", "pfx/drive_c", A), QString("pfx/drive_c/Program Files (x86)"));
        QCOMPARE(CapturePreview::MountBase("mods", "pfx/drive_c", A), QString("pfx/drive_c/mods"));
        QCOMPARE(CapturePreview::MountBase("  ", "pfx/drive_c", A), QString("pfx/drive_c"));
        QCOMPARE(CapturePreview::MountBase("%Nope%/x", "pfx/drive_c", A), QString("pfx/drive_c/%Nope%/x"));   // not this runtime's
    }

    // One ticked folder that IS where the target mounts (the game's folder, TARGET %GameDir%) is captured as its
    // contents, as is one for a node waiting at that target; another folder, or several, land at their own level.
    // Teeth: only honour the waiting case (the game's folder nests inside itself on a node that waits for nothing).
    void the_targets_own_folder_is_captured_as_its_contents()
    {
        const QMap<QString, QString> A = CapturePreview::ParseAnchors(CapturePreview::EncodeAnchors({{"%GameDir%", "pfx/drive_c/23441"}}));
        QVERIFY(CapturePreview::CapturesContents({"pfx/drive_c/23441"}, "%GameDir%", QString(), "pfx/drive_c", A));
        QVERIFY(CapturePreview::CapturesContents({"pfx/drive_c/23441/"}, "FILES/%GameDir%", QString(), "pfx/drive_c", A));
        QVERIFY(CapturePreview::CapturesContents({"pfx/drive_c/GOG Games/X"}, "%GameDir%", "%GameDir%", "pfx/drive_c", A));
        QVERIFY(!CapturePreview::CapturesContents({"pfx/drive_c/23441/Patches"}, "%GameDir%", QString(), "pfx/drive_c", A));
        QVERIFY(!CapturePreview::CapturesContents({"pfx/drive_c/23441", "pfx/drive_c/x"}, "%GameDir%", QString(), "pfx/drive_c", A));
        QVERIFY(!CapturePreview::CapturesContents({"pfx/drive_c/23441"}, "", QString(), "pfx/drive_c", A));
    }

    // A session lives on disk under <DataRoot>/AUTHORING/<game>/<instance>; ending it leaves nothing behind there - the
    // game's folder too, or one piles up per game ever captured. Teeth: skip the game folder's removal.
    void an_ended_session_leaves_no_folder_behind()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        AppPaths::SetDataRoot(D.path().toStdString());
        NodeIndex Idx;
        Node N;
        QVERIFY(ManifestModel::ParseNode(json::parse(R"({"LABEL":"G","VARIANT":"Original","LAYERS":[
            {"EXEC":[{"LABEL":"Play","TILE":{"UID":"77"}}]}]})"), "g.json", D.path().toStdString(), N));
        N.Cid = "g";
        Idx.Nodes["g"] = N;
        ManifestModel::DeriveFacts(Idx);
        const fs::path Root = fs::path(D.path().toStdString()) / "AUTHORING";
        {
            AuthoringSession S(json::object(), QDir(D.path()));
            QVERIFY(S.Begin(Idx, "g"));
            QVERIFY(fs::exists(Root / "77"));
            S.End();
        }
        QVERIFY(!fs::exists(Root / "77"));
    }

    // "Run Windows program" passes what the Arguments box holds after the program (an installer's /force), each
    // argument whole. Teeth: drop OverrideArgs in Execute's native branch, or in RunExe.
    void a_program_the_session_runs_gets_its_arguments()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        AppPaths::SetDataRoot(D.path().toStdString());
        NodeIndex Idx;
        Node N;
        QVERIFY(ManifestModel::ParseNode(json::parse(R"({"LABEL":"G","VARIANT":"Original","LAYERS":[
            {"EXEC":[{"LABEL":"Play","TILE":{"UID":"78"}}]}]})"), "g.json", D.path().toStdString(), N));
        N.Cid = "g";
        Idx.Nodes["g"] = N;
        ManifestModel::DeriveFacts(Idx);
        const fs::path Out = fs::path(D.path().toStdString()) / "argv.txt";
        const fs::path Script = fs::path(D.path().toStdString()) / "show.sh";
        touch(Script, "#!/bin/sh\nprintf '[%s]' \"$@\" > '" + Out.string() + "'\n");
        fs::permissions(Script, fs::perms::owner_all);
        AuthoringSession S(json::object(), QDir(D.path()));
        QVERIFY(S.Begin(Idx, "g"));
        QVERIFY(S.RunExe(Script.string(), {"/force", "two words"}));
        S.End();
        std::ifstream In(Out);
        const std::string Got((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        QCOMPARE(QString::fromStdString(Got), QString("[/force][two words]"));
    }

    // IsChurn: registry hives, wine-managed trees and the overlay's whiteouts are filtered; real content is not.
    // Teeth: drop the whiteout check, or match ".wh." anywhere in the name.
    void is_churn()
    {
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/user.reg"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/windows/system32/foo.dll"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/Windows/win.ini"));
        QVERIFY(AuthoringSession::IsChurn("pfx/dosdevices/c:"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/Temp/setup.log"));
        QVERIFY(AuthoringSession::IsChurn("pfx/.update-timestamp"));
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/2312/.wh.EULA.RTF"));       // a whiteout: a deletion, not a file
        QVERIFY(AuthoringSession::IsChurn("pfx/drive_c/2312/art/.wh..wh..opq"));
        QVERIFY(AuthoringSession::IsChurn(".wh.top"));
        // Real game content survives - a name merely containing ".wh." too.
        QVERIFY(!AuthoringSession::IsChurn("pfx/drive_c/2312/EULA.rtf"));
        QVERIFY(!AuthoringSession::IsChurn("pfx/drive_c/2312/notes.wh.txt"));
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
};

QTEST_MAIN(AuthoringSessionTest)
#include "test_authoringsession.moc"
