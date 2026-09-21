// PkgLayout — the automatic canvas layout.
//
// A layout is easy to eyeball and hard to assert, so these tests pin the properties that actually make a
// drawing usable, and the ones that make it SAFE to stamp into a package: determinism (same graph ⇒ same
// bytes, or publishing churns the CID on every run), no two nodes on the same point, parents drawn before
// their children, and a bounded aspect ratio on the shapes the real library contains — a 904-layer chain
// (Minecraft) and a 1000-wide fan-out, both of which a naive one-column-per-layer pass draws as an unusable
// ribbon hundreds of thousands of pixels long.

#include "vgtest.h"
#include "pkglayout.h"
#include "pkggraph.h"
#include <vector>

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using ordered_json = nlohmann::ordered_json;


namespace {

//A chain of N nodes, each parented to the one before: the Minecraft delta-chain shape in miniature.
ordered_json Chain(int N)
{
    ordered_json A = ordered_json::array();
    for (int I = 0; I < N; ++I)
    {
        ordered_json Nd;
        Nd["CID"] = "n" + std::to_string(I);
        Nd["TYPE"]    = "Group";
        if (I > 0) Nd["PARENTS"] = ordered_json::array({"n" + std::to_string(I - 1)});
        A.push_back(Nd);
    }
    return A;
}

//One root with N children: the opposite extreme, a single enormous layer.
ordered_json Fan(int N)
{
    ordered_json A = ordered_json::array();
    ordered_json Root; Root["CID"] = "root"; Root["TYPE"] = "Group";
    A.push_back(Root);
    for (int I = 0; I < N; ++I)
    {
        ordered_json Nd;
        Nd["CID"] = "c" + std::to_string(I);
        Nd["TYPE"]    = "Group";
        Nd["PARENTS"] = ordered_json::array({"root"});
        A.push_back(Nd);
    }
    return A;
}

struct Extent { float W = 0, H = 0; };

Extent ExtentOf(const PkgGraph::Graph &G)
{
    float MinX = 0, MaxX = 0, MinY = 0, MaxY = 0;
    bool First = true;
    for (const auto &N : G.Nodes)
    {
        if (First) { MinX = MaxX = N.X; MinY = MaxY = N.Y; First = false; continue; }
        MinX = std::min(MinX, N.X); MaxX = std::max(MaxX, N.X);
        MinY = std::min(MinY, N.Y); MaxY = std::max(MaxY, N.Y);
    }
    return {MaxX - MinX, MaxY - MinY};
}

size_t DistinctPoints(const PkgGraph::Graph &G)
{
    std::set<std::pair<float, float>> S;
    for (const auto &N : G.Nodes) S.insert({N.X, N.Y});
    return S.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Determinism — the property that lets a computed layout be written into the package.
// ---------------------------------------------------------------------------

TEST(layout_is_deterministic_across_runs)
{
    const ordered_json Doc = Chain(200);
    PkgGraph::Graph A = PkgGraph::Build(Doc);
    PkgGraph::Graph B = PkgGraph::Build(Doc);
    CHECK_EQ(A.Nodes.size(), B.Nodes.size());
    for (size_t I = 0; I < A.Nodes.size(); ++I)
    {
        CHECK_EQ(A.Nodes[I].X, B.Nodes[I].X);
        CHECK_EQ(A.Nodes[I].Y, B.Nodes[I].Y);
    }
}

// ---------------------------------------------------------------------------
// The two extremes the real library actually contains.
// ---------------------------------------------------------------------------

TEST(a_904_layer_chain_does_not_draw_as_a_ribbon)
{
    //Minecraft: 904 layers, ~3 nodes wide. One column per layer is 388,720 px across — the wrap is the point.
    PkgGraph::Graph G = PkgGraph::Build(Chain(904));
    const Extent E = ExtentOf(G);
    CHECK(E.W > 0);
    CHECK(E.H > 0);
    //Not a ribbon: the drawing has to stay within a sane aspect either way up.
    const float Aspect = E.W / E.H;
    CHECK(Aspect < 3.0f);
    CHECK(Aspect > 1.0f / 3.0f);
    //And it must be dramatically narrower than the unwrapped 904 * 430.
    CHECK(E.W < 904.0f * 430.0f / 4.0f);
}

TEST(a_1000_wide_layer_wraps_into_a_block)
{
    PkgGraph::Graph G = PkgGraph::Build(Fan(1000));
    const Extent E = ExtentOf(G);
    //1000 stacked rows would be 299,700 px tall.
    CHECK(E.H < 1000.0f * 300.0f / 4.0f);
    //Tight on purpose. Capping the layer at MaxRows alone already satisfies a loose bound, so a loose bound
    //cannot tell whether the oversized-layer reshape ran: without it this fan draws 23,650 x 5,620 (aspect
    //4.2), with it 15,050 x 8,620 (aspect 1.75, against a 1.78 target). 3.0 sits between the two.
    const float Aspect = E.W / E.H;
    CHECK(Aspect < 3.0f);
    CHECK(Aspect > 1.0f / 3.0f);
}

TEST(no_two_nodes_share_a_point)
{
    for (const ordered_json &Doc : {Chain(904), Fan(1000), Chain(3), Fan(17)})
    {
        PkgGraph::Graph G = PkgGraph::Build(Doc);
        CHECK_EQ(DistinctPoints(G), G.Nodes.size());
    }
}

// ---------------------------------------------------------------------------
// Readability invariants.
// ---------------------------------------------------------------------------

TEST(a_parent_is_never_drawn_right_of_its_child_within_a_band)
{
    PkgGraph::Graph G = PkgGraph::Build(Chain(50));
    for (const auto &L : G.Links)
    {
        if (L.ParentIndex < 0) continue;
        const auto &P = G.Nodes[L.ParentIndex];
        const auto &C = G.Nodes[L.ChildIndex];
        //Either the child is further right in the same band, or it starts a later band (drawn lower).
        CHECK(C.X > P.X || C.Y > P.Y);
    }
}

TEST(ordering_reduces_crossings_versus_document_order)
{
    //A deliberately adversarial bipartite graph: children listed in the reverse order of their parents, which
    //document-order placement draws as a full crossing bundle.
    ordered_json A = ordered_json::array();
    const int N = 12;
    for (int I = 0; I < N; ++I)
    { ordered_json P; P["CID"] = "p" + std::to_string(I); P["TYPE"] = "Group"; A.push_back(P); }
    for (int I = 0; I < N; ++I)
    {
        ordered_json C;
        C["CID"] = "c" + std::to_string(I);
        C["TYPE"]    = "Group";
        C["PARENTS"] = ordered_json::array({"p" + std::to_string(N - 1 - I)});
        A.push_back(C);
    }
    PkgGraph::Graph Ordered = PkgGraph::Build(A);

    PkgLayout::Options NoSweeps;
    NoSweeps.Sweeps = 0;
    PkgGraph::Graph Raw = PkgGraph::Build(A);
    PkgLayout::Compute(Raw, NoSweeps);

    //Asserting only "ordered <= raw" is blind: disabling the ordering pass changes BOTH sides identically and
    //the comparison still holds. So demand the ABSOLUTE result — a plain reversal is exactly what the median
    //heuristic exists to solve, and it must solve it completely — and that the unsorted seed really is a
    //crossed drawing, or the zero above would prove nothing either.
    CHECK(PkgLayout::CountCrossings(Raw) > 0);
    CHECK_EQ(PkgLayout::CountCrossings(Ordered), 0LL);
}

// ---------------------------------------------------------------------------
// POS on the node, and the local override on top of it.
// ---------------------------------------------------------------------------

TEST(a_node_POS_is_used_as_the_default_position)
{
    ordered_json A = ordered_json::array();
    ordered_json N; N["CID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({1234.0, 567.0});
    A.push_back(N);
    PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes[0].X, 1234.0f);
    CHECK_EQ(G.Nodes[0].Y, 567.0f);
    CHECK(G.Nodes[0].HasPos);
}

TEST(a_local_override_beats_the_node_POS)
{
    ordered_json A = ordered_json::array();
    ordered_json N; N["CID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({1234.0, 567.0});
    A.push_back(N);
    ordered_json Override = ordered_json::object();
    Override["a"] = ordered_json::array({10.0, 20.0});
    PkgGraph::Graph G = PkgGraph::Build(A, &Override);
    CHECK_EQ(G.Nodes[0].X, 10.0f);
    CHECK_EQ(G.Nodes[0].Y, 20.0f);
}

TEST(a_malformed_POS_falls_through_to_the_computed_layout)
{
    //A hand-edited node file can carry anything; a bad POS must not leave the node stacked at the origin with
    //HasPos set, which would also suppress the auto-layout for it.
    for (const ordered_json &Bad : {ordered_json("nope"),
                                    ordered_json::array({1.0}),
                                    ordered_json::array({"x", "y"}),
                                    ordered_json::object()})
    {
        ordered_json A = ordered_json::array();
        ordered_json N; N["CID"] = "a"; N["TYPE"] = "Group"; N["POS"] = Bad;
        ordered_json M; M["CID"] = "b"; M["TYPE"] = "Group"; M["PARENTS"] = ordered_json::array({"a"});
        A.push_back(N); A.push_back(M);
        PkgGraph::Graph G = PkgGraph::Build(A);
        CHECK(!G.Nodes[0].HasPos);                // a malformed POS is ABSENT, not a declared position
        CHECK(G.Nodes[1].X > G.Nodes[0].X);       // and the graph still reads parent → child
    }
}

TEST(placed_nodes_keep_their_position_when_a_new_one_is_added)
{
    //ComputeUnplaced must not move what the author already positioned — and must not drop the new node on top
    //of one of them either.
    ordered_json A = ordered_json::array();
    ordered_json N; N["CID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({500.0, 500.0});
    ordered_json M; M["CID"] = "b"; M["TYPE"] = "Group"; M["PARENTS"] = ordered_json::array({"a"});
    A.push_back(N); A.push_back(M);
    PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes[0].X, 500.0f);
    CHECK_EQ(G.Nodes[0].Y, 500.0f);
    CHECK(G.Nodes[1].X != 500.0f || G.Nodes[1].Y != 500.0f);
}

// ---------------------------------------------------------------------------
// Malformed graphs still draw.
// ---------------------------------------------------------------------------

TEST(a_parents_cycle_still_produces_a_layout)
{
    ordered_json A = ordered_json::array();
    ordered_json X; X["CID"] = "x"; X["TYPE"] = "Group"; X["PARENTS"] = ordered_json::array({"y"});
    ordered_json Y; Y["CID"] = "y"; Y["TYPE"] = "Group"; Y["PARENTS"] = ordered_json::array({"x"});
    A.push_back(X); A.push_back(Y);
    PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)2);
    CHECK_EQ(DistinctPoints(G), (size_t)2);
}

TEST(an_empty_graph_is_not_a_crash)
{
    PkgGraph::Graph G = PkgGraph::Build(ordered_json::array());
    CHECK_EQ(G.Nodes.size(), (size_t)0);
    PkgLayout::Compute(G);
}

//A PINNED layout. Building the same document twice in one process rules out almost nothing that threatens CID
//stability: the danger is the algorithm DRIFTING between the machine that stamps POS and the machine that reads
//it, or between today's build and next month's. Concrete coordinates for a fixed graph are the only assertion
//that catches that — if this test fails, every published POS in the library is now inconsistent with the code,
//and the golden must be updated deliberately rather than by reflex.
//THE thing a constant row step got wrong. A node's box is as tall as its payload makes it — a RegEdit with
//dozens of registry rows, a BinaryPatch with a dozen patch entries — and the layout stepped by 300 regardless,
//so tall nodes were drawn straight through the ones beneath them. Every other property here held while the
//picture was unreadable, which is why this one has to be asserted directly.
//
//Overlap is checked as RECTANGLES, per column: two nodes at the same X overlap when their [Y, Y+Height) spans
//intersect. The X threshold is the WIDEST drawn body — 355px, which is RegEdit; most types are 346. Not
//kNodeWidth (330): that is the interior width, so two nodes 330-354 units apart overlap by up to 25px and
//slipped through earlier versions of this check, which used 330 and then 346. The GUI suite's
//theEstimatedNodeHeightMatchesTheDrawnOne fails if any node reaches the 430px column step, which is the
//property that keeps different COLUMNS from colliding; this one is about nodes inside a column.
TEST(tall_nodes_do_not_overlap_the_ones_below_them)
{
    ordered_json A = ordered_json::array();
    //One parent, and a fan of children of WILDLY different heights hanging off it: a bare Group, a Content,
    //and RegEdits carrying 5, 40 and 120 registry rows. The 120-row node is roughly ten nominal steps tall.
    ordered_json Root; Root["CID"] = "root"; Root["TYPE"] = "Group"; A.push_back(Root);
    auto Child = [&](const char *Id, const ordered_json &Extra) {
        ordered_json J = Extra;
        J["CID"] = Id;
        J["PARENTS"] = ordered_json::array({"root"});
        A.push_back(J);
    };
    auto RegNode = [&](int Rows) {
        ordered_json Keys = ordered_json::object();
        for (int R = 0; R < Rows; ++R) Keys["Software"]["App"]["v" + std::to_string(R)] = "data";
        ordered_json Entry = ordered_json::object();
        Entry["ARCHITECTURE"] = ordered_json::array({"64"});
        Entry["HKLM"] = Keys;
        ordered_json J; J["TYPE"] = "RegEdit"; J["EDITS"] = ordered_json::array({Entry});
        return J;
    };
    ordered_json Grp; Grp["TYPE"] = "Group";
    ordered_json Cnt; Cnt["TYPE"] = "Content"; Cnt["FORM"] = "zip"; Cnt["PATH"] = "game.zip";
    Child("c1_group", Grp);
    Child("c2_content", Cnt);
    Child("c3_reg5",   RegNode(5));
    Child("c4_reg40",  RegNode(40));
    Child("c5_reg120", RegNode(120));

    const PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)6);
    //The estimate has to actually vary with the payload, or the overlap check below passes on a flat graph.
    CHECK(G.Nodes[5].Height > G.Nodes[1].Height * 5.0f);
    //And the horizontal assumption the per-column check rests on: a node has to be narrower than the step, or
    //"different X cannot collide" stops being true and this test silently stops covering half the plane.
    CHECK(PkgLayout::Options{}.ColumnStep > 400.0f);

    int Overlaps = 0;
    for (size_t I = 0; I < G.Nodes.size(); ++I)
        for (size_t J = I + 1; J < G.Nodes.size(); ++J)
        {
            if (std::abs(G.Nodes[I].X - G.Nodes[J].X) >= 355.0f) continue;   // overlapping in X, not identical
            const float Top1 = G.Nodes[I].Y, Bot1 = Top1 + G.Nodes[I].Height;
            const float Top2 = G.Nodes[J].Y, Bot2 = Top2 + G.Nodes[J].Height;
            if (Top1 < Bot2 && Top2 < Bot1) ++Overlaps;
        }
    CHECK_EQ(Overlaps, 0);
}

//And the same property on the shapes the library actually contains, where the packing wraps on both axes and
//a mistake in the band arithmetic is what would put two nodes on top of each other rather than a mistake in
//one layer's stacking.
TEST(no_node_overlaps_another_on_a_realistic_mixed_graph)
{
    ordered_json A = ordered_json::array();
    //A 60-layer chain with a fan of varied-height children hanging off every fourth link: deep enough to wrap
    //into bands, wide enough in places to wrap into sub-columns.
    for (int I = 0; I < 60; ++I)
    {
        ordered_json Nd;
        Nd["CID"] = "n" + std::to_string(I);
        Nd["TYPE"]    = "Content";
        Nd["FORM"]    = "zip";
        Nd["PATH"]    = "layer.zip";
        if (I) Nd["PARENTS"] = ordered_json::array({"n" + std::to_string(I - 1)});
        A.push_back(Nd);
        if (I % 4) continue;
        for (int K = 0; K < 7; ++K)
        {
            ordered_json Keys = ordered_json::object();
            for (int R = 0; R < (I + K) % 30; ++R) Keys["Software"]["v" + std::to_string(R)] = "d";
            ordered_json E = ordered_json::object(); E["HKLM"] = Keys;
            ordered_json C;
            C["CID"] = "f" + std::to_string(I) + "_" + std::to_string(K);
            C["TYPE"]    = "RegEdit";
            C["EDITS"]   = ordered_json::array({E});
            C["PARENTS"] = ordered_json::array({"n" + std::to_string(I)});
            A.push_back(C);
        }
    }

    const PkgGraph::Graph G = PkgGraph::Build(A);
    int Overlaps = 0;
    std::string First;
    for (size_t I = 0; I < G.Nodes.size(); ++I)
        for (size_t J = I + 1; J < G.Nodes.size(); ++J)
        {
            if (std::abs(G.Nodes[I].X - G.Nodes[J].X) >= 355.0f) continue;   // overlapping in X, not identical
            const float Top1 = G.Nodes[I].Y, Bot1 = Top1 + G.Nodes[I].Height;
            const float Top2 = G.Nodes[J].Y, Bot2 = Top2 + G.Nodes[J].Height;
            if (Top1 < Bot2 && Top2 < Bot1)
            {
                if (First.empty())
                    First = G.Nodes[I].Id + " [" + std::to_string(Top1) + "," + std::to_string(Bot1) + "] and "
                          + G.Nodes[J].Id + " [" + std::to_string(Top2) + "," + std::to_string(Bot2) + "]";
                ++Overlaps;
            }
        }
    if (Overlaps) std::printf("    first overlap: %s\n", First.c_str());
    CHECK_EQ(Overlaps, 0);
}

//A coordinate no layout could have produced is corruption, not a position. Accepting it made it permanent: the
//canvas cannot draw, select or drag a node at 5e9, and the position read-back then refused the value it was
//handed, re-seeded from the same number and refused it again — once per frame, forever.
//The height estimate counts registry rows with its own walk instead of building the row structs, because
//building them was 40% of a graph rebuild. Two readings of the same tree that drift apart is exactly how the
//estimate would start under-reserving again, so they are pinned against each other here — on the shapes that
//differ: empty keys, keys holding only subkeys, mixed values and subkeys, non-string values, deep nesting.
TEST(the_registry_row_count_matches_the_flattening)
{
    auto Check = [](const char *What, const ordered_json &Entry) {
        const size_t Walked = PkgGraph::RegRowsOf(Entry).size();
        const size_t Counted = PkgGraph::CountRegRows(Entry);
        if (Walked != Counted)
            std::printf("    %s: flattening says %zu rows, the count says %zu\n", What, Walked, Counted);
        CHECK_EQ(Counted, Walked);
    };
    Check("empty entry", ordered_json::object());
    Check("scalars only", ordered_json{{"ARCHITECTURE", ordered_json::array({"64"})}, {"OVERRIDE", true}});
    Check("one value", ordered_json{{"HKLM", {{"Software", {{"v", "d"}}}}}});
    Check("key with no children", ordered_json{{"HKLM", {{"Software", ordered_json::object()}}}});
    Check("values and subkeys", ordered_json{{"HKLM", {{"Software", {{"v", "d"}, {"Sub", {{"w", "e"}}}}}}}});
    Check("non-string values", ordered_json{{"HKLM", {{"S", {{"n", 42}, {"b", true}}}}}});
    Check("two hives", ordered_json{{"HKLM", {{"A", {{"v", "d"}}}}}, {"HKCU", {{"B", {{"w", "e"}}}}}});
    // Deep and wide, the shape the codec libraries actually ship.
    {
        ordered_json Deep = ordered_json::object();
        ordered_json *Cur = &Deep;
        for (int I = 0; I < 8; ++I) { (*Cur)["k" + std::to_string(I)] = ordered_json::object(); Cur = &(*Cur)["k" + std::to_string(I)]; }
        for (int I = 0; I < 20; ++I) (*Cur)["v" + std::to_string(I)] = "data";
        Check("deep chain", ordered_json{{"HKLM", Deep}});
    }
}

//The coordinates this file produces are STAMPED INTO THE PACKAGE at publish time, so they are part of its
//bytes and therefore of its CID. That makes them a golden, and a golden has to be a literal: the chain-shaped
//test below puts every node in its own column at Y0, where the vertical pitch — the thing this whole layout
//change is about — never enters an assertion at all. This one has a LAYER, so the pitch is visible, and it is
//pinned exactly. Changing these numbers is a deliberate act that re-stamps every package and changes its CID;
//if a change here was not intended, the change that caused it was not either.
//The wide two-layer fan-out. This is the ONE shape where a second band-budget pass (re-deriving the budget
//from post-re-split block heights) measurably changed the drawing — every real bundle in the library came out
//byte-identical — so deleting that pass needs the shape it touched pinned, or the deletion is unobserved and
//so was the thing it deleted. Heights vary, because uniform ones never reach the re-split at all.
TEST(a_wide_fanout_of_varied_heights_stays_a_rectangle)
{
    ordered_json A = ordered_json::array();
    ordered_json Root; Root["CID"] = "root"; Root["TYPE"] = "Group"; A.push_back(Root);
    for (int I = 0; I < 500; ++I)
    {
        ordered_json Keys = ordered_json::object();
        for (int R = 0; R < (I % 17); ++R) Keys["Software"]["v" + std::to_string(R)] = "d";
        ordered_json E = ordered_json::object(); E["HKLM"] = Keys;
        ordered_json N;
        N["CID"] = "f" + std::to_string(I);
        N["TYPE"]    = "RegEdit";
        N["EDITS"]   = ordered_json::array({E});
        N["PARENTS"] = ordered_json::array({"root"});
        A.push_back(N);
    }

    const PkgGraph::Graph G = PkgGraph::Build(A);
    float MinX = 1e30f, MinY = 1e30f, MaxX = -1e30f, MaxY = -1e30f;
    for (const auto &N : G.Nodes)
    {
        MinX = std::min(MinX, N.X);             MinY = std::min(MinY, N.Y);
        MaxX = std::max(MaxX, N.X + 355.0f);    MaxY = std::max(MaxY, N.Y + N.Height);
    }
    const float W = MaxX - MinX, H = MaxY - MinY;
    const float Aspect = W / H;
    //Bounded either way up, and deliberately no tighter. One band-budget pass lands at 1.85 against a 16:9
    //target and the deleted second pass gave 1.76 — this band does NOT distinguish them and is not meant to:
    //what it guards is that the wrap keeps producing a readable rectangle on the one shape where the deleted
    //code had any effect at all, so that shape stops being untested. It fires at a 4.0 target (aspect 4.35).
    if (!(Aspect > 0.8f && Aspect < 3.2f))
        std::printf("    fan-out laid out %.0f x %.0f, aspect %.2f\n", W, H, Aspect);
    CHECK(Aspect > 0.8f);
    CHECK(Aspect < 3.2f);
    //And no overlaps, on the shape that wraps on both axes at once.
    int Overlaps = 0;
    for (size_t I = 0; I < G.Nodes.size(); ++I)
        for (size_t J = I + 1; J < G.Nodes.size(); ++J)
        {
            //Overlapping in X, not identically placed in it: two nodes 100 units apart share 230px of a
            //330px body and collide just as much as two in the same column. An equality test sees neither.
            if (std::abs(G.Nodes[I].X - G.Nodes[J].X) >= 355.0f) continue;
            if (G.Nodes[I].Y < G.Nodes[J].Y + G.Nodes[J].Height
                && G.Nodes[J].Y < G.Nodes[I].Y + G.Nodes[I].Height) ++Overlaps;
        }
    CHECK_EQ(Overlaps, 0);
}

//Build REFUSES a declared position it could not have produced, and hands the refusal back rather than logging
//it (it runs per keystroke in the editor). Both halves have to be observable, and neither was: deleting the
//malformed-shape record left every suite green, because the only test feeding a bad POS used the out-of-range
//arm and never looked at what came back.
TEST(every_refused_position_is_handed_back_with_its_reason)
{
    ordered_json A = ordered_json::array();
    auto N = [&](const char *Id, ordered_json Pos) {
        ordered_json J; J["CID"] = Id; J["TYPE"] = "Group";
        if (!Pos.is_null()) J["POS"] = Pos;
        A.push_back(J);
    };
    N("ok",       ordered_json::array({120, 340}));
    N("huge",     ordered_json::array({5e9, 5e9}));                     // out of range
    N("strings",  ordered_json::array({"10", "20"}));                   // malformed shape
    N("three",    ordered_json::array({1, 2, 3}));                      // malformed shape
    N("object",   ordered_json{{"x", 1}, {"y", 2}});                    // malformed shape
    N("none",     ordered_json());

    const PkgGraph::Graph G = PkgGraph::Build(A);
    //One record per refusal, and none for the good one or the absent one.
    CHECK_EQ(G.RejectedPositions.size(), (size_t)4);
    std::set<std::string> Ids;
    for (const auto &R : G.RejectedPositions)
    {
        Ids.insert(R.NodeId);
        CHECK(!R.Source.empty());
        CHECK(!R.Value.empty());
        //Never the raw value: a package from a content source can carry a megabytes-long POS, and this is
        //built on the per-keystroke path.
        CHECK(R.Value.size() < 80);
    }
    CHECK(Ids.count("huge") == 1);
    CHECK(Ids.count("strings") == 1);
    CHECK(Ids.count("three") == 1);
    CHECK(Ids.count("object") == 1);
    CHECK(Ids.count("ok") == 0);
    CHECK(Ids.count("none") == 0);

    //And this machine's override is named as a DIFFERENT source from the package's own POS, because the two
    //are fixed in different places.
    ordered_json Layout = ordered_json::object();
    Layout["ok"] = ordered_json::array({1e300, 4.0});
    const PkgGraph::Graph G2 = PkgGraph::Build(A, &Layout);
    bool SawOverride = false;
    for (const auto &R : G2.RejectedPositions)
        if (R.NodeId == "ok") { SawOverride = true; CHECK(R.Source.find("machine") != std::string::npos); }
    CHECK(SawOverride);

    //Each refusal addresses ONE slot of NODES. The consumer of these (StampNodePositions) has to answer
    //"was THIS node's file rewritten?", and it used to ask by id — so two nodes sharing a name, or two with
    //no name at all, answered for each other and the publish line said a file was rewritten that was not.
    ordered_json D = ordered_json::array();
    auto Dup = [&](const char *Id, ordered_json Pos) {
        ordered_json J; J["TYPE"] = "Group";
        if (Id) J["CID"] = Id;
        J["POS"] = Pos;
        D.push_back(J);
    };
    Dup("same", ordered_json::array({0, 0}));          // slot 0: fine
    Dup("same", ordered_json::array({5e9, 5e9}));      // slot 1: refused
    Dup(nullptr, ordered_json::array({10, 10}));       // slot 2: fine, and nameless
    Dup(nullptr, ordered_json::array({"a", "b"}));     // slot 3: refused, and nameless

    const PkgGraph::Graph G3 = PkgGraph::Build(D);
    CHECK_EQ(G3.RejectedPositions.size(), (size_t)2);
    std::set<int> Slots;
    for (const auto &R : G3.RejectedPositions) Slots.insert(R.Index);
    CHECK(Slots.count(1) == 1);
    CHECK(Slots.count(3) == 1);
    CHECK(Slots.count(0) == 0);
    CHECK(Slots.count(2) == 0);
}

//A NODES entry that is not an object is kept as a PLACEHOLDER so every index still addresses its own node
//(PkgGraph::Build). It is drawn — a title bar and a line saying what is wrong — so the layout has to reserve
//space for it like anything else. It did not: Build push_backs the placeholder before Height is ever
//computed, so it stayed 0 and the next node in the column was placed on top of it.
TEST(a_malformed_node_still_gets_room_in_the_layout)
{
    ordered_json A = ordered_json::array();
    auto Group = [&](const char *Id) {
        ordered_json J; J["CID"] = Id; J["TYPE"] = "Group"; A.push_back(J);
    };
    Group("first");
    A.push_back("this entry is not an object");     // the placeholder, same layer
    Group("third");

    const PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)3);
    //EXACT, not a range. `> 40 && < a Group's 242` is a 200px window that cannot tell 77 from 177 — a test
    //that passes for any plausible mistake is not covering the number, only its sign. The value is
    //kChromePx 26 + kTitlePx 34 + kTextPx 17, which is what drawNode's placeholder arm draws: a title bar and
    //one disabled line. It measures 63px against the renderer (theEstimatedNodeHeightMatchesTheDrawnOne
    //checks that directly), so 14px of this is chrome padding the placeholder does not spend — over-estimating,
    //which is the safe direction, and small enough that the GUI check bounds it at 20.
    CHECK_EQ(G.Nodes[1].Height, 77.0f);
    CHECK(G.Nodes[1].Height < G.Nodes[0].Height);
    //All three are unparented, so they stack in ONE column — in whatever order the ordering pass chose
    //(it is not document order: an idless node barycentres to the front), so this reads the column off the
    //Y values rather than assuming which node is which.
    CHECK_EQ(G.Nodes[0].X, G.Nodes[1].X);
    CHECK_EQ(G.Nodes[1].X, G.Nodes[2].X);
    const PkgLayout::Options O;
    std::vector<size_t> ByY = {0, 1, 2};
    std::sort(ByY.begin(), ByY.end(), [&](size_t A_, size_t B_) { return G.Nodes[A_].Y < G.Nodes[B_].Y; });
    CHECK_EQ(G.Nodes[ByY[0]].Y, O.OriginY);
    for (int K = 1; K < 3; ++K)
    {
        const PkgGraph::Node &Above = G.Nodes[ByY[(size_t)K - 1]], &Below = G.Nodes[ByY[(size_t)K]];
        //The pitch rule, and the overlap it exists to prevent stated directly — with a zero-height
        //placeholder the two boxes are drawn through each other and this is the check that says so.
        CHECK_EQ(Below.Y, Above.Y + Above.Height + O.RowGap);
        CHECK(Below.Y >= Above.Y + Above.Height);
    }
}

//SafeId is what puts a peer-authored NODE_ID into a log line, so its input is hostile by definition.
TEST(a_hostile_node_id_cannot_reach_the_terminal_through_the_log)
{
    //C0 controls and DEL — the case it always covered.
    //Spelled as char arrays: a C++ hex escape is GREEDY, so "a\x7fb" is the single character 0x7fb, not
    //DEL followed by 'b' — the kind of literal that makes a test assert something it never meant to.
    const char Esc[] = {'a', '\x1b', '[', '3', '1', 'm', 'b', 0};
    const char Nul[] = {'a', '\0', 'b'};
    const char Del[] = {'a', '\x7f', 'b', 0};
    CHECK_EQ(PkgGraph::SafeId(Esc), std::string("a?[31mb"));
    CHECK_EQ(PkgGraph::SafeId(std::string(Nul, 3)), std::string("a?b"));
    CHECK_EQ(PkgGraph::SafeId(Del), std::string("a?b"));
    //...and the C1 controls, which arrive as UTF-8 and were passing through untouched. 0x9B is CSI: on its
    //own it does everything ESC-[ does, so "\xc2\x9b31m" coloured the terminal of whoever read the log.
    const char Csi[] = {'a', '\xc2', '\x9b', '3', '1', 'm', 'b', 0};
    const char Pad[] = {'a', '\xc2', '\x80', 'z', 0};
    const char Eacute[] = {'c', 'a', 'f', '\xc3', '\xa9', 0};
    CHECK_EQ(PkgGraph::SafeId(Csi), std::string("a?31mb"));
    CHECK_EQ(PkgGraph::SafeId(Pad), std::string("a?z"));
    //A legitimate two-byte character that is NOT a C1 control survives intact.
    CHECK_EQ(PkgGraph::SafeId(Eacute), std::string(Eacute));

    //And the truncation cuts on a character boundary. A lone lead byte is invalid UTF-8 — produced by the
    //sanitiser itself, for a consumer that has to re-encode the line.
    std::string Long(95, 'x');
    Long += Eacute[3]; Long += Eacute[4];               // starts at byte 95, so it straddles the 96-byte cap
    Long += std::string(50, 'y');                       // ...and there is more after it, so the cap fires
    const std::string Cut = PkgGraph::SafeId(Long);
    //The cap FIRED. Without these the whole block passes when truncation is disabled outright: "no dangling
    //lead byte" and "at least 96 bytes" are both trivially true of a string that was never cut.
    CHECK(Cut.size() < Long.size());
    //The cap can overshoot by at most the bytes a lead byte already owes — three, for a 4-byte character —
    //plus the three of the marker. It is a cap on the id, not an exact length.
    CHECK(Cut.size() <= 96 + 3 + 3);
    CHECK_EQ(Cut.compare(Cut.size() - 3, 3, "..."), 0);
    CHECK_EQ(Cut.substr(0, 95), std::string(95, 'x'));
    //The e-acute is kept WHOLE — it began before the cap, so its second byte is owed and comes through — and
    //the truncation then happens at the next character boundary rather than inside it.
    CHECK_EQ(Cut.substr(95, 2), std::string(Eacute + 3));
    CHECK_EQ(Cut.size(), (size_t)97 + 3);
    size_t Trailing = 0;
    for (size_t I = 0; I < Cut.size(); ++I)
        if ((static_cast<unsigned char>(Cut[I]) & 0xC0) == 0xC0)        // a lead byte...
        {
            const unsigned char L = static_cast<unsigned char>(Cut[I]);
            size_t Need = (L & 0xE0) == 0xC0 ? 1 : (L & 0xF0) == 0xE0 ? 2 : 3, Have = 0;
            while (I + Have + 1 < Cut.size()
                   && (static_cast<unsigned char>(Cut[I + Have + 1]) & 0xC0) == 0x80) ++Have;
            if (Have < Need) ++Trailing;
        }
    CHECK_EQ(Trailing, (size_t)0);

    //A continuation byte is exempt from the cap only while a LEAD byte is still owed one. Exempting all of
    //them meant a tail of orphan continuations defeated the cap entirely — 96 'x' plus a thousand 0x80s came
    //back as 1096 bytes, and that tail is itself invalid UTF-8, passed through because those bytes are
    //>= 0x20. nlohmann would reject such an id today; this function promises a cap regardless of its caller.
    std::string Orphans(96, 'x');
    Orphans.append(1000, '\x80');
    const std::string Capped = PkgGraph::SafeId(Orphans);
    CHECK(Capped.size() <= 96 + 3);

    //...and the MIRROR of it, which the first version of that fix opened while closing this one: a run of
    //LEAD bytes, each setting a debt the next one never pays. `Owed` never returned to zero, so the cap never
    //fired — 95 'x' plus a thousand 0xF0 came back as 2095 bytes, the lone leads passed through verbatim.
    for (char Lead : {'\xf0', '\xe0', '\xc3'})
    {
        std::string Leads(95, 'x');
        Leads.append(1000, Lead);
        CHECK(PkgGraph::SafeId(Leads).size() <= 96 + 3 + 3);
    }

    //And a 4-byte character straddling the cap is kept whole, not cut after three of its bytes.
    std::string Emoji(94, 'x');
    const char Four[] = {'\xf0', '\x9f', '\x92', '\xa9', 0};       // U+1F4A9, four bytes
    Emoji += Four;
    const std::string Kept = PkgGraph::SafeId(Emoji);
    CHECK(Kept.size() <= 98 + 3);
    CHECK_EQ(Kept.substr(94, 4), std::string(Four));
}

//The renderer and the height estimator must answer "can this be edited as a list?" the same way, because
//drawField draws one line for a value it refuses and FieldPx has to charge one line for it — and that number
//is stamped into the package at publish. They drifted once already: the element scan went into drawField
//alone, and a list of six strings plus one number was drawn 437px and estimated 600px.
TEST(the_string_list_fault_predicate_is_the_one_both_sides_use)
{
    //Editable: absent, null, empty, and every entry a string.
    CHECK_EQ(PkgGraph::StringListFault(nullptr), PkgGraph::kStringListOk);
    const ordered_json Null, Empty = ordered_json::array(), Good = ordered_json::array({"a", "b"});
    CHECK_EQ(PkgGraph::StringListFault(&Null), PkgGraph::kStringListOk);
    CHECK_EQ(PkgGraph::StringListFault(&Empty), PkgGraph::kStringListOk);
    CHECK_EQ(PkgGraph::StringListFault(&Good), PkgGraph::kStringListOk);
    //Not a list at all — reported as the container being wrong, which is what the canvas needs to describe.
    const ordered_json Str("not a list"), Obj = ordered_json{{"k", "v"}}, Num(5);
    CHECK_EQ(PkgGraph::StringListFault(&Str), PkgGraph::kStringListNotAList);
    CHECK_EQ(PkgGraph::StringListFault(&Obj), PkgGraph::kStringListNotAList);
    CHECK_EQ(PkgGraph::StringListFault(&Num), PkgGraph::kStringListNotAList);
    //A list with a bad entry names the FIRST one, so the message can point at it.
    const ordered_json Bad0 = ordered_json::array({5, "b"});
    const ordered_json Bad2 = ordered_json::array({"a", "b", 5, 6});
    const ordered_json Nest = ordered_json::array({"a", ordered_json::array({"x"})});
    //A NULL entry too. null is "nothing to lose" for the FIELD — there is no list there — but inside a list
    //it is an entry the author wrote, and ListToText drops it exactly like any other wrong type. Exempting it
    //here is a one-word change that reopens the destruction this whole arm exists to refuse, and nothing else
    //in the suite would notice: both sides read this predicate, so the height tests agree either way.
    const ordered_json NullIn = ordered_json::array({"a", nullptr});
    CHECK_EQ(PkgGraph::StringListFault(&Bad0), 0);
    CHECK_EQ(PkgGraph::StringListFault(&Bad2), 2);
    CHECK_EQ(PkgGraph::StringListFault(&Nest), 1);
    CHECK_EQ(PkgGraph::StringListFault(&NullIn), 1);
    //And the estimator charges the SAME single line for every one of those, which is what the renderer draws
    //for them. Asserted through EstimateHeight — FieldPx is internal — by comparing the shapes against each
    //other rather than against a constant: every refused shape must cost exactly what every other one does,
    //and an EDITABLE list must cost more, or the predicate has grown to cover values that still draw a box.
    //The absolute pixel agreement with the widget is theEstimatedNodeHeightMatchesTheDrawnOne's job.
    auto HeightWith = [](const ordered_json &Args) {
        return PkgGraph::EstimateHeight(ordered_json{{"TYPE", "DeclareExec"}, {"ARGS", Args}});
    };
    const float Refused = HeightWith(Str);
    for (const ordered_json *V : {&Obj, &Num, &Bad0, &Bad2, &Nest, &NullIn})
        CHECK_EQ(HeightWith(*V), Refused);
    //A good list is measured as the box it actually is. One entry is a single-line input, so it is the
    //TIGHTEST comparison available — and it must still be at least as tall as the refused line, never less.
    CHECK(HeightWith(ordered_json::array({"a", "b", "c"})) > Refused);
}

//DescribeValue is the other half of the same promise: the canvas has to SHOW an author a payload of the wrong
//shape, on a per-frame path, and a package from a content source can carry megabytes in any field.
TEST(a_value_of_the_wrong_shape_is_described_without_being_serialised)
{
    //A container is described by its SIZE. This is the property that matters: dump() on a 10 MB array is
    //10 MB allocated and measured every frame, which is the editor freezing on a package it opened to repair.
    ordered_json Big = ordered_json::array();
    for (int I = 0; I < 20000; ++I) Big.push_back(std::string(200, 'q'));
    const std::string D = PkgGraph::DescribeValue(Big);
    CHECK(D.size() < 40);
    CHECK(D.find("20000") != std::string::npos);

    ordered_json Obj = ordered_json::object();
    for (int I = 0; I < 500; ++I) Obj[std::to_string(I)] = std::string(300, 'z');
    CHECK(PkgGraph::DescribeValue(Obj).size() < 40);

    //A long STRING is cut to the budget, quoted, and marked.
    const std::string Long = PkgGraph::DescribeValue(ordered_json(std::string(5000, 'a')));
    CHECK(Long.size() < 40);
    CHECK(Long.find("...") != std::string::npos);
    //A short one survives intact and readable — the point is to tell the author what is there.
    CHECK_EQ(PkgGraph::DescribeValue(ordered_json("not a list")), std::string("\"not a list\""));
    CHECK_EQ(PkgGraph::DescribeValue(ordered_json(5)), std::string("5"));
    CHECK_EQ(PkgGraph::DescribeValue(ordered_json(true)), std::string("true"));

    //And it is SafeId-clean, because it goes straight to a widget and to nothing that re-escapes it.
    const char Csi[] = {'a', '\xc2', '\x9b', '3', '1', 'm', 0};
    CHECK_EQ(PkgGraph::DescribeValue(ordered_json(std::string(Csi))), std::string("\"a?31m\""));
}

TEST(a_layered_graph_lays_out_at_pinned_coordinates)
{
    ordered_json A = ordered_json::array();
    auto N = [&](const char *Id, const char *Type, std::initializer_list<const char *> Parents) {
        ordered_json J; J["CID"] = Id; J["TYPE"] = Type;
        if (Parents.size())
        {
            ordered_json P = ordered_json::array();
            for (const char *X : Parents) P.push_back(X);
            J["PARENTS"] = P;
        }
        A.push_back(J);
    };
    N("root", "Group",   {});
    N("a",    "Group",   {"root"});     // short
    N("b",    "Content", {"root"});     // taller: Content has five fields
    N("c",    "Group",   {"root"});     // short again, so it must clear b's height and not a's
    N("tail", "Group",   {"a", "b", "c"});

    const PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)5);
    CHECK_EQ(G.Nodes[0].X,  60.0f);  CHECK_EQ(G.Nodes[0].Y,  60.0f);   // root, layer 0
    CHECK_EQ(G.Nodes[1].X, 490.0f);  CHECK_EQ(G.Nodes[1].Y,  60.0f);   // a,    layer 1 row 0
    CHECK_EQ(G.Nodes[2].X, 490.0f);  CHECK_EQ(G.Nodes[2].Y, 392.0f);   // b,    after a's 242 + 90 gap
    CHECK_EQ(G.Nodes[3].X, 490.0f);  CHECK_EQ(G.Nodes[3].Y, 800.0f);   // c,    after b's 318 + 90 gap
    CHECK_EQ(G.Nodes[4].X, 920.0f);  CHECK_EQ(G.Nodes[4].Y,  60.0f);   // tail, layer 2
    //And the heights those Y values are made of, so a failure says WHICH half moved. These moved by 2px when
    //the height estimate stopped charging a full label-and-widget row for rows that hold only SmallButtons —
    //a deliberate correction, and this golden is where that shows up as a decision rather than a side effect.
    //They moved again when the action row started charging for the Separator above it, and a third time (353
    //-> 318) when the estimate stopped charging BASE_TARGETS on a Content node that is not a delta — the
    //canvas has never drawn that field there, so the 35px were a hole reserved in every published layout.
    CHECK_EQ(G.Nodes[1].Height, 242.0f);
    CHECK_EQ(G.Nodes[2].Height, 318.0f);
}

TEST(an_impossible_declared_position_is_rejected_not_honoured)
{
    ordered_json A = ordered_json::array();
    auto N = [&](const char *Id, ordered_json Pos) {
        ordered_json J; J["CID"] = Id; J["TYPE"] = "Group";
        if (!Pos.is_null()) J["POS"] = Pos;
        A.push_back(J);
    };
    N("sane",     ordered_json::array({120, 340}));
    N("huge",     ordered_json::array({5e9, 5e9}));
    N("enormous", ordered_json::array({1e300, 1.0}));
    N("nan",      ordered_json::array({std::numeric_limits<double>::quiet_NaN(), 0.0}));
    N("none",     ordered_json());

    const PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)5);
    //The sane one is honoured, exactly.
    CHECK(G.Nodes[0].HasPos);
    CHECK_EQ(G.Nodes[0].X, 120.0f);
    CHECK_EQ(G.Nodes[0].Y, 340.0f);
    //The rest are treated as undeclared, so the layout places them where they can be seen and fixed.
    for (size_t I = 1; I < G.Nodes.size(); ++I)
    {
        CHECK(!G.Nodes[I].HasPos);
        CHECK(std::isfinite(G.Nodes[I].X));
        CHECK(std::isfinite(G.Nodes[I].Y));
        CHECK(std::abs(G.Nodes[I].X) < 1.0e6f);
        CHECK(std::abs(G.Nodes[I].Y) < 1.0e6f);
    }
}

TEST(a_fixed_graph_lays_out_at_pinned_coordinates)
{
    ordered_json A = ordered_json::array();
    auto N = [&](const char *Id, std::initializer_list<const char *> Parents) {
        ordered_json J;
        J["CID"] = Id;
        J["TYPE"]    = "Group";
        if (Parents.size())
        {
            ordered_json P = ordered_json::array();
            for (const char *X : Parents) P.push_back(X);
            J["PARENTS"] = P;
        }
        A.push_back(J);
    };
    N("root",  {});
    N("a",     {"root"});
    N("b",     {"root"});
    N("c",     {"a", "b"});
    N("d",     {"c"});

    const PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)5);
    const float X0 = 60.0f, Y0 = 60.0f, CW = 430.0f, Gap = 90.0f;
    //The vertical step is the node's OWN height plus the gap, which is the whole point of the change: a
    //constant step is what drew tall nodes through the ones beneath them. Asserted as the rule rather than as
    //a literal, so re-calibrating the height estimate against the renderer does not churn this golden — the
    //magnitude of the estimate is pinned separately, just below.
    CHECK_EQ(G.Nodes[0].X, X0);              CHECK_EQ(G.Nodes[0].Y, Y0);            // root   layer 0
    CHECK_EQ(G.Nodes[1].X, X0 + CW);         CHECK_EQ(G.Nodes[1].Y, Y0);            // a      layer 1 row 0
    CHECK_EQ(G.Nodes[2].X, X0 + CW);
    CHECK_EQ(G.Nodes[2].Y, Y0 + G.Nodes[1].Height + Gap);                           // b      layer 1 row 1
    CHECK_EQ(G.Nodes[3].X, X0 + 2 * CW);     CHECK_EQ(G.Nodes[3].Y, Y0);            // c      layer 2
    CHECK_EQ(G.Nodes[4].X, X0 + 3 * CW);     CHECK_EQ(G.Nodes[4].Y, Y0);            // d      layer 3
    //And the estimate itself is a real number of pixels, not zero (which would silently restore the constant
    //step through the Height == 0 fallback) and not something absurd. A bare Group is the smallest node the
    //canvas draws — a title, a pin row, an id — plus the two reservations it always makes: the "node options"
    //tree as though it were open, and a couple of validation-warning lines.
    CHECK(G.Nodes[1].Height > 90.0f);
    CHECK(G.Nodes[1].Height < 320.0f);
}

//Every golden above this point fits in ONE band, so the two knobs that make a DEEP graph readable — BandGap
//and the band budget itself — were computed and never observed. That matters concretely: the Minecraft bundle
//is 904 layers deep, and laid out one column per layer it is a 388,720px ribbon. A chain long enough to wrap
//is the smallest graph that shows the wrap happening.
TEST(a_deep_chain_wraps_into_bands_and_stays_readable)
{
    ordered_json A = ordered_json::array();
    const int Depth = 24;
    for (int I = 0; I < Depth; ++I)
    {
        ordered_json J; J["CID"] = "n" + std::to_string(I); J["TYPE"] = "Group";
        if (I) J["PARENTS"] = ordered_json::array({"n" + std::to_string(I - 1)});
        A.push_back(J);
    }

    PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes.size(), (size_t)Depth);
    const PkgLayout::Options O;

    //It WRAPPED. One column per layer would put every node on the same row; at least one must start a new one.
    int Bands = 0;
    for (int I = 0; I < Depth; ++I) if (G.Nodes[(size_t)I].X == O.OriginX) ++Bands;
    CHECK(Bands >= 2);

    //Each band is a fresh left-to-right run at a strictly greater Y, and the step between bands is the
    //TALLEST block in the band above plus BandGap — not a constant, and not zero, which is what a missing
    //BandGap would look like (bands touching, the last node of one band drawn into the first of the next).
    float PrevBandTop = -1.0f, PrevBandTall = 0.0f;
    float BandTop = G.Nodes[0].Y, BandTall = 0.0f;
    for (int I = 0; I < Depth; ++I)
    {
        const PkgGraph::Node &N = G.Nodes[(size_t)I];
        if (N.X == O.OriginX && I > 0)
        {
            PrevBandTop = BandTop; PrevBandTall = BandTall;
            BandTop = N.Y; BandTall = 0.0f;
            //Associated the way Compute associates it. `PrevBandTop + (PrevBandTall + BandGap)` is a
            //different float expression from `PrevBandTop + PrevBandTall + BandGap`, and the two agree today
            //only because every operand happens to be integer-valued; a non-integer kTextPx would make this
            //golden fail by rounding rather than by regression.
            CHECK_EQ(BandTop, PrevBandTop + (PrevBandTall + O.BandGap));
        }
        CHECK_EQ(N.Y, BandTop);                                   // one node per layer, so every band row is flat
        BandTall = std::max(BandTall, N.Height + O.RowGap);
    }
    CHECK(PrevBandTop >= 0.0f);                                   // the band arithmetic above actually ran

    //And the POINT of the budget: a shape a person can look at. The un-banded ribbon this replaced is 24
    //columns wide and one node tall — an aspect of ~31 against a 1.78 target. The budget is chosen from two
    //whole-number candidates, so the result is approximate by construction; anything under 4 means it is
    //choosing at all.
    float MinX = G.Nodes[0].X, MaxX = MinX, MinY = G.Nodes[0].Y, MaxY = MinY;
    for (const PkgGraph::Node &N : G.Nodes)
    {
        MinX = std::min(MinX, N.X); MaxX = std::max(MaxX, N.X + 330.0f);
        MinY = std::min(MinY, N.Y); MaxY = std::max(MaxY, N.Y + N.Height);
    }
    const float Aspect = (MaxX - MinX) / std::max(1.0f, MaxY - MinY);
    CHECK(Aspect > 0.4f);
    CHECK(Aspect < 4.0f);
}

//Document ORDER seeds the within-layer ordering, so this has to be tested on a WIDE layer — a chain has one
//node per layer, where the property holds trivially and the test proves nothing. It matters concretely because
//the editor enumerates node files with QDir::entryList (which skips dotfiles) and the publisher with
//std::filesystem::directory_iterator (which does not): the two can hand the layout a different order for the
//same bundle, and the author would then see one picture while a different one is stamped into POS.
TEST(a_wide_layers_ordering_does_not_depend_on_document_order)
{
    auto BuildPositions = [](bool Reversed) {
        std::vector<ordered_json> Ns;
        ordered_json Root; Root["CID"] = "root"; Root["TYPE"] = "Group";
        Ns.push_back(Root);
        for (int I = 0; I < 10; ++I)
        {
            ordered_json J;
            J["CID"] = "c" + std::to_string(I);
            J["TYPE"]    = "Group";
            J["PARENTS"] = ordered_json::array({"root"});
            Ns.push_back(J);
        }
        if (Reversed) std::reverse(Ns.begin(), Ns.end());
        ordered_json A = ordered_json::array();
        for (const auto &J : Ns) A.push_back(J);
        std::map<std::string, std::pair<float, float>> Pos;
        const PkgGraph::Graph G = PkgGraph::Build(A);
        for (const auto &Nd : G.Nodes) Pos[Nd.Id] = {Nd.X, Nd.Y};
        return Pos;
    };
    const auto Forward = BuildPositions(false), Backward = BuildPositions(true);
    for (const auto &[Id, P] : Forward)
    {
        CHECK(Backward.count(Id) == 1);
        if (Backward.count(Id) != 1) continue;
        CHECK_EQ(Backward.at(Id).first,  P.first);
        CHECK_EQ(Backward.at(Id).second, P.second);
    }
}
