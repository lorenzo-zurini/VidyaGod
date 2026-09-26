// test_manifestmodel.cpp — the node graph: fold order, gated containment, and validation.
//
// manifestmodel is the largest module in the engine and had no tests. It is also the layer that decides what
// counts as a VALID package — so every quiet package bug this session hunted is, ultimately, something this
// layer either catches or does not. Ordering matters for correctness too: the fold order IS layer priority, so a
// mistake here silently changes which file wins in the composed filesystem.

#include <QtTest/QtTest>
#include <set>
#include <string>
#include <vector>

#include "manifestmodel.h"
#include "fold.h"

using nlohmann::ordered_json;

namespace {

// A generation-6 node: its parents are NODE layers at the head of its list, then its own content (a DIR named for it).
ordered_json NodeJson(const std::string &Id, const std::vector<std::string> &Parents = {}, bool Launchable = false)
{
    ordered_json L = ordered_json::array();
    for (const auto &P : Parents) L.push_back({ {"NODE", P} });
    L.push_back({ {"DIR", Id} });
    if (Launchable)
        L.push_back({ {"EXEC", ordered_json::array({ { {"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "game.exe"},
                                                        {"TILE", { {"UID", Id}, {"TITLE", Id} }} } })} });
    ordered_json N = { {"CID", Id}, {"LABEL", Id}, {"LAYERS", L} };
    if (Launchable) N["VARIANT"] = Id;
    return N;
}

void Add(NodeIndex &Idx, const ordered_json &J)
{
    Node N;
    QVERIFY(ManifestModel::ParseNode(J, "f.json", "/tmp/vg_test_bundle", N));
    Idx.Nodes[N.Key()] = N;
}
void AddNode(NodeIndex &Idx, const std::string &Id, const std::vector<std::string> &Parents = {})
{ Add(Idx, NodeJson(Id, Parents)); }
void AddLaunchable(NodeIndex &Idx, const std::string &Id, const std::vector<std::string> &Parents = {})
{ Add(Idx, NodeJson(Id, Parents, true)); }

Fold::Plan ResolveOf(const NodeIndex &Idx, const std::string &Root)
{
    return Fold::Resolve(ManifestModel::LibraryOf(Idx), Root);
}

// The fold order of the content layers: the node each came from, bottom -> top.
std::vector<std::string> Stack(const Fold::Plan &P)
{
    std::vector<std::string> Out;
    for (const auto &I : P.Seq) Out.push_back(I.From);
    return Out;
}

bool Contains(const std::vector<std::string> &V, const std::string &S)
{
    return std::find(V.begin(), V.end(), S) != V.end();
}

// Index of an id in the stack, or -1.
int IndexOf(const std::vector<std::string> &V, const std::string &S)
{
    for (size_t I = 0; I < V.size(); ++I) if (V[I] == S) return static_cast<int>(I);
    return -1;
}

bool HasEvent(const Fold::Plan &P, const std::string &Kind, const std::string &Cid)
{
    for (const auto &E : P.Events) if (E.first == Kind && E.second == Cid) return true;
    return false;
}

} // namespace

class ManifestModelTest : public QObject
{
    Q_OBJECT
private slots:

    // ---- the fold: a contained node lies beneath what the container lists after it ----

    // The launchable's own files must win over what it contains: the stack IS the layer priority. Getting this
    // backwards would silently invert every override in every package.
    void contained_nodes_lie_beneath_the_container()
    {
        NodeIndex Idx;
        AddNode(Idx, "base");
        AddNode(Idx, "patch", {"base"});
        AddLaunchable(Idx, "game", {"patch"});

        const auto S = Stack(ResolveOf(Idx, "game"));
        QCOMPARE(S.back(), std::string("game"));
        QVERIFY2(IndexOf(S, "base") < IndexOf(S, "patch"), "a contained node lies beneath its container");
        QVERIFY2(IndexOf(S, "patch") < IndexOf(S, "game"), "what the launchable contains lies beneath it");
    }

    // List order is the tie-break: a later NODE layer lies higher.
    void list_order_is_the_tie_break()
    {
        NodeIndex Idx;
        AddNode(Idx, "a");
        AddNode(Idx, "b");
        AddLaunchable(Idx, "game", {"a", "b"});

        const auto S = Stack(ResolveOf(Idx, "game"));
        QVERIFY2(IndexOf(S, "a") < IndexOf(S, "b"), "a later NODE layer must fold later, i.e. at higher priority");
    }

    // A diamond includes the shared node exactly once, beneath both containers.
    void a_shared_node_appears_once_and_early()
    {
        NodeIndex Idx;
        AddNode(Idx, "lib");
        AddNode(Idx, "l", {"lib"});
        AddNode(Idx, "r", {"lib"});
        AddLaunchable(Idx, "game", {"l", "r"});

        const auto S = Stack(ResolveOf(Idx, "game"));
        QCOMPARE(std::count(S.begin(), S.end(), std::string("lib")), 1L);
        QVERIFY(IndexOf(S, "lib") < IndexOf(S, "l"));
        QVERIFY(IndexOf(S, "lib") < IndexOf(S, "r"));
    }

    // A cycle must TERMINATE and be reported. An authoring mistake must not hang the launcher.
    void a_cycle_terminates_instead_of_hanging()
    {
        NodeIndex Idx;
        AddNode(Idx, "a", {"b"});
        AddNode(Idx, "b", {"a"});
        AddLaunchable(Idx, "game", {"a"});

        const auto P = ResolveOf(Idx, "game");
        QCOMPARE(Stack(P).back(), std::string("game"));
        QVERIFY2(HasEvent(P, "cycle", "a"), "a node inside itself is reported");
    }

    // A missing NODE ref must be REPORTED, not silently dropped — a typo'd dependency otherwise produces a package
    // that launches with a layer missing and no complaint.
    void a_missing_node_is_reported()
    {
        NodeIndex Idx;
        AddLaunchable(Idx, "game", {"nonexistent_dependency"});

        QVERIFY(HasEvent(ResolveOf(Idx, "game"), "missing", "nonexistent_dependency"));
        std::vector<std::string> Missing;
        ManifestModel::Closure(Idx, "game", &Missing);
        QVERIFY2(Contains(Missing, "nonexistent_dependency"), "the closure surfaces a dangling ref too");
    }

    void resolving_an_unknown_node_is_safe()
    {
        NodeIndex Idx;
        AddLaunchable(Idx, "game");
        const auto P = ResolveOf(Idx, "no_such_node");
        QVERIFY(P.Seq.empty());
        QVERIFY(ManifestModel::Closure(Idx, "no_such_node").empty() || !Contains(ManifestModel::Closure(Idx, "no_such_node"), "game"));
    }

    // A WHEN-gated NODE layer is the only optional containment: the node is contained exactly when its WHEN holds.
    void a_gated_node_layer_is_contained_when_its_when_holds()
    {
        NodeIndex Idx;
        AddNode(Idx, "hd_textures");
        Add(Idx, ordered_json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "game"}, {"LAYERS", ordered_json::array({
            { {"VARS", { {"hd", { {"DEFAULT", "0"}, {"UI", { {"CONTROL", "bool"} }} }} }} },
            { {"DIR", "game"} },
            { {"NODE", "hd_textures"}, {"WHEN", "%hd% == 1"} } })} });

        const auto Lib = ManifestModel::LibraryOf(Idx);
        QVERIFY(!Contains(Stack(Fold::Resolve(Lib, "game")), "hd_textures"));
        QVERIFY(Contains(Stack(Fold::Resolve(Lib, "game", {{"hd", "1"}})), "hd_textures"));
    }

    void validate_accepts_a_healthy_graph()
    {
        NodeIndex Idx;
        AddNode(Idx, "base");
        AddLaunchable(Idx, "game", {"base"});
        ManifestModel::DeriveFacts(Idx);

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        QVERIFY2(Errors.empty(), qPrintable(QString("unexpected errors: %1")
                 .arg(Errors.empty() ? QString() : QString::fromStdString(Errors.front()))));
    }

    void validate_reports_a_dangling_parent()
    {
        NodeIndex Idx;
        AddLaunchable(Idx, "game", {"ghost"});
        ManifestModel::DeriveFacts(Idx);

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        bool Mentions = false;
        for (const auto &E : Errors) if (E.find("ghost") != std::string::npos) Mentions = true;
        for (const auto &W : Warnings) if (W.find("ghost") != std::string::npos) Mentions = true;
        QVERIFY2(Mentions, "a NODE ref that does not exist must be reported by name");
    }

    void validate_reports_a_cycle()
    {
        NodeIndex Idx;
        AddNode(Idx, "a", {"b"});
        AddNode(Idx, "b", {"a"});
        AddLaunchable(Idx, "game", {"a"});
        ManifestModel::DeriveFacts(Idx);

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        QVERIFY2(!Errors.empty() || !Warnings.empty(), "a containment cycle must not validate silently");
    }

    // The %variable% lint: an undefined reference is almost always a typo that would survive into a real path or
    // command line. The engine-injected VIDYAGOD_* tokens must NOT be flagged — they are supplied per launch.
    void validate_flags_unknown_variables_but_not_engine_injected_ones()
    {
        NodeIndex Idx;
        auto WithEdit = [](const std::string &Id, const std::string &Value) {
            ordered_json J = NodeJson(Id, {}, true);
            J["LAYERS"].push_back({ {"EDIT", ordered_json::array({ { {"MODE", "Overwrite"}, {"VALUE", Value} } })},
                                    {"TARGET", "FILES/C:/cfg.ini"} });
            return J;
        };
        Add(Idx, WithEdit("game", "name=%VIDYAGOD_SELF_NAME%"));
        ManifestModel::DeriveFacts(Idx);

        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        for (const auto &E : Errors)
            QVERIFY2(E.find("VIDYAGOD_SELF_NAME") == std::string::npos,
                     "engine-injected session tokens must not be reported as undefined");

        Add(Idx, WithEdit("game2", "%DEFINITELY_NOT_A_REAL_TOKEN%"));
        ManifestModel::DeriveFacts(Idx);
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
        AddLaunchable(Idx, "good");
        AddLaunchable(Idx, "bad", {"ghost"});
        ManifestModel::DeriveFacts(Idx);

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
