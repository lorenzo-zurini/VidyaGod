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

#include <algorithm>
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
        Nd["NODE_ID"] = "n" + std::to_string(I);
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
    ordered_json Root; Root["NODE_ID"] = "root"; Root["TYPE"] = "Group";
    A.push_back(Root);
    for (int I = 0; I < N; ++I)
    {
        ordered_json Nd;
        Nd["NODE_ID"] = "c" + std::to_string(I);
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
    { ordered_json P; P["NODE_ID"] = "p" + std::to_string(I); P["TYPE"] = "Group"; A.push_back(P); }
    for (int I = 0; I < N; ++I)
    {
        ordered_json C;
        C["NODE_ID"] = "c" + std::to_string(I);
        C["TYPE"]    = "Group";
        C["PARENTS"] = ordered_json::array({"p" + std::to_string(N - 1 - I)});
        A.push_back(C);
    }
    PkgGraph::Graph Ordered = PkgGraph::Build(A);

    PkgLayout::Options NoSweeps;
    NoSweeps.Sweeps = 0;
    PkgGraph::Graph Raw = PkgGraph::Build(A);
    PkgLayout::Compute(Raw, NoSweeps);

    //Document order draws every pair crossed: C(12,2) = 66. Asserting only "ordered <= raw" is blind, because
    //disabling the ordering pass changes BOTH sides identically and the comparison still holds. A plain
    //reversal is what the median heuristic exists to solve, so demand it be solved completely.
    CHECK_EQ(PkgLayout::CountCrossings(Raw), 66LL);
    CHECK_EQ(PkgLayout::CountCrossings(Ordered), 0LL);
}

// ---------------------------------------------------------------------------
// POS on the node, and the local override on top of it.
// ---------------------------------------------------------------------------

TEST(a_node_POS_is_used_as_the_default_position)
{
    ordered_json A = ordered_json::array();
    ordered_json N; N["NODE_ID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({1234.0, 567.0});
    A.push_back(N);
    PkgGraph::Graph G = PkgGraph::Build(A);
    CHECK_EQ(G.Nodes[0].X, 1234.0f);
    CHECK_EQ(G.Nodes[0].Y, 567.0f);
    CHECK(G.Nodes[0].HasPos);
}

TEST(a_local_override_beats_the_node_POS)
{
    ordered_json A = ordered_json::array();
    ordered_json N; N["NODE_ID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({1234.0, 567.0});
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
        ordered_json N; N["NODE_ID"] = "a"; N["TYPE"] = "Group"; N["POS"] = Bad;
        ordered_json M; M["NODE_ID"] = "b"; M["TYPE"] = "Group"; M["PARENTS"] = ordered_json::array({"a"});
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
    ordered_json N; N["NODE_ID"] = "a"; N["TYPE"] = "Group"; N["POS"] = ordered_json::array({500.0, 500.0});
    ordered_json M; M["NODE_ID"] = "b"; M["TYPE"] = "Group"; M["PARENTS"] = ordered_json::array({"a"});
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
    ordered_json X; X["NODE_ID"] = "x"; X["TYPE"] = "Group"; X["PARENTS"] = ordered_json::array({"y"});
    ordered_json Y; Y["NODE_ID"] = "y"; Y["TYPE"] = "Group"; Y["PARENTS"] = ordered_json::array({"x"});
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
