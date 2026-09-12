#include "pkglayout.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>

namespace PkgLayout
{
using PkgGraph::Graph;
using PkgGraph::Link;

namespace {

//The adjacency the whole pass runs on. External parents (ParentIndex < 0) are reference chips, not boxes, so
//they carry no depth and no ordering weight — including them would drag every node that mentions a shared
//library into a phantom layer.
struct Adjacency
{
    std::vector<std::vector<int>> Parents;
    std::vector<std::vector<int>> Children;
};

Adjacency BuildAdjacency(const Graph &G)
{
    const int N = (int)G.Nodes.size();
    Adjacency A;
    A.Parents.resize(N);
    A.Children.resize(N);
    for (const Link &L : G.Links)
    {
        if (L.ParentIndex < 0 || L.ParentIndex >= N) continue;
        if (L.ChildIndex  < 0 || L.ChildIndex  >= N) continue;
        if (L.ParentIndex == L.ChildIndex) continue;          //a self-parent is malformed, not a layer step
        A.Parents[L.ChildIndex].push_back(L.ParentIndex);
        A.Children[L.ParentIndex].push_back(L.ChildIndex);
    }
    return A;
}

//Longest-path layering by Kahn's algorithm. Iterative on purpose: the recursive form recurses once per chain
//step, and the real bundles are 904 deep — deep enough that a slightly longer chain would blow the stack on a
//machine with a small thread stack, which is exactly the sort of failure that shows up only on someone else's
//computer. Nodes left unvisited are in a cycle; they keep depth 0 rather than hanging the pass.
std::vector<int> AssignLayers(const Graph &G, const Adjacency &A)
{
    const int N = (int)G.Nodes.size();
    std::vector<int> Depth(N, 0), InDeg(N, 0);
    for (int I = 0; I < N; ++I) InDeg[I] = (int)A.Parents[I].size();

    std::deque<int> Q;
    for (int I = 0; I < N; ++I) if (InDeg[I] == 0) Q.push_back(I);
    while (!Q.empty())
    {
        const int U = Q.front(); Q.pop_front();
        for (int V : A.Children[U])
        {
            Depth[V] = std::max(Depth[V], Depth[U] + 1);
            if (--InDeg[V] == 0) Q.push_back(V);
        }
    }
    return Depth;
}

//Median-of-neighbours ordering (Eades & Wells): sweep down using parents, up using children, keeping nodes with
//no neighbour on that side exactly where they are. Ties break on the previous ordinal and then on node index,
//so the result is a pure function of the graph — no map iteration order, no clock, no randomness.
void OrderWithinLayers(const Graph &G, const Adjacency &A, const std::vector<int> &Depth,
                       std::vector<std::vector<int>> &Layers, int Sweeps)
{
    std::vector<int> Ordinal((int)G.Nodes.size(), 0);
    auto Reindex = [&] {
        for (const auto &L : Layers)
            for (int I = 0; I < (int)L.size(); ++I) Ordinal[L[I]] = I;
    };
    Reindex();

    auto MedianOf = [&](const std::vector<int> &Neighbours, int Fallback) -> double {
        if (Neighbours.empty()) return (double)Fallback;
        std::vector<int> Pos;
        Pos.reserve(Neighbours.size());
        for (int N : Neighbours) Pos.push_back(Ordinal[N]);
        std::sort(Pos.begin(), Pos.end());
        const size_t M = Pos.size() / 2;
        //Even counts take the mean of the two middles: a node with parents at the top and bottom of the layer
        //belongs between them, not arbitrarily at one of them.
        return (Pos.size() % 2) ? (double)Pos[M] : ((double)Pos[M - 1] + (double)Pos[M]) / 2.0;
    };

    auto SortLayer = [&](std::vector<int> &L, const std::vector<std::vector<int>> &Side) {
        std::vector<std::pair<double, int>> Key;
        Key.reserve(L.size());
        for (int Idx : L) Key.emplace_back(MedianOf(Side[Idx], Ordinal[Idx]), Idx);
        std::stable_sort(Key.begin(), Key.end(), [&](const auto &X, const auto &Y) {
            if (X.first != Y.first) return X.first < Y.first;
            if (Ordinal[X.second] != Ordinal[Y.second]) return Ordinal[X.second] < Ordinal[Y.second];
            return X.second < Y.second;
        });
        for (int I = 0; I < (int)L.size(); ++I) L[I] = Key[I].second;
    };

    for (int S = 0; S < Sweeps; ++S)
    {
        for (int D = 1; D < (int)Layers.size(); ++D) { SortLayer(Layers[D], A.Parents); Reindex(); }
        for (int D = (int)Layers.size() - 2; D >= 0; --D) { SortLayer(Layers[D], A.Children); Reindex(); }
    }
    (void)Depth;
}

//How many sub-columns a layer needs, and its drawn width in columns.
inline int SubColumns(int Count, int MaxRows) { return (Count <= MaxRows) ? 1 : (Count + MaxRows - 1) / MaxRows; }

} // namespace

void Compute(Graph &G, const Options &O)
{
    const int N = (int)G.Nodes.size();
    if (N <= 0) return;

    const Adjacency A = BuildAdjacency(G);
    const std::vector<int> Depth = AssignLayers(G, A);

    int MaxDepth = 0;
    for (int D : Depth) MaxDepth = std::max(MaxDepth, D);
    std::vector<std::vector<int>> Layers((size_t)MaxDepth + 1);
    for (int I = 0; I < N; ++I) Layers[(size_t)Depth[I]].push_back(I);
    //Seed each layer by NODE_ID, not by document order. The ordering sweeps below refine this, but they start
    //from it, so with a document-order seed the same graph laid out from a different FILE ORDER produced a
    //different picture — and the editor and the publisher genuinely enumerate differently (QDir::entryList
    //skips dotfiles, std::filesystem::directory_iterator does not). That is a layout the author never saw
    //being stamped into POS, and a CID that changes because of how a directory happened to be read.
    for (auto &L : Layers)
        std::stable_sort(L.begin(), L.end(), [&](int A, int B) {
            if (G.Nodes[(size_t)A].Id != G.Nodes[(size_t)B].Id)
                return G.Nodes[(size_t)A].Id < G.Nodes[(size_t)B].Id;
            return A < B;                       // an empty/duplicate id still orders deterministically
        });

    OrderWithinLayers(G, A, Depth, Layers, std::max(0, O.Sweeps));

    const int MaxRows = std::max(1, O.MaxRows);

    //---- band packing -------------------------------------------------------
    //Each layer is a block `SubColumns` columns wide and at most MaxRows tall. Bands are filled left to right
    //until adding the next block would pass the band's column budget, which is chosen so the finished drawing
    //approaches TargetAspect. Solving it exactly is circular (the budget depends on the height, which depends
    //on the budget), so it is derived from the totals once — the drawing only has to be readable, not optimal.
    //Each layer becomes a block: Rows tall, Cols wide. A layer at or under the cap is one column.
    std::vector<int> Rows((size_t)MaxDepth + 1, 1), Cols((size_t)MaxDepth + 1, 1);
    for (size_t D = 0; D < Layers.size(); ++D)
    {
        const int Count = std::max(1, (int)Layers[D].size());
        Rows[D] = std::min(Count, MaxRows);
        Cols[D] = (Count + Rows[D] - 1) / Rows[D];
    }

    //The band's height is the TALLEST block in it, so the budget has to be derived from the real distribution
    //of block heights — not from MaxRows. Estimating with MaxRows is what made a 904-layer chain (every block
    //one row tall) compute a band budget three times too wide and draw the ribbon this wrap exists to avoid.
    //The 90th percentile rather than the max: one outlier layer must not set the budget for all the others.
    std::vector<int> SortedRows = Rows;
    std::sort(SortedRows.begin(), SortedRows.end());
    const int TypicalRows = SortedRows.empty() ? 1
                          : SortedRows[std::min(SortedRows.size() - 1, (size_t)((double)SortedRows.size() * 0.9))];

    long long TotalColumns = 0;
    for (size_t D = 0; D < Layers.size(); ++D) TotalColumns += Cols[D];

    const double ColumnW = (double)O.ColumnStep;
    const double BandH   = (double)TypicalRows * (double)O.RowStep + (double)O.BandGap;
    //width == aspect * height  ⇒  budget*ColumnW == aspect * (TotalColumns/budget) * BandH
    const double Ideal   = std::sqrt(((double)TotalColumns * BandH * (double)O.TargetAspect) / std::max(1.0, ColumnW));
    const int BandBudget = std::max(1, (int)std::llround(Ideal));

    //A single layer wider than the whole band would stick out past every other band. Re-shape it to exactly the
    //budget and let it grow downward instead — that is what turns a 1000-wide fan-out into a rectangle.
    for (size_t D = 0; D < Layers.size(); ++D)
        if (Cols[D] > BandBudget)
        {
            const int Count = (int)Layers[D].size();
            Cols[D] = BandBudget;
            Rows[D] = (Count + Cols[D] - 1) / Cols[D];
        }

    float BandTop = O.OriginY;
    int   ColumnInBand = 0;
    int   TallestInBand = 0;

    for (size_t D = 0; D < Layers.size(); ++D)
    {
        const std::vector<int> &L = Layers[D];
        const int BlockCols = Cols[D], BlockRows = std::max(1, Rows[D]);
        //A block that would overflow the band starts a new one — unless the band is empty, in which case it
        //simply is the band (a single layer wider than the whole budget still has to go somewhere).
        if (ColumnInBand > 0 && ColumnInBand + BlockCols > BandBudget)
        {
            BandTop += (float)((double)TallestInBand * (double)O.RowStep + (double)O.BandGap);
            ColumnInBand = 0;
            TallestInBand = 0;
        }

        TallestInBand = std::max(TallestInBand, BlockRows);

        for (int I = 0; I < (int)L.size(); ++I)
        {
            //Column-major within the block: consecutive nodes of a layer stay vertically adjacent, so a wrapped
            //layer reads as a column that continued, not as a row.
            const int SubCol = I / BlockRows;
            const int Row    = I % BlockRows;
            PkgGraph::Node &Nd = G.Nodes[L[I]];
            Nd.X = O.OriginX + (float)((double)(ColumnInBand + SubCol) * ColumnW);
            Nd.Y = BandTop   + (float)((double)Row * (double)O.RowStep);
            //HasPos stays as it was on purpose: it means "somebody DECLARED this position" (a node's POS or
            //this machine's override), which is what tells the canvas there is nothing to persist. A computed
            //position is a default the reader made up, so it must not masquerade as a declared one.
        }
        ColumnInBand += BlockCols;
    }
}

void ComputeUnplaced(Graph &G, const Options &O)
{
    //Remember what was already placed, lay the whole graph out, then restore the placed ones. Laying out the
    //WHOLE graph (rather than the unplaced subset alone) is what keeps an unplaced node in the right LAYER
    //relative to its neighbours — its depth depends on nodes that already have positions.
    //
    //It does NOT guarantee a new node misses a dragged one: the computed slot is in layout space and a dragged
    //node can sit anywhere. Overlap is possible and is the author's to resolve by moving it.
    std::vector<char> Had((size_t)G.Nodes.size(), 0);
    std::vector<float> X((size_t)G.Nodes.size()), Y((size_t)G.Nodes.size());
    for (size_t I = 0; I < G.Nodes.size(); ++I)
    { Had[I] = G.Nodes[I].HasPos ? 1 : 0; X[I] = G.Nodes[I].X; Y[I] = G.Nodes[I].Y; }

    //Nothing unplaced means nothing to do — and that is the STEADY STATE once POS stamping has run, so
    //without this every canvas cache rebuild (i.e. every edit) re-ran a full Kahn plus four median sweeps over
    //the whole graph to produce coordinates it was about to throw away. On Minecraft that is ~21 ms a keystroke.
    if (std::find(Had.begin(), Had.end(), (char)0) == Had.end()) return;

    Compute(G, O);

    for (size_t I = 0; I < G.Nodes.size(); ++I)
        if (Had[I]) { G.Nodes[I].X = X[I]; G.Nodes[I].Y = Y[I]; G.Nodes[I].HasPos = true; }
}

long long CountCrossings(const Graph &G)
{
    //Two edges cross when their endpoints are ordered oppositely on the two sides. Counted only between edges
    //that share the same pair of columns, which is where a layered drawing's crossings actually live.
    struct Edge { float PX, PY, CX, CY; };
    std::vector<Edge> E;
    E.reserve(G.Links.size());
    for (const Link &L : G.Links)
    {
        if (L.ParentIndex < 0 || L.ParentIndex == L.ChildIndex) continue;
        const auto &P = G.Nodes[L.ParentIndex];
        const auto &C = G.Nodes[L.ChildIndex];
        E.push_back({P.X, P.Y, C.X, C.Y});
    }
    long long Crossings = 0;
    for (size_t I = 0; I < E.size(); ++I)
        for (size_t J = I + 1; J < E.size(); ++J)
        {
            if (E[I].PX != E[J].PX || E[I].CX != E[J].CX) continue;   //different column pair: not comparable
            const bool A = (E[I].PY < E[J].PY) && (E[I].CY > E[J].CY);
            const bool B = (E[I].PY > E[J].PY) && (E[I].CY < E[J].CY);
            if (A || B) ++Crossings;
        }
    return Crossings;
}

} // namespace PkgLayout
