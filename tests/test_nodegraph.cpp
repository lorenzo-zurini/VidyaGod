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
    D.Write("Games/[1] A/a.json", ordered_json{{"LABEL", "a_exec"}, {"TYPE", "DeclareExec"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK(Tree.count("a_exec") == 1);      // the honest neighbour still scans
    CHECK(Tree.size() == 1);               // the bomb contributed nothing
    CHECK(!NodeGraph::JsonDepthWithinLimit(Deep, 64));
    CHECK(NodeGraph::JsonDepthWithinLimit("{\"a\":[1,2,{\"b\":3}]}", 64));
}

TEST(nodegraph_scan_survives_hostile_nonstring_label_and_keeps_nameless)
{
    // A received block controls LABEL's TYPE. A non-string LABEL must NOT throw the catalog scan (that aborts the
    // GUI/worker); it reads as nameless. And a legal NAMELESS node (TYPE, no LABEL) must still be gathered (keyed by
    // a synthetic key), never dropped. Teeth: revert GatherWorkingTree's is_string guard → this throws; drop the
    // TYPE-gate/synthetic-key → the nameless node vanishes.
    ScanDir D;
    D.Write("A/hostile.json", std::string("{\"TYPE\":\"Content\",\"LABEL\":1}"));      // non-string LABEL
    D.Write("A/nameless.json", std::string("{\"TYPE\":\"Content\",\"FORM\":\"zip\",\"PATH\":\"x.zip\"}"));  // no LABEL
    D.Write("A/named.json", ordered_json{{"LABEL","named"},{"TYPE","Content"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);   // must NOT throw
    CHECK(Tree.count("named") == 1);
    // The hostile + nameless nodes are gathered under synthetic keys (never a real handle), so both are present but
    // neither claims a forgeable label. Total gathered = 3 (named + 2 synthetic).
    CHECK((int)Tree.size() == 3);
    CHECK(Tree.count("1") == 0);        // the number LABEL did NOT become a "1" handle
}

TEST(nodegraph_duplicate_label_keeps_first_seen)
{
    // LABEL is COSMETIC — identity is the CID. Two DIFFERENT nodes MAY share a label (RoC/TFT "v1.21b", two
    // "Vanilla" editions). On a duplicate the scan KEEPS FIRST-SEEN (with a warning) and never erases — the label is
    // not a unique logic key. BuildCatalogIndex scans LIBRARY before CATALOG, so a local node wins the handle over a
    // later-scanned received one. IDENTICAL copies dedupe silently (the multi-seeder normal).
    const ordered_json Wine = {{"LABEL", "wine"}, {"TYPE", "DeclareExec"}, {"EXECUTABLE", "wine"}};
    ordered_json Evil = Wine; Evil["EXECUTABLE"] = "pwned";
    // Mirror BuildCatalogIndex: LIBRARY is gathered into the tree BEFORE CATALOG, into the SAME map. The local
    // (LIBRARY, first-call) node must win the label; the later CATALOG claimant is dropped, never erasing the local.
    ScanDir Lib;   Lib.Write("VidyaGodRunners/wine/wine.json", Wine.dump());
    ScanDir Cat;   Cat.Write("Mallory - Lib/[x] x/wine.json", Evil.dump());
    Lib.Write("Games/[1] A/a.json", ordered_json{{"LABEL", "a_exec"}, {"TYPE", "DeclareExec"}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(Lib.P, Tree, Dirs);   // LIBRARY first (local)
    NodeGraph::GatherWorkingTree(Cat.P, Tree, Dirs);   // CATALOG second (received)
    CHECK(Tree.count("wine") == 1);                    // the handle -> first-seen LOCAL node
    CHECK(Tree["wine"].value("EXECUTABLE", std::string()) == "wine");   // LOCAL content won, not the received "pwned"
    CHECK(Tree.count("a_exec") == 1);
    // The collision LOSER is NOT dropped — it survives under a synthetic key, so BOTH distinct nodes still index by
    // CID (a real library legitimately has RoC "v1.21b" AND TFT "v1.21b"; neither may vanish). Count both contents.
    int wineNodes = 0, pwned = 0;
    for (const auto & [K, N] : Tree)
        if (N.value("EXECUTABLE", std::string()) == "wine") wineNodes++;
        else if (N.value("EXECUTABLE", std::string()) == "pwned") pwned++;
    CHECK(wineNodes == 1);
    CHECK(pwned == 1);          // the loser is retained (re-keyed), NOT erased -- no game vanishes on a label clash

    // Identical duplicate (the same received node in two library dirs) -> kept once.
    ScanDir D2;
    D2.Write("Alice - Games/[1] A/a.json", Wine.dump());
    D2.Write("Bob - Games/[1] A/a.json",   Wine.dump());
    std::map<std::string, ordered_json> T2;
    std::map<std::string, std::filesystem::path> Dirs2;
    NodeGraph::GatherWorkingTree(D2.P, T2, Dirs2);
    CHECK(T2.count("wine") == 1);
}
