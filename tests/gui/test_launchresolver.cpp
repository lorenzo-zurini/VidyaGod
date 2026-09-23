// Tests for the launch-engine resolver (LaunchResolver) — the param/recipe/runner/persistence/exec resolution
// extracted from ContainerWrapper. Builds synthetic NodeIndex graphs in memory and asserts the resolved
// ContainerParams. Uses QApplication (DerivePaths reads the primary screen) under offscreen QPA.

#include <QtTest>

#include "launchresolver.h"
#include "launchparams.h"
#include "manifestmodel.h"
#include "nodefixture.h"
#include "instancestore.h"
#include <QTemporaryDir>

using json = nlohmann::ordered_json;

namespace {
// The machine platform the chain resolver bridges TO. Hardcoding "linux64" made every chain test assume a Linux
// host: on Windows MachinePlatform()=="win64", so a runner declared host "linux64" never reaches the machine, the
// chain resolves EMPTY, and the tests' ids[0] indexed out of bounds (a SEGFAULT under the Windows/MinGW CI). Use
// the real machine platform so these tests exercise the same win32->machine bridge on every OS.
const std::string kMachine = ManifestModel::MachinePlatform();
Node contentNode(const std::string & id, const json & layers = json::array())
{
    Node n; n.NodeId = id; n.Layers = layers; n.BundleDir = "/tmp/vg_bundle"; return n;
}
Node launchNode(const std::string & id, const std::string & host, const std::vector<std::string> & parents)
{
    Node n; n.NodeId = id; n.HasExec = true; n.HostPlatform = host; NodeFixture::Wire(n, parents);
    n.Meta = json{{"TITLE", id}};
    n.Exec = json{{"CONTENTPATH", "game.exe"}};
    n.Layers = json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "game"}} });
    n.BundleDir = "/tmp/vg_bundle"; return n;
}
Node runnerNode(const std::string & id, const std::vector<std::string> & guests,
                const std::vector<std::string> & parents = {})
{
    Node n; n.NodeId = id; n.HasRunner = true; n.GuestPlatform = guests; NodeFixture::Wire(n, parents);
    n.HostPlatform = ManifestModel::MachinePlatform();
    n.Exec = json{{"EXECUTABLE", "%RunnerMount%/proton"}, {"CONTENT_ROOT", "pfx/drive_c/%PackageUID%"},
                  {"PREFIX_GENERATE", true}, {"ARGS", json::array({"waitforexitandrun", "%Content%"})}};
    n.BundleDir = "/tmp/vg_runner"; return n;
}
bool recipeHas(const std::vector<std::string> & r, const std::string & needle)
{
    for (const auto & s : r) if (s.find(needle) != std::string::npos) return true;
    return false;
}
// A runner edge GUEST→HOST for daisy-chain tests. A '%'-bearing or empty EXECUTABLE is always "available"
// (ExecutableAvailable), so these resolve without a real binary on PATH.
Node chainRunner(const std::string & id, const std::vector<std::string> & guests, const std::string & host,
                 const std::string & exec = "%RunnerMount%/run")
{
    Node n; n.NodeId = id; n.HasRunner = true; n.GuestPlatform = guests; n.HostPlatform = host;
    n.Exec = json{{"EXECUTABLE", exec}};
    n.BundleDir = "/tmp/vg_runner"; return n;
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
    // ---- engine-injected session vars (friend LAN / presence) ----
    // These are the reason SessionVars exists at all: the LAN vars used to be merged AFTER this resolve and after
    // EXEARGS substitution, so %VIDYAGOD_*% survived into argv as a literal. And because only DECLARED keys reach
    // the fixpoint, an undeclared key handed in as an override was silently dropped.
    void session_vars_reach_exeargs_and_custom_var_defaults()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win32"});
        Node g = launchNode("game", "win32", {});
        g.Layers = json::array({ json{{"TYPE", "CustomVar"}, {"KEY", "join"}, {"DEFAULT", "%VIDYAGOD_JOIN_ADDRESS%"}} });
        g.Exec = json{{"CONTENTPATH", "game.exe"},
                      {"EXEARGS", json::array({"--player", "%VIDYAGOD_SELF_NAME%", "--address=%join%"})}};
        idx.Nodes["game"] = g;

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars = { {"VIDYAGOD_JOIN_ADDRESS", "10.66.1.2"}, {"VIDYAGOD_SELF_NAME", "Lorenzo"} };
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(json::object(), cp));

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
        Node g = launchNode("game", "win32", {});
        g.Exec = json{{"CONTENTPATH", "game.exe"},
                      {"EXEARGS", json::array({"--address=%VIDYAGOD_JOIN_ADDRESS%", "%VIDYAGOD_JOIN_ADDRESS%", "--wait"})}};
        idx.Nodes["game"] = g;

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars = { {"VIDYAGOD_JOIN_ADDRESS", ""} };
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
        QVERIFY(LaunchResolver::ResolveExecutableDefinition(json::object(), cp));

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
        Node g = launchNode("game", "win32", {});
        g.Layers = json::array({ json{{"TYPE", "CustomVar"}, {"KEY", "VIDYAGOD_SELF_NAME"}, {"DEFAULT", "PackageChoice"}} });
        idx.Nodes["game"] = g;

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.SessionVars       = { {"VIDYAGOD_SELF_NAME", "FromLan"}, {"VIDYAGOD_JOIN_ADDRESS", "10.66.9.9"} };
        cp.VariableOverrides = { {"VIDYAGOD_JOIN_ADDRESS", "1.2.3.4"} };
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));

        QCOMPARE(cp.CustomVariables["VIDYAGOD_JOIN_ADDRESS"], std::string("1.2.3.4"));      // override wins
        QCOMPARE(cp.CustomVariables["VIDYAGOD_SELF_NAME"],    std::string("PackageChoice")); // declaration wins
    }

    // InitializeFromNode resolves the whole container from the node graph: runner pick, recipe, content-root.
    void initialize_from_node_resolves_runner_and_recipe()
    {
        NodeIndex idx;
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});   // build ships via parent content node
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"TYPE", "VFSZipLayer"}, {"PATH", "proton.zip"}} }));
        idx.Nodes["base"]   = contentNode("base", json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "base"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {"base"});

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx;
        cp.LaunchNodeId = "game";
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};

        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));

        QCOMPARE(cp.RunnerID, std::string("wine"));
        QVERIFY(cp.PrefixGenerate);
        QVERIFY(cp.RunnerShipsBuild);                          // runner has VFS build layers
        QCOMPARE(cp.ContentRoot, std::string("pfx/drive_c/game"));   // %PackageUID% substituted by DerivePaths
        QVERIFY(recipeHas(cp.Recipe, "base"));                 // content parent in the recipe
        QVERIFY(recipeHas(cp.Recipe, "game"));                 // launch node's own layers
        QVERIFY(pool.contains("COMPONENTS") && !pool["COMPONENTS"].empty());
    }

    // A library pinned by the RUNNER contributes its order-independent layers to the game runtime, exactly as a
    // library pinned by the GAME does — the runner chain is equivalent to the content chain in capability for
    // DllOverride/RegEdit/FileEdit. (It is NOT equivalent for VFS: a runner parent's VFS builds the runner tree.)
    void runner_closure_contributes_overrides_and_edits()
    {
        NodeIndex idx;
        idx.Nodes["mediastack"] = contentNode("mediastack", json::array({
            json{{"TYPE", "DllOverride"}, {"DLLOVERRIDE", "winegstreamer="}},
            json{{"TYPE", "RegEdit"}, {"REGPATH", "HKLM\\Software\\Lav"}},
            json{{"TYPE", "VFSZipLayer"}, {"PATH", "codecs.zip"}, {"TARGET", "files/lib/gstreamer-1.0"}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"mediastack"});
        idx.Nodes["game"] = launchNode("game", "win32", {});

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));

        int overrides = 0, regedits = 0, prefixVfs = 0;
        for (const auto & L : cp.SubComponentsArray)
        {
            const std::string T = L.value("TYPE", std::string());
            if (T == "DllOverride" && L.value("DLLOVERRIDE", std::string()) == "winegstreamer=") ++overrides;
            else if (T == "RegEdit" && L.value("REGPATH", std::string()) == "HKLM\\Software\\Lav") ++regedits;
            else if (T == "VFSZipLayer" && L.value("PATH", std::string()) == "codecs.zip") ++prefixVfs;
        }
        QCOMPARE(overrides, 1);    // reaches WINEDLLOVERRIDES (FileEdits::ProcessDLLOverrides reads this array)
        QCOMPARE(regedits, 1);     // applied to the game prefix
        QCOMPARE(prefixVfs, 0);    // runner-tree build layer stays in the runner mount, not the prefix
    }

    // A runner-closure node the user switched ON must contribute its prefix-assembly layers. The walk that
    // collects them used to derive its own toggle map by first walking with DEFAULTS - which never visits an
    // off-by-default node, so the node got no entry and was gated off again. The toggle could only ever move a
    // node OFF; turning one ON silently did nothing.
    void runner_optional_node_toggled_on_contributes_prefix_layers()
    {
        auto build = [](const std::map<std::string, bool> & states) {
            NodeIndex idx;
            // Prefix-ASSEMBLY layer: runtime-sourced PATH, so it assembles the game prefix rather than the
            // runner tree - which is exactly the set the toggled walk collects.
            Node extra = contentNode("wine_extra_dlls", json::array({
                json{{"TYPE", "VFSDirLayer"}, {"PATH", "%RunnerMount%/extra"}, {"TARGET", "pfx/drive_c/extra"}} }));
            extra.Optional = true;
            extra.Default  = false;                       // OFF unless the user says otherwise
            idx.Nodes["wine_extra_dlls"] = extra;
            idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"wine_extra_dlls"});
            idx.Nodes["game"] = launchNode("game", "win32", {});

            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.ModuleStates = states;
            json pool = json::object();
            LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}});
            int n = 0;
            for (const auto & L : cp.SubComponentsArray)
                if (L.value("PATH", std::string()) == "%RunnerMount%/extra") ++n;
            return n;
        };

        QCOMPARE(build({}), 0);                                        // off by default
        QCOMPARE(build({{"wine_extra_dlls", false}}), 0);               // explicitly off
        QCOMPARE(build({{"wine_extra_dlls", true}}), 1);                // explicitly ON - the regression
    }

    // No qualifying runner (guest platform mismatch) → no runner picked.
    void initialize_from_node_no_matching_runner()
    {
        NodeIndex idx;
        idx.Nodes["wine"] = runnerNode("wine", {"win64"});     // serves win64
        idx.Nodes["game"] = launchNode("game", "win32", {});   // needs win32
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        json pool = json::object();
        // No runner serves win32 → resolution FAILS (must not "succeed" into an empty-runner launch).
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerID.empty());
    }

    // AuthoringBare: a platformless, content-less node resolves with NO runner and CONTENT_ROOT="" (the authoring
    // workbench). Resolution must succeed where a normal launch would fail (no platform → no chain).
    void initialize_from_node_authoring_bare()
    {
        NodeIndex idx;
        Node n; n.NodeId = "fresh"; n.BundleDir = "/tmp/vg_bundle";   // no Declare* layers → content, no platform
        idx.Nodes["fresh"] = n;

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "fresh";
        cp.AuthoringBare = true;
        json pool = json::object();

        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerChain.empty());                      // no runner resolved
        QVERIFY(cp.RunnerID.empty());
        QVERIFY(cp.ContentRoot.empty());                      // content at the runtime root
        QVERIFY(!cp.PrefixGenerate);                          // no wine prefix
        QVERIFY(pool.contains("COMPONENTS"));                 // a valid (empty) component pool
    }

    // A node WITH content but no platform still resolves bare (content overlay, no runner).
    void initialize_from_node_authoring_bare_with_content()
    {
        NodeIndex idx;
        idx.Nodes["c"] = contentNode("c", json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "files"}} }));
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "c"; cp.AuthoringBare = true;
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerChain.empty());
        QVERIFY(recipeHas(cp.Recipe, "c"));                   // the node's own content layer is in the recipe
    }

    // PickRunnerNode honours an explicit RunnerID pin over the first-qualifying default.
    void pick_runner_honours_pin()
    {
        NodeIndex idx;
        idx.Nodes["wineA"] = runnerNode("wineA", {"win32"});
        idx.Nodes["wineB"] = runnerNode("wineB", {"win32"});
        Node launch = launchNode("game", "win32", {});
        const json cfg = json{{"Settings", json::object()}};

        ContainerParams cp("/tmp/vg_bundle");
        const Node * def = LaunchResolver::PickRunnerNode(idx, launch, cp, cfg);
        QVERIFY(def != nullptr);   // some runner qualifies (map order → wineA)

        cp.RunnerID = "wineB";
        const Node * pinned = LaunchResolver::PickRunnerNode(idx, launch, cp, cfg);
        QVERIFY(pinned != nullptr);
        QCOMPARE(pinned->NodeId, std::string("wineB"));
    }

    // The default pick prefers a RECOMMENDED runner over the alphabetically-first one (no arbitrary default).
    void pick_runner_prefers_recommended()
    {
        NodeIndex idx;
        idx.Nodes["aaa_wine"] = runnerNode("aaa_wine", {"win32"});   // sorts first
        Node rec = runnerNode("zzz_wine", {"win32"}); rec.Recommended = true;
        idx.Nodes["zzz_wine"] = rec;
        Node launch = launchNode("game", "win32", {});
        const json cfg = json{{"Settings", json::object()}};

        ContainerParams cp("/tmp/vg_bundle");
        const Node * def = LaunchResolver::PickRunnerNode(idx, launch, cp, cfg);
        QVERIFY(def != nullptr);
        QCOMPARE(def->NodeId, std::string("zzz_wine"));   // recommended beats alphabetical
    }

    // Between equally-(non-)recommended runners, one shipped in the launch node's own package wins (embedded > global).
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
        const Node * def = LaunchResolver::PickRunnerNode(idx, launch, cp, cfg);
        QVERIFY(def != nullptr);
        QCOMPARE(def->NodeId, std::string("zzz_wine"));   // package-local beats alphabetical global
    }

    // DerivePersistence (unified Persist primitive): pristine by default, KEEP target classification by shape, and a
    // DROP path.
    void derive_persistence_unified_persist()
    {
        // No DeclarePersist anywhere → pristine, every keep-set empty.
        ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"};
        json poolNone = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array()}} })}};
        LaunchResolver::DerivePersistence(poolNone, cp);
        QVERIFY(cp.KeepDirs.empty() && cp.KeepFiles.empty() && cp.KeepRegHives.empty() && cp.KeepRegKeys.empty());

        // DeclarePersist: SCOPE=file PATH shape decides dir (no extension) vs file (extension); TARGET defaults to
        // PATH's leaf; CLOUD default true. SCOPE=registry PATH=key → subtree, PATH="" → all hives (authoring).
        ContainerParams cp2("/tmp/vg_bundle"); cp2.Recipe = {"c1"};
        json poolKeep = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/saves"},{"CLOUD",false}},              // dir; TARGET→"saves"; local-only
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/Game/config.ini"},{"TARGET","GameConfig"}}, // file, explicit target
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKCU\\Software\\id Software\\Quake"}},      // subtree
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH",""}} })}} })}};                             // all hives (authoring)
        LaunchResolver::DerivePersistence(poolKeep, cp2);
        QCOMPARE((int)cp2.KeepDirs.size(), 1);
        QCOMPARE(cp2.KeepDirs[0].Path, std::string("drive_c/saves"));
        QCOMPARE(cp2.KeepDirs[0].Target, std::string("saves"));     // defaulted to the last PATH component
        QVERIFY(!cp2.KeepDirs[0].Cloud);                            // CLOUD:false honoured
        QCOMPARE((int)cp2.KeepFiles.size(), 1);
        QCOMPARE(cp2.KeepFiles[0].Path, std::string("drive_c/Game/config.ini"));
        QCOMPARE(cp2.KeepFiles[0].Target, std::string("GameConfig"));
        QCOMPARE((int)cp2.KeepRegKeys.size(), 1);
        QCOMPARE((int)cp2.KeepRegHives.size(), 3);                  // registry PATH "" → all three hives
    }

    // Every file/dir persist lands at UserDataPath/<TARGET>. Two guards keep that namespace safe from SILENT SAVE
    // LOSS: (a) a TARGET colliding with an earlier one is refused (else the second capture clobbers the first, or two
    // dir persists mount the same durable dir RW and corrupt it); (b) a TARGET naming the instance's OWN reserved
    // state (instance.json / REGISTRY / REGKEYS) is refused (else a persist would seed the launcher's secrets into a
    // game-visible mount, or overwrite the instance config). Both are case-insensitive — durable dirs travel.
    void derive_persistence_refuses_colliding_and_reserved_targets()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"};
        json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/save"},{"TARGET","Save"}},          // kept
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","mods/save"},{"TARGET","Save"}},          // dup → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","other/save"},{"TARGET","save"}},         // dup (case) → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","cfg.ini"},{"TARGET","instance.json"}},   // reserved → skip
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","hive"},{"TARGET","REGISTRY"}} })}} })}};  // reserved → skip
        LaunchResolver::DerivePersistence(pool, cp);
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
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","pfx/drive_c/users"}},
            json{{"TYPE","DeclarePersist"},{"SCOPE","registry"},{"PATH","HKCU"}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"proton_keepset"});
        idx.Nodes["game"] = launchNode("game", "win32", {});

        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        LaunchResolver::DerivePersistence(pool, cp);

        QVERIFY2(!cp.KeepDirs.empty(), "the runner's keep-set never reached persistence - saves are lost");
        QVERIFY(hasDir(cp, "pfx/drive_c/users"));
        QVERIFY(!cp.KeepRegHives.empty() || !cp.KeepRegKeys.empty());          // HKCU

        // ...and in a CHAIN, every runner's keep-set counts, not just the boundary's: an emulator nested
        // under proton has user-state of its own, and taking only the outermost link drops it silently.
        NodeIndex ch;
        ch.Nodes["emu_keep"] = contentNode("emu_keep", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/emu_state"}} }));
        ch.Nodes["emu"]      = chainRunner("emu", {"vortex"}, "win32");
        NodeFixture::Wire(ch.Nodes["emu"], {"emu_keep"});
        ch.Nodes["proton"]   = chainRunner("proton", {"win32"}, kMachine);
        NodeFixture::Wire(ch.Nodes["proton"], {"proton_keep"});
        ch.Nodes["proton_keep"] = contentNode("proton_keep", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","pfx/drive_c/users"}} }));
        ch.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        ch.Nodes["vgame"]     = launchNode("vgame", "vortex", {});

        ContainerParams cp2("/tmp/vg_bundle");
        cp2.NodeIdx = &ch; cp2.LaunchNodeId = "vgame";
        json pool2 = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp2, pool2, json{{"Settings", json::object()}}));
        LaunchResolver::DerivePersistence(pool2, cp2);
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
                json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","drive_c/Saves"},{"WHEN", when}} }));
            idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"keep"});
            idx.Nodes["game"] = launchNode("game", "win32", {});
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
            json pool = json::object();
            LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}});
            LaunchResolver::DerivePersistence(pool, cp);
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
        ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"game"};
        cp.RuntimePath = "/tmp/rt";
        cp.RunnerPersistLayers = json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", "pfx/drive_c/users"}},
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "registry"}, {"PATH", "HKCU"}} });
        json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "game"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", ""}, {"TARGET", "AllData"}} })}} })}};
        LaunchResolver::DerivePersistence(pool, cp);
        QVERIFY(hasDir(cp, ""));                                   // PATH "" ⇒ whole-runtime persist recorded
        QVERIFY(hasDir(cp, "pfx/drive_c/users"));                 // runner's user-profile keep still recorded
        QCOMPARE((int)cp.KeepDirs.size(), 2);
        QVERIFY(!cp.KeepRegKeys.empty());                          // HKCU ⇒ granular registry keep

        // Runner keep-set alone (no game persist) → pristine runtime with the runner's user-profile + HKCU kept.
        ContainerParams cp2("/tmp/vg_bundle"); cp2.Recipe = {"game"};
        cp2.RunnerPersistLayers = json::array({
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "file"}, {"PATH", "drive_c/users"}},
            json{{"TYPE", "DeclarePersist"}, {"SCOPE", "registry"}, {"PATH", "HKCU"}} });
        json pool2 = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "game"}, {"SUBCOMPONENTS", json::array()}} })}};
        LaunchResolver::DerivePersistence(pool2, cp2);
        QCOMPARE((int)cp2.KeepDirs.size(), 1);                     // …the user profile
        QVERIFY(!cp2.KeepRegKeys.empty());                         // …and HKCU
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

    // The chain bridges the SELECTED entrypoint's platform, never the node's default view: a node carrying a
    // native entry (the default) and a win32 entry routes the win32 one through proton — and the win32 entry's
    // own RUNNER is honoured as its soft pin.
    void chain_follows_the_selected_entrypoint()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["protonB"] = chainRunner("protonB", {"win32"}, kMachine);
        Node launch = launchNode("game", kMachine, {});
        launch.Entrypoints = json::array({
            json{{"LABEL", "Native"}, {"HOST", kMachine}, {"PATH", "game"}},
            json{{"LABEL", "Windows"}, {"HOST", "win32"}, {"PATH", "game.exe"}, {"RUNNER", "protonB"}} });
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto def = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)def.size(), 1);                                          // native: terminal only
        cp.Entrypoint = "Windows";
        auto win = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)win.size(), 2);
        QCOMPARE(win[0], std::string("protonB"));                              // the ENTRY's declared runner, not BFS's protonA
        cp.Entrypoint = "Native";
        QCOMPARE((int)LaunchResolver::ResolveChainIds(idx, launch, cp, cfg).size(), 1);
    }

    // A runner's build is its closure, ITSELF INCLUDED — the same rule as a game's mount, no runner special case.
    // A runner whose build lives ON the runner node (ENTRYPOINTS + LAYERS in one node — the java runners after the
    // one-edge fold) ships that build: it is available with a bare EXECUTABLE, and its link mounts the layer.
    // Before this, every runner-build walk skipped the runner node itself, so Minecraft's JRE was never mounted
    // (execvp of /__jre/bin/java → exit 127) while proton, whose build is a chain it is OVER, kept working.
    void runner_build_on_the_runner_node_itself_ships()
    {
        NodeIndex idx;
        Node java = chainRunner("java8", {"java_8"}, kMachine, "%RunnerMount%/__jre/bin/java");
        java.Layers = json::array({ json{{"TYPE", "VFSZipLayer"}, {"PATH", "jre_8.zip"}, {"TARGET", "__jre"}} });
        idx.Nodes["java8"] = java;
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
        // What the runner is OVER is part of its build too — whatever kind of node it is (no special case).
        Node lib = chainRunner("lib", {"snes"}, "win32", "snes9x.exe");
        lib.Layers = json::array({ json{{"TYPE", "VFSZipLayer"}, {"PATH", "lib.zip"}} });
        idx.Nodes["lib"] = lib;
        NodeFixture::Wire(idx.Nodes["java8"], {"lib"});
        auto chain2 = LaunchResolver::ResolveRunnerChain(idx, launch, cp, cfg);
        QCOMPARE((int)chain2[0].Layers.size(), 2);
        QCOMPARE(chain2[0].Layers.back().value("TARGET", std::string()), std::string("__jre"));   // own layer last = on top
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

    // BFS tie-break: a RECOMMENDED bridge runner beats the alphabetically-first one.
    void chain_bridge_prefers_recommended()
    {
        NodeIndex idx;
        idx.Nodes["aaa_proton"] = chainRunner("aaa_proton", {"win32"}, kMachine);
        Node rec = chainRunner("zzz_proton", {"win32"}, kMachine); rec.Recommended = true;
        idx.Nodes["zzz_proton"] = rec;
        idx.Nodes["nativerun"]  = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE(ids[0], std::string("zzz_proton"));
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

    // A launchable's DECLARED runner (DeclareExec.RUNNER → Node.RecommendedRunner) is honoured with NO persisted
    // pin — the job the removed appmodel PREFERRED_RUNNER seed used to do, now at the resolver so a FRESH game gets
    // it. Teeth: drop the RecommendedRunner soft-pin in ResolveChainIds → the BFS default picks protonA (sorts
    // first, same node-Recommended flag) and this fails.
    void chain_honours_declared_runner()
    {
        NodeIndex idx;
        idx.Nodes["protonA"]   = chainRunner("protonA", {"win32"}, kMachine);   // sorts first by node-id
        idx.Nodes["protonB"]   = chainRunner("protonB", {"win32"}, kMachine);
        idx.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        Node launch = launchNode("game", "win32", {});
        launch.RecommendedRunner = "protonB";                  // the DeclareExec.RUNNER the package declares
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
        QTemporaryDir ud; QVERIFY(ud.isValid());               // no pin persisted (temp root, empty instance)
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("protonB"));              // declared runner beats the default
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
        const json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE", "CustomVar"}, {"KEY", "OPT"},    {"DEFAULT", "7"}, {"UI", {{"CONTROL", "int"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "BIND"},   {"DEFAULT", "1"}},
            json{{"TYPE", "CustomVar"}, {"KEY", "SECRET"}, {"UI", {{"CONTROL", "secret"}, {"POOL", json::array({"A","B","C"})}}}} })}} })}};

        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["OPT"],  std::string("7"));   // RAW — NOT dword-encoded at resolution
          QCOMPARE(cp.CustomVariables["BIND"], std::string("1"));   // a no-UI binding
          const std::string s = cp.CustomVariables["SECRET"];
          QVERIFY(s == "A" || s == "B" || s == "C"); }              // picked from the pool

        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          cp.VariableOverrides["SECRET"] = "X";                      // CLI/--var beats the pool
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["SECRET"], std::string("X")); }
    }

    // Absolute scope: a var's hierarchy-final value is visible to every reference, regardless of declaration order
    // (forward references, chains, and post-override values all resolve). Reference cycles terminate safely.
    void resolve_custom_var_absolute_scope()
    {
        auto cv = [](const std::string& k, const std::string& d){ return json{{"TYPE","CustomVar"},{"KEY",k},{"DEFAULT",d}}; };

        // Forward reference: A (declared first) references B (declared later).
        { json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID","c1"}, {"SUBCOMPONENTS", json::array({
              cv("A","%B%"), cv("B","x") })}} })}};
          ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["A"], std::string("x")); }

        // Chain A->B->C resolves fully.
        { json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID","c1"}, {"SUBCOMPONENTS", json::array({
              cv("A","%B%"), cv("B","%C%"), cv("C","z") })}} })}};
          ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["A"], std::string("z"));
          QCOMPARE(cp.CustomVariables["B"], std::string("z")); }

        // Hierarchy override is globally visible: a later component re-declares B; A sees the FINAL B.
        { json pool = json{{"COMPONENTS", json::array({
              json{{"COMPONENTID","parent"}, {"SUBCOMPONENTS", json::array({ cv("B","x"), cv("A","%B%") })}},
              json{{"COMPONENTID","child"},  {"SUBCOMPONENTS", json::array({ cv("B","y") })}} })}};
          ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"parent","child"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["B"], std::string("y"));     // child wins
          QCOMPARE(cp.CustomVariables["A"], std::string("y")); }   // and A sees the final value, not "x"

        // A reference cycle terminates (no hang); the residual token is left literal.
        { json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID","c1"}, {"SUBCOMPONENTS", json::array({
              cv("A","%B%"), cv("B","%A%") })}} })}};
          ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QVERIFY(cp.CustomVariables["A"].find('%') != std::string::npos); }  // unresolved, but resolution returned
    }

    // ---- Cross-namespace nesting (ComposeGuestTarget / BoundaryLinkIndex / GuestPath) ----

    // A win32 emulator (snes9x) nested under proton: the boundary (proton) is redirected at snes9x's guest exe, and
    // snes9x's own arg (the ROM, guest-translated) trails the boundary's ARGS.
    void cross_namespace_composes_guest_target()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        cp.ExePathRelative = std::filesystem::path("roms/game.smc");
        //proton has NO explicit GUEST_PATH — the wine-drive template is DERIVED from CONTENT_ROOT (drive_c → C:\).
        cp.RunnerChain = {
            mkLink("snes9x", "win32", "snes9x.exe", "", {"%Content%"}),
            mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%",
                   {"waitforexitandrun", "C:\\%PackageUID%\\%ContentPath%"}),
            mkLink("native", "linux64", "%Content%")
        };
        QCOMPARE(LaunchResolver::BoundaryLinkIndex(cp), 1);          // proton owns the guest fs
        QVERIFY(LaunchResolver::ChainHasInnerLinks(cp));

        auto gt = LaunchResolver::ComposeGuestTarget(cp);
        QVERIFY(gt.CrossNamespace);
        QCOMPARE(gt.ContentRel, std::string("__runner_snes9x__/snes9x.exe"));   // boundary's %ContentPath% target
        QCOMPARE((int)gt.TrailingArgs.size(), 1);
        QCOMPARE(gt.TrailingArgs[0], std::string("C:\\game\\roms\\game.smc"));  // ROM, guest-translated (derived C:\ + backslashes)
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

    // The wine-drive guest template is derived from CONTENT_ROOT when a boundary declares no explicit GUEST_PATH.
    void derived_guest_template_from_content_root()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "game";
        cp.ExePathRelative = std::filesystem::path("rom.sfc");
        cp.RunnerChain = {
            mkLink("emu", "win32", "emu.exe", "", {"%Content%"}),
            mkLink("umu", "linux64", "umu-run", "drive_c/%PackageUID%", {"C:\\%PackageUID%\\%ContentPath%"}),
            mkLink("native", "linux64", "%Content%")
        };
        auto gt = LaunchResolver::ComposeGuestTarget(cp);
        QVERIFY(gt.CrossNamespace);
        QCOMPARE(gt.TrailingArgs[0], std::string("C:\\game\\rom.sfc"));         // derived from "drive_c/%PackageUID%"
    }

    // ResolveCustomVariables priority: CLI override > USERSETTINGS > DEFAULT.
    void resolve_custom_variables_priority()
    {
        json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE", "CustomVar"}, {"KEY", "MYVAR"}, {"DEFAULT", "def"}, {"VARTYPE", "string"}} })}} })}};

        // temp UserDataRoot isolates every read/write here from the real ~/.VidyaGod (the accessors now hit disk).
        QTemporaryDir ud; QVERIFY(ud.isValid());
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        // default (no instance config written yet → empty → the CustomVar DEFAULT)
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("def")); }
        // user setting (now lives in the INSTANCE file — write it there)
        QVERIFY(InstanceStore::WriteConfig(cfg, "pkg", "DefaultInstance", json{{"VARIABLES", {{"MYVAR", "cfg"}}}}));
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("cfg")); }
        // CLI override wins
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          cp.VariableOverrides["MYVAR"] = "ovr";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
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
