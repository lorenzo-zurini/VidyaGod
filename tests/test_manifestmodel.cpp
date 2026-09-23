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

TEST(parse_requires_type_label_optional)
{
    Node N;
    // A node is an object carrying a node field (identity is the CID; LABEL is optional). A legacy TYPE object is
    // NOT a node — the library is migrated, never read two ways.
    CHECK(!ManifestModel::ParseNode(ordered_json{{"FOO", "bar"}}, "f.json", "/b", N));        // no node field ⇒ not a node
    CHECK(!ManifestModel::ParseNode(ordered_json{{"TYPE", "Group"}}, "f.json", "/b", N));     // legacy TYPE ⇒ not a node
    CHECK(!ManifestModel::IsNodeObject(ordered_json{{"TYPE", "VFSLayer"}, {"LAYERS", ordered_json::array()}}));
    // A LABEL alone makes a node; a nameless node with an OVER is still a node (identified by its CID).
    CHECK(ManifestModel::ParseNode(ordered_json{{"OVER", ordered_json::array({"x"})}}, "f.json", "/b", N));
    CHECK(N.NodeId.empty());
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL", "ok"}}, "f.json", "/b", N));
    CHECK(!N.IsLaunchable() && !N.IsRunner());   // a plain node
    CHECK(N.LowerError.empty());
    // A node with an UNLOWERABLE payload is KEPT, carrying the reason (validation names it, resolution refuses to
    // route through it).
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL", "bad"}, {"LAYERS", 5}}, "f.json", "/b", N));
    CHECK(!N.LowerError.empty());
}

// The one edge, parsed: a ref, an any-of group, a NOT. Anything else is refused by name.
TEST(parse_over_is_cnf)
{
    std::vector<OverReq> R; std::string Err;
    CHECK(ManifestModel::ParseOver(ordered_json::array({"a", ordered_json::array({"b", "c"}), ordered_json{{"NOT", "d"}}}), R, &Err));
    CHECK_EQ((int)R.size(), 3);
    CHECK(!R[0].IsGroup() && !R[0].Not && R[0].Any == std::vector<std::string>{"a"});
    CHECK(R[1].IsGroup() && R[1].Any == (std::vector<std::string>{"b", "c"}));
    CHECK(R[2].Not && R[2].Any == std::vector<std::string>{"d"});
    CHECK(!ManifestModel::ParseOver(ordered_json::array({ordered_json::array()}), R, &Err));           // empty group
    CHECK(!ManifestModel::ParseOver(ordered_json::array({ordered_json{{"NOT", "d"}, {"X", 1}}}), R, &Err)); // not exactly {NOT}
    CHECK(!ManifestModel::ParseOver(ordered_json::array({5}), R, &Err));
    CHECK(!ManifestModel::ParseOver(ordered_json::array({""}), R, &Err));
    CHECK(!ManifestModel::ParseOver(ordered_json("a"), R, &Err));                                          // not a list

    // ParseNode flattens: Parents = every positive ref in order, Excludes = the NOTs.
    Node N;
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL", "n"}, {"OVER", ordered_json::array({"a", ordered_json::array({"b", "c"}), ordered_json{{"NOT", "d"}}})}}, "f.json", "/b", N));
    CHECK(N.Parents == (std::vector<std::string>{"a", "b", "c"}));
    CHECK(N.Excludes == std::vector<std::string>{"d"});
    // ...and a malformed OVER is a refusal that still carries the readable refs (a scoped validate can reach it).
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL", "n"}, {"OVER", ordered_json::array({5})}}, "f.json", "/b", N));
    CHECK(!N.LowerError.empty());

    // OverRefs / RemapOverRefs walk the RAW JSON the same way (freeze, hydrate and the CID write-back use them).
    ordered_json Raw{{"OVER", ordered_json::array({"a", ordered_json::array({"b", "c"}), ordered_json{{"NOT", "d"}}})}};
    std::vector<std::string> Nots;
    CHECK(ManifestModel::OverRefs(Raw, &Nots) == (std::vector<std::string>{"a", "b", "c"}));
    CHECK(Nots == std::vector<std::string>{"d"});
    CHECK(ManifestModel::RemapOverRefs(Raw, [](const std::string &X) { return X == "c" ? std::string("C") : (X == "d" ? std::string("D") : X); }));
    CHECK(Raw["OVER"][1][1] == "C");
    CHECK(Raw["OVER"][2]["NOT"] == "D");
    CHECK(!ManifestModel::RemapOverRefs(Raw, [](const std::string &X) { return X; }));   // nothing changed ⇒ false
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

static ordered_json Not(const char *Ref) { return ordered_json::array({ ordered_json{{"NOT", Ref}} }); }

TEST(resolve_order_exclude_is_mutually_exclusive)
{
    NodeIndex Idx;
    AddChain(Idx, "a",    {}, {}, {{"TOGGLE", "on"}, {"OVER", Not("b")}});
    AddChain(Idx, "b",    {}, {}, {{"TOGGLE", "on"}, {"OVER", Not("a")}});
    AddChain(Idx, "game", {}, {"a", "b"});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "a") != Contains(Order, "b"));   // exactly one of the mutually-exclusive pair

    // A NOT is ONE-SIDED in the file and symmetric in effect: only `x` says NOT y, and still at most one is kept.
    NodeIndex One;
    AddChain(One, "x",    {}, {}, {{"TOGGLE", "on"}, {"OVER", Not("y")}});
    AddChain(One, "y",    {}, {}, {{"TOGGLE", "on"}});
    AddChain(One, "game", {}, {"y", "x"});                  // y first in OVER order
    const auto O2 = ManifestModel::ResolveNodeOrder(One, "game", {});
    CHECK(Contains(O2, "y") && !Contains(O2, "x"));         // y kept first; x excludes a kept node ⇒ dropped
}

// An any-of group on a requirement: an already-kept member satisfies it, else an explicitly toggled-on one,
// else the first member the index has — deterministic, and never two.
TEST(resolve_order_any_of_group_picks_one_member)
{
    NodeIndex Idx;
    AddChain(Idx, "v640", {});
    AddChain(Idx, "v659", {});
    AddChain(Idx, "game", {}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"v640", "v659"}) })}});
    const auto Def = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Def, "v640") && !Contains(Def, "v659"));                       // first present member
    const auto Tog = ManifestModel::ResolveNodeOrder(Idx, "game", {{"v659", true}});
    CHECK(Contains(Tog, "v659") && !Contains(Tog, "v640"));                       // an explicit choice wins
    // A member already kept through another route satisfies the group without a second pick.
    NodeIndex Two;
    AddChain(Two, "v640", {});
    AddChain(Two, "v659", {});
    AddChain(Two, "game", {}, {}, {{"OVER", ordered_json::array({"v659", ordered_json::array({"v640", "v659"})})}});
    const auto Kept = ManifestModel::ResolveNodeOrder(Two, "game", {});
    CHECK(Contains(Kept, "v659") && !Contains(Kept, "v640"));
    // A group with NO member present is reported missing, not silently skipped.
    NodeIndex None;
    AddChain(None, "game", {}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"ghost1", "ghost2"}) })}});
    std::vector<std::string> Missing;
    ManifestModel::ResolveNodeOrder(None, "game", {}, &Missing);
    CHECK(Contains(Missing, "ghost1"));

    // A pick the gate REFUSES is not the pick: the next member is tried. v640 toggled off by the user (or
    // excluded by a kept node) ⇒ v659 mounts — never a closure with NEITHER member and no word said.
    NodeIndex Off;
    AddChain(Off, "v640", {}, {}, {{"TOGGLE", "on"}});
    AddChain(Off, "v659", {});
    AddChain(Off, "game", {}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"v640", "v659"}) })}});
    std::vector<std::string> M2;
    const auto Fallback = ManifestModel::ResolveNodeOrder(Off, "game", {{"v640", false}}, &M2);
    CHECK(!Contains(Fallback, "v640") && Contains(Fallback, "v659") && M2.empty());
    NodeIndex Excl;
    AddChain(Excl, "v640", {});
    AddChain(Excl, "v659", {});
    AddChain(Excl, "purist", {}, {}, {{"OVER", Not("v640")}});
    AddChain(Excl, "game", {}, {}, {{"OVER", ordered_json::array({"purist", ordered_json::array({"v640", "v659"})})}});
    const auto Excluded = ManifestModel::ResolveNodeOrder(Excl, "game", {}, &M2);
    CHECK(Contains(Excluded, "purist") && !Contains(Excluded, "v640") && Contains(Excluded, "v659") && M2.empty());
    // Every present member refused ⇒ MISSING, loudly.
    AddChain(Excl, "purist2", {}, {}, {{"OVER", Not("v659")}});
    AddChain(Excl, "game2", {}, {}, {{"OVER", ordered_json::array({"purist", "purist2", ordered_json::array({"v640", "v659"})})}});
    std::vector<std::string> M3;
    const auto NoneKept = ManifestModel::ResolveNodeOrder(Excl, "game2", {}, &M3);
    CHECK(!Contains(NoneKept, "v640") && !Contains(NoneKept, "v659") && Contains(M3, "v640"));
}

// Explicitly toggling a mutually-exclusive option ON wins over the conflicting DEFAULT-on option (the user's
// explicit choice isn't silently dropped in favour of the other's default).
TEST(resolve_order_explicit_toggle_beats_conflicting_default)
{
    NodeIndex Idx;
    AddChain(Idx, "hd",   {}, {}, {{"TOGGLE", "on"},  {"OVER", Not("lo")}});
    AddChain(Idx, "lo",   {}, {}, {{"TOGGLE", "off"}, {"OVER", Not("hd")}});
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
    AddChain(Idx, "c", {ordered_json{{"LAYERS", ordered_json::array({
        ordered_json{{"FORM", "zip"}}})}}});   // no PATH/SOURCE
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
        ordered_json Zj = NodeFixture::Content("zip", "c.zip"); Zj["LABEL"] = "z";
        Node N; CHECK(ManifestModel::ParseNode(Zj, "f.json", Dir.string(), N));
        NodeIndex Idx; Idx.Nodes["z"] = N;
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
        CHECK(AnyContains(Errors, "STORE"));
    }

    // Stored → clean.
    MakeZip((Dir / "s.zip").string(), /*Stored=*/true);
    {
        ordered_json Zj = NodeFixture::Content("zip", "s.zip"); Zj["LABEL"] = "z";
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
    // IsRuntimeSourcedLayer takes a LOWERED layer (VFSDirLayer/VFSZipLayer with a top-level PATH), which is what
    // Node::Layers holds — not a VFSLayer NODE (whose PATH lives inside a LAYERS entry).
    auto Lyr = [](const char *T, const std::string &P) { return ordered_json{{"TYPE", T}, {"PATH", P}}; };
    CHECK(ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSDirLayer", "%DefaultPfxDir%")));
    CHECK(ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSDirLayer", "%RunnerMount%/files/share/default_pfx")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "rt.zip")));                // real build content

    ordered_json ViaSource = Lyr("VFSZipLayer", "rt.zip");                                     // SOURCE.PATH wins
    ViaSource["SOURCE"] = ordered_json{{"PATH", "%RunnerMount%/x"}};
    CHECK(ManifestModel::IsRuntimeSourcedLayer(ViaSource));

    //A %variable% is a matched pair around a NAME. A doubly-URL-escaped filename has matched pairs around
    //digits, and calling that runtime-sourced excluded a real file from hydration, verification AND a runner's
    //build — the layer simply never arrived, with no diagnostic.
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "100%25%20done.zip")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "a%1%b.zip")));

    //And the rule the CALL SITES actually ask (IsVfsLayer AND NOT runtime-sourced), so a site that forgets
    //half of it cannot pass this file. NOTE it takes a LOWERED layer (VFSZipLayer), which is what Node::Layers
    //holds — a flat node's own "TYPE":"Content" is not a layer type. The call sites are pinned in
    //test_packagecatalog::prefix_assembly_content_is_not_the_runners_build.
    CHECK(ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSZipLayer"}, {"PATH", "rt.zip"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSDirLayer"}, {"PATH", "%DefaultPfxDir%"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "RegEdit"}}));

    Node R;
    ordered_json rj = NodeFixture::Runner("linux64", {"win64"}, "x"); rj["LABEL"] = "r";
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner());
    CHECK_EQ((int)R.Layers.size(), 0);          // ENTRYPOINTS is a facet: it lowers to no layer at all
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

// One UID = one card, and the nesting INSIDE the card is derived from the chain, never declared: the main is
// the launchable OVER no other of its UID; a different tile OVER it is a child (an expansion); the same tile OVER
// it is a variant. The card reads its title off the front of the ordered group; the recommended edition may be
// a child. Two mains in one UID (two independent installs) is legitimate and merely noted.
TEST(card_nesting_is_derived_from_the_chain)
{
    NodeIndex Idx;
    ordered_json Aok  = NodeFixture::Merge({NodeFixture::Content("zip", "aok.zip"), NodeFixture::Exec("win32", "e.exe"), NodeFixture::Tile("749", "Age of Kings")});
    ordered_json Conq = NodeFixture::Merge({NodeFixture::Content("zip", "x1.zip"), NodeFixture::Exec("win32", "x1.exe"), NodeFixture::Tile("749", "The Conquerors")});
    Conq["ENTRYPOINTS"][0]["RECOMMENDED"] = true;
    ordered_json Fe   = NodeFixture::Merge({NodeFixture::Content("zip", "fe.zip"), NodeFixture::Exec("win32", "fe.exe"), NodeFixture::Tile("749", "Forgotten Empires")});
    ordered_json Hd   = NodeFixture::Merge({NodeFixture::Content("zip", "hd.zip"), NodeFixture::Exec("win32", "e.exe"), NodeFixture::Tile("749", "Age of Kings")});   // same tile = a variant
    AddChain(Idx, "aok",  {Aok});
    AddChain(Idx, "conq", {Conq}, {"aok"});
    AddChain(Idx, "fe",   {Fe},   {"conq"});
    AddChain(Idx, "hd",   {Hd},   {"aok"});
    AddChain(Idx, "mod",  {NodeFixture::Content("dir", "m")}, {"conq"});         // not a launchable: never in the group
    ManifestModel::DeriveIdentity(Idx);
    CHECK_EQ(ManifestModel::SameTitleDepth(Idx, *Idx.Find("aok")),  0);
    CHECK_EQ(ManifestModel::SameTitleDepth(Idx, *Idx.Find("conq")), 1);
    CHECK_EQ(ManifestModel::SameTitleDepth(Idx, *Idx.Find("fe")),   2);
    CHECK(ManifestModel::SameTile(*Idx.Find("hd"), *Idx.Find("aok")));
    CHECK(!ManifestModel::SameTile(*Idx.Find("conq"), *Idx.Find("aok")));
    std::vector<const Node *> G{Idx.Find("fe"), Idx.Find("conq"), Idx.Find("hd"), Idx.Find("aok")};
    const auto O = ManifestModel::OrderVariants(Idx, G);
    std::vector<std::string> Ids; for (const Node *N : O) Ids.push_back(N->NodeId);
    CHECK(Ids == (std::vector<std::string>{"aok", "hd", "conq", "fe"}));      // main's tile first, then children in chain order
    CHECK_EQ(O.front()->Meta.value("TITLE", std::string()), std::string("Age of Kings"));   // the card's name
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(!AnyContains(Warnings, "TITLE differs") && !AnyContains(Warnings, "mains"));       // an expansion is not a lint
    // Two mains: a standalone install sharing the UID but OVER nothing of it.
    AddChain(Idx, "custom", {NodeFixture::Merge({NodeFixture::Content("zip", "c.zip"), NodeFixture::Exec("win32", "c.exe"), NodeFixture::Tile("749", "Custom Edition")})});
    ManifestModel::DeriveIdentity(Idx);
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "2 mains"));
}

// Validation names the OVER-specific mistakes: a group with no member in the library, a ref both required and
// excluded, a missing NOT target (harmless — a word, not an error).
TEST(validate_flags_over_mistakes)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {});
    AddChain(Idx, "g1", {}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"ghost1", "ghost2"}) })}});
    AddChain(Idx, "g2", {}, {}, {{"OVER", ordered_json::array({"base", ordered_json{{"NOT", "base"}}})}});
    AddChain(Idx, "g3", {}, {}, {{"OVER", ordered_json::array({ ordered_json{{"NOT", "ghost"}} })}});
    // A group with ONE member present resolves (it is a choice), so an absent member is a warning, never an error.
    AddChain(Idx, "g4", {}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"base", "ghost3"}) })}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "unsatisfiable"));
    CHECK(AnyContains(Errors, "both requires and excludes"));
    CHECK(AnyContains(Warnings, "NOT references missing"));
    CHECK(!AnyContains(Errors, "NOT references missing"));
    CHECK(AnyContains(Warnings, "any-of member 'ghost3'"));
    CHECK(!AnyContains(Errors, "ghost3"));
    CHECK(!AnyContains(Errors, "missing node 'ghost1'"));                   // members of an unsatisfiable group: one error, not three
}

// Identity follows the edge. A node with its own TILE IS its identity; every other node inherits the UNION of
// its requirements' identities, in OVER order (a mod for two games belongs to both; a group contributes every
// member). A node reaching no tile has no identity — it is substance (a library), never a card.
TEST(derive_identity_follows_over)
{
    NodeIndex Idx;
    AddChain(Idx, "aok",  {NodeFixture::Merge({NodeFixture::Exec("win32", "a.exe"), NodeFixture::Tile("1", "AoK")})});
    AddChain(Idx, "conq", {NodeFixture::Merge({NodeFixture::Exec("win32", "c.exe"), NodeFixture::Tile("2", "Conquerors")})}, {"aok"});
    AddChain(Idx, "mod_a",    {NodeFixture::Content("dir", "m")}, {"aok"});
    AddChain(Idx, "mod_both", {NodeFixture::Content("dir", "m")}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"aok", "conq"}) })}});
    AddChain(Idx, "mod_deep", {NodeFixture::Content("dir", "m")}, {"mod_a"});
    AddChain(Idx, "lib",      {NodeFixture::Content("dir", "l")});
    AddChain(Idx, "enh",      {NodeFixture::Content("dir", "e")}, {"aok", "lib"});
    AddChain(Idx, "mod_conq", {NodeFixture::Content("dir", "c")}, {"conq"});
    ManifestModel::DeriveIdentity(Idx);
    CHECK_EQ(Idx.Find("conq")->Uid, std::string("2"));                            // own tile wins over what is under it
    CHECK(Idx.Find("conq")->Uids == std::vector<std::string>{"2"});
    // ...and that boundary HOLDS through inheritance: a mod OVER the expansion belongs to the expansion only —
    // the base game under it contributes nothing, or every Conquerors mod would show up under Age of Kings.
    CHECK(Idx.Find("mod_conq")->Uids == std::vector<std::string>{"2"});
    CHECK_EQ(Idx.Find("mod_a")->Uid, std::string("1"));
    CHECK(Idx.Find("mod_both")->Uids == (std::vector<std::string>{"1", "2"}));  // both, in OVER order
    CHECK_EQ(Idx.Find("mod_deep")->Uid, std::string("1"));                        // transitively
    CHECK_EQ(Idx.Find("mod_deep")->Meta.value("TITLE", std::string()), std::string("AoK"));   // the tile's fields ride along
    CHECK(!Idx.Find("lib")->HasIdentity());                                       // substance
    CHECK(Idx.Find("enh")->Uids == std::vector<std::string>{"1"});                // a substance requirement adds nothing
    CHECK_EQ(Idx.Find("aok")->GameKey(), std::string("1"));                       // the group-by key is the UID
}

// A node whose payload cannot be lowered must be REPORTED, not silently deleted. Dropping it made a referrer
// dangle (loud) but left a LEAF mistake — an unknown FORM, a typo'd TYPE — reported by nothing at all:
// --validate-nodes printed a perfect package while a layer had vanished. The spec states the opposite rule.
TEST(validate_errors_on_a_node_whose_payload_cannot_be_lowered)
{
    Node N;
    // ParseNode KEEPS it (so it is in the graph to be named) but gives it no layers (so it can never apply).
    CHECK(ManifestModel::ParseNode(
        ordered_json{{"LABEL", "oops"}, {"LAYERS", ordered_json::array({ordered_json{{"FORM", "tarball"}, {"PATH", "a.tar"}}})}},
        "f.json", "/b", N));
    CHECK(!N.LowerError.empty());
    CHECK_EQ((int)N.Layers.size(), 0);

    NodeIndex Idx;
    Add(Idx, ordered_json{{"LABEL", "oops"}, {"LAYERS", ordered_json::array({ordered_json{{"FORM", "tarball"}, {"PATH", "a.tar"}}})}});
    CHECK_EQ((int)Idx.Nodes.size(), 1);                       // indexed, not vanished
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "tarball"));

    // ...and a launch that routes through it is REFUSED, rather than quietly applying nothing where the
    // author declared something.
    NodeIndex L;
    Add(L, ordered_json{{"LABEL", "bad"}, {"LAYERS", ordered_json::array({ordered_json{{"FORM", "tarball"}, {"PATH", "a.tar"}}})}});
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
    Add(Idx, ordered_json{{"LABEL","g"}, {"WHEN","%NETMODE% == host"},
                          {"OVER", ordered_json::array()}});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "never evaluated"));

    NodeIndex Ok;                                   // ...and a Group with no WHEN is perfectly fine
    Add(Ok, ordered_json{{"LABEL","g"}, {"OVER", ordered_json::array()}});
    std::vector<std::string> E2, W2;
    ManifestModel::ValidateNodeGraph(Ok, E2, W2);
    CHECK(!AnyContains(E2, "never evaluated"));
}

TEST(validate_errors_on_an_unknown_toggle_value)
{
    Node N;
    CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL","t"}, {"TOGGLE","of"}},
                                   "f.json", "/b", N));
    CHECK(!N.LowerError.empty());

    for (const char *V : {"on", "off"})                    // ...and both legal values are accepted
    {
        Node Ok;
        CHECK(ManifestModel::ParseNode(ordered_json{{"LABEL","t"}, {"TOGGLE", V}},
                                       "f.json", "/b", Ok));
        CHECK(Ok.LowerError.empty());
    }
}

// A launchable with no identity at all appears under no card — worth a warning; a runner legitimately has none.
TEST(validate_warns_on_a_launchable_with_no_identity)
{
    NodeIndex Idx;
    AddChain(Idx, "lost", {NodeFixture::Exec("win32", "g.exe")});
    AddChain(Idx, "wine", {NodeFixture::Runner("linux64", {"win32"}, "wine")});
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "no identity"));
    int N = 0; for (const auto &W : Warnings) if (W.find("no identity") != std::string::npos) ++N;
    CHECK_EQ(N, 1);                                                    // the runner is not warned about
}

// ---- Role collapse: identity DERIVED from Declare* layers ----

TEST(declare_layers_derive_identity)
{
    Node N;
    ordered_json lj{{"ENTRYPOINTS", ordered_json::array({ ordered_json{{"HOST","win32"},{"PATH","game.exe"},
                    {"LABEL","Vanilla"},{"RECOMMENDED",true},{"RUNNER","geproton_9_20_runner"}} })}};
    CHECK(ManifestModel::ParseNode(lj, "f.json", "/b", N));
    CHECK(N.IsLaunchable()); CHECK(!N.IsRunner());
    CHECK_EQ(N.HostPlatform, std::string("win32"));
    CHECK_EQ(N.Exec.value("CONTENTPATH", std::string()), std::string("game.exe"));
    CHECK_EQ(N.Label, std::string("Vanilla")); CHECK(N.Recommended);
    CHECK_EQ(N.RecommendedRunner, std::string("geproton_9_20_runner"));   // the entry's RUNNER → soft package-side runner

    Node R;
    // A runner is the SAME declaration with GUEST platforms — no GUEST ⇒ terminal ⇒ launchable.
    ordered_json rj = NodeFixture::Runner("linux64", {"win32","win64"}, "wine");
    rj["LABEL"] = "wine";
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner()); CHECK(!R.IsLaunchable());
    CHECK_EQ(R.HostPlatform, std::string("linux64"));
    CHECK_EQ((int)R.GuestPlatform.size(), 2);
    CHECK_EQ(R.Exec.value("EXECUTABLE", std::string()), std::string("wine"));

    Node L;
    ordered_json tj = NodeFixture::Tile("42", "My Game");
    tj["LABEL"] = "tile";
    CHECK(ManifestModel::ParseNode(tj, "f.json", "/b", L));
    CHECK(L.Presentable()); CHECK(!L.IsLaunchable()); CHECK(L.OwnTile);
    CHECK_EQ(L.Uid, std::string("42"));
    CHECK_EQ(L.Meta.value("TITLE", std::string()), std::string("My Game"));
}

TEST(link_games_groups_variants_under_tile)
{
    auto exec = [](const char *label){ ordered_json E = NodeFixture::Exec("win32","g.exe"); E["ENTRYPOINTS"][0]["LABEL"] = label; return E; };
    NodeIndex Idx;
    // The tile-carrying node is a plain node UNDER its variants — the Minecraft/WC3 shape: identity is inherited
    // through OVER, and the launcher groups the variants by UID.
    AddChain(Idx, "mygame",    {NodeFixture::Tile("7", "My Game")});
    AddChain(Idx, "mygame_v1", {exec("v1")}, {"mygame"});
    AddChain(Idx, "mygame_v2", {exec("v2")}, {"mygame"});
    ManifestModel::DeriveIdentity(Idx);
    const Node *v1 = Idx.Find("mygame_v1");
    CHECK(v1->IsLaunchable());
    CHECK_EQ(v1->GameKey(), std::string("7"));           // grouped under the tile by UID
    CHECK(v1->Presentable());                             // inherited the tile metadata
    CHECK_EQ(v1->Meta.value("TITLE", std::string()), std::string("My Game"));
    CHECK_EQ(v1->Uid, std::string("7"));
    CHECK_EQ(v1->Label, std::string("v1"));
    CHECK(Idx.Find("mygame")->Presentable());            // the tile node is presentable
    CHECK(!Idx.Find("mygame")->IsLaunchable());          // but not launchable itself
    // ...and a launchable carrying its OWN TILE is its own card, with its own fields — nothing composes across
    // the closure (a version under it contributes no identity, no exec, nothing).
    AddChain(Idx, "mygame_v3", {NodeFixture::Merge({exec("v3"), NodeFixture::Tile("8", "Other")})}, {"mygame_v2"});
    ManifestModel::DeriveIdentity(Idx);
    CHECK_EQ(Idx.Find("mygame_v3")->Uid, std::string("8"));
    CHECK_EQ(Idx.Find("mygame_v3")->Meta.value("TITLE", std::string()), std::string("Other"));
    CHECK_EQ(Idx.Find("mygame_v3")->Label, std::string("v3"));
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

//ENV is a BAG: composing a variant's ENV over a base's must keep the base's other keys. Previously the whole
//object was replaced, which deleted them — harmless only while nothing consumed a launchable's ENV.
// Execution is NOT transitive: the launch exec is the SELECTED entrypoint of the launch node and nothing under
// it. A version under it (1.16.5 OVER 1.16.4) contributes no exec, no ENV, no args.
TEST(exec_is_the_selected_entrypoint_and_nothing_under_it)
{
    NodeIndex Idx;
    ordered_json Old = NodeFixture::Exec("win32", "old.exe");
    Old["ENTRYPOINTS"][0]["ENV"] = ordered_json{{"FROM_OLD", "1"}};
    AddChain(Idx, "v1", {Old});
    ordered_json New = NodeFixture::Exec("win32", "new.exe");
    New["ENTRYPOINTS"][0]["ENV"] = ordered_json{{"FROM_NEW", "1"}};
    New["ENTRYPOINTS"].push_back(ordered_json{{"LABEL", "Server"}, {"HOST", "win32"}, {"PATH", "srv.exe"}});
    AddChain(Idx, "v2", {New}, {"v1"});
    const Node *V2 = Idx.Find("v2");
    CHECK_EQ(V2->ExecFor("").value("CONTENTPATH", std::string()), std::string("new.exe"));
    CHECK(!V2->ExecFor("").value("ENV", ordered_json::object()).contains("FROM_OLD"));
    CHECK_EQ(V2->ExecFor("Server").value("CONTENTPATH", std::string()), std::string("srv.exe"));
    // The closure still mounts v1 underneath — bytes travel, execution does not.
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "v2", {});
    CHECK(Contains(Order, "v1") && Order.back() == "v2");
}

// ---- grafts: selection ≠ closure ----

// A graft is a node in nobody's list that is OVER something of this title. It is OFFERED when the SELECTED set
// (launchable + ticked grafts) satisfies its identity-bearing requirements — never the closure: a mod OVER a
// version UNDER the one you play is a sibling branch, not a mod for you. Substance (no identity) is satisfied by
// mounting. NOT is judged against the selected set. Ticking grows the set to a fixpoint.
TEST(offered_grafts_follow_the_selected_set_not_the_closure)
{
    NodeIndex Idx;
    AddChain(Idx, "v1", {NodeFixture::Merge({NodeFixture::Content("zip", "v1.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G")})});
    AddChain(Idx, "data", {NodeFixture::Content("zip", "d.zip")}, {"v1"});                        // part of v2's own composition
    AddChain(Idx, "v2", {NodeFixture::Merge({NodeFixture::Content("zip", "v2.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G")})}, {"v1", "data"});
    AddChain(Idx, "mod_v1",   {NodeFixture::Content("dir", "a")}, {"v1"});                       // a branch off v1
    AddChain(Idx, "mod_v2",   {NodeFixture::Content("dir", "b")}, {"v2"});                       // a branch off v2
    AddChain(Idx, "mod_any",  {NodeFixture::Content("dir", "c")}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"v1", "v2"}) })}});
    AddChain(Idx, "hd",       {NodeFixture::Content("dir", "d")}, {"mod_v2"});                   // a graft on a graft
    AddChain(Idx, "lib",      {NodeFixture::Content("dir", "l")});                               // substance
    AddChain(Idx, "uses_lib", {NodeFixture::Content("dir", "e")}, {"v2", "lib"});
    AddChain(Idx, "rival",    {NodeFixture::Content("dir", "r")}, {}, {{"OVER", ordered_json::array({"v2", ordered_json{{"NOT", "mod_v2"}}})}});
    AddChain(Idx, "other_game", {NodeFixture::Merge({NodeFixture::Exec("win32", "o.exe"), NodeFixture::Tile("2", "O")})});
    AddChain(Idx, "mod_other", {NodeFixture::Content("dir", "o")}, {"other_game"});
    ManifestModel::DeriveIdentity(Idx);

    auto Offer = [&](const std::map<std::string, bool> &T) {
        std::map<std::string, ManifestModel::GraftOffer> Out;
        for (const auto &O : ManifestModel::OfferedGrafts(Idx, "v2", T)) Out[O.Graft->NodeId] = O;
        return Out;
    };
    // Nothing ticked: playing v2.
    auto O = Offer({});
    CHECK(O.count("mod_v2") && O["mod_v2"].Applicable && !O["mod_v2"].Selected);
    CHECK(O.count("mod_v1") && !O["mod_v1"].Applicable);            // v1 is UNDER v2, not selected ⇒ not for you
    CHECK_EQ(O["mod_v1"].Blocker, std::string("v1"));
    CHECK(O.count("mod_any") && O["mod_any"].Applicable);           // the group is satisfied by the launchable
    CHECK(O.count("hd") && !O["hd"].Applicable);                    // needs mod_v2 ticked first
    CHECK(O.count("uses_lib") && O["uses_lib"].Applicable);         // lib is substance: satisfied by mounting
    CHECK(O.count("rival") && O["rival"].Applicable);
    CHECK(!O.count("mod_other"));                                   // another title's mod is not offered here
    CHECK(!O.count("v1") && !O.count("v2"));                        // launchables are variants, never grafts
    CHECK(!O.count("lib"));                                         // substance is never offered
    CHECK(!O.count("data"));                                        // a node in the launchable's OWN closure is not a graft

    // Tick mod_v2: hd becomes applicable (fixpoint), rival's NOT trips.
    auto O2 = Offer({{"mod_v2", true}});
    CHECK(O2["mod_v2"].Selected);
    CHECK(O2["hd"].Applicable && !O2["hd"].Selected);
    CHECK(!O2["rival"].Applicable);
    CHECK_EQ(O2["rival"].ExcludedBy, std::string("mod_v2"));                // a NOT block names the excluder, not a need
    // Tick hd too: selected through the fixpoint in ONE pass.
    auto O3 = Offer({{"mod_v2", true}, {"hd", true}});
    CHECK(O3["hd"].Selected);
    // Tick hd WITHOUT mod_v2: not applicable ⇒ not selected, whatever the tick says.
    auto O4 = Offer({{"hd", true}});
    CHECK(!O4["hd"].Selected && !O4["hd"].Applicable);
    // Tick rival AND mod_v2: mod_v2 comes first in candidate order and wins; rival (whose NOT names it) is out.
    auto O5 = Offer({{"mod_v2", true}, {"rival", true}});
    CHECK(O5["mod_v2"].Selected && !O5["rival"].Selected);
    CHECK_EQ(O5["rival"].ExcludedBy, std::string("mod_v2"));
    // NOT is SYMMETRIC: "arch" sorts before mod_v2 and its NOT names mod_v2 — with both ticked, arch (first) is
    // selected and mod_v2 is EXCLUDED BY it, even though mod_v2 carries no NOT of its own; and hd, ticked and
    // standing on mod_v2, is RETRACTED with it — never mounted next to the node that excludes its base.
    AddChain(Idx, "arch", {NodeFixture::Content("dir", "x")}, {}, {{"OVER", ordered_json::array({"v2", ordered_json{{"NOT", "mod_v2"}}})}});
    ManifestModel::DeriveIdentity(Idx);
    auto O5b = Offer({{"arch", true}, {"mod_v2", true}, {"hd", true}});
    CHECK(O5b["arch"].Selected);
    CHECK(!O5b["mod_v2"].Selected && !O5b["mod_v2"].Applicable);
    CHECK_EQ(O5b["mod_v2"].ExcludedBy, std::string("arch"));
    CHECK(!O5b["hd"].Selected && !O5b["hd"].Applicable);
    CHECK_EQ(O5b["hd"].Blocker, std::string("mod_v2"));
    const auto Base5 = ManifestModel::ResolveNodeOrder(Idx, "v2", {});
    const auto Mount5 = ManifestModel::ResolveGraftOrder(Idx, "v2", {{"arch", true}, {"mod_v2", true}, {"hd", true}}, Base5);
    CHECK(Contains(Mount5, "arch") && !Contains(Mount5, "mod_v2") && !Contains(Mount5, "hd"));
    // Untick arch ⇒ mod_v2 and hd are back.
    auto O5c = Offer({{"arch", false}, {"mod_v2", true}, {"hd", true}});
    CHECK(O5c["mod_v2"].Selected && O5c["hd"].Selected && !O5c["arch"].Selected);

    // The author's default (TOGGLE "on") pre-selects a graft the user has not touched.
    AddChain(Idx, "default_on", {NodeFixture::Content("dir", "z")}, {"v2"}, {{"TOGGLE", "on"}});
    ManifestModel::DeriveIdentity(Idx);
    auto O6 = Offer({});
    CHECK(O6["default_on"].Selected);
    CHECK(!Offer({{"default_on", false}})["default_on"].Selected);

    // Scope: a candidate the scope predicate rejects (a CATALOG stub) is not offered at all.
    std::vector<ManifestModel::GraftOffer> Scoped = ManifestModel::OfferedGrafts(Idx, "v2", {}, [](const Node &N) { return N.NodeId != "mod_v2"; });
    bool SawModV2 = false; for (const auto &G : Scoped) if (G.Graft->NodeId == "mod_v2") SawModV2 = true;
    CHECK(!SawModV2);
}

// The graft order: selected applicable grafts mount ABOVE the base closure, in instance precedence (higher =
// later = wins), ties by key; each graft's own substance is pulled in beneath it; nothing already in the base
// mount is repeated.
TEST(resolve_graft_order_is_precedence_then_key_with_substance_beneath)
{
    NodeIndex Idx;
    AddChain(Idx, "game", {NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G")})});
    AddChain(Idx, "lib",  {NodeFixture::Content("dir", "l")});
    AddChain(Idx, "a",    {NodeFixture::Content("dir", "a")}, {"game", "lib"});
    AddChain(Idx, "b",    {NodeFixture::Content("dir", "b")}, {"game"});
    AddChain(Idx, "b_hd", {NodeFixture::Content("dir", "bh")}, {"b"});
    ManifestModel::DeriveIdentity(Idx);
    const std::vector<std::string> Base = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    const std::map<std::string, bool> Ticks{{"a", true}, {"b", true}, {"b_hd", true}};

    // Default: key order (a, b, b_hd); lib is a's substance and comes right before a; game is never repeated.
    const auto Def = ManifestModel::ResolveGraftOrder(Idx, "game", Ticks, Base);
    CHECK(Contains(Def, "a") && Contains(Def, "b") && Contains(Def, "b_hd") && Contains(Def, "lib"));
    CHECK(!Contains(Def, "game"));
    CHECK(IndexOf(Def, "lib") < IndexOf(Def, "a"));
    CHECK(IndexOf(Def, "a") < IndexOf(Def, "b"));
    CHECK(IndexOf(Def, "b") < IndexOf(Def, "b_hd"));
    // Precedence: a ranks 10, b ranks 5 ⇒ b mounts BEFORE a (higher = later = wins); b_hd, OVER b, still follows b.
    const auto Prec = ManifestModel::ResolveGraftOrder(Idx, "game", Ticks, Base, {{"a", 10}, {"b", 5}});
    CHECK(IndexOf(Prec, "b") < IndexOf(Prec, "a"));
    CHECK(IndexOf(Prec, "b") < IndexOf(Prec, "b_hd"));
    // Nothing ticked ⇒ nothing above the base.
    CHECK(ManifestModel::ResolveGraftOrder(Idx, "game", {}, Base).empty());
    // An unapplicable tick (b_hd without b) contributes nothing.
    CHECK(ManifestModel::ResolveGraftOrder(Idx, "game", {{"b_hd", true}}, Base).empty());
}
