#ifndef VALIDATE_H
#define VALIDATE_H

#include "manifestmodel.h"   // NodeIndex

#include <filesystem>
#include <set>
#include <string>
#include <vector>

// Validate — what a library holds that would not work: the format's rules, each launchable as a launch would meet it,
// the shelf. Read-only, but for FixCaseConflicts, which rewrites the zips it is asked to.
namespace Validate
{

// Validate the node graph (generation 6): refs resolve, no cycles, one type key per layer, well-typed payloads,
// STORE zips, BinaryPatch shape, WHEN grammar and the phase rule, undefined %KEY%s, KEEP lint, a VARIANT presents a
// tiled entry, RECOMMENDED names a presented tile, and — per variant, over its resolved plan — no EDIT beneath a
// layer that replaces its file, and every DELTA sits on content. OnlyNodes scopes it (the rest of Idx is consulted).
void Nodes(const NodeIndex &Idx, std::vector<std::string> &Errors, std::vector<std::string> &Warnings,
           const std::set<std::string> *OnlyNodes = nullptr);

// The shelf's view of the tiles, which no single node shows: a tile no variant presents (its card has no row to
// launch; a graft's card is exempt, its rows are the versions it applies onto), and a tile whose presentation — its
// recommended variant's folded tile, docs/09 §9.3 — has no TITLE (the card is named by a CID). OnlyNodes: only tiles
// one of those nodes presents. Part of Nodes' warnings, and of --audit-packages.
std::vector<std::string> Shelf(const NodeIndex &Idx, const std::set<std::string> *OnlyNodes = nullptr);

// Resolves cross-layer case conflicts (the FindCrossLayerCaseCollisions errors) by canonicalizing the
// higher-priority layers' zip entries to the base layer's case (unpack→rename→repackage STORE, same
// structure; the base zip is never touched). Canonicalization always uses the full index (correct base-case
// resolution); ScopeDir, if given, restricts which zips are actually rewritten to those under that directory
// (so the Package Editor button fixes only the open bundle). Returns the number of zips rewritten; Log gets a
// human report. Used by --fix-case-conflicts and the Package Editor "Fix case conflicts" action.
int FixCaseConflicts(const NodeIndex &Idx, std::vector<std::string> &Log,
                     const std::filesystem::path *ScopeDir = nullptr);

} // namespace Validate

#endif // VALIDATE_H
