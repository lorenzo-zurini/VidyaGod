#include "vgtest.h"
#include "manifestmodel.h"

#include <algorithm>
#include <vector>
#include <string>

using nlohmann::ordered_json;

// Build a node from a JSON object via ParseNode (so these tests also cover ParseNode) and add it to Idx.
static void Add(NodeIndex &Idx, const ordered_json &J)
{
    Node N;
    if (ManifestModel::ParseNode(J, "f.json", "/bundle", N)) Idx.Nodes[N.NodeId] = N;
}

static bool Contains(const std::vector<std::string> &V, const std::string &S)
{
    return std::find(V.begin(), V.end(), S) != V.end();
}
static int IndexOf(const std::vector<std::string> &V, const std::string &S)
{
    for (size_t i = 0; i < V.size(); ++i) if (V[i] == S) return (int)i;
    return -1;
}
static bool AnyContains(const std::vector<std::string> &V, const std::string &Needle)
{
    for (const auto &S : V) if (S.find(Needle) != std::string::npos) return true;
    return false;
}

TEST(parse_rejects_missing_node_id)
{
    Node N;
    CHECK(!ManifestModel::ParseNode(ordered_json{{"ROLE", "content"}}, "f.json", "/b", N));      // no NODE_ID
    CHECK(!ManifestModel::ParseNode(ordered_json{{"NODE_ID", ""}}, "f.json", "/b", N));           // empty NODE_ID
    CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID", "ok"}}, "f.json", "/b", N));
    CHECK_EQ(N.Role, std::string("content"));   // default role
}

TEST(resolve_order_parents_before_launchable)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}, {"ROLE", "content"}});
    Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", {"base"}}});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "base"));
    CHECK(Contains(Order, "game"));
    CHECK(IndexOf(Order, "base") < IndexOf(Order, "game"));   // parent before launchable
    CHECK_EQ(Order.back(), std::string("game"));              // launchable last = highest priority
}

TEST(resolve_order_optional_gated_by_toggle)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}, {"ROLE", "content"}});
    Add(Idx, {{"NODE_ID", "opt"},  {"ROLE", "content"}, {"OPTIONAL", true}, {"DEFAULT", false}});
    Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", {"base", "opt"}}});

    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {}), "opt"));                 // default off
    CHECK(Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", true}}), "opt"));     // toggled on
    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", false}}), "opt"));   // toggled off
}

TEST(resolve_order_exclude_is_mutually_exclusive)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "a"},    {"ROLE", "content"}, {"OPTIONAL", true}, {"DEFAULT", true}, {"EXCLUDE", {"b"}}});
    Add(Idx, {{"NODE_ID", "b"},    {"ROLE", "content"}, {"OPTIONAL", true}, {"DEFAULT", true}, {"EXCLUDE", {"a"}}});
    Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", {"a", "b"}}});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "a") != Contains(Order, "b"));   // exactly one of the mutually-exclusive pair
}

TEST(resolve_order_missing_parent_reported)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", {"ghost"}}});
    std::vector<std::string> Missing;
    ManifestModel::ResolveNodeOrder(Idx, "game", {}, &Missing);
    CHECK(Contains(Missing, "ghost"));
}

TEST(optional_nodes_lists_toggleable_ancestors)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}, {"ROLE", "content"}});
    Add(Idx, {{"NODE_ID", "opt"},  {"ROLE", "content"}, {"OPTIONAL", true}, {"DEFAULT", true}});
    Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PARENTS", {"base", "opt"}}});
    bool SawOpt = false, SawBase = false;
    for (const Node *N : ManifestModel::OptionalNodes(Idx, "game"))
    {
        if (N->NodeId == "opt")  SawOpt = true;
        if (N->NodeId == "base") SawBase = true;
    }
    CHECK(SawOpt);
    CHECK(!SawBase);   // required content is not an "optional" node
}

TEST(validate_flags_missing_parent_and_cycle)
{
    {   // missing parent → error
        NodeIndex Idx;
        Add(Idx, {{"NODE_ID", "game"}, {"ROLE", "launchable"}, {"PLATFORM", {{"HOST", "win32"}}}, {"PARENTS", {"ghost"}}});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "ghost"));
    }
    {   // PARENTS cycle → error (and must not hang)
        NodeIndex Idx;
        Add(Idx, {{"NODE_ID", "a"}, {"ROLE", "content"}, {"PARENTS", {"b"}}});
        Add(Idx, {{"NODE_ID", "b"}, {"ROLE", "content"}, {"PARENTS", {"a"}}});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "cycle"));
    }
}

TEST(validate_flags_vfs_layer_without_path)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "c"}, {"ROLE", "content"}, {"LAYERS", {{{"TYPE", "VFSZipLayer"}}}}});   // no PATH/SOURCE
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "PATH"));
}

TEST(validate_flags_asymmetric_exclude)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "a"}, {"ROLE", "content"}, {"EXCLUDE", {"b"}}});   // a excludes b, but b does not exclude a
    Add(Idx, {{"NODE_ID", "b"}, {"ROLE", "content"}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "symmetric"));
}
