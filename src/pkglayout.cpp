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

//Split a layer's ordered nodes into sub-columns, each at most Budget tall. Greedy and order-preserving, so a
//wrapped layer still reads as a column that continued. A node taller than the whole budget gets a column of
//its own rather than being dropped or overlapped — the budget is a target, the node's height is a fact.
std::vector<std::vector<int>> SplitByHeight(const std::vector<int> &L,
                                            const std::vector<float> &Pitch, float Budget)
{
    std::vector<std::vector<int>> Cols;
    float Used = 0.0f;
    for (int Idx : L)
    {
        const float P = Pitch[(size_t)Idx];
        if (Cols.empty() || (!Cols.back().empty() && Used + P > Budget))
        { Cols.emplace_back(); Used = 0.0f; }
        Cols.back().push_back(Idx);
        Used += P;
    }
    if (Cols.empty()) Cols.emplace_back();
    return Cols;
}

//The tallest sub-column in a block, which is what the block costs vertically.
float BlockHeight(const std::vector<std::vector<int>> &Cols, const std::vector<float> &Pitch)
{
    float Tallest = 0.0f;
    for (const auto &C : Cols)
    {
        float H = 0.0f;
        for (int Idx : C) H += Pitch[(size_t)Idx];
        Tallest = std::max(Tallest, H);
    }
    return Tallest;
}

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
        std::stable_sort(L.begin(), L.end(), [&](int Lhs, int Rhs) {
            if (G.Nodes[(size_t)Lhs].Id != G.Nodes[(size_t)Rhs].Id)
                return G.Nodes[(size_t)Lhs].Id < G.Nodes[(size_t)Rhs].Id;
            return Lhs < Rhs;                   // an empty/duplicate id still orders deterministically
        });

    OrderWithinLayers(G, A, Depth, Layers, std::max(0, O.Sweeps));

    const int MaxRows = std::max(1, O.MaxRows);

    //---- vertical pitch -----------------------------------------------------
    //Each node's own drawn height plus the gap. A node whose height nothing estimated (an empty placeholder,
    //a caller that built a Graph by hand) falls back to the nominal step, so the old behaviour is what you get
    //when there is nothing better to go on.
    std::vector<float> Pitch((size_t)N, O.RowStep);
    for (int I = 0; I < N; ++I)
        if (G.Nodes[(size_t)I].Height > 1.0f) Pitch[(size_t)I] = G.Nodes[(size_t)I].Height + O.RowGap;

    //---- band packing -------------------------------------------------------
    //Each layer is a block: as many sub-columns as its nodes need at ColumnBudget tall, and as tall as its
    //tallest sub-column. Bands are filled left to right until adding the next block would pass the band's
    //column budget, which is chosen so the finished drawing approaches TargetAspect. Solving it exactly is
    //circular (the budget depends on the height, which depends on the budget), so it is derived from the
    //totals once — the drawing only has to be readable, not optimal.
    const float ColumnBudget = (float)MaxRows * O.RowStep;

    //Splitting a layer into sub-columns and deciding how wide a band may be are mutually dependent: the band
    //budget is derived from how tall the blocks are, and an over-wide block is then re-split — which can only
    //make it TALLER, so the budget was aimed at a shape the drawing would not have. Two passes settle it. Not
    //iterated to a fixed point: the second budget is computed from real post-re-split heights, which is the
    //information the first pass lacked, and a third pass has nothing new to learn.
    std::vector<std::vector<std::vector<int>>> Blocks(Layers.size());
    std::vector<float> BlockH(Layers.size(), 0.0f);
    int BandBudget = 1;

    for (int Pass = 0; Pass < 2; ++Pass)
    {
        //Fresh from the ordered layers every pass — re-splitting an already-split block is not the same
        //operation and would not be reproducible.
        for (size_t D = 0; D < Layers.size(); ++D)
        {
            Blocks[D] = SplitByHeight(Layers[D], Pitch, ColumnBudget);
            BlockH[D] = BlockHeight(Blocks[D], Pitch);
        }
        if (Pass > 0)
            for (size_t D = 0; D < Layers.size(); ++D)
                if ((int)Blocks[D].size() > BandBudget)
                {
                    double Budget = 0.0;
                    for (int Idx : Layers[D]) Budget += (double)Pitch[(size_t)Idx];
                    Budget /= (double)BandBudget;
                    for (int Tries = 0; Tries < 64 && (int)Blocks[D].size() > BandBudget; ++Tries)
                    {
                        Blocks[D] = SplitByHeight(Layers[D], Pitch, (float)Budget);
                        Budget *= 1.1;
                    }
                    BlockH[D] = BlockHeight(Blocks[D], Pitch);
                }

        //The band's height is the TALLEST block in it, so the budget has to come from the real distribution of
        //block heights — not from the cap. Estimating with the cap is what made a 904-layer chain (every block
        //one node tall) compute a band budget three times too wide and draw the ribbon this wrap exists to
        //avoid. The 90th percentile rather than the max: one outlier layer must not set the budget for all the
        //others.
        std::vector<float> SortedH = BlockH;
        std::sort(SortedH.begin(), SortedH.end());
        const double TypicalH = SortedH.empty() ? (double)O.RowStep
                              : (double)SortedH[std::min(SortedH.size() - 1,
                                                         (size_t)((double)SortedH.size() * 0.9))];
        long long TotalColumns = 0;
        for (size_t D = 0; D < Layers.size(); ++D) TotalColumns += (long long)Blocks[D].size();

        const double ColumnW = (double)O.ColumnStep;
        const double BandH   = std::max(1.0, TypicalH) + (double)O.BandGap;
        //width == aspect * height  ⇒  budget*ColumnW == aspect * (TotalColumns/budget) * BandH
        const double Ideal = std::sqrt(((double)TotalColumns * BandH * (double)O.TargetAspect)
                                       / std::max(1.0, ColumnW));
        //Ideal is continuous; a band budget is a whole number of columns, and on a SMALL graph the two
        //candidates either side of it are not close to equivalent — rounding 3.35 down to 3 split a five-node
        //graph across two bands when all four of its columns fit on one. So evaluate both and keep the better.
        //
        //Judged by RATIO to the target, so "twice too wide" and "twice too tall" weigh the same — and by
        //divisions rather than std::log, because this choice is baked into published POS and therefore into
        //the package's CID: std::log is not correctly rounded and varies between libm versions, where these
        //divisions are the same exact operation on every machine.
        auto AspectOf = [&](int Budget) {
            const double Bands = std::ceil((double)TotalColumns / (double)std::max(1, Budget));
            const double W = (double)Budget * ColumnW, H = Bands * BandH;
            return (H > 0.0) ? W / H : 0.0;
        };
        auto Badness = [&](int Budget) {
            const double Asp = AspectOf(Budget);
            if (Asp <= 0.0) return 1e9;
            const double T = std::max(1e-6, (double)O.TargetAspect);
            return (Asp > T) ? (Asp / T) : (T / Asp);
        };
        const int Low  = std::max(1, (int)std::floor(Ideal));
        const int High = std::max(1, (int)std::ceil(Ideal));
        BandBudget = (Badness(High) < Badness(Low)) ? High : Low;
    }

    const double ColumnW = (double)O.ColumnStep;

    //The final split at the settled budget: a single layer wider than the whole band would stick out past
    //every other one, so it is re-split with a taller column budget and grows downward instead — that is what
    //turns a 1000-wide fan-out into a rectangle. Raising the budget can only reduce the column count, so this
    //terminates; the loop bound is belt and braces against a pathological pitch.
    for (size_t D = 0; D < Layers.size(); ++D)
    {
        if ((int)Blocks[D].size() <= BandBudget) continue;
        double Budget = 0.0;
        for (int Idx : Layers[D]) Budget += (double)Pitch[(size_t)Idx];
        Budget /= (double)BandBudget;
        for (int Tries = 0; Tries < 64 && (int)Blocks[D].size() > BandBudget; ++Tries)
        {
            Blocks[D] = SplitByHeight(Layers[D], Pitch, (float)Budget);
            Budget *= 1.1;
        }
        BlockH[D] = BlockHeight(Blocks[D], Pitch);
    }

    float BandTop = O.OriginY;
    int   ColumnInBand = 0;
    float TallestInBand = 0.0f;

    for (size_t D = 0; D < Layers.size(); ++D)
    {
        const int BlockCols = (int)Blocks[D].size();
        //A block that would overflow the band starts a new one — unless the band is empty, in which case it
        //simply is the band (a single layer wider than the whole budget still has to go somewhere).
        if (ColumnInBand > 0 && ColumnInBand + BlockCols > BandBudget)
        {
            BandTop += (float)(TallestInBand + O.BandGap);
            ColumnInBand = 0;
            TallestInBand = 0.0f;
        }

        TallestInBand = std::max(TallestInBand, BlockH[D]);

        for (int C = 0; C < BlockCols; ++C)
        {
            //Column-major within the block: consecutive nodes of a layer stay vertically adjacent, so a
            //wrapped layer reads as a column that continued, not as a row. Y advances by each node's OWN
            //pitch, which is the whole point — a tall node pushes the next one down by its own height rather
            //than by a constant that it long ago outgrew.
            float Y = BandTop;
            for (int Idx : Blocks[D][(size_t)C])
            {
                PkgGraph::Node &Nd = G.Nodes[(size_t)Idx];
                Nd.X = O.OriginX + (float)((double)(ColumnInBand + C) * ColumnW);
                Nd.Y = Y;
                Y += Pitch[(size_t)Idx];
                //HasPos stays as it was on purpose: it means "somebody DECLARED this position" (a node's POS
                //or this machine's override), which is what tells the canvas there is nothing to persist. A
                //computed position is a default the reader made up, so it must not masquerade as a declared one.
            }
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
