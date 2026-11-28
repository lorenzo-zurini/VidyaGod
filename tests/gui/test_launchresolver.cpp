// Tests for the launch-engine resolver (LaunchResolver) — the param/recipe/runner/persistence/exec resolution
// extracted from ContainerWrapper. Builds synthetic NodeIndex graphs in memory and asserts the resolved
// ContainerParams. Uses QApplication (DerivePaths reads the primary screen) under offscreen QPA.

#include <QtTest>

#include "launchresolver.h"
#include "sessions.h"
#include "installed.h"
#include "shelf.h"
#include <QScopeGuard>
#include <QTemporaryDir>
#include "launchsession.h"
#include "fileedits.h"
#include "containerwrapper.h"
#include "vfsmount.h"
#include "varsubst.h"
#include "nodelower.h"
#include "registrywrapper.h"
#include "packagecatalog.h"
#include "launchparams.h"
#include "manifestmodel.h"
#include "nodefixture.h"
#include "instancestore.h"
#include "builtinrunners.h"
#include "cid.h"
#include <QTemporaryDir>
#include <fstream>
#include <set>
#include <zip.h>
#include "launchtree.h"
#include "validate.h"
#include <unistd.h>

using json = nlohmann::ordered_json;

namespace {
//A node key shaped like a node CID (02 §2.3), so a condition can name it ("<cid> == false"): "bafkrei" + the name
//(letters a-z and 2-7) padded with 'a' to the 52 base32 characters.
std::string CidOf(const std::string &Name) { std::string C = "bafkrei" + Name; C.resize(59, 'a'); return C; }

// The machine platform the chain resolver bridges TO. Hardcoding "linux64" made every chain test assume a Linux
// host: on Windows MachinePlatform()=="win64", so a runner declared host "linux64" never reaches the machine, the
// chain resolves EMPTY, and the tests' ids[0] indexed out of bounds (a SEGFAULT under the Windows/MinGW CI). Use
// the real machine platform so these tests exercise the same win32->machine bridge on every OS.
const std::string kMachine = ManifestModel::MachinePlatform();

// ---- generation-6 node builders: the JSON a node file holds, parsed as the index would parse it ----
json nodeRefs(const std::vector<std::string> & parents)
{
    json L = json::array();
    for (const auto & P : parents) L.push_back({ {"NODE", P} });
    return L;
}
Node parse(const json & j, const std::string & bundle)
{
    Node n;
    ManifestModel::ParseNode(j, "f.json", bundle, n);
    return n;
}
// A node's facts (entries, runner, tile) are FOLDED; a node handed around on its own gets its own fold derived.
Node standalone(Node n)
{
    NodeIndex t; const std::string k = n.Key(); t.Nodes[k] = n; ManifestModel::DeriveFacts(t); return t.Nodes.at(k);
}
// An index is complete once every node is in: its facts fold across containment.
void finish(NodeIndex & idx) { ManifestModel::DeriveFacts(idx); }

Node contentNode(const std::string & id, const json & layers = json::array())
{
    return parse(json{ {"CID", id}, {"LABEL", id}, {"LAYERS", layers} }, "/tmp/vg_bundle");
}
// A variant: its content ("game" dir), a "Play" entry carrying its tile, extra layers before the entry.
Node launchNode(const std::string & id, const std::string & host, const std::vector<std::string> & parents,
                const json & entryExtra = json::object(), const json & extraLayers = json::array())
{
    json L = nodeRefs(parents);
    L.push_back({ {"DIR", "game"} });
    for (const auto & X : extraLayers) L.push_back(X);
    json E = { {"LABEL", "Play"}, {"HOST", host}, {"EXE", "game.exe"}, {"TILE", {{"UID", id}, {"TITLE", id}}} };
    for (const auto & [K, V] : entryExtra.items()) E[K] = V;
    L.push_back({ {"EXEC", NodeFixture::ExecOf({ E })} });
    return standalone(parse(json{ {"CID", id}, {"LABEL", id}, {"VARIANT", id}, {"LAYERS", L} }, "/tmp/vg_bundle"));
}
// A prefix-generating runner (proton-like) whose build is what it contains.
Node runnerNode(const std::string & id, const std::vector<std::string> & guests,
                const std::vector<std::string> & parents = {}, const json & entryExtra = json::object())
{
    json L = nodeRefs(parents);
    json E = { {"LABEL", "run"}, {"HOST", ManifestModel::MachinePlatform()}, {"GUEST", guests}, {"EXE", "%RunnerMount%/proton"},
               {"CONTENT_ROOT", "pfx/drive_c/%PackageUID%"}, {"PREFIX_GENERATE", true},
               {"ARGS", json::array({"waitforexitandrun", "%Content%"})} };
    for (const auto & [K, V] : entryExtra.items()) E[K] = V;
    L.push_back({ {"EXEC", NodeFixture::ExecOf({ E })} });
    return standalone(parse(json{ {"CID", id}, {"LABEL", id}, {"LAYERS", L} }, "/tmp/vg_runner"));
}
bool stackHas(const ContainerParams & cp, const std::string & needle)
{
    for (const auto & g : cp.Stack) if (g.Name.find(needle) != std::string::npos) return true;
    return false;
}
// A runner edge GUEST→HOST for daisy-chain tests. A '%'-bearing or empty EXECUTABLE is always "available"
// (ExecutableAvailable), so these resolve without a real binary on PATH.
Node chainRunner(const std::string & id, const std::vector<std::string> & guests, const std::string & host,
                 const std::string & exec = "%RunnerMount%/run", const std::vector<std::string> & parents = {},
                 const json & extraLayers = json::array(), const std::string & label = std::string())
{
    json L = nodeRefs(parents);
    for (const auto & X : extraLayers) L.push_back(X);
    json E = { {"LABEL", "run"}, {"HOST", host}, {"GUEST", guests} };
    if (!exec.empty()) E["EXE"] = exec;
    L.push_back({ {"EXEC", NodeFixture::ExecOf({ E })} });
    return standalone(parse(json{ {"CID", id}, {"LABEL", label.empty() ? id : label}, {"LAYERS", L} }, "/tmp/vg_runner"));
}
// A resolved RunnerLink for cross-namespace composition tests.
RunnerLink mkLink(const std::string & id, const std::string & host, const std::string & exec,
                  const std::string & contentRoot = "", const std::vector<std::string> & args = {},
                  const std::string & guestTpl = "")
{
    RunnerLink L; L.NodeId = id; L.Name = id; L.HostPlatform = host; L.GuestPlatform = {host};
    L.Executable = exec; L.ContentRoot = contentRoot; L.Args = args; L.GuestPathTemplate = guestTpl;
    return L;
}
}

class LaunchResolverTest : public QObject
{
    Q_OBJECT
private slots:
    // ---- engine-injected session vars (peer LAN / presence) ----
    // These are the reason SessionVars exists at all: the LAN vars used to be merged AFTER this resolve and after
    // EXEARGS substitution, so %VIDYAGOD_*% survived into argv as a literal. And because only DECLARED keys reach
    // the fixpoint, an undeclared key handed in as an override was silently dropped.
    void session_vars_reach_exeargs_and_custom_var_defaults()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"});
        idx.Nodes["game"] = launchNode("game", "win32", {},
            json{{"ARGS", json::array({"--player", "%VIDYAGOD_SELF_NAME%", "--address=%join%"})}},
            json::array({ json{{"VARS", {{"join", {{"DEFAULT", "%VIDYAGOD_JOIN_ADDRESS%"}}}}}} }));
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars = { {"VIDYAGOD_JOIN_ADDRESS", "10.66.1.2"}, {"VIDYAGOD_SELF_NAME", "Lorenzo"} };
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(cp));

        const std::vector<std::string> Want{"--player", "Lorenzo", "--address=10.66.1.2"};
        QCOMPARE(cp.ExeArgs, Want);
    }

    // Hosting: the address substitutes to empty. The single-token form collapses to "--address=", which the game
    // side reads as "no address"; a "--flag value" PAIR could not express this — the flag would survive alone and
    // swallow the next argument.
    void empty_session_var_collapses_to_hosting()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"});
        idx.Nodes["game"] = launchNode("game", "win32", {},
            json{{"ARGS", json::array({"--address=%VIDYAGOD_JOIN_ADDRESS%", "%VIDYAGOD_JOIN_ADDRESS%", "--wait"})}});
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars = { {"VIDYAGOD_JOIN_ADDRESS", ""} };
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(cp));

        // The bare token drops entirely; the single-token form survives with an empty value.
        const std::vector<std::string> Want{"--address=", "--wait"};
        QCOMPARE(cp.ExeArgs, Want);
    }

    // Precedence: a session var is the LOWEST source. An explicit --var/picker override beats it, and a node that
    // declares the key with its own DEFAULT keeps control of it.
    void session_vars_are_lowest_priority()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"});
        idx.Nodes["game"] = launchNode("game", "win32", {}, json::object(),
            json::array({ json{{"VARS", {{"VIDYAGOD_SELF_NAME", {{"DEFAULT", "PackageChoice"}}}}}} }));
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars       = { {"VIDYAGOD_SELF_NAME", "FromLan"}, {"VIDYAGOD_JOIN_ADDRESS", "10.66.9.9"} };
        cp.VariableOverrides = { {"VIDYAGOD_JOIN_ADDRESS", "1.2.3.4"} };
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));

        QCOMPARE(cp.CustomVariables["VIDYAGOD_JOIN_ADDRESS"], std::string("1.2.3.4"));      // override wins
        QCOMPARE(cp.CustomVariables["VIDYAGOD_SELF_NAME"],    std::string("PackageChoice")); // declaration wins
    }

    // A runner's layer WHEN reads the variables as the launch resolves them: a --var; else the game's value for a key
    // the game declares (a game seeds a runner's knob); else a saved value only for a key the runner declares with a UI
    // (a setting — a value saved before the option became computed is left over). Every runner fold reads them: its
    // link's environment, its edits and declarations, its keep list. Its own resolve saw only its declared defaults,
    // so the gated layers were dropped whatever the launch said (xemu's cache reset, 2026-11-24). Teeth: fold a runner
    // without them (at any of the three places); the game's declaration not read; a saved value of a UI-less key used.
    void a_runners_when_reads_the_launchs_variables()
    {
        struct Case { std::optional<std::string> Flag, GameDefault, Saved; bool Ui = true; };
        auto Launch = [](const Case &C) {
            NodeIndex idx;
            json Decl = {{"DEFAULT", "1"}};
            if (C.Ui) Decl["UI"] = {{"CONTROL", "bool"}, {"LABEL", "Reset the cache"}};
            json L = json::array({
                json{{"VARS", {{"cache_reset", Decl}}}},
                json{{"ENV", {{"VG_CACHE_RESET", "0"}}}, {"WHEN", "%cache_reset%==0"}},
                json{{"ENV", {{"VG_ALWAYS", "1"}}}},
                json{{"VARS", {{"reset_note", {{"DEFAULT", "off"}}}}}, {"WHEN", "%cache_reset%==0"}},
                json{{"KEEP", {{"FILES/%GameDir%/cache", true}}}, {"WHEN", "%cache_reset%==0"}},
                json{{"EXEC", json::object({ {"run", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"xbox"}},
                                                 {"EXE", "%RunnerMount%/xemu"}, {"ARGS", json::array({ "%Content%" })}}} })}} });
            idx.Nodes["xemu"] = standalone(parse(json{ {"CID", "xemu"}, {"LABEL", "xemu"}, {"LAYERS", L} }, "/tmp/vg_runner"));
            json GameLayers = json::array();
            if (C.GameDefault) GameLayers.push_back(json{{"VARS", {{"cache_reset", {{"DEFAULT", *C.GameDefault}}}}}});
            idx.Nodes["game"] = launchNode("game", "xbox", {}, json::object(), GameLayers);
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            if (C.Flag) cp.VariableOverrides = { {"cache_reset", *C.Flag} };
            QTemporaryDir ud;   // the instance's saved values: here, never in a user-data folder another case reads
            json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
            if (C.Saved) InstanceStore::MergeVariables(cfg, idx.Find("game")->PackageUid, {{"cache_reset", *C.Saved}});
            const bool Ok = LaunchResolver::InitializeFromNode(cp, cfg);
            bool Kept = false;
            for (const auto &K : cp.RunnerPersistLayers) Kept = Kept || K.dump().find("cache") != std::string::npos;
            return std::tuple<bool, json, bool, bool>(Ok, cp.RunnerEnv, cp.CustomVariables.count("reset_note") > 0, Kept);
        };
        auto Open = [&](const Case &C) {
            const auto [Ok, Env, Note, Kept] = Launch(C);
            if (!Ok || Env.value("VG_ALWAYS", std::string()) != "1") return -1;
            const bool E = Env.value("VG_CACHE_RESET", std::string()) == "0";
            if (E != Note || E != Kept) return -2;                    // every runner fold agrees
            return E ? 1 : 0;
        };
        QCOMPARE(Open({ std::string("0") }), 1);                                        // --var
        QCOMPARE(Open({}), 0);                                                          // its default: shut
        QCOMPARE(Open({ std::nullopt, std::string("0") }), 1);                          // the game seeds it
        QCOMPARE(Open({ std::string("1"), std::string("0") }), 0);                      // --var over the game
        QCOMPARE(Open({ std::nullopt, std::nullopt, std::string("0"), true }), 1);      // a saved setting
        QCOMPARE(Open({ std::nullopt, std::nullopt, std::string("0"), false }), 0);     // saved, no UI: left over
    }

    // A --var for a variable no node declares still sets it: the engine reads its own switches (the peer LAN's
    // gateway, VIDYAGOD_LAN_BRIDGE=off) from the launch's variables. Teeth: drop an undeclared override (the switch is
    // never seen and the gateway stays on).
    void an_override_sets_an_engine_switch_no_node_declares()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.VariableOverrides = { {"VIDYAGOD_LAN_BRIDGE", "off"} };
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QCOMPARE(cp.CustomVariables["VIDYAGOD_LAN_BRIDGE"], std::string("off"));
    }

    // A session's scratch lives in the system temp (RAM on Linux) unless the caller roots it elsewhere: an authoring
    // session roots it on disk, since an installer writes the whole game there. Teeth: ignore the override.
    void a_session_root_override_moves_its_scratch()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {});
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        for (const bool Override : {false, true})
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx;
            cp.LaunchNodeId = "game";
            if (Override) cp.TempRootOverride = "/srv/disk/AUTHORING";
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            const std::string T = cp.TempPath.string();
            if (Override) QVERIFY2(T.rfind("/srv/disk/AUTHORING/", 0) == 0, T.c_str());
            else QVERIFY2(T.rfind(Sessions::Root().string(), 0) == 0, T.c_str());
        }
    }

    // A launch of an instance that is already running is refused before it touches anything, and its Cleanup (every
    // caller runs one after a failed build) leaves the running launch's session folder alone: that folder holds the live
    // runtime and, through it, the user's kept files. Teeth: Cleanup without the claim guard removes the folder; the
    // claim after the stale clear (the clear empties the folder first).
    void a_refused_second_launch_leaves_the_running_session_alone()
    {
#ifdef _WIN32
        QSKIP("session ownership locks are Linux's");
#else
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {});
        finish(idx);
        QTemporaryDir Tmp;
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx;
        cp.LaunchNodeId = "game";
        cp.TempRootOverride = Tmp.path().toStdString();
        ContainerWrapper Second(json{{"Settings", json::object()}}, cp);
        QVERIFY(Second.Resolved());
        const std::filesystem::path Folder = Second.ContainerParams.TempPath;
        int Running = Sessions::ClaimSession(Folder);                // the first launch, live
        QVERIFY(Running >= 0);
        std::filesystem::create_directories(Folder / "WRITELAYER");
        std::ofstream(Folder / "WRITELAYER" / "save.dat") << "the running game's";
        QVERIFY(!Second.BuildContainerRuntime());
        QVERIFY(Second.Cleanup());
        QVERIFY(std::filesystem::exists(Folder / "WRITELAYER" / "save.dat"));
        QVERIFY(Sessions::SessionOwned(Folder));
        QCOMPARE(ContainerWrapper::LiveCount(), size_t(0));          // a refused launch is no session
        Sessions::ReleaseSession(Running, Folder);
#endif
    }

    // A literal '%' in an edit is unescaped once, on the way through the resolve AND the edit pass: "%%AppData%%" stays
    // the text the game expands itself, "100%%%%" is written 100%%. Teeth: the resolve's pass writing out (the edit
    // pass then expands %AppData% and writes 100%); the edit pass keeping escapes (100%%%% written).
    void a_literal_percent_is_written_once_through_resolve_and_edit()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {}, json::object(), json::array({
            json{{"VARS", {{"AppData", {{"DEFAULT", "C:/expanded"}}}}}},
            json{{"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", "path=%%AppData%%\\Saves rate=100%%%%"}} })},
                  {"TARGET", "FILES/pfx/drive_c/game/game.ini"}},
            json{{"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", "x"}} })},
                  {"TARGET", "FILES/pfx/drive_c/game/%%AppData%%.cfg"}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QTemporaryDir Rt;
        cp.RuntimePath = Rt.path().toStdString();
        QVERIFY(FileEdits::ProcessFileEdits(cp, /*OverridePass=*/true));
        std::ifstream In(cp.RuntimePath / "pfx/drive_c/game/game.ini");
        const std::string Got((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        QCOMPARE(Got, std::string("path=%AppData%\\Saves rate=100%%"));
        QVERIFY(std::filesystem::exists(cp.RuntimePath / "pfx/drive_c/game/%AppData%.cfg"));   // the name as written
    }

    // A launch reads back what the game changed in a file the user keeps: the saved copy's value becomes the option's,
    // in the instance and in this launch; one the file still holds as last known leaves a change made in the launch
    // window since alone; a dry resolve (InitializeFromNode) reads nothing. Teeth: no read-back in ResolveTakingBack
    // (res stays 1920); no re-resolve after adopting (the instance changes, the launch still writes 1920).
    void a_launch_reads_back_what_the_game_changed()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        const std::string Ini = "FILES/pfx/drive_c/game/settings.ini";
        idx.Nodes["game"]   = launchNode("game", "win32", {}, json::object(), json::array({
            json{{"VARS", {{"res", {{"DEFAULT", "1920"}, {"UI", {{"CONTROL", "int"}, {"LABEL", "Resolution"}}}}}}}},
            json{{"EDIT", json::array({ json{{"MODE", "ConfigWrite"}, {"KEY", "Resolution="}, {"VALUE", "%res%"}} })}, {"TARGET", Ini}},
            json{{"KEEP", {{Ini, true}, {Ini + "#Resolution=", false}}}} }));
        finish(idx);
        QTemporaryDir ud; QVERIFY(ud.isValid());
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        const auto resolve = [&](bool ReadBack) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            if (!(ReadBack ? LaunchSession::ResolveTakingBack(cp, cfg) : LaunchResolver::InitializeFromNode(cp, cfg))) qFatal("resolve failed");
            return cp;
        };
        const ContainerParams First = resolve(false);
        QCOMPARE((int)First.KeepFiles.size(), 1);
        const std::filesystem::path Saved = First.UserDataPath / First.KeepFiles[0].Target;
        std::filesystem::create_directories(Saved.parent_path());
        std::ofstream(Saved) << "[Video]\nResolution=2560\n";                 // changed in the game's own menu

        QCOMPARE(resolve(false).CustomVariables.at("res"), std::string("1920"));   // a dry resolve reads nothing
        QCOMPARE(resolve(true).CustomVariables.at("res"), std::string("2560"));
        json C = InstanceStore::ReadConfig(cfg, First.PackageUID, First.InstanceName);
        QCOMPARE(C["VARIABLES"]["res"].get<std::string>(), std::string("2560"));
        C["VARIABLES"]["res"] = "1280";                                          // then changed in the launch window
        QVERIFY(InstanceStore::WriteConfig(cfg, First.PackageUID, First.InstanceName, C));
        QCOMPARE(resolve(true).CustomVariables.at("res"), std::string("1280"));
    }

    // InitializeFromNode resolves the whole container from the node graph: runner pick, recipe, content-root.
    void initialize_from_node_resolves_runner_and_recipe()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});   // build ships via the content node it contains
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["base"]   = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {"base"});
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx;
        cp.LaunchNodeId = "game";
        const json cfg = json{{"Settings", json::object()}};

        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));

        QCOMPARE(cp.RunnerID, std::string("wine"));
        QVERIFY(cp.PrefixGenerate);
        QVERIFY(cp.RunnerShipsBuild);                          // runner has VFS build layers
        QCOMPARE(cp.ContentRoot, std::string("pfx/drive_c/game"));   // %PackageUID% substituted by DerivePaths
        QVERIFY(stackHas(cp, "game"));                 // the resolved row is the recipe
        QVERIFY(!cp.Stack.empty() && !cp.Stack.back().Ops.empty());
        int base = 0;                                          // …and the content it contains is in it, placed
        for (const auto & L : cp.SubComponentsArray)
            if (L.value("TYPE", std::string()) == "VFSDirLayer" && L.value("PATH", std::string()).find("/base") != std::string::npos) ++base;
        QCOMPARE(base, 1);
    }

    // %SteamAppId% is the launched tile's META.STEAMAPPID (a string or a number), else its nearest parent tile's, else "0"
    // — never the package UID. Teeth: always "0"; the package UID as the fallback; no parent walk; a number dropped.
    void steam_app_id_comes_from_the_tile_or_its_parent()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["base"]   = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {"base"},
                                         json{{"TILE", {{"UID", "game"}, {"TITLE", "Game"}, {"META", {{"STEAMAPPID", "1771300"}}}}}});
        idx.Nodes["exp"]    = launchNode("exp", "win32", {"base"},
                                         json{{"TILE", {{"UID", "exp"}, {"PARENTUID", "game"}, {"TITLE", "Expansion"}}}});
        idx.Nodes["num"]    = launchNode("num", "win32", {"base"},
                                         json{{"TILE", {{"UID", "num"}, {"TITLE", "Num"}, {"META", {{"STEAMAPPID", 220}}}}}});
        idx.Nodes["none"]   = launchNode("none", "win32", {"base"});
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        const auto appId = [&](const char *Id) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx;
            cp.LaunchNodeId = Id;
            if (!LaunchResolver::InitializeFromNode(cp, cfg)) return std::string("<resolve failed>");
            return cp.GetVariablesMap()["SteamAppId"];
        };
        QCOMPARE(appId("game"), std::string("1771300"));
        QCOMPARE(appId("exp"),  std::string("1771300"));     // an expansion runs as its base game
        QCOMPARE(appId("num"),  std::string("220"));
        QCOMPARE(appId("none"), std::string("0"));           // not "none", its package UID
    }

    // A library pinned by the RUNNER contributes its order-independent layers to the game runtime, exactly as a
    // library pinned by the GAME does — the runner chain is equivalent to the content chain in capability for
    // DllOverride/RegEdit/FileEdit. (It is NOT equivalent for VFS: a runner parent's VFS builds the runner tree.)
    void runner_closure_contributes_overrides_and_edits()
    {
        NodeIndex idx;
        idx.Nodes["mediastack"] = contentNode("mediastack", json::array({
            json{{"DLL", {{"winegstreamer", ""}}}},
            json{{"REG", {{"HKLM\\Software\\Lav", json::object()}}}, {"ARCH", json::array({"64"})}},
            json{{"ZIP", "codecs.zip"}, {"TARGET", "FILES/files/lib/gstreamer-1.0"}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"mediastack"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));

        int overrides = 0, regedits = 0, prefixVfs = 0;
        for (const auto & L : cp.SubComponentsArray)
        {
            const std::string T = L.value("TYPE", std::string());
            if (T == "DllOverride" && L.value("DLLOVERRIDE", std::string()) == "winegstreamer=") ++overrides;
            else if (T == "RegEdit" && L.value("REGPATH", std::string()) == "HKLM\\Software\\Lav") ++regedits;
            else if (T == "VFSZipLayer" && L.value("PATH", std::string()).find("codecs.zip") != std::string::npos) ++prefixVfs;
        }
        QCOMPARE(overrides, 1);    // reaches WINEDLLOVERRIDES (FileEdits::ProcessDLLOverrides reads this array)
        QCOMPARE(regedits, 1);     // applied to the game prefix
        QCOMPARE(prefixVfs, 0);    // runner-tree build layer stays in the runner mount, not the prefix
    }

    // A node a runner CONTAINS is its composition: its prefix-assembly layers (a %runtime% PATH) reach the game's
    // prefix. There is no toggle inside a fold — a runner's optional piece would be a WHEN-gated NODE layer.
    void runner_closure_nodes_always_contribute_prefix_layers()
    {
        NodeIndex idx;
        idx.Nodes["wine_extra_dlls"] = contentNode("wine_extra_dlls", json::array({
            json{{"DIR", "%RunnerMount%/extra"}, {"TARGET", "FILES/pfx/drive_c/extra"}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"wine_extra_dlls"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        int n = 0;
        for (const auto & L : cp.SubComponentsArray)
            if (L.value("PATH", std::string()) == "%RunnerMount%/extra") ++n;
        QCOMPARE(n, 1);
    }

    // No qualifying runner (guest platform mismatch) → no runner picked.
    void initialize_from_node_no_matching_runner()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win64"});     // serves win64
        idx.Nodes["game"] = launchNode("game", "win32", {});   // needs win32
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        // No runner serves win32 → resolution FAILS (must not "succeed" into an empty-runner launch).
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerID.empty());
    }

    // AuthoringBare: a platformless, content-less node resolves with NO runner and CONTENT_ROOT="" (the authoring
    // workbench). Resolution must succeed where a normal launch would fail (no platform → no chain).
    void initialize_from_node_authoring_bare()
    {
        NodeIndex idx;
        idx.Nodes["fresh"] = contentNode("fresh");                  // no entry, no content: a fresh node
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "fresh";
        cp.AuthoringBare = true;

        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerChain.empty());                      // no runner resolved
        QVERIFY(cp.RunnerID.empty());
        QVERIFY(cp.ContentRoot.empty());                      // content at the runtime root
        QVERIFY(!cp.PrefixGenerate);                          // no wine prefix
        QCOMPARE((int)cp.Stack.size(), 1);                    // the node alone, no runner beneath it
    }

    // A node WITH content but no platform still resolves bare (content overlay, no runner).
    void initialize_from_node_authoring_bare_with_content()
    {
        NodeIndex idx;
        idx.Nodes["c"] = contentNode("c", json::array({ json{{"DIR", "files"}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "c"; cp.AuthoringBare = true;
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerChain.empty());
        QVERIFY(stackHas(cp, "c"));                   // the node's own content layer is in the recipe
        QCOMPARE((int)cp.SubComponentsArray.size(), 1);
    }

    // A runner's own edit names its file as a game's does (%GameDir%/…, in guest coordinates). It must be mapped into
    // the boundary runner's layout like the game's edits: an emulator's first-launch settings file landed at
    // RUNTIME/C:\<uid>/… instead of the game folder (snes9x, RetroArch 2026-10-20). Teeth: append the runner's edits
    // unmapped and FILE stays "C:\game/emu/emu.cfg".
    void a_runners_edit_lands_in_the_game_folder()
    {
        NodeIndex idx;
        idx.Nodes["emu"] = standalone(parse(json{ {"CID", "emu"}, {"LABEL", "emu"}, {"LAYERS", json::array({
            json{{"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", "x"}} })}, {"TARGET", "FILES/%GameDir%/emu/emu.cfg"}},
            json{{"EXEC", json::object({ {"emu", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"snes"}},
                                             {"EXE", "%RunnerMount%/emu"}, {"CONTENT_ROOT", "snes/%PackageUID%"},
                                             {"DRIVES", {{"C:", "snes"}}}, {"GUEST_ROOTS", {{"%GameDir%", "C:\\%PackageUID%"}}}}} })}} })} },
            "/tmp/vg_runner"));
        idx.Nodes["game"] = launchNode("game", "snes", {});
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        std::string file;
        for (const auto & L : cp.SubComponentsArray)
            if (L.value("TYPE", std::string()) == "FileEdit") file = L.value("FILE", std::string());
        QVERIFY2(!file.empty(), cp.SubComponentsArray.dump().c_str());
        QVERIFY2(file.find("C:") == std::string::npos && file.find("emu/emu.cfg") != std::string::npos, file.c_str());
        QVERIFY2(file.rfind("snes/", 0) == 0, file.c_str());
    }

    // A runner's edit on a drive its DRIVES does not lay out is refused, as a game's layer is.
    void a_runners_edit_on_an_unmapped_drive_is_refused()
    {
        NodeIndex idx;
        idx.Nodes["emu"] = standalone(parse(json{ {"CID", "emu"}, {"LABEL", "emu"}, {"LAYERS", json::array({
            json{{"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", "x"}} })}, {"TARGET", "FILES/E:/emu.cfg"}},
            json{{"EXEC", json::object({ {"emu", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"snes"}},
                                             {"EXE", "%RunnerMount%/emu"}, {"CONTENT_ROOT", "snes/%PackageUID%"},
                                             {"DRIVES", {{"C:", "snes"}}}, {"GUEST_ROOTS", {{"%GameDir%", "C:\\%PackageUID%"}}}}} })}} })} },
            "/tmp/vg_runner"));
        idx.Nodes["game"] = launchNode("game", "snes", {});
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
    }

    // A unified runner's recipe is one of the game's components: its edit is applied once, not again as a runner edit.
    void a_unified_runners_edit_is_applied_once()
    {
        NodeIndex idx;
        idx.Nodes["emu"] = standalone(parse(json{ {"CID", "emu"}, {"LABEL", "emu"}, {"LAYERS", json::array({
            json{{"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", "x"}} })}, {"TARGET", "FILES/%GameDir%/emu.cfg"}},
            json{{"EXEC", json::object({ {"emu", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"snes"}},
                                             {"EXE", "%RunnerMount%/ge/emu"}, {"UNIFIED_RUNTIME", true}, {"CONTENT_ROOT", "ge"},
                                             {"DRIVES", {{"C:", "ge"}}}, {"GUEST_ROOTS", {{"%GameDir%", "C:\\"}}}}} })}} })} },
            "/tmp/vg_runner"));
        idx.Nodes["game"] = launchNode("game", "snes", {});
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(cp.UnifiedRuntime);
        int edits = 0;
        for (const auto & L : cp.SubComponentsArray)
            if (L.value("TYPE", std::string()) == "FileEdit") ++edits;
        QVERIFY2(edits == 1, cp.SubComponentsArray.dump().c_str());
    }

    // A runner-less workbench lays each anchor any runner declares out as a folder of its name: content placed at
    // %GameDir% mounts at GameDir/, not at a literal "%GameDir%" folder.
    void authoring_bare_lays_anchors_out_as_folders()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = runnerNode("proton", {"win32"}, {}, json{{"GUEST_ROOTS", {{"%GameDir%", "C:\\%PackageUID%"}}},
                                                                       {"DRIVES", {{"C:", "%PrefixRoot%/drive_c"}}}});
        idx.Nodes["c"] = contentNode("c", json::array({ json{{"DIR", "files"}, {"TARGET", "FILES/%GameDir%/data"}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "c"; cp.AuthoringBare = true;
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QCOMPARE((int)cp.SubComponentsArray.size(), 1);
        QCOMPARE(cp.SubComponentsArray[0]["TARGET"].get<std::string>(), std::string("GameDir/data"));
        const auto L = LaunchResolver::AnchorLayouts(cp);
        QCOMPARE((int)L.size(), 1);
        QCOMPARE(L[0].second, std::string("GameDir"));
    }

    // A pinned chain is honoured over the first-qualifying default.
    void pick_runner_honours_pin()
    {
        NodeIndex idx;
        idx.Nodes["wineA"] = runnerNode("wineA", {"win32"});
        idx.Nodes["wineB"] = runnerNode("wineB", {"win32"});
        Node launch = launchNode("game", "win32", {});
        const json cfg = json{{"Settings", json::object()}};

        ContainerParams cp("/tmp/vg_bundle");
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("wineA"));   // node-id order

        cp.RunnerChainIds = { "wineB" };
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("wineB"));
    }

    // The game's entry may recommend a runner by LABEL (RECOMMENDEDRUNNER): a soft pin. It is the default when the
    // library has it installed; the instance's own pin wins over it; a label the library lacks, or a runner not installed
    // while another carries the launch, leaves the default chain; with nothing installed it is the one to download.
    // Teeth: ignore the field (aaa wins); let it beat the instance's pin; let an uninstalled one beat an installed chain.
    void pick_runner_honours_the_entrys_recommended_runner()
    {
        NodeIndex idx;
        idx.Nodes["aaa"]          = runnerNode("aaa", {"win32"});                       // sorts first
        idx.Nodes["zzz_proton11"] = runnerNode("zzz_proton11", {"win32"});
        idx.Nodes["p12build"]     = contentNode("p12build", json::array({ json{{"ZIP", "absent-proton12.zip"}} }));
        idx.Nodes["zzz_proton12"] = runnerNode("zzz_proton12", {"win32"}, {"p12build"});   // not downloaded
        const json cfg = json{{"Settings", json::object()}};
        const auto first = [&](const char *Rec, std::vector<std::string> Pin = {}) {
            Node launch = launchNode("game", "win32", {}, json{{"RECOMMENDEDRUNNER", Rec}});
            ContainerParams cp("/tmp/vg_bundle");
            cp.RunnerChainIds = std::move(Pin);
            const auto Ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
            return Ids.empty() ? std::string() : Ids[0];
        };
        QVERIFY(!Installed::RunnerInstalled(idx, "zzz_proton12"));             // precondition
        QCOMPARE(first("zzz_proton11"), std::string("zzz_proton11"));
        QCOMPARE(first("zzz_proton11", {"aaa"}), std::string("aaa"));                 // the instance's pin wins
        QCOMPARE(first("no_such_runner"), std::string("aaa"));                        // the default chain
        QCOMPARE(first("zzz_proton12"), std::string("aaa"));                          // installed chains first
        idx.Nodes.erase("aaa");
        idx.Nodes.erase("zzz_proton11");
        QCOMPARE(first("zzz_proton12"), std::string("zzz_proton12"));                 // nothing installed: the one to get
    }

    // Between runners otherwise equal, one shipped in the launch node's own package wins (embedded > global).
    void pick_runner_prefers_package_local()
    {
        NodeIndex idx;
        Node global = runnerNode("aaa_wine", {"win32"}); global.BundleDir = "/repo/global";   // sorts first, but global
        idx.Nodes["aaa_wine"] = global;
        Node local = runnerNode("zzz_wine", {"win32"}); local.BundleDir = "/repo/mygame";      // same bundle as launch
        idx.Nodes["zzz_wine"] = local;

        Node launch = launchNode("game", "win32", {}); launch.BundleDir = "/repo/mygame";
        const json cfg = json{{"Settings", json::object()}};
        ContainerParams cp("/tmp/vg_bundle");
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("zzz_wine"));   // package-local beats alphabetical global
    }

    // Among otherwise equal runners the specialist wins: a dedicated SNES emulator before a multi-system one that also
    // lists snes and sorts first by label (ares beside snes9x: the alternative never becomes every console's default).
    // Teeth: drop the guest-count step and "aaa_multi" is picked.
    // A machine that fetched an earlier windows-native (C:\<uid> anchors) still has it beside the built-in one: same
    // label, same guests, neither recommended nor package-local. The built-in one wins, whichever sorts first by CID and
    // whichever the index holds first. Teeth: drop the built-in step and the old node is picked.
    void pick_runner_prefers_the_builtin_over_its_old_namesake()
    {
        json Old = json::parse(R"VG({"LABEL":"windows-native_exec","LAYERS":[{"COMMENT":"A Windows game on Windows runs as itself: the game's exe, started from the mounted game folder. The guest's C: is laid out inside the mount (drive_c), the way a Wine prefix lays it out, so every package places its files the same way on both hosts.","EXEC":{"windows-native_exec":{"ARGS":[],"CONTENT_ROOT":"drive_c/%PackageUID%","DRIVES":{"C:":"drive_c"},"EXE":"%Content%","GUEST":["win32","win64"],"GUEST_ROOTS":{"%AppData%":"C:\\users\\steamuser\\AppData\\Roaming","%Documents%":"C:\\users\\steamuser\\Documents","%GameDir%":"C:\\%PackageUID%","%LocalAppData%":"C:\\users\\steamuser\\AppData\\Local","%ProgramData%":"C:\\ProgramData","%ProgramFiles%":"C:\\Program Files","%ProgramFiles32%":"C:\\Program Files (x86)","%SysDir32%":"C:\\windows\\syswow64","%SysDir64%":"C:\\windows\\system32","%UserProfile%":"C:\\users\\steamuser","%Windows%":"C:\\windows"},"HOST":"win64"}}}]})VG");
        const json New = BuiltinRunners::All().at(0).Node;
        const std::string NewCid = Cid::OfNode(New);
        //the old one sorts FIRST by CID (the teeth: a pick by CID alone would take it)
        for (int K = 0; Cid::OfNode(Old) >= NewCid && K < 256; ++K) Old["LAYERS"][0]["COMMENT"] = "an earlier build " + std::to_string(K);
        const std::string OldCid = Cid::OfNode(Old);
        QVERIFY(OldCid < NewCid);
        QVERIFY(BuiltinRunners::IsBuiltin(NewCid) && !BuiltinRunners::IsBuiltin(OldCid));

        auto Indexed = [](json N, const std::string &C) { N["CID"] = C; return standalone(parse(N, "/tmp/vg_runner")); };
        NodeIndex idx;
        idx.Nodes[OldCid] = Indexed(Old, OldCid);
        idx.Nodes[NewCid] = Indexed(New, NewCid);
        if (kMachine != "win64") idx.Nodes["bridge"] = chainRunner("bridge", {"win64"}, kMachine);   // win64 → this machine
        finish(idx);
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        const auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QVERIFY(!ids.empty());
        QCOMPARE(ids[0], NewCid);
    }

    // Runners equal on every count and sharing a LABEL (two versions of one runner) are ordered by CID: the pick does not
    // depend on how the sort treats ties (enough of them that std::sort partitions instead of insertion-sorting).
    void pick_runner_breaks_a_tie_by_cid()
    {
        NodeIndex idx;
        for (int I = 39; I >= 0; --I)
        {
            const std::string Id = std::string("r") + char('0' + I / 10) + char('0' + I % 10);
            idx.Nodes[Id] = chainRunner(Id, {"snes"}, kMachine, "%RunnerMount%/run", {}, json::array(), "same_exec");
        }
        finish(idx);
        Node launch = launchNode("game", "snes", {});
        ContainerParams cp("/tmp/vg_bundle");
        const auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QVERIFY(!ids.empty());
        QCOMPARE(ids[0], std::string("r00"));
    }

    void pick_runner_prefers_the_specialist()
    {
        NodeIndex idx;
        idx.Nodes["aaa_multi"] = runnerNode("aaa_multi", {"nes", "snes", "megadrive", "gba"});   // sorts first
        idx.Nodes["zzz_snes"] = runnerNode("zzz_snes", {"snes"});
        Node launch = launchNode("game", "snes", {});
        const json cfg = json{{"Settings", json::object()}};
        ContainerParams cp("/tmp/vg_bundle");
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("zzz_snes"));
    }

    // DerivePersistence (unified Persist primitive): pristine by default, KEEP target classification by shape, and a
    // DROP path.
    void derive_persistence_unified_persist()
    {
        // No DeclarePersist anywhere → pristine, every keep-set empty.
        ContainerParams cp("/tmp/vg_bundle");
        const std::vector<OpGroup> poolNone = { OpGroup{"c1", json::array()} };
        cp.Stack = poolNone; LaunchResolver::DerivePersistence(cp);
        QVERIFY(cp.KeepDirs.empty() && cp.KeepFiles.empty() && cp.KeepRegKeys.empty());

        // DeclarePersist: SCOPE=file PATH shape decides dir (no extension) vs file (extension); TARGET defaults to
        // PATH's leaf; CLOUD default true. SCOPE=registry PATH=key → subtree; PATH="" (a whole registry) is refused.
        ContainerParams cp2("/tmp/vg_bundle");
        const std::vector<OpGroup> poolKeep = { OpGroup{"c1", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/saves"},{"CLOUD",false}},              // dir; TARGET→"saves"; local-only
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/Game/config.ini"},{"TARGET","GameConfig"}}, // file, explicit target
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKCU\\Software\\id Software\\Quake"}},      // subtree
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH",""}},                                      // whole registry: refused
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKEY_CURRENT_USER"}},                     // a bare hive root: refused
            // a key kept for a 32-bit program is where that program has it (Wow6432Node); for a 64-bit one, as named
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKLM\\Software\\EA GAMES\\NFSU2"},{"ARCHITECTURE","32"}},
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKLM\\Software\\EA GAMES\\NFSU2"},{"ARCHITECTURE","64"}} })} };
        cp2.Stack = poolKeep; LaunchResolver::DerivePersistence(cp2);
        QCOMPARE((int)cp2.KeepDirs.size(), 1);
        QCOMPARE(cp2.KeepDirs[0].Path, std::string("drive_c/saves"));
        QCOMPARE(cp2.KeepDirs[0].Target, std::string("saves"));     // defaulted to the last PATH component
        QVERIFY(!cp2.KeepDirs[0].Cloud);                            // CLOUD:false honoured
        QCOMPARE((int)cp2.KeepFiles.size(), 1);
        QCOMPARE(cp2.KeepFiles[0].Path, std::string("drive_c/Game/config.ini"));
        QCOMPARE(cp2.KeepFiles[0].Target, std::string("GameConfig"));
        QCOMPARE((int)cp2.KeepRegKeys.size(), 3);                   // the named keys only: "" and the bare root added nothing
        QCOMPARE(cp2.KeepRegKeys[0], std::string("HKCU\\Software\\id Software\\Quake"));
        QCOMPARE(cp2.KeepRegKeys[1], std::string("HKLM\\Software\\Wow6432Node\\EA GAMES\\NFSU2"));
        QCOMPARE(cp2.KeepRegKeys[2], std::string("HKLM\\Software\\EA GAMES\\NFSU2"));
    }

    // A pattern names files in one folder: always a file keep, whatever its last segment looks like ("slot*" has no
    // dot — by shape alone it would become a passthrough of a folder literally named "slot*" and persist nothing),
    // and a pattern outside the last segment is refused, loudly. Teeth: classify patterns by shape; accept "save/*/x".
    void derive_persistence_patterns_are_file_keeps()
    {
        ContainerParams cp("/tmp/vg_bundle");
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/save/slot*"},{"TARGET","Slots"}},
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/*/profile.dat"},{"TARGET","Profiles"}} })} };
        cp.Stack = pool; LaunchResolver::DerivePersistence(cp);
        QCOMPARE((int)cp.KeepDirs.size(), 0);
        QCOMPARE((int)cp.KeepFiles.size(), 1);
        QCOMPARE(cp.KeepFiles[0].Path, std::string("game/save/slot*"));
    }

    // A kept place reached through ".." (the Public user's Documents, which no anchor names) is mounted at the place
    // itself, and one that leaves the runtime is refused. Teeth: keep the ".." spelled out; let it climb out.
    void derive_persistence_resolves_dotdot_in_a_kept_path()
    {
        ContainerParams cp("/tmp/vg_bundle");
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","pfx/drive_c/users/steamuser/../Public/Documents/Game/"},{"TARGET","Saves"}},
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","pfx/../../outside/"},{"TARGET","Out"}} })} };
        cp.Stack = pool; LaunchResolver::DerivePersistence(cp);
        QCOMPARE((int)cp.KeepDirs.size(), 1);
        QCOMPARE(cp.KeepDirs[0].Path, std::string("pfx/drive_c/users/Public/Documents/Game"));
    }

    // Every file/dir persist lands at UserDataPath/<TARGET>. Two guards keep that namespace safe from SILENT SAVE
    // LOSS: (a) a TARGET colliding with an earlier one is refused (else the second capture clobbers the first, or two
    // dir persists mount the same durable dir RW and corrupt it); (b) a TARGET naming the instance's OWN reserved
    // state (instance.json / REGKEYS) is refused (else a persist would seed the launcher's secrets into a
    // game-visible mount, or overwrite the instance config). Both are case-insensitive — durable dirs travel.
    void derive_persistence_refuses_colliding_and_reserved_targets()
    {
        ContainerParams cp("/tmp/vg_bundle");
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/save"},{"TARGET","Save"}},          // kept
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","mods/save"},{"TARGET","Save"}},          // dup → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","other/save"},{"TARGET","save"}},         // dup (case) → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","cfg.ini"},{"TARGET","instance.json"}},   // reserved → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","hive"},{"TARGET","RegKeys"}} })} };   // reserved (case) → skip
        cp.Stack = pool; LaunchResolver::DerivePersistence(cp);
        // Only the first survives (a dir persist — "game/save" has no extension); the two collisions and the two
        // reserved targets are dropped (loudly), not merged.
        QCOMPARE((int)cp.KeepDirs.size() + (int)cp.KeepFiles.size(), 1);
        QVERIFY(!cp.KeepDirs.empty());
        QCOMPARE(cp.KeepDirs[0].Target, std::string("Save"));
    }

    // "Keep everything" is just a KEEP of the runtime root (%RuntimePath%) — no mode flag. The runner keep-set unions
    // in regardless (additive); a bare runner keep-set leaves the runtime pristine apart from the user profile + HKCU.
    // THE FLOW, not the function. The two tests below hand `RunnerPersistLayers` to DerivePersistence
    // directly — a shape InitializeFromNode can no longer produce — so they stayed green while the runner
    // keep-set stopped reaching it at all and EVERY GAME SILENTLY LOST ITS SAVES at exit. Nothing else
    // resolves a runner closure for persistence, so nothing else could notice: DerivePersistence printed a
    // green summary and --audit-packages reported every launchable clean.
    //
    // Pre-flat the keep-set sat on the runner NODE's own layers; it is now its own Persist node in the
    // runner's PARENTS. This asserts the whole path from a graph to KeepDirs/KeepHives.
    void runner_keepset_reaches_persistence_through_the_closure()
    {
        auto hasDir = [](const ContainerParams &cp, const std::string &p){
            return std::any_of(cp.KeepDirs.begin(), cp.KeepDirs.end(), [&](const PersistTarget &d){ return d.Path == p; }); };
        NodeIndex idx;
        idx.Nodes["proton_keepset"] = contentNode("proton_keepset", json::array({
            json{{"KEEP", {{"FILES/pfx/drive_c/users", true}, {"REG/HKCU\\Software\\Wine", true}}}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"proton_keepset"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        LaunchResolver::DerivePersistence(cp);

        QVERIFY2(!cp.KeepDirs.empty(), "the runner's keep-set never reached persistence - saves are lost");
        QVERIFY(hasDir(cp, "pfx/drive_c/users"));
        QVERIFY(!cp.KeepRegKeys.empty());                                       // HKCU\Software\Wine

        // ...and in a CHAIN, every runner's keep-set counts, not just the boundary's: an emulator nested
        // under proton has user-state of its own, and taking only the outermost link drops it silently.
        NodeIndex ch;
        ch.Nodes["emu_keep"] = contentNode("emu_keep", json::array({ json{{"KEEP", {{"FILES/drive_c/emu_state", true}}}} }));
        ch.Nodes["emu"]      = chainRunner("emu", {"vortex"}, "win32", "%RunnerMount%/run", {"emu_keep"});
        ch.Nodes["proton"]   = chainRunner("proton", {"win32"}, kMachine, "%RunnerMount%/run", {"proton_keep"});
        ch.Nodes["proton_keep"] = contentNode("proton_keep", json::array({ json{{"KEEP", {{"FILES/pfx/drive_c/users", true}}}} }));
        ch.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        ch.Nodes["vgame"]     = launchNode("vgame", "vortex", {});
        finish(ch);

        ContainerParams cp2("/tmp/vg_bundle");
        cp2.NodeIdx = &ch; cp2.LaunchNodeId = "vgame";
        QVERIFY(LaunchResolver::InitializeFromNode(cp2, json{{"Settings", json::object()}}));
        LaunchResolver::DerivePersistence(cp2);
        QVERIFY2(hasDir(cp2, "drive_c/emu_state"), "the INNER runner's keep-set was dropped");
        QVERIFY(hasDir(cp2, "pfx/drive_c/users"));
    }

    // A WHEN on a Persist node gates it like any other layer. BuildSubComponentsArray's gate deliberately
    // SKIPS Persist (it is consumed by DerivePersistence, not mounted), so without a gate here a conditional
    // keep-set applied unconditionally.
    void a_false_when_on_a_persist_node_keeps_nothing()
    {
        auto build = [](const char *when) {
            NodeIndex idx;
            idx.Nodes["keep"] = contentNode("keep", json::array({
                json{{"KEEP", {{"FILES/drive_c/Saves", true}}}, {"WHEN", when}} }));
            idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"keep"});
            idx.Nodes["game"] = launchNode("game", "win32", {});
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            if (!LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}})) return -1;   // -1: did not resolve
            LaunchResolver::DerivePersistence(cp);
            return (int)cp.KeepDirs.size();
        };
        QCOMPARE(build("1 == 1"), 1);            // true  -> kept
        QCOMPARE(build("1 == 2"), 0);            // false -> inert
    }

    void derive_persistence_keep_root_and_runner_keepset()
    {
        // The runner keep-set (RunnerPersistLayers) supplies persists; the game declares a whole-runtime persist
        // (PATH "" ⇒ the whole write layer, TARGET-mapped). Both fold into the same KeepDirs list.
        auto hasDir = [](const ContainerParams &c, const std::string &p) {
            return std::any_of(c.KeepDirs.begin(), c.KeepDirs.end(),
                               [&](const PersistTarget &t){ return t.Path == p; });
        };
        ContainerParams cp("/tmp/vg_bundle");
        cp.RuntimePath = "/tmp/rt";
        cp.RunnerPersistLayers = json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", "pfx/drive_c/users"}},
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "registry"}, {"PATH", "HKCU\\Software\\Wine"}} });
        const std::vector<OpGroup> pool = { OpGroup{"game", json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", ""}, {"TARGET", "AllData"}} })} };
        cp.Stack = pool; LaunchResolver::DerivePersistence(cp);
        QVERIFY(hasDir(cp, ""));                                   // PATH "" ⇒ whole-runtime persist recorded
        QVERIFY(hasDir(cp, "pfx/drive_c/users"));                 // runner's user-profile keep still recorded
        QCOMPARE((int)cp.KeepDirs.size(), 2);
        QVERIFY(!cp.KeepRegKeys.empty());                          // a named key ⇒ granular registry keep

        // Runner keep-set alone (no game persist) → pristine runtime with the runner's user-profile + its key kept.
        ContainerParams cp2("/tmp/vg_bundle");
        cp2.RunnerPersistLayers = json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", "drive_c/users"}},
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "registry"}, {"PATH", "HKCU\\Software\\Wine"}} });
        const std::vector<OpGroup> pool2 = { OpGroup{"game", json::array()} };
        cp2.Stack = pool2; LaunchResolver::DerivePersistence(cp2);
        QCOMPARE((int)cp2.KeepDirs.size(), 1);                     // …the user profile
        QVERIFY(!cp2.KeepRegKeys.empty());                         // …and the key
    }

    // ---- Runner daisy-chaining (PickRunnerChain / ResolveChainIds / ResolveRunnerChain) ----

    // win32 content with a direct proton (win32→linux64): chain is [proton, <native terminal>], terminal LAST.
    void chain_win32_single_bridge_plus_native_terminal()
    {
        NodeIndex idx;
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, kMachine);
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");   // explicit native passthrough
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("proton"));
        QCOMPARE(ids.back(), std::string("nativerun"));        // native terminal always last
    }

    // "custom" content is run by its OWN package's engine. A Windows-only engine in the game's package (custom→win64,
    // then proton) wins over another package's native engine one hop shorter (custom→linux64): that one runs a
    // different game. Teeth: let ServesPlatform accept any package's "custom" runner and the BFS picks gemrb.
    void chain_custom_runs_only_the_packages_own_engine()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = chainRunner("proton", {"win64"}, kMachine);
        idx.Nodes["gemrb"]  = chainRunner("gemrb", {"custom"}, kMachine);                 // another package's engine
        Node engine = chainRunner("engine", {"custom"}, "win64");
        engine.BundleDir = "/tmp/vg_bundle";                                              // shipped in the game's package
        idx.Nodes["engine"] = engine;
        Node launch = launchNode("game", "custom", {});
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QVERIFY((int)ids.size() >= 2);
        QCOMPARE(ids[0], std::string("engine"));
        QCOMPARE(ids[1], std::string("proton"));
        // ...and a pinned chain naming the foreign engine is not honoured: it falls back to the package's own.
        cp.RunnerChainIds = { "gemrb" };
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("engine"));
    }

    // Installed runners first: the package's Windows engine under proton (installed) beats its native engine (one hop
    // shorter, but its build is not downloaded yet) — the chain the Play gate counted is the one that runs. With
    // nothing installed to carry it, the chain is the one to download, and the gate says so.
    // Teeth: drop the installed pass (ChainOver over InstalledOf) and the native engine wins; make HasUsableChain
    // return true and the last check fails.
    void chain_prefers_installed_runners_and_agrees_with_the_play_gate()
    {
        NodeIndex idx;
        idx.Nodes["linbuild"] = contentNode("linbuild", json::array({ json{{"ZIP", "engine-linux.zip"}} }));
        idx.Nodes["lin"] = chainRunner("lin", {"custom"}, kMachine, "%RunnerMount%/engine", {"linbuild"});
        Node win = chainRunner("win", {"custom"}, "win64", "%RunnerMount%/engine.exe");
        win.BundleDir = "/tmp/vg_bundle";
        idx.Nodes["win"] = win;
        idx.Nodes["lin"].BundleDir = "/tmp/vg_bundle";
        idx.Nodes["proton"] = chainRunner("proton", {"win64"}, kMachine);
        Node launch = launchNode("game", "custom", {});
        idx.Nodes["game"] = launch;
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(!Installed::RunnerInstalled(idx, "lin"));                       // precondition: not downloaded
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QVERIFY((int)ids.size() >= 2);
        QCOMPARE(ids[0], std::string("win"));
        QCOMPARE(ids[1], std::string("proton"));
        QVERIFY(Installed::HasUsableChain(idx, launch));
        idx.Nodes.erase("proton");
        QCOMPARE(LaunchResolver::ResolveChainIds(idx, launch, cp, cfg)[0], std::string("lin"));   // what to download
        QVERIFY2(!Installed::HasUsableChain(idx, launch), "nothing installed runs it: Play asks for the runner");
    }

    // The chain bridges the SELECTED entry's platform, never the node's default view: a node carrying a native entry
    // (the default) and a win32 entry routes the win32 one through a win32 runner.
    void chain_follows_the_selected_entrypoint()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["protonB"] = chainRunner("protonB", {"win32"}, kMachine);
        Node launch = standalone(parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "game"}, {"LAYERS", json::array({
            json{{"EXEC", json::object({ {"Native", {{"HOST", kMachine}, {"EXE", "game"}, {"TILE", {{"UID", "game"}}}}}, {"Windows", {{"HOST", "win32"}, {"EXE", "game.exe"}}} })}} })} }, "/tmp/vg_bundle"));
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto def = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)def.size(), 1);                                          // native: terminal only
        cp.Entrypoint = "Windows";
        auto win = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)win.size(), 2);
        QCOMPARE(win[0], std::string("protonA"));                              // a win32 bridge (BFS: first by key)
        cp.Entrypoint = "Native";
        QCOMPARE((int)LaunchResolver::ResolveChainIds(idx, launch, cp, cfg).size(), 1);
    }

    // A runner's build is its resolution, ITSELF INCLUDED — the same rule as a game's: a runner whose build lives on
    // the runner node (the java runners: EXEC + the JRE zip in one node) ships it, and its link mounts the layer.
    void runner_build_on_the_runner_node_itself_ships()
    {
        NodeIndex idx;
        idx.Nodes["java8"] = chainRunner("java8", {"java_8"}, kMachine, "%RunnerMount%/__jre/bin/java", {},
                                         json::array({ json{{"ZIP", "jre_8.zip"}, {"TARGET", "FILES/__jre"}} }));
        finish(idx);
        Node launch = launchNode("mc", "java_8", {});
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QVERIFY2(!ids.empty() && ids[0] == "java8", "the self-built runner bridges java_8 (it ships its build)");
        auto chain = LaunchResolver::ResolveRunnerChain(idx, launch, cp, cfg);
        QVERIFY(!chain.empty());
        QVERIFY2(chain[0].ShipsBuild, "the link ships a build");
        QCOMPARE((int)chain[0].Layers.size(), 1);                  // the runner's OWN layer is the build
        QCOMPARE(chain[0].Layers[0].value("TARGET", std::string()), std::string("__jre"));
        // What the runner CONTAINS is part of its build too, beneath its own layer.
        idx.Nodes["lib"] = contentNode("lib", json::array({ json{{"ZIP", "lib.zip"}} }));
        idx.Nodes["java8"] = chainRunner("java8", {"java_8"}, kMachine, "%RunnerMount%/__jre/bin/java", {"lib"},
                                         json::array({ json{{"ZIP", "jre_8.zip"}, {"TARGET", "FILES/__jre"}} }));
        finish(idx);
        auto chain2 = LaunchResolver::ResolveRunnerChain(idx, launch, cp, cfg);
        QCOMPARE((int)chain2[0].Layers.size(), 2);
        QCOMPARE(chain2[0].Layers.back().value("TARGET", std::string()), std::string("__jre"));   // own layer last = on top
    }

    // A part (a node whose list begins with INTO naming the variant) that carries an entry is a WAY TO RUN the
    // variant: applied, its entry is in the row's fold, --entry picks it, and its HOST drives the chain — the mount
    // stays the variant's plus the graft.
    void a_grafts_entry_runs_over_the_variants_mount()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        idx.Nodes["loader"] = parse(json{ {"CID", "loader"}, {"LABEL", "loader"}, {"LAYERS", json::array({
            json{{"INTO", json::array({"game"})}}, json{{"DIR", "loader"}},
            json{{"EXEC", json::object({ {"Loader", {{"HOST", "win32"}, {"EXE", "loader.exe"}}} })}} })} }, "/tmp/vg_bundle");
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.Grafts = std::vector<std::string>{ "loader" };
        cp.Entrypoint = "Loader";
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QCOMPARE(cp.ComposedExec.value("CONTENTPATH", std::string()), std::string("loader.exe"));   // the graft's entry
        QCOMPARE(cp.LaunchNodeId, std::string("game"));                                              // over the variant's mount
        QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "loader" });                            // the graft is applied
        QCOMPARE(cp.RunnerID, std::string("protonA"));   // the chain serves the ENTRY's platform (win32), not the variant's
    }

    // The game's environment is the fold of its resolution: what it contains, itself, and the applied grafts above
    // it — later wins, null removes. The runner link's environment is the fold of the runner's own resolution.
    void the_environment_folds_from_the_mount_and_from_the_runner_build()
    {
        NodeIndex idx;
        idx.Nodes["lib"] = contentNode("lib", json::array({ json{{"DIR", "lib"}}, json{{"ENV", {{"A", "lib"}, {"B", "lib"}, {"C", "lib"}}}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"lib"}, json::object(),
                                       json::array({ json{{"ENV", {{"A", "game"}, {"B", nullptr}, {"HOST_ONLY", nullptr}}}} }));
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"LAYERS", json::array({
            json{{"INTO", json::array({"game"})}}, json{{"DIR", "mod"}}, json{{"ENV", {{"C", "mod"}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["rlib"] = contentNode("rlib", json::array({ json{{"ZIP", "rlib.zip"}}, json{{"ENV", {{"R", "rlib"}, {"S", "rlib"}, {"T", "rlib"}}}} }));
        idx.Nodes["native"] = chainRunner("native", {kMachine}, kMachine, "%RunnerMount%/run", {"rlib"},
                                          json::array({ json{{"ENV", {{"S", "runner"}, {"R", nullptr}}}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{ "mod" };
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QCOMPARE(cp.LaunchEnv.value("A", std::string()), std::string("game"));   // the game over the library beneath
        QCOMPARE(cp.LaunchEnv.value("C", std::string()), std::string("mod"));    // the graft over the game
        QVERIFY(!cp.LaunchEnv.contains("B"));                                       // removed by the game
        QVERIFY(std::find(cp.LaunchRemoveEnv.begin(), cp.LaunchRemoveEnv.end(), "HOST_ONLY") != cp.LaunchRemoveEnv.end());
        auto chain = LaunchResolver::ResolveRunnerChain(idx, idx.Nodes.at("game"), cp, cfg);
        QVERIFY(!chain.empty());
        QCOMPARE(chain[0].Env.value("S", std::string()), std::string("runner"));   // the runner over its library
        QCOMPARE(chain[0].Env.value("T", std::string()), std::string("rlib"));     // the library it contains reaches the link
        QVERIFY(!chain[0].Env.contains("R"));                                        // removed by the runner
        QVERIFY(std::find(chain[0].RemoveEnv.begin(), chain[0].RemoveEnv.end(), "R") != chain[0].RemoveEnv.end());
    }

    // A label may repeat (a peer's received stub of "proton" beside the local "proton"): every walk on the launch
    // path is keyed by the INDEX key, or the first label match — the stub, sorted first here — is walked instead of
    // the runner that ships the build.
    void closure_walks_are_keyed_by_the_index_key_not_the_label()
    {
        NodeIndex idx;
        Node stub = chainRunner("aaa_stub", {"nothing"}, kMachine, "%RunnerMount%/run", {}, json::array(), "proton");
        stub.Received = true;
        idx.Nodes["aaa_stub"] = stub;
        idx.Nodes["zzz_local"] = chainRunner("zzz_local", {"win32"}, kMachine, "%RunnerMount%/run", {},
                                             json::array({ json{{"ZIP", "proton.zip"}, {"TARGET", "FILES/__build"}} }), "proton");
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);
        const Node & launch = idx.Nodes.at("game");
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        const json cfg = json{{"Settings", json::object()}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QVERIFY(!ids.empty());
        QCOMPARE(ids[0], std::string("zzz_local"));
        auto chain = LaunchResolver::ResolveRunnerChain(idx, launch, cp, cfg);
        QVERIFY(!chain.empty());
        QCOMPARE(chain[0].NodeId, std::string("zzz_local"));                 // the link carries the key…
        QCOMPARE(chain[0].Name, std::string("proton"));                      // …and shows the label
        QVERIFY2(chain[0].ShipsBuild, "the LOCAL runner's build is found, not the stub's nothing");
        QCOMPARE((int)chain[0].Layers.size(), 1);
    }

    // An entry that is not in the row's fold is refused — a graft's entry exists only while the graft is applied, and
    // a graft this row does not offer is not applied.
    void an_unticked_grafts_entry_is_refused()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        idx.Nodes["loader"] = parse(json{ {"CID", "loader"}, {"LABEL", "loader"}, {"LAYERS", json::array({
            json{{"INTO", json::array({"game"})}},
            json{{"EXEC", json::object({ {"Loader", {{"HOST", "win32"}, {"EXE", "loader.exe"}}} })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["elsewhere"] = parse(json{ {"CID", "elsewhere"}, {"LABEL", "elsewhere"}, {"LAYERS", json::array({
            json{{"INTO", json::array({"other_game"})}},
            json{{"EXEC", json::object({ {"Loader", {{"HOST", "win32"}, {"EXE", "x.exe"}}} })}} })} }, "/tmp/vg_bundle");
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{};   // unticked
        cp.Entrypoint = "Loader";
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, cfg));
        cp.Grafts = std::vector<std::string>{ "elsewhere" };                                  // not offered to this row
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, cfg));
        QVERIFY(cp.AppliedGrafts.empty());
    }

    // A row whose parts do not hold is blocked (12 §12.5): an end that goes INTO what is not there. A part whose
    // condition says "never with" something the row contains is not applied — the row still launches without it.
    void an_unsatisfied_row_is_blocked_and_a_conflicting_graft_is_not_applied()
    {
        NodeIndex idx;
        const std::string Base = CidOf("base");
        idx.Nodes[Base] = contentNode(Base, json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {Base});
        idx.Nodes["needs"] = launchNode("needs", kMachine, {}, json::object(), json::array({ json{{"INTO", json::array({"nowhere"})}} }));
        idx.Nodes["conflict"] = parse(json{ {"CID", "conflict"}, {"LABEL", "conflict"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}, {"WHEN", Base + " == false"}, {"RECOMMENDED", true}}, json{{"DIR", "c"}} })} }, "/tmp/vg_bundle");
        idx.Nodes["fine"] = parse(json{ {"CID", "fine"}, {"LABEL", "fine"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}, {"RECOMMENDED", true}}, json{{"DIR", "f"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "needs";
            QVERIFY(!LaunchResolver::InitializeFromNode(cp, cfg));
        }
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "fine" });   // both pre-ticked; the conflicting one does not apply
        cp.Grafts = std::vector<std::string>{ "conflict", "fine" };
        QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
        QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "fine" });
    }

    // Parts have no order: the list shows a part after what it goes into, then by LABEL, then CID (a presentation,
    // never a fold order). A part is offered only on the end it goes INTO (the hard link): an end that contains that
    // end (FE holds the game) is not offered its parts. Teeth: offer by containment (mod gets the game's parts).
    void parts_are_listed_after_what_they_go_into_then_by_label()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        const std::string Aok = CidOf("aok"), Tc = CidOf("tc");
        const auto graft = [](const std::string &Id, const char *Label, const std::string &When, const char *Into = "game") {
            json In = json{{"INTO", json::array({Into})}};
            if (!When.empty()) In["WHEN"] = When;
            return parse(json{ {"CID", Id}, {"LABEL", Label}, {"LAYERS", json::array({ In, json{{"DIR", Id}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes[Aok] = graft(Aok, "Age of Kings", "");
        idx.Nodes[Tc] = graft(Tc, "The Conquerors", Aok + " == false");
        idx.Nodes["both"] = graft("both", "Both", Aok + " == false && " + Tc + " == false");
        idx.Nodes["patch"] = graft("patch", "Patch", "");
        idx.Nodes["onaok"] = graft("onaok", "A Aok Addon", "", "");
        idx.Nodes["onaok"] = parse(json{ {"CID", "onaok"}, {"LABEL", "A Aok Addon"},
            {"LAYERS", json::array({ json{{"INTO", json::array({Aok})}}, json{{"DIR", "o"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        QCOMPARE(Shelf::OfferedGrafts(idx, "game"), (std::vector<std::string>{ Aok, "both", "patch", Tc }));
        //with Aok applied, its add-on follows the parts of the end (one INTO step further), whatever its LABEL
        std::vector<std::string> Pre;
        QCOMPARE(Shelf::OfferedGrafts(idx, "game", &Pre, std::vector<std::string>{ Aok }), (std::vector<std::string>{ Aok, "both", "patch", Tc, "onaok" }));
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"VARIANT", "m"},
            {"LAYERS", json::array({ json{{"NODE", "game"}}, json{{"NODE", "patch"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        QVERIFY(Shelf::OfferedGrafts(idx, "mod").empty());
    }

    // The selection is judged as it will launch (adversary 2026-11-27): a part is judged beside what can be present (the
    // parts offered with it and those applied, never a requested part not offered here), and one excluded by a part a
    // later round brings is dropped, never listed applied. (A) a stale list {P: "only with X", Q: into P, X: for another end} launches, applying neither P nor Q.
    // (B) a fresh instance: A "never with X" and B are recommended, X goes into B (round 2): A is not applied.
    // (C) "never with X", X requested but not offered: the part applies. Teeth: drop the final as-launched check (A
    // blocks the row; B lists A applied); judge beside every requested part (C drops "na").
    void a_selection_is_judged_as_it_will_launch()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        idx.Nodes["other"] = launchNode("other", kMachine, {});
        const std::string X = CidOf("x"), Xb = CidOf("xb");
        const auto part = [](const std::string &Id, const json &Into, const std::string &When, bool Rec) {
            json In = json{{"INTO", Into}};
            if (!When.empty()) In["WHEN"] = When;
            if (Rec) In["RECOMMENDED"] = true;
            json J{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ In, json{{"DIR", Id}} })} };
            return parse(J, "/tmp/vg_bundle");
        };
        idx.Nodes[X] = part(X, json::array({"other"}), "", false);
        idx.Nodes["p"] = part("p", json::array({"game"}), X + " == true", false);
        idx.Nodes["q"] = part("q", json::array({"p"}), "", false);
        idx.Nodes["a"] = part("a", json::array({"game"}), Xb + " == false", true);
        idx.Nodes["b"] = part("b", json::array({"game"}), "", true);
        idx.Nodes[Xb] = part(Xb, json::array({"b"}), "", true);
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{ "p", "q", X };
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            QVERIFY(cp.AppliedGrafts.empty());
        }
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            const std::set<std::string> A(cp.AppliedGrafts.begin(), cp.AppliedGrafts.end());
            QCOMPARE(A, (std::set<std::string>{"b", Xb}));
        }
        {   // (C) "never with X", X requested but not offered here: X is not in the launch, so the part applies
            idx.Nodes["na"] = part("na", json::array({"game"}), X + " == false", false);
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{ "na", X };
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "na" });
        }
    }

    // Parts that write one unit differently conflict, shown, never settled by position (23 §23.4): two parts setting one
    // environment variable to different values, two parts placing different bytes at one file path (compared without
    // case). Identical writes are one write, and a part and one it goes INTO (a patch on a mod) never conflict.
    // Teeth: compare names only (identical writes reported); skip the INTO exemption; compare paths with case.
    // A part's INTO may carry a TAKE: its FOOTPRINT (MetaPackageFormat 23 §23.4) — what of it goes into the end, and where,
    // as a NODE's TAKE selects and renames. ENB and ReShade both ship d3d11.dll; a ReShade part taking its d3d11.dll as
    // dxgi.dll (chain-loading) mounts it there and conflicts with nothing; without the TAKE they conflict. A TAKE that
    // takes none of the part's files leaves nothing of it mounted. Conflicts are read where files land. Teeth: expand parts
    // with no view; address a footprint by its layer's TARGET.
    void a_parts_into_take_is_its_footprint()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        const auto part = [](const char *Id, json Into, const char *Src) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ Into, json{{"FILE", "d3d11.dll"}, {"SOURCE", Src}, {"TARGET", "FILES/C:/g"}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["enb"] = part("enb", json{{"INTO", json::array({"game"})}}, "QmEnb");
        idx.Nodes["rs"] = part("rs", json{{"INTO", json::array({"game"})}, {"TAKE", json::array({ json::array({"FILES/C:/g/d3d11.dll", "FILES/C:/g/dxgi.dll"}) })}}, "QmRs");
        idx.Nodes["rsplain"] = part("rsplain", json{{"INTO", json::array({"game"})}}, "QmRs2");
        idx.Nodes["none"] = part("none", json{{"INTO", json::array({"game"})}, {"TAKE", json::array({"FILES/C:/elsewhere/"})}}, "QmNone");
        finish(idx);
        QVERIFY2(idx.Find("rs") && idx.Find("rs")->LowerError.empty(), "a TAKE on INTO is a valid layer");
        using V = std::vector<std::string>;
        const auto Addrs = [&](const V &Parts) {
            std::set<std::string> S;
            for (const auto &C : Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", Parts))) S.insert(C.Address);
            return S;
        };
        QVERIFY2(Addrs(V{"enb", "rsplain"}).count("FILES/C:/g/d3d11.dll"), "two parts' d3d11.dll");
        QVERIFY2(Addrs(V{"enb", "rs"}).empty(), "ReShade taken as dxgi.dll writes another file");
        //where it mounts: the launch's own reading of the plan
        const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(idx), "game", {}, {}, Shelf::AppliedGrafts(idx, "game", V{"rs"}));
        const LaunchTree T = LaunchTree::Of(P);
        bool AtDxgi = false, AtD3d11 = false;
        for (size_t I = 0; I < T.S.layers.size(); ++I)
            for (const auto &[Src, Dst] : T.T.mounts[I]) { AtDxgi = AtDxgi || Dst == "C:/g/dxgi.dll"; AtD3d11 = AtD3d11 || Dst == "C:/g/d3d11.dll"; }
        QVERIFY2(AtDxgi && !AtD3d11, "the part's file mounts where its footprint puts it");
        const Fold::Plan Pn = Fold::Resolve(ManifestModel::LibraryOf(idx), "game", {}, {}, Shelf::AppliedGrafts(idx, "game", V{"none"}));
        const LaunchTree Tn = LaunchTree::Of(Pn);
        size_t Shown = 0;
        for (size_t I = 0; I < Tn.S.layers.size(); ++I)
            for (const auto &[Src, Dst] : Tn.T.mounts[I]) if (Dst.find("d3d11") != std::string::npos || Dst.find("elsewhere") != std::string::npos) ++Shown;
        QCOMPARE(Shown, size_t(0));
        //a malformed TAKE on INTO is refused like a NODE's, and a footprint selects files only (facts are the part's own)
        Node Bad = parse(json{ {"CID", "bad"}, {"LABEL", "bad"}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}, {"TAKE", "FILES/x"}} })} }, "/tmp/vg_bundle");
        QVERIFY(!Bad.LowerError.empty());
        Node Facts = parse(json{ {"CID", "facts"}, {"LABEL", "facts"}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}, {"TAKE", json::array({"DLL"})}} })} }, "/tmp/vg_bundle");
        QVERIFY(!Facts.LowerError.empty());
        //a rename that lands on another part's file conflicts THERE (ENB ships dxgi.dll too)
        idx.Nodes["enbdxgi"] = parse(json{ {"CID", "enbdxgi"}, {"LABEL", "enbdxgi"}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}},
                                     json{{"FILE", "dxgi.dll"}, {"SOURCE", "QmEnbDxgi"}, {"TARGET", "FILES/C:/g"}} })} }, "/tmp/vg_bundle");
        //the part's facts are never filtered by its footprint (its DLL override, its option)
        idx.Nodes["rsf"] = parse(json{ {"CID", "rsf"}, {"LABEL", "rsf"}, {"LAYERS", json::array({
                                     json{{"INTO", json::array({"game"})}, {"TAKE", json::array({ json::array({"FILES/C:/g/d3d11.dll", "FILES/C:/g/dxgi.dll"}) })}},
                                     json{{"FILE", "d3d11.dll"}, {"SOURCE", "QmRsf"}, {"TARGET", "FILES/C:/g"}},
                                     json{{"DLL", {{"dxgi", "native,builtin"}}}}, json{{"VARS", {{"preset", {{"DEFAULT", "a"}}}}}},
                                     json{{"KEEP", {{"FILES/C:/g/ReShade.ini", true}}}},
                                     json{{"EDIT", json::array({ {{"MODE", "ConfigWrite"}, {"SECTION", "[S]"}, {"KEY", "K"}, {"VALUE", "1"}} })}, {"TARGET", "FILES/C:/g/ReShade.ini"}} })} }, "/tmp/vg_bundle");
        //a pack contains the ReShade part: its footprint holds there too
        idx.Nodes["pack"] = parse(json{ {"CID", "pack"}, {"LABEL", "pack"}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}}, json{{"NODE", "rs"}} })} }, "/tmp/vg_bundle");
        //two INTO TAKEs apply in layer order: a.dll taken as b.dll, then b.dll taken
        idx.Nodes["two"] = parse(json{ {"CID", "two"}, {"LABEL", "two"}, {"LAYERS", json::array({
                                     json{{"INTO", json::array({"game"})}, {"TAKE", json::array({ json::array({"FILES/C:/g/a.dll", "FILES/C:/g/b.dll"}) })}},
                                     json{{"INTO", json::array({"game"})}, {"TAKE", json::array({"FILES/C:/g/b.dll"})}},
                                     json{{"FILE", "a.dll"}, {"SOURCE", "QmTwo"}, {"TARGET", "FILES/C:/g"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        QVERIFY2(Addrs(V{"enbdxgi", "rs"}).count("FILES/C:/g/dxgi.dll"), "a rename onto another part's file");
        const auto MountsOf = [&](const V &Parts) {
            std::set<std::string> M;
            const LaunchTree Tm = LaunchTree::Of(Fold::Resolve(ManifestModel::LibraryOf(idx), "game", {}, {}, Shelf::AppliedGrafts(idx, "game", Parts)));
            for (size_t I = 0; I < Tm.S.layers.size(); ++I) for (const auto &[Src, Dst] : Tm.T.mounts[I]) M.insert(Dst);
            return M;
        };
        {   const Fold::Plan Pf = Fold::Resolve(ManifestModel::LibraryOf(idx), "game", {}, {}, Shelf::AppliedGrafts(idx, "game", V{"rsf"}));
            QVERIFY2(Pf.Dll.contains("dxgi"), "the part's DLL override stays");
            QVERIFY2(Pf.Decls.contains("preset"), "the part's option stays");
            QVERIFY2(Pf.Keep.dump().find("ReShade.ini") != std::string::npos, "the part's kept file stays");
            bool Edit = false; for (const auto &It : Pf.Seq) Edit = Edit || (It.Kind == "EDIT" && It.Target == "C:/g/ReShade.ini");
            QVERIFY2(Edit, "the part's edit of a file its footprint does not name stays"); }
        {   const auto M = MountsOf(V{"pack"});
            QVERIFY2(M.count("C:/g/dxgi.dll") && !M.count("C:/g/d3d11.dll"), "a contained part keeps its footprint"); }
        {   const auto M = MountsOf(V{"two"});
            QVERIFY2(M.count("C:/g/b.dll") && !M.count("C:/g/a.dll"), "two footprints, in layer order"); }
        //a footprint that takes none of the part's files is said, never silent
        std::vector<std::string> Errors, Warnings;
        Validate::Nodes(idx, Errors, Warnings);
        bool Said = false; for (const auto &W : Warnings) Said = Said || (W.find("'none'") != std::string::npos && W.find("takes none of its files") != std::string::npos);
        QVERIFY2(Said, "the empty footprint warns");
    }

    // Units INSIDE files (MetaPackageFormat 23 §23.4): an archive's members are units at the addresses loose files have
    // (two mods' BSAs replacing one texture conflict, and so does a loose replacer of it), a plugin's records are units of
    // the plugin that owns them (two plugins overriding one Skyrim.esm record conflict; the same override twice does not;
    // their own new records never do). Teeth: list no archive members; address a record by the plugin writing it; compare
    // records by name only.
    void units_inside_archives_and_plugins_conflict()
    {
        namespace fs = std::filesystem;
        const fs::path B = fs::temp_directory_path() / ("vg_inner_units_" + std::to_string(::getpid()));
        fs::remove_all(B); fs::create_directories(B);
        auto u32 = [](std::string &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<char>(x >> (8 * i))); };
        auto u16 = [](std::string &v, uint16_t x) { v.push_back(static_cast<char>(x)); v.push_back(static_cast<char>(x >> 8)); };
        //a BSA 105 of one folder, its members stored
        const auto bsa = [&](const std::string &Folder, const std::vector<std::pair<std::string, std::string>> &Members) {
            std::string H; uint32_t tfin = 0; for (auto &m : Members) tfin += static_cast<uint32_t>(m.first.size()) + 1;
            const uint32_t tfn = static_cast<uint32_t>(Folder.size()) + 1;
            u32(H, 0x00415342); u32(H, 105); u32(H, 36); u32(H, 0x3); u32(H, 1); u32(H, static_cast<uint32_t>(Members.size())); u32(H, tfn); u32(H, tfin); u32(H, 0);
            u32(H, 1); u32(H, 0); u32(H, static_cast<uint32_t>(Members.size())); u32(H, 0); u32(H, 0); u32(H, 0);
            const uint32_t region = 36 + 24 + 1 + tfn + 16 * static_cast<uint32_t>(Members.size()) + tfin;
            H.push_back(static_cast<char>(tfn)); H += Folder; H.push_back(0);
            uint32_t at = region;
            for (auto &m : Members) { u32(H, 7); u32(H, 0); u32(H, static_cast<uint32_t>(m.second.size())); u32(H, at); at += static_cast<uint32_t>(m.second.size()); }
            for (auto &m : Members) { H += m.first; H.push_back(0); }
            for (auto &m : Members) H += m.second;
            return H;
        };
        //a Skyrim plugin with Skyrim.esm as its master: records (FormID, data) in one group
        const auto esp = [&](const std::vector<std::pair<uint32_t, std::string>> &Recs) {
            std::string D; D += "HEDR"; u16(D, 12); u32(D, 0x3FD9999A); u32(D, static_cast<uint32_t>(Recs.size())); u32(D, 0x800);
            D += "MAST"; u16(D, 11); D += "Skyrim.esm"; D.push_back(0); D += "DATA"; u16(D, 8); u32(D, 0); u32(D, 0);
            std::string P = "TES4"; u32(P, static_cast<uint32_t>(D.size())); u32(P, 0); u32(P, 0); u32(P, 0); u32(P, 0); P += D;
            std::string G;
            for (auto &r : Recs) { std::string E = "EDID"; u16(E, static_cast<uint16_t>(r.second.size() + 1)); E += r.second; E.push_back(0);
                G += "WEAP"; u32(G, static_cast<uint32_t>(E.size())); u32(G, 0); u32(G, r.first); u32(G, 0); u32(G, 0); G += E; }
            P += "GRUP"; u32(P, 24 + static_cast<uint32_t>(G.size())); P += "WEAP"; u32(P, 0); u32(P, 0); u32(P, 0); P += G;
            return P;
        };
        const auto put = [&](const char *N, const std::string &Bytes) { std::ofstream(B / N, std::ios::binary) << Bytes; };
        put("ModA.bsa", bsa("textures", { {"x.dds", "AAAA"}, {"a.dds", "aa"} }));
        put("ModB.bsa", bsa("textures", { {"x.dds", "BBBB"}, {"b.dds", "bb"} }));
        put("x.dds", "LOOSE");
        put("A.esp", esp({ {0x00012EB7u, "IronSword"}, {0x01000800u, "NewA"} }));
        put("A2.esp", esp({ {0x00012EB7u, "IronSword"}, {0x01000800u, "NewA"} }));
        put("B.esp", esp({ {0x00012EB7u, "IronSwordB"}, {0x00012EB8u, "Other"} }));
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        const auto part = [&](const char *Id, const char *File, const char *Target) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}},
                                                                                     json{{"FILE", File}, {"SOURCE", std::string("Qm") + Id}, {"TARGET", Target}} })} }, B.string());
        };
        idx.Nodes["ba"] = part("ba", "ModA.bsa", "FILES/C:/g/Data");
        idx.Nodes["bb"] = part("bb", "ModB.bsa", "FILES/C:/g/Data");
        idx.Nodes["lx"] = part("lx", "x.dds", "FILES/C:/g/Data/textures");
        idx.Nodes["ea"] = part("ea", "A.esp", "FILES/C:/g/Data");
        idx.Nodes["ea2"] = part("ea2", "A2.esp", "FILES/C:/g/Data");
        idx.Nodes["eb"] = part("eb", "B.esp", "FILES/C:/g/Data");
        {   //a mod as most are shipped: a zip (stored) holding Data/ModC.bsa
            int Ze = 0; zip_t *Z = zip_open((B / "modc.zip").c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Ze);
            static const std::string C = bsa("textures", { {"x.dds", "CCCC"}, {"c.dds", "cc"} });
            zip_source_t *Src = zip_source_buffer(Z, C.data(), C.size(), 0);
            const zip_int64_t I = zip_file_add(Z, "Data/ModC.bsa", Src, ZIP_FL_ENC_UTF_8);
            zip_set_file_compression(Z, static_cast<zip_uint64_t>(I), ZIP_CM_STORE, 0);
            zip_close(Z);
        }
        idx.Nodes["zc"] = parse(json{ {"CID", "zc"}, {"LABEL", "zc"}, {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}},
                                     json{{"ZIP", "modc.zip"}, {"SOURCE", "QmZc"}, {"TARGET", "FILES/C:/g"}} })} }, B.string());
        finish(idx);
        using V = std::vector<std::string>;
        const auto Addrs = [&](const V &Parts) {
            std::set<std::string> S;
            for (const auto &C : Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", Parts))) S.insert(C.Address);
            return S;
        };
        const auto Bsas = Addrs(V{"ba", "bb"});
        QVERIFY2(Bsas.count("FILES/C:/g/Data/textures/x.dds"), "two archives replacing one member");
        QVERIFY(!Bsas.count("FILES/C:/g/Data/textures/a.dds") && !Bsas.count("FILES/C:/g/Data/textures/b.dds"));
        QVERIFY2(Addrs(V{"ba", "lx"}).count("FILES/C:/g/Data/textures/x.dds"), "a loose replacer of an archive's member");
        const auto Zipped = Addrs(V{"ba", "zc"});
        QVERIFY2(Zipped.count("FILES/C:/g/Data/textures/x.dds"), "an archive inside a mod's zip");
        QVERIFY(!Zipped.count("FILES/C:/g/Data/textures/c.dds"));
        const auto Esps = Addrs(V{"ea", "eb"});
        QVERIFY2(Esps.count("RECORD/skyrim.esm/012EB7"), "two plugins overriding one master record");
        QVERIFY(!Esps.count("RECORD/skyrim.esm/012EB8") && !Esps.count("RECORD/a.esp/000800"));
        QVERIFY2(!Addrs(V{"ea", "ea2"}).count("RECORD/skyrim.esm/012EB7"), "the same override twice is one write");
        fs::remove_all(B);
    }

    void parts_writing_one_unit_differently_conflict()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        const auto part = [](const char *Id, const json &Into, json Layers) {
            json L = json::array({ json{{"INTO", Into}} });
            for (auto &X : Layers) L.push_back(X);
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", L} }, "/tmp/vg_bundle");
        };
        idx.Nodes["a"] = part("a", json::array({"game"}), json::array({ json{{"ENV", {{"X", "1"}}}}, json{{"FILE", "Readme.txt"}, {"SOURCE", "QmA"}, {"TARGET", "FILES/C:/g"}} }));
        idx.Nodes["b"] = part("b", json::array({"game"}), json::array({ json{{"ENV", {{"X", "2"}}}}, json{{"FILE", "README.TXT"}, {"SOURCE", "QmB"}, {"TARGET", "FILES/C:/g"}} }));
        idx.Nodes["c"] = part("c", json::array({"game"}), json::array({ json{{"ENV", {{"X", "1"}}}} }));
        idx.Nodes["pa"] = part("pa", json::array({"a"}), json::array({ json{{"ENV", {{"X", "3"}}}} }));
        finish(idx);
        using V = std::vector<std::string>;
        const auto Has = [](const std::vector<Shelf::PartConflict> &Cs, const std::string &Addr, const std::string &A, const std::string &B) {
            for (const auto &C : Cs) if (C.Address == Addr && ((C.A == A && C.B == B) || (C.A == B && C.B == A))) return true;
            return false;
        };
        const auto AB = Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"a", "b"}));
        QVERIFY(Has(AB, "ENV/X", "a", "b"));
        bool File = false;
        for (const auto &C : AB) if (C.Address.find("README.TXT") != std::string::npos || C.Address.find("Readme.txt") != std::string::npos) File = true;
        QVERIFY2(File, "one file path written by two parts with different bytes");
        QVERIFY(Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"a", "c"})).empty());   // identical
        {   // identical writes with another between them (a=1, b=2, c=1): a and c agree, each disagrees with b
            const auto Abc = Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"a", "b", "c"}));
            QVERIFY(!Has(Abc, "ENV/X", "a", "c"));
            QVERIFY(Has(Abc, "ENV/X", "a", "b") && Has(Abc, "ENV/X", "b", "c"));
        }
        const auto P = Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"a", "pa", "b"}));
        QVERIFY(!Has(P, "ENV/X", "a", "pa"));                                                         // a patch on a
        QVERIFY(Has(P, "ENV/X", "pa", "b") || Has(P, "ENV/X", "a", "b"));
        //binary edits merge by byte range (Wipeout XL's 200 fps and widescreen patch one exe at disjoint offsets);
        //overlapping ranges written differently conflict; appended lines are a set
        const auto poke = [](const char *Off, const char *Val) { return json{{"MODE", "Poke"}, {"OFFSET", Off}, {"VALUE", Val}}; };
        idx.Nodes["fps"] = part("fps", json::array({"game"}), json::array({ json{{"EDIT", json::array({ poke("0x100", "1e00"), json{{"MODE", "AppendLine"}, {"VALUE", "fps=200"}} })}, {"TARGET", "FILES/C:/g/w.exe"}} }));
        idx.Nodes["wide"] = part("wide", json::array({"game"}), json::array({ json{{"EDIT", json::array({ poke("0x200", "aabbccdd"), json{{"MODE", "AppendLine"}, {"VALUE", "wide=1"}} })}, {"TARGET", "FILES/C:/g/w.exe"}} }));
        idx.Nodes["over"] = part("over", json::array({"game"}), json::array({ json{{"EDIT", json::array({ poke("0x202", "ffff") })}, {"TARGET", "FILES/C:/g/W.EXE"}} }));
        finish(idx);
        QVERIFY(Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"fps", "wide"})).empty());
        const auto O = Shelf::PartConflicts(idx, "game", Shelf::AppliedGrafts(idx, "game", V{"wide", "over"}));
        QCOMPARE(O.size(), size_t(1));
        if (!O.empty()) QVERIFY2(O[0].Address.find("@0x202-0x204") != std::string::npos, O[0].Address.c_str());
        //the user's resolution (the instance's RESOLUTIONS): the part kept folds after its siblings, so its write stands;
        //what goes into it still comes after it. Teeth: ignore the resolution (b, later by CID, keeps winning).
        const V Ab = Shelf::AppliedGrafts(idx, "game", V{"a", "b"});
        const auto EnvX = [&](const V &Order) {
            const Fold::Plan Pl = Fold::Resolve(ManifestModel::LibraryOf(idx), "game", {}, {}, Order);
            return Pl.Env.contains("X") ? Pl.Env["X"].get<std::string>() : std::string();
        };
        const V Kept = Shelf::OrderResolved(idx, Ab, json{{"ENV/X", "a"}});
        QCOMPARE(EnvX(Kept), std::string("1"));
        QCOMPARE(EnvX(Shelf::OrderResolved(idx, Ab, json{{"ENV/X", "b"}})), std::string("2"));
        //kept b against pa (a part one INTO step deeper, into a): b goes after pa, as its links allow, so X is b's
        const V WithPatch = Shelf::OrderResolved(idx, Shelf::AppliedGrafts(idx, "game", V{"a", "pa", "b"}), json{{"ENV/X", "b"}});
        QVERIFY(std::find(WithPatch.begin(), WithPatch.end(), "pa") > std::find(WithPatch.begin(), WithPatch.end(), "a"));
        QCOMPARE(EnvX(WithPatch), std::string("2"));
        QVERIFY(Shelf::ResolutionHolds(WithPatch, {"ENV/X", "pa", "b"}, "b"));
        //a resolution that cannot hold is never reported settled: the kept part is not one of the two, or not applied
        QVERIFY(!Shelf::ResolutionHolds(Kept, {"ENV/X", "a", "b"}, "c"));
        QVERIFY(!Shelf::ResolutionHolds(Ab, {"ENV/X", "a", "b"}, "a"));                // a is not after b in this order
        //two resolutions between one pair that need it both ways: one of them does not hold
        {
            const V Both = Shelf::OrderResolved(idx, Ab, json{{"ENV/X", "a"}, {"ENV/Y", "b"}});
            QVERIFY(!(Shelf::ResolutionHolds(Both, {"ENV/X", "a", "b"}, "a") && Shelf::ResolutionHolds(Both, {"ENV/Y", "a", "b"}, "b")));
        }
        //three parts at one address, b kept: b folds after both a and c, so every pair there is settled
        {
            idx.Nodes["c3"] = part("c3", json::array({"game"}), json::array({ json{{"ENV", {{"X", "3"}}}} }));
            finish(idx);
            const V Three = Shelf::OrderResolved(idx, Shelf::AppliedGrafts(idx, "game", V{"a", "b", "c3"}), json{{"ENV/X", "b"}});
            QCOMPARE(EnvX(Three), std::string("2"));
            const auto All = Shelf::PartConflicts(idx, "game", Three);
            for (const auto &C : All) if (C.Address == "ENV/X") QVERIFY2(Shelf::ResolutionHolds(Three, C, "b", All), (C.A + "/" + C.B).c_str());
        }
        //a part whose INTO lists alternatives needs only one of them before it: a resolution elsewhere moves nothing else
        {
            idx.Nodes["e2"] = part("e2", json::array({"game"}), json::array({ json{{"ENV", {{"Z", "e"}}}} }));
            idx.Nodes["alt"] = part("alt", json::array({"game", "e2"}), json::array({ json{{"ENV", {{"Z", "alt"}}}} }));
            finish(idx);
            const V In{"alt", "e2", "a"};                                 // alt before e2: allowed, it goes into game
            QCOMPARE(Shelf::OrderResolved(idx, In, json{{"ENV/X", "a"}}), In);
        }
        //a FILES address is matched without case, as its unit is
        QCOMPARE(Shelf::ResolutionFor(json{{"FILES/C:/g/README.TXT", "a"}}, "FILES/C:/g/Readme.txt"), std::string("a"));
        QVERIFY(Shelf::ResolutionFor(json{{"ENV/x", "a"}}, "ENV/X").empty());
        //and the launch reads the instance's RESOLUTIONS: the kept part stands, and the plan says so
        {
            QTemporaryDir ud;   // the instance's resolutions: here, never in a user-data folder another test reads
            const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
            InstanceStore::SetSetting(cfg, "game", "RESOLUTIONS", json{{"ENV/X", "a"}});
            NodeIndex env;                                                     // parts that only set ENV (no files to place)
            env.Nodes["game"] = launchNode("game", kMachine, {});
            env.Nodes["a"] = part("a", json::array({"game"}), json::array({ json{{"ENV", {{"X", "1"}}}} }));
            env.Nodes["b"] = part("b", json::array({"game"}), json::array({ json{{"ENV", {{"X", "2"}}}} }));
            finish(env);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &env; cp.LaunchNodeId = "game"; cp.Grafts = V{"a", "b"};
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            QCOMPARE(cp.AppliedGrafts.back(), std::string("a"));
            QCOMPARE(cp.LaunchEnv.value("X", std::string()), std::string("1"));
            bool Kept = false;
            for (const auto &C : cp.PartConflicts) if (C.value("address", std::string()) == "ENV/X" && C.value("kept", std::string()) == "a") Kept = true;
            QVERIFY(Kept);
        }
    }

    // An end built on ends (AoE2: Forgotten Empires INTO 1.0e, an end that goes INTO 1.0c): launching it runs the root end
    // with every end between applied — those are its row's hard links — as its own row and under its own tile. Teeth: force only the launched part (FE is offered on 1.0e only, so it "cannot be applied").
    void an_end_built_on_ends_runs_with_the_ends_between()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", Title}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        idx.Nodes["c"] = parse(json{ {"CID", "c"}, {"LABEL", "c"}, {"VARIANT", "1.0c"},
            {"LAYERS", json::array({ json{{"DIR", "c"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors v1.0c") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["e"] = parse(json{ {"CID", "e"}, {"LABEL", "e"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"c"})}}, json{{"DIR", "e"}},
                json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors v1.0e") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["fe"] = parse(json{ {"CID", "fe"}, {"LABEL", "fe"}, {"VARIANT", "2.2"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"e"})}}, json{{"DIR", "fe"}},
                json{{"EXEC", NodeFixture::ExecOf({ entry("fe.exe", "749000001", "Forgotten Empires v2.2") })}} })} }, "/tmp/vg_bundle");
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "fe";
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            QCOMPARE(cp.AppliedGrafts, (std::vector<std::string>{"e", "fe"}));
            QVERIFY(cp.ComposedExec.value("CONTENTPATH", std::string()).find("fe.exe") != std::string::npos);
        }
        {   // and as its card shows it (its own row, its own tile)
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "fe"; cp.LaunchFace = "749000001";
            QVERIFY(LaunchResolver::InitializeFromNode(cp, cfg));
            QCOMPARE(cp.AppliedGrafts, (std::vector<std::string>{"e", "fe"}));
        }
        QCOMPARE(*Shelf::IntoChain(idx, "fe", "c"), (std::vector<std::string>{"e"}));
        QVERIFY(Shelf::IntoChain(idx, "fe", "e")->empty());
        QVERIFY(!Shelf::IntoChain(idx, "c", "fe"));
    }

    // --with adds parts to the selection the launch would use: a fresh instance keeps its RECOMMENDED parts and gains
    // the named one; a part not offered here is reported dropped, never applied. Teeth: replace the selection with the
    // --with list (the recommended part is lost); drop the dropped list.
    // A source port's engine as a graft (09 §9.4, 10 §10.6): a part INTO the release whose data it reads, with its own
    // entry on the release's card and a custom-platform runner. It is a ROW of that card — never an add-on offered on
    // the release's row — and launching it runs ITS entry over the release. Teeth: list graft rows as add-ons; launch the
    // release's entry for the engine row.
    void a_source_port_engine_is_a_graft_row_not_an_add_on()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        idx.Nodes["engine"] = parse(json{ {"CID", "engine"}, {"LABEL", "engine"}, {"VARIANT", "Open-source engine"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}},
                json{{"EXEC", json{{"Engine", {{"HOST", kMachine}, {"EXE", "engine.bin"}, {"TILE", {{"UID", "game"}}}}}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"game"})}, {"RECOMMENDED", true}}, json{{"DIR", "mod"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        std::vector<std::string> Pre;
        QCOMPARE(Shelf::OfferedGrafts(idx, "game", &Pre), (std::vector<std::string>{"mod"}));   // the engine is a row, not a box to tick
        QCOMPARE(Pre, (std::vector<std::string>{"mod"}));
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "engine";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QCOMPARE(cp.ComposedExec.value("LABEL", std::string()), std::string("Engine"));        // its own entry runs
        const std::set<std::string> Applied(cp.AppliedGrafts.begin(), cp.AppliedGrafts.end());
        QCOMPARE(Applied, (std::set<std::string>{"engine", "mod"}));                              // over the release, its mods pre-ticked
    }

    // The entry a row's launch runs (09 §9.2): its own RECOMMENDED entry on its face, else its own on its face, else
    // an inherited RECOMMENDED one on its face, else any on its face, else the first. Teeth: drop any one step (A runs
    // for B, X for Y, the off-face entry for the on-face one).
    void a_launch_runs_the_rows_entry_in_the_default_order()
    {
        const auto E = [](const char *Exe, const char *Uid, bool Rec) {
            json J{ {"HOST", kMachine}, {"EXE", Exe} };
            if (*Uid) J["TILE"] = json{{"UID", Uid}};
            if (Rec) J["RECOMMENDED"] = true;
            return J;
        };
        NodeIndex idx;
        idx.Nodes["two"] = parse(json{ {"CID", "two"}, {"LABEL", "two"}, {"VARIANT", "two"}, {"LAYERS", json::array({ json{{"DIR", "g"}},
            json{{"EXEC", json{{"A", E("a.exe", "two", false)}, {"B", E("b.exe", "two", true)}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["b3"] = parse(json{ {"CID", "b3"}, {"LABEL", "b3"}, {"LAYERS", json::array({ json{{"DIR", "g"}},
            json{{"EXEC", json{{"X", E("x.exe", "inh", false)}, {"Y", E("y.exe", "inh", true)}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["inh"] = parse(json{ {"CID", "inh"}, {"LABEL", "inh"}, {"VARIANT", "inh"}, {"LAYERS", json::array({ json{{"NODE", "b3"}} })} }, "/tmp/vg_bundle");
        idx.Nodes["b4"] = parse(json{ {"CID", "b4"}, {"LABEL", "b4"}, {"LAYERS", json::array({ json{{"DIR", "g"}},
            json{{"EXEC", json{{"Off", E("o.exe", "", false)}, {"On", E("n.exe", "face", false)}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["face"] = parse(json{ {"CID", "face"}, {"LABEL", "face"}, {"VARIANT", "face"}, {"LAYERS", json::array({ json{{"NODE", "b4"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        const auto runs = [&](const char *Id) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Id;
            if (!LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}})) return std::string("(failed)");
            return cp.ComposedExec.value("LABEL", std::string());
        };
        QCOMPARE(runs("two"), std::string("B"));
        QCOMPARE(runs("inh"), std::string("Y"));
        QCOMPARE(runs("face"), std::string("On"));
    }

    void with_adds_parts_to_the_selection()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        const auto part = [](const char *Id, const char *Into, bool Rec) {
            json J{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ json{{"INTO", json::array({Into})}}, json{{"DIR", Id}} })} };
            if (Rec) J["LAYERS"][0]["RECOMMENDED"] = true;
            return parse(J, "/tmp/vg_bundle");
        };
        idx.Nodes["rec"] = part("rec", "game", true);
        idx.Nodes["extra"] = part("extra", "game", false);
        idx.Nodes["other"] = part("other", "elsewhere", false);
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.WithGrafts = { "extra", "other" };
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QCOMPARE(std::set<std::string>(cp.AppliedGrafts.begin(), cp.AppliedGrafts.end()), (std::set<std::string>{"rec", "extra"}));
        QVERIFY(std::find(cp.DroppedGrafts.begin(), cp.DroppedGrafts.end(), "other") != cp.DroppedGrafts.end());
    }

    // A part with several INTO layers goes into ALL of them (a patch between two mods): it is offered, and applies,
    // only once every layer has a member applied; each layer's members are alternatives. Teeth: offer on the first
    // layer alone; apply it with one of its two mods.
    void a_part_into_two_mods_needs_both()
    {
        NodeIndex idx;
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        const auto part = [](const std::string &Id, const std::vector<json> &Intos) {
            json L = json::array();
            for (const auto &I : Intos) L.push_back(json{{"INTO", I}});
            L.push_back(json{{"DIR", Id}});
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", L} }, "/tmp/vg_bundle");
        };
        idx.Nodes["ma"] = part("ma", { json::array({"game"}) });
        idx.Nodes["mb"] = part("mb", { json::array({"game"}) });
        idx.Nodes["mb2"] = part("mb2", { json::array({"game"}) });
        idx.Nodes["patch"] = part("patch", { json::array({"ma"}), json::array({"mb", "mb2"}) });
        finish(idx);
        using V = std::vector<std::string>;
        std::vector<std::string> Pre;
        const auto offered = [&](const V &T) { const auto O = Shelf::OfferedGrafts(idx, "game", &Pre, T); return std::set<std::string>(O.begin(), O.end()); };
        QVERIFY(!offered({}).count("patch"));
        QVERIFY(!offered({"ma"}).count("patch"));
        QVERIFY(!offered({"mb"}).count("patch"));
        QVERIFY(offered({"ma", "mb"}).count("patch"));
        QVERIFY(offered({"ma", "mb2"}).count("patch"));                        // either member of the second layer
        QCOMPARE(Shelf::AppliedGrafts(idx, "game", V{"patch", "ma"}), (V{"ma"}));
        const V All = Shelf::AppliedGrafts(idx, "game", V{"patch", "mb", "ma"});
        QCOMPARE(std::set<std::string>(All.begin(), All.end()), (std::set<std::string>{"ma", "mb", "patch"}));
        QCOMPARE(All.back(), std::string("patch"));                             // after both it goes into
    }

    // Ticking a graft in the pre-launch window goes through TickGraft: the graft just ticked wins, so grafts that
    // exclude each other (AoE2's three soundtracks: TC "never with AoK", Both "never with AoK or TC" — a CID can only
    // name an earlier one) behave as a choice of one, whichever side carries the condition. A graft the ticked one needs, or one unrelated,
    // stays ticked. Teeth: skip the unticking (all three stay ticked); untick without judging the whole list (a graft
    // on a graft whose base is ticked unticks that base); keep G in its old place (it must go last).
    void ticking_a_graft_unticks_the_grafts_it_excludes()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        const auto graft = [](const std::string &Id, const std::string &When) {
            json In = json{{"INTO", json::array({"game"})}};
            if (!When.empty()) In["WHEN"] = When;
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", json::array({ In, json{{"DIR", Id}} })} }, "/tmp/vg_bundle");
        };
        const std::string Aok = CidOf("aok"), Tc = CidOf("tc");
        idx.Nodes[Aok]    = graft(Aok,    "");
        idx.Nodes[Tc]     = graft(Tc,     Aok + " == false");
        idx.Nodes["both"] = graft("both", Aok + " == false && " + Tc + " == false");
        idx.Nodes["free"] = graft("free", "");
        idx.Nodes["ona"] = parse(json{ {"CID", "ona"}, {"LABEL", "ona"},
            {"LAYERS", json::array({ json{{"INTO", json::array({Aok})}}, json{{"DIR", "ona"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        using V = std::vector<std::string>;
        V Un;
        QCOMPARE(Shelf::TickGraft(idx, "game", V{ "free", Aok }, "both", &Un), (V{ "free", "both" }));   // both's condition
        QCOMPARE(Un, V{ Aok });
        Un.clear();
        QCOMPARE(Shelf::TickGraft(idx, "game", V{ "both", "free" }, Aok, &Un), (V{ "free", Aok }));    // the other side's condition
        QCOMPARE(Un, V{ "both" });
        Un.clear();
        QCOMPARE(Shelf::TickGraft(idx, "game", V{ Tc }, Aok, &Un), (V{ Aok }));
        QCOMPARE(Un, V{ Tc });
        Un.clear();
        QCOMPARE(Shelf::TickGraft(idx, "game", V{ Aok, "free" }, "ona", &Un), (V{ Aok, "free", "ona" })); // needs aok
        QVERIFY(Un.empty());
        QCOMPARE(Shelf::TickGraft(idx, "game", V{ "free", Aok }, "ona", &Un), (V{ "free", Aok, "ona" })); // ...ticked after free:
        QVERIFY(Un.empty());                                                     // ona cannot apply before aok, so free is not to blame
        QCOMPARE(Shelf::AppliedGrafts(idx, "game", Shelf::TickGraft(idx, "game", V{ Aok, Tc, "both" }, Tc)),
                 (V{ Tc }));                                                                  // one soundtrack applies
    }

    // `--tile <UID> [--variant <name>]` launches what the shelf shows: the named variant under that tile, else the
    // tile's default row (the variant whose entry on it is RECOMMENDED). A variant presenting two tiles is a row under each.
    void the_row_under_a_tile_is_picked_by_variant_name_or_recommendation()
    {
        NodeIndex idx;
        const auto variant = [](const char *Id, const char *Name, json Faces, json Rec) {
            json Entries = json::object();
            for (const auto &U : Faces)
            {
                json E{ {"HOST", kMachine}, {"EXE", "g.exe"}, {"TILE", {{"UID", U}, {"TITLE", "T" + U.get<std::string>()}}} };
                for (const auto &R : Rec) if (R == U) E["RECOMMENDED"] = true;       // the card's recommended row
                Entries["Play " + U.get<std::string>()] = std::move(E);
            }
            json N = { {"CID", Id}, {"LABEL", Id}, {"VARIANT", Name}, {"LAYERS", json::array({ json{{"EXEC", Entries}} })} };
            return parse(N, "/tmp/vg_bundle");
        };
        idx.Nodes["old"] = variant("old", "1.00", json::array({"802"}), json::array());
        idx.Nodes["new"] = variant("new", "1.28", json::array({"802", "803"}), json::array({"802"}));   // recommended under 802 only
        idx.Nodes["tft"] = variant("tft", "1.31", json::array({"803"}), json::array({"803"}));
        finish(idx);
        std::string Why;
        QCOMPARE(Shelf::RowUnderTile(idx, "802", ""), std::string("new"));      // the recommended row
        QCOMPARE(Shelf::RowUnderTile(idx, "803", ""), std::string("tft"));      // recommended here, not "new"
        QCOMPARE(Shelf::RowUnderTile(idx, "802", "1.00"), std::string("old"));
        QCOMPARE(Shelf::RowUnderTile(idx, "803", "1.28"), std::string("new"));  // a row under both tiles
        QCOMPARE(Shelf::RowUnderTile(idx, "803", "1.00", &Why), std::string());
        QVERIFY2(Why.find("1.28") != std::string::npos && Why.find("1.31") != std::string::npos, Why.c_str());   // it says what there is
        QCOMPARE(Shelf::RowUnderTile(idx, "999", "", &Why), std::string());
        QVERIFY2(Why.find("999") != std::string::npos, Why.c_str());
    }

    // A tile is one presentable game: each UID is its own card — the expansion too, nested after its base game by
    // PARENTUID — and a version presenting two tiles is a row under both. Launched from a card it runs AS that tile:
    // %UID% is the card's, and (no entry named) the entry that presents it runs.
    void each_tile_is_a_card_and_a_row_runs_as_its_card()
    {
        NodeIndex idx;
        const auto entry = [](const char *Label, const char *Uid, const char *Parent, const char *Exe, const char *Title) {
            json T = {{"UID", Uid}, {"TITLE", Title}};
            if (Parent) T["PARENTUID"] = Parent;
            return json{ {"LABEL", Label}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", T} };
        };
        // v1 presents both RoC (802) and TFT (803, child of 802); v2 presents only RoC; z and s are unrelated games.
        // Titles disagree with UID order, and s's title falls BETWEEN RoC's and TFT's: a UID sort, or a flat title sort
        // that ignores PARENTUID, both get the order wrong.
        json Tft = entry("The Frozen Throne", "803", "802", "tft.exe", "Warcraft III: The Frozen Throne");
        Tft["RECOMMENDED"] = true;
        idx.Nodes["v1"] = parse(json{ {"CID", "v1"}, {"LABEL", "v1"}, {"VARIANT", "1.0"},
            {"LAYERS", json::array({ json{{"DIR", "g"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("Reign of Chaos", "802", nullptr, "roc.exe", "Warcraft III: Reign of Chaos"), Tft })}} })} }, "/tmp/vg_bundle");
        //v2's own RoC entry: whole (an entry write replaces it), the recommended row of 802.
        json Roc2 = entry("Reign of Chaos", "802", nullptr, "roc2.exe", "Warcraft III: Reign of Chaos");
        Roc2["RECOMMENDED"] = true;
        idx.Nodes["v2"] = parse(json{ {"CID", "v2"}, {"LABEL", "v2"}, {"VARIANT", "2.0"},
            {"LAYERS", json::array({ json{{"NODE", "v1"}}, json{{"EXEC", NodeFixture::ExecOf({ Roc2 })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["z"] = parse(json{ {"CID", "z"}, {"LABEL", "z"}, {"VARIANT", "1"},
            {"LAYERS", json::array({ json{{"DIR", "z"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("Play", "9", nullptr, "z.exe", "Age of Empires II") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["s"] = parse(json{ {"CID", "s"}, {"LABEL", "s"}, {"VARIANT", "1"},
            {"LAYERS", json::array({ json{{"DIR", "s"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("Play", "850", nullptr, "s.exe", "Warcraft III: Scenario Pack") })}} })} }, "/tmp/vg_bundle");
        finish(idx);

        std::vector<std::string> Order;
        std::map<std::string, std::vector<std::string>> Rows;
        for (const auto &T : Shelf::Tiles(idx))
        {
            Order.push_back(T.Uid);
            for (const Node *N : T.Rows) Rows[T.Uid].push_back(N->NodeId);
        }
        QCOMPARE(Order, (std::vector<std::string>{"9", "802", "803", "850"}));       // families by title; the child follows its base game
        QCOMPARE(Rows["802"], (std::vector<std::string>{"v2", "v1"}));                // recommended under 802 first
        QCOMPARE(Rows["803"], (std::vector<std::string>{"v1", "v2"}));                // v2 contains v1: its fold presents 803 too
        QCOMPARE(idx.Tile("803")->value("PARENTUID", std::string()), std::string("802"));

        const json cfg = json{{"Settings", json::object()}};
        const auto launch = [&](const char *Node, const char *Face) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Node; cp.LaunchFace = Face;
            const bool Ok = LaunchResolver::InitializeFromNode(cp, cfg);
            const std::string Exe = cp.ComposedExec.is_object() ? cp.ComposedExec.value("CONTENTPATH", std::string()) : std::string();
            return std::make_tuple(Ok, Exe, cp.PackageName);
        };
        const auto [OkT, ExeT, NameT] = launch("v1", "803");
        QVERIFY(OkT);
        QVERIFY2(ExeT.find("tft.exe") != std::string::npos, ExeT.c_str());          // the card's entry runs
        QCOMPARE(NameT, std::string("Warcraft III: The Frozen Throne"));
        const auto [OkR, ExeR, NameR] = launch("v1", "802");
        QVERIFY(OkR && ExeR.find("roc.exe") != std::string::npos);
        QCOMPARE(NameR, std::string("Warcraft III: Reign of Chaos"));
        QVERIFY(!std::get<0>(launch("v1", "9")));                                     // a tile the node does not present
    }

    // A graft whose own entry carries a TILE (a mod with its own card: Forgotten Empires over The Conquerors) is a card:
    // its rows are the ends it goes INTO (exactly those: an end containing one is another game), and launched from that card a
    // row runs with the graft applied, as the card's title, by the graft's entry. From the version's own card nothing
    // changes. Teeth: no card for a graft (ShelfTiles); launch the row without the graft (the version's own entry runs).
    void a_graft_with_its_own_tile_is_a_card_of_what_it_applies_onto()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", "Play"}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["tc2"] = parse(json{ {"CID", "tc2"}, {"LABEL", "tc2"}, {"VARIANT", "1.0f"},
            {"LAYERS", json::array({ json{{"NODE", "tc"}}, json{{"DIR", "tc2"}} })} }, "/tmp/vg_bundle");       // contains tc
        idx.Nodes["aok"] = parse(json{ {"CID", "aok"}, {"LABEL", "aok"}, {"VARIANT", "2.0a"},
            {"LAYERS", json::array({ json{{"DIR", "aok"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("aok.exe", "749", "The Age of Kings") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["fe"] = parse(json{ {"CID", "fe"}, {"LABEL", "fe"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}}, json{{"DIR", "fe"}},
                json{{"EXEC", NodeFixture::ExecOf({ entry("fe.exe", "749000001", "Forgotten Empires") })}} })} }, "/tmp/vg_bundle");
        finish(idx);

        const Shelf::Tile *Card = nullptr;
        const auto Shelf = Shelf::Tiles(idx);
        for (const auto &T : Shelf) if (T.Uid == "749000001") Card = &T;
        QVERIFY2(Card, "the graft's tile is not a card");
        std::set<std::string> Rows;
        for (const Node *N : Card->Rows) Rows.insert(N->NodeId);
        QCOMPARE(Rows, (std::set<std::string>{"tc"}));                                 // what it goes INTO: not tc2 (contains tc), not aok
        QCOMPARE(Card->Graft, std::string("fe"));

        const json cfg = json{{"Settings", json::object()}};
        const auto launch = [&](const char *Node, const char *Face) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Node; cp.LaunchFace = Face;
            const bool Ok = LaunchResolver::InitializeFromNode(cp, cfg);
            const std::string Exe = cp.ComposedExec.is_object() ? cp.ComposedExec.value("CONTENTPATH", std::string()) : std::string();
            return std::make_tuple(Ok, Exe, cp.PackageName, cp.AppliedGrafts);
        };
        QVERIFY(!std::get<0>(launch("tc2", "749000001")));                            // the hard link: tc2 is not tc
        const auto [Ok, Exe, Name, Grafts] = launch("tc", "749000001");
        QVERIFY(Ok);
        QVERIFY2(Exe.find("fe.exe") != std::string::npos, Exe.c_str());              // the graft's entry runs
        QCOMPARE(Name, std::string("Forgotten Empires"));
        QCOMPARE(Grafts, (std::vector<std::string>{"fe"}));
        QVERIFY(!std::get<0>(launch("aok", "749000001")));                            // not something it applies onto
        const auto [OkTc, ExeTc, NameTc, GraftsTc] = launch("tc", "13006");           // the version's own card: unchanged
        QVERIFY(OkTc && ExeTc.find("tc.exe") != std::string::npos && GraftsTc.empty());
    }

    // A graft can be a VERSION (it carries VARIANT): a row of its own that runs on a version it applies onto, with it
    // applied. Two such grafts presenting one tile are that card's rows (Forgotten Empires 2.2 and 2.5); one presenting
    // no tile is another row of its base's card (a mod build of The Conquerors). Teeth: launch the graft alone (no base:
    // nothing runs); leave tile-less graft versions off the shelf; accept a base it does not apply onto.
    void a_graft_can_be_a_version()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", "Play"}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["aok"] = parse(json{ {"CID", "aok"}, {"LABEL", "aok"}, {"VARIANT", "2.0a"},
            {"LAYERS", json::array({ json{{"DIR", "aok"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("aok.exe", "749", "The Age of Kings") })}} })} }, "/tmp/vg_bundle");
        const auto fe = [&](const char *Id, const char *Ver, const char *Exe) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"VARIANT", Ver},
                {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}}, json{{"DIR", Id}},
                    json{{"EXEC", NodeFixture::ExecOf({ entry(Exe, "749000001", "Forgotten Empires") })}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["fe22"] = fe("fe22", "2.2", "fe22.exe");
        idx.Nodes["fe25"] = fe("fe25", "2.5", "fe25.exe");
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"VARIANT", "1.0e + mod"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}}, json{{"DIR", "mod"}} })} }, "/tmp/vg_bundle");
        finish(idx);

        std::map<std::string, std::set<std::string>> Rows;
        for (const auto &T : Shelf::Tiles(idx)) for (const Node *N : T.Rows) Rows[T.Uid].insert(N->NodeId);
        QCOMPARE(Rows["749000001"], (std::set<std::string>{"fe22", "fe25"}));
        QCOMPARE(Rows["13006"], (std::set<std::string>{"tc", "mod"}));
        QCOMPARE(Shelf::GraftBases(idx, "fe25"), (std::vector<std::string>{"tc"}));

        const json cfg = json{{"Settings", json::object()}};
        const auto launch = [&](const char *Node, const char *Face, const char *Base) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Node; cp.LaunchFace = Face; cp.GraftBase = Base;
            const bool Ok = LaunchResolver::InitializeFromNode(cp, cfg);
            const std::string Exe = cp.ComposedExec.is_object() ? cp.ComposedExec.value("CONTENTPATH", std::string()) : std::string();
            return std::make_tuple(Ok, Exe, cp.PackageName, cp.AppliedGrafts);
        };
        const auto [Ok, Exe, Name, Grafts] = launch("fe25", "749000001", "");
        QVERIFY(Ok);
        QVERIFY2(Exe.find("fe25.exe") != std::string::npos, Exe.c_str());
        QCOMPARE(Name, std::string("Forgotten Empires"));
        QCOMPARE(Grafts, (std::vector<std::string>{"fe25"}));
        const auto [OkM, ExeM, NameM, GraftsM] = launch("mod", "13006", "");
        QVERIFY(OkM && ExeM.find("tc.exe") != std::string::npos);                    // the base's entry, with the mod
        QCOMPARE(GraftsM, (std::vector<std::string>{"mod"}));
        QVERIFY(!std::get<0>(launch("fe25", "749000001", "aok")));                     // not a base it applies onto
    }

    // A fresh instance launching a graft version gets the recommended add-ons with it (the GUI pre-ticks them; the CLI
    // and the audit must launch the same game), but never a sibling version: a graft with an EXEC is a row, never
    // pre-ticked, and its entry's RECOMMENDED picks the default row. Teeth: replace the fresh list with the forced graft
    // alone (no add-on); tick grafts with an EXEC too (the sibling version applies).
    void a_fresh_graft_version_launch_keeps_recommended_add_ons_only()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", "Play"}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, "/tmp/vg_bundle");
        const auto fe = [&](const char *Id, const char *Ver, const char *Exe, bool Rec) {
            json E = entry(Exe, "749000001", "Forgotten Empires");
            if (Rec) E["RECOMMENDED"] = true;                    // the card's recommended row
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"VARIANT", Ver},
                {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}}, json{{"DIR", Id}},
                    json{{"EXEC", NodeFixture::ExecOf({ E })}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["fe22"] = fe("fe22", "2.2", "fe22.exe", false);
        idx.Nodes["fe25"] = fe("fe25", "2.5", "fe25.exe", true);
        idx.Nodes["up"] = parse(json{ {"CID", "up"}, {"LABEL", "up"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}, {"RECOMMENDED", true}}, json{{"DIR", "up"}} })} }, "/tmp/vg_bundle");
        finish(idx);

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "fe25"; cp.LaunchFace = "749000001";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        const std::set<std::string> Applied(cp.AppliedGrafts.begin(), cp.AppliedGrafts.end());
        QCOMPARE(Applied, (std::set<std::string>{"fe25", "up"}));
    }

    // What a graft's row puts on disk is what it mounts: a graft that is a version runs on its base, so its content and
    // its "installed" verdict take in the base's, though the graft names the base only through INTO. Without that, a
    // peer downloading Forgotten Empires got FE and never The Conquerors, and the card read installed. A dehydrate
    // still removes the graft's own content only (the base is a version with its own card). Teeth: walk only the
    // row's closure (content or hydration); walk the base on dehydrate.
    void a_graft_rows_content_includes_its_base()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const std::string B = Dir.path().toStdString();
        const auto zip = [](const char *Name, const char *Cid) { return json{ {"ZIP", Name}, {"SOURCE", Cid}, {"TARGET", "FILES/%GameDir%"} }; };
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", "Play"}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        NodeIndex idx;
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ zip("tc.zip", "QmTc"), json{{"EXEC", NodeFixture::ExecOf({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, B);
        idx.Nodes["fe"] = parse(json{ {"CID", "fe"}, {"LABEL", "fe"}, {"VARIANT", "2.2"},
            {"LAYERS", json::array({ json{{"INTO", json::array({"tc"})}}, zip("fe.zip", "QmFe"),
                json{{"EXEC", NodeFixture::ExecOf({ entry("fe.exe", "749000001", "Forgotten Empires") })}} })} }, B);
        finish(idx);

        const std::vector<std::string> Cids = PackageCatalog::NodeContentCids(idx, "fe");
        QVERIFY2(std::find(Cids.begin(), Cids.end(), "QmTc") != Cids.end(), "the base's content is not part of the graft version's download");
        QVERIFY(std::find(Cids.begin(), Cids.end(), "QmFe") != Cids.end());

        const auto touch = [&](const char *Name) { std::ofstream(B + "/" + Name) << "x"; };
        touch("fe.zip");
        QVERIFY2(!Installed::HydrationMap(idx).at("fe").Hydrated, "the graft version reads installed with its base's content missing");
        touch("tc.zip");
        QVERIFY(Installed::HydrationMap(idx).at("fe").Hydrated);

        PackageCatalog::DehydrateNode(idx, "fe");
        QVERIFY2(std::filesystem::exists(B + "/tc.zip"), "removing the graft version deleted its base's content");
        QVERIFY(!std::filesystem::exists(B + "/fe.zip"));
    }

    // No authored native runner → the terminal is the synthesized passthrough sentinel.
    void chain_synthesizes_native_terminal_when_unauthored()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = chainRunner("proton", {"win32"}, kMachine);
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("proton"));
        QCOMPARE(ids.back(), std::string(LaunchResolver::kNativeTerminalId));
    }

    // Cross-platform: a SNES ROM whose only emulator is win32 → snes9x(snes→win32) under proton(win32→linux64) → native.
    void chain_snes_through_win32_emulator()
    {
        NodeIndex idx;
        idx.Nodes["snes9x"]    = chainRunner("snes9x", {"snes"}, "win32");
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, kMachine);
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", "snes", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE((int)ids.size(), 3);
        QCOMPARE(ids[0], std::string("snes9x"));
        QCOMPARE(ids[1], std::string("proton"));
        QCOMPARE(ids[2], std::string("nativerun"));
    }

    // Native linux content: the chain is just the native terminal (it runs the content directly).
    void chain_native_content_is_terminal_only()
    {
        NodeIndex idx;
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", kMachine, {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE((int)ids.size(), 1);
        QCOMPARE(ids[0], std::string("nativerun"));
    }

    // A runner that bridges AND is a terminal here (windows-native: win32 and win64, on a win64 host) is the whole chain
    // for a win32 game, once. Appending the terminal named it a second time, nested inside itself: AoE2's command on
    // Windows was "<GameDir>/__runner_<windows-native>__/<age2_x1.exe>". Teeth: always append the terminal and the chain
    // is ["both", "both"].
    void chain_a_bridge_that_is_the_terminal_ends_the_chain()
    {
        NodeIndex idx;
        idx.Nodes["both"] = chainRunner("both", {"win32", kMachine}, kMachine, "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE(ids, std::vector<std::string>({"both"}));
        // A bridge that is not a terminal here still gets one.
        idx.Nodes.erase("both");
        idx.Nodes["bridge"] = chainRunner("bridge", {"win32"}, kMachine, "");
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE(ids, std::vector<std::string>({"bridge", "nativerun"}));
    }

    // A persisted RUNNER_CHAIN pin is honored over the default bridge (terminal appended if the pin omits it).
    void chain_honours_persisted_pin()
    {
        NodeIndex idx;
        idx.Nodes["protonA"]   = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["protonB"]   = chainRunner("protonB", {"win32"}, kMachine);
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
        // The persisted pin now lives in the INSTANCE file, not GlobalConfig — write it there (temp UserDataRoot
        // isolates the test from the real ~/.VidyaGod). cp.InstanceName is empty ⇒ the resolver reads the active
        // instance, which is the DefaultInstance we just wrote.
        QTemporaryDir ud; QVERIFY(ud.isValid());
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        QVERIFY(InstanceStore::WriteConfig(cfg, "pkg", "DefaultInstance", json{{"RUNNER_CHAIN", json::array({"protonB"})}}));
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("protonB"));              // pin beats default (protonA sorts first)
        QCOMPARE(ids.back(), std::string("nativerun"));
    }

    // An unreachable platform (no bridging runner) resolves to an empty chain (→ InitializeFromNode aborts).
    void chain_unreachable_platform_is_empty()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = chainRunner("proton", {"win32"}, kMachine);
        Node launch = launchNode("game", "ps2", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QVERIFY(ids.empty());
    }

    // ResolveRunnerChain materializes RunnerLinks; the terminal link is a native-namespace passthrough.
    void chain_resolves_links_with_native_terminal()
    {
        NodeIndex idx;
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, kMachine);
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto links = LaunchResolver::ResolveRunnerChain(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE((int)links.size(), 2);
        QCOMPARE(links.front().NodeId, std::string("proton"));
        QVERIFY(links.back().NativeNamespace());               // terminal runs in the host namespace
        QVERIFY(links.back().Passthrough());                   // empty EXECUTABLE → forwards the inner command
    }

    // CustomVar new shape: values resolve RAW (no encoding — that's a use-site %KEY:format% concern); a secret+POOL
    // picks from the pool each launch; a CLI override beats the pool; a no-UI var is a plain binding.
    void resolve_custom_var_new_shape()
    {
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            json{{"TYPE", "CustomVar"}, {"KEY", "OPT"},    {"DEFAULT", "7"}, {"UI", {{"CONTROL", "int"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "BIND"},   {"DEFAULT", "1"}},
            json{{"TYPE", "CustomVar"}, {"KEY", "SECRET"}, {"UI", {{"CONTROL", "secret"}, {"POOL", json::array({"A","B","C"})}}}} })} };

        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["OPT"],  std::string("7"));   // RAW — NOT dword-encoded at resolution
          QCOMPARE(cp.CustomVariables["BIND"], std::string("1"));   // a no-UI binding
          const std::string s = cp.CustomVariables["SECRET"];
          QVERIFY(s == "A" || s == "B" || s == "C"); }              // picked from the pool

        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.VariableOverrides["SECRET"] = "X";                      // CLI/--var beats the pool
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["SECRET"], std::string("X")); }
    }

    // EVAL: a declaration whose value is an integer expression over other vars — UserPatch's Mini-map Colors from its
    // three installer options — evaluates once its operands resolve (forward references included); an override of an
    // operand reaches it; a plain declaration stays text. Teeth: skip EVAL (the value stays "1*2 | 0*32 | ...").
    void resolve_custom_var_eval()
    {
        auto cv = [](const std::string& k, const std::string& d){ return json{{"TYPE","CustomVar"},{"KEY",k},{"DEFAULT",d}}; };
        json mm = cv("MM", "%RED%*2 | %PURPLE%*32 | (1-%GREY%)*64"); mm["EVAL"] = true;
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            mm, cv("RED","1"), cv("PURPLE","0"), cv("GREY","1"), cv("RAW","%RED%*2") })} };
        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["MM"], std::string("2"));
          QCOMPARE(cp.CustomVariables["RAW"], std::string("1*2")); }            // no EVAL: text
        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.VariableOverrides["GREY"] = "0"; cp.VariableOverrides["PURPLE"] = "1";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["MM"], std::string("98")); }              // 2 | 32 | 64
    }

    // A MAP table at launch: its entries resolve with everything else (an entry may hold a %token%), the bound value
    // — default or overridden — reads through it at the use site, its ELSE fills what it lacks, and a table whose WHEN
    // is off passes values through. Teeth: skip TableEntries in the resolver (every lookup passes through).
    void resolve_custom_var_map_table()
    {
        auto cv = [](const std::string& k, const json& more){ json j{{"TYPE","CustomVar"},{"KEY",k}}; j.update(more); return j; };
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            cv("FWD", {{"DEFAULT", "keypad 0"}, {"UI", {{"CONTROL", "key"}}}}),
            cv("DIR", {{"DEFAULT", "C:/g"}}),
            cv("KEYS", {{"MAP", {{"keypad 0", "[0]"}, {"home", "%DIR%/h"}}}}),
            cv("STRICT", {{"MAP", {{"a", "A"}}}, {"ELSE", "none"}}),
            cv("OFF", {{"MAP", {{"keypad 0", "NO"}}}, {"WHEN", "%DIR%==never"}}),
            cv("DERIVED", {{"DEFAULT", "%FWD:map:KEYS%"}}) })} };
        const auto run = [&](const std::string& Fwd) {
            ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
            if (!Fwd.empty()) cp.VariableOverrides["FWD"] = Fwd;
            cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
            std::string S = "%FWD:map:KEYS%|%FWD:map:STRICT%|%FWD:map:OFF%|%DERIVED%";
            VarSubst::StringVariableSubstitution(S, cp.GetVariablesMap());
            return S;
        };
        QCOMPARE(run(""), std::string("[0]|none|keypad 0|[0]"));
        QCOMPARE(run("home"), std::string("C:/g/h|none|home|C:/g/h"));
        QCOMPARE(run("a"), std::string("a|A|a|a"));
    }

    // Absolute scope: a var's hierarchy-final value is visible to every reference, regardless of declaration order
    // (forward references, chains, and post-override values all resolve). Reference cycles terminate safely.
    void resolve_custom_var_absolute_scope()
    {
        auto cv = [](const std::string& k, const std::string& d){ return json{{"TYPE","CustomVar"},{"KEY",k},{"DEFAULT",d}}; };

        // Forward reference: A (declared first) references B (declared later).
        { const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
              cv("A","%B%"), cv("B","x") })} };
          ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["A"], std::string("x")); }

        // Chain A->B->C resolves fully.
        { const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
              cv("A","%B%"), cv("B","%C%"), cv("C","z") })} };
          ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["A"], std::string("z"));
          QCOMPARE(cp.CustomVariables["B"], std::string("z")); }

        // Hierarchy override is globally visible: a later component re-declares B; A sees the FINAL B.
        { const std::vector<OpGroup> pool = { OpGroup{"parent", json::array({ cv("B","x"), cv("A","%B%") })}, OpGroup{"child", json::array({ cv("B","y") })} };
          ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["B"], std::string("y"));     // child wins
          QCOMPARE(cp.CustomVariables["A"], std::string("y")); }   // and A sees the final value, not "x"

        // A reference cycle terminates (no hang); the residual token is left literal.
        { const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
              cv("A","%B%"), cv("B","%A%") })} };
          ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, json{{"Settings", json::object()}});
          QVERIFY(cp.CustomVariables["A"].find('%') != std::string::npos); }  // unresolved, but resolution returned
    }

    // ---- Cross-namespace nesting (ComposeGuestTarget / BoundaryLinkIndex / GuestPath) ----

    // A win32 emulator (snes9x) nested under proton: the boundary (proton) is redirected at snes9x's guest exe, and
    // snes9x's own arg (the ROM, guest-translated) trails the boundary's ARGS.
    void cross_namespace_composes_guest_target()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        cp.ExePathRelative = std::filesystem::path("roms/game.smc");
        //proton has NO explicit GUEST_PATH — the guest sees the game's content at its GUEST_ROOTS %GameDir%.
        RunnerLink Proton = mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%",
                                   {"waitforexitandrun", "%GameDir%\\%ContentPath:winpath%"});
        Proton.GameDirGuest = "C:\\%PackageUID%";
        cp.RunnerChain = { mkLink("snes9x", "win32", "snes9x.exe", "", {"%Content%"}), Proton, mkLink("native", "linux64", "%Content%") };
        QCOMPARE(LaunchResolver::BoundaryLinkIndex(cp), 1);          // proton owns the guest fs
        QVERIFY(LaunchResolver::ChainHasInnerLinks(cp));

        auto gt = LaunchResolver::ComposeGuestTarget(cp);
        QVERIFY(gt.CrossNamespace);
        QCOMPARE(gt.ContentRel, std::string("__runner_snes9x__/snes9x.exe"));   // boundary's %ContentPath% target
        QCOMPARE((int)gt.TrailingArgs.size(), 1);
        QCOMPARE(gt.TrailingArgs[0], std::string("C:\\game\\roms\\game.smc"));  // ROM, guest-translated (%GameDir% + backslashes)
    }

    // A Windows engine runner is written with %RunnerMount% (it must run directly on Windows too). Nested under proton,
    // %RunnerMount% is ITS build folder (__runner_<id>__): "" in the exe path, the folder's guest path in arguments —
    // never the boundary's host mount.
    void cross_namespace_runner_mount_is_the_inner_build()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "766";
        cp.RunnerMountPath = "/tmp/rt/RUNNER";
        RunnerLink Proton = mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%",
                                   {"waitforexitandrun", "%GameDir%\\%ContentPath:winpath%"});
        Proton.GameDirGuest = "C:\\%PackageUID%";
        cp.RunnerChain = { mkLink("gok", "win64", "%RunnerMount%/__gok/homeworld.exe", "", {"-data", "%RunnerMount%/x"}),
                           Proton, mkLink("native", "linux64", "%Content%") };
        auto gt = LaunchResolver::ComposeGuestTarget(cp);
        QCOMPARE(gt.ContentRel, std::string("__runner_gok__/__gok/homeworld.exe"));
        QCOMPARE((int)gt.TrailingArgs.size(), 2);
        QCOMPARE(gt.TrailingArgs[1], std::string("C:\\766\\__runner_gok__/x"));   // its own folder, as the guest sees it
        // Two inner links: the inner-most runner's exe is what the next one runs (its %Content%) — its own build folder
        // too, never the boundary's host mount.
        cp.RunnerChain = { mkLink("emu", "plat", "%RunnerMount%/emu.exe", "", {"%Content%"}),
                           mkLink("wrap", "win64", "%RunnerMount%/wrap.exe", "", {"%Content%"}),
                           Proton, mkLink("native", "linux64", "%Content%") };
        cp.ExePathRelative = std::filesystem::path("rom.bin");
        auto two = LaunchResolver::ComposeGuestTarget(cp);
        QCOMPARE(two.ContentRel, std::string("__runner_wrap__/wrap.exe"));
        QCOMPARE(two.TrailingArgs[0], std::string("C:\\766\\__runner_emu__\\emu.exe"));
    }

    // An inner runner's ENV reaches the process (one process tree): over the boundary's, the innermost winning, its
    // %RunnerMount% its own build — and under the game's own ENV, which wins over every runner.
    // Teeth: drop ApplyInnerEnv's loops and HW_Data never arrives; let an outer inner link overwrite and
    // SHARED reads "wrap"; skip GameSays and the game's GAME_WINS loses.
    void inner_runner_env_joins_the_process()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "766";
        cp.RunnerMountPath = "/tmp/rt/RUNNER";
        RunnerLink Proton = mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%",
                                   {"waitforexitandrun", "%GameDir%\\%ContentPath:winpath%"});
        Proton.GameDirGuest = "C:\\%PackageUID%";
        RunnerLink Emu = mkLink("emu", "plat", "%RunnerMount%/emu.exe", "", {"%Content%"});
        Emu.Env = { {"HW_Data", "%RunnerMount%\\data"}, {"SHARED", "emu"}, {"GAME_WINS", "emu"} };
        Emu.RemoveEnv = { "DROPPED" };
        RunnerLink Wrap = mkLink("wrap", "win64", "%RunnerMount%/wrap.exe", "", {"%Content%"});
        Wrap.Env = { {"SHARED", "wrap"}, {"DROPPED", "wrap"}, {"WRAP_ONLY", "1"} };
        cp.RunnerChain = { Emu, Wrap, Proton, mkLink("native", "linux64", "%Content%") };
        cp.ExePathRelative = std::filesystem::path("rom.bin");
        cp.RunnerEnv = { {"DROPPED", "boundary"}, {"GAME_WINS", "game"} };      // the game's merged over the boundary's
        cp.LaunchEnv = { {"GAME_WINS", "game"} };
        const auto gt = LaunchResolver::ComposeGuestTarget(cp);
        LaunchResolver::ApplyInnerEnv(cp, gt);
        QCOMPARE(cp.RunnerEnv.value("HW_Data", std::string()), std::string("C:\\766\\__runner_emu__\\data"));
        QCOMPARE(cp.RunnerEnv.value("SHARED", std::string()), std::string("emu"));      // the innermost wins
        QCOMPARE(cp.RunnerEnv.value("WRAP_ONLY", std::string()), std::string("1"));
        QCOMPARE(cp.RunnerEnv.value("GAME_WINS", std::string()), std::string("game"));  // the game wins over runners
        QVERIFY(!cp.RunnerEnv.contains("DROPPED"));                                      // an inner removal beats outer sets
        QVERIFY(std::find(cp.RunnerRemoveEnv.begin(), cp.RunnerRemoveEnv.end(), "DROPPED") != cp.RunnerRemoveEnv.end());
    }

    // Every classic chain ([content-runner, native]) has no inner links → ComposeGuestTarget is a no-op.
    void classic_chain_is_not_cross_namespace()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        cp.RunnerChain = {
            mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%", {}, "C:\\%PackageUID%\\%REL%"),
            mkLink("native", "linux64", "%Content%")
        };
        QCOMPARE(LaunchResolver::BoundaryLinkIndex(cp), 0);
        QVERIFY(!LaunchResolver::ChainHasInnerLinks(cp));
        QVERIFY(!LaunchResolver::ComposeGuestTarget(cp).CrossNamespace);
    }

    // GuestPath maps a CONTENT_ROOT-relative path into the boundary's namespace via its template; "" = identity.
    void guest_path_translation()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        // Wine-style template → REL separators become backslashes.
        QCOMPARE(LaunchResolver::GuestPath("C:\\%PackageUID%\\%REL%", "a/b.exe", cp), std::string("C:\\game\\a\\b.exe"));
        QCOMPARE(LaunchResolver::GuestPath(std::string(), "a/b.exe", cp), std::string("a/b.exe"));   // identity
    }

    // Without GUEST_ROOTS %GameDir% the boundary has no guest namespace to translate into: an inner path passes as is
    // (a runner is not guessed into having a C: drive from the shape of its CONTENT_ROOT).
    void guest_template_comes_from_gamedir_only()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        cp.ExePathRelative = std::filesystem::path("rom.sfc");
        RunnerLink Umu = mkLink("umu", "linux64", "umu-run", "drive_c/%PackageUID%", {"%Content%"});
        cp.RunnerChain = { mkLink("emu", "win32", "emu.exe", "", {"%Content%"}), Umu, mkLink("native", "linux64", "%Content%") };
        QCOMPARE(LaunchResolver::ComposeGuestTarget(cp).TrailingArgs[0], std::string("rom.sfc"));
        cp.RunnerChain[1].GameDirGuest = "D:\\%PackageUID%";
        QCOMPARE(LaunchResolver::ComposeGuestTarget(cp).TrailingArgs[0], std::string("D:\\game\\rom.sfc"));
    }

    // A package names places by anchor. A VALUE naming one reads the guest path (GUEST_ROOTS); a PATH the launch places
    // lands where that guest path's drive lives in the runner's layout (DRIVES) — the same anchor, two spellings.
    void anchors_resolve_to_guest_values_and_layout_paths()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802"; cp.PrefixRoot = "pfx";
        cp.GuestRoots = { {"%GameDir%", "C:\\%PackageUID%"}, {"%SysDir32%", "C:\\windows\\syswow64"},
                          {"%Media%", "E:\\"} };
        cp.Drives = { {"C:", "%PrefixRoot%/drive_c"}, {"E:", "%PrefixRoot%/media"} };
        const auto V = cp.GetVariablesMap();
        QCOMPARE(V.at("GameDir"), std::string("C:\\802"));                                    // a value: the guest path
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/data/x.cfg"), std::string("pfx/drive_c/802/data/x.cfg"));
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%SysDir32%/ir32_32.dll"), std::string("pfx/drive_c/windows/syswow64/ir32_32.dll"));
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%Media%/track.ogg"), std::string("pfx/media/track.ogg"));   // per drive
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "runner/own.sh"), std::string("runner/own.sh"));         // not a guest path
        cp.Drives = nlohmann::ordered_json::object();                                                          // no drives:
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/x"), std::string("C:\\802/x"));               // nothing maps
    }

    // A native runner's guest IS its runtime: an anchor spelled from %RuntimePath% is, in a value, the host path the
    // program sees, and a path the launch places lands at its path below the runtime — no drive involved. Teeth: drop
    // the runtime rule in GuestToLayout and the placement stays an absolute host path; move it after the drives and
    // the Windows-shaped runtime (a HOST drive letter) is mapped through the guest's C: instead.
    void a_native_guest_is_its_runtime()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802";
        cp.RuntimePath = "/tmp/rt/RUNTIME";
        cp.GuestRoots = { {"%GameDir%", "%RuntimePath%/n64/%PackageUID%"} };
        QCOMPARE(cp.GetVariablesMap().at("GameDir"), std::string("/tmp/rt/RUNTIME/n64/802"));          // a value
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/oot.o2r"), std::string("n64/802/oot.o2r")); // a placement
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%"), std::string("n64/802"));
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%RuntimePath%"), std::string(""));                  // the root
        QVERIFY(cp.UnmappedPlacements.empty());
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "/tmp/rt/RUNTIMEX/a"), std::string("/tmp/rt/RUNTIMEX/a"));   // not inside:
        QCOMPARE(cp.UnmappedPlacements, std::vector<std::string>{"/tmp/rt/RUNTIMEX/a"});                        // refused
        cp.UnmappedPlacements.clear();
        // A Windows runtime starts with the host's drive letter: it is the runtime, never a guest's C:.
        cp.RuntimePath = "C:\\vg\\RUNTIME";
        cp.Drives = { {"C:", "drive_c"} };
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/x.cfg"), std::string("n64/802/x.cfg"));
        QVERIFY(cp.UnmappedPlacements.empty());
    }

    // windows-native, the runner of every Windows game on a Windows PC, is a native guest: an anchor in a value (an
    // argument, an edited config line) names the runtime folder where that anchor's files are placed, never the
    // machine's own C:\<uid>, in backslashes as a Windows program expects; placements land where they always did
    // (drive_c/…). An inner engine nested under it is handed runtime paths too, its own build folder included.
    // Teeth: put the fake drive back into the builtin and every value reads C:\….
    // Windows mounts a runtime at a drive root ("W:\"): %RuntimePath% is spelled without its separator, so
    // "%RuntimePath%\drive_c" reads W:\drive_c, the path a game stores and compares, not W:\\drive_c. A folder is as it is.
    void runtime_at_a_drive_root_is_spelled_without_its_separator()
    {
        ContainerParams cp("/tmp/vg_bundle");
        for (const char *Root : {"W:\\", "W:/"})
        {
            cp.RuntimePath = Root; cp.UnifiedRuntime = true;
            const auto V = cp.GetVariablesMap();
            QCOMPARE(V.at("RuntimePath"), std::string("W:"));
            QCOMPARE(V.at("RunnerMount"), std::string("W:"));
        }
        cp.UnifiedRuntime = false; cp.RunnerMountPath = "V:\\";
        QCOMPARE(cp.GetVariablesMap().at("RunnerMount"), std::string("V:"));
        cp.RuntimePath = "/tmp/vg/RUNTIME/";
        QCOMPARE(cp.GetVariablesMap().at("RuntimePath"), std::filesystem::path("/tmp/vg/RUNTIME/").string());
    }

    void windows_native_names_the_runtime_on_a_windows_host()
    {
        nlohmann::ordered_json Exec;
        for (const auto & B : BuiltinRunners::All())
            if (B.Folder == "windows-native") Exec = B.Node["LAYERS"][0]["EXEC"]["windows-native_exec"];
        QVERIFY(Exec.contains("GUEST_ROOTS"));
        // Its box is its prefix (BoxPrefix): C: is laid out at drive_c, the profile and system anchors are the machine's
        // real folders (Platform::HostFolders, as %Host…% variables: given here, Linux has none).
        QCOMPARE(Exec["DRIVES"], nlohmann::ordered_json({{"C:", "drive_c"}}));
        QVERIFY2(NodeLower::CheckNode(BuiltinRunners::All()[0].Node, "windows-native").empty(), "windows-native fails CheckNode");
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802";
        cp.RuntimePath = "C:\\Users\\me\\VidyaGod\\TEMP\\802\\RUNTIME";
        cp.GuestRoots = Exec["GUEST_ROOTS"];
        cp.Drives = Exec["DRIVES"];
        cp.CustomVariables = {{"HostAppData", "C:\\Users\\me\\AppData\\Roaming"}, {"HostSysDir32", "C:\\Windows\\SysWOW64"},
                              {"HostDocuments", "C:\\Users\\me\\Documents"}};
        const auto V = cp.GetVariablesMap();
        QCOMPARE(V.at("GameDir"), std::string("C:\\Users\\me\\VidyaGod\\TEMP\\802\\RUNTIME\\drive_c\\802"));
        QCOMPARE(V.at("AppData"), std::string("C:\\Users\\me\\AppData\\Roaming"));   // the real folder: what the game asks Windows for
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/data/x.cfg"), std::string("drive_c/802/data/x.cfg"));
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%SysDir32%/d3d8.dll"), std::string("drive_c/Windows/SysWOW64/d3d8.dll"));
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%Documents%/My Games/x.ini"), std::string("drive_c/Users/me/Documents/My Games/x.ini"));
        QVERIFY(cp.UnmappedPlacements.empty());
        // An engine nested under it (a win64 DOSBox twin) runs the content at runtime paths, backslashed.
        cp.ExePathRelative = std::filesystem::path("GAME.EXE");
        RunnerLink Native = mkLink("windows-native", "win64", "%Content%", Exec["CONTENT_ROOT"].get<std::string>());
        Native.GameDirGuest = Exec["GUEST_ROOTS"]["%GameDir%"].get<std::string>();
        cp.RunnerChain = { mkLink("dosbox", "win64", "%RunnerMount%/dosbox.exe", "", {"-conf", "%RunnerMount%\\dosbox.conf", "%Content%"}), Native };
        const auto gt = LaunchResolver::ComposeGuestTarget(cp);
        const std::string Rt = "C:\\Users\\me\\VidyaGod\\TEMP\\802\\RUNTIME\\drive_c\\802";
        QCOMPARE(gt.ContentRel, std::string("__runner_dosbox__/dosbox.exe"));                     // mounted under CONTENT_ROOT
        QCOMPARE(gt.TrailingArgs, (std::vector<std::string>{"-conf", Rt + "\\__runner_dosbox__\\dosbox.conf", Rt + "\\GAME.EXE"}));
        // A runtime at a drive root keeps its separator: C:\ is the runtime, and everything on it is in it.
        cp.RuntimePath = "C:\\";
        QCOMPARE(LaunchResolver::GuestToLayout(cp, "%GameDir%/x.cfg"), std::string("drive_c/802/x.cfg"));
        QVERIFY(cp.UnmappedPlacements.empty());
    }

    // The same through a whole launch: a native engine without a fake drive places the game's files in its folder,
    // and %GameDir% in the game's arguments and in its EXE reads the real runtime path — what Linux runners spelled
    // by hand as %RuntimePath%/n64/%PackageUID%. Teeth: as above.
    void a_native_runner_places_and_names_the_game_folder_without_a_drive()
    {
        NodeIndex idx;
        idx.Nodes["soh"] = standalone(parse(json{ {"CID", "soh"}, {"LABEL", "soh"}, {"LAYERS", json::array({
            json{{"EXEC", json::object({ {"soh", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"n64"}},
                                             {"EXE", "%RunnerMount%/soh.elf"}, {"CONTENT_ROOT", "n64/%PackageUID%"},
                                             {"GUEST_ROOTS", {{"%GameDir%", "%RuntimePath%/n64/%PackageUID%"}}}}} })}} })} },
            "/tmp/vg_runner"));
        idx.Nodes["game"] = launchNode("game", "n64", {},
            json{{"EXE", "%GameDir%/oot.o2r"}, {"ARGS", json::array({"--home=%GameDir%"})}},
            json::array({ json{{"DIR", "saves"}, {"TARGET", "FILES/%GameDir%/Save"}},
                          json{{"KEEP", {{"FILES/%GameDir%/Save/", true}}}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(cp));
        const std::string Home = (cp.RuntimePath / "n64" / "game").string();
        std::string target;
        for (const auto & L : cp.SubComponentsArray)
            if (L.contains("TARGET") && L["TARGET"].is_string() && L["TARGET"].get<std::string>().find("Save") != std::string::npos)
                target = L["TARGET"].get<std::string>();
        QCOMPARE(target, std::string("n64/game/Save"));
        QCOMPARE(cp.ExeArgs, std::vector<std::string>{"--home=" + Home});
        QCOMPARE(cp.ExePathRelative.string(), std::string("oot.o2r"));
        QCOMPARE(cp.ExePathComplete.string(), Home + "/oot.o2r");
        // The kept folder is where the game writes it, below the runtime (saves are kept, not lost).
        bool kept = false;
        for (const auto & K : cp.KeepDirs) kept |= K.Path == "n64/game/Save";
        QVERIFY2(kept, nlohmann::ordered_json(cp.KeepDirs).dump().c_str());
    }

    // A program installed beside the game, not in it (MTA:SA in Program Files, run on the San Andreas it finds): its
    // %ContentPath% is still relative to the content root, climbing out of it, so the runner's "C:\<uid>\%ContentPath%"
    // names it. Measured from the runtime root instead, wine was asked for C:\790\pfx/drive_c/Program Files (x86)/….
    // Teeth: return the runtime-root path for an exe outside the content root.
    void a_program_beside_the_game_folder_is_named_from_it()
    {
        const json Wine = json{{"GUEST_ROOTS", {{"%GameDir%", "C:\\%PackageUID%"}, {"%ProgramFiles32%", "C:\\Program Files (x86)"}}},
                               {"DRIVES", {{"C:", "%PrefixRoot%/drive_c"}}}};
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {}, Wine);
        idx.Nodes["game"] = launchNode("game", "win32", {}, json{{"EXE", "%ProgramFiles32%/Tool 1.0/tool.exe"}},
                                       json::array({ json{{"DIR", "tool"}, {"TARGET", "FILES/%ProgramFiles32%/Tool 1.0"}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        QVERIFY(LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}}));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(cp));
        QCOMPARE(cp.ExePathRelative.generic_string(), std::string("../Program Files (x86)/Tool 1.0/tool.exe"));
        QCOMPARE(cp.ExePathComplete.generic_string(), (cp.RuntimePath / "pfx/drive_c/Program Files (x86)/Tool 1.0/tool.exe").generic_string());
        QCOMPARE(cp.WorkDirPathComplete.generic_string(), (cp.RuntimePath / "pfx/drive_c/Program Files (x86)/Tool 1.0").generic_string());
    }

    // A runner may name where the program it is starts (an emulator whose portable layout is its own folder: Supermodel
    // reads Assets/ and writes NVRAM/ beside itself), used when the game names none; the game's WORKDIR wins; and a
    // runner that names none leaves the default (the game EXE's folder). Teeth: ignore the runner's WORKDIR; let it win
    // over the game's; rebase a %RunnerMount% path into the runtime.
    void a_runners_workdir_applies_when_the_game_names_none()
    {
        const auto launch = [&](const json &RunnerExtra, const json &GameExtra) {
            NodeIndex idx;
            idx.Nodes["emubuild"] = standalone(parse(json{ {"CID", "emubuild"}, {"LABEL", "emubuild"},
                {"LAYERS", json::array({ json{{"ZIP", "emu.zip"}, {"TARGET", "FILES/__emu"}} })} }, "/tmp/vg_runner"));
            json Native = { {"CONTENT_ROOT", ""}, {"PREFIX_GENERATE", false}, {"EXE", "%RunnerMount%/__emu/emu"},
                            {"ARGS", json::array({ "%Content%" })} };
            for (const auto &[K, V] : RunnerExtra.items()) Native[K] = V;
            idx.Nodes["emu"] = runnerNode("emu", {"model3"}, {"emubuild"}, Native);
            idx.Nodes["game"] = launchNode("game", "model3", {}, GameExtra);
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            const bool Ok = LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}})
                         && LaunchResolver::ResolveExecutableDefinition(cp);
            return std::make_pair(Ok, cp);
        };
        {
            const auto [Ok, cp] = launch(json{{"WORKDIR", "%RunnerMount%/__emu"}}, json::object());
            QVERIFY(Ok);
            QVERIFY(!cp.RunnerMountPath.empty());
            QCOMPARE(cp.WorkDirPathComplete.generic_string(), (cp.RunnerMountPath / "__emu").generic_string());
        }
        {
            const auto [Ok, cp] = launch(json{{"WORKDIR", "%RunnerMount%/__emu"}}, json{{"WORKDIR", "%GameDir%"}});
            QVERIFY(Ok);
            QVERIFY2(cp.WorkDirPathComplete.generic_string().find("__emu") == std::string::npos, cp.WorkDirPathComplete.string().c_str());
        }
        {
            const auto [Ok, cp] = launch(json::object(), json::object());
            QVERIFY(Ok);
            QCOMPARE(cp.WorkDirPathComplete.generic_string(), cp.ExePathComplete.parent_path().generic_string());
        }
        {   // a place in the runtime, like a game's
            const auto [Ok, cp] = launch(json{{"WORKDIR", "%GameDir%/System"}}, json::object());
            QVERIFY(Ok);
            QVERIFY2(cp.WorkDirPathComplete.generic_string().rfind(cp.RuntimePath.generic_string(), 0) == 0
                     && cp.WorkDirPathComplete.filename() == "System", cp.WorkDirPathComplete.string().c_str());
        }
    }

    // A placement outside the runtime lands nowhere the program sees: under a native runner, an anchor spelled from a
    // host path (%TempPath%/x would mount at RUNTIME/tmp/…) or a literal drive with no DRIVES (a "C:" folder) is
    // refused, as an unmapped drive is under Wine — for a layer and for a kept path alike. Teeth: drop the
    // outside-the-runtime branch in GuestToLayout and these launch; check before DerivePersistence only and the
    // KEEP one launches.
    void a_native_placement_outside_the_runtime_is_refused()
    {
        const auto launch = [](const json &Roots, const json &Layers) {
            NodeIndex idx;
            idx.Nodes["emu"] = standalone(parse(json{ {"CID", "emu"}, {"LABEL", "emu"}, {"LAYERS", json::array({
                json{{"EXEC", json::object({ {"emu", {{"HOST", ManifestModel::MachinePlatform()}, {"GUEST", {"n64"}},
                                                 {"EXE", "%RunnerMount%/emu"}, {"CONTENT_ROOT", "n64/%PackageUID%"},
                                                 {"GUEST_ROOTS", Roots}}} })}} })} }, "/tmp/vg_runner"));
            idx.Nodes["game"] = launchNode("game", "n64", {}, json::object(), Layers);
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            return LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}});
        };
        const json Roots = { {"%GameDir%", "%RuntimePath%/n64/%PackageUID%"}, {"%Scratch%", "%TempPath%/scratch"} };
        QVERIFY2(launch(Roots, json::array({ json{{"DIR", "cfg"}, {"TARGET", "FILES/%GameDir%/cfg"}} })), "an in-runtime layer was refused");
        QVERIFY2(!launch(Roots, json::array({ json{{"DIR", "cfg"}, {"TARGET", "FILES/%Scratch%/cfg"}} })), "a host-path layer launched");
        QVERIFY2(!launch(Roots, json::array({ json{{"DIR", "cfg"}, {"TARGET", "FILES/C:/cfg"}} })), "a literal-drive layer launched");
        QVERIFY2(!launch(Roots, json::array({ json{{"KEEP", {{"FILES/%Scratch%/saves/", true}}}} })), "a host-path keep launched");
    }

    // A guest path is on a drive the runner lays out or spelled from a variable (%RuntimePath%, another anchor, a VAR).
    // A bare relative or host-absolute path is no place in any guest: refused. Teeth: drop the check and "n64/x" passes.
    void a_guest_root_must_be_on_a_drive_or_start_at_a_variable()
    {
        const auto check = [](const nlohmann::ordered_json &Roots, const nlohmann::ordered_json &Drives) {
            nlohmann::ordered_json E = { {"LABEL", "run"}, {"HOST", "linux64"}, {"GUEST", nlohmann::ordered_json::array({"custom"})},
                                         {"EXE", "%RunnerMount%/x"}, {"GUEST_ROOTS", Roots} };
            if (!Drives.is_null()) E["DRIVES"] = Drives;
            return NodeLower::CheckNode({ {"LABEL", "r"}, {"LAYERS", nlohmann::ordered_json::array({ { {"EXEC", NodeFixture::ExecOf({E})} } })} }, "r");
        };
        QVERIFY2(check({ {"%GameDir%", "%RuntimePath%/n64/%PackageUID%"} }, nullptr).empty(), "a runtime guest root was refused");
        QVERIFY(check({ {"%GameDir%", "C:\\%PackageUID%"} }, { {"C:", "pfx/drive_c"} }).empty());
        for (const char *Bad : {"n64/%PackageUID%", "/opt/game"})
        {
            const std::string Why = check({ {"%GameDir%", Bad} }, nullptr);
            QVERIFY2(Why.find("starts at %RuntimePath%") != std::string::npos, (std::string(Bad) + ": " + Why).c_str());
        }
    }

    // An inner runner nested under a native boundary sees the content at the boundary's runtime %GameDir%, joined with
    // '/' (a Wine boundary joins with '\'). Teeth: join every template with '\' and the ROM reads "…/n64/game\roms\…".
    void a_native_boundary_joins_guest_paths_with_slashes()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game"; cp.RuntimePath = "/tmp/rt/RUNTIME";
        cp.ExePathRelative = std::filesystem::path("roms/game.z64");
        RunnerLink Engine = mkLink("engine", "linux64", "%RunnerMount%/engine", "n64/%PackageUID%", {"%Content%"});
        Engine.GameDirGuest = "%RuntimePath%/n64/%PackageUID%";
        cp.RunnerChain = { mkLink("plugin", "linux64", "%RunnerMount%/plugin", "", {"%Content%"}), Engine, mkLink("native", "linux64", "%Content%") };
        const auto gt = LaunchResolver::ComposeGuestTarget(cp);
        QCOMPARE(gt.TrailingArgs[0], std::string("/tmp/rt/RUNTIME/n64/game/roms/game.z64"));
        // The separator follows the spelling, not one variable's name: a root spelled through another variable.
        cp.CustomVariables["Base"] = "/tmp/rt/RUNTIME";
        cp.RunnerChain[1].GameDirGuest = "%Base%/n64/%PackageUID%";
        QCOMPARE(LaunchResolver::ComposeGuestTarget(cp).TrailingArgs[0], std::string("/tmp/rt/RUNTIME/n64/game/roms/game.z64"));
    }

    // An anchor may be spelled from another, declared in any order.
    void anchors_resolve_through_each_other_in_any_order()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802";
        cp.GuestRoots = { {"%AppData%", "%UserProfile%\\AppData\\Roaming"}, {"%Saves%", "%AppData%\\G"},
                          {"%UserProfile%", "C:\\users\\me"} };
        const auto V = cp.GetVariablesMap();
        QCOMPARE(V.at("AppData"), std::string("C:\\users\\me\\AppData\\Roaming"));
        QCOMPARE(V.at("Saves"), std::string("C:\\users\\me\\AppData\\Roaming\\G"));
        // Anchors spelled from each other have no place: resolving must end AND stay small (repeated passes doubled
        // "%A%%A%" every pass — ≈25 GB at 30 anchors). Teeth: go back to re-substituting pass after pass and the
        // self-doubling value blows past the bound below.
        cp.GuestRoots = { {"%A%", "%B%\\a"}, {"%B%", "%A%\\b"} };
        QVERIFY(cp.GetVariablesMap().count("A"));
        nlohmann::ordered_json Many = { {"%A%", "%A%%A%"} };
        for (int i = 0; i < 30; ++i) Many["%P" + std::to_string(i) + "%"] = "C:\\p" + std::to_string(i);
        cp.GuestRoots = Many;
        const auto V2 = cp.GetVariablesMap();
        QVERIFY2(V2.at("A").size() < 64, "a self-referencing anchor must not grow");
    }

    // A runner whose anchors are spelled from each other is refused before any launch reads it (it is shared JSON).
    // Teeth: drop the circle check in CheckLayer and both nodes pass CheckNode.
    void a_runner_with_anchors_in_a_circle_is_refused()
    {
        for (const nlohmann::ordered_json &Roots : { nlohmann::ordered_json{ {"%A%", "%A%\\x"} },
                                                     nlohmann::ordered_json{ {"%A%", "%B%\\a"}, {"%B%", "%A%\\b"} } })
        {
            nlohmann::ordered_json L = { {"EXEC", nlohmann::ordered_json::object({ {"run", {{"HOST", "linux64"},
                {"GUEST", nlohmann::ordered_json::array({"win32"})}, {"EXE", "wine"}, {"GUEST_ROOTS", Roots},
                {"DRIVES", { {"C:", "pfx/drive_c"} }}}} })} };
            const std::string Why = NodeLower::CheckNode({ {"LABEL", "r"}, {"LAYERS", nlohmann::ordered_json::array({L})} }, "r");
            QVERIFY2(Why.find("in a circle") != std::string::npos, Why.c_str());
        }
        nlohmann::ordered_json Ok = { {"EXEC", nlohmann::ordered_json::object({ {"run", {{"HOST", "linux64"},
            {"GUEST", nlohmann::ordered_json::array({"win32"})}, {"EXE", "wine"},
            {"GUEST_ROOTS", { {"%AppData%", "%UserProfile%\\AppData"}, {"%UserProfile%", "C:\\users\\me"} }},
            {"DRIVES", { {"C:", "pfx/drive_c"} }}}} })} };
        const std::string OkWhy = NodeLower::CheckNode({ {"LABEL", "r"}, {"LAYERS", nlohmann::ordered_json::array({Ok})} }, "r");
        QVERIFY2(OkWhy.empty(), OkWhy.c_str());
    }

    // What an installer wrote (a captured registry delta) comes back spelled by anchor; the rest untouched.
    // A place spelled from a variable is known only at launch. A layer placed through an anchor that resolves onto a drive
    // the runner does not lay out is refused (its files would land in a literal "E:" folder); the same anchor only named
    // in an argument places nothing and launches. Teeth: drop the check after BuildSubComponentsArray and the E: layer
    // launches; refuse on any unmapped anchor, or count every placement, and the second launch is refused.
    void a_layer_placed_on_an_unmapped_drive_is_refused()
    {
        const json Wine = json{{"GUEST_ROOTS", {{"%GameDir%", "C:\\%PackageUID%"}, {"%Media%", "%MEDIA%"}}},
                               {"DRIVES", {{"C:", "%PrefixRoot%/drive_c"}}}};
        const json Media = json{{"VARS", {{"MEDIA", {{"DEFAULT", "E:\\"}}}}}};
        const auto launch = [&](const json &Entry, const json &Layers) {
            NodeIndex idx;
            idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {}, Wine);
            idx.Nodes["game"] = launchNode("game", "win32", {}, Entry, Layers);
            finish(idx);
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            return LaunchResolver::InitializeFromNode(cp, json{{"Settings", json::object()}});
        };
        QVERIFY2(!launch(json::object(), json::array({ Media, json{{"DIR", "music"}, {"TARGET", "FILES/%Media%/music"}} })),
                 "a layer placed on E:, which DRIVES does not lay out, launched");
        QVERIFY2(launch(json{{"ARGS", json::array({"-cd=%Media%"})}},
                        json::array({ Media, json{{"DIR", "cfg"}, {"TARGET", "FILES/%GameDir%/cfg"}} })),   // C: is laid out
                 "an anchor only named in an argument (beside a layer placed on a mapped drive) refused the launch");
    }

    void captured_registry_values_are_respelled_by_anchor()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802";
        cp.GuestRoots = { {"%GameDir%", "C:\\%PackageUID%"}, {"%ProgramFiles32%", "C:\\Program Files (x86)"} };
        const nlohmann::ordered_json D = LaunchResolver::AnchorRegEdits(cp, nlohmann::ordered_json::array({
            { {"REGPATH", "HKLM\\Software\\G"}, {"KEYVALUES", { {"InstallPath", "C:\\802"}, {"Codec", "C:\\Program Files (x86)\\LAV\\a.ax"},
                                                              {"Ver", "dword:00000001"} }} },
            { {"REGPATH", "HKLM\\Software\\KeyOnly"} } }));
        QCOMPARE(D[0]["KEYVALUES"]["InstallPath"].get<std::string>(), std::string("%GameDir%"));
        QCOMPARE(D[0]["KEYVALUES"]["Codec"].get<std::string>(), std::string("%ProgramFiles32%\\LAV\\a.ax"));
        QCOMPARE(D[0]["KEYVALUES"]["Ver"].get<std::string>(), std::string("dword:00000001"));
        QVERIFY(!D[1].contains("KEYVALUES"));
        // where each anchor lands, for the capture preview
        cp.PrefixRoot = "pfx"; cp.Drives = { {"C:", "%PrefixRoot%/drive_c"} };
        const auto L = LaunchResolver::AnchorLayouts(cp);
        QCOMPARE((int)L.size(), 2);
        QCOMPARE(L[0].second, std::string("pfx/drive_c/802"));
        QCOMPARE(L[1].second, std::string("pfx/drive_c/Program Files (x86)"));
    }

    // Typed strings and value names are captured too: a REG_EXPAND_SZ / REG_MULTI_SZ value is the raw .reg payload
    // (backslashes escaped: "C:\\\\Program Files"), and SharedDLLs / AppCompatFlags\\Layers are KEYED by the full path —
    // left as drive letters, the validator refused every such capture. At launch a token in a typed payload takes its
    // value escaped, so the round trip is exact. Teeth: drop the separator-run match in Fold::ToAnchors (typed values
    // stay as drives), the key re-spelling in AnchorRegEdits (the name stays), or the escaped map in
    // SubstituteRegEdit (the payload comes back with single backslashes).
    void typed_registry_strings_and_value_names_round_trip_through_anchors()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "802";
        cp.GuestRoots = { {"%GameDir%", "C:\\%PackageUID%"}, {"%ProgramFiles32%", "C:\\Program Files (x86)"} };
        const std::string Expand = R"(str(2):"C:\\Program Files (x86)\\LAV\\a.ax")";
        const std::string Multi  = R"(str(7):"C:\\802\0C:\\802\\x")";
        const nlohmann::ordered_json D = LaunchResolver::AnchorRegEdits(cp, nlohmann::ordered_json::array({
            { {"REGPATH", "HKLM\\Software\\G"}, {"KEYVALUES", { {"Expand", Expand}, {"Multi", Multi},
                                                              {"C:\\802\\g.exe", "~ RUNASADMIN"} }} } }));
        const auto &KV = D[0]["KEYVALUES"];
        QCOMPARE(KV["Expand"].get<std::string>(), std::string(R"(str(2):"%ProgramFiles32%\\LAV\\a.ax")"));
        QCOMPARE(KV["Multi"].get<std::string>(),  std::string(R"(str(7):"%GameDir%\0%GameDir%\\x")"));
        QVERIFY2(KV.contains("%GameDir%\\g.exe") && !KV.contains("C:\\802\\g.exe"), "the value NAME is re-spelled");

        nlohmann::ordered_json Edit = D[0];
        Edit["TYPE"] = "RegEdit";
        const auto Vars = cp.GetVariablesMap();
        const nlohmann::ordered_json Back = RegistryWrapper::SubstituteRegEdit(Edit, Vars);
        QCOMPARE(Back["KEYVALUES"]["Expand"].get<std::string>(), Expand);                // escaped exactly as captured
        QCOMPARE(Back["KEYVALUES"]["Multi"].get<std::string>(), Multi);
        QVERIFY2(Back["KEYVALUES"].contains("C:\\802\\g.exe"), "the value name comes back as the path");
    }

    // ResolveCustomVariables priority: CLI override > saved setting > DEFAULT — and a saved value counts only for a var
    // with a UI facet (a setting). UserPatch's Setup Terrain was an option, then became an EVAL bitfield over four
    // options; the instance's old saved "0" kept pinning it, so ticking "Disable weather" changed nothing. Teeth: honour
    // SavedVars regardless of the UI facet (DERIVED stays "0").
    void resolve_custom_variables_priority()
    {
        json derived = json{{"TYPE", "CustomVar"}, {"KEY", "DERIVED"}, {"DEFAULT", "%WEATHER%*4"}, {"EVAL", true}};
        const std::vector<OpGroup> pool = { OpGroup{"c1", json::array({
            json{{"TYPE", "CustomVar"}, {"KEY", "MYVAR"}, {"DEFAULT", "def"}, {"UI", {{"CONTROL", "text"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "WEATHER"}, {"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}}}},
            derived })} };

        // temp UserDataRoot isolates every read/write here from the real ~/.VidyaGod (the accessors now hit disk).
        QTemporaryDir ud; QVERIFY(ud.isValid());
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        // default (no instance config written yet → empty → the CustomVar DEFAULT)
        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("def")); }
        // user setting (now lives in the INSTANCE file — write it there)
        QVERIFY(InstanceStore::WriteConfig(cfg, "pkg", "DefaultInstance", json{{"VARIABLES", {{"MYVAR", "cfg"}, {"DERIVED", "0"}}}}));
        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("cfg"));
          QCOMPARE(cp.CustomVariables["DERIVED"], std::string("4")); }        // the stale saved "0" is not a setting
        // "%%" is a literal '%' through the whole resolution, undone once where the value is written: "%%WEATHER%%"
        // stays the text %WEATHER%, and "100%%%%" (meaning 100%%) is not undone a second time on insertion.
        // Teeth: no KeepEscapes in the fixpoint (100%%%% writes as 100%).
        { const std::vector<OpGroup> esc = { OpGroup{"c1", json::array({
              json{{"TYPE", "CustomVar"}, {"KEY", "WEATHER"}, {"DEFAULT", "1"}},
              json{{"TYPE", "CustomVar"}, {"KEY", "LIT"}, {"DEFAULT", "%%WEATHER%% is %WEATHER%, 100%%%%"}} })} };
          ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.Stack = esc; LaunchResolver::ResolveCustomVariables(cp, cfg);
          std::string Out = "%LIT%";
          VarSubst::StringVariableSubstitution(Out, cp.GetVariablesMap());
          QCOMPARE(Out, std::string("%WEATHER% is 1, 100%%")); }
        // CLI override wins
        { ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
          cp.VariableOverrides["MYVAR"] = "ovr";
          cp.Stack = pool; LaunchResolver::ResolveCustomVariables(cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("ovr")); }
    }

    // The prefix LAYOUT variables (%DefaultPfxDir%, %WineSys32Dir%, …) exist only for a prefix-generating runner
    // that ships a build. Defining them for anything else does not fix an undefined %token%, it HIDES one: a
    // layer under a native runner that references %DefaultPfxDir% mounts at that literal path and the game sees
    // none of its files. The gate lives in the function so its two callers — the launch and --audit-packages,
    // which derives them without ever mounting — cannot answer the question differently.
    void probe_prefix_layout_is_gated_on_a_prefix_generating_runner()
    {
        auto Vars = [](bool PrefixGen, bool ShipsBuild, const char *Mount) {
            ContainerParams CP("/tmp/vg_bundle");
            CP.PrefixGenerate   = PrefixGen;
            CP.RunnerShipsBuild = ShipsBuild;
            CP.RunnerMountPath  = Mount;
            LaunchResolver::ProbePrefixLayout(CP);
            return CP.CustomVariables;
        };
        QVERIFY2(Vars(false, true,  "/tmp/vg_runner").empty(), "a runner that generates no prefix defines none of them");
        QVERIFY2(Vars(true,  false, "/tmp/vg_runner").empty(), "nor one that ships no build");
        QVERIFY2(Vars(true,  true,  "").empty(),               "nor one with nothing mounted to probe");

        const auto Defined = Vars(true, true, "/tmp/vg_runner");
        QVERIFY(Defined.count("DefaultPfxDir"));
        QVERIFY(Defined.count("WineSys32Dir"));
        QVERIFY2(Defined.count("WineSysWow64Dir"), "and the full set when the runner really does generate one");
    }
};

QTEST_MAIN(LaunchResolverTest)
#include "test_launchresolver.moc"
