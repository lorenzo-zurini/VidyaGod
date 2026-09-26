#ifndef PKGGRAPH_H
#define PKGGRAPH_H

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PkgGraph — the package as a graph, plus the per-LAYER-TYPE payload schema the editor renders (generation 6). A
// node is {LABEL, VARIANT?, RECOMMENDED?, LAYERS}: an ordered list of typed layers (ZIP DIR FILE DELTA NODE EDIT REG
// VARS ENV DLL EXEC KEEP ANY NOT), each with an optional WHEN. The node's parents are its NODE layers — they are
// the wires — and ANY/NOT layers are wires too (a requirement, an exclusion). "Type" below is the node's derived
// KIND (its first layer that is not a reference) — a display word, never stored.
//
// Pure data + description, NO UI and NO filesystem: the canvas renders a Graph and edits the SAME
// `{"NODES":[…]}` document the model persists one-file-per-node, so the graph IS the package — there is no
// separate representation to keep in sync.
//
// POSITION is split in two, because it answers two different questions. A node's own `POS` is the AUTHOR'S
// default — computed by PkgLayout and stamped at publish time — so a package opens laid out on the machine of
// someone who has never seen it. Where THIS machine has since dragged a node is a local preference and lives in
// GlobalConfig, never in the package: a Meta-CID is minted add-by-reference IN PLACE over the author's node
// files, so writing a drag back into a node would put every mouse movement into the package's bytes and change
// the CID for every peer. Build applies POS first and the local override second.
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
    std::string Id;             // the wiring handle (stored CID)
    std::string Type;           // the node's KIND: its first layer type that is not a reference, "" when plain
    std::string Form;           // its first content layer's type, lower case ("zip"/"dir"/"file"/"delta"; "" without
                                // content) — what a delta can be based on
    bool        HasDeltaBase = false;   // a zip whose DeltaBase exists: "-> delta" is offered
    float       X = 0, Y = 0;   // canvas position: node POS, then this machine's override, then computed
    bool        HasPos = false; // false → nothing declared one, so PkgLayout placed it
    //Estimated DRAWN height, in the same units as X/Y. A node's box is as tall as its payload makes it — a
    //RegEdit with 59 registry rows draws 59 rows — so a layout that steps by a constant runs tall nodes
    //straight through the ones beneath them. Estimated rather than measured because the layout has to be
    //PURE: it is stamped into POS at publish time, headless, where no node has ever been rendered.
    float       Height = 0.0f;
};

// One reference layer drawn as a wire: a NODE layer (the child contains the parent), one member of an ANY layer (the
// child requires one of them), or a NOT layer (the child excludes it). `ParentIndex` >= 0 is an in-bundle node; -1
// means the ref names a node in another bundle and is drawn as a reference chip rather than a full box.
struct Link
{
    int         ChildIndex  = 0;
    int         ParentIndex = -1;
    std::string ExternalId;     // set when ParentIndex < 0
    //The ordinal of this ref among the child's references FLATTENED in LAYERS order (an ANY's members count one
    //each). Carried rather than recovered: the renderer needs it every frame to key the wire. EraseRef(child,
    //Slot) walks the same flattening, so a detach addresses exactly this ref.
    int         Slot        = 0;
    int         Layer       = 0;    // the LAYERS entry this ref lives in
    int         Member      = -1;   // -1: a NODE or NOT layer; >= 0: this member of an ANY layer
    bool        Any         = false;// an ANY member (a requirement, not containment)
    bool        Not         = false;// a NOT layer (an exclusion)
};

//A declared position that was refused because no layout could have produced it. Carried rather than logged:
//Build runs on every cache rebuild — every keystroke — and the canvas is where the "said it once" state lives.
//`Source` names WHICH of the two declarations was bad, because they are fixed in different places: the
//package's own POS by editing the package, this machine's override by clearing a local setting.
struct RejectedPosition
{
    //The INDEX as well as the id, because an id is not a key. A hand-edited bundle can carry two nodes named
    //the same, or a node with no NODE_ID at all — and every such node collapses onto one id, so a consumer
    //asking "was THIS node's file rewritten?" by id answers for a different node. The index addresses exactly
    //one entry of NODES by construction.
    int         Index = -1;
    std::string NodeId;
    std::string Source;
    std::string Value;
};

struct Graph
{
    std::vector<Node> Nodes;
    std::vector<RejectedPosition> RejectedPositions;
    std::vector<Link> Links;
    std::vector<std::string> Externals;   // distinct out-of-bundle parent ids, in first-seen order
};

//Read the document into a Graph. `Layout` (optional) is THIS MACHINE's position override — an object keyed by
//NODE_ID whose values are [x, y], held in GlobalConfig — and it wins over a node's own POS. Anything neither
//declares is placed by PkgLayout, so a bundle always opens readable instead of stacked at the origin.
//
Graph Build(const nlohmann::ordered_json &NodesArray, const nlohmann::ordered_json *Layout = nullptr);

//Write a node's position into THIS MACHINE's override object (GlobalConfig, never the package). Returns false
//if the id is empty.
bool SetPos(nlohmann::ordered_json &Layout, const std::string &NodeId, float X, float Y);

//Pin ids, derived (never stored) — imnodes needs ints: in = idx*4, out = idx*4+1.
inline int InPin (int Index) { return Index * 4;     }
inline int OutPin(int Index) { return Index * 4 + 1; }
inline int PinNode(int Pin)  { return Pin / 4;       }
inline bool PinIsIn(int Pin) { return (Pin % 4) == 0; }

// ---- layer vocabulary -----------------------------------------------------

//Every LAYER TYPE, in palette order (content, references, transforms, facts, entries).
const std::vector<std::string> &AllTypes();
//A layer's one type key ("" when it has none or several — a malformed layer).
std::string LayerType(const nlohmann::ordered_json &Layer);
//The layer types THIS node carries, in LAYERS order.
std::vector<std::string> LayerTypes(const nlohmann::ordered_json &Node);
//The node's KIND: its first layer that is not a reference (NODE/ANY/NOT), else "NODE" for a node made only of
//references, else "" — the display word and the accent colour key.
std::string KindOf(const nlohmann::ordered_json &Node);
//One line for the palette and the layer tooltip — discoverability in the format, not in a manual.
const char *TypeHelp(const std::string &Type);
//Accent colour (r,g,b) by kind — content, transforms, facts, entries and composition read differently at a glance.
void TypeColour(const std::string &Kind, int &R, int &G, int &B);
//A node's CONTENT layer — its first ZIP, FILE, DELTA or DIR layer, the one the per-node content actions work on —
//as an index into LAYERS (-1 without one), its type, its file name (the payload) and its TARGET.
int ContentIndex(const nlohmann::ordered_json &Node);
std::string ContentType(const nlohmann::ordered_json &Node);
std::string ContentName(const nlohmann::ordered_json &Node);
std::string ContentTarget(const nlohmann::ordered_json &Node);
//The file a node's content can be diffed against, "" when there is none. In the fold a delta's base is the nearest
//content at its own target, so it is a node this one contains EARLIER in its LAYERS — whole (no TAKE, no TARGET:
//either moves or narrows what lands) — whose content layer is a ZIP at the SAME target. Nearest first. The "-> delta"
//button (ActionsFor) and the action itself both ask this, so the button never offers what the action refuses.
std::string DeltaBase(const nlohmann::ordered_json &NodesArray, int Index);
//A starter layer of a type (valid by construction: required keys present).
nlohmann::ordered_json NewLayer(const std::string &Type);
//A fresh node carrying ONE starter layer ("" ⇒ a node with no layers yet).
nlohmann::ordered_json NewPayload(const std::string &Type);
//Append a starter layer of a type to a node. Returns whether it changed.
bool AddLayer(nlohmann::ordered_json &Node, const std::string &Type);
//Move layer I by Delta (-1 up, +1 down) — the order IS the fold order. Returns whether it moved.
bool MoveLayer(nlohmann::ordered_json &Node, int I, int Delta);
//Erase the reference at flattened ordinal `Slot` (the Link::Slot of its wire): a NODE or NOT layer is removed, an
//ANY member is removed (an ANY left with none is removed). False if none.
bool EraseRef(nlohmann::ordered_json &Node, int Slot);
//Contain `Ref`: a NODE layer, after the node's leading NODE layers (what it contains lies beneath its own layers).
//No-op if a NODE layer already names it. Returns whether it changed.
bool AddNodeRef(nlohmann::ordered_json &Node, const std::string &Ref);
//Remove every reference naming `Ref` (NODE layers, ANY members, NOT layers). Returns whether anything changed.
bool RemoveRefs(nlohmann::ordered_json &Node, const std::string &Ref);

// ---- payload schema -------------------------------------------------------

enum class FieldKind
{
    Text,        // a string
    Enum,        // a string with a fixed option list
    Check,       // a bool
    StringList,  // an array of strings, one per line; blank lines are typing noise and are dropped
    // ...except here: an array of strings where an EMPTY entry is MEANINGFUL, so a blank line is data, not noise.
    // A delta's BASE_TARGETS is the only such field: "" is the mount root, a perfectly ordinary base. Under the
    // plain StringList writer that entry could not be typed (blank line dropped → list empty → key erased) and,
    // worse, an existing ["", "dxvk"] lost its first element the moment anyone touched the box — turning a
    // two-base delta into a one-base one, which is a base of the wrong SIZE and a layer the mount silently skips.
    StringListKeepEmpty,
    KeyValue,    // an object of string→string (a null value is shown as a removal)
    ObjArray,    // an array of objects, each described by `Sub`
    RegTree,     // a REG layer's hive tree, edited as flattened key paths
    Arch,        // a REG layer's ARCH: the 32/64-bit views, as checkboxes
    TakeList,    // a NODE layer's TAKE: selections, one per line ("FILES/a/b", "FILES/dir/" for contents, "X => Y" renames)
    VarMap,      // a VARS layer: {KEY: declaration described by `Sub`, plus its launcher UI facet}
    KeepMap,     // a KEEP layer: {ADDRESS: true | false | {NAME, CLOUD}}
    Cover,       // a COVER {FILE, SOURCE, SIZE}
    Object,      // ONE optional nested object described by `Sub` (an entry's TILE)
};

struct Field
{
    const char *Key   = "";
    const char *Label = "";
    FieldKind   Kind  = FieldKind::Text;
    const char *Hint  = "";
    std::vector<std::pair<const char *, const char *>> Options;   // Enum: {stored value, shown label}
    std::vector<Field> Sub;                                       // ObjArray: the per-entry fields
    bool VarUI = false;                                           // ObjArray of CustomVars: draw the UI facet per entry
};

//The rows to render for ONE layer of a type, in order, drawn against the layer object (WHEN and COMMENT included).
const std::vector<Field> &FieldsFor(const std::string &Type);

//Toggle a CustomVar's launch-dialog visibility, which the format expresses as the PRESENCE of the UI facet
//(08-variables.md): visible adds a minimal UI object (keeping any existing one), hidden removes it so the var
//resolves from DEFAULT and never renders. Pure — the editor's Visible checkbox and tests both call it.
void SetVarVisible(nlohmann::ordered_json &node, bool visible);

//How tall this node will be drawn, in canvas units. Derived from the SAME declared field table the canvas
//renders from, so it tracks a schema change instead of drifting away from one, and from the payload's own
//sizes (registry rows, patch entries, list lines) because that is what actually makes a node tall.
//
//It is an estimate and is allowed to be generous: the cost of over-estimating is a little white space, the
//cost of under-estimating is two nodes drawn on top of each other. It is pinned to the real renderer by
//theEstimatedNodeHeightMatchesTheDrawnOne, which measures every SECTION and fails if the estimate falls short.
float EstimateHeight(const nlohmann::ordered_json &Node);

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

//Flatten a hive tree object ({HKLM: {...}, HKCU: {...}}) into rows (the tree is the on-disk shape; rows are what a
//human edits).
std::vector<RegRow> RegRowsOf(const nlohmann::ordered_json &Entry);
//How many rows RegRowsOf WOULD produce, without producing them. The height estimate needs the count on every
//graph rebuild and building the rows for it was 40% of that rebuild; the two are pinned against each other by
//the_registry_row_count_matches_the_flattening, because a drift between them silently under-reserves height.
size_t CountRegRows(const nlohmann::ordered_json &Entry);

//A NODE_ID rendered safe to put in a log line — C0 and C1 controls replaced, length capped on a UTF-8
//character boundary. Ids come from arbitrary on-disk or peer JSON and the log is this codebase's verdict
//channel.
std::string SafeId(const std::string &Id);

//Whether a StringList field's value can be edited as a list AT ALL, and if not, what is wrong with it.
//
//ONE predicate, because the renderer and the height estimator both need this answer and they must never
//disagree: drawField draws one line for a value it refuses, FieldPx has to charge one line for it, and that
//number is STAMPED into the package at publish. When the element scan lived only in drawField, a list holding
//six strings and one number was drawn 437px and estimated 600px — a 163px hole in every peer's canvas.
//Pass nullptr for an absent field.
enum : int { kStringListOk = -1, kStringListNotAList = -2 };
int StringListFault(const nlohmann::ordered_json *V);

//What a value IS, in a handful of characters — for the places that have to SHOW the author a payload of the
//wrong shape. Never serialises the value: a package fetched from a content source can carry megabytes in any
//field, and both callers of this are on a per-FRAME path, so `dump()` there is a multi-megabyte allocation and
//a multi-megabyte CalcTextSize sixty times a second. A long string is cut to `MaxChars` on a character
//boundary; a container is described by its size instead of its contents. Output is SafeId-clean.
std::string DescribeValue(const nlohmann::ordered_json &V, size_t MaxChars = 24);

//Make Node[Key] safe to write an OBJECT into — materialising it when absent or null — and say whether that is
//now possible. And the ONE guarded writer every UI path uses for Node[Key][Sub] = Value.
//
//nlohmann's operator[](string) throws type_error.305 on a value that is not an object, and the editor exists
//to open packages that are wrong, including ones fetched from a peer: "COVER": 5 plus one keystroke used to
//terminate the application out of paintGL. A guard at each write site can be deleted at that site with nothing
//else noticing (a mutation proved it), so the guard and the write are one function, exported so the tests
//drive this function rather than a copy of the pattern.
bool WritableObject(nlohmann::ordered_json &Node, const char *Key);
bool WriteSubKey(nlohmann::ordered_json &Node, const char *Key, const std::string &Sub,
                 const nlohmann::ordered_json &Value);
//Rebuild a hive tree object from rows, preserving any non-hive member.
void RegRowsInto(nlohmann::ordered_json &Entry, const std::vector<RegRow> &Rows);

//A NODE layer's TAKE as text, one selection per line ("X => Y" for a rename pair), and back. TextToTake drops blank
//lines. A TAKE entry that is neither a string nor a [from, to] pair makes TakeFault report it (not editable here).
std::string TakeToText(const nlohmann::ordered_json &Take);
nlohmann::ordered_json TextToTake(const std::string &Text);
bool TakeFault(const nlohmann::ordered_json *Take);

} // namespace PkgGraph

#endif // PKGGRAPH_H
