#ifndef FOLD_H
#define FOLD_H

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Fold — resolution of generation-6 nodes (MetaPackageFormat gen 6, §4). A node is {LABEL, VARIANT?, RECOMMENDED?,
// LAYERS}; each layer is one typed entry (ZIP FILE DELTA DIR NODE EDIT REG VARS ENV DLL EXEC KEEP ANY NOT) that may
// carry WHEN. A NODE layer CONTAINS another node — its ancestors are layers — selected by TAKE and placed by
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

//The nodes a resolve may reach: handle (CID) -> the node's JSON and the directory its content files live in.
struct Library
{
    struct Entry { json Node; std::string Dir; };
    std::unordered_map<std::string, Entry> Nodes;
    const Entry *Find(const std::string &Cid) const
    { auto It = Nodes.find(Cid); return It == Nodes.end() ? nullptr : &It->second; }
};

//One applied content or EDIT layer, in fold order (bottom -> top).
struct Item
{
    std::string Kind;            // ZIP FILE DELTA DIR, or EDIT
    std::string Payload;         // content: the file name (or a %runtime% dir) — empty for EDIT
    json        Source;          // content: SOURCE (a CID) or null
    json        Submounts;       // content: SUBMOUNTS or null
    json        Take;            // content: the TAKE view it is seen through (null = whole)
    json        Ops;             // EDIT: the ordered ops
    std::string Dir;             // the containing node's bundle dir
    std::string Target;          // the placed target (FILES namespace stripped; guest coordinates)
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
    std::vector<std::pair<std::string, std::string>> Events;            // move/held/cycle/missing/any-unmet/not-hit
    std::map<std::string, std::string> WhenVars;                       // what phase 1 resolved (layer WHEN reads these)
    std::string Error;                                                  // a malformed layer: nothing is launched from it
};

using Vars = std::map<std::string, std::string>;

Plan Resolve(const Library &Lib, const std::string &Root, const Vars &Instance = {}, const Vars &Builtins = {},
             const std::vector<std::string> &Grafts = {});

//Today's variable semantics over folded declarations ({KEY: {DEFAULT, UI, WHEN, COMMENT}}, in fold order): the
//instance value, else the DEFAULT; a declaration's WHEN gates its VALUE to "" (inside the fixpoint); every value is
//substituted against the built-ins + every other variable until stable (16 passes at most).
Vars ResolveVars(const json &Decls, const Vars &Builtins, const Vars &Instance);

//Guest coordinates -> a runner's layout through its GUEST_ROOTS ({"C:": "%PrefixRoot%/drive_c", …}); drives are
//case-insensitive, the longest anchor wins; a path under no mapped anchor is returned unchanged.
std::string ToLayout(const std::string &Path, const json &GuestRoots);

//The layer's one type key, or "" when it has none or several (a malformed layer).
std::string TypeOf(const json &Layer);

// ----- grafts (§4.4): a graft is a node whose list begins with ANY; it is offered to a row whose resolution
// contains a member of that ANY, and pre-ticked when RECOMMENDED lists the row's tile.
using GraftIndex = std::unordered_map<std::string, std::vector<std::string>>;   // member -> grafts
GraftIndex BuildGraftIndex(const Library &Lib);
struct Offer { std::vector<std::string> Offered, Ticked; };                    // default order: LABEL, then CID
Offer OfferedGrafts(const Library &Lib, const GraftIndex &Idx, const Plan &P, const std::string &FaceUid);

//The plan as JSON, in the shape tools/gen6/resolve.py's plan_json() emits — what the gate compares.
json PlanToJson(const Plan &P);

} // namespace Fold

#endif // FOLD_H
