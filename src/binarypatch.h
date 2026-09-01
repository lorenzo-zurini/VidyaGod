#ifndef BINARYPATCH_H
#define BINARYPATCH_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "launchparams.h"   // ContainerParams

// BinaryPatch — the fourth edit layer (beside FileEdit / RegEdit / DllOverride). It mutates an executable in
// place, declaratively, over the PRISTINE file: the seeded/content-addressed exe is never touched; on-disk
// patches land only in the copy-on-write writelayer at launch. Three MODEs:
//   Replace : verify EXPECT bytes at a site, overwrite with REPLACE (NOP-padded if shorter). No-CD flag flips.
//   Poke    : write a scalar VALUE (usually a %token:u32le%) at a site. Config baked straight into the binary.
//   Cave    : splice a jmp trampoline — displace EXPECT (>=5 bytes) into a code cave, run PAYLOAD then the
//             displaced bytes, jmp back. Generalises tools/woxl_blitqueue_patch.py. Crash fixes / logic edits.
// A site is located by ANCHOR (hex signature with `??` wildcards, must be unique) or by a fixed OFFSET (a VA if
// >= the PE image base, else a raw file offset — PE section headers are parsed to convert). Every field is
// %token%-substituted through ContainerParams::GetVariablesMap() before use, so offsets/bytes/values can come
// from CustomVars. Patches are idempotent: an already-patched site is detected and skipped, a foreign site errors.
namespace BinaryPatch
{
enum class Result : uint8_t { Applied, Skipped, Error };

// Apply one BinaryPatch descriptor to an in-memory PE image buffer (the testable core; the on-disk launch path
// and the unit tests both drive this). Fields are already whole-JSON %token%-substituted by BuildSubComponentsArray,
// but Vars is passed for any residual field-level render (e.g. a Poke VALUE like "%woxl_res_w:u16le%"). On
// Result::Error, Msg explains why; on Applied/Skipped it is a short human note. Image is modified only on Applied.
Result ApplyOne(const nlohmann::ordered_json &Patch, std::vector<uint8_t> &Image,
                const std::map<std::string, std::string> &Vars, std::string &Msg);

// Process every BinaryPatch subcomponent with APPLY=="prefix" (the default): read the target file from the
// mounted runtime, ApplyOne, write it back (COW into the writelayer). APPLY=="memory" layers are skipped here —
// they are handled by vglobby at process-start. Returns false if any patch errored (loud, non-fatal — feeds the
// launch verdict, same contract as ProcessFileEdits).
[[nodiscard]] bool ProcessBinaryPatches(struct ContainerParams &ContainerParams);
}

#endif // BINARYPATCH_H
