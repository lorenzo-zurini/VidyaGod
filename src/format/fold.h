#ifndef FOLD_H
#define FOLD_H

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Fold — resolution of generation-6 nodes (MetaPackageFormat gen 6, §4). A node is {LABEL, VARIANT?, RECOMMENDED?,
// LAYERS}; each layer is one typed entry (ZIP FILE DELTA DIR NODE EDIT REG VARS ENV DLL EXEC KEEP INTO) that may
// carry WHEN — over variables and over node CIDs (a named node holds when it is in the composition). A NODE layer CONTAINS another node — its ancestors are layers — selected by TAKE and placed by
// TARGET. Resolving a root:
//   1. phase 1 — the variables a layer WHEN may read: instance values, built-ins, and the defaults of ungated VARS
//      layers reached through ungated NODE layers;
//   2. expansion — walk LAYERS, expanding NODE layers in place. An OCCURRENCE is (node, take, target); reached twice
//      it is one occurrence at its first position (a diamond), except that a later mention in the SAME list moves
//      it there. A node never leaves the node that contains it;
//   3. the fold — per address, the latest writer wins; null deletes.
// The reference definition is tools/gen6/resolve.py; tools/gen6/fold_gate.py holds this port to it plan-for-plan.
// ---------------------------------------------------------------------------

namespace Fold
{

using json = nlohmann::ordered_json;

//The nodes a resolve may reach: handle (CID) -> the node's JSON and the directory its content files live in. The
//JSON is borrowed (a node index owns it); a Library is cheap to assemble per call.
struct Library
{
    struct Entry { const json *Node = nullptr; std::string Dir; };
    std::unordered_map<std::string, Entry> Nodes;
    const Entry *Find(const std::string &Cid) const
    { auto It = Nodes.find(Cid); return It == Nodes.end() ? nullptr : &It->second; }
};

//One applied content or EDIT layer, in fold order (bottom -> top).
struct Item
{
    std::string Kind;            // ZIP FILE DELTA CODED DIR, or EDIT
    std::string Payload;         // content: the file name (or a %runtime% dir) — empty for EDIT
    json        Source;          // content: SOURCE (a CID) or null
    json        Size;            // content: SIZE (bytes, stamped at publish) or null
    json        View;            // content: the [TAKE, TARGET] steps it reaches the root through, innermost first,
                                 // when any of them takes (null = seen whole: Target places it)
    json        Ops;             // EDIT: the ordered ops
    std::string Dir;             // the containing node's bundle dir
    std::string Target;          // the placed target (FILES namespace stripped; guest coordinates)
    std::string Own;             // content: the layer's own target in its node (unplaced) — what TAKE addresses
    json        BaseSources;     // DELTA / CODED: BASE_SOURCES as written, the SOURCE CIDs of its bases in order
    std::string Description;     // CODED: the SOURCE CID of the layer holding its format's description
    std::string Name;            // CODED: the file name the shipped stream appears by, in Target
    std::string From;            // the node it came from
    int         At = 0;          // its index in that node's LAYERS
};

struct RegValue { json Arch; std::string Path, Name; json Value; };

struct Plan
{
    std::vector<Item> Seq;
    std::map<std::string, RegValue> Reg;                                // key: arch|path-lower|name-lower
    std::map<std::string, std::pair<json, std::string>> RegKeys;       // key: arch|path-lower -> (arch, path)
    json Dll = json::object(), Env = json::object(), Exec = json::object(), Keep = json::object(), Decls = json::object();
    std::vector<std::string> Order;                                     // occurrences' nodes, in expansion order
    std::vector<std::string> Parts;                                     // the parts applied onto the end (INTO), in order
    std::vector<std::pair<std::string, std::string>> Events;            // move/held/cycle/missing/into-unmet/into-off
    std::map<std::string, std::string> WhenVars;                       // what phase 1 resolved (layer WHEN reads these)
    std::string Error;                                                  // a malformed layer: nothing is launched from it
    //Every layer the fold applies, in order — each occurrence's (WHEN-true, not moved away), a contained node's where
    //its NODE layer is (Depth one deeper); View as Item's. A NODE layer naming a node the library does not hold is a
    //step too (Missing: Ref names it), not entered. What an editor shows as "the fold".
    struct Step { std::string From; int At = 0; std::string Kind; int Depth = 0; json View; std::string Ref; };
    std::vector<Step> Steps;
};

using Vars = std::map<std::string, std::string>;

//Grafts: the parts applied onto the end, in order (ApplyGrafts). Selection: what a WHEN naming a node CID reads as
//selected (default: Grafts) — parts are judged on the user's selection, so parts that exclude each other cannot loop.
Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance = {}, const Vars &Builtins = {},
             const std::vector<std::string> &Grafts = {}, const std::vector<std::string> *Selection = nullptr);

//The held and moved mentions (§4.2) whose position decides a winner: a node mentioned again after it already occurred
//is HELD at its first occurrence, or MOVED to the later mention when both sit in the same list. For each such event of
//P (Resolve's plan for these arguments), the plan with that one decision taken the other way is compared: a changed
//fact, or two layers whose targets overlap folding in the other order, means the position chose a winner — a WARN,
//since the author meant one of the two. Returns those events ({"held"|"move", cid}), in order. One resolve per event.
std::vector<std::pair<std::string, std::string>> DecidingMentions(const Library &Lib, const std::string &Root,
                                                                  const Vars &Instance, const Vars &Builtins,
                                                                  const std::vector<std::string> &Grafts, const Plan &P);

//Resolve with only the EXEC fold built (the same expansion, occurrences and phase 1 — so the same entries — without
//collecting or folding anything else). For facts derived over a whole library: a node's entries, runner, tiles.
Plan ResolveEntries(const Library &Lib, const std::string &Root);

//Today's variable semantics over folded declarations ({KEY: {DEFAULT, UI, WHEN, COMMENT}}, in fold order): the
//instance value, else the DEFAULT; a declaration's WHEN gates its VALUE to "" (inside the fixpoint); every value is
//substituted against the built-ins + every other variable until stable (16 passes at most).
Vars ResolveVars(const json &Decls, const Vars &Builtins, const Vars &Instance);

//A guest path -> a runner's layout through its DRIVES ({"C:": "%PrefixRoot%/drive_c", …}); drives are
//case-insensitive, the longest match wins; a path under no mapped drive is returned unchanged.
std::string ToLayout(const std::string &Path, const json &Drives);

//The reverse, for what a run wrote (a captured registry value "C:\\Program Files (x86)\\X\\x.ax"): every guest path
//inside Value that starts at a drive and lies under an anchor's guest path (Anchors: %Anchor% -> that path, as the
//runner resolves it) is re-spelled from the MOST specific such anchor ("%ProgramFiles32%\\X\\x.ax"). A guest path under
//no anchor is left as written. Case-insensitive; '\\' and '/' are the same separator.
std::string ToAnchors(const std::string &Value, const std::map<std::string, std::string> &Anchors);

//A relative path placed under a NODE layer's TARGET (anchored paths — a drive, a %Anchor% — stay where they are).
std::string PlaceUnder(const std::string &Path, const std::string &Prefix);
//What one TAKE keeps of an address, left-stripped and renamed; nullopt = not taken.
std::optional<std::string> TakeView(const json &Take, const std::string &Addr);
//A FILES address of an occurrence through its whole view (Item::View: each step's TAKE, then its placement): where the
//root sees it, or nothing when a TAKE leaves it out.
std::optional<std::string> ThroughView(const json &View, std::string Addr);

//The layer's one type key, or "" when it has none or several (a malformed layer).
std::string TypeOf(const json &Layer);
//A part's INTO layers: the run of INTO layers its list begins with (each one a list of alternatives; the part goes into
//all of them). Empty for a node that is not a part. Only layers whose INTO is a list count.
std::vector<const json *> IntoLayers(const json &Layers);

// ----- parts (MetaPackageFormat 12 §12.5, 23.0): a part (a "graft") is a node whose list begins with INTO [ends]: it goes
// into an END (a node with VARIANT) or into a part applied to it — a HARD LINK, never "somewhere beneath". Parts have
// no order between them (one would need knowledge of every mod): they are applied after what they go into, siblings by
// CID, and merge.
using GraftIndex = std::unordered_map<std::string, std::vector<std::string>>;   // INTO member -> parts
GraftIndex BuildGraftIndex(const Library &Lib);
struct Offer { std::vector<std::string> Offered, Ticked; };                    // shown after what each goes into, then LABEL, CID
Offer OfferedGrafts(const Library &Lib, const GraftIndex &Idx, const Plan &P, const std::string &FaceUid);

//How many of a resolution's requirements fail: an INTO with nothing it goes into beneath it.
int Unsatisfied(const Plan &P);

//The parts a launch applies, in order. Requested = the instance's selection (a SET: its order is ignored); applied in
//rounds, each round the selected parts offered with the ones before applied (so a part follows what it goes into),
//siblings by CID. A part whose INTO's WHEN does not hold on the selection, or that leaves a requirement unmet, is
//dropped (listed in Dropped), as is one never offered here. No list = a fresh instance: the parts RECOMMENDED under
//the row's tile — or, with EveryOffered, every offered part — in rounds.
std::vector<std::string> ApplyGrafts(const Library &Lib, const GraftIndex &Idx, const std::string &Root, const Vars &Instance,
                                     const Vars &Builtins, const std::string &FaceUid,
                                     const std::vector<std::string> *Requested, std::vector<std::string> *Dropped = nullptr,
                                     bool EveryOffered = false);

//The plan as JSON, in the shape tools/gen6/resolve.py's plan_json() emits — what the gate compares.
json PlanToJson(const Plan &P);

} // namespace Fold

#endif // FOLD_H
