#include "vgtest.h"
#include "nodegraph.h"
#include "cid.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::ordered_json;

// Teeth for the node graph's PURE transforms (nodegraphpure.cpp): POS/CID strip + ref resolution in FreezeNodeJson
// (references stay plain CID strings), deps-first topo order with cycle breaking, the working-tree gather's defences,
// and the edit cascade over real identities (Cid::OfNode). Each assertion fails if its behaviour is removed.

static int IndexOf(const std::vector<std::string> &V, const std::string &S)
{
    for (size_t i = 0; i < V.size(); ++i) if (V[i] == S) return (int)i;
    return -1;
}

TEST(nodegraph_freeze_strips_pos_and_resolves_every_ref)
{
    std::map<std::string, std::string> HandleToCid = {
        {"content_node", "cidContent"},
        {"tile_node", "cidTile"},
    };
    ordered_json Raw = {
        {"CID", "premint-9"},                                   // the working-tree handle (last-minted CID / draft)
        {"LABEL", "exec"},
        {"POS", ordered_json::array({100.0, 200.0})},
        {"LAYERS", ordered_json::array({
            {{"NODE", "content_node"}}, {{"NODE", "external_already_a_cid"}},
            {{"ANY", ordered_json::array({"tile_node", "content_node"})}}, {{"NOT", "tile_node"}},
            {{"ZIP", "g.zip"}, {"SOURCE", "cidBytes"}},
            {{"EXEC", ordered_json::array({ {{"LABEL", "Play"}, {"TILE", {{"UID", "1"}, {"COVER", {{"FILE", "c.png"}, {"SOURCE", "cidCover"}}}}}} })}},
        })},
    };
    const ordered_json F = NodeGraph::FreezeNodeJson(Raw, HandleToCid);

    CHECK(!F.contains("POS"));                                  // non-semantic canvas coords dropped
    CHECK(!F.contains("CID"));                                  // the node's OWN handle is stripped (a file cannot hold its own hash)
    // every ref form follows a re-minted node to its CID — and stays a PLAIN string (there are no links)
    CHECK(F["LAYERS"][0]["NODE"] == "cidContent");
    CHECK(F["LAYERS"][1]["NODE"] == "external_already_a_cid");  // an external ref passes through
    CHECK(F["LAYERS"][2]["ANY"][0] == "cidTile");
    CHECK(F["LAYERS"][2]["ANY"][1] == "cidContent");
    CHECK(F["LAYERS"][3]["NOT"] == "cidTile");
    CHECK(F["LAYERS"][4]["SOURCE"] == "cidBytes");              // content CIDs untouched
    CHECK(F["LAYERS"][5]["EXEC"][0]["TILE"]["COVER"]["SOURCE"] == "cidCover");
    CHECK(F["LABEL"] == "exec");
}

// The CID write-back and the freeze share ONE remapper (ManifestModel::RemapNodeRefs): a NODE, an ANY member and a
// NOT all follow a re-minted node to its new CID; an external ref passes through.
TEST(nodegraph_remap_node_refs_reaches_every_ref_form)
{
    ordered_json N = {{"LABEL", "n"}, {"LAYERS", ordered_json::array({
        {{"NODE", "a"}}, {{"ANY", ordered_json::array({"b", "ext"})}}, {{"NOT", "c"}}, {{"ZIP", "x.zip"}} })}};
    const std::map<std::string, std::string> Map = {{"a", "A"}, {"b", "B"}, {"c", "C"}};
    CHECK(ManifestModel::RemapNodeRefs(N, [&](const std::string &R) { auto It = Map.find(R); return It == Map.end() ? R : It->second; }));
    CHECK(N["LAYERS"][0]["NODE"] == "A");
    CHECK(N["LAYERS"][1]["ANY"][0] == "B");
    CHECK(N["LAYERS"][1]["ANY"][1] == "ext");
    CHECK(N["LAYERS"][2]["NOT"] == "C");
    CHECK(N["LAYERS"][3]["ZIP"] == "x.zip");                      // a payload is not a ref
    // A node with no refs, or not a node at all, is left alone and reports no change.
    ordered_json Bare = {{"LABEL", "x"}, {"LAYERS", ordered_json::array()}};
    CHECK(!ManifestModel::RemapNodeRefs(Bare, [](const std::string &R) { return R + "!"; }));
    ordered_json Odd = {{"LABEL", "x"}, {"LAYERS", "notalist"}};
    CHECK(!ManifestModel::RemapNodeRefs(Odd, [](const std::string &R) { return R + "!"; }));
}

TEST(nodegraph_topo_order_is_deps_first)
{
    // exec contains content, which contains lib; exec's ANY names tile; its NOT names rival. Every ref form is a
    // freeze dependency: the frozen file embeds the CID of each, NOT included.
    std::map<std::string, ordered_json> Tree = {
        {"lib",     {{"LABEL", "lib"}, {"LAYERS", ordered_json::array()}}},
        {"content", {{"LABEL", "content"}, {"LAYERS", ordered_json::array({ {{"NODE", "lib"}} })}}},
        {"tile",    {{"LABEL", "tile"}, {"LAYERS", ordered_json::array()}}},
        {"rival",   {{"LABEL", "rival"}, {"LAYERS", ordered_json::array()}}},
        {"exec",    {{"LABEL", "exec"}, {"LAYERS", ordered_json::array({
                        {{"NODE", "content"}}, {{"ANY", ordered_json::array({"tile", "lib"})}}, {{"NOT", "rival"}} })}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
    CHECK(Order.size() == 5);
    CHECK(IndexOf(Order, "lib")     < IndexOf(Order, "content"));
    CHECK(IndexOf(Order, "content") < IndexOf(Order, "exec"));
    CHECK(IndexOf(Order, "tile")    < IndexOf(Order, "exec"));   // an ANY member is a freeze dependency too
    CHECK(IndexOf(Order, "rival")   < IndexOf(Order, "exec"));   // ...and so is a NOT (it names a CID)
}

TEST(nodegraph_edit_cascades_new_cids_up_the_chain)
{
    // References are CIDs; a mint freezes leaf→root, resolving each ref to the freshly computed CID of its target. So
    // editing a LEAF must change its CID AND every container's CID, each container re-pointing to the new child CID.
    // Identity is the real one (Cid::OfNode: the canonical bytes' raw CID). Teeth: if FreezeNodeJson stopped
    // resolving handles (or topo stopped being deps-first), a container would keep the OLD child CID.
    auto mint = [&](std::map<std::string, ordered_json> Tree) {
        std::vector<std::string> Order; std::string Err;
        CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));
        std::map<std::string, std::string> H2C;
        for (const std::string &H : Order)
            H2C[H] = Cid::OfNode(NodeGraph::FreezeNodeJson(Tree.at(H), H2C));   // deps already in H2C ⇒ refs resolve
        return H2C;
    };
    auto tree = [](const std::string &basePayload) {
        return std::map<std::string, ordered_json>{
            {"A", {{"CID", "A"}, {"LABEL", "base"}, {"LAYERS", ordered_json::array({ {{"ZIP", basePayload}} })}}},
            {"B", {{"CID", "B"}, {"LABEL", "content"}, {"LAYERS", ordered_json::array({ {{"NODE", "A"}} })}}},
            {"C", {{"CID", "C"}, {"LABEL", "exec"}, {"LAYERS", ordered_json::array({ {{"NODE", "B"}} })}}},
        };
    };
    const auto V1 = mint(tree("v1.zip"));
    const auto V2 = mint(tree("v2.zip"));        // ONLY the leaf A's payload changed
    CHECK(V1.at("A") != V2.at("A"));             // the edited leaf's CID moved
    CHECK(V1.at("B") != V2.at("B"));             // its container re-minted (its ref resolved to A's NEW cid)
    CHECK(V1.at("C") != V2.at("C"));             // cascade reached the root
    const ordered_json FB = NodeGraph::FreezeNodeJson(tree("v2.zip").at("B"), { {"A", V2.at("A")} });
    CHECK(FB["LAYERS"][0]["NODE"] == V2.at("A"));
    // The same bytes give the same CID on every run (identity is content).
    CHECK(mint(tree("v1.zip")) == V1);
}

TEST(nodegraph_topo_order_breaks_cycle)
{
    // A cycle must NOT abort the order (that would empty the whole library on one self-referential typo): the back
    // edge is broken, both nodes still enter the order, and they get dropped later at freeze (unfreezable). A THIRD
    // acyclic node must survive regardless — the degrade-per-node guarantee.
    std::map<std::string, ordered_json> Tree = {
        {"a",    {{"LABEL", "a"}, {"LAYERS", ordered_json::array({ {{"NODE", "b"}} })}}},
        {"b",    {{"LABEL", "b"}, {"LAYERS", ordered_json::array({ {{"NODE", "a"}} })}}},
        {"good", {{"LABEL", "good"}, {"LAYERS", ordered_json::array()}}},
    };
    std::vector<std::string> Order;
    std::string Err;
    CHECK(NodeGraph::TopoOrderForMint(Tree, Order, &Err));   // cycle is broken, not fatal
    CHECK(Order.size() == 3);                                 // all ordered; cyclic ones skip at freeze
    CHECK(IndexOf(Order, "good") >= 0);                       // the acyclic node always survives
}

TEST(nodegraph_topo_external_refs_impose_no_order)
{
    // A ref not in the tree is an external (already-frozen) dep — it must not block ordering or error.
    std::map<std::string, ordered_json> Tree = {
        {"only", {{"LABEL", "only"},
                  {"LAYERS", ordered_json::array({ {{"NODE", "some_external_cid"}}, {{"NOT", "another_external_cid"}} })}}},
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
    // SKIPPED by the depth pre-scan, not parsed — nlohmann's parser recurses per level, so
    // without the guard this test dies by stack overflow (no assert ever fires — the crash IS the failure).
    ScanDir D;
    std::string Deep;
    for (int i = 0; i < 200000; ++i) Deep += '[';
    for (int i = 0; i < 200000; ++i) Deep += ']';
    D.Write("Evil - Lib/[x] x/bomb.json", Deep);
    // Model C: a node is KEYED by its stored "CID" handle (LABEL is cosmetic).
    D.Write("Games/[1] A/a.json", ordered_json{{"CID", "cidA"}, {"LABEL", "a_exec"}, {"LAYERS", ordered_json::array()}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK(Tree.count("cidA") == 1);        // keyed by the CID handle; the honest neighbour still scans
    CHECK(Tree.size() == 1);               // the bomb contributed nothing
    CHECK(!NodeGraph::JsonDepthWithinLimit(Deep, 64));
    CHECK(NodeGraph::JsonDepthWithinLimit("{\"a\":[1,2,{\"b\":3}]}", 64));
}

TEST(nodegraph_scan_skips_a_folder_fetch_still_landing)
{
    // A folder fetch materializes into "<dest>.tmp" and renames it into place: its half-landed node files are not a
    // package (indexed, the closure pass wrote INTO the scratch dir and collided with the fetch). The landed folder is
    // one, its nodes' bundle the package dir.
    ScanDir D;
    D.Write("Lib/[1] G/.package.tmp/half.json", ordered_json{{"CID", "cidHalf"}, {"LABEL", "h"}, {"LAYERS", ordered_json::array()}}.dump());
    D.Write("Lib/[1] G/.package/done.json", ordered_json{{"CID", "cidDone"}, {"LABEL", "d"}, {"LAYERS", ordered_json::array()}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK(Tree.count("cidHalf") == 0);
    CHECK(Tree.count("cidDone") == 1);
    CHECK_EQ(Dirs["cidDone"].filename().string(), std::string("[1] G"));
}

TEST(nodegraph_scan_survives_hostile_handle_and_keeps_handleless)
{
    // A received/working-tree file controls the "CID" handle's TYPE. A non-string handle must NOT throw the scan (that
    // aborts the GUI/worker); it reads as handle-less. A control-byte handle is rejected (unforgeable synthetic key).
    // And a legal HANDLE-LESS node (TYPE, no "CID" — a never-minted draft, or a frozen block) must still be gathered
    // under a synthetic key, never dropped. LABEL is cosmetic and is NEVER a key. Teeth: revert GatherWorkingTree's
    // is_string guard → this throws; drop the synthetic-key path → the handle-less node vanishes.
    ScanDir D;
    D.Write("A/hostile.json",  std::string("{\"CID\":1,\"LABEL\":\"x\",\"LAYERS\":[]}"));                        // non-string handle
    D.Write("A/nameless.json", std::string("{\"LAYERS\":[{\"ZIP\":\"x.zip\"}]}"));                                // no handle
    D.Write("A/named.json",    ordered_json{{"CID","cidN"},{"LABEL","named"},{"LAYERS",ordered_json::array()}}.dump());
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
    D.Write("Games/[1] RoC/roc.json", ordered_json{{"CID","cidRoC"},{"LABEL","v1.21b"},{"LAYERS",ordered_json::array()}}.dump());
    D.Write("Games/[2] TfT/tft.json", ordered_json{{"CID","cidTfT"},{"LABEL","v1.21b"},{"LAYERS",ordered_json::array()}}.dump());
    std::map<std::string, ordered_json> Tree;
    std::map<std::string, std::filesystem::path> Dirs;
    NodeGraph::GatherWorkingTree(D.P, Tree, Dirs);
    CHECK((int)Tree.size() == 2);         // both kept — a shared label is NOT a clash
    CHECK(Tree.count("cidRoC") == 1);
    CHECK(Tree.count("cidTfT") == 1);
}

TEST(nodegraph_untrusted_gather_ignores_forged_cid_handle)
{
    // A received CATALOG stub's bytes are attacker-controlled. Gathered with TrustStoredCid=FALSE, a crafted "CID"
    // (equal to a local node's external-dep CID, or forged to look authored) MUST NOT become a resolvable handle — the
    // node is synthetic-keyed instead. Teeth: pass TrustStoredCid=true here and the forged handle keys the node,
    // re-opening the dependency-substitution / BundleDir-steal hijacks.
    ScanDir D;
    D.Write("Mallory - Lib/[x] x/forge.json",
            ordered_json{{"CID", "bafyVICTIMdep"}, {"LABEL", "innocent"}, {"LAYERS", ordered_json::array()}}.dump());
    std::map<std::string, ordered_json> Untrusted;
    std::map<std::string, std::filesystem::path> DirsU;
    NodeGraph::GatherWorkingTree(D.P, Untrusted, DirsU, /*SkipReserved=*/false, /*TrustStoredCid=*/false);
    CHECK(Untrusted.count("bafyVICTIMdep") == 0);   // the forged handle did NOT take the slot
    CHECK(Untrusted.size() == 1);                   // the node is still gathered (browse) — under a synthetic key
    // The same file gathered as TRUSTED (our own root) DOES honor the stored handle.
    std::map<std::string, ordered_json> Trusted;
    std::map<std::string, std::filesystem::path> DirsT;
    NodeGraph::GatherWorkingTree(D.P, Trusted, DirsT, /*SkipReserved=*/false, /*TrustStoredCid=*/true);
    CHECK(Trusted.count("bafyVICTIMdep") == 1);
}

TEST(nodegraph_duplicate_HANDLE_keeps_first_seen_local_wins)
{
    // A CID is a content hash, so a duplicate HANDLE only happens on a STALE (edited, not re-minted) or FORGED stored
    // CID. Keep FIRST-SEEN; BuildCatalogIndex scans LIBRARY before CATALOG, so a local node wins the handle over a
    // later-scanned received one (a received block can never shadow a local handle by order). The loser is NOT dropped
    // — it survives under a synthetic key so no node vanishes. Teeth: drop the re-key path → the loser is erased.
    const ordered_json Wine = {{"CID","cidW"},{"LABEL","wine"},{"EXECUTABLE","wine"},{"LAYERS",ordered_json::array()}};
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

// Landing a peer's node bytes (a received package folder, a fetched closure node, a hydrated CID): only an honest
// node gets in. Each refusal below is a way a hostile or broken peer's file would otherwise reach the working tree —
// a handle ("CID") that hijacks a local node once the package is installed, bytes that are not the CID they are
// named by, a non-canonical encoding that re-freezes to another identity, or JSON that is not a node at all.
TEST(nodegraph_landing_verifies_node_bytes)
{
    const ordered_json Node = { {"LABEL", "n"}, {"LAYERS", ordered_json::array({ ordered_json{{"ZIP", "a.zip"}} })} };
    const std::string Bytes = Cid::Canonical(Node);
    const std::string C = Cid::OfBytes(Bytes);
    ordered_json J;
    std::string Err;
    CHECK(NodeGraph::VerifyNodeBytes(Bytes, C, J, &Err));
    CHECK(J == Node);
    CHECK(!NodeGraph::VerifyNodeBytes(Bytes, Cid::OfBytes("other"), J, &Err));                       // not its name
    ordered_json WithHandle = Node; WithHandle["CID"] = "bafkrei-local-handle";
    const std::string H = nlohmann::json(WithHandle).dump();                                           // canonical, but a handle
    CHECK(!NodeGraph::VerifyNodeBytes(H, Cid::OfBytes(H), J, &Err));
    ordered_json WithPos = Node; WithPos["POS"] = ordered_json::array({ 1, 2 });
    const std::string P = nlohmann::json(WithPos).dump();
    CHECK(!NodeGraph::VerifyNodeBytes(P, Cid::OfBytes(P), J, &Err));
    const std::string Pretty = Node.dump(2);                                                           // not canonical
    CHECK(!NodeGraph::VerifyNodeBytes(Pretty, Cid::OfBytes(Pretty), J, &Err));
    const std::string NotNode = R"({"LABEL":"x"})";                                                   // no LAYERS
    CHECK(!NodeGraph::VerifyNodeBytes(NotNode, Cid::OfBytes(NotNode), J, &Err));
    const std::string Deep = std::string(200, '[') + std::string(200, ']');
    CHECK(!NodeGraph::VerifyNodeBytes(Deep, Cid::OfBytes(Deep), J, &Err));
    // …and over a landed file.
    const std::filesystem::path F = std::filesystem::temp_directory_path() / ("vg_landing_" + std::to_string(getpid()) + ".json");
    { std::ofstream O(F, std::ios::binary); O << Bytes; }
    CHECK(NodeGraph::VerifyLanded(F, C, &J, &Err));
    CHECK(!NodeGraph::VerifyLanded(F, Cid::OfBytes("x"), nullptr, &Err));
    std::filesystem::remove(F);
    CHECK(!NodeGraph::VerifyLanded(F, C, nullptr, &Err));                                             // absent
}

TEST(nodegraph_path_within_refuses_escapes)
{
    const std::filesystem::path B = "/tmp/vg_pkg_base";
    CHECK(NodeGraph::PathWithin(B, B / "x.zip"));
    CHECK(NodeGraph::PathWithin(B, B / "sub" / "x.zip"));
    CHECK(NodeGraph::PathWithin(B, B / ".wine" / "x"));                 // a leading-dot NAME is fine
    CHECK(!NodeGraph::PathWithin(B, B / ".." / "x.zip"));
    CHECK(!NodeGraph::PathWithin(B, "/home/u/.bashrc"));
}
