#include "vgtest.h"
#include "manifestmodel.h"

#include <zip.h>

#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <unistd.h>

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

// A VFSZipLayer present on disk must be STORE (uncompressed) — VidyaGodFS cannot inflate DEFLATE, so a compressed
// zip mounts to garbage. Validation must flag it (regression for the "exit 0 on unmountable layer" footgun found
// via headless launch testing). Build real zips with libzip so the on-disk compression check has something to read.
static void MakeZip(const std::string &Path, bool Stored)
{
    static const char *Data = "vgtest payload aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";   // compressible
    int Err = 0;
    zip_t *Za = zip_open(Path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Err);
    CHECK(Za != nullptr);
    if (!Za) return;
    zip_source_t *S = zip_source_buffer(Za, Data, std::strlen(Data), 0);
    zip_int64_t Idx = zip_file_add(Za, "hello.sh", S, ZIP_FL_OVERWRITE);
    zip_set_file_compression(Za, Idx, Stored ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    zip_close(Za);
}

TEST(validate_flags_compressed_zip_layer)
{
    namespace fs = std::filesystem;
    fs::path Dir = fs::temp_directory_path() / ("vgtest_zip_" + std::to_string(::getpid()));
    fs::create_directories(Dir);

    // Compressed → flagged.
    MakeZip((Dir / "c.zip").string(), /*Stored=*/false);
    {
        Node N; ManifestModel::ParseNode({{"NODE_ID", "z"}, {"ROLE", "content"},
            {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "c.zip"}}}}}, "f.json", Dir.string(), N);
        NodeIndex Idx; Idx.Nodes["z"] = N;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "STORE"));
    }

    // Stored → clean.
    MakeZip((Dir / "s.zip").string(), /*Stored=*/true);
    {
        Node N; ManifestModel::ParseNode({{"NODE_ID", "z"}, {"ROLE", "content"},
            {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "s.zip"}}}}}, "f.json", Dir.string(), N);
        NodeIndex Idx; Idx.Nodes["z"] = N;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(!AnyContains(Errors, "STORE"));
    }

    std::error_code Ec; fs::remove_all(Dir, Ec);
}

// A VFSDirLayer is an unzipped authoring intermediary — warn (not error) so the node still test-runs from the
// editor, but the author is told to zip it before publishing (dir layers can't be seeded to IPFS).
TEST(validate_warns_unzipped_dir_layer)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "d"}, {"ROLE", "content"}, {"LAYERS", {{{"TYPE", "VFSDirLayer"}, {"PATH", "payload"}}}}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "unzipped authoring layer"));
    CHECK(!AnyContains(Errors, "VFSDirLayer"));   // warning, never an error
}

// A runner's build comes from its PARENT content nodes; VFS layers on the runner node itself are silently ignored.
// Validation must warn so authors don't ship a runner with a never-mounted build.
TEST(validate_warns_runner_self_layers)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "r"}, {"ROLE", "runner"},
              {"PLATFORM", {{"HOST", "linux64"}, {"GUEST", {"win64"}}}},
              {"EXEC", {{"EXECUTABLE", "x"}}},
              {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "rt.zip"}}}}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "runner layers are IGNORED"));
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
