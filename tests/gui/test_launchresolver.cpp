// Tests for the launch-engine resolver (LaunchResolver) — the param/recipe/runner/persistence/exec resolution
// extracted from ContainerWrapper. Builds synthetic NodeIndex graphs in memory and asserts the resolved
// ContainerParams. Uses QApplication (DerivePaths reads the primary screen) under offscreen QPA.

#include <QtTest>

#include "launchresolver.h"
#include "launchparams.h"
#include "manifestmodel.h"

using json = nlohmann::ordered_json;

namespace {
Node contentNode(const std::string & id, const json & layers = json::array())
{
    Node n; n.NodeId = id; n.Role = "content"; n.Layers = layers; n.BundleDir = "/tmp/vg_bundle"; return n;
}
Node launchNode(const std::string & id, const std::string & host, const std::vector<std::string> & parents)
{
    Node n; n.NodeId = id; n.Role = "launchable"; n.HostPlatform = host; n.Parents = parents;
    n.Meta = json{{"TITLE", id}};
    n.Exec = json{{"CONTENTPATH", "game.exe"}};
    n.Layers = json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "game"}} });
    n.BundleDir = "/tmp/vg_bundle"; return n;
}
Node runnerNode(const std::string & id, const std::vector<std::string> & guests,
                const std::vector<std::string> & parents = {})
{
    Node n; n.NodeId = id; n.Role = "runner"; n.GuestPlatform = guests; n.Parents = parents;
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
    Node n; n.NodeId = id; n.Role = "runner"; n.GuestPlatform = guests; n.HostPlatform = host;
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

    // DerivePersistence: no Persist* subcomponents → whole-runtime persist (PersistAll). Declared → selective.
    void derive_persistence_all_vs_declared()
    {
        ContainerParams cp("/tmp/vg_bundle");
        cp.Recipe = {"c1"};

        json poolNone = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array()}} })}};
        LaunchResolver::DerivePersistence(poolNone, cp);
        QVERIFY(cp.PersistAll);
        QVERIFY(cp.PersistDirs.empty());

        ContainerParams cp2("/tmp/vg_bundle"); cp2.Recipe = {"c1"};
        json poolDecl = json{{"COMPONENTS", json::array({ json{{"COMPONENTID", "c1"}, {"SUBCOMPONENTS", json::array({
            json{{"TYPE", "PersistDir"}, {"PATH", "drive_c/saves"}},
            json{{"TYPE", "RegPersist"}} })}} })}};
        LaunchResolver::DerivePersistence(poolDecl, cp2);
        QVERIFY(!cp2.PersistAll);
        QCOMPARE((int)cp2.PersistDirs.size(), 1);
        QVERIFY(cp2.PersistRegistry);
    }

    // ---- Runner daisy-chaining (PickRunnerChain / ResolveChainIds / ResolveRunnerChain) ----

    // win32 content with a direct proton (win32→linux64): chain is [proton, <native terminal>], terminal LAST.
    void chain_win32_single_bridge_plus_native_terminal()
    {
        NodeIndex idx;
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, "linux64");
        idx.Nodes["nativerun"] = chainRunner("nativerun", {"linux64"}, "linux64", "");   // explicit native passthrough
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        const json cfg = json{{"Settings", json::object()}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("proton"));
        QCOMPARE(ids.back(), std::string("nativerun"));        // native terminal always last
    }

    // No authored native runner → the terminal is the synthesized passthrough sentinel.
    void chain_synthesizes_native_terminal_when_unauthored()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = chainRunner("proton", {"win32"}, "linux64");
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
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, "linux64");
        idx.Nodes["nativerun"] = chainRunner("nativerun", {"linux64"}, "linux64", "");
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
        idx.Nodes["nativerun"] = chainRunner("nativerun", {"linux64"}, "linux64", "");
        Node launch = launchNode("game", "linux64", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE((int)ids.size(), 1);
        QCOMPARE(ids[0], std::string("nativerun"));
    }

    // BFS tie-break: a RECOMMENDED bridge runner beats the alphabetically-first one.
    void chain_bridge_prefers_recommended()
    {
        NodeIndex idx;
        idx.Nodes["aaa_proton"] = chainRunner("aaa_proton", {"win32"}, "linux64");
        Node rec = chainRunner("zzz_proton", {"win32"}, "linux64"); rec.Recommended = true;
        idx.Nodes["zzz_proton"] = rec;
        idx.Nodes["nativerun"]  = chainRunner("nativerun", {"linux64"}, "linux64", "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QCOMPARE(ids[0], std::string("zzz_proton"));
    }

    // A persisted RUNNER_CHAIN pin is honored over the default bridge (terminal appended if the pin omits it).
    void chain_honours_persisted_pin()
    {
        NodeIndex idx;
        idx.Nodes["protonA"]   = chainRunner("protonA", {"win32"}, "linux64");
        idx.Nodes["protonB"]   = chainRunner("protonB", {"win32"}, "linux64");
        idx.Nodes["nativerun"] = chainRunner("nativerun", {"linux64"}, "linux64", "");
        Node launch = launchNode("game", "win32", {});
        ContainerParams cp("/tmp/vg_bundle"); cp.PackageUID = "pkg";
        const json cfg = json{{"LIBRARY", json::array({ json{{"PACKAGEUID", "pkg"},
            {"USERSETTINGS", {{"RUNNER_CHAIN", json::array({"protonB"})}}}} })}};
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, cfg);
        QCOMPARE((int)ids.size(), 2);
        QCOMPARE(ids[0], std::string("protonB"));              // pin beats default (protonA sorts first)
        QCOMPARE(ids.back(), std::string("nativerun"));
    }

    // An unreachable platform (no bridging runner) resolves to an empty chain (→ InitializeFromNode aborts).
    void chain_unreachable_platform_is_empty()
    {
        NodeIndex idx;
        idx.Nodes["proton"] = chainRunner("proton", {"win32"}, "linux64");
        Node launch = launchNode("game", "ps2", {});
        ContainerParams cp("/tmp/vg_bundle");
        auto ids = LaunchResolver::ResolveChainIds(idx, launch, cp, json{{"Settings", json::object()}});
        QVERIFY(ids.empty());
    }

    // ResolveRunnerChain materializes RunnerLinks; the terminal link is a native-namespace passthrough.
    void chain_resolves_links_with_native_terminal()
    {
        NodeIndex idx;
        idx.Nodes["proton"]    = chainRunner("proton", {"win32"}, "linux64");
        idx.Nodes["nativerun"] = chainRunner("nativerun", {"linux64"}, "linux64", "");
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

        // default
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, json{{"Settings", json::object()}});
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("def")); }
        // user setting (lives in the package's LIBRARY entry → USERSETTINGS → VARIABLES)
        const json cfg = json{{"LIBRARY", json::array({ json{{"PACKAGEUID", "pkg"},
            {"USERSETTINGS", {{"VARIABLES", {{"MYVAR", "cfg"}}}}}} })}};
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("cfg")); }
        // CLI override wins
        { ContainerParams cp("/tmp/vg_bundle"); cp.Recipe = {"c1"}; cp.PackageUID = "pkg";
          cp.VariableOverrides["MYVAR"] = "ovr";
          LaunchResolver::ResolveCustomVariables(pool, cp, cfg);
          QCOMPARE(cp.CustomVariables["MYVAR"], std::string("ovr")); }
    }
};

QTEST_MAIN(LaunchResolverTest)
#include "test_launchresolver.moc"
