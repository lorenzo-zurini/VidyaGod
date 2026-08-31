// test_manifestmodel.cpp — the node graph: dependency ordering, optional selection, and validation.
//
// manifestmodel is the largest module in the engine and had no tests. It is also the layer that decides what
// counts as a VALID package — so every quiet package bug this session hunted is, ultimately, something this
// layer either catches or does not. Ordering matters for correctness too: PARENTS order IS layer priority, so a
// mistake here silently changes which file wins in the composed filesystem.

#include <QtTest/QtTest>
#include <set>
#include <string>
#include <vector>

#include "manifestmodel.h"

using nlohmann::ordered_json;

namespace {

Node MakeNode(const std::string &Id, const std::vector<std::string> &Parents = {})
{
    Node N;
    N.NodeId    = Id;
    N.Parents   = Parents;
    N.BundleDir = "/tmp/vg_test_bundle";
    N.Layers    = ordered_json::array();
    return N;
}

Node MakeLaunchable(const std::string &Id, const std::vector<std::string> &Parents = {})
{
    Node N = MakeNode(Id, Parents);
    N.HasExec      = true;
    N.HostPlatform = "win32";
    N.Exec         = ordered_json{{"CONTENTPATH", "game.exe"}};
    return N;
}

bool Contains(const std::vector<std::string> &V, const std::string &S)
{
    return std::find(V.begin(), V.end(), S) != V.end();
}

// Index of an id in the resolved order, or -1.
int IndexOf(const std::vector<std::string> &V, const std::string &S)
{
    for (size_t I = 0; I < V.size(); ++I) if (V[I] == S) return static_cast<int>(I);
    return -1;
}

} // namespace

class ManifestModelTest : public QObject
{
    Q_OBJECT
private slots:

    // ---- ResolveNodeOrder: parents first, launchable last ----

    // The launchable must be LAST, because the order IS the layer priority — its own files must win over its
    // dependencies'. Getting this backwards would silently invert every override in every package.
    void order_puts_parents_first_and_the_launchable_last()
    {
        NodeIndex Idx;
        Idx.Nodes["base"]  = MakeNode("base");
        Idx.Nodes["patch"] = MakeNode("patch", {"base"});
        Idx.Nodes["game"]  = MakeLaunchable("game", {"patch"});

        const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QCOMPARE(Order.back(), std::string("game"));
        QVERIFY2(IndexOf(Order, "base") < IndexOf(Order, "patch"), "a parent must come before its child");
        QVERIFY2(IndexOf(Order, "patch") < IndexOf(Order, "game"), "dependencies must come before the launchable");
    }

    // PARENTS list order is the documented tie-break: a later parent has higher priority, so it must resolve later.
    void parents_list_order_is_the_tie_break()
    {
        NodeIndex Idx;
        Idx.Nodes["a"]    = MakeNode("a");
        Idx.Nodes["b"]    = MakeNode("b");
        Idx.Nodes["game"] = MakeLaunchable("game", {"a", "b"});

        const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QVERIFY2(IndexOf(Order, "a") < IndexOf(Order, "b"),
                 "a later entry in PARENTS must resolve later, i.e. at higher priority");
    }

    // A diamond must include the shared ancestor exactly once, and still before both dependents.
    void a_shared_ancestor_appears_once_and_early()
    {
        NodeIndex Idx;
        Idx.Nodes["lib"]  = MakeNode("lib");
        Idx.Nodes["l"]    = MakeNode("l", {"lib"});
        Idx.Nodes["r"]    = MakeNode("r", {"lib"});
        Idx.Nodes["game"] = MakeLaunchable("game", {"l", "r"});

        const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QCOMPARE(std::count(Order.begin(), Order.end(), std::string("lib")), 1L);
        QVERIFY(IndexOf(Order, "lib") < IndexOf(Order, "l"));
        QVERIFY(IndexOf(Order, "lib") < IndexOf(Order, "r"));
    }

    // A cycle must TERMINATE. An authoring mistake must not hang the launcher.
    void a_cycle_terminates_instead_of_hanging()
    {
        NodeIndex Idx;
        Idx.Nodes["a"]    = MakeNode("a", {"b"});
        Idx.Nodes["b"]    = MakeNode("b", {"a"});
        Idx.Nodes["game"] = MakeLaunchable("game", {"a"});

        const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QVERIFY2(!Order.empty(), "a cyclic graph must still resolve to something rather than hanging");
        QCOMPARE(Order.back(), std::string("game"));
    }

    // A missing PARENTS id must be REPORTED, not silently dropped — a typo'd dependency otherwise produces a
    // package that launches with a layer missing and no complaint.
    void a_missing_parent_is_reported()
    {
        NodeIndex Idx;
        Idx.Nodes["game"] = MakeLaunchable("game", {"nonexistent_dependency"});

        std::vector<std::string> Missing;
        ManifestModel::ResolveNodeOrder(Idx, "game", {}, &Missing);
        QVERIFY2(Contains(Missing, "nonexistent_dependency"), "a dangling PARENTS id must be surfaced");
    }

    void resolving_an_unknown_node_is_safe()
    {
        NodeIndex Idx;
        Idx.Nodes["game"] = MakeLaunchable("game");
        const auto Order = ManifestModel::ResolveNodeOrder(Idx, "no_such_node", {});
        QVERIFY(Order.empty() || !Contains(Order, "game"));
    }

    // ---- OPTIONAL nodes: the selection axis ----

    // An OPTIONAL parent is included only when its toggle says so; DEFAULT decides when the user has not chosen.
    void optional_nodes_follow_their_toggle_and_default()
    {
        NodeIndex Idx;
        Node Opt = MakeNode("hd_textures");
        Opt.Optional = true;
        Opt.Default  = false;
        Idx.Nodes["hd_textures"] = Opt;
        Idx.Nodes["game"] = MakeLaunchable("game", {"hd_textures"});

        const auto Off = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QVERIFY2(!Contains(Off, "hd_textures"), "an optional node defaulting to off must be excluded");

        const auto On = ManifestModel::ResolveNodeOrder(Idx, "game", {{"hd_textures", true}});
        QVERIFY2(Contains(On, "hd_textures"), "enabling the toggle must include it");

        Idx.Nodes["hd_textures"].Default = true;
        const auto DefOn = ManifestModel::ResolveNodeOrder(Idx, "game", {});
        QVERIFY2(Contains(DefOn, "hd_textures"), "DEFAULT true must include it when the user has not chosen");

        const auto ForcedOff = ManifestModel::ResolveNodeOrder(Idx, "game", {{"hd_textures", false}});
        QVERIFY2(!Contains(ForcedOff, "hd_textures"), "an explicit toggle must beat DEFAULT");
    }

    void optional_nodes_are_listed_for_the_picker()
    {
        NodeIndex Idx;
        Node Opt = MakeNode("mod");
        Opt.Optional = true;
        Idx.Nodes["mod"]  = Opt;
        Idx.Nodes["base"] = MakeNode("base");
        Idx.Nodes["game"] = MakeLaunchable("game", {"base", "mod"});

        const auto Opts = ManifestModel::OptionalNodes(Idx, "game");
        bool Found = false;
        for (const Node *N : Opts) if (N->NodeId == "mod") Found = true;
        QVERIFY2(Found, "an optional ancestor must be offered to the picker");
        for (const Node *N : Opts)
            QVERIFY2(N->NodeId != "base", "a non-optional node must not appear as a choice");
    }

    // ---- ValidateNodeGraph: the thing that is supposed to catch package bugs ----

    void validate_accepts_a_healthy_graph()
    {
        NodeIndex Idx;
        Idx.Nodes["base"] = MakeNode("base");
        Idx.Nodes["game"] = MakeLaunchable("game", {"base"});

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        QVERIFY2(Errors.empty(), qPrintable(QString("unexpected errors: %1")
                 .arg(Errors.empty() ? QString() : QString::fromStdString(Errors.front()))));
    }

    void validate_reports_a_dangling_parent()
    {
        NodeIndex Idx;
        Idx.Nodes["game"] = MakeLaunchable("game", {"ghost"});

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        bool Mentions = false;
        for (const auto &E : Errors) if (E.find("ghost") != std::string::npos) Mentions = true;
        for (const auto &W : Warnings) if (W.find("ghost") != std::string::npos) Mentions = true;
        QVERIFY2(Mentions, "a PARENTS id that does not exist must be reported by name");
    }

    void validate_reports_a_cycle()
    {
        NodeIndex Idx;
        Idx.Nodes["a"]    = MakeNode("a", {"b"});
        Idx.Nodes["b"]    = MakeNode("b", {"a"});
        Idx.Nodes["game"] = MakeLaunchable("game", {"a"});

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        QVERIFY2(!Errors.empty() || !Warnings.empty(), "a PARENTS cycle must not validate silently");
    }

    // The %variable% lint: an undefined reference is almost always a typo that would survive into a real path or
    // command line. The engine-injected VIDYAGOD_* tokens must NOT be flagged — they are supplied per launch.
    void validate_flags_unknown_variables_but_not_engine_injected_ones()
    {
        NodeIndex Idx;
        Node G = MakeLaunchable("game");
        G.Layers = ordered_json::array({
            ordered_json{{"TYPE", "FileEdit"}, {"MODE", "Overwrite"}, {"OVERRIDE", true},
                         {"FILE", "%PrefixRoot%/drive_c/cfg.ini"}, {"VALUE", "name=%VIDYAGOD_SELF_NAME%"}},
        });
        Idx.Nodes["game"] = G;

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        for (const auto &E : Errors)
            QVERIFY2(E.find("VIDYAGOD_SELF_NAME") == std::string::npos,
                     "engine-injected session tokens must not be reported as undefined");

        Node H = MakeLaunchable("game2");
        H.Layers = ordered_json::array({
            ordered_json{{"TYPE", "FileEdit"}, {"MODE", "Overwrite"}, {"OVERRIDE", true},
                         {"FILE", "%PrefixRoot%/x"}, {"VALUE", "%DEFINITELY_NOT_A_REAL_TOKEN%"}},
        });
        Idx.Nodes["game2"] = H;
        std::vector<std::string> Errors2, Warnings2;
        ManifestModel::ValidateNodeGraph(Idx, Errors2, Warnings2);
        bool Flagged = false;
        for (const auto &E : Errors2)   if (E.find("DEFINITELY_NOT_A_REAL_TOKEN") != std::string::npos) Flagged = true;
        for (const auto &W : Warnings2) if (W.find("DEFINITELY_NOT_A_REAL_TOKEN") != std::string::npos) Flagged = true;
        QVERIFY2(Flagged, "an undefined %token% must be reported — it would survive verbatim into a path or argv");
    }

    // Scoping must not change the verdict for the node in scope: the launch gate validates one package, and it
    // must reach the same conclusion the full sweep would.
    void scoped_validation_agrees_with_the_full_sweep()
    {
        NodeIndex Idx;
        Idx.Nodes["good"] = MakeLaunchable("good");
        Idx.Nodes["bad"]  = MakeLaunchable("bad", {"ghost"});

        std::vector<std::string> AllE, AllW;
        ManifestModel::ValidateNodeGraph(Idx, AllE, AllW);
        QVERIFY(!AllE.empty() || !AllW.empty());

        std::set<std::string> OnlyGood{"good"};
        std::vector<std::string> E, W;
        ManifestModel::ValidateNodeGraph(Idx, E, W, &OnlyGood);
        for (const auto &X : E) QVERIFY2(X.find("ghost") == std::string::npos,
                                         "scoping to a healthy node must not report another node's problem");
    }

    // ---- Layer-type helpers: small, but they gate what gets mounted ----

    void vfs_layer_types_are_recognised()
    {
        QVERIFY(ManifestModel::IsVfsLayer("VFSZipLayer"));
        QVERIFY(ManifestModel::IsVfsLayer("VFSDirLayer"));
        QVERIFY(!ManifestModel::IsVfsLayer("FileEdit"));
        QVERIFY(!ManifestModel::IsVfsLayer("RegEdit"));
        QVERIFY(!ManifestModel::IsVfsLayer("CustomVar"));
        QVERIFY(!ManifestModel::IsVfsLayer(""));
    }

    void layer_type_reads_the_type_field()
    {
        QCOMPARE(ManifestModel::LayerType(ordered_json{{"TYPE", "FileEdit"}}), std::string("FileEdit"));
        QCOMPARE(ManifestModel::LayerType(ordered_json::object()), std::string());
    }

    // Target paths are how layers address the composed filesystem; normalisation must be stable, because two
    // spellings of the same path silently become two different mount targets.
    void target_paths_normalise_consistently()
    {
        const std::string A = ManifestModel::NormalizeTargetPath("pfx/drive_c/game/");
        const std::string B = ManifestModel::NormalizeTargetPath("/pfx/drive_c/game");
        const std::string C = ManifestModel::NormalizeTargetPath("pfx//drive_c/game");
        QCOMPARE(A, B);
        QCOMPARE(B, C);
    }
};

QTEST_MAIN(ManifestModelTest)
#include "test_manifestmodel.moc"
