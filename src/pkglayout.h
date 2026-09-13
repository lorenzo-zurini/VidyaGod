#ifndef PKGLAYOUT_H
#define PKGLAYOUT_H

#include "pkggraph.h"

// ---------------------------------------------------------------------------
// PkgLayout — the automatic canvas layout for a node graph.
//
// Pure: it reads a PkgGraph::Graph's Links and writes each Node's X/Y. No UI, no filesystem, no randomness —
// the same graph always produces the same coordinates, which is what lets a computed layout be STAMPED into
// the package at publish time (a layout that wandered between runs would churn the package's bytes).
//
// The shape it has to survive is not the obvious one. A library's graphs are not "medium DAGs": the Minecraft
// bundle is 2775 nodes and 5325 links whose longest path is 904 layers, while its WIDEST layer holds 15 nodes.
// Laying one column per layer draws a ribbon 388,720 px wide and 4,500 tall — technically a layout, useless as
// a picture. The opposite extreme is just as real (a base with a thousand siblings hanging off it), so both
// axes wrap:
//
//   * a layer taller than the band cap wraps into a BLOCK of sub-columns (handles enormous fan-in/fan-out),
//   * the layer sequence itself wraps into BANDS stacked downward, like lines of text (handles enormous depth).
//
// Bands read strictly left-to-right, the same direction as every other band, so "parents are to my left" holds
// everywhere inside a band. The cost is the wire from the end of one band to the start of the next; a
// serpentine would avoid it, but then every other band reads backwards, which is worse.
// ---------------------------------------------------------------------------

namespace PkgLayout
{

struct Options
{
    float ColumnStep = 430.0f;   //x distance between adjacent layers — wider than one node body, so columns never overlap
    //Vertical placement is by HEIGHT, not by a row index: a node's pitch is its own drawn height plus RowGap.
    //A constant step was the bug this replaced — a RegEdit with 59 registry rows, or a BinaryPatch with a
    //dozen entries, is several times taller than the step and simply drew through the node beneath it.
    float RowGap     =  90.0f;   //clear space between the bottom of one node and the top of the next
    float RowStep    = 300.0f;   //pitch for a node whose height is unknown (Height == 0)
    float BandGap    = 220.0f;   //extra y between one band and the next
    float OriginX    =  60.0f;
    float OriginY    =  60.0f;
    //Height budget for one sub-column, expressed as a number of NOMINAL rows so the knob still reads the way
    //it did. A layer taller than this wraps into another sub-column.
    int   MaxRows    = 18;
    //Ordering passes over the layers (down, then up, is one sweep). 4 is where crossing counts stop improving
    //measurably on the real bundles; the cost is O(sweeps * (V + E log E)).
    int   Sweeps     = 4;
    //Target width/height of the finished drawing. Bands are chosen to approach it, so a graph that is mostly
    //depth (Minecraft) and one that is mostly breadth both land on a readable rectangle.
    float TargetAspect = 16.0f / 9.0f;
};

//Assigns X/Y to EVERY node in G (including ones that already carry a position — callers decide what to keep).
//Leaves HasPos alone: it records whether a position was DECLARED (node POS, or this machine's override), and a
//computed default is not a declaration.
//Deterministic and cycle-safe: a PARENTS cycle is broken by leaving the back-edge's child at the depth it
//already reached, so a malformed package still draws.
void Compute(PkgGraph::Graph &G, const Options &O = {});

//Assigns X/Y only to nodes whose HasPos is false, leaving positioned ones exactly where they are. This is the
//canvas path: a bundle that carries POS for some nodes and not others opens with the new ones placed sensibly.
void ComputeUnplaced(PkgGraph::Graph &G, const Options &O = {});

//Number of edge crossings in the current coordinates — the quality measure the ordering pass minimises, exposed
//so a test can assert the heuristic actually helps rather than trusting that it does.
long long CountCrossings(const PkgGraph::Graph &G);

}

#endif // PKGLAYOUT_H
