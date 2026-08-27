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
    CHECK(!ManifestModel::ParseNode(ordered_json{{"FOO", "bar"}}, "f.json", "/b", N));      // no NODE_ID
    CHECK(!ManifestModel::ParseNode(ordered_json{{"NODE_ID", ""}}, "f.json", "/b", N));           // empty NODE_ID
    CHECK(ManifestModel::ParseNode(ordered_json{{"NODE_ID", "ok"}}, "f.json", "/b", N));
    CHECK(!N.IsLaunchable() && !N.IsRunner());   // a bare node is content (no Declare* identity)
}

TEST(resolve_order_parents_before_launchable)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"base"}}});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "base"));
    CHECK(Contains(Order, "game"));
    CHECK(IndexOf(Order, "base") < IndexOf(Order, "game"));   // parent before launchable
    CHECK_EQ(Order.back(), std::string("game"));              // launchable last = highest priority
}

TEST(resolve_order_optional_gated_by_toggle)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}});
    Add(Idx, {{"NODE_ID", "opt"},  {"OPTIONAL", true}, {"DEFAULT", false}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"base", "opt"}}});

    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {}), "opt"));                 // default off
    CHECK(Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", true}}), "opt"));     // toggled on
    CHECK(!Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", false}}), "opt"));   // toggled off
}

TEST(resolve_order_exclude_is_mutually_exclusive)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "a"},    {"OPTIONAL", true}, {"DEFAULT", true}, {"EXCLUDE", {"b"}}});
    Add(Idx, {{"NODE_ID", "b"},    {"OPTIONAL", true}, {"DEFAULT", true}, {"EXCLUDE", {"a"}}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"a", "b"}}});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "a") != Contains(Order, "b"));   // exactly one of the mutually-exclusive pair
}

// Explicitly toggling a mutually-exclusive option ON wins over the conflicting DEFAULT-on option (the user's
// explicit choice isn't silently dropped in favour of the other's default).
TEST(resolve_order_explicit_toggle_beats_conflicting_default)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "hd"},   {"OPTIONAL", true}, {"DEFAULT", true},  {"EXCLUDE", {"lo"}}});
    Add(Idx, {{"NODE_ID", "lo"},   {"OPTIONAL", true}, {"DEFAULT", false}, {"EXCLUDE", {"hd"}}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"hd", "lo"}}});

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
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"ghost"}}});
    std::vector<std::string> Missing;
    ManifestModel::ResolveNodeOrder(Idx, "game", {}, &Missing);
    CHECK(Contains(Missing, "ghost"));
}

TEST(optional_nodes_lists_toggleable_ancestors)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "base"}});
    Add(Idx, {{"NODE_ID", "opt"},  {"OPTIONAL", true}, {"DEFAULT", true}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"base", "opt"}}});
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
    Add(Idx, {{"NODE_ID", "c1"}, {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "a.zip"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "c2"}, {"PARENTS", {"c1"}}, {"LAYERS", {{{"TYPE", "VFSDeltaLayer"}, {"PATH", "b.vgdelta"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "c3"}, {"PARENTS", {"c2"}}, {"LAYERS", {{{"TYPE", "VFSDeltaLayer"}, {"PATH", "c.vgdelta"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "g1"}, {"PARENTS", {"c1"}}, {"LAYERS", {{{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
    Add(Idx, {{"NODE_ID", "g2"}, {"PARENTS", {"c2"}}, {"LAYERS", {{{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
    Add(Idx, {{"NODE_ID", "g3"}, {"PARENTS", {"c3"}}, {"LAYERS", {{{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
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
    Add(Idx, {{"NODE_ID", "base"}, {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "base.zip"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "aok"}, {"PARENTS", {"base"}}, {"LAYERS", {
        {{"TYPE", "VFSZipLayer"}, {"PATH", "aok.zip"}, {"TARGET", "t"}},
        {{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
    Add(Idx, {{"NODE_ID", "tc"},  {"PARENTS", {"base"}}, {"LAYERS", {
        {{"TYPE", "VFSZipLayer"}, {"PATH", "tc.zip"}, {"TARGET", "t"}},
        {{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
    Add(Idx, {{"NODE_ID", "base2"}, {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "modbase.zip"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "mod"}, {"OPTIONAL", true}, {"PARENTS", {"base2"}},
              {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "mod.zip"}, {"TARGET", "t"}}}}});
    Add(Idx, {{"NODE_ID", "game"}, {"PARENTS", {"aok", "mod"}}, {"LAYERS", {{{"TYPE", "DeclareExec"}, {"CONTENTPATH", "x"}}}}});
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
        Add(Idx, {{"NODE_ID", "game"}, {"LAYERS", {{{"TYPE", "DeclareExec"}, {"PLATFORM", "win32"}, {"CONTENTPATH", "g.exe"}}}}, {"PARENTS", {"ghost"}}});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "ghost"));
    }
    {   // PARENTS cycle → error (and must not hang)
        NodeIndex Idx;
        Add(Idx, {{"NODE_ID", "a"}, {"PARENTS", {"b"}}});
        Add(Idx, {{"NODE_ID", "b"}, {"PARENTS", {"a"}}});
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "cycle"));
    }
}

TEST(validate_flags_vfs_layer_without_path)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "c"}, {"LAYERS", {{{"TYPE", "VFSZipLayer"}}}}});   // no PATH/SOURCE
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
        Node N; ManifestModel::ParseNode({{"NODE_ID", "z"},
            {"LAYERS", {{{"TYPE", "VFSZipLayer"}, {"PATH", "c.zip"}}}}}, "f.json", Dir.string(), N);
        NodeIndex Idx; Idx.Nodes["z"] = N;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "STORE"));
    }

    // Stored → clean.
    MakeZip((Dir / "s.zip").string(), /*Stored=*/true);
    {
        Node N; ManifestModel::ParseNode({{"NODE_ID", "z"},
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
    Add(Idx, {{"NODE_ID", "d"}, {"LAYERS", {{{"TYPE", "VFSDirLayer"}, {"PATH", "payload"}}}}});
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
    Add(Idx, {{"NODE_ID", "r"},
              {"LAYERS", {{{"TYPE", "DeclareRunner"}, {"HOST", "linux64"}, {"GUEST", {"win64"}}, {"EXECUTABLE", "x"}},
                          {{"TYPE", "VFSZipLayer"}, {"PATH", "rt.zip"}}}}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "runner layers are IGNORED"));
}

TEST(validate_flags_asymmetric_exclude)
{
    NodeIndex Idx;
    Add(Idx, {{"NODE_ID", "a"}, {"EXCLUDE", {"b"}}});   // a excludes b, but b does not exclude a
    Add(Idx, {{"NODE_ID", "b"}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "symmetric"));
}

// ---- Role collapse: identity DERIVED from Declare* layers ----

TEST(declare_layers_derive_identity)
{
    Node N;
    ordered_json lj; lj["NODE_ID"] = "g";
    lj["LAYERS"] = ordered_json::array({ ordered_json{{"TYPE","DeclareExec"},{"PLATFORM","win32"},
                    {"CONTENTPATH","game.exe"},{"LABEL","Vanilla"},{"RECOMMENDED",true},{"RUNNER","geproton_9_20_runner"}} });
    CHECK(ManifestModel::ParseNode(lj, "f.json", "/b", N));
    CHECK(N.IsLaunchable()); CHECK(!N.IsRunner());
    CHECK_EQ(N.HostPlatform, std::string("win32"));
    CHECK_EQ(N.Exec.value("CONTENTPATH", std::string()), std::string("game.exe"));
    CHECK_EQ(N.Label, std::string("Vanilla")); CHECK(N.Recommended);
    CHECK_EQ(N.RecommendedRunner, std::string("geproton_9_20_runner"));   // DeclareExec.RUNNER → soft package-side runner

    Node R;
    ordered_json rj; rj["NODE_ID"] = "wine";
    rj["LAYERS"] = ordered_json::array({ ordered_json{{"TYPE","DeclareRunner"},{"HOST","linux64"},
                    {"GUEST", ordered_json::array({"win32","win64"})},{"EXECUTABLE","wine"}} });
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner()); CHECK(!R.IsLaunchable());
    CHECK_EQ(R.HostPlatform, std::string("linux64"));
    CHECK_EQ((int)R.GuestPlatform.size(), 2);
    CHECK_EQ(R.Exec.value("EXECUTABLE", std::string()), std::string("wine"));

    Node L;
    ordered_json tj; tj["NODE_ID"] = "tile";
    tj["LAYERS"] = ordered_json::array({ ordered_json{{"TYPE","DeclareLibraryItem"},{"UID","42"},{"TITLE","My Game"}} });
    CHECK(ManifestModel::ParseNode(tj, "f.json", "/b", L));
    CHECK(L.Presentable()); CHECK(!L.IsLaunchable());
    CHECK_EQ(L.Uid, std::string("42"));
    CHECK_EQ(L.Meta.value("TITLE", std::string()), std::string("My Game"));
}

TEST(link_games_groups_variants_under_tile)
{
    auto exec = [](const char *label){ return ordered_json{{"TYPE","DeclareExec"},{"PLATFORM","win32"},{"CONTENTPATH","g.exe"},{"LABEL",label}}; };
    NodeIndex Idx;
    { ordered_json g; g["NODE_ID"]="mygame"; g["LAYERS"]=ordered_json::array({ ordered_json{{"TYPE","DeclareLibraryItem"},{"UID","7"},{"TITLE","My Game"}} }); Add(Idx, g); }
    { ordered_json v; v["NODE_ID"]="mygame_v1"; v["PARENTS"]=ordered_json::array({"mygame"}); v["LAYERS"]=ordered_json::array({ exec("v1") }); Add(Idx, v); }
    { ordered_json v; v["NODE_ID"]="mygame_v2"; v["PARENTS"]=ordered_json::array({"mygame"}); v["LAYERS"]=ordered_json::array({ exec("v2") }); Add(Idx, v); }
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
    { ordered_json g; g["NODE_ID"]="game"; g["LAYERS"]=ordered_json::array({
        ordered_json{{"TYPE","DeclareLibraryItem"},{"UID","7"},{"TITLE","Base Title"},{"DEVELOPER","Acme"}} }); Add(Idx, g); }
    // The variant declares a DeclareExec AND a partial DeclareLibraryItem (overrides TITLE only).
    { ordered_json v; v["NODE_ID"]="variant"; v["PARENTS"]=ordered_json::array({"game"}); v["LAYERS"]=ordered_json::array({
        ordered_json{{"TYPE","DeclareLibraryItem"},{"TITLE","Override Title"}},
        ordered_json{{"TYPE","DeclareExec"},{"PLATFORM","win32"},{"CONTENTPATH","g.exe"}} }); Add(Idx, v); }

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
