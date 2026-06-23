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
