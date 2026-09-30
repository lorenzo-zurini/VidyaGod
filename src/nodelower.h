#ifndef NODELOWER_H
#define NODELOWER_H

#include <nlohmann/json.hpp>
#include <functional>
#include <string>

namespace Fold { struct Plan; }

// ---------------------------------------------------------------------------
// NodeLower — the schema front-end (MetaPackageFormat generation 6).
//
// On disk a node is {CID, LABEL, VARIANT?, RECOMMENDED?, LAYERS}; each layer has exactly one type key (ZIP FILE
// DELTA DIR NODE EDIT REG VARS ENV DLL EXEC KEEP ANY NOT) and may carry WHEN. The launch engine consumes its own
// internal vocabulary of layer ops ("VFSZipLayer", "FileEdit", "BinaryPatch", "RegEdit", "DllOverride", "CustomVar",
// "DeclarePersist") — never written to disk. This file is the one bridge:
//   * CheckNode — the vocabulary: one type key per layer, the fields each type allows, well-typed values;
//   * LowerNode — a node's OWN layers as engine ops, where they land relative to the node (content bookkeeping);
//   * LowerPlan — a RESOLVED plan (Fold::Resolve) as engine ops, placed, and mapped from guest coordinates into the
//     runner's layout (its GUEST_ROOTS): what a launch mounts and applies;
//   * LowerEntry — one EXEC entry as the exec block the engine runs.
// ---------------------------------------------------------------------------

namespace NodeLower
{

//"" when the node is well-formed, else why not (naming the layer). A malformed node is indexed with the reason and
//never routed through.
std::string CheckNode(const nlohmann::ordered_json &J, const std::string &NodeId, const std::string &Label = std::string());

//The node's own layers as engine ops (a node that fails CheckNode lowers to nothing). Targets stay as authored
//(guest coordinates, relative to the node); EXEC, ENV, NODE, ANY, NOT lower to nothing — they are folded facts.
nlohmann::ordered_json LowerNode(const nlohmann::ordered_json &J, const std::string &NodeId);

//An EXEC entry → the engine's exec block. A launchable entry lowers to {PLATFORM, CONTENTPATH, EXEARGS, WORKDIR,
//LABEL}; a runner entry (non-empty GUEST) to {HOST, GUEST, EXECUTABLE, ARGS, CONTENT_ROOT, PREFIX_GENERATE,
//UNIFIED_RUNTIME, GUEST_ROOTS, DRIVES, LABEL}. Paths stay as the package wrote them (by anchor).
nlohmann::ordered_json LowerEntry(const nlohmann::ordered_json &Entry);

//A resolved plan → the engine's ordered ops: content (PATH absolute from the node's bundle dir; TARGET and SUBMOUNT
//destinations as the package addresses them — by anchor; the launch maps them into the runner's layout once
//substituted, LaunchResolver::GuestToLayout), EDIT ops as FileEdit/BinaryPatch in fold order, the folded registry as
//RegEdit per key, DllOverride, CustomVar per declaration (fold order), and KEEP as DeclarePersist. A FileEdit/RegEdit
//whose address lies inside a user-owned (KEEP) subtree but is taken back by a more specific KEEP false is marked
//OVERRIDE: it is re-applied after the user's state is restored — the package owns it.
nlohmann::ordered_json LowerPlan(const Fold::Plan &P);

//Is Address (a FILES/… or REG/… address, guest coordinates) owned by the user under this KEEP fold? The most
//specific KEEP entry covering it decides; nothing covering it ⇒ package-owned.
bool UserOwned(const nlohmann::ordered_json &Keep, const std::string &Address);

} // namespace NodeLower

#endif // NODELOWER_H
