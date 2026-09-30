#ifndef PKGFORM_H
#define PKGFORM_H

#include "pkggraph.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PkgForm — the body of one node on the package canvas: its properties (VARIANT, SECTION, RECOMMENDED) and its
// LAYERS as a tree of folded rows (SECTION folders, then each layer and, inside it, the fields its type declares).
//
// Drawn with ordinary ImGui widgets into the node's own child window, at the canvas zoom: every size here is a
// UNIT scaled by Host.Z, the font is already at the zoomed size, so the form is crisp and correctly hit-tested at
// every zoom without any coordinate trickery.
//
// It edits the node JSON it is handed and calls Host.Dirty() on every write; it never writes anything merely by
// being drawn (a node whose bytes changed on view would change its CID for every peer).
// ---------------------------------------------------------------------------

namespace PkgForm
{

//What the form needs from the canvas.
struct Host
{
    float Z = 1.0f;                                                     // zoom: one unit = Z pixels
    //A tree row whose open state the canvas holds (Key is unique within the node). Returns whether it is open; an
    //open row must be closed with ImGui::TreePop().
    std::function<bool(const std::string &Key, const std::string &Label)> Fold;
    std::function<std::string(const std::string &Ref)> RefLabel;        // a node reference's display name
    const PkgGraph::VarFacets *Facets = nullptr;                        // launcher facets: names WHEN-gated layers
    std::function<void()> Dirty;                                        // the node was edited
    std::function<void(const std::string &Action)> Request;             // a host action (browse_cover, …)
    //Registry rows being edited, held steady while a field is live (see RegTree), keyed per layer.
    std::map<std::string, std::vector<PkgGraph::RegRow>> *RegBuf = nullptr;

    float S(float Units) const { return Units * Z; }
    //A field fills the row to the node's right padding (a negative width is "to the right edge, less this").
    float RightW() const { return -S(PadRight); }
    float PadRight = 9.0f;                                              // units kept clear at the right edge
};

//Properties + layers. `Layer` indexes are stable keys for folds and edit buffers.
void Body(nlohmann::ordered_json &Node, Host &H);

//The text cut to fit Width pixels at the current font, at a UTF-8 boundary, with "…" when cut.
std::string FitWidth(const std::string &Text, float Width);

}

#endif // PKGFORM_H
