#include "vgtest.h"
#include "manifestmodel.h"
#include "nodefixture.h"

#include <zip.h>

#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using nlohmann::ordered_json;

// Build a node from a JSON object via ParseNode (so these tests also cover ParseNode) and add it to Idx.
static void Add(NodeIndex &Idx, const ordered_json &J)
{
    Node N;
    if (ManifestModel::ParseNode(J, "f.json", "/bundle", N)) Idx.Nodes[N.NodeId] = N;
}

// A fixture "node with these layers" is now a CHAIN of one-layer nodes; the tail owns the bare id.
static void AddChain(NodeIndex &Idx, const std::string &Id, const std::vector<ordered_json> &Layers,
                     const std::vector<std::string> &Parents = {}, const ordered_json &Tail = ordered_json::object())
{
    for (const auto &N : NodeFixture::Chain(Id, Layers, Parents, Tail)) Add(Idx, N);
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
    CHECK(!ManifestModel::ParseNode(ordered_json{{"FOO", "bar"}}, "f.json", "/b", N));      // no NODE_ID
    CHECK(!ManifestModel::ParseNode(ordered_json{{"NODE_ID", ""}}, "f.json", "/b", N));           // empty NODE_ID
    //A node with an id but an UNLOWERABLE payload is now KEPT, carrying the reason, so validation can name it
    //and resolution can refuse to route through it — dropping it left a leaf mistake reported by nothing.
    CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID", "ok"}}, "f.json", "/b", N));    // no TYPE
    CHECK(!N.LowerError.empty());
    CHECK_EQ((int)N.Layers.size(), 0);
    CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID", "ok"}, {"TYPE", "Group"}}, "f.json", "/b", N));
    CHECK(!N.IsLaunchable() && !N.IsRunner());   // pure composition (no Declare* identity)
}

TEST(resolve_order_parents_before_launchable)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {});
    AddChain(Idx, "game", {}, {"base"});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "base"));
    CHECK(Contains(Order, "game"));
    CHECK(IndexOf(Order, "base") < IndexOf(Order, "game"));   // parent before launchable
    CHECK_EQ(Order.back(), std::string("game"));              // launchable last = highest priority
}

TEST(resolve_order_optional_gated_by_toggle)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {});
    AddChain(Idx, "opt",  {}, {}, {{"TOGGLE", "off"}});
    AddChain(Idx, "game", {}, {"base", "opt"});

    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {}), "opt"));                 // default off
    CHECK(Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", true}}), "opt"));     // toggled on
    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", false}}), "opt"));   // toggled off
}

TEST(resolve_order_exclude_is_mutually_exclusive)
{
    NodeIndex Idx;
    AddChain(Idx, "a",    {}, {}, {{"TOGGLE", "on"}, {"EXCLUDE", {"b"}}});
    AddChain(Idx, "b",    {}, {}, {{"TOGGLE", "on"}, {"EXCLUDE", {"a"}}});
    AddChain(Idx, "game", {}, {"a", "b"});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "a") != Contains(Order, "b"));   // exactly one of the mutually-exclusive pair
}

// Explicitly toggling a mutually-exclusive option ON wins over the conflicting DEFAULT-on option (the user's
// explicit choice isn't silently dropped in favour of the other's default).
TEST(resolve_order_explicit_toggle_beats_conflicting_default)
{
    NodeIndex Idx;
    AddChain(Idx, "hd",   {}, {}, {{"TOGGLE", "on"},  {"EXCLUDE", {"lo"}}});
    AddChain(Idx, "lo",   {}, {}, {{"TOGGLE", "off"}, {"EXCLUDE", {"hd"}}});
    AddChain(Idx, "game", {}, {"hd", "lo"});

    // Default: hd (default-on) kept, lo dropped.
    const auto Def = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Def, "hd") && !Contains(Def, "lo"));

    // Toggle lo ON → lo wins, hd dropped (was kept only by default).
    const auto Tog = ManifestModel::ResolveNodeOrder(Idx, "game", {{"lo", true}});
    CHECK(Contains(Tog, "lo") && !Contains(Tog, "hd"));
}

TEST(resolve_order_missing_parent_reported)
{
    NodeIndex Idx;
    AddChain(Idx, "game", {}, {"ghost"});
    std::vector<std::string> Missing;
    ManifestModel::ResolveNodeOrder(Idx, "game", {}, &Missing);
    CHECK(Contains(Missing, "ghost"));
}

TEST(optional_nodes_lists_toggleable_ancestors)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {});
    AddChain(Idx, "opt",  {}, {}, {{"TOGGLE", "on"}});
    AddChain(Idx, "game", {}, {"base", "opt"});
    bool SawOpt = false, SawBase = false;
    for (const Node *N : ManifestModel::OptionalNodes(Idx, "game"))
    {
        if (N->NodeId == "opt")  SawOpt = true;
        if (N->NodeId == "base") SawBase = true;
    }
    CHECK(SawOpt);
    CHECK(!SawBase);   // required content is not an "optional" node
}

TEST(tile_endpoints_chain_collapses_to_tip)
{
    // Minecraft-shaped: a CONTENT delta chain c1 ← c2 ← c3, with a content-FREE launchable wrapper per version
    // hanging off it (g1..g3). Every wrapper is a raw graph sink — the naive sink rule showed 903 "endpoints" —
    // but content-wise the chain nests, so the tile collapses to ONE endpoint: the variant owning the chain tip,
    // dominating all 3 versions.
    NodeIndex Idx;
    AddChain(Idx, "c1", {NodeFixture::Content("zip",   "a.zip",     "t")});
    AddChain(Idx, "c2", {NodeFixture::Content("delta", "b.vgdelta", "t")}, {"c1"});
    AddChain(Idx, "c3", {NodeFixture::Content("delta", "c.vgdelta", "t")}, {"c2"});
    AddChain(Idx, "g1", {NodeFixture::Exec("win32", "x")}, {"c1"});
    AddChain(Idx, "g2", {NodeFixture::Exec("win32", "x")}, {"c2"});
    AddChain(Idx, "g3", {NodeFixture::Exec("win32", "x")}, {"c3"});
    const auto Eps = ManifestModel::TileEndpoints(Idx, {"g1", "g2", "g3"});
    CHECK_EQ((int)Eps.size(), 1);
    CHECK_EQ((int)Eps[0].Ids.size(), 1);
    CHECK_EQ(Eps[0].Ids[0], std::string("g3"));      // the variant covering the whole content chain
    CHECK_EQ(Eps[0].LaunchableCount, 3);             // dominates all 3 versions (their content nests under it)
}

TEST(tile_endpoints_divergent_tips_and_optional_excluded)
{
    // AoE2-shaped: two editions with OWN content sharing a content base → two endpoints (base surfaces in neither).
    // An optional mod's private content (base2) must not surface an endpoint — optional subtrees belong to the
    // optional-content section.
    NodeIndex Idx;
    AddChain(Idx, "base",  {NodeFixture::Content("zip", "base.zip", "t")});
    AddChain(Idx, "aok",   {NodeFixture::Content("zip", "aok.zip", "t"), NodeFixture::Exec("win32", "x")}, {"base"});
    AddChain(Idx, "tc",    {NodeFixture::Content("zip", "tc.zip",  "t"), NodeFixture::Exec("win32", "x")}, {"base"});
    AddChain(Idx, "base2", {NodeFixture::Content("zip", "modbase.zip", "t")});
    AddChain(Idx, "mod",   {NodeFixture::Content("zip", "mod.zip", "t")}, {"base2"}, {{"TOGGLE", "on"}});
    AddChain(Idx, "game",  {NodeFixture::Exec("win32", "x")}, {"aok", "mod"});
    const auto Eps = ManifestModel::TileEndpoints(Idx, {"game", "tc"});
    bool SawGame = false, SawTc = false;
    for (const auto &E : Eps)
        for (const std::string &Id : E.Ids)
        {
            if (Id == "game") SawGame = true;        // covers the aok content tip
            if (Id == "tc")   SawTc   = true;        // covers the tc content tip
            CHECK(Id != "base");                     // shared base is depended-upon content
            CHECK(Id != "mod");                      // optional subtree → optionals section
            CHECK(Id != "base2");                    // reachable only through the optional mod
        }
    CHECK(SawGame);
    CHECK(SawTc);
    CHECK_EQ((int)Eps.size(), 2);
}

TEST(validate_flags_missing_parent_and_cycle)
{
    {   // missing parent → error
        NodeIndex Idx;
        AddChain(Idx, "game", {NodeFixture::Exec("win32", "g.exe")}, {"ghost"});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "ghost"));
    }
    {   // PARENTS cycle → error (and must not hang)
        NodeIndex Idx;
        AddChain(Idx, "a", {}, {"b"});
        AddChain(Idx, "b", {}, {"a"});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "cycle"));
    }
}

TEST(validate_flags_vfs_layer_without_path)
{
    NodeIndex Idx;
    AddChain(Idx, "c", {ordered_json{{"TYPE", "Content"}, {"FORM", "zip"}}});   // no PATH/SOURCE
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
        ordered_json Zj = NodeFixture::Content("zip", "c.zip"); Zj["NODE_ID"] = "z";
        Node N; CHECK(ManifestModel::ParseNode(Zj, "f.json", Dir.string(), N));
        NodeIndex Idx; Idx.Nodes["z"] = N;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "STORE"));
    }

    // Stored → clean.
    MakeZip((Dir / "s.zip").string(), /*Stored=*/true);
    {
        ordered_json Zj = NodeFixture::Content("zip", "s.zip"); Zj["NODE_ID"] = "z";
        Node N; CHECK(ManifestModel::ParseNode(Zj, "f.json", Dir.string(), N));
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
    AddChain(Idx, "d", {NodeFixture::Content("dir", "payload")});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "unzipped authoring layer"));
    CHECK(!AnyContains(Errors, "VFSDirLayer"));   // warning, never an error
}

// A runner's build comes from its PARENT content nodes; VFS layers on the runner node itself are silently ignored.
// Validation must warn so authors don't ship a runner with a never-mounted build.
// Replaces validate_warns_runner_self_layers. That warning guarded a footgun that is now UNREPRESENTABLE: a node
// is one layer of one TYPE, so a runner DECLARATION cannot also carry content, and there is no longer anywhere to
// put a VFS layer where it would be silently dropped. What used to be decided by node MEMBERSHIP — the runner
// node's own layers are prefix assembly, its parents' layers are the build — is now decided by the LAYER: a
// %variable% PATH resolves against the live runner mount at launch (assembly), a real on-disk path is importable
// build content. FIVE call sites ask that question, so it is one named function (IsRunnerBuildLayer) rather than
// a conjunction each site must remember — two sites once spelled only half of it and reported every proton
// runner as not installed, which greys out Play on every Windows game.
TEST(runtime_sourced_layer_separates_assembly_from_build)
{
    CHECK(ManifestModel::IsRuntimeSourcedLayer(NodeFixture::Content("dir", "%DefaultPfxDir%")));
    CHECK(ManifestModel::IsRuntimeSourcedLayer(NodeFixture::Content("dir", "%RunnerMount%/files/share/default_pfx")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(NodeFixture::Content("zip", "rt.zip")));       // real build content

    ordered_json ViaSource = NodeFixture::Content("zip", "rt.zip");                            // SOURCE.PATH wins
    ViaSource["SOURCE"] = ordered_json{{"PATH", "%RunnerMount%/x"}};
    CHECK(ManifestModel::IsRuntimeSourcedLayer(ViaSource));

    //A %variable% is a matched pair around a NAME. A doubly-URL-escaped filename has matched pairs around
    //digits, and calling that runtime-sourced excluded a real file from hydration, verification AND a runner's
    //build — the layer simply never arrived, with no diagnostic.
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(NodeFixture::Content("zip", "100%25%20done.zip")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(NodeFixture::Content("zip", "a%1%b.zip")));

    //And the rule the CALL SITES actually ask (IsVfsLayer AND NOT runtime-sourced), so a site that forgets
    //half of it cannot pass this file. NOTE it takes a LOWERED layer (VFSZipLayer), which is what Node::Layers
    //holds — a flat node's own "TYPE":"Content" is not a layer type. The call sites are pinned in
    //test_packagecatalog::prefix_assembly_content_is_not_the_runners_build.
    CHECK(ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSZipLayer"}, {"PATH", "rt.zip"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSDirLayer"}, {"PATH", "%DefaultPfxDir%"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "RegEdit"}}));

    Node R;
    ordered_json rj = NodeFixture::Runner("linux64", {"win64"}, "x"); rj["NODE_ID"] = "r";
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner());
    CHECK_EQ((int)R.Layers.size(), 1);          // the declaration, and nothing else — no content can ride along
}

// The two rules the FLAT model made checkable, and that replaced the "more than one Declare* layer on a node"
// family (now unrepresentable: a node is one layer of one TYPE, so those checks could never fire).
//
// A tile's UID keys saved state, settings and the content root inside the prefix. Missing, it fell back
// silently at launch and the user's saves moved to a different path with no diagnostic.
TEST(validate_errors_on_a_tile_with_no_uid)
{
    NodeIndex Idx;
    AddChain(Idx, "notile", {NodeFixture::Tile("")});                  // a tile, no UID
    AddChain(Idx, "g", {NodeFixture::Exec("win32", "g.exe")}, {"notile"});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "no UID"));

    NodeIndex Ok;                                                      // ...and a UID clears it
    AddChain(Ok, "tile", {NodeFixture::Tile("749")});
    AddChain(Ok, "g", {NodeFixture::Exec("win32", "g.exe")}, {"tile"});
    std::vector<std::string> E2, W2;
    ManifestModel::ValidateNodeGraph(Ok, E2, W2);
    CHECK(!AnyContains(E2, "no UID"));
}

// A launchable reaching TWO tiles is bound to whichever comes first in PARENTS order — an order the format
// says carries no meaning (I9). It shows under one game, keyed by that game's UID, and nothing reports that the
// other was dropped.
// NOTE the LinkGames() calls. BuildNodeIndex runs it, and it copies the tile's Meta onto every variant — so in
// the real pipeline every linked launchable satisfies Presentable(). An earlier version of this test built the
// index by hand, never ran LinkGames, and passed while the rule was DEAD against the actual binary: it asked
// "is this node presentable?" to mean "is this node a tile", and after linking that is true of everything.
// Run the same pass the runtime runs.
TEST(validate_errors_on_a_launchable_reaching_two_tiles)
{
    NodeIndex Idx;
    AddChain(Idx, "tile_a", {NodeFixture::Tile("1", "A")});
    AddChain(Idx, "tile_b", {NodeFixture::Tile("2", "B")});
    AddChain(Idx, "game", {NodeFixture::Exec("win32", "g.exe")}, {"tile_a", "tile_b"});
    ManifestModel::LinkGames(Idx);                      // as BuildNodeIndex does
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "more than one DeclareLibraryItem"));

    // Reached through a CHAIN rather than directly — the shape a real package has, and the one a
    // direct-parents-only check would miss.
    NodeIndex Deep;
    AddChain(Deep, "tile_a", {NodeFixture::Tile("1", "A")});
    AddChain(Deep, "tile_b", {NodeFixture::Tile("2", "B")});
    AddChain(Deep, "mid_a", {NodeFixture::Content("zip", "a.zip")}, {"tile_a"});
    AddChain(Deep, "mid_b", {NodeFixture::Content("zip", "b.zip")}, {"tile_b"});
    AddChain(Deep, "game", {NodeFixture::Exec("win32", "g.exe")}, {"mid_a", "mid_b"});
    ManifestModel::LinkGames(Deep);
    std::vector<std::string> E2, W2;
    ManifestModel::ValidateNodeGraph(Deep, E2, W2);
    CHECK(AnyContains(E2, "more than one DeclareLibraryItem"));

    // One tile reached by several routes is NOT ambiguous — it is the normal diamond.
    NodeIndex One;
    AddChain(One, "tile", {NodeFixture::Tile("1", "A")});
    AddChain(One, "mid1", {NodeFixture::Content("zip", "a.zip")}, {"tile"});
    AddChain(One, "mid2", {NodeFixture::Content("zip", "b.zip")}, {"tile"});
    AddChain(One, "game", {NodeFixture::Exec("win32", "g.exe")}, {"mid1", "mid2"});
    ManifestModel::LinkGames(One);
    std::vector<std::string> E3, W3;
    ManifestModel::ValidateNodeGraph(One, E3, W3);
    CHECK(!AnyContains(E3, "more than one DeclareLibraryItem"));
}

// A node whose payload cannot be lowered must be REPORTED, not silently deleted. Dropping it made a referrer
// dangle (loud) but left a LEAF mistake — an unknown FORM, a typo'd TYPE — reported by nothing at all:
// --validate-nodes printed a perfect package while a layer had vanished. The spec states the opposite rule.
TEST(validate_errors_on_a_node_whose_payload_cannot_be_lowered)
{
    Node N;
    // ParseNode KEEPS it (so it is in the graph to be named) but gives it no layers (so it can never apply).
    CHECK(ManifestModel::ParseNode(
        ordered_json{{"NODE_ID", "oops"}, {"TYPE", "Content"}, {"FORM", "tarball"}, {"PATH", "a.tar"}},
        "f.json", "/b", N));
    CHECK(!N.LowerError.empty());
    CHECK_EQ((int)N.Layers.size(), 0);

    NodeIndex Idx;
    Add(Idx, ordered_json{{"NODE_ID", "oops"}, {"TYPE", "Content"}, {"FORM", "tarball"}, {"PATH", "a.tar"}});
    CHECK_EQ((int)Idx.Nodes.size(), 1);                       // indexed, not vanished
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "tarball"));

    // ...and a launch that routes through it is REFUSED, rather than quietly applying nothing where the
    // author declared something.
    NodeIndex L;
    Add(L, ordered_json{{"NODE_ID", "bad"}, {"TYPE", "Content"}, {"FORM", "tarball"}, {"PATH", "a.tar"}});
    AddChain(L, "game", {NodeFixture::Exec("win32", "g.exe")}, {"bad"});
    std::vector<std::string> Miss;
    ManifestModel::ResolveNodeOrder(L, "game", {}, &Miss);   // Chain's TAIL owns the bare id
    CHECK(Contains(Miss, "bad"));
}

// An unknown TOGGLE value is refused, not guessed. `Default = (Toggle != "off")` means every typo — "of",
// "Off", "false" — reads as ON: the author asks for opt-in and ships opt-out, silently.
// Group emits NO layers, so a WHEN on it has nothing to be stamped on and evaporates in silence. Group is
// the most natural place to write a conditional module ("this module IS these parents"), and the whole
// subtree would then apply unconditionally — the exact failure the WHEN-at-the-tail work exists to kill.
TEST(validate_errors_on_a_when_that_gates_nothing)
{
    NodeIndex Idx;
    Add(Idx, ordered_json{{"NODE_ID","g"}, {"TYPE","Group"}, {"WHEN","%NETMODE% == host"},
                          {"PARENTS", ordered_json::array()}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "never evaluated"));

    NodeIndex Ok;                                   // ...and a Group with no WHEN is perfectly fine
    Add(Ok, ordered_json{{"NODE_ID","g"}, {"TYPE","Group"}, {"PARENTS", ordered_json::array()}});
    std::vector<std::string> E2, W2;
    ManifestModel::ValidateNodeGraph(Ok, E2, W2);
    CHECK(!AnyContains(E2, "never evaluated"));
}

TEST(validate_errors_on_an_unknown_toggle_value)
{
    Node N;
    CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID","t"}, {"TYPE","Group"}, {"TOGGLE","of"}},
                                   "f.json", "/b", N));
    CHECK(!N.LowerError.empty());

    for (const char *V : {"on", "off"})                    // ...and both legal values are accepted
    {
        Node Ok;
        CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID","t"}, {"TYPE","Group"}, {"TOGGLE", V}},
                                       "f.json", "/b", Ok));
        CHECK(Ok.LowerError.empty());
    }
}

TEST(validate_flags_asymmetric_exclude)
{
    NodeIndex Idx;
    AddChain(Idx, "a", {}, {}, {{"EXCLUDE", {"b"}}});   // a excludes b, but b does not exclude a
    AddChain(Idx, "b", {});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "symmetric"));
}

// ---- Role collapse: identity DERIVED from Declare* layers ----

TEST(declare_layers_derive_identity)
{
    Node N;
    ordered_json lj{{"NODE_ID","g"},{"TYPE","DeclareExec"},{"HOST","win32"},{"PATH","game.exe"},
                    {"LABEL","Vanilla"},{"RECOMMENDED",true},{"RUNNER","geproton_9_20_runner"}};
    CHECK(ManifestModel::ParseNode(lj, "f.json", "/b", N));
    CHECK(N.IsLaunchable()); CHECK(!N.IsRunner());
    CHECK_EQ(N.HostPlatform, std::string("win32"));
    CHECK_EQ(N.Exec.value("CONTENTPATH", std::string()), std::string("game.exe"));
    CHECK_EQ(N.Label, std::string("Vanilla")); CHECK(N.Recommended);
    CHECK_EQ(N.RecommendedRunner, std::string("geproton_9_20_runner"));   // DeclareExec.RUNNER → soft package-side runner

    Node R;
    // A runner is the SAME declaration with GUEST platforms — no GUEST ⇒ terminal ⇒ launchable.
    ordered_json rj = NodeFixture::Runner("linux64", {"win32","win64"}, "wine");
    rj["NODE_ID"] = "wine";
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner()); CHECK(!R.IsLaunchable());
    CHECK_EQ(R.HostPlatform, std::string("linux64"));
    CHECK_EQ((int)R.GuestPlatform.size(), 2);
    CHECK_EQ(R.Exec.value("EXECUTABLE", std::string()), std::string("wine"));

    Node L;
    ordered_json tj = NodeFixture::Tile("42", "My Game");
    tj["NODE_ID"] = "tile";
    CHECK(ManifestModel::ParseNode(tj, "f.json", "/b", L));
    CHECK(L.Presentable()); CHECK(!L.IsLaunchable());
    CHECK_EQ(L.Uid, std::string("42"));
    CHECK_EQ(L.Meta.value("TITLE", std::string()), std::string("My Game"));
}

TEST(link_games_groups_variants_under_tile)
{
    auto exec = [](const char *label){ ordered_json E = NodeFixture::Exec("win32","g.exe"); E["LABEL"] = label; return E; };
    NodeIndex Idx;
    // The tile is a pure-Meta node and the PARENT of its variants — the Minecraft/WC3 shape.
    AddChain(Idx, "mygame",    {NodeFixture::Tile("7", "My Game")});
    AddChain(Idx, "mygame_v1", {exec("v1")}, {"mygame"});
    AddChain(Idx, "mygame_v2", {exec("v2")}, {"mygame"});
    ManifestModel::LinkGames(Idx);
    const Node *v1 = Idx.Find("mygame_v1");
    CHECK(v1->IsLaunchable());
    CHECK_EQ(v1->GameKey(), std::string("mygame"));      // grouped under the tile by the PARENTS edge
    CHECK(v1->Presentable());                             // inherited the tile metadata
    CHECK_EQ(v1->Meta.value("TITLE", std::string()), std::string("My Game"));
    CHECK_EQ(v1->Uid, std::string("7"));
    CHECK(Idx.Find("mygame")->Presentable());            // the tile is presentable
    CHECK(!Idx.Find("mygame")->IsLaunchable());          // but not launchable itself
}

// Identity-layer composition is FIELD-LEVEL last-wins along the closure (ComposeAcrossClosure): a variant that carries
// its OWN partial DeclareLibraryItem overrides just that field of its tile and inherits the rest. Same merge as the
// launch-time DeclareExec composition.
TEST(declare_library_item_composes_field_level)
{
    NodeIndex Idx;
    AddChain(Idx, "game", {NodeFixture::Tile("7", "Base Title", {{"DEVELOPER","Acme"}})});
    // The variant carries a partial DeclareLibraryItem (overrides TITLE only) upstream of its DeclareExec.
    AddChain(Idx, "variant", {ordered_json{{"TYPE","DeclareLibraryItem"},{"TITLE","Override Title"}},
                              NodeFixture::Exec("win32","g.exe")}, {"game"});

    const nlohmann::ordered_json Meta = ManifestModel::ComposeAcrossClosure(Idx, "variant", {},
        [](const Node &N) -> const nlohmann::ordered_json * { return N.Presentable() ? &N.Meta : nullptr; });
    CHECK_EQ(Meta.value("TITLE", std::string()), std::string("Override Title"));   // variant wins on its field
    CHECK_EQ(Meta.value("DEVELOPER", std::string()), std::string("Acme"));         // inherited from the tile
    CHECK_EQ(Meta.value("UID", std::string()), std::string("7"));                  // inherited from the tile
}

// Cross-repo parity: the parent's NormalizeTargetPath and the FS's NormalizeVPath (layerspec.cpp) MUST agree
// on every slash-only case — target strings computed here have to match the FS's per-target base map. (The FS
// side never sees backslashes — its inputs are zip entry names — so the win-separator cases pin only ours.)
#include "layerspec.h"   // NormalizeVPath — linked from vgfs_core
TEST(normalize_target_path_parity_with_fs)
{
    const char *SlashCases[] = { "", "/", "a", "/a", "a/", "/a/b/", "a/b/c", "//x//", "a/b//" };
    for (const char *C : SlashCases)
        CHECK_EQ(ManifestModel::NormalizeTargetPath(C), NormalizeVPath(C));
    // Parent-only: windows separators normalize before the trim.
    CHECK_EQ(ManifestModel::NormalizeTargetPath("a\\b\\c\\"), std::string("a/b/c"));
    CHECK_EQ(ManifestModel::NormalizeTargetPath("\\pfx\\drive_c"), std::string("pfx/drive_c"));
}

// The cross-layer case-collision lint was memoized (LayerEntriesPrepared) after perf showed it was 87% of a
// 49-second --validate-nodes: every launchable re-lowered and re-joined every entry of every SHARED layer. A lint
// that gets fast by never firing is worse than a slow one, so this pins the detection itself: two dir layers on
// one node, colliding only in case, must produce the error — and the same content seen through TWO launchables
// (the sharing the cache exists for) must still dedupe to one report, not zero.
TEST(validate_flags_cross_layer_case_collision_after_memoization)
{
    namespace fs = std::filesystem;
    fs::path Dir = fs::temp_directory_path() / ("vgtest_case_" + std::to_string(::getpid()));
    fs::remove_all(Dir);
    fs::create_directories(Dir / "base" / "MAPS");
    fs::create_directories(Dir / "patch" / "maps");
    { std::ofstream F(Dir / "base" / "MAPS" / "level.dat");  F << "a"; }
    { std::ofstream F(Dir / "patch" / "maps" / "LEVEL.dat"); F << "b"; }

    NodeIndex Idx;
    auto AddAt = [&](const ordered_json &J) {
        Node N;
        if (ManifestModel::ParseNode(J, "f.json", Dir.string(), N)) Idx.Nodes[N.NodeId] = N;
    };
    auto AddChainAt = [&](const std::string &Id, const std::vector<ordered_json> &Layers,
                          const std::vector<std::string> &Parents) {
        for (const auto &N : NodeFixture::Chain(Id, Layers, Parents)) AddAt(N);
    };
    AddChainAt("shared", {NodeFixture::Content("dir", "base"), NodeFixture::Content("dir", "patch")}, {});
    AddChainAt("gameA",  {NodeFixture::Tile("cA", "A"), NodeFixture::Exec("win32", "base/MAPS/level.dat")}, {"shared"});
    AddChainAt("gameB",  {NodeFixture::Tile("cB", "B"), NodeFixture::Exec("win32", "base/MAPS/level.dat")}, {"shared"});

    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);

    int CaseReports = 0;
    for (const std::string &E : Errors)
        if (E.find("case conflict across layers") != std::string::npos) ++CaseReports;
    CHECK(CaseReports >= 1);                       // the lint still FIRES through the prepared-entry cache
    CHECK_EQ(CaseReports, 1);                      // and the shared content dedupes across launchables, not per-launchable spam

    fs::remove_all(Dir);
}
