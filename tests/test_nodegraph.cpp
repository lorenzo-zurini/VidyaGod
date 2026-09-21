#include "vgtest.h"
#include "nodegraph.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::ordered_json;

// Teeth for the gigagraph PURE transforms (nodegraphpure.cpp): link normalize ↔ linkify, POS strip + handle
// resolution + linkify in FreezeNodeJson, and deps-first topo order with cycle detection. Each assertion fails if the
// corresponding behavior is removed or inverted. The end-to-end CID determinism is proven separately in Go
// (VidyaGodIPFS/dag_test.go); these cover the C++ side that shapes what gets hashed.

static int IndexOf(const std::vector<std::string> &V, const std::string &S)
{
    for (size_t i = 0; i < V.size(); ++i) if (V[i] == S) return (int)i;
    return -1;
}

TEST(nodegraph_normalizelinks_collapses_link_objects)
{
    ordered_json J = {
        {"LABEL", "x"},
        {"PARENTS", ordered_json::array({ {{"/", "cidA"}}, {{"/", "cidB"}} })},
        {"LIBRARYITEM", {{"/", "cidTile"}}},
        {"SOURCE", {{"TYPE", "ipfs"}, {"CID", {{"/", "cidContent"}}}, {"SIZE", 5}}},
    };
    NodeGraph::NormalizeLinks(J);
    CHECK(J["PARENTS"][0] == "cidA");
    CHECK(J["PARENTS"][1] == "cidB");
    CHECK(J["LIBRARYITEM"] == "cidTile");
    CHECK(J["SOURCE"]["CID"] == "cidContent");   // nested link collapsed
    CHECK(J["SOURCE"]["SIZE"] == 5);             // non-link sibling untouched
}

TEST(nodegraph_normalizelinks_leaves_nonlinks_alone)
{
    // A byte value {"/":{"bytes":...}} is NOT a link (non-string "/") and must survive; a plain object too.
    ordered_json J = {
        {"BYTES", {{"/", {{"bytes", "abcd"}}}}},
        {"NESTED", {{"a", 1}, {"b", 2}}},
        {"SLASH_PLUS", {{"/", "notalink"}, {"extra", 1}}},   // 2 keys ⇒ not a link
    };
    NodeGraph::NormalizeLinks(J);
    CHECK(J["BYTES"]["/"].is_object());
    CHECK(J["NESTED"]["a"] == 1);
    CHECK(J["SLASH_PLUS"]["/"] == "notalink");   // still an object, not collapsed
    CHECK(J["SLASH_PLUS"]["extra"] == 1);
}

TEST(nodegraph_freeze_strips_pos_resolves_and_linkifies)
{
    std::map<std::string, std::string> HandleToCid = {
        {"content_node", "cidContent"},
        {"tile_node", "cidTile"},
    };
    ordered_json Raw = {
        {"CID", "premint-9"},                                   // the working-tree handle (last-minted CID / draft)
        {"LABEL", "exec"},
        {"TYPE", "DeclareExec"},
        {"POS", ordered_json::array({100.0, 200.0})},
        {"PUBLISH", true},
        {"PARENTS", ordered_json::array({"content_node", "external_already_a_cid"})},
        {"LIBRARYITEM", "tile_node"},
        {"COVER", {{"PATH", "c.png"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "cidCover"}, {"SIZE", 9}}}}},
    };
    const ordered_json F = NodeGraph::FreezeNodeJson(Raw, HandleToCid);

    CHECK(!F.contains("POS"));                                   // non-semantic canvas coords dropped
    CHECK(!F.contains("CID"));                                   // the node's OWN handle is STRIPPED (a block can't
                                                               // contain its own hash) → identity stays purely the
                                                               // hash of {TYPE, refs→CIDs, LABEL, payload}
    CHECK(F.contains("PUBLISH") && F["PUBLISH"] == true);       // PUBLISH is minted IN (identity-bearing): unlike POS it
                                                               // is NOT stripped, so shareability travels with the node
    // intra-tree handle → CID, then linkified; external ref passes through, linkified
    CHECK(F["PARENTS"][0] == ordered_json({{"/", "cidContent"}}));
    CHECK(F["PARENTS"][1] == ordered_json({{"/", "external_already_a_cid"}}));
    CHECK(F["LIBRARYITEM"] == ordered_json({{"/", "cidTile"}}));
    CHECK(F["COVER"]["SOURCE"]["CID"] == ordered_json({{"/", "cidCover"}}));   // nested SOURCE.CID linkified
    CHECK(F["LABEL"] == "exec");                              // identity label preserved
}

TEST(nodegraph_freeze_then_normalize_roundtrips)
{
    std::map<std::string, std::string> HandleToCid = {{"dep", "cidDep"}};
    ordered_json Raw = {
        {"LABEL", "n"}, {"TYPE", "Content"},
        {"PARENTS", ordered_json::array({"dep"})},
        {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "cidBytes"}, {"SIZE", 1}}},
    };
    ordered_json F = NodeGraph::FreezeNodeJson(Raw, HandleToCid);
    NodeGraph::NormalizeLinks(F);
    CHECK(F["PARENTS"][0] == "cidDep");        // handle resolved, link collapsed back to a plain CID
    CHECK(F["SOURCE"]["CID"] == "cidBytes");
}

TEST(nodegraph_lift_libraryitem_edge_direct)
{
    std::map<std::string, ordered_json> Tree = {
        {"tile", {{"LABEL", "tile"}, {"TYPE", "DeclareLibraryItem"}, {"TITLE", "X"}, {"UID", "42"}}},
        {"exec", {{"LABEL", "exec"}, {"TYPE", "DeclareExec"},
                  {"PARENTS", ordered_json::array({"persist", "tile", "content"})}}},
        {"content", {{"LABEL", "content"}, {"TYPE", "Content"}}},
        {"persist", {{"LABEL", "persist"}, {"TYPE", "DeclarePersist"}}},
    };
    NodeGraph::LiftLibraryItemEdge(Tree);
    // The tile moves out of PARENTS into LIBRARYITEM; the composition parents stay, order preserved.
    CHECK(Tree["exec"]["LIBRARYITEM"] == "tile");
    CHECK(Tree["exec"]["PARENTS"].size() == 2);
    CHECK(Tree["exec"]["PARENTS"][0] == "persist");
    CHECK(Tree["exec"]["PARENTS"][1] == "content");
    // Idempotent: a second pass changes nothing.
    const ordered_json Before = Tree["exec"];
    NodeGraph::LiftLibraryItemEdge(Tree);
    CHECK(Tree["exec"] == Before);
    CHECK(!Tree["content"].contains("LIBRARYITEM"));
}

TEST(nodegraph_lift_libraryitem_edge_transitive)
{
    // The tile is a TRANSITIVE ancestor (exec → base → tile), not a direct parent. It must still be lifted onto the
    // exec, and stripped from EVERY node's PARENTS (base's too) — the tile leaves the composition graph entirely.
    std::map<std::string, ordered_json> Tree = {
        {"tile", {{"LABEL", "tile"}, {"TYPE", "DeclareLibraryItem"}, {"TITLE", "X"}, {"UID", "7"}}},
        {"base", {{"LABEL", "base"}, {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"tile"})}}},
        {"exec", {{"LABEL", "exec"}, {"TYPE", "DeclareExec"}, {"PARENTS", ordered_json::array({"base"})}}},
    };
    NodeGraph::LiftLibraryItemEdge(Tree);
    CHECK(Tree["exec"]["LIBRARYITEM"] == "tile");          // found transitively
    CHECK(Tree["exec"]["PARENTS"].size() == 1);
    CHECK(Tree["exec"]["PARENTS"][0] == "base");
    CHECK(Tree["base"]["PARENTS"].empty());               // tile stripped from base's PARENTS too
}

TEST(nodegraph_topo_order_is_deps_first)
{
    // exec → (PARENTS) content → (PARENTS) lib ; exec → (LIBRARYITEM) tile. Deps must precede dependents.
    std::map<std::string, ordered_json> Tree = {
        {"lib",     {{"LABEL", "lib"}, {"TYPE", "Content"}}},
        {"content", {{"LABEL", "content"}, {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"lib"})}}},
        {"tile",    {{"LABEL", "tile"}, {"TYPE", "DeclareLibraryItem"}}},
        {"exec",    {{"LABEL", "exec"}, {"TYPE", "DeclareExec"},
                     {"PARENTS", ordered_json::array({"content"})}, {"LIBRARYITEM", "tile"}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
    CHECK(Order.size() == 4);
    CHECK(IndexOf(Order, "lib")     < IndexOf(Order, "content"));
    CHECK(IndexOf(Order, "content") < IndexOf(Order, "exec"));
    CHECK(IndexOf(Order, "tile")    < IndexOf(Order, "exec"));   // LIBRARYITEM is a freeze dependency too
}

TEST(nodegraph_edit_cascades_new_cids_up_the_chain)
{
    // THE Model C cascade: references are CID handles; a mint freezes leaf→root, resolving each ref to the freshly
    // computed CID of its target (FreezeNodeJson via HandleToCid). So editing a LEAF must change its CID AND every
    // ancestor's CID, and each ancestor's reference must re-point to the new child CID. We stand in a deterministic
    // fake "CID" (a hash of the frozen bytes) for IpfsWrapper::DagCid — the CASCADE LOGIC is pure (topo + resolve).
    // Teeth: if FreezeNodeJson stopped resolving handles (or topo stopped being deps-first), an ancestor would keep
    // the OLD child CID and its own CID would not move.
    auto fakeCid = [](const ordered_json &Frozen) {
        return "cid_" + std::to_string(std::hash<std::string>{}(Frozen.dump()) & 0xffffffffULL);
    };
    // Handles are the stored "CID" values; refs point to them. base(A) ← content(B) ← exec(C).
    auto mint = [&](std::map<std::string, ordered_json> Tree) {
        std::vector<std::string> Order; std::string Err;
        CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
        std::map<std::string, std::string> H2C;
        for (const std::string &H : Order)
            H2C[H] = fakeCid(NodeGraph::FreezeNodeJson(Tree.at(H), H2C));   // deps already in H2C ⇒ refs resolve
        return H2C;
    };
    auto tree = [](const std::string &basePayload) {
        return std::map<std::string, ordered_json>{
            {"A", {{"CID", "A"}, {"LABEL", "base"},    {"TYPE", "Content"}, {"PATH", "b.zip"}, {"NOTE", basePayload}}},
            {"B", {{"CID", "B"}, {"LABEL", "content"}, {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"A"})}}},
            {"C", {{"CID", "C"}, {"LABEL", "exec"},    {"TYPE", "DeclareExec"}, {"PARENTS", ordered_json::array({"B"})}}},
        };
    };
    const auto V1 = mint(tree("v1"));
    const auto V2 = mint(tree("v2"));            // ONLY the leaf A's payload changed
    CHECK(V1.at("A") != V2.at("A"));             // the edited leaf's CID moved
    CHECK(V1.at("B") != V2.at("B"));             // its parent re-minted (its ref resolved to A's NEW cid)
    CHECK(V1.at("C") != V2.at("C"));             // cascade reached the root exec
    // And an UNRELATED edit does NOT move a node that does not transitively depend on it: freezing B in V2 must embed
    // A's V2 cid, proving the ref actually re-pointed (not a coincidental hash).
    const ordered_json FB = NodeGraph::FreezeNodeJson(tree("v2").at("B"), { {"A", V2.at("A")} });
    CHECK(FB["PARENTS"][0] == ordered_json({{"/", V2.at("A")}}));   // B's parent link IS A's new cid
}

TEST(nodegraph_topo_order_breaks_cycle)
{
    // A cycle must NOT abort the order (that would empty the whole library on one self-referential typo): the back
    // edge is broken, both nodes still enter the order, and they get dropped later at freeze (unfreezable). A THIRD
    // acyclic node must survive regardless — the degrade-per-node guarantee.
    std::map<std::string, ordered_json> Tree = {
        {"a",    {{"LABEL", "a"},    {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"b"})}}},
        {"b",    {{"LABEL", "b"},    {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"a"})}}},
        {"good", {{"LABEL", "good"}, {"TYPE", "Content"}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));   // cycle is broken, not fatal
    CHECK(Order.size() == 3);                                 // all ordered; cyclic ones skip at freeze
    CHECK(IndexOf(Order, "good") >= 0);                       // the acyclic node always survives
}

TEST(nodegraph_topo_external_refs_impose_no_order)
{
    // A PARENTS ref not in the tree is an external (already-frozen) dep — it must not block ordering or error.
    std::map<std::string, ordered_json> Tree = {
        {"only", {{"LABEL", "only"}, {"TYPE", "DeclareExec"},
                  {"PARENTS", ordered_json::array({"some_external_cid"})}, {"LIBRARYITEM", "another_external_cid"}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
    CHECK(Order.size() == 1);
    CHECK(Order[0] == "only");
}

// A TEMPORARY tree dir for the scan tests below (vgtest has no fixture helper; keep it dead simple).
struct ScanDir
{
    std::filesystem::path P;
    ScanDir()  { P = std::filesystem::temp_directory_path() / ("vg_scan_" + std::to_string(::getpid()) + "_" + std::to_string(rand())); std::filesystem::create_directories(P); }
    ~ScanDir() { std::error_code Ec; std::filesystem::remove_all(P, Ec); }
    void Write(const std::string &Rel, const std::string &Bytes)
    {
        const std::filesystem::path F = P / Rel;
        std::filesystem::create_directories(F.parent_path());
        std::ofstream O(F, std::ios::binary); O << Bytes;
    }
};

TEST(nodegraph_scan_rejects_hostile_deep_json_without_crashing)
{
    // UNTRUSTED bytes live in the tree now (a received share's block lands verbatim): a deeply nested file must be
    // SKIPPED by the depth pre-scan, not parsed — nlohmann's parser and NormalizeLinks both recurse per level, so
    // without the guard this test dies by stack overflow (no assert ever fires — the crash IS the failure).
    ScanDir D;
    std::string Deep;
    for (int i = 0; i < 200000; ++i) Deep += '[';
    for (int i = 0; i < 200000; ++i) Deep += ']';
    D.Write("Evil - Lib/[x] x/bomb.json", Deep);
    // Model C: a node is KEYED by its stored "CID" handle (LABEL is cosmetic).
    D.Write("Games/[1] A/a.json", ordered_json{{"CID", "cidA"}, {"LABEL", "a_exec"}, {"TYPE", "DeclareExec"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK(Tree.count("cidA") == 1);        // keyed by the CID handle; the honest neighbour still scans
    CHECK(Tree.size() == 1);               // the bomb contributed nothing
    CHECK(!NodeGraph::JsonDepthWithinLimit(Deep, 64));
    CHECK(NodeGraph::JsonDepthWithinLimit("{\"a\":[1,2,{\"b\":3}]}", 64));
}

TEST(nodegraph_scan_survives_hostile_handle_and_keeps_handleless)
{
    // A received/working-tree file controls the "CID" handle's TYPE. A non-string handle must NOT throw the scan (that
    // aborts the GUI/worker); it reads as handle-less. A control-byte handle is rejected (unforgeable synthetic key).
    // And a legal HANDLE-LESS node (TYPE, no "CID" — a never-minted draft, or a frozen block) must still be gathered
    // under a synthetic key, never dropped. LABEL is cosmetic and is NEVER a key. Teeth: revert GatherWorkingTree's
    // is_string guard → this throws; drop the synthetic-key path → the handle-less node vanishes.
    ScanDir D;
    D.Write("A/hostile.json",  std::string("{\"TYPE\":\"Content\",\"CID\":1,\"LABEL\":\"x\"}"));            // non-string handle
    D.Write("A/nameless.json", std::string("{\"TYPE\":\"Content\",\"FORM\":\"zip\",\"PATH\":\"x.zip\"}"));  // no handle
    D.Write("A/named.json",    ordered_json{{"CID","cidN"},{"LABEL","named"},{"TYPE","Content"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);   // must NOT throw
    CHECK(Tree.count("cidN") == 1);       // keyed by its CID handle
    // hostile + handle-less land under synthetic keys (never a real handle). Total gathered = 3.
    CHECK((int)Tree.size() == 3);
    CHECK(Tree.count("1") == 0);          // the number handle did NOT become a "1" key
    CHECK(Tree.count("x") == 0);          // the cosmetic LABEL is NEVER a key
}

TEST(nodegraph_duplicate_LABEL_is_fine_distinct_handles_both_kept)
{
    // THE Model C win: LABEL is cosmetic, so two DIFFERENT nodes sharing a label (RoC/TFT "v1.21b") are NOT a
    // collision at all — they have distinct CID handles and BOTH gather normally. Teeth: if anything still keyed on
    // LABEL, one of these would evict the other.
    ScanDir D;
    D.Write("Games/[1] RoC/roc.json", ordered_json{{"CID","cidRoC"},{"LABEL","v1.21b"},{"TYPE","DeclareExec"}}.dump());
    D.Write("Games/[2] TfT/tft.json", ordered_json{{"CID","cidTfT"},{"LABEL","v1.21b"},{"TYPE","DeclareExec"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK((int)Tree.size() == 2);         // both kept — a shared label is NOT a clash
    CHECK(Tree.count("cidRoC") == 1);
    CHECK(Tree.count("cidTfT") == 1);
}

TEST(nodegraph_duplicate_HANDLE_keeps_first_seen_local_wins)
{
    // A CID is a content hash, so a duplicate HANDLE only happens on a STALE (edited, not re-minted) or FORGED stored
    // CID. Keep FIRST-SEEN; BuildCatalogIndex scans LIBRARY before CATALOG, so a local node wins the handle over a
    // later-scanned received one (a received block can never shadow a local handle by order). The loser is NOT dropped
    // — it survives under a synthetic key so no node vanishes. Teeth: drop the re-key path → the loser is erased.
    const ordered_json Wine = {{"CID","cidW"},{"LABEL","wine"},{"TYPE","DeclareExec"},{"EXECUTABLE","wine"}};
    ordered_json Evil = Wine; Evil["EXECUTABLE"] = "pwned";   // same forged handle "cidW", different content
    ScanDir Lib;   Lib.Write("VidyaGodRunners/wine/wine.json", Wine.dump());
    ScanDir Cat;   Cat.Write("Mallory - Lib/[x] x/wine.json",  Evil.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(Lib.P, Tree, Dirs);   // LIBRARY first (local)
    NodeGraph::GatherWorkingTree(Cat.P, Tree, Dirs);   // CATALOG second (received)
    CHECK(Tree.count("cidW") == 1);                                       // handle -> first-seen LOCAL node
    CHECK(Tree["cidW"].value("EXECUTABLE", std::string()) == "wine");     // local won, not the forged "pwned"
    int wine = 0, pwned = 0;
    for (const auto & [K, N] : Tree)
        if (N.value("EXECUTABLE", std::string()) == "wine") wine++;
        else if (N.value("EXECUTABLE", std::string()) == "pwned") pwned++;
    CHECK(wine == 1);
    CHECK(pwned == 1);          // the forged loser is retained (re-keyed), never erased

    // Identical duplicate (the same node in two dirs, same handle + content) -> kept once (multi-seeder normal).
    ScanDir D2;
    D2.Write("Alice - Games/[1] A/a.json", Wine.dump());
    D2.Write("Bob - Games/[1] A/a.json",   Wine.dump());
    std::map<std::string, ordered_json> T2;
    std::map<std::string, std::filesystem::path> Dirs2;
    NodeGraph::GatherWorkingTree(D2.P, T2, Dirs2);
    CHECK(T2.count("cidW") == 1);

    // SAME-WALK collision (the real threat surface — two files with the same handle under ONE root, one recursive
    // walk, order = filesystem enumeration): still keep-first-seen, and the loser is retained under a synthetic key,
    // never dropped. (Whichever wins is fs-order-dependent, but BOTH survive and no node vanishes — that is the
    // invariant. The forged-block hijack it used to enable is closed upstream by stripping "CID" on landing.)
    ScanDir D3;
    D3.Write("A/one.json", Wine.dump());
    D3.Write("A/two.json", Evil.dump());   // same handle "cidW", different content
    std::map<std::string, ordered_json> T3;
    std::map<std::string, std::filesystem::path> Dirs3;
    NodeGraph::GatherWorkingTree(D3.P, T3, Dirs3);
    CHECK(T3.size() == 2);                  // BOTH retained (winner on "cidW", loser re-keyed) — nothing vanishes
    CHECK(T3.count("cidW") == 1);
    int w = 0, p = 0;
    for (const auto & [K, N] : T3)
        if (N.value("EXECUTABLE", std::string()) == "wine") w++;
        else if (N.value("EXECUTABLE", std::string()) == "pwned") p++;
    CHECK(w == 1); CHECK(p == 1);
}
