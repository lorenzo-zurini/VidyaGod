#ifndef BINARYPATCH_H
#define BINARYPATCH_H

#include <cstdint>
#include <filesystem>
#include <functional>
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

// The bytes a patch reads and writes. A whole file in memory (MemoryImage), or a file read only where a patch looks
// (SparseImage: its header, its sites, one pass for a scan) with what the patches wrote kept as runs — a disc image of
// gigabytes is never held, and a one-byte patch costs one byte.
class Image
{
public:
    virtual ~Image() = default;
    virtual uint64_t Size() const = 0;
    // [Off, Off + N) as the patches so far left it; false past the end or on a read error.
    virtual bool Read(uint64_t Off, size_t N, uint8_t *Out) const = 0;
    virtual void Write(uint64_t Off, const uint8_t *P, size_t N) = 0;
    // How much a scan (ANCHOR, ReplaceAll) reads at a time.
    virtual size_t ScanChunk() const { return size_t(4) << 20; }
};

class MemoryImage final : public Image
{
public:
    explicit MemoryImage(std::vector<uint8_t> &Bytes) : Bytes(Bytes) {}
    uint64_t Size() const override { return Bytes.size(); }
    bool Read(uint64_t Off, size_t N, uint8_t *Out) const override;
    void Write(uint64_t Off, const uint8_t *P, size_t N) override;
private:
    std::vector<uint8_t> &Bytes;
};

class SparseImage final : public Image
{
public:
    // Reads N bytes at Off into Buf: how many it read, < 0 on an error.
    using Reader = std::function<long long(void *Buf, size_t N, uint64_t Off)>;
    SparseImage(uint64_t Size, Reader R, size_t Chunk = size_t(4) << 20) : Bytes(Size), From(std::move(R)), Chunk(Chunk) {}
    uint64_t Size() const override { return Bytes; }
    bool Read(uint64_t Off, size_t N, uint8_t *Out) const override;
    void Write(uint64_t Off, const uint8_t *P, size_t N) override;
    size_t ScanChunk() const override { return Chunk; }
    // What the patches wrote: runs by offset, none touching another.
    const std::map<uint64_t, std::vector<uint8_t>> &Written() const { return Runs; }
    // Done reading: the reader (and whatever it holds — a whole file read once) is let go; Written() stays.
    void LetGo() { From = nullptr; }
private:
    uint64_t Bytes;
    Reader From;
    size_t Chunk;
    std::map<uint64_t, std::vector<uint8_t>> Runs;
};

// Apply one BinaryPatch descriptor to an in-memory PE image buffer (the testable core; the on-disk launch path
// and the unit tests both drive this). Fields are already whole-JSON %token%-substituted by BuildSubComponentsArray,
// but Vars is passed for any residual field-level render (e.g. a Poke VALUE like "%woxl_res_w:u16le%"). On
// Result::Error, Msg explains why; on Applied/Skipped it is a short human note. Image is modified only on Applied.
Result ApplyOne(const nlohmann::ordered_json &Patch, Image &Img,
                const std::map<std::string, std::string> &Vars, std::string &Msg);
Result ApplyOne(const nlohmann::ordered_json &Patch, std::vector<uint8_t> &Bytes,
                const std::map<std::string, std::string> &Vars, std::string &Msg);

// Before the mount: every APPLY:"prefix" patch on a file a lower layer serves (a zip's, a folder's, a single file
// like a disc image) is resolved against that file as the mount will serve it, reading only what it needs, and its
// bytes go into the spec's "patches": the mount serves them on every read, and nothing is copied into the writelayer.
// A file the writelayer or a kept folder serves, or a patch that fails, is left to ProcessBinaryPatches, which applies
// and reports it after the mount. The files planned are noted in ContainerParams::ReadTimePatched.
//Left: each file not planned, and why (a copy of its own serves it, a patch on it failed, …).
void PlanReadTime(struct ContainerParams &ContainerParams, nlohmann::ordered_json &Spec,
                  std::map<std::filesystem::path, std::string> *Left = nullptr);

// Process every BinaryPatch subcomponent with APPLY=="prefix" (the default) on the mounted runtime: a file patched as
// the mount reads it (PlanReadTime) is found patched and passes; any other is read where its patches look and only
// the bytes they change are written (COW into the writelayer). APPLY=="memory" layers are skipped here — they are
// handled by vglobby at process-start. Returns false if any patch errored (loud, non-fatal — feeds the launch verdict,
// same contract as ProcessFileEdits).
[[nodiscard]] bool ProcessBinaryPatches(struct ContainerParams &ContainerParams);
}

#endif // BINARYPATCH_H
