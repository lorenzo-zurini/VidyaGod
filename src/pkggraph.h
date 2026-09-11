#ifndef PKGGRAPH_H
#define PKGGRAPH_H

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PkgGraph — the package as a graph, plus the per-TYPE payload schema the editor renders.
//
// Pure data + description, NO UI and NO filesystem: the canvas renders a Graph and edits the SAME
// `{"NODES":[…]}` document the model persists one-file-per-node, so the graph IS the package — there is no
// separate representation to keep in sync. Canvas POSITION is the one thing that is NOT in the document: it is
// presentation, and a Meta-CID is minted add-by-reference IN PLACE over the node files, so a position stored
// there would put every drag into the package's bytes. It lives in a sidecar at <bundle>/LAYOUT.vglayout —
// both publish paths take only *.json, and nothing mounts the bundle directory itself.
//
// The field tables below are why this is not nodesections.cpp: an editor row is DECLARED (key, label, kind,
// options, hint), not hand-built per type. A field with N legal values is an enum with N entries, so the
// two-value ARCHITECTURE and the four-value Content FORM stop being free-text QLineEdits.
// ---------------------------------------------------------------------------

namespace PkgGraph
{

// ---- graph ----------------------------------------------------------------

struct Node
{
    int         Index = 0;      // position in doc()["NODES"] — the identity the canvas binds to
    std::string Id;             // NODE_ID
    std::string Type;           // TYPE ("Group" when payload-less)
    std::string Form;           // Content's FORM ("" for other types) — what a delta can be based on
    float       X = 0, Y = 0;   // canvas position, from the layout sidecar (never the node)
    bool        HasPos = false; // false → auto-layout placed it; first drag persists
};

// A PARENTS edge. `ParentIndex` >= 0 is an in-bundle node; -1 means the parent lives in another bundle and is
// drawn as a reference chip (MediaStack_MS, dgvoodoo, asiloader…) rather than a full box.
struct Link
{
    int         ChildIndex  = 0;
    int         ParentIndex = -1;
    std::string ExternalId;     // set when ParentIndex < 0
    //The exact index in the child's PARENTS array this edge came from. Carried rather than recovered: the
    //renderer needs it every frame to key the wire, and re-finding it meant a linear scan with a std::string
    //construction per comparison — on the 2775-node Minecraft bundle (39k links, 902 nodes with 75 parents
    //each) that is millions of allocations per frame, for a value Build already knew.
    int         Slot        = 0;
};

struct Graph
{
    std::vector<Node> Nodes;
    std::vector<Link> Links;
    std::vector<std::string> Externals;   // distinct out-of-bundle parent ids, in first-seen order
};

//Read the document into a Graph. `Layout` (optional) is the canvas-position SIDECAR: an object keyed by NODE_ID
//whose values are [x, y]. Nodes with no stored position are laid out (layered by dependency depth) so an
//un-positioned bundle opens readable instead of stacked at the origin.
//
//Position is NOT a node field. It is presentation, and a package is content-addressed: a Meta-CID is minted
//add-by-reference and IN PLACE over the author's own files, so anything living in a node file is in the CID.
//Storing layout there meant dragging a box on screen changed the package's bytes for every peer. The sidecar
//lives at <bundle>/LAYOUT.vglayout: both publish paths take only *.json by stated invariant, and nothing
//mounts the bundle directory itself, so it is invisible to publishing, seeding AND the runtime.
Graph Build(const nlohmann::ordered_json &NodesArray, const nlohmann::ordered_json *Layout = nullptr);

//Write a node's position into the layout sidecar. Returns false if the id is empty.
bool SetPos(nlohmann::ordered_json &Layout, const std::string &NodeId, float X, float Y);

//Pin ids, derived (never stored) — imnodes needs ints: in = idx*4, out = idx*4+1.
inline int InPin (int Index) { return Index * 4;     }
inline int OutPin(int Index) { return Index * 4 + 1; }
inline int PinNode(int Pin)  { return Pin / 4;       }
inline bool PinIsIn(int Pin) { return (Pin % 4) == 0; }

// ---- type vocabulary ------------------------------------------------------

//Every TYPE the format has. Order is palette order.
const std::vector<std::string> &AllTypes();
//One line for the palette and the node tooltip — discoverability in the type system, not in a manual.
const char *TypeHelp(const std::string &Type);
//Node accent colour (r,g,b) — content, transforms, identity and composition read differently at a glance.
void TypeColour(const std::string &Type, int &R, int &G, int &B);
//A fresh payload for a newly-created node of this type (valid by construction: required keys present).
nlohmann::ordered_json NewPayload(const std::string &Type);

// ---- payload schema -------------------------------------------------------

enum class FieldKind
{
    Text,        // a string
    Enum,        // a string with a fixed option list
    Check,       // a bool
    StringList,  // an array of strings, one per line
    KeyValue,    // an object of string→string
    ObjArray,    // an array of objects, each described by `Sub`
    RegEdits,    // RegEdit's EDITS[]: ARCHITECTURE + a hive tree, edited as flattened key paths
    Cover,       // DeclareLibraryItem's COVER {PATH, SOURCE}
};

struct Field
{
    const char *Key   = "";
    const char *Label = "";
    FieldKind   Kind  = FieldKind::Text;
    const char *Hint  = "";
    std::vector<std::pair<const char *, const char *>> Options;   // Enum: {stored value, shown label}
    std::vector<Field> Sub;                                       // ObjArray: the per-entry fields
};

//The rows to render for a node of this TYPE, in order. Empty for "Group" (pure composition).
const std::vector<Field> &FieldsFor(const std::string &Type);

// ---- node actions ---------------------------------------------------------

//An action offered ON a node. The canvas only renders these and reports the click; the HOST performs them
//(file dialogs, zip/delta conversion, capture sessions, launching) — the canvas does no IO and knows no engine.
struct Action
{
    const char *Id    = "";
    const char *Label = "";
    const char *Tip   = "";
    bool        Heavy = false;   // long-running: runs async, gets a progress bar + cancel, locks the node
};

//The actions VALID FOR THIS NODE RIGHT NOW — contextual, not a fixed per-type list: "flatten" only on a delta,
//"make delta" only when a parent can be its base, "re-store" only when the zip is actually DEFLATE. `Hints` are
//facts the canvas cannot know without touching disk (e.g. "deflate"), pushed in by the host.
std::vector<Action> ActionsFor(const nlohmann::ordered_json &Node, const Graph &G, int Index,
                               const std::vector<std::string> &Hints);

// ---- flattened registry editing -------------------------------------------

//One editable registry row: the full key path, the value name ("" = the key itself, no value), and the value.
struct RegRow
{
    std::string Path;    // "HKLM\\Software\\Ubi Soft\\TONICT"
    std::string Name;    // "Version", or "" for the key's DEFAULT value (what regedit shows as @)
    std::string Value;   // "1.00" / "dword:0000035c" / "hex:.."
    bool WasString = true;   // the on-disk JSON type, so a round trip does not retype a number into a string
    //"create this key, no values" — NOT the same as an empty NAME. The empty name is the key's default value,
    //and conflating the two silently destroyed every one of them on any edit: 48 live in LAVFilters alone,
    //carrying the whole DirectShow COM registration.
    bool KeyOnly = false;
};

//Flatten one EDITS entry's hive trees into rows (the tree is the on-disk shape; rows are what a human edits).
std::vector<RegRow> RegRowsOf(const nlohmann::ordered_json &Entry);
//Rebuild an entry's hive trees from rows, preserving ARCHITECTURE/OVERRIDE.
void RegRowsInto(nlohmann::ordered_json &Entry, const std::vector<RegRow> &Rows);

} // namespace PkgGraph

#endif // PKGGRAPH_H
