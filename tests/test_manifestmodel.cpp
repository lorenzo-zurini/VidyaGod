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
    CHECK(!N.IsRunnable() && !N.IsRunner());   // a plain node
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

static ordered_json Not(const char *Ref) { return ordered_json::array({ ordered_json{{"NOT", Ref}} }); }

// The closure is a PURE CONJUNCTION: everything reachable through BARE refs, and nothing else. A TOGGLE on a
// node in the closure is inert (it mounts, whatever the toggles say); an any-of group is a requirement, never
// followed; a NOT is a requirement, never followed. Facts fold, choices don't — and there are no choices here.
TEST(closure_is_a_pure_conjunction)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {});
    AddChain(Idx, "opt",  {}, {}, {{"TOGGLE", "off"}});
    AddChain(Idx, "m1", {}); AddChain(Idx, "m2", {}); AddChain(Idx, "x", {});
    AddChain(Idx, "game", {}, {}, {{"OVER", ordered_json::array({"base", "opt", ordered_json::array({"m1", "m2"}), ordered_json{{"NOT", "x"}}})}});
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "game", {});
    CHECK(Contains(Order, "base") && Contains(Order, "opt") && Order.back() == "game");
    CHECK(!Contains(Order, "m1") && !Contains(Order, "m2") && !Contains(Order, "x"));   // requirements are not composition
    CHECK(Contains(ManifestModel::ResolveNodeOrder(Idx, "game", {{"opt", false}}), "opt"));   // toggles change nothing
    CHECK_EQ(Idx.Find("game")->Composes.size(), (size_t)2);
    CHECK_EQ(Idx.Find("game")->Parents.size(), (size_t)4);                        // every positive ref, for freeze/fetch
}

TEST(resolve_order_missing_parent_reported)
{
    NodeIndex Idx;
    AddChain(Idx, "game", {}, {"ghost"});
    std::vector<std::string> Missing;
    ManifestModel::ResolveNodeOrder(Idx, "game", {}, &Missing);
    CHECK(Contains(Missing, "ghost"));
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

// The final chain: the TILE sits at the base (the pristine) and identity ascends; a tile above another of the
// same UID is a CHILD face (an expansion), nested by containment; a runnable node with VARIANT is on the shelf;
// how to run folds along the chain — own entries, else the nearest beneath. The card reads its name off the
// main face and opens on its RECOMMENDED variant. Two main faces in one UID are noted.
TEST(faces_nest_by_containment_and_entries_fold)
{
    NodeIndex Idx;
    ordered_json Pristine = NodeFixture::Merge({NodeFixture::Content("zip", "aok.zip"), NodeFixture::Tile("749", "Age of Kings"),
                                                NodeFixture::Exec("win32", "empires2.exe")});
    AddChain(Idx, "pristine", {Pristine});
    AddChain(Idx, "nocd",     {NodeFixture::Content("dir", "nocd")}, {"pristine"});
    AddChain(Idx, "lib1", {NodeFixture::Content("dir", "l1")});                   // a library chain, shared with another title
    AddChain(Idx, "lib2", {NodeFixture::Content("dir", "l2")}, {"lib1"});
    AddChain(Idx, "aok",  {NodeFixture::Variant("Age of Kings")}, {"nocd", "lib2"});          // inherits its entry from the pristine
    AddChain(Idx, "tc_base", {NodeFixture::Merge({NodeFixture::Content("zip", "tc.zip"), NodeFixture::Tile("749", "The Conquerors")})}, {"pristine"});
    AddChain(Idx, "conq", {NodeFixture::Merge({NodeFixture::Variant("The Conquerors"), NodeFixture::Exec("win32", "age2_x1.exe")})}, {"tc_base"}, {{"RECOMMENDED", true}});
    AddChain(Idx, "fe_base", {NodeFixture::Merge({NodeFixture::Content("zip", "fe.zip"), NodeFixture::Tile("749", "Forgotten Empires")})}, {"tc_base"});
    AddChain(Idx, "fe",   {NodeFixture::Merge({NodeFixture::Variant("Forgotten Empires"), NodeFixture::Exec("win32", "age2_x1.5.exe")})}, {"fe_base"});
    AddChain(Idx, "other", {NodeFixture::Merge({NodeFixture::Exec("win32", "o.exe"), NodeFixture::Tile("2", "Other"), NodeFixture::Variant("Play")})}, {"lib2"});
    AddChain(Idx, "step",  {NodeFixture::Content("dir", "s")}, {"tc_base"});                   // no tile: tc_base is 1 beneath
    AddChain(Idx, "near_face",  {NodeFixture::Content("dir", "nf")}, {"step", "pristine"});    // pristine at 1, tc_base at 2
    AddChain(Idx, "near_entry", {NodeFixture::Content("dir", "ne")}, {"nocd", "conq"});        // conq's entry at 1, pristine's at 2
    AddChain(Idx, "grouped", {NodeFixture::Content("dir", "gr")}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"conq", "aok"}) })}});
    AddChain(Idx, "conq_copy", {NodeFixture::Merge({NodeFixture::Variant("The Conquerors (copy)"), NodeFixture::Tile("749", "The Conquerors")})}, {"tc_base"});
    ManifestModel::DeriveIdentity(Idx);
    // a group is a requirement, not composition: nothing is inherited THROUGH it (face yes — identity ascends
    // over every positive ref — but the entry, a fact of the composed chain, no)
    CHECK_EQ(Idx.Find("grouped")->Uid, std::string("749"));
    CHECK(!Idx.Find("grouped")->IsRunnable());
    CHECK(Idx.Find("grouped")->EntrySource.empty());
    // the SAME tile repeated above itself is the same face, not a nested one
    CHECK_EQ(ManifestModel::FaceDepth(Idx, *Idx.Find("conq_copy")), 1);
    // NEAREST beneath, by OVER distance — not the first found, not the deepest
    CHECK_EQ(Idx.Find("near_face")->FaceKey, std::string("pristine"));   CHECK_EQ(Idx.Find("near_face")->FaceDistance, 1);
    CHECK_EQ(Idx.Find("near_entry")->EntrySource, std::string("conq"));
    CHECK_EQ(Idx.Find("near_entry")->ExecFor("").value("CONTENTPATH", std::string()), std::string("age2_x1.exe"));
    // faces
    CHECK_EQ(Idx.Find("aok")->FaceKey, std::string("pristine"));  CHECK_EQ(Idx.Find("aok")->FaceDistance, 2);
    CHECK_EQ(Idx.Find("conq")->FaceKey, std::string("tc_base"));
    CHECK_EQ(Idx.Find("fe")->FaceKey, std::string("fe_base"));
    CHECK_EQ(Idx.Find("aok")->Meta.value("TITLE", std::string()), std::string("Age of Kings"));
    CHECK_EQ(Idx.Find("conq")->Meta.value("TITLE", std::string()), std::string("The Conquerors"));
    CHECK_EQ(Idx.Find("nocd")->Uid, std::string("749"));                                      // identity ascends
    CHECK(!Idx.Find("lib1")->HasIdentity());                                                  // substance
    CHECK_EQ(ManifestModel::FaceDepth(Idx, *Idx.Find("pristine")), 0);                         // the main face
    CHECK_EQ(ManifestModel::FaceDepth(Idx, *Idx.Find("tc_base")), 1);                          // a child
    CHECK_EQ(ManifestModel::FaceDepth(Idx, *Idx.Find("fe_base")), 2);                          // a grandchild
    // entries fold: aok declares none and runs the pristine's; conq declares its own
    CHECK(Idx.Find("aok")->IsVariant() && Idx.Find("aok")->IsRunnable());
    CHECK_EQ(Idx.Find("aok")->EntrySource, std::string("pristine"));
    CHECK_EQ(Idx.Find("aok")->ExecFor("").value("CONTENTPATH", std::string()), std::string("empires2.exe"));
    CHECK_EQ(Idx.Find("conq")->EntrySource, std::string("conq"));
    CHECK(Idx.Find("nocd")->IsRunnable() && !Idx.Find("nocd")->IsVariant());                  // runnable, not on the shelf
    // the card: main face first (its variants), then children by depth; the recommended variant may be a child
    const auto O = ManifestModel::OrderVariants(Idx, {Idx.Find("fe"), Idx.Find("conq"), Idx.Find("aok")});
    std::vector<std::string> Ids; for (const Node *N : O) Ids.push_back(N->NodeId);
    CHECK(Ids == (std::vector<std::string>{"aok", "conq", "fe"}));
    CHECK_EQ(O.front()->Meta.value("TITLE", std::string()), std::string("Age of Kings"));
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(Errors.empty());
    CHECK(!AnyContains(Warnings, "main faces"));
    // Two main faces: a standalone install sharing the UID, OVER nothing of it.
    AddChain(Idx, "custom", {NodeFixture::Merge({NodeFixture::Content("zip", "c.zip"), NodeFixture::Tile("749", "Custom Edition"),
                                                 NodeFixture::Exec("win32", "c.exe"), NodeFixture::Variant("Custom")})});
    ManifestModel::DeriveIdentity(Idx);
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "2 main faces"));
}

// A version chain: one tile, one entry at the base, every version a delta and a VARIANT. The entry folds up the
// chain until a version declares a new one; a VARIANT with nothing to run is an error; TOGGLE / any-of / NOT on
// a variant are inert and said so.
TEST(entries_fold_along_a_version_chain)
{
    NodeIndex Idx;
    AddChain(Idx, "mc", {NodeFixture::Merge({NodeFixture::Content("file", "rd.jar"), NodeFixture::Tile("320", "Minecraft"), NodeFixture::Exec("java8", "")})});
    AddChain(Idx, "v1", {NodeFixture::Merge({NodeFixture::Content("delta", "v1.vgdelta"), NodeFixture::Variant("1.16.4")})}, {"mc"});
    AddChain(Idx, "v2", {NodeFixture::Merge({NodeFixture::Content("delta", "v2.vgdelta"), NodeFixture::Variant("1.16.5")})}, {"v1"});
    AddChain(Idx, "v3", {NodeFixture::Merge({NodeFixture::Content("delta", "v3.vgdelta"), NodeFixture::Variant("1.17"), NodeFixture::Exec("java16", "")})}, {"v2"});
    AddChain(Idx, "v4", {NodeFixture::Merge({NodeFixture::Content("delta", "v4.vgdelta"), NodeFixture::Variant("1.17.1")})}, {"v3"});
    AddChain(Idx, "lonely", {NodeFixture::Merge({NodeFixture::Content("dir", "x"), NodeFixture::Variant("Nothing to run")})});
    AddChain(Idx, "sp", {NodeFixture::Variant("SP")}, {"v4"}, {{"TOGGLE", "on"}});
    ManifestModel::DeriveIdentity(Idx);
    CHECK_EQ(Idx.Find("v2")->EntrySource, std::string("mc"));   CHECK_EQ(Idx.Find("v2")->HostPlatform, std::string("java8"));
    CHECK_EQ(Idx.Find("v3")->EntrySource, std::string("v3"));   CHECK_EQ(Idx.Find("v3")->HostPlatform, std::string("java16"));
    CHECK_EQ(Idx.Find("v4")->EntrySource, std::string("v3"));   CHECK_EQ(Idx.Find("v4")->HostPlatform, std::string("java16"));
    CHECK(Idx.Find("v4")->IsVariant() && Idx.Find("v4")->Uid == "320");
    CHECK(!Idx.Find("lonely")->IsRunnable() && !Idx.Find("lonely")->IsVariant());
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Errors, "VARIANT 'Nothing to run' has no effective entrypoint"));
    CHECK(AnyContains(Warnings, "TOGGLE / any-of / NOT on a variant are inert"));
    // The mount of a version is its chain; nothing beneath is a choice.
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "v4", {});
    CHECK(Contains(Order, "mc") && Contains(Order, "v1") && Order.back() == "v4");
}

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
TEST(validate_warns_on_a_variant_with_no_face)
{
    NodeIndex Idx;
    AddChain(Idx, "lost", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})});
    AddChain(Idx, "wine", {NodeFixture::Runner("linux64", {"win32"}, "wine")});
    AddChain(Idx, "tool", {NodeFixture::Exec("win32", "t.exe")});     // runnable, not on the shelf: no card, no warning
    ManifestModel::DeriveIdentity(Idx);
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(AnyContains(Warnings, "has no face"));
    int N = 0; for (const auto &W : Warnings) if (W.find("has no face") != std::string::npos) ++N;
    CHECK_EQ(N, 1);                                                    // the runner and the tool are not warned about
}

// ---- Role collapse: identity DERIVED from Declare* layers ----

TEST(declare_layers_derive_identity)
{
    Node N;
    ordered_json lj{{"RECOMMENDED", true}, {"ENTRYPOINTS", ordered_json::array({ ordered_json{{"HOST","win32"},{"PATH","game.exe"},
                    {"LABEL","Vanilla"},{"RUNNER","geproton_9_20_runner"}} })}};
    CHECK(ManifestModel::ParseNode(lj, "f.json", "/b", N));
    CHECK(N.IsRunnable()); CHECK(!N.IsRunner()); CHECK(!N.IsVariant());   // runnable, not on the shelf (no VARIANT)
    CHECK_EQ(N.HostPlatform, std::string("win32"));
    CHECK_EQ(N.Exec.value("CONTENTPATH", std::string()), std::string("game.exe"));
    CHECK_EQ(N.Label, std::string("Vanilla")); CHECK(N.Recommended);
    CHECK_EQ(N.RecommendedRunner, std::string("geproton_9_20_runner"));   // the entry's RUNNER → soft package-side runner

    Node R;
    // A runner is the SAME declaration with GUEST platforms — no GUEST ⇒ terminal ⇒ launchable.
    ordered_json rj = NodeFixture::Runner("linux64", {"win32","win64"}, "wine");
    rj["LABEL"] = "wine";
    CHECK(ManifestModel::ParseNode(rj, "f.json", "/b", R));
    CHECK(R.IsRunner()); CHECK(!R.IsRunnable());
    CHECK_EQ(R.HostPlatform, std::string("linux64"));
    CHECK_EQ((int)R.GuestPlatform.size(), 2);
    CHECK_EQ(R.Exec.value("EXECUTABLE", std::string()), std::string("wine"));

    Node L;
    ordered_json tj = NodeFixture::Tile("42", "My Game");
    tj["LABEL"] = "tile";
    CHECK(ManifestModel::ParseNode(tj, "f.json", "/b", L));
    CHECK(L.Presentable()); CHECK(!L.IsRunnable()); CHECK(L.OwnTile);
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
    CHECK(v1->IsRunnable());
    CHECK_EQ(v1->GameKey(), std::string("7"));           // grouped under the tile by UID
    CHECK(v1->Presentable());                             // inherited the tile metadata
    CHECK_EQ(v1->Meta.value("TITLE", std::string()), std::string("My Game"));
    CHECK_EQ(v1->Uid, std::string("7"));
    CHECK_EQ(v1->Label, std::string("v1"));
    CHECK(Idx.Find("mygame")->Presentable());            // the tile node is presentable
    CHECK(!Idx.Find("mygame")->IsRunnable());            // but not runnable itself
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

// The entry is a CHOICE of how to run and is replaced whole by a declaration above; the environment is a FACT
// and folds: v2's own entry does not carry v1's, but the mount's environment carries both nodes' ENV (a bag).
TEST(exec_is_the_effective_entry_declared_or_inherited)
{
    NodeIndex Idx;
    ordered_json Old = NodeFixture::Exec("win32", "old.exe");
    Old["ENV"] = ordered_json{{"FROM_OLD", "1"}};
    AddChain(Idx, "v1", {Old});
    ordered_json New = NodeFixture::Exec("win32", "new.exe");
    New["ENV"] = ordered_json{{"FROM_NEW", "1"}};
    New["ENTRYPOINTS"].push_back(ordered_json{{"LABEL", "Server"}, {"HOST", "win32"}, {"PATH", "srv.exe"}});
    AddChain(Idx, "v2", {New}, {"v1"});
    const Node *V2 = Idx.Find("v2");
    CHECK_EQ(V2->ExecFor("").value("CONTENTPATH", std::string()), std::string("new.exe"));
    CHECK(!V2->ExecFor("").contains("ENV"));                                                    // an entry carries no environment
    CHECK_EQ(V2->ExecFor("Server").value("CONTENTPATH", std::string()), std::string("srv.exe"));
    ordered_json Env; std::vector<std::string> Rm;
    ManifestModel::FoldEnv(Idx, ManifestModel::ResolveNodeOrder(Idx, "v2", {}), Env, Rm);
    CHECK(Env.contains("FROM_OLD") && Env.contains("FROM_NEW"));                                // the mount's environment folds
    // The closure still mounts v1 underneath — bytes travel; the entry is REPLACED by v2's own declaration.
    const auto Order = ManifestModel::ResolveNodeOrder(Idx, "v2", {});
    CHECK(Contains(Order, "v1") && Order.back() == "v2");
    // A version that declares no entry INHERITS the nearest one beneath — the whole list, so "Server" rides up.
    AddChain(Idx, "v3", {NodeFixture::Content("delta", "v3.vgdelta")}, {"v2"});
    ManifestModel::DeriveIdentity(Idx);
    const Node *V3 = Idx.Find("v3");
    CHECK(V3->IsRunnable());
    CHECK_EQ(V3->EntrySource, std::string("v2"));
    CHECK_EQ(V3->ExecFor("").value("CONTENTPATH", std::string()), std::string("new.exe"));
    CHECK_EQ(V3->ExecFor("Server").value("CONTENTPATH", std::string()), std::string("srv.exe"));
}

// The environment is mutation: it folds along the mount, lowest first — a later node wins a name, a later set
// undoes an earlier remove, a later remove undoes an earlier set; what is removed and never set again is reported.
TEST(env_folds_along_the_mount_order)
{
    NodeIndex Idx;
    ordered_json Lib = NodeFixture::Content("dir", "lib");
    Lib["ENV"] = ordered_json{{"A", "lib"}, {"B", "lib"}, {"C", "lib"}};
    ordered_json Game = NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G"), NodeFixture::Variant("Play")});
    Game["ENV"] = ordered_json{{"A", "game"}, {"D", "game"}};
    Game["ENV_REMOVE"] = ordered_json::array({"B", "Z"});
    ordered_json Mod = NodeFixture::Content("dir", "m");
    Mod["ENV"] = ordered_json{{"B", "mod"}};                                 // sets what the game removed: a later set wins
    Mod["ENV_REMOVE"] = ordered_json::array({"D"});                           // removes what the game set: a later remove wins
    AddChain(Idx, "lib", {Lib});
    AddChain(Idx, "game", {Game}, {"lib"});
    AddChain(Idx, "mod", {Mod}, {"game"});
    ManifestModel::DeriveIdentity(Idx);
    CHECK_EQ(Idx.Find("game")->EnvRemove.size(), (size_t)2);
    ordered_json Env; std::vector<std::string> Remove;
    ManifestModel::FoldEnv(Idx, {"lib", "game", "mod"}, Env, Remove);
    CHECK_EQ(Env.value("A", std::string()), std::string("game"));            // later wins
    CHECK_EQ(Env.value("B", std::string()), std::string("mod"));             // removed by the game, set again by the mod
    CHECK_EQ(Env.value("C", std::string()), std::string("lib"));             // untouched from below
    CHECK(!Env.contains("D"));                                                // set by the game, removed by the mod
    CHECK(std::find(Remove.begin(), Remove.end(), "D") != Remove.end());
    CHECK(std::find(Remove.begin(), Remove.end(), "Z") != Remove.end());     // removed, never set: reported for the host env
    CHECK(std::find(Remove.begin(), Remove.end(), "B") == Remove.end());     // set again after the remove: not a removal
    // a non-string value is refused at parse, never skipped at exec
    Node N; ordered_json Bad = NodeFixture::Content("dir", "x"); Bad["ENV"] = ordered_json{{"N", 5}};
    ManifestModel::ParseNode(Bad, {}, {}, N);                                 // stays indexed (referrers must not dangle)…
    CHECK(!N.LowerError.empty() && N.Env.empty());                            // …but malformed: no environment, an error
}

// TILE.META is free-form (and foreign on a received block): it must not overwrite the tile's own validated fields.
TEST(tile_meta_cannot_override_the_tiles_own_fields)
{
    Node N;
    ordered_json J = NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Tile("1", "G")});
    J["TILE"]["META"] = ordered_json{{"TITLE", 5}, {"UID", "9"}, {"COVER", 7}, {"YEAR", "1999"}};
    CHECK(ManifestModel::ParseNode(J, {}, {}, N));
    CHECK_EQ(N.Meta.value("TITLE", std::string()), std::string("G"));
    CHECK_EQ(N.Uid, std::string("1"));
    CHECK(N.Meta["UID"].is_string() && N.Meta["UID"] == "1");
    CHECK(!N.Meta.contains("COVER") || !N.Meta["COVER"].is_number());
    CHECK_EQ(N.Meta.value("YEAR", std::string()), std::string("1999"));
}

// PUBLISH is per node and the share list silently lacks an unflagged variant: a card shared by halves is an omission
// the validator must name.
TEST(validate_warns_on_an_unpublished_variant_beside_published_ones)
{
    NodeIndex Idx;
    AddChain(Idx, "base", {NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Tile("1", "G")})});
    AddChain(Idx, "sp", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("SP")})}, {"base"});
    AddChain(Idx, "mp", {NodeFixture::Merge({NodeFixture::Exec("win32", "m.exe"), NodeFixture::Variant("MP")})}, {"base"}, {{"PUBLISH", true}});
    AddChain(Idx, "other", {NodeFixture::Merge({NodeFixture::Content("zip", "o.zip"), NodeFixture::Tile("2", "O"), NodeFixture::Exec("win32", "o.exe"), NodeFixture::Variant("Play")})});
    ManifestModel::DeriveIdentity(Idx);
    std::vector<std::string> Errors, Warnings;
    ManifestModel::ValidateNodeGraph(Idx, Errors, Warnings);
    CHECK(Errors.empty());
    int Hits = 0; for (const auto &W : Warnings) if (W.find("'sp'") != std::string::npos && W.find("no PUBLISH") != std::string::npos) ++Hits;
    CHECK_EQ(Hits, 1);
    for (const auto &W : Warnings) CHECK(W.find("'other'") == std::string::npos || W.find("no PUBLISH") == std::string::npos);   // a wholly unpublished title is the author's choice
}

// A frozen or fetched block carries no handle: ParseNode keys it by LABEL, and the index then files it under its
// real CID. The face's FaceKey must be that index key — a label-keyed FaceKey found a DIFFERENT node with the same
// label (a stale copy from an earlier publish), depth -1, null Meta, and OrderVariants threw on it.
TEST(a_faces_key_is_its_index_key_not_its_label)
{
    NodeIndex Idx;
    auto Parse = [&](const ordered_json &J, const std::string &Cid) {
        Node N; CHECK(ManifestModel::ParseNode(J, {}, {}, N)); N.Cid = Cid; Idx.Nodes.emplace(Cid, std::move(N));
    };
    ordered_json Tile = NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Tile("1", "G"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("v2")});
    Tile["LABEL"] = "v2";                                                   // no CID field: keyed by LABEL at parse
    ordered_json Stale = NodeFixture::Merge({NodeFixture::Content("zip", "old.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("v2")});
    Stale["LABEL"] = "v2";                                                  // same label, no tile, another CID
    Parse(Tile, "bafyTILE"); Parse(Stale, "bafySTALE");
    ManifestModel::DeriveIdentity(Idx);
    const Node *T = Idx.Find("bafyTILE");
    CHECK_EQ(T->NodeId, std::string("v2"));
    CHECK_EQ(T->FaceKey, std::string("bafyTILE"));
    CHECK(Idx.Find(T->FaceKey)->OwnTile);
    CHECK_EQ(ManifestModel::FaceDepth(Idx, *T), 0);
    const auto O = ManifestModel::OrderVariants(Idx, {Idx.Find("bafySTALE"), T});   // must not throw
    CHECK_EQ(O.front()->Cid, std::string("bafyTILE"));                      // the faced variant first, the faceless last
}

// ---- grafts: selection ≠ closure ----

// A graft is a node in nobody's list that is OVER something of this title. It is OFFERED when the SELECTED set
// (launchable + ticked grafts) satisfies its identity-bearing requirements — never the closure: a mod OVER a
// version UNDER the one you play is a sibling branch, not a mod for you. Substance (no identity) is satisfied by
// mounting. NOT is judged against the selected set. Ticking grows the set to a fixpoint.
TEST(offered_grafts_follow_the_selected_set_not_the_closure)
{
    NodeIndex Idx;
    AddChain(Idx, "v1", {NodeFixture::Merge({NodeFixture::Content("zip", "v1.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G"), NodeFixture::Variant("v1")})});
    AddChain(Idx, "data", {NodeFixture::Content("zip", "d.zip")}, {"v1"});                        // part of v2's own composition
    AddChain(Idx, "v2", {NodeFixture::Merge({NodeFixture::Content("zip", "v2.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G"), NodeFixture::Variant("v2")})}, {"v1", "data"});
    AddChain(Idx, "mod_v1",   {NodeFixture::Content("dir", "a")}, {"v1"});                       // a branch off v1
    AddChain(Idx, "mod_v2",   {NodeFixture::Content("dir", "b")}, {"v2"});                       // a branch off v2
    AddChain(Idx, "mod_any",  {NodeFixture::Content("dir", "c")}, {}, {{"OVER", ordered_json::array({ ordered_json::array({"v1", "v2"}) })}});
    AddChain(Idx, "hd",       {NodeFixture::Content("dir", "d")}, {"mod_v2"});                   // a graft on a graft
    AddChain(Idx, "hd_v1",    {NodeFixture::Content("dir", "d1")}, {"mod_v1"});                  // made of a v1-only mod
    AddChain(Idx, "mod_gone", {NodeFixture::Content("dir", "g")}, {"v2", "v_missing"});          // names a node that never landed
    AddChain(Idx, "v3",       {NodeFixture::Merge({NodeFixture::Content("delta", "v3.vgdelta"), NodeFixture::Variant("v3"), NodeFixture::Tile("1", "G")})}, {"lib"});   // declared VARIANT, no entry beneath (not yet landed)
    AddChain(Idx, "mod_v3",   {NodeFixture::Content("dir", "m3")}, {"v3", "v2"});
    AddChain(Idx, "lib",      {NodeFixture::Content("dir", "l")});                               // substance
    AddChain(Idx, "uses_lib", {NodeFixture::Content("dir", "e")}, {"v2", "lib"});
    AddChain(Idx, "rival",    {NodeFixture::Content("dir", "r")}, {}, {{"OVER", ordered_json::array({"v2", ordered_json{{"NOT", "mod_v2"}}})}});
    AddChain(Idx, "other_game", {NodeFixture::Merge({NodeFixture::Exec("win32", "o.exe"), NodeFixture::Tile("2", "O"), NodeFixture::Variant("Play")})});
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
    CHECK(O.count("hd") && O["hd"].Applicable);                     // made of mod_v2: ticking hd brings mod_v2
    CHECK(O.count("hd_v1") && !O["hd_v1"].Applicable);              // requirements are TRANSITIVE over composition:
    CHECK_EQ(O["hd_v1"].Blocker, std::string("v1"));                // what hd_v1 brings (mod_v1) needs v1
    CHECK(O.count("mod_gone") && !O["mod_gone"].Applicable);        // a ref not in the graph is a blocker, never composed
    CHECK_EQ(O["mod_gone"].Blocker, std::string("v_missing"));
    CHECK(O.count("mod_v3") && !O["mod_v3"].Applicable);            // v3 DECLARES VARIANT: a requirement even before its
    CHECK_EQ(O["mod_v3"].Blocker, std::string("v3"));               // entry source has landed (IsVariant() would be false)
    CHECK(O.count("uses_lib") && O["uses_lib"].Applicable);         // lib is substance: satisfied by mounting
    CHECK(O.count("rival") && O["rival"].Applicable);
    CHECK(!O.count("mod_other"));                                   // another title's mod is not offered here
    CHECK(!O.count("v1") && !O.count("v2"));                        // launchables are variants, never grafts
    CHECK(!O.count("lib"));                                         // substance is never offered
    CHECK(!O.count("data"));                                        // a node in the launchable's OWN closure is not a graft

    // Tick mod_v2: rival's NOT trips.
    auto O2 = Offer({{"mod_v2", true}});
    CHECK(O2["mod_v2"].Selected);
    CHECK(O2["hd"].Applicable && !O2["hd"].Selected);
    CHECK(!O2["rival"].Applicable);
    CHECK_EQ(O2["rival"].ExcludedBy, std::string("mod_v2"));                // a NOT block names the excluder, not a need
    // Tick hd too: selected through the fixpoint in ONE pass.
    auto O3 = Offer({{"mod_v2", true}, {"hd", true}});
    CHECK(O3["hd"].Selected);
    // Tick hd WITHOUT mod_v2: hd is MADE OF mod_v2 — ticking hd brings mod_v2 along (it mounts beneath hd, and
    // counts as selected: rival's NOT trips through it).
    auto O4 = Offer({{"hd", true}});
    CHECK(O4["hd"].Selected && O4["hd"].Applicable);
    CHECK(!O4["rival"].Applicable);
    const auto Base4 = ManifestModel::ResolveNodeOrder(Idx, "v2", {});
    const auto Mount4 = ManifestModel::ResolveGraftOrder(Idx, "v2", {{"hd", true}}, Base4);
    CHECK(Contains(Mount4, "mod_v2") && Contains(Mount4, "hd") && IndexOf(Mount4, "mod_v2") < IndexOf(Mount4, "hd"));
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
    CHECK(!O5b["hd"].Selected && !O5b["hd"].Applicable);                    // hd brings mod_v2, which arch excludes
    CHECK_EQ(O5b["hd"].ExcludedBy, std::string("arch"));
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
    AddChain(Idx, "game", {NodeFixture::Merge({NodeFixture::Content("zip", "g.zip"), NodeFixture::Exec("win32", "g.exe"), NodeFixture::Tile("1", "G"), NodeFixture::Variant("Play")})});
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
    // b_hd is MADE OF b: ticking it alone brings b beneath it.
    const auto Alone = ManifestModel::ResolveGraftOrder(Idx, "game", {{"b_hd", true}}, Base);
    CHECK(Contains(Alone, "b") && Contains(Alone, "b_hd") && IndexOf(Alone, "b") < IndexOf(Alone, "b_hd"));
}
