#include "vgtest.h"
#include "manifestmodel.h"
#include "nodefixture.h"
#include "nodelower.h"
#include "fold.h"
#include "cid.h"
#include "layerspec.h"   // NormalizeVPath — linked from vgfs_core (the parity test below)

#include <zip.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using nlohmann::ordered_json;
namespace NF = NodeFixture;

// Generation 6, one rule per test. The FOLD itself (expansion, occurrences, TAKE/TARGET, the per-address fold) is held
// plan-for-plan to the reference resolver by tools/gen6/fold_gate.py (ctest: fold_fixtures); these cover what the
// engine builds on it — parsing and the vocabulary, the index's derived facts, closure, lowering, ownership, the
// validator, and identity.

static void Add(NodeIndex &Idx, const ordered_json &J, const std::string &Dir = "/bundle")
{
    Node N;
    if (ManifestModel::ParseNode(J, "f.json", Dir, N)) Idx.Nodes[N.Key()] = N;
}
static void AddChain(NodeIndex &Idx, const std::string &Id, const std::vector<ordered_json> &Sections,
                     const std::vector<std::string> &Contains = {}, const ordered_json &Tail = ordered_json::object(),
                     const std::string &Dir = "/bundle")
{
    for (const auto &N : NF::Chain(Id, Sections, Contains, Tail)) Add(Idx, N, Dir);
}
static bool AnyContains(const std::vector<std::string> &V, const std::string &Needle)
{
    for (const auto &S : V) if (S.find(Needle) != std::string::npos) return true;
    return false;
}
static std::vector<std::string> Validate(const NodeIndex &Idx, std::vector<std::string> *Warnings = nullptr)
{
    std::vector<std::string> E, W;
    ManifestModel::ValidateNodeGraph(Idx, E, W);
    if (Warnings) *Warnings = W;
    return E;
}

// ---- parsing and the vocabulary -------------------------------------------------------------------------------

TEST(parse_reads_a_gen6_node)
{
    const ordered_json J = {
        {"CID", "h1"}, {"LABEL", "v"}, {"VARIANT", "1.0"}, {"RECOMMENDED", ordered_json::array({"7"})},
        {"LAYERS", ordered_json::array({
            {{"NODE", "base"}}, {{"NOT", "rival"}},
            {{"ZIP", "g.zip"}, {"SOURCE", "Qm1"}, {"SIZE", 5}, {"TARGET", "FILES/%GameDir%"}},
            {{"EDIT", ordered_json::array({ {{"MODE", "Poke"}, {"OFFSET", "0x1"}, {"VALUE", "00"}} })}, {"TARGET", "FILES/%GameDir%/g.exe"}},
            {{"EXEC", ordered_json::array({ {{"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "%GameDir%/g.exe"}, {"TILE", {{"UID", "7"}}}} })}},
        })},
    };
    Node N;
    CHECK(ManifestModel::ParseNode(J, "f.json", "/b", N));
    CHECK(N.LowerError.empty());
    CHECK_EQ(N.Cid, std::string("h1"));
    CHECK_EQ(N.Variant, std::string("1.0"));
    CHECK(N.Recommended == std::vector<std::string>{"7"});
    CHECK(N.Refs == std::vector<std::string>{"base"});                // NODE refs are followed…
    CHECK(N.Requires == std::vector<std::string>{"rival"});           // …ANY/NOT are only compared
    CHECK(!N.IsGraft);
    CHECK(N.OwnTile);
    //Its own layers lowered to the engine's vocabulary: content (TARGET without the FILES namespace), edit ops.
    CHECK_EQ(N.Layers.size(), (size_t)2);
    CHECK_EQ(N.Layers[0]["TYPE"].get<std::string>(), std::string("VFSZipLayer"));
    CHECK_EQ(N.Layers[0]["TARGET"].get<std::string>(), std::string("%GameDir%"));
    CHECK_EQ(N.Layers[0]["SOURCE"]["CID"].get<std::string>(), std::string("Qm1"));
    CHECK_EQ(N.Layers[0]["SOURCE"]["SIZE"].get<int>(), 5);
    CHECK_EQ(N.Layers[1]["TYPE"].get<std::string>(), std::string("BinaryPatch"));
    CHECK_EQ(N.Layers[1]["FILE"].get<std::string>(), std::string("%GameDir%/g.exe"));
}

TEST(a_graft_is_a_node_whose_list_begins_with_any)
{
    Node G, H;
    CHECK(ManifestModel::ParseNode({{"LABEL", "g"}, {"LAYERS", ordered_json::array({ {{"ANY", ordered_json::array({"v"})}}, {{"ZIP", "m.zip"}} })}}, "f", "/b", G));
    CHECK(ManifestModel::ParseNode({{"LABEL", "h"}, {"LAYERS", ordered_json::array({ {{"ZIP", "m.zip"}}, {{"ANY", ordered_json::array({"v"})}} })}}, "f", "/b", H));
    CHECK(G.IsGraft);
    CHECK(!H.IsGraft);                                                // an ANY later in the list is a check, not an anchor
}

// A layer's LABEL and SECTION name it and place it in the editor's tree; the fold never reads them, so a named layer
// lowers exactly like the bare one. Teeth: drop them from CheckLayer's accepted keys (the node stops lowering).
TEST(a_layer_label_and_section_are_presentation_only)
{
    Node Bare, Named;
    CHECK(ManifestModel::ParseNode({{"LABEL", "n"}, {"LAYERS", ordered_json::array({ {{"ENV", {{"A", "1"}}}} })}}, "f", "/b", Bare));
    CHECK(ManifestModel::ParseNode({{"LABEL", "n"}, {"LAYERS", ordered_json::array({ {{"ENV", {{"A", "1"}}},
          {"LABEL", "Font hinting"}, {"SECTION", "Compatibility/Fonts"}, {"COMMENT", "why"}} })}}, "f", "/b", Named));
    CHECK(Named.LowerError.empty());
    CHECK_EQ(Named.Layers.dump(), Bare.Layers.dump());
}

TEST(the_vocabulary_refuses_malformed_nodes_and_keeps_them_indexed)
{
    const std::vector<ordered_json> Bad = {
        {{"LABEL", "a"}, {"LAYERS", ordered_json::array({ {{"ZIP", "x.zip"}, {"DIR", "y"}} })}},          // two type keys
        {{"LABEL", "b"}, {"LAYERS", ordered_json::array({ {{"PATH", "x.zip"}} })}},                          // none
        {{"LABEL", "c"}, {"OVER", ordered_json::array()}, {"LAYERS", ordered_json::array()}},                // unknown field
        {{"LABEL", "d"}, {"VARIANT", ""}, {"LAYERS", ordered_json::array()}},                                 // empty VARIANT
        {{"LABEL", "e"}, {"RECOMMENDED", true}, {"LAYERS", ordered_json::array()}},                           // not a UID list
        {{"LABEL", "f"}, {"LAYERS", ordered_json::array({ {{"EDIT", ordered_json::array({ {{"MODE", "Poke"}} })}} })}},   // no TARGET
        {{"LABEL", "g"}, {"LAYERS", ordered_json::array({ {{"KEEP", {{"EXEC/Play", true}}}} })}},             // KEEP outside FILES/REG/VARS
        {{"LABEL", "h"}, {"LAYERS", ordered_json::array({ {{"ZIP", "x.zip"}, {"TARGET", "REG/HKCU"}} })}},     // content TARGET not in FILES
        {{"LABEL", "i"}, {"LAYERS", ordered_json::array({ {{"EDIT", ordered_json::array({ {{"MODE", "Poke"}, {"VALUE", 5}} })}, {"TARGET", "FILES/x"}} })}},   // VALUE not text
        {{"LABEL", "j"}, {"LAYERS", ordered_json::array({ {{"EXEC", ordered_json::array({ {{"LABEL", "Play"}}, {{"LABEL", "Play"}} })}} })}},   // two entries, one label
        {{"LABEL", "k"}, {"LAYERS", ordered_json::array({ {{"ENV", {{"A", "1"}}}, {"LABEL", ""}} })}},             // empty layer LABEL
        {{"LABEL", "l"}, {"LAYERS", ordered_json::array({ {{"ENV", {{"A", "1"}}}, {"SECTION", 3}} })}},            // SECTION not a path
    };
    for (const auto &J : Bad)
    {
        Node N;
        CHECK(ManifestModel::ParseNode(J, "f.json", "/b", N));        // still a node: indexed so validation can name it
        CHECK(!N.LowerError.empty());
        CHECK(N.Layers.empty());                                      // …and it lowers to nothing
    }
    NodeIndex Idx;
    Add(Idx, Bad[0]);
    CHECK(AnyContains(Validate(Idx), "exactly one type key"));
}

// ---- the index: closure and derived facts ---------------------------------------------------------------------

TEST(closure_follows_node_refs_post_order_and_reports_missing)
{
    NodeIndex Idx;
    AddChain(Idx, "lib", { NF::Content("zip", "l.zip") });
    AddChain(Idx, "v", { NF::Content("zip", "a.zip"), NF::Content("zip", "b.zip") }, { "lib", "ghost" });
    std::vector<std::string> Missing;
    const auto Order = ManifestModel::Closure(Idx, "v", &Missing);
    CHECK((Order == std::vector<std::string>{ "lib", "v__l0", "v" }));   // contained first, the root last
    CHECK((Missing == std::vector<std::string>{ "ghost" }));
    //Options count: a download is whole chains, a WHEN-gated NODE layer included.
    NodeIndex Idx2;
    AddChain(Idx2, "opt", { NF::Content("zip", "o.zip") });
    Add(Idx2, { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({ {{"NODE", "opt"}, {"WHEN", "%o%==1"}} })} });
    CHECK(ManifestModel::Closure(Idx2, "g").size() == 2);
}

TEST(facts_fold_entries_and_tiles_along_containment)
{
    // base carries the tile on a partial "Play" entry; the variant contains it and adds how to run.
    NodeIndex Idx;
    AddChain(Idx, "base", { NF::Merge({ NF::Content("zip", "g.zip", "%GameDir%"), NF::Tile("7", "Game", {{"TGDBID", "7"}}) }) });
    AddChain(Idx, "v", { NF::Merge({ NF::Exec("win32", "%GameDir%/g.exe"), NF::Variant("1.0") }) }, { "base" });
    ManifestModel::DeriveFacts(Idx);
    const Node &V = Idx.Nodes.at("v");
    CHECK(V.IsVariant());
    CHECK(V.HasExec && !V.HasRunner);
    CHECK_EQ(V.HostPlatform, std::string("win32"));
    CHECK_EQ(V.Exec["CONTENTPATH"].get<std::string>(), std::string("%GameDir%/g.exe"));
    CHECK(V.Faces == std::vector<std::string>{"7"});
    CHECK_EQ(V.Uid, std::string("7"));
    CHECK_EQ(V.PackageUid, std::string("7"));
    CHECK_EQ(V.Meta.value("TITLE", std::string()), std::string("Game"));
    CHECK_EQ(V.Meta.value("TGDBID", std::string()), std::string("7"));   // META hoisted flat
    CHECK(Idx.Nodes.at("base").OwnTile && !Idx.Nodes.at("base").HasExec);   // a partial entry does not run
}

TEST(a_tiles_presentation_is_its_recommended_variants_fold)
{
    // Two variants of one tile; each folds a different TITLE onto the entry. The shelf shows the recommended one's.
    NodeIndex Idx;
    AddChain(Idx, "base", { NF::Merge({ NF::Tile("7", "Old Title"), NF::Exec("win32", "g.exe") }) });
    AddChain(Idx, "a", { NF::Merge({ NF::Tile("7", "Title A"), NF::Variant("a") }) }, { "base" });
    AddChain(Idx, "b", { NF::Merge({ NF::Tile("7", "Title B"), NF::Variant("b") }) }, { "base" },
             { {"RECOMMENDED", ordered_json::array({"7"})} });
    ManifestModel::DeriveFacts(Idx);
    CHECK_EQ(Idx.Nodes.at("a").Meta.value("TITLE", std::string()), std::string("Title B"));
    CHECK_EQ(Idx.Nodes.at("b").Meta.value("TITLE", std::string()), std::string("Title B"));
}

TEST(package_uid_is_the_root_of_the_parentuid_chain)
{
    NodeIndex Idx;
    AddChain(Idx, "roc", { NF::Merge({ NF::Tile("802", "RoC"), NF::Exec("win32", "war3.exe"), NF::Variant("roc") }) });
    AddChain(Idx, "tft", { NF::Merge({ NF::Tile("803", "TFT", {}, "802"), NF::Variant("tft") }) }, { "roc" });
    ManifestModel::DeriveFacts(Idx);
    CHECK_EQ(Idx.Nodes.at("tft").Uid, std::string("803"));
    CHECK_EQ(Idx.Nodes.at("tft").PackageUid, std::string("802"));   // %PackageUID% = the family (the base game)
    CHECK_EQ(Idx.Nodes.at("tft").GameKey(), std::string("802"));
}

TEST(a_runner_is_an_entry_with_guest_platforms)
{
    NodeIndex Idx;
    ordered_json R = NF::Runner("linux64", {"win32", "win64"}, "%RunnerMount%/proton");
    R["LAYERS"][0]["EXEC"][0]["GUEST_ROOTS"] = { {"%GameDir%", "C:\\%PackageUID%"} };
    R["LAYERS"][0]["EXEC"][0]["DRIVES"] = { {"C:", "%PrefixRoot%/drive_c"} };
    R["LAYERS"][0]["EXEC"][0]["PREFIX_GENERATE"] = true;
    AddChain(Idx, "proton", { R });
    ManifestModel::DeriveFacts(Idx);
    const Node &P = Idx.Nodes.at("proton");
    CHECK(P.IsRunner() && P.OwnRunner && !P.HasExec);
    CHECK((P.GuestPlatform == std::vector<std::string>{"win32", "win64"}));
    CHECK_EQ(P.Exec["EXECUTABLE"].get<std::string>(), std::string("%RunnerMount%/proton"));
    CHECK(P.Exec["GUEST_ROOTS"].contains("%GameDir%"));
    CHECK(P.Exec["DRIVES"].contains("C:"));
    CHECK(P.Exec["PREFIX_GENERATE"].get<bool>());
}

TEST(tile_endpoints_chain_collapses_to_tip)
{
    // v1 ← v2 ← v3 (each contains the previous and adds content): one download row, the tip, dominating all three.
    NodeIndex Idx;
    AddChain(Idx, "v1", { NF::Merge({ NF::Content("zip", "1.zip"), NF::Exec("win32", "g"), NF::Tile("1"), NF::Variant("1") }) });
    AddChain(Idx, "v2", { NF::Merge({ NF::Content("delta", "2.vgdelta"), NF::Variant("2") }) }, { "v1" });
    AddChain(Idx, "v3", { NF::Merge({ NF::Content("delta", "3.vgdelta"), NF::Variant("3") }) }, { "v2" });
    ManifestModel::DeriveFacts(Idx);
    const auto Rows = ManifestModel::TileEndpoints(Idx, { "v1", "v2", "v3" });
    CHECK_EQ(Rows.size(), (size_t)1);
    CHECK((Rows[0].Ids == std::vector<std::string>{ "v3" }));
    CHECK_EQ(Rows[0].LaunchableCount, 3);
    CHECK_EQ(Rows[0].Label, std::string("3"));                        // a row is named by its VARIANT
}

// ---- lowering and ownership -------------------------------------------------------------------------------------

TEST(user_ownership_is_decided_by_the_most_specific_keep)
{
    const ordered_json Keep = { {"FILES/%UserProfile%/Saved Games/X/", {{"NAME", "X"}}},
                                {"FILES/%UserProfile%/Saved Games/X/ubi.ini", false},
                                {"FILES/%GameDir%/Default.cfg#/W:", true} };
    CHECK(NodeLower::UserOwned(Keep, "FILES/%UserProfile%/Saved Games/X/save1.sav"));
    CHECK(!NodeLower::UserOwned(Keep, "FILES/%UserProfile%/Saved Games/X/ubi.ini"));   // taken back
    CHECK(NodeLower::UserOwned(Keep, "FILES/%UserProfile%/saved games/x/other"));       // FILES compare case-insensitively
    CHECK(!NodeLower::UserOwned(Keep, "FILES/%GameDir%/Default.cfg"));                        // the file is the package's…
    CHECK(NodeLower::UserOwned(Keep, "FILES/%GameDir%/Default.cfg#/W:"));                     // …one key in it is the user's
    CHECK(!NodeLower::UserOwned(Keep, "FILES/%Documents%/elsewhere"));                            // nothing covers it: the package's
    // A pattern keep covers the files it names (AoE2's per-profile hotkeys) and nothing beside them.
    // Teeth: compare the pattern as a literal path.
    const ordered_json Hk = { {"FILES/%GameDir%/player*.hki", true} };
    CHECK(NodeLower::UserOwned(Hk, "FILES/%GameDir%/player2.hki"));
    CHECK(NodeLower::UserOwned(Hk, "FILES/%GameDir%/PLAYER12.HKI"));
    CHECK(!NodeLower::UserOwned(Hk, "FILES/%GameDir%/player.nfz"));
    CHECK(!NodeLower::UserOwned(Hk, "FILES/%GameDir%/Data/player2.hki.bak"));
    CHECK(!NodeLower::UserOwned(Hk, "FILES/%GameDir%/sub/player2.hki"));   // only files in the pattern's own folder, as persistence keeps
    const ordered_json Ini = { {"FILES/%GameDir%/*.ini", true} };
    CHECK(NodeLower::UserOwned(Ini, "FILES/%GameDir%/game.ini"));
    CHECK(!NodeLower::UserOwned(Ini, "FILES/%GameDir%/sub/x.ini"));        // a * does not cross a folder
    // The validator: a pattern only in a FILES address's last segment, and never a folder.
    const auto Keeps = [](const char *A) {
        return NodeLower::CheckNode(ordered_json{ {"LABEL", "n"}, {"LAYERS", ordered_json::array({ ordered_json{{"KEEP", {{A, true}}}} })} }, "n");
    };
    CHECK(Keeps("FILES/%GameDir%/save/slot*").empty());
    CHECK(!Keeps("FILES/%GameDir%/*/profile.dat").empty());
    CHECK(!Keeps("REG/HKCU/Software/*").empty());
    CHECK(!Keeps("FILES/%GameDir%/save*/").empty());
}

TEST(lower_plan_places_folds_and_applies_edits_by_ownership)
{
    // An EDIT applies to the files beneath it, so every file edit runs after the mount (OVERRIDE) — before the mount
    // (the engine's base pass) a zip's config.ini does not exist yet, and an edit there would shadow it with a stub.
    // The game keeps its save dir but takes ubi.ini back: its edit of ubi.ini is the package's, re-applied every
    // launch; its edit of prefs.ini (inside the kept dir) is a package DEFAULT — applied while the user has no saved
    // copy (IF_UNSAVED). A binary patch always runs after the mount and carries neither.
    const ordered_json Game = { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"ZIP", "g.zip"}, {"SOURCE", "Qm1"}, {"SIZE", 9}, {"TARGET", "FILES/%GameDir%"}},
        {{"EDIT", ordered_json::array({ {{"MODE", "Overwrite"}, {"VALUE", "v"}} })}, {"TARGET", "FILES/%UserProfile%/Saved Games/X/ubi.ini"}},
        {{"EDIT", ordered_json::array({ {{"MODE", "ConfigWrite"}, {"KEY", "/W:"}, {"VALUE", "%ScreenWidth%"}} })}, {"TARGET", "FILES/%GameDir%/Default.cfg"}},
        {{"EDIT", ordered_json::array({ {{"MODE", "Overwrite"}, {"VALUE", "p"}} })}, {"TARGET", "FILES/%UserProfile%/Saved Games/X/prefs.ini"}},
        {{"EDIT", ordered_json::array({ {{"MODE", "Poke"}, {"OFFSET", "0x10"}, {"VALUE", "ff"}} })}, {"TARGET", "FILES/%GameDir%/g.exe"}},
        {{"KEEP", {{"FILES/%UserProfile%/Saved Games/X/", {{"NAME", "X"}}}, {"FILES/%UserProfile%/Saved Games/X/ubi.ini", false}}}},
        {{"REG", {{"HKCU", {{"Software", {{"A", {{"k1", "1"}, {"k2", "2"}}}}}}}}}, {"ARCH", ordered_json::array({"32"})}},
        {{"DLL", {{"d3d8", "n,b"}}}},
        {{"VARS", {{"W", {{"DEFAULT", "1"}}}}}},
    })} };
    Fold::Library Lib;
    Lib.Nodes["g"] = { &Game, "/b" };
    const Fold::Plan P = Fold::Resolve(Lib, "g");
    const ordered_json Ops = NodeLower::LowerPlan(P);
    auto Find = [&](const std::string &Type, const std::string &Key, const std::string &Val) -> const ordered_json * {
        for (const auto &O : Ops) if (O.value("TYPE", std::string()) == Type && O.value(Key, std::string()) == Val) return &O;
        return nullptr;
    };
    const ordered_json *Zip = Find("VFSZipLayer", "TARGET", "%GameDir%");
    CHECK(Zip && (*Zip)["PATH"] == "/b/g.zip" && (*Zip)["SOURCE"]["CID"] == "Qm1" && (*Zip)["SOURCE"]["SIZE"] == 9);
    const ordered_json *Ubi = Find("FileEdit", "FILE", "%UserProfile%/Saved Games/X/ubi.ini");
    CHECK(Ubi && Ubi->value("OVERRIDE", false) && !Ubi->contains("IF_UNSAVED"));   // taken back ⇒ the package's, every launch
    const ordered_json *Cfg = Find("FileEdit", "FILE", "%GameDir%/Default.cfg");
    CHECK(Cfg && Cfg->value("OVERRIDE", false) && !Cfg->contains("IF_UNSAVED"));   // the package's: over the mounted zip
    const ordered_json *Prefs = Find("FileEdit", "FILE", "%UserProfile%/Saved Games/X/prefs.ini");
    CHECK(Prefs && Prefs->value("OVERRIDE", false) && Prefs->value("IF_UNSAVED", false));   // the user's: a default
    const ordered_json *Poke = Find("BinaryPatch", "FILE", "%GameDir%/g.exe");
    CHECK(Poke && !Poke->contains("OVERRIDE") && !Poke->contains("IF_UNSAVED"));
    const ordered_json *Reg = Find("RegEdit", "REGPATH", "HKCU\\Software\\A");
    CHECK(Reg && (*Reg)["KEYVALUES"].size() == 2 && (*Reg)["ARCHITECTURE"] == "32");   // one RegEdit per key
    CHECK(Find("DllOverride", "DLLOVERRIDE", "d3d8=n,b"));
    CHECK(Find("CustomVar", "KEY", "W"));
    const ordered_json *Keep = Find("DeclarePersist", "SCOPE", "file");
    CHECK(Keep && (*Keep)["PATH"] == "%UserProfile%/Saved Games/X/" && (*Keep)["TARGET"] == "X");
    int Persists = 0;
    for (const auto &O : Ops) if (O.value("TYPE", std::string()) == "DeclarePersist") ++Persists;
    CHECK_EQ(Persists, 1);                                              // a KEEP false persists nothing
}

TEST(a_registry_value_taken_back_inside_a_kept_key_is_reapplied)
{
    // AoE2: the user keeps TC's key (music/sound volume) while UserPatch's options in it stay the package's (they come
    // from the pre-launch options). The kept value is a base default under the user's restored state; the value taken
    // back goes to the key's OVERRIDE RegEdit, re-applied after the restore. Teeth: judge the key, not the value
    // (both values land in one base RegEdit and the user's stored option wins over the picker).
    const ordered_json Game = { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"REG", {{"HKCU", {{"Software", {{"K", {{"Music Volume", "dword:00000001"}, {"Numeric Age", "dword:00000000"}}}}}}}}}},
        {{"KEEP", {{"REG/HKCU/Software/K", true}, {"REG/HKCU/Software/K/Numeric Age", false}}}},
    })} };
    Fold::Library Lib;
    Lib.Nodes["g"] = { &Game, "/b" };
    const ordered_json Ops = NodeLower::LowerPlan(Fold::Resolve(Lib, "g"));
    const ordered_json *Base = nullptr, *Over = nullptr;
    for (const auto &O : Ops)
        if (O.value("TYPE", std::string()) == "RegEdit" && O.value("REGPATH", std::string()) == "HKCU\\Software\\K")
            (O.value("OVERRIDE", false) ? Over : Base) = &O;
    CHECK(Base && Over);
    CHECK(Base && (*Base)["KEYVALUES"] == ordered_json({{"Music Volume", "dword:00000001"}}));
    CHECK(Over && (*Over)["KEYVALUES"] == ordered_json({{"Numeric Age", "dword:00000000"}}));
    int Persists = 0;
    for (const auto &O : Ops) if (O.value("TYPE", std::string()) == "DeclarePersist") ++Persists;
    CHECK_EQ(Persists, 1);                                              // the key; the value taken back persists nothing
}

// EVAL is honoured only as a boolean true; anything else (a string from a foreign or unvalidated node) is ignored,
// never thrown out of the resolve. Teeth: read it with .value("EVAL", false) (type_error on "true").
TEST(eval_only_a_boolean_true_evaluates)
{
    const nlohmann::ordered_json D = { {"a", {{"DEFAULT", "1+1"}, {"EVAL", true}}}, {"b", {{"DEFAULT", "1+1"}, {"EVAL", "true"}}},
                                       {"c", {{"DEFAULT", "1+1"}, {"EVAL", 1}}} };
    Fold::Vars V;
    bool Threw = false;
    try { V = Fold::ResolveVars(D, {}, {}); } catch (...) { Threw = true; }
    CHECK(!Threw);
    CHECK_EQ(V["a"], std::string("2"));
    CHECK_EQ(V["b"], std::string("1+1"));
    CHECK_EQ(V["c"], std::string("1+1"));
}

TEST(lower_plan_mounts_only_what_take_selects_where_it_lands)
{
    // lib's zip mounts at bin/; mid takes one file of it (renamed) and a directory, and lib's own a.dll (renamed),
    // placing lib at lib/; g takes the contents of lib/ to its game folder. Each layer mounts only what the takes
    // select, each piece where the takes put it: submounts "path in the layer:where it lands".
    const ordered_json Lib = { {"CID", "lib"}, {"LABEL", "lib"}, {"LAYERS", ordered_json::array({
        {{"ZIP", "lib.zip"}, {"TARGET", "FILES/bin"}},
        {{"FILE", "a.dll"}},
        {{"DIR", "x"}, {"TARGET", "FILES/other"}} })} };
    const ordered_json Mid = { {"CID", "mid"}, {"LABEL", "mid"}, {"LAYERS", ordered_json::array({
        {{"NODE", "lib"}, {"TAKE", ordered_json::array({ ordered_json::array({"FILES/bin/a.dll", "FILES/b.dll"}), "FILES/bin/save",
                                                         ordered_json::array({"FILES/a.dll", "FILES/c.dll"}) })}, {"TARGET", "FILES/lib"}} })} };
    const ordered_json G = { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"NODE", "mid"}, {"TAKE", ordered_json::array({ "FILES/lib/" })}, {"TARGET", "FILES/%GameDir%"}} })} };
    Fold::Library L;
    L.Nodes["lib"] = { &Lib, "/b" };
    L.Nodes["mid"] = { &Mid, "/b" };
    L.Nodes["g"] = { &G, "/b" };
    const ordered_json Ops = NodeLower::LowerPlan(Fold::Resolve(L, "g"));
    CHECK_EQ(Ops.size(), (size_t)2);                                                 // the untaken dir mounts nothing
    const ordered_json &Zip = Ops[0];
    CHECK(Zip["TYPE"] == "VFSZipLayer" && Zip["PATH"] == "/b/lib.zip");
    CHECK(Zip["SUBMOUNTS"] == ordered_json::array({ "a.dll:%GameDir%/b.dll", "save:%GameDir%/save" }));
    const ordered_json &File = Ops[1];                                                // a FILE: its dir, the one file submounted
    CHECK(File["TYPE"] == "VFSDirLayer" && File["PATH"] == "/b");
    CHECK(File["SUBMOUNTS"] == ordered_json::array({ "a.dll:%GameDir%/c.dll" }));
    // Without a take nothing is submounted: the layer mounts whole at its placement.
    const ordered_json Whole = { {"CID", "w"}, {"LABEL", "w"}, {"LAYERS", ordered_json::array({
        {{"NODE", "lib"}, {"TARGET", "FILES/%GameDir%"}} })} };
    L.Nodes["w"] = { &Whole, "/b" };
    const ordered_json W = NodeLower::LowerPlan(Fold::Resolve(L, "w"));
    CHECK_EQ(W.size(), (size_t)3);
    CHECK(!W[0].contains("SUBMOUNTS") && W[0]["TARGET"] == "%GameDir%/bin");
}

TEST(guest_paths_map_through_a_runners_drives)
{
    // A guest path (an anchor already resolved: C:\\802\\g.exe, separators normalized) lands where its drive lives.
    const ordered_json Drives = { {"C:", "%PrefixRoot%/drive_c"}, {"D:", "%PrefixRoot%/media"} };
    CHECK_EQ(Fold::ToLayout("C:/802/g.exe", Drives), std::string("%PrefixRoot%/drive_c/802/g.exe"));
    CHECK_EQ(Fold::ToLayout("c:/x", Drives), std::string("%PrefixRoot%/drive_c/x"));            // drives case-insensitive
    CHECK_EQ(Fold::ToLayout("D:/track.ogg", Drives), std::string("%PrefixRoot%/media/track.ogg"));
    CHECK_EQ(Fold::ToLayout("client/x.jar", Drives), std::string("client/x.jar"));             // not a guest path
    CHECK_EQ(Fold::ToLayout("C:x", Drives), std::string("C:x"));                               // not under the drive
}

TEST(captured_guest_paths_are_respelled_by_their_most_specific_anchor)
{
    const std::map<std::string, std::string> A = { {"%GameDir%", "C:\\802"}, {"%ProgramFiles%", "C:\\Program Files"},
                                                   {"%ProgramFiles32%", "C:\\Program Files (x86)"}, {"%Windows%", "C:\\windows"},
                                                   {"%SysDir32%", "C:\\windows\\syswow64"}, {"%Media%", "E:\\"} };
    CHECK_EQ(Fold::ToAnchors("C:\\Program Files (x86)\\LAV\\x.ax", A), std::string("%ProgramFiles32%\\LAV\\x.ax"));  // longest wins
    CHECK_EQ(Fold::ToAnchors("c:/WINDOWS/SysWOW64/a.dll", A), std::string("%SysDir32%/a.dll"));          // case, separators
    CHECK_EQ(Fold::ToAnchors("C:\\windows\\win.ini", A), std::string("%Windows%\\win.ini"));
    CHECK_EQ(Fold::ToAnchors("C:\\802", A), std::string("%GameDir%"));                                  // the whole value
    CHECK_EQ(Fold::ToAnchors("C:\\8020\\x", A), std::string("C:\\8020\\x"));                        // not a prefix at a boundary
    CHECK_EQ(Fold::ToAnchors("\"C:\\802\\u.exe\" /S", A), std::string("\"%GameDir%\\u.exe\" /S"));   // inside a command line
    CHECK_EQ(Fold::ToAnchors("RunDll32 C:\\windows\\syswow64\\x.dll,Go", A), std::string("RunDll32 %SysDir32%\\x.dll,Go"));
    CHECK_EQ(Fold::ToAnchors("E:\\", A), std::string("%Media%"));                                       // a drive-root anchor
    CHECK_EQ(Fold::ToAnchors("D:\\x", A), std::string("D:\\x"));                                        // under no anchor: as written
    CHECK_EQ(Fold::ToAnchors("dword:0000C:\\802", A), std::string("dword:0000C:\\802"));               // not at a path start
}

// ---- the validator ----------------------------------------------------------------------------------------------

TEST(validate_names_missing_refs_and_cycles)
{
    NodeIndex Idx;
    AddChain(Idx, "a", { NF::Content("zip", "a.zip") }, { "nowhere" });
    Add(Idx, { {"CID", "p"}, {"LABEL", "p"}, {"LAYERS", ordered_json::array({ {{"NODE", "q"}} })} });
    Add(Idx, { {"CID", "q"}, {"LABEL", "q"}, {"LAYERS", ordered_json::array({ {{"NODE", "p"}} })} });
    ManifestModel::DeriveFacts(Idx);
    const auto E = Validate(Idx);
    CHECK(AnyContains(E, "contains missing node 'nowhere'"));
    CHECK(AnyContains(E, "form a cycle"));
}

TEST(validate_requires_a_variant_to_present_a_tile_it_recommends)
{
    NodeIndex Idx;
    AddChain(Idx, "untiled", { NF::Merge({ NF::Exec("win32", "g.exe"), NF::Variant("x") }) });
    AddChain(Idx, "wrongrec", { NF::Merge({ NF::Exec("win32", "g.exe"), NF::Tile("5"), NF::Variant("y") }) }, {},
             { {"RECOMMENDED", ordered_json::array({"6"})} });
    AddChain(Idx, "norun", { NF::Merge({ NF::Tile("5"), NF::Variant("z") }) });
    ManifestModel::DeriveFacts(Idx);
    const auto E = Validate(Idx);
    CHECK(AnyContains(E, "'untiled': VARIANT 'x' presents no tile"));
    CHECK(AnyContains(E, "RECOMMENDED names tile '6'"));
    CHECK(AnyContains(E, "'norun': VARIANT 'z' has no effective entry"));
}

TEST(validate_refuses_an_edit_beneath_what_replaces_its_file)
{
    // A DIR provides everything under its target: an EDIT beneath it has no effect. Above it, it is fine.
    NodeIndex Covered, Fine;
    const ordered_json Edit = NF::Layers({ { {"EDIT", ordered_json::array({ {{"MODE", "Overwrite"}, {"VALUE", "x"}} })}, {"TARGET", "FILES/%GameDir%/a.cfg"} } });
    AddChain(Covered, "v", { NF::Merge({ Edit, NF::Content("dir", "d", "%GameDir%"), NF::Exec("win32", "g.exe"), NF::Tile("1"), NF::Variant("v") }) });
    AddChain(Fine, "v", { NF::Merge({ NF::Content("dir", "d", "%GameDir%"), Edit, NF::Exec("win32", "g.exe"), NF::Tile("1"), NF::Variant("v") }) });
    ManifestModel::DeriveFacts(Covered);
    ManifestModel::DeriveFacts(Fine);
    CHECK(AnyContains(Validate(Covered), "lies beneath"));
    CHECK(!AnyContains(Validate(Fine), "lies beneath"));
}

TEST(validate_refuses_a_delta_with_nothing_beneath_it)
{
    NodeIndex Idx, Based;
    AddChain(Idx, "v", { NF::Merge({ NF::Content("delta", "d.vgdelta", "%GameDir%"), NF::Exec("win32", "g.exe"), NF::Tile("1"), NF::Variant("v") }) });
    AddChain(Based, "v", { NF::Merge({ NF::Content("zip", "b.zip", "%GameDir%"), NF::Content("delta", "d.vgdelta", "%GameDir%"),
                                       NF::Exec("win32", "g.exe"), NF::Tile("1"), NF::Variant("v") }) });
    ManifestModel::DeriveFacts(Idx);
    ManifestModel::DeriveFacts(Based);
    CHECK(AnyContains(Validate(Idx), "has no content beneath it"));
    CHECK(!AnyContains(Validate(Based), "has no content beneath it"));
}

TEST(validate_a_layer_when_may_only_read_ungated_variables)
{
    NodeIndex Idx;
    Add(Idx, { {"CID", "n"}, {"LABEL", "n"}, {"LAYERS", ordered_json::array({
        {{"VARS", {{"mode", {{"DEFAULT", "a"}}}}}, {"WHEN", "%other%==1"}},           // mode: declared only gated
        {{"VARS", {{"other", {{"DEFAULT", "1"}}}}}},
        {{"ZIP", "x.zip"}, {"WHEN", "%mode%==a"}},                                    // reads it → phase-1 violation
        {{"ZIP", "y.zip"}, {"WHEN", "%other%==1"}},                                   // fine
    })} });
    const auto E = Validate(Idx);
    CHECK(AnyContains(E, "reads %mode%"));
    CHECK(!AnyContains(E, "reads %other%"));
}

TEST(validate_names_undefined_variables_and_malformed_whens)
{
    NodeIndex Idx;
    Add(Idx, { {"CID", "n"}, {"LABEL", "n"}, {"LAYERS", ordered_json::array({
        {{"ZIP", "%typo_var%.zip"}},
        {{"DIR", "d"}, {"WHEN", "%a% === 1"}},
        {{"EDIT", ordered_json::array({ {{"MODE", "Poke"}, {"OFFSET", "0x1"}, {"VALUE", "01"}, {"COMMENT", "keeps '%prose%'"}} })},
         {"TARGET", "FILES/%GameDir%/g.exe"}, {"COMMENT", "%also_prose%"}, {"LABEL", "%label_prose%"}},
    })} });
    const auto E = Validate(Idx);
    CHECK(!AnyContains(E, "%prose%"));                                  // a COMMENT is prose, not a reference
    CHECK(!AnyContains(E, "%also_prose%"));
    CHECK(!AnyContains(E, "%label_prose%"));
    CHECK(AnyContains(E, "undefined variable %typo_var%"));
    CHECK(AnyContains(E, "malformed WHEN"));
}

TEST(validate_refuses_drive_letters_and_non_canonical_anchors)
{
    // A package names places by anchor only, and each place by its MOST specific anchor; the runner's own map
    // (GUEST_ROOTS, DRIVES) is where drive letters live.
    NodeIndex Idx;
    ordered_json R = NF::Runner("linux64", {"win32"}, "%RunnerMount%/proton");
    R["LAYERS"][0]["EXEC"][0]["GUEST_ROOTS"] = { {"%GameDir%", "C:\\%PackageUID%"}, {"%Windows%", "C:\\windows"},
                                                 {"%SysDir32%", "C:\\windows\\syswow64"} };
    R["LAYERS"][0]["EXEC"][0]["DRIVES"] = { {"C:", "%PrefixRoot%/drive_c"} };
    AddChain(Idx, "proton", { R });
    Add(Idx, { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"ZIP", "g.zip"}, {"TARGET", "FILES/C:/%PackageUID%"}},                              // a drive in a path
        {{"REG", {{"HKLM", {{"Software", {{"G", {{"CDPath", "E:\\"}}}}}}}}}},                   // …and in a value
        {{"FILE", "a.dll"}, {"TARGET", "FILES/%Windows%/syswow64/a.dll"}},                     // %SysDir32% spelled long
        {{"FILE", "b.ini"}, {"TARGET", "FILES/%Windows%/win.ini"}},                            // fine: nothing narrower
        {{"FILE", "c.dll"}, {"TARGET", "FILES/%SysDir32%/c.dll"}},                             // fine
        {{"REG", {{"HKLM", {{"Software", {{"G", {{"Path", "%GameDir%\\bin"}}}}}}}}}},            // fine
    })} });
    const auto E = Validate(Idx);
    CHECK(AnyContains(E, "'FILES/C:/%PackageUID%' names a drive"));
    CHECK(AnyContains(E, "'E:\\' names a drive"));
    CHECK(AnyContains(E, "'FILES/%Windows%/syswow64/a.dll' is inside %SysDir32%"));
    CHECK(!AnyContains(E, "win.ini"));
    CHECK(!AnyContains(E, "c.dll"));
    CHECK(!AnyContains(E, "%GameDir%\\bin"));
    CHECK(!AnyContains(E, "node 'proton'"));                                                   // the map is not a use
}

// Every anchor in a value is held to its most specific spelling — the second one too ("%GameDir%\\a;%Windows%\\
// syswow64\\x" names a %SysDir32% place). Teeth: let the anchor's tail run past the next token ([^"]*) and the second
// anchor is never looked at.
TEST(validate_holds_every_anchor_in_a_value_to_its_most_specific_spelling)
{
    NodeIndex Idx;
    ordered_json R = NF::Runner("linux64", {"win32"}, "%RunnerMount%/proton");
    R["LAYERS"][0]["EXEC"][0]["GUEST_ROOTS"] = { {"%GameDir%", "C:\\%PackageUID%"}, {"%Windows%", "C:\\windows"},
                                                 {"%SysDir32%", "C:\\windows\\syswow64"} };
    R["LAYERS"][0]["EXEC"][0]["DRIVES"] = { {"C:", "%PrefixRoot%/drive_c"} };
    AddChain(Idx, "proton", { R });
    Add(Idx, { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"REG", {{"HKLM", {{"Software", {{"G", {{"Paths", "%GameDir%\\a;%Windows%\\syswow64\\x.dll"}}}}}}}}}},
    })} });
    CHECK(AnyContains(Validate(Idx), "is inside %SysDir32%"));
}

TEST(validate_places_anchors_at_the_start_of_paths_and_reads_every_value)
{
    NodeIndex Idx;
    ordered_json R = NF::Runner("linux64", {"win32"}, "%RunnerMount%/proton");
    R["LAYERS"][0]["EXEC"][0]["GUEST_ROOTS"] = { {"%GameDir%", "C:\\%PackageUID%"}, {"%ProgramFiles32%", "C:\\Program Files (x86)"} };
    R["LAYERS"][0]["EXEC"][0]["DRIVES"] = { {"C:", "%PrefixRoot%/drive_c"} };
    AddChain(Idx, "proton", { R });
    Add(Idx, { {"CID", "g"}, {"LABEL", "g"}, {"LAYERS", ordered_json::array({
        {{"ZIP", "g.zip"}, {"TARGET", "FILES/mods/%GameDir%/x"}},                              // anchor mid-path
        {{"ZIP", "h.zip"}, {"SUBMOUNTS", ordered_json::array({"a:b/%GameDir%"})}},             // …in a submount
        {{"KEEP", {{"FILES/saves/%GameDir%/", true}}}},                                         // …in a KEEP
        {{"REG", {{"HKLM", {{"Software", {{"G", {{"CD", "E:"}}}}}}}}}},                          // a bare drive value
        {{"EDIT", ordered_json::array({ {{"MODE", "ConfigWrite"}, {"KEY", "/W:"}, {"VALUE", "1"}} })}, {"TARGET", "FILES/%GameDir%/c.cfg"}},  // a key, not a drive
        {{"REG", {{"HKLM", {{"Software", {{"G", {{"U", "RunDll32 %ProgramFiles32%\\x.dll"}}}}}}}}}, // a value may embed one
         {"COMMENT", "the installer wrote C:\\Program Files (x86)\\G"}},                     // prose is not a place
        {{"EXEC", ordered_json::array({ {{"LABEL", "Play"}, {"EXE", "%GameDir%/g.exe"}, {"WORKDIR", "bin/%GameDir%"}} })}},
        // TAKE destinations: with the namespace or without (Fold::Taken adds it) the path starts at the anchor —
        // only an anchor INSIDE the path is refused.
        {{"NODE", "lib"}, {"TAKE", ordered_json::array({ ordered_json::array({"FILES/a/", "FILES/%GameDir%/a"}),
                                                          ordered_json::array({"FILES/b/", "%ProgramFiles32%/b"}),
                                                          ordered_json::array({"FILES/c/", "FILES/mods/%GameDir%"}) })}},
    })} });
    Add(Idx, { {"CID", "lib"}, {"LABEL", "lib"}, {"LAYERS", ordered_json::array({ {{"ZIP", "l.zip"}} })} });
    const auto E = Validate(Idx);
    CHECK(!AnyContains(E, "path 'FILES/%GameDir%/a'"));
    CHECK(!AnyContains(E, "path '%GameDir%/a'"));
    CHECK(!AnyContains(E, "%ProgramFiles32%/b' has"));
    CHECK(AnyContains(E, "path 'mods/%GameDir%' has %GameDir% inside it"));
    CHECK(AnyContains(E, "path 'mods/%GameDir%/x' has %GameDir% inside it"));
    CHECK(AnyContains(E, "path 'b/%GameDir%' has %GameDir% inside it"));
    CHECK(AnyContains(E, "path 'saves/%GameDir%/' has %GameDir% inside it"));
    CHECK(AnyContains(E, "path 'bin/%GameDir%' has %GameDir% inside it"));
    CHECK(!AnyContains(E, "path '%GameDir%/g.exe'"));
    CHECK(AnyContains(E, "'E:' names a drive"));
    CHECK(!AnyContains(E, "'/W:'"));
    CHECK(!AnyContains(E, "RunDll32"));
    CHECK(!AnyContains(E, "installer wrote"));
    // Two runners that place an anchor differently: packages are checked against one of them — said out loud.
    ordered_json R2 = NF::Runner("linux64", {"win64"}, "%RunnerMount%/other");
    R2["LAYERS"][0]["EXEC"][0]["GUEST_ROOTS"] = { {"%GameDir%", "D:\\%PackageUID%"} };
    R2["LAYERS"][0]["EXEC"][0]["DRIVES"] = { {"D:", "%PrefixRoot%/drive_d"} };
    AddChain(Idx, "other", { R2 });
    std::vector<std::string> W;
    Validate(Idx, &W);
    CHECK(AnyContains(W, "runners disagree where %GameDir% is"));
}

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
    MakeZip((Dir / "c.zip").string(), /*Stored=*/false);
    MakeZip((Dir / "s.zip").string(), /*Stored=*/true);
    {
        NodeIndex Idx;
        AddChain(Idx, "z", { NF::Content("zip", "c.zip") }, {}, ordered_json::object(), Dir.string());
        CHECK(AnyContains(Validate(Idx), "STORE"));
    }
    {
        NodeIndex Idx;
        AddChain(Idx, "z", { NF::Content("zip", "s.zip") }, {}, ordered_json::object(), Dir.string());
        CHECK(!AnyContains(Validate(Idx), "STORE"));
    }
    std::error_code Ec; fs::remove_all(Dir, Ec);
}

TEST(validate_warns_unzipped_dir_layer)
{
    NodeIndex Idx;
    AddChain(Idx, "d", { NF::Content("dir", "payload") });
    std::vector<std::string> W;
    const auto E = Validate(Idx, &W);
    CHECK(AnyContains(W, "unzipped authoring layer"));
    CHECK(!AnyContains(E, "unzipped"));
}

TEST(validate_flags_cross_layer_case_collision_once)
{
    namespace fs = std::filesystem;
    fs::path Dir = fs::temp_directory_path() / ("vgtest_case_" + std::to_string(::getpid()));
    fs::remove_all(Dir);
    fs::create_directories(Dir / "base" / "MAPS");
    fs::create_directories(Dir / "patch" / "maps");
    { std::ofstream F(Dir / "base" / "MAPS" / "level.dat");  F << "a"; }
    { std::ofstream F(Dir / "patch" / "maps" / "LEVEL.dat"); F << "b"; }
    NodeIndex Idx;
    AddChain(Idx, "shared", { NF::Content("dir", "base"), NF::Content("dir", "patch") }, {}, ordered_json::object(), Dir.string());
    AddChain(Idx, "gameA", { NF::Merge({ NF::Tile("cA", "A"), NF::Exec("win32", "base/MAPS/level.dat"), NF::Variant("a") }) }, { "shared" },
             ordered_json::object(), Dir.string());
    AddChain(Idx, "gameB", { NF::Merge({ NF::Tile("cB", "B"), NF::Exec("win32", "base/MAPS/level.dat"), NF::Variant("b") }) }, { "shared" },
             ordered_json::object(), Dir.string());
    ManifestModel::DeriveFacts(Idx);
    int CaseReports = 0;
    for (const std::string &E : Validate(Idx))
        if (E.find("case conflict across layers") != std::string::npos) ++CaseReports;
    CHECK_EQ(CaseReports, 1);                      // fires, and the shared content reports once, not per launchable
    fs::remove_all(Dir);
}

// ---- helpers shared with the FS and the runner chain ---------------------------------------------------------------

TEST(runtime_sourced_layer_separates_assembly_from_build)
{
    auto Lyr = [](const char *T, const std::string &P) { return ordered_json{{"TYPE", T}, {"PATH", P}}; };
    CHECK(ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSDirLayer", "%DefaultPfxDir%")));
    CHECK(ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSDirLayer", "%RunnerMount%/files/share/default_pfx")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "rt.zip")));
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "100%25%20done.zip")));   // escapes are not variables
    CHECK(!ManifestModel::IsRuntimeSourcedLayer(Lyr("VFSZipLayer", "a%1%b.zip")));
    CHECK(ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSZipLayer"}, {"PATH", "rt.zip"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "VFSDirLayer"}, {"PATH", "%DefaultPfxDir%"}}));
    CHECK(!ManifestModel::IsRunnerBuildLayer(ordered_json{{"TYPE", "RegEdit"}}));
}

TEST(normalize_target_path_parity_with_fs)
{
    const char *SlashCases[] = { "", "/", "a", "/a", "a/", "/a/b/", "a/b/c", "//x//", "a/b//" };
    for (const char *C : SlashCases)
        CHECK_EQ(ManifestModel::NormalizeTargetPath(C), NormalizeVPath(C));
    CHECK_EQ(ManifestModel::NormalizeTargetPath("a\\b\\c\\"), std::string("a/b/c"));
    CHECK_EQ(ManifestModel::NormalizeTargetPath("\\pfx\\drive_c"), std::string("pfx/drive_c"));
}

// ---- identity -----------------------------------------------------------------------------------------------------

TEST(identity_is_the_kubo_cid_of_the_canonical_bytes)
{
    // SHA-256 known answers (FIPS 180-4): "abc", and a two-block message.
    auto Hex = [](const std::string &B) { std::string H; for (unsigned char C : B) { char X[3]; std::snprintf(X, 3, "%02x", C); H += X; } return H; };
    CHECK_EQ(Hex(Cid::Sha256("abc")), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Hex(Cid::Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(Hex(Cid::Sha256("")), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    // Kubo: `ipfs add --cid-version=1` of "hello\n" (one raw block).
    CHECK_EQ(Cid::OfBytes("hello\n"), std::string("bafkreicysg23kiwv34eg2d7qweipxwosdo2py4ldv42nbauguluen5v6am"));
    // Canonical bytes: keys sorted, compact, the node's own CID and POS left out.
    const ordered_json N = { {"LAYERS", ordered_json::array()}, {"CID", "x"}, {"POS", ordered_json::array({1, 2})}, {"LABEL", "é"} };
    CHECK_EQ(Cid::Canonical(N), std::string("{\"LABEL\":\"é\",\"LAYERS\":[]}"));
    CHECK_EQ(Cid::OfNode(N), Cid::OfBytes("{\"LABEL\":\"é\",\"LAYERS\":[]}"));
    // Past one 256 KiB block a node is a chunked UnixFS file — refused by name, never a wrong CID.
    std::string Err;
    CHECK(Cid::OfBytes(std::string(256 * 1024 + 1, 'x'), &Err).empty());
    CHECK(!Err.empty());
}

// §4.2: a node mentioned twice counts once — at its FIRST mention (held), or at a later mention in the same list
// (moved). The validator warns where that rule, not the author, picked a winner: the other reading must change what
// wins, and a reordering of layers at unrelated targets does not.
TEST(deciding_mentions_are_the_ones_whose_position_picks_a_winner)
{
    const auto Zip = [](const char *F, const char *T) { return ordered_json{{"ZIP", F}, {"TARGET", T}}; };
    const auto N = [](ordered_json Layers) { return ordered_json{{"LABEL", "n"}, {"LAYERS", std::move(Layers)}}; };
    const ordered_json A = N(ordered_json::array({ Zip("a.zip", "FILES/%GameDir%") }));
    // held, deciding: B puts its own zip at A's target and mentions A AFTER it — folded there, A would win
    const ordered_json B1 = N(ordered_json::array({ Zip("b.zip", "FILES/%GameDir%"), {{"NODE", "a"}} }));
    // held, not deciding: B's zip is elsewhere
    const ordered_json B2 = N(ordered_json::array({ Zip("b.zip", "FILES/%Documents%/other"), {{"NODE", "a"}} }));
    const ordered_json V1 = N(ordered_json::array({ {{"NODE", "a"}}, {{"NODE", "b1"}} }));
    const ordered_json V2 = N(ordered_json::array({ {{"NODE", "a"}}, {{"NODE", "b2"}} }));
    // moved, deciding: V3's own zip sits between the two mentions of A at A's target
    const ordered_json V3 = N(ordered_json::array({ {{"NODE", "a"}}, Zip("v.zip", "FILES/%GameDir%/sub"), {{"NODE", "a"}} }));
    // a held fact: B3 sets ENV X after A's value; holding A keeps B3's
    const ordered_json EA = N(ordered_json::array({ {{"ENV", {{"X", "a"}}}} }));
    const ordered_json B3 = N(ordered_json::array({ {{"ENV", {{"X", "b"}}}}, {{"NODE", "ea"}} }));
    const ordered_json V4 = N(ordered_json::array({ {{"NODE", "ea"}}, {{"NODE", "b3"}} }));
    Fold::Library L;
    for (const auto &[K, J] : std::vector<std::pair<std::string, const ordered_json *>>{
             {"a", &A}, {"b1", &B1}, {"b2", &B2}, {"v1", &V1}, {"v2", &V2}, {"v3", &V3}, {"ea", &EA}, {"b3", &B3}, {"v4", &V4}})
        L.Nodes[K] = { J, "/b" };
    const auto Deciding = [&](const std::string &Root) { return Fold::DecidingMentions(L, Root, {}, {}, {}, Fold::Resolve(L, Root)); };
    using Ev = std::vector<std::pair<std::string, std::string>>;
    CHECK(Deciding("v1") == Ev({ {"held", "a"} }));
    CHECK(Deciding("v2").empty());                                   // held, but nothing it overlaps sits in between
    CHECK(Deciding("v3") == Ev({ {"move", "a"} }));
    CHECK(Deciding("v4") == Ev({ {"held", "ea"} }));
    // the rule itself is unchanged: the first mention holds, a same-list mention moves
    const Fold::Plan P1 = Fold::Resolve(L, "v1");
    CHECK(P1.Seq.size() == 2 && P1.Seq[0].Payload == "a.zip" && P1.Seq[1].Payload == "b.zip");
    const Fold::Plan P3 = Fold::Resolve(L, "v3");
    CHECK(P3.Seq.size() == 2 && P3.Seq[0].Payload == "v.zip" && P3.Seq[1].Payload == "a.zip");
}
