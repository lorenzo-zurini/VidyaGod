#include "vgtest.h"
#include "nodegraph.h"

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
        {"NODE_ID", "x"},
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
        {"NODE_ID", "exec"},
        {"TYPE", "DeclareExec"},
        {"POS", ordered_json::array({100.0, 200.0})},
        {"PARENTS", ordered_json::array({"content_node", "external_already_a_cid"})},
        {"LIBRARYITEM", "tile_node"},
        {"COVER", {{"PATH", "c.png"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "cidCover"}, {"SIZE", 9}}}}},
    };
    const ordered_json F = NodeGraph::FreezeNodeJson(Raw, HandleToCid);

    CHECK(!F.contains("POS"));                                   // non-semantic canvas coords dropped
    // intra-tree handle → CID, then linkified; external ref passes through, linkified
    CHECK(F["PARENTS"][0] == ordered_json({{"/", "cidContent"}}));
    CHECK(F["PARENTS"][1] == ordered_json({{"/", "external_already_a_cid"}}));
    CHECK(F["LIBRARYITEM"] == ordered_json({{"/", "cidTile"}}));
    CHECK(F["COVER"]["SOURCE"]["CID"] == ordered_json({{"/", "cidCover"}}));   // nested SOURCE.CID linkified
    CHECK(F["NODE_ID"] == "exec");                              // identity label preserved
}

TEST(nodegraph_freeze_then_normalize_roundtrips)
{
    std::map<std::string, std::string> HandleToCid = {{"dep", "cidDep"}};
    ordered_json Raw = {
        {"NODE_ID", "n"}, {"TYPE", "Content"},
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
        {"tile", {{"NODE_ID", "tile"}, {"TYPE", "DeclareLibraryItem"}, {"TITLE", "X"}, {"UID", "42"}}},
        {"exec", {{"NODE_ID", "exec"}, {"TYPE", "DeclareExec"},
                  {"PARENTS", ordered_json::array({"persist", "tile", "content"})}}},
        {"content", {{"NODE_ID", "content"}, {"TYPE", "Content"}}},
        {"persist", {{"NODE_ID", "persist"}, {"TYPE", "DeclarePersist"}}},
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
        {"tile", {{"NODE_ID", "tile"}, {"TYPE", "DeclareLibraryItem"}, {"TITLE", "X"}, {"UID", "7"}}},
        {"base", {{"NODE_ID", "base"}, {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"tile"})}}},
        {"exec", {{"NODE_ID", "exec"}, {"TYPE", "DeclareExec"}, {"PARENTS", ordered_json::array({"base"})}}},
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
        {"lib",     {{"NODE_ID", "lib"}, {"TYPE", "Content"}}},
        {"content", {{"NODE_ID", "content"}, {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"lib"})}}},
        {"tile",    {{"NODE_ID", "tile"}, {"TYPE", "DeclareLibraryItem"}}},
        {"exec",    {{"NODE_ID", "exec"}, {"TYPE", "DeclareExec"},
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
        {"a",    {{"NODE_ID", "a"},    {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"b"})}}},
        {"b",    {{"NODE_ID", "b"},    {"TYPE", "Content"}, {"PARENTS", ordered_json::array({"a"})}}},
        {"good", {{"NODE_ID", "good"}, {"TYPE", "Content"}}},
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
        {"only", {{"NODE_ID", "only"}, {"TYPE", "DeclareExec"},
                  {"PARENTS", ordered_json::array({"some_external_cid"})}, {"LIBRARYITEM", "another_external_cid"}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
    CHECK(Order.size() == 1);
    CHECK(Order[0] == "only");
}
