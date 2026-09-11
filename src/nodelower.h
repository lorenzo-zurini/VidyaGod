#ifndef NODELOWER_H
#define NODELOWER_H

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// NodeLower — the schema front-end.
//
// On disk a node is ONE layer of one TYPE: identity (NODE_ID/PARENTS/TOGGLE/EXCLUDE/WHEN) plus a
// type-specific payload hoisted flat. Plural payloads batch WITHIN a type (a RegEdit's EDITS[], a
// BinaryPatch's EDITS[], a DllOverride's OVERRIDES{}), never across types — so node granularity is
// decided by TYPE, not by taste, and ordering between nodes is carried by PARENTS edges instead of
// by an array index that asserted relationships nobody meant.
//
// The launch engine consumes an ORDERED LAYER SEQUENCE — which is what a resolved closure collapses
// to anyway (ResolveNodeOrder emits parents-before-children; the executor concatenates each node's
// contribution). So this is a pure front-end: `Lower` expands one flat node's payload into that
// sequence. Nothing downstream of ParseNode changes shape.
//
// Batch order is meaningful for BinaryPatch and FileEdit (a zero-the-region patch must precede the
// stubs written into it) and irrelevant for RegEdit/DllOverride/Persist; expansion preserves array
// order in every case, so the distinction costs nothing here.
// ---------------------------------------------------------------------------

namespace NodeLower
{

//Expand one flat node object into the executor's ordered layer array.
//`Err` is set (and an empty array returned) if the payload is malformed — the caller logs and skips
//the node rather than launching something half-understood.
nlohmann::ordered_json Lower(const nlohmann::ordered_json &J, const std::string &NodeId, std::string &Err);

} // namespace NodeLower

#endif // NODELOWER_H
