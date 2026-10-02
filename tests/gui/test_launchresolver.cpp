// Tests for the launch-engine resolver (LaunchResolver) — the param/recipe/runner/persistence/exec resolution
// extracted from ContainerWrapper. Builds synthetic NodeIndex graphs in memory and asserts the resolved
// ContainerParams. Uses QApplication (DerivePaths reads the primary screen) under offscreen QPA.

#include <QtTest>

#include "launchresolver.h"
#include "nodelower.h"
#include "registrywrapper.h"
#include "packagecatalog.h"
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
    L.push_back({ {"EXEC", json::array({ E })} });
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
    L.push_back({ {"EXEC", json::array({ E })} });
    return standalone(parse(json{ {"CID", id}, {"LABEL", id}, {"LAYERS", L} }, "/tmp/vg_runner"));
}
bool recipeHas(const std::vector<std::string> & r, const std::string & needle)
{
    for (const auto & s : r) if (s.find(needle) != std::string::npos) return true;
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
    L.push_back({ {"EXEC", json::array({ E })} });
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
    // ---- engine-injected session vars (friend LAN / presence) ----
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
        idx.Nodes["game"] = launchNode("game", "win32", {},
            json{{"ARGS", json::array({"--address=%VIDYAGOD_JOIN_ADDRESS%", "%VIDYAGOD_JOIN_ADDRESS%", "--wait"})}});
        finish(idx);

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
        idx.Nodes["game"] = launchNode("game", "win32", {}, json::object(),
            json::array({ json{{"VARS", {{"VIDYAGOD_SELF_NAME", {{"DEFAULT", "PackageChoice"}}}}}} }));
        finish(idx);

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
        idx.Nodes["wine"]   = runnerNode("wine", {"win32"}, {"proton"});   // build ships via the content node it contains
        idx.Nodes["proton"] = contentNode("proton", json::array({ json{{"ZIP", "proton.zip"}} }));
        idx.Nodes["base"]   = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"]   = launchNode("game", "win32", {"base"});
        finish(idx);

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
        QVERIFY(recipeHas(cp.Recipe, "game"));                 // the resolved row is the recipe
        QVERIFY(pool.contains("COMPONENTS") && !pool["COMPONENTS"].empty());
        int base = 0;                                          // …and the content it contains is in it, placed
        for (const auto & L : cp.SubComponentsArray)
            if (L.value("TYPE", std::string()) == "VFSDirLayer" && L.value("PATH", std::string()).find("/base") != std::string::npos) ++base;
        QCOMPARE(base, 1);
    }

    // A library pinned by the RUNNER contributes its order-independent layers to the game runtime, exactly as a
    // library pinned by the GAME does — the runner chain is equivalent to the content chain in capability for
    // DllOverride/RegEdit/FileEdit. (It is NOT equivalent for VFS: a runner parent's VFS builds the runner tree.)
    void runner_closure_contributes_overrides_and_edits()
    {
        NodeIndex idx;
        idx.Nodes["mediastack"] = contentNode("mediastack", json::array({
            json{{"DLL", {{"winegstreamer", ""}}}},
            json{{"REG", {{"HKLM", {{"Software", {{"Lav", json::object()}}}}}}}},
            json{{"ZIP", "codecs.zip"}, {"TARGET", "FILES/files/lib/gstreamer-1.0"}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"mediastack"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);

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
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
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
        idx.Nodes["fresh"] = contentNode("fresh");                  // no entry, no content: a fresh node
        finish(idx);

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
        idx.Nodes["c"] = contentNode("c", json::array({ json{{"DIR", "files"}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "c"; cp.AuthoringBare = true;
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        QVERIFY(cp.RunnerChain.empty());
        QVERIFY(recipeHas(cp.Recipe, "c"));                   // the node's own content layer is in the recipe
        QCOMPARE((int)cp.SubComponentsArray.size(), 1);
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
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}}));
        QCOMPARE((int)cp.SubComponentsArray.size(), 1);
        QCOMPARE(cp.SubComponentsArray[0]["TARGET"].get<std::string>(), std::string("GameDir/data"));
        const auto L = LaunchResolver::AnchorLayouts(cp);
        QCOMPARE((int)L.size(), 1);
        QCOMPARE(L[0].second, std::string("GameDir"));
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

    // The default pick prefers a runner RECOMMENDED for this tile over the alphabetically-first one. (A package never
    // names a runner; a runner may say which tiles it is recommended for.)
    void pick_runner_prefers_recommended()
    {
        NodeIndex idx;
        idx.Nodes["aaa_wine"] = runnerNode("aaa_wine", {"win32"});   // sorts first
        Node rec = runnerNode("zzz_wine", {"win32"}); rec.Recommended = {"game"};
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

    // A pattern names files in one folder: always a file keep, whatever its last segment looks like ("slot*" has no
    // dot — by shape alone it would become a passthrough of a folder literally named "slot*" and persist nothing),
    // and a pattern outside the last segment is refused, loudly. Teeth: classify patterns by shape; accept "save/*/x".
    void derive_persistence_patterns_are_file_keeps()
    {
        ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"};
        json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/save/slot*"},{"TARGET","Slots"}},
            json{{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH","game/*/profile.dat"},{"TARGET","Profiles"}} })}} })}};
        LaunchResolver::DerivePersistence(pool, cp);
        QCOMPARE((int)cp.KeepDirs.size(), 0);
        QCOMPARE((int)cp.KeepFiles.size(), 1);
        QCOMPARE(cp.KeepFiles[0].Path, std::string("game/save/slot*"));
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
            json{{"KEEP", {{"FILES/pfx/drive_c/users", true}, {"REG/HKCU", true}}}} }));
        idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"proton_keepset"});
        idx.Nodes["game"] = launchNode("game", "win32", {});
        finish(idx);

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
        ch.Nodes["emu_keep"] = contentNode("emu_keep", json::array({ json{{"KEEP", {{"FILES/drive_c/emu_state", true}}}} }));
        ch.Nodes["emu"]      = chainRunner("emu", {"vortex"}, "win32", "%RunnerMount%/run", {"emu_keep"});
        ch.Nodes["proton"]   = chainRunner("proton", {"win32"}, kMachine, "%RunnerMount%/run", {"proton_keep"});
        ch.Nodes["proton_keep"] = contentNode("proton_keep", json::array({ json{{"KEEP", {{"FILES/pfx/drive_c/users", true}}}} }));
        ch.Nodes["nativerun"] = chainRunner("nativerun", {kMachine}, kMachine, "");
        ch.Nodes["vgame"]     = launchNode("vgame", "vortex", {});
        finish(ch);

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
                json{{"KEEP", {{"FILES/drive_c/Saves", true}}}, {"WHEN", when}} }));
            idx.Nodes["wine"] = runnerNode("wine", {"win32"}, {"keep"});
            idx.Nodes["game"] = launchNode("game", "win32", {});
            finish(idx);
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

    // The chain bridges the SELECTED entry's platform, never the node's default view: a node carrying a native entry
    // (the default) and a win32 entry routes the win32 one through a win32 runner.
    void chain_follows_the_selected_entrypoint()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["protonB"] = chainRunner("protonB", {"win32"}, kMachine);
        Node launch = standalone(parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "game"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({ json{{"LABEL", "Native"}, {"HOST", kMachine}, {"EXE", "game"}, {"TILE", {{"UID", "game"}}}},
                                        json{{"LABEL", "Windows"}, {"HOST", "win32"}, {"EXE", "game.exe"}} })}} })} }, "/tmp/vg_bundle"));
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

    // A graft (a node whose list begins with ANY naming the variant) that carries an entry is a WAY TO RUN the
    // variant: applied, its entry is in the row's fold, --entry picks it, and its HOST drives the chain — the mount
    // stays the variant's plus the graft.
    void a_grafts_entry_runs_over_the_variants_mount()
    {
        NodeIndex idx;
        idx.Nodes["protonA"] = chainRunner("protonA", {"win32"}, kMachine);
        idx.Nodes["game"] = launchNode("game", kMachine, {});
        idx.Nodes["loader"] = parse(json{ {"CID", "loader"}, {"LABEL", "loader"}, {"LAYERS", json::array({
            json{{"ANY", json::array({"game"})}}, json{{"DIR", "loader"}},
            json{{"EXEC", json::array({ json{{"LABEL", "Loader"}, {"HOST", "win32"}, {"EXE", "loader.exe"}} })}} })} }, "/tmp/vg_bundle");
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        cp.Grafts = std::vector<std::string>{ "loader" };
        cp.Entrypoint = "Loader";
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
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
            json{{"ANY", json::array({"game"})}}, json{{"DIR", "mod"}}, json{{"ENV", {{"C", "mod"}}}} })} }, "/tmp/vg_bundle");
        idx.Nodes["rlib"] = contentNode("rlib", json::array({ json{{"ZIP", "rlib.zip"}}, json{{"ENV", {{"R", "rlib"}, {"S", "rlib"}, {"T", "rlib"}}}} }));
        idx.Nodes["native"] = chainRunner("native", {kMachine}, kMachine, "%RunnerMount%/run", {"rlib"},
                                          json::array({ json{{"ENV", {{"S", "runner"}, {"R", nullptr}}}} }));
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{ "mod" };
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
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

    // A label may repeat (a friend's received stub of "proton" beside the local "proton"): every walk on the launch
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
            json{{"ANY", json::array({"game"})}},
            json{{"EXEC", json::array({ json{{"LABEL", "Loader"}, {"HOST", "win32"}, {"EXE", "loader.exe"}} })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["elsewhere"] = parse(json{ {"CID", "elsewhere"}, {"LABEL", "elsewhere"}, {"LAYERS", json::array({
            json{{"ANY", json::array({"other_game"})}},
            json{{"EXEC", json::array({ json{{"LABEL", "Loader"}, {"HOST", "win32"}, {"EXE", "x.exe"}} })}} })} }, "/tmp/vg_bundle");
        finish(idx);
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game"; cp.Grafts = std::vector<std::string>{};   // unticked
        cp.Entrypoint = "Loader";
        json pool = json::object();
        const json cfg = json{{"Settings", json::object()}};
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, pool, cfg));
        cp.Grafts = std::vector<std::string>{ "elsewhere" };                                  // not offered to this row
        QVERIFY(!LaunchResolver::InitializeFromNode(cp, pool, cfg));
        QVERIFY(cp.AppliedGrafts.empty());
    }

    // A row whose requirements fail is blocked (§4): a NOT naming something the row contains, or an ANY that finds
    // none. A graft whose NOT would hit is not applied — the row still launches without it.
    void an_unsatisfied_row_is_blocked_and_a_conflicting_graft_is_not_applied()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        idx.Nodes["blocked"] = launchNode("blocked", kMachine, {"base"}, json::object(), json::array({ json{{"NOT", "base"}} }));
        idx.Nodes["needs"] = launchNode("needs", kMachine, {}, json::object(), json::array({ json{{"ANY", json::array({"nowhere"})}} }));
        idx.Nodes["conflict"] = parse(json{ {"CID", "conflict"}, {"LABEL", "conflict"}, {"RECOMMENDED", json::array({"game"})},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"NOT", "base"}}, json{{"DIR", "c"}} })} }, "/tmp/vg_bundle");
        idx.Nodes["fine"] = parse(json{ {"CID", "fine"}, {"LABEL", "fine"}, {"RECOMMENDED", json::array({"game"})},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"DIR", "f"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        const json cfg = json{{"Settings", json::object()}};
        for (const char *Row : {"blocked", "needs"})
        {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Row;
            json pool = json::object();
            QVERIFY2(!LaunchResolver::InitializeFromNode(cp, pool, cfg), Row);
        }
        ContainerParams cp("/tmp/vg_bundle");
        cp.NodeIdx = &idx; cp.LaunchNodeId = "game";
        json pool = json::object();
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
        QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "fine" });   // both pre-ticked; the conflicting one does not apply
        cp.Grafts = std::vector<std::string>{ "conflict", "fine" };
        QVERIFY(LaunchResolver::InitializeFromNode(cp, pool, cfg));
        QVERIFY(cp.AppliedGrafts == std::vector<std::string>{ "fine" });
    }

    // The instance's graft list is ordered — the order grafts apply in — and the pre-launch window reorders it
    // through MoveGraft. A graft applies only once the grafts it needs are applied, so a move that would leave one
    // unapplied (a graft on a graft moved above its base) is refused and names it; an allowed move reorders.
    void a_graft_reorder_that_would_drop_a_graft_is_refused()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        const auto graft = [](const char *Id, const char *On) {
            return parse(json{ {"CID", Id}, {"LABEL", Id},
                {"LAYERS", json::array({ json{{"ANY", json::array({On})}}, json{{"DIR", Id}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["a"] = graft("a", "game");
        idx.Nodes["b"] = graft("b", "game");
        idx.Nodes["ona"] = graft("ona", "a");                                     // a graft on the graft a
        finish(idx);
        std::vector<std::string> T{ "a", "ona", "b" };
        QCOMPARE(PackageCatalog::AppliedGrafts(idx, "game", T).size(), size_t(3));
        std::string Why;
        QVERIFY(!PackageCatalog::MoveGraft(idx, "game", T, 1, -1, &Why));           // ona above a: it would drop
        QVERIFY2(Why.find("'ona'") != std::string::npos, Why.c_str());
        QVERIFY(!PackageCatalog::MoveGraft(idx, "game", T, 0, +1));                // a below ona: the same
        QVERIFY((T == std::vector<std::string>{ "a", "ona", "b" }));                // refused moves change nothing
        QVERIFY(PackageCatalog::MoveGraft(idx, "game", T, 2, -1));                 // b is independent: it moves
        QVERIFY((T == std::vector<std::string>{ "a", "b", "ona" }));
        QVERIFY(!PackageCatalog::MoveGraft(idx, "game", T, 0, -1));                // nothing above the first
        QVERIFY(!PackageCatalog::MoveGraft(idx, "game", T, 2, +1));                // nothing below the last

        // Two grafts that exclude each other: the first applies, the second not. Swapping them keeps the COUNT (one
        // applies either way) while the one that applied stops — refused by WHICH grafts apply, not how many.
        // Teeth: compare the sizes again and the swap is accepted.
        const auto excl = [](const char *Id, const char *Not) {
            return parse(json{ {"CID", Id}, {"LABEL", Id},
                {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"NOT", Not}}, json{{"DIR", Id}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["x"] = excl("x", "y");
        idx.Nodes["y"] = excl("y", "x");
        finish(idx);
        std::vector<std::string> XY{ "x", "y" };
        QVERIFY((PackageCatalog::AppliedGrafts(idx, "game", XY) == std::vector<std::string>{ "x" }));
        std::string WhyX;
        QVERIFY(!PackageCatalog::MoveGraft(idx, "game", XY, 1, -1, &WhyX));
        QVERIFY2(WhyX.find("'x'") != std::string::npos, WhyX.c_str());
        // The tile decides it too: graft p brings q only under tile 7, and onq is a graft on q. Under tile 7 moving onq
        // above p drops it; judged without the tile (the node's own, "game") onq applies in neither order and the drop
        // goes unseen. The pre-launch window judges as the launch does (%UID% = the tile it launches).
        // Teeth: ignore Builtins in MoveGraft and the move below is accepted.
        idx.Nodes["q"] = contentNode("q", json::array({ json{{"DIR", "q"}} }));
        idx.Nodes["p"] = parse(json{ {"CID", "p"}, {"LABEL", "p"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"game"})}}, json{{"NODE", "q"}, {"WHEN", "%UID%==7"}} })} }, "/tmp/vg_bundle");
        idx.Nodes["onq"] = graft("onq", "q");
        finish(idx);
        std::vector<std::string> PQ{ "p", "onq" };
        const std::map<std::string, std::string> Tile7{ {"UID", "7"} };
        QCOMPARE(PackageCatalog::AppliedGrafts(idx, "game", PQ, {}, Tile7).size(), size_t(2));
        QCOMPARE(PackageCatalog::AppliedGrafts(idx, "game", PQ).size(), size_t(1));      // not under the node's own tile
        QVERIFY2(!PackageCatalog::MoveGraft(idx, "game", PQ, 1, -1, nullptr, {}, Tile7), "under tile 7 the move drops onq");
    }

    // The default graft order keeps related grafts together: a graft follows every offered graft its ANY/NOT names,
    // and the one naming the latest listed goes next; unrelated ones go by LABEL. AoE2's soundtracks stay adjacent
    // though a patch's LABEL sorts between them. Teeth: sort by LABEL (patch lands between aok and tc).
    void the_default_graft_order_keeps_related_grafts_together()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        const auto graft = [](const char *Id, const char *Label, const json &Extra) {
            json L = json::array({ json{{"ANY", json::array({"game"})}} });
            for (const auto &X : Extra) L.push_back(X);
            L.push_back(json{{"DIR", Id}});
            return parse(json{ {"CID", Id}, {"LABEL", Label}, {"LAYERS", L} }, "/tmp/vg_bundle");
        };
        idx.Nodes["aok"] = graft("aok", "Age of Kings", json::array());
        idx.Nodes["tc"] = graft("tc", "The Conquerors", json::array({ json{{"NOT", "aok"}} }));
        idx.Nodes["both"] = graft("both", "Both", json::array({ json{{"NOT", "aok"}}, json{{"NOT", "tc"}} }));
        idx.Nodes["patch"] = graft("patch", "Patch", json::array());
        finish(idx);
        QCOMPARE(PackageCatalog::OfferedGrafts(idx, "game"), (std::vector<std::string>{ "aok", "tc", "both", "patch" }));
        // A variant that contains a graft (FE holds UserPatch) is not offered it again. Teeth: offer contained grafts.
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"VARIANT", "m"},
            {"LAYERS", json::array({ json{{"NODE", "game"}}, json{{"NODE", "patch"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        QCOMPARE(PackageCatalog::OfferedGrafts(idx, "mod"), (std::vector<std::string>{ "aok", "tc", "both" }));
    }

    // Ticking a graft in the pre-launch window goes through TickGraft: the graft just ticked wins, so grafts that
    // exclude each other (AoE2's three soundtracks: TC NOT AoK, Both NOT AoK + NOT TC — a CID can only name an earlier
    // one) behave as a choice of one, whichever side carries the NOT. A graft the ticked one needs, or one unrelated,
    // stays ticked. Teeth: skip the unticking (all three stay ticked); untick without judging the whole list (a graft
    // on a graft whose base is ticked unticks that base); keep G in its old place (it must go last).
    void ticking_a_graft_unticks_the_grafts_it_excludes()
    {
        NodeIndex idx;
        idx.Nodes["base"] = contentNode("base", json::array({ json{{"DIR", "base"}} }));
        idx.Nodes["game"] = launchNode("game", kMachine, {"base"});
        const auto graft = [](const char *Id, const json &Layers) {
            json L = json::array({ json{{"ANY", json::array({"game"})}} });
            for (auto &X : Layers) L.push_back(X);
            L.push_back(json{{"DIR", Id}});
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"LAYERS", L} }, "/tmp/vg_bundle");
        };
        idx.Nodes["aok"]  = graft("aok",  json::array());
        idx.Nodes["tc"]   = graft("tc",   json::array({ json{{"NOT", "aok"}} }));
        idx.Nodes["both"] = graft("both", json::array({ json{{"NOT", "aok"}}, json{{"NOT", "tc"}} }));
        idx.Nodes["free"] = graft("free", json::array());
        idx.Nodes["ona"] = parse(json{ {"CID", "ona"}, {"LABEL", "ona"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"aok"})}}, json{{"DIR", "ona"}} })} }, "/tmp/vg_bundle");
        finish(idx);
        using V = std::vector<std::string>;
        V Un;
        QCOMPARE(PackageCatalog::TickGraft(idx, "game", V{ "free", "aok" }, "both", &Un), (V{ "free", "both" }));   // both's NOT
        QCOMPARE(Un, V{ "aok" });
        Un.clear();
        QCOMPARE(PackageCatalog::TickGraft(idx, "game", V{ "both", "free" }, "aok", &Un), (V{ "free", "aok" }));    // the other side's NOT
        QCOMPARE(Un, V{ "both" });
        Un.clear();
        QCOMPARE(PackageCatalog::TickGraft(idx, "game", V{ "tc" }, "aok", &Un), (V{ "aok" }));
        QCOMPARE(Un, V{ "tc" });
        Un.clear();
        QCOMPARE(PackageCatalog::TickGraft(idx, "game", V{ "aok", "free" }, "ona", &Un), (V{ "aok", "free", "ona" })); // needs aok
        QVERIFY(Un.empty());
        QCOMPARE(PackageCatalog::TickGraft(idx, "game", V{ "free", "aok" }, "ona", &Un), (V{ "free", "aok", "ona" })); // ...ticked after free:
        QVERIFY(Un.empty());                                                     // ona cannot apply before aok, so free is not to blame
        QCOMPARE(PackageCatalog::AppliedGrafts(idx, "game", PackageCatalog::TickGraft(idx, "game", V{ "aok", "tc", "both" }, "tc")),
                 (V{ "tc" }));                                                                  // one soundtrack applies
    }

    // `--tile <UID> [--variant <name>]` launches what the shelf shows: the named variant under that tile, else the
    // tile's default row (the variant RECOMMENDED under it). A variant presenting two tiles is a row under each.
    void the_row_under_a_tile_is_picked_by_variant_name_or_recommendation()
    {
        NodeIndex idx;
        const auto variant = [](const char *Id, const char *Name, json Faces, json Rec) {
            json Entries = json::array();
            for (const auto &U : Faces)
                Entries.push_back(json{ {"LABEL", "Play " + U.get<std::string>()}, {"HOST", kMachine}, {"EXE", "g.exe"},
                                        {"TILE", {{"UID", U}, {"TITLE", "T" + U.get<std::string>()}}} });
            json N = { {"CID", Id}, {"LABEL", Id}, {"VARIANT", Name}, {"LAYERS", json::array({ json{{"EXEC", Entries}} })} };
            if (!Rec.empty()) N["RECOMMENDED"] = Rec;
            return parse(N, "/tmp/vg_bundle");
        };
        idx.Nodes["old"] = variant("old", "1.00", json::array({"802"}), json::array());
        idx.Nodes["new"] = variant("new", "1.28", json::array({"802", "803"}), json::array({"802"}));   // recommended under 802 only
        idx.Nodes["tft"] = variant("tft", "1.31", json::array({"803"}), json::array({"803"}));
        finish(idx);
        std::string Why;
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "802", ""), std::string("new"));      // the recommended row
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "803", ""), std::string("tft"));      // recommended here, not "new"
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "802", "1.00"), std::string("old"));
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "803", "1.28"), std::string("new"));  // a row under both tiles
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "803", "1.00", &Why), std::string());
        QVERIFY2(Why.find("1.28") != std::string::npos && Why.find("1.31") != std::string::npos, Why.c_str());   // it says what there is
        QCOMPARE(PackageCatalog::RowUnderTile(idx, "999", "", &Why), std::string());
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
        idx.Nodes["v1"] = parse(json{ {"CID", "v1"}, {"LABEL", "v1"}, {"VARIANT", "1.0"}, {"RECOMMENDED", json::array({"803"})},
            {"LAYERS", json::array({ json{{"DIR", "g"}}, json{{"EXEC", json::array({ entry("Reign of Chaos", "802", nullptr, "roc.exe", "Warcraft III: Reign of Chaos"),
                                                                                entry("The Frozen Throne", "803", "802", "tft.exe", "Warcraft III: The Frozen Throne") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["v2"] = parse(json{ {"CID", "v2"}, {"LABEL", "v2"}, {"VARIANT", "2.0"}, {"RECOMMENDED", json::array({"802"})},
            {"LAYERS", json::array({ json{{"NODE", "v1"}}, json{{"EXEC", json::array({ json{{"LABEL", "Reign of Chaos"}, {"EXE", "roc2.exe"}} })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["z"] = parse(json{ {"CID", "z"}, {"LABEL", "z"}, {"VARIANT", "1"},
            {"LAYERS", json::array({ json{{"DIR", "z"}}, json{{"EXEC", json::array({ entry("Play", "9", nullptr, "z.exe", "Age of Empires II") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["s"] = parse(json{ {"CID", "s"}, {"LABEL", "s"}, {"VARIANT", "1"},
            {"LAYERS", json::array({ json{{"DIR", "s"}}, json{{"EXEC", json::array({ entry("Play", "850", nullptr, "s.exe", "Warcraft III: Scenario Pack") })}} })} }, "/tmp/vg_bundle");
        finish(idx);

        std::vector<std::string> Order;
        std::map<std::string, std::vector<std::string>> Rows;
        for (const auto &T : PackageCatalog::ShelfTiles(idx))
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
            json pool = json::object();
            const bool Ok = LaunchResolver::InitializeFromNode(cp, pool, cfg);
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
    // its rows are the versions it applies onto (those containing what its ANY names), and launched from that card a
    // row runs with the graft applied, as the card's title, by the graft's entry. From the version's own card nothing
    // changes. Teeth: no card for a graft (ShelfTiles); launch the row without the graft (the version's own entry runs).
    void a_graft_with_its_own_tile_is_a_card_of_what_it_applies_onto()
    {
        NodeIndex idx;
        const auto entry = [](const char *Exe, const char *Uid, const char *Title) {
            return json{ {"LABEL", "Play"}, {"HOST", kMachine}, {"EXE", Exe}, {"TILE", {{"UID", Uid}, {"TITLE", Title}}} };
        };
        idx.Nodes["tc"] = parse(json{ {"CID", "tc"}, {"LABEL", "tc"}, {"VARIANT", "1.0e"},
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", json::array({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["tc2"] = parse(json{ {"CID", "tc2"}, {"LABEL", "tc2"}, {"VARIANT", "1.0f"},
            {"LAYERS", json::array({ json{{"NODE", "tc"}}, json{{"DIR", "tc2"}} })} }, "/tmp/vg_bundle");       // contains tc
        idx.Nodes["aok"] = parse(json{ {"CID", "aok"}, {"LABEL", "aok"}, {"VARIANT", "2.0a"},
            {"LAYERS", json::array({ json{{"DIR", "aok"}}, json{{"EXEC", json::array({ entry("aok.exe", "749", "The Age of Kings") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["fe"] = parse(json{ {"CID", "fe"}, {"LABEL", "fe"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", "fe"}},
                json{{"EXEC", json::array({ entry("fe.exe", "749000001", "Forgotten Empires") })}} })} }, "/tmp/vg_bundle");
        finish(idx);

        const PackageCatalog::ShelfTile *Card = nullptr;
        const auto Shelf = PackageCatalog::ShelfTiles(idx);
        for (const auto &T : Shelf) if (T.Uid == "749000001") Card = &T;
        QVERIFY2(Card, "the graft's tile is not a card");
        std::set<std::string> Rows;
        for (const Node *N : Card->Rows) Rows.insert(N->NodeId);
        QCOMPARE(Rows, (std::set<std::string>{"tc", "tc2"}));                         // what it applies onto, not aok
        QCOMPARE(Card->Graft, std::string("fe"));

        const json cfg = json{{"Settings", json::object()}};
        const auto launch = [&](const char *Node, const char *Face) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Node; cp.LaunchFace = Face;
            json pool = json::object();
            const bool Ok = LaunchResolver::InitializeFromNode(cp, pool, cfg);
            const std::string Exe = cp.ComposedExec.is_object() ? cp.ComposedExec.value("CONTENTPATH", std::string()) : std::string();
            return std::make_tuple(Ok, Exe, cp.PackageName, cp.AppliedGrafts);
        };
        const auto [Ok, Exe, Name, Grafts] = launch("tc2", "749000001");
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
            {"LAYERS", json::array({ json{{"DIR", "tc"}}, json{{"EXEC", json::array({ entry("tc.exe", "13006", "The Conquerors") })}} })} }, "/tmp/vg_bundle");
        idx.Nodes["aok"] = parse(json{ {"CID", "aok"}, {"LABEL", "aok"}, {"VARIANT", "2.0a"},
            {"LAYERS", json::array({ json{{"DIR", "aok"}}, json{{"EXEC", json::array({ entry("aok.exe", "749", "The Age of Kings") })}} })} }, "/tmp/vg_bundle");
        const auto fe = [&](const char *Id, const char *Ver, const char *Exe) {
            return parse(json{ {"CID", Id}, {"LABEL", Id}, {"VARIANT", Ver},
                {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", Id}},
                    json{{"EXEC", json::array({ entry(Exe, "749000001", "Forgotten Empires") })}} })} }, "/tmp/vg_bundle");
        };
        idx.Nodes["fe22"] = fe("fe22", "2.2", "fe22.exe");
        idx.Nodes["fe25"] = fe("fe25", "2.5", "fe25.exe");
        idx.Nodes["mod"] = parse(json{ {"CID", "mod"}, {"LABEL", "mod"}, {"VARIANT", "1.0e + mod"},
            {"LAYERS", json::array({ json{{"ANY", json::array({"tc"})}}, json{{"DIR", "mod"}} })} }, "/tmp/vg_bundle");
        finish(idx);

        std::map<std::string, std::set<std::string>> Rows;
        for (const auto &T : PackageCatalog::ShelfTiles(idx)) for (const Node *N : T.Rows) Rows[T.Uid].insert(N->NodeId);
        QCOMPARE(Rows["749000001"], (std::set<std::string>{"fe22", "fe25"}));
        QCOMPARE(Rows["13006"], (std::set<std::string>{"tc", "mod"}));
        QCOMPARE(PackageCatalog::GraftBases(idx, "fe25"), (std::vector<std::string>{"tc"}));

        const json cfg = json{{"Settings", json::object()}};
        const auto launch = [&](const char *Node, const char *Face, const char *Base) {
            ContainerParams cp("/tmp/vg_bundle");
            cp.NodeIdx = &idx; cp.LaunchNodeId = Node; cp.LaunchFace = Face; cp.GraftBase = Base;
            json pool = json::object();
            const bool Ok = LaunchResolver::InitializeFromNode(cp, pool, cfg);
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

    // BFS tie-break: a bridge runner RECOMMENDED for this tile beats the alphabetically-first one.
    void chain_bridge_prefers_recommended()
    {
        NodeIndex idx;
        idx.Nodes["aaa_proton"] = chainRunner("aaa_proton", {"win32"}, kMachine);
        Node rec = chainRunner("zzz_proton", {"win32"}, kMachine); rec.Recommended = {"game"};
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

    // EVAL: a declaration whose value is an integer expression over other vars — UserPatch's Mini-map Colors from its
    // three installer options — evaluates once its operands resolve (forward references included); an override of an
    // operand reaches it; a plain declaration stays text. Teeth: skip EVAL (the value stays "1*2 | 0*32 | ...").
    void resolve_custom_var_eval()
    {
        auto cv = [](const std::string& k, const std::string& d){ return json{{"TYPE","CustomVar"},{"KEY",k},{"DEFAULT",d}}; };
        json mm = cv("MM", "%RED%*2 | %PURPLE%*32 | (1-%GREY%)*64"); mm["EVAL"] = true;
        const json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID","c1"}, {"SUBCOMPONENTS", json::array({
            mm, cv("RED","1"), cv("PURPLE","0"), cv("GREY","1"), cv("RAW","%RED%*2") })}} })}};
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["MM"], std::string("2"));
          QCOMPARE(cp.CustomVariables["RAW"], std::string("1*2")); }            // no EVAL: text
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          cp.VariableOverrides["GREY"] = "0"; cp.VariableOverrides["PURPLE"] = "1";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["MM"], std::string("98")); }              // 2 | 32 | 64
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
        //proton has NO explicit GUEST_PATH — the guest sees the game's content at its GUEST_ROOTS %GameDir%.
        RunnerLink Proton = mkLink("proton", "linux64", "%RunnerMount%/proton", "pfx/drive_c/%PackageUID%",
                                   {"waitforexitandrun", "%GameDir%\\%ContentPath%"});
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
            nlohmann::ordered_json L = { {"EXEC", nlohmann::ordered_json::array({ { {"LABEL", "run"}, {"HOST", "linux64"},
                {"GUEST", nlohmann::ordered_json::array({"win32"})}, {"EXE", "wine"}, {"GUEST_ROOTS", Roots},
                {"DRIVES", { {"C:", "pfx/drive_c"} }} } })} };
            const std::string Why = NodeLower::CheckNode({ {"LABEL", "r"}, {"LAYERS", nlohmann::ordered_json::array({L})} }, "r");
            QVERIFY2(Why.find("in a circle") != std::string::npos, Why.c_str());
        }
        nlohmann::ordered_json Ok = { {"EXEC", nlohmann::ordered_json::array({ { {"LABEL", "run"}, {"HOST", "linux64"},
            {"GUEST", nlohmann::ordered_json::array({"win32"})}, {"EXE", "wine"},
            {"GUEST_ROOTS", { {"%AppData%", "%UserProfile%\\AppData"}, {"%UserProfile%", "C:\\users\\me"} }},
            {"DRIVES", { {"C:", "pfx/drive_c"} }} } })} };
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
            json pool = json::object();
            return LaunchResolver::InitializeFromNode(cp, pool, json{{"Settings", json::object()}});
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
        json pool = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE", "CustomVar"}, {"KEY", "MYVAR"}, {"DEFAULT", "def"}, {"UI", {{"CONTROL", "text"}}}},
            json{{"TYPE", "CustomVar"}, {"KEY", "WEATHER"}, {"DEFAULT", "1"}, {"UI", {{"CONTROL", "bool"}}}},
            derived })}} })}};

        // temp UserDataRoot isolates every read/write here from the real ~/.VidyaGod (the accessors now hit disk).
        QTemporaryDir ud; QVERIFY(ud.isValid());
        const json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        // default (no instance config written yet → empty → the CustomVar DEFAULT)
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("def")); }
        // user setting (now lives in the INSTANCE file — write it there)
        QVERIFY(InstanceStore::WriteConfig(cfg, "pkg", "DefaultInstance", json{{"VARIABLES", {{"MYVAR", "cfg"}, {"DERIVED", "0"}}}}));
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("cfg"));
          QCOMPARE(cp.CustomVariables["DERIVED"], std::string("4")); }        // the stale saved "0" is not a setting
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
