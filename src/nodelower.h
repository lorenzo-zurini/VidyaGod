#ifndef NODELOWER_H
#define NODELOWER_H

#include <nlohmann/json.hpp>
#include <string>

// ---------------------------------------------------------------------------
// NodeLower — the schema front-end.
//
// On disk a node is facets (CID/LABEL/WHEN/TOGGLE/PUBLISH) + TILE + ENTRYPOINTS + any subset of the payload
// arrays (LAYERS PATCHES FILEEDITS REGEDITS DLLOVERRIDES VARS PERSISTS) + the one edge OVER. One node is one
// meaningful change, which may span kinds.
//
// The launch engine consumes an ORDERED LAYER SEQUENCE — which is what a resolved closure collapses to anyway
// (ResolveNodeOrder emits requirements-before-dependants; the executor concatenates each node's contribution and
// applies each KIND in its own engine phase: VFS mount → binary patch → file edit → registry). So this is a pure
// front-end: `Lower` expands one node's payload arrays into that sequence, in payload-kind order and array order
// within a kind. Nothing downstream of ParseNode changes shape — the layer TYPE tags it emits ("VFSZipLayer",
// "RegEdit", …) are the engine's internal vocabulary, never written to disk.
//
// TILE and ENTRYPOINTS are FACETS, not payload: they never lower into layers (ParseNode reads them directly).
// `LowerEntrypoint` turns one ENTRYPOINTS entry into the exec block the engine consumes.
// ---------------------------------------------------------------------------

namespace NodeLower
{

//Expand one node object's payload arrays into the executor's ordered layer array. Refuses (Err set, empty array
//returned) a malformed payload, an empty payload array, or a top-level key outside the format's vocabulary — the
//caller indexes the node with the reason attached rather than launching something half-understood.
nlohmann::ordered_json Lower(const nlohmann::ordered_json &J, const std::string &NodeId, std::string &Err);

//One ENTRYPOINTS entry → the engine's exec block. A launchable entry ({HOST, PATH, ARGS, ENV, ENV_REMOVE, WORKDIR,
//LABEL, RECOMMENDED, RUNNER}) lowers to {PLATFORM, CONTENTPATH, EXEARGS, ENV, REMOVE_ENV, WORKDIR, LABEL,
//RECOMMENDED, RUNNER}; a runner entry (non-empty GUEST) to {HOST, GUEST, EXECUTABLE, ARGS, ENV, REMOVE_ENV,
//CONTENT_ROOT, PREFIX_GENERATE, UNIFIED_RUNTIME}. Well-typed by construction (Err set + null on a mismatch).
nlohmann::ordered_json LowerEntrypoint(const nlohmann::ordered_json &Entry, const std::string &NodeId, std::string &Err);

} // namespace NodeLower

#endif // NODELOWER_H
