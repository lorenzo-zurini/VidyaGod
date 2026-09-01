#include "vgtest.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "binarypatch.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using BinaryPatch::ApplyOne;
using BinaryPatch::Result;

// ---- a minimal but real PE32 image: one executable .text section at VA 0x401000 / file offset 0x400 ---------
// The engine parses actual PE section headers, so the tests must feed it a genuine (if tiny) PE.
namespace
{
constexpr uint32_t kImageBase = 0x400000;
constexpr uint32_t kTextRva   = 0x1000;
constexpr uint32_t kTextOff   = 0x400;
constexpr uint32_t kTextSize  = 0x2000;
constexpr uint32_t kTextVa    = kImageBase + kTextRva;   // 0x401000

void W16(std::vector<uint8_t> &b, size_t o, uint16_t v) { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; }
void W32(std::vector<uint8_t> &b, size_t o, uint32_t v)
{ for (int i = 0; i < 4; ++i) b[o + i] = (v >> (8 * i)) & 0xFF; }

// Build a PE whose .text raw region begins with `code`, the rest zero-filled (so the tail is a valid auto-cave).
std::vector<uint8_t> MakePe(const std::vector<uint8_t> &code)
{
    const size_t peOff = 0x80, optSize = 0xE0, secTab = peOff + 24 + optSize;
    std::vector<uint8_t> b(kTextOff + kTextSize, 0);
    b[0] = 'M'; b[1] = 'Z';
    W32(b, 0x3C, uint32_t(peOff));
    b[peOff] = 'P'; b[peOff + 1] = 'E';
    W16(b, peOff + 4, 0x14C);            // Machine i386
    W16(b, peOff + 6, 1);                // NumberOfSections
    W16(b, peOff + 20, uint16_t(optSize));
    W16(b, peOff + 24, 0x10B);           // Optional magic PE32
    W32(b, peOff + 24 + 28, kImageBase); // ImageBase
    // one section header
    std::memcpy(&b[secTab], ".text\0\0\0", 8);
    W32(b, secTab + 8,  kTextSize);      // VirtualSize
    W32(b, secTab + 12, kTextRva);       // VirtualAddress
    W32(b, secTab + 16, kTextSize);      // SizeOfRawData
    W32(b, secTab + 20, kTextOff);       // PointerToRawData
    W32(b, secTab + 36, 0x60000020);     // CODE | EXECUTE | READ
    std::memcpy(&b[kTextOff], code.data(), code.size());
    return b;
}

std::vector<uint8_t> Bytes(std::initializer_list<int> v) { return {v.begin(), v.end()}; }
int32_t DecodeRel(const std::vector<uint8_t> &b, size_t at)
{ return int32_t(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (uint32_t(b[at + 3]) << 24)); }

const std::map<std::string, std::string> NoVars{};
}  // namespace

TEST(bp_replace_by_offset_va)
{
    auto img = MakePe(Bytes({0x01, 0x00, 0x00, 0x00, 0x01}));
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "0100000001"}, {"REPLACE", "0000000000"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);
    for (int i = 0; i < 5; ++i) CHECK_EQ(int(img[kTextOff + i]), 0);
}

TEST(bp_replace_shorter_is_nop_padded)
{
    auto img = MakePe(Bytes({0x01, 0x00, 0x00, 0x00, 0x01}));
    // REPLACE is 1 byte into a 5-byte EXPECT region → 0x00 then four 0x90.
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "0100000001"}, {"REPLACE", "00"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);
    CHECK_EQ(int(img[kTextOff]), 0x00);
    for (int i = 1; i < 5; ++i) CHECK_EQ(int(img[kTextOff + i]), 0x90);
}

TEST(bp_replace_by_anchor)
{
    auto img = MakePe(Bytes({0x90, 0x8B, 0x44, 0x24, 0x08, 0xC3}));
    // Anchor with a wildcard locates the site uniquely; overwrite the two middle bytes.
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"ANCHOR", "8b ?? 24 08"},
                             {"EXPECT", "8b442408"}, {"REPLACE", "8b542408"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);
    CHECK_EQ(int(img[kTextOff + 2]), 0x54);   // 0x44 → 0x54
}

TEST(bp_expect_mismatch_errors)
{
    auto img = MakePe(Bytes({0x01, 0x02, 0x03, 0x04, 0x05}));
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "aabbccddee"}, {"REPLACE", "0000000000"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Error);
    // the original bytes are untouched
    CHECK_EQ(int(img[kTextOff]), 0x01);
}

TEST(bp_replace_is_idempotent)
{
    auto img = MakePe(Bytes({0x01, 0x00, 0x00, 0x00, 0x01}));
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "0100000001"}, {"REPLACE", "0000000000"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Skipped);   // already patched
}

TEST(bp_anchor_ambiguous_errors)
{
    auto img = MakePe(Bytes({0x90, 0x90, 0x90, 0x90}));   // "90 90" matches at three places
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"ANCHOR", "90 90"},
                             {"EXPECT", "9090"}, {"REPLACE", "0000"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Error);
}

TEST(bp_poke_renders_and_writes_le)
{
    auto img = MakePe(Bytes({0xFF, 0xFF, 0xFF, 0xFF}));
    // VALUE is a %token:u32le% — 10 → 0a 00 00 00, written little-endian at the site.
    nlohmann::ordered_json p{{"MODE", "Poke"}, {"OFFSET", "0x401000"}, {"VALUE", "%W:u32le%"}};
    std::string msg;
    CHECK(ApplyOne(p, img, {{"W", "10"}}, msg) == Result::Applied);
    CHECK_EQ(int(img[kTextOff + 0]), 0x0A);
    CHECK_EQ(int(img[kTextOff + 1]), 0x00);
    CHECK_EQ(int(img[kTextOff + 2]), 0x00);
    CHECK_EQ(int(img[kTextOff + 3]), 0x00);
}

TEST(bp_poke_guarded_by_expect)
{
    auto img = MakePe(Bytes({0x00, 0x00}));
    nlohmann::ordered_json p{{"MODE", "Poke"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "ffff"}, {"VALUE", "%W:u16le%"}};
    std::string msg;
    CHECK(ApplyOne(p, img, {{"W", "1"}}, msg) == Result::Error);   // EXPECT ff ff, file has 00 00
}

TEST(bp_cave_trampoline_math)
{
    // A 6-byte instruction at the site is displaced into an auto-cave; PAYLOAD is a stand-in body (two NOPs).
    auto img = MakePe(Bytes({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}));   // mov eax,1 ; ret
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "b801000000c3"}, {"PAYLOAD", "9090"}, {"CAVE", "auto"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);

    // Site: E9 rel32 (jmp to cave) then one NOP (6-byte region, 5-byte jmp).
    CHECK_EQ(int(img[kTextOff]), 0xE9);
    CHECK_EQ(int(img[kTextOff + 5]), 0x90);
    const int32_t relToCave = DecodeRel(img, kTextOff + 1);
    const uint32_t caveVa   = kTextVa + 5 + uint32_t(relToCave);   // site_va + 5 + rel
    const size_t   caveOff  = kTextOff + (caveVa - kTextVa);

    // Cave body: PAYLOAD(90 90) + displaced(b8 01 00 00 00 c3) + jmp-back(E9 rel32).
    CHECK_EQ(int(img[caveOff + 0]), 0x90);
    CHECK_EQ(int(img[caveOff + 1]), 0x90);
    CHECK_EQ(int(img[caveOff + 2]), 0xB8);
    CHECK_EQ(int(img[caveOff + 7]), 0xC3);
    CHECK_EQ(int(img[caveOff + 8]), 0xE9);
    const int32_t relBack   = DecodeRel(img, caveOff + 9);
    const uint32_t backFrom = caveVa + 8;                          // address of the jmp-back
    const uint32_t backTo   = backFrom + 5 + uint32_t(relBack);
    CHECK_EQ(backTo, kTextVa + 6);                                 // resumes just past the displaced region
}

TEST(bp_cave_is_idempotent)
{
    auto img = MakePe(Bytes({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}));
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "b801000000c3"}, {"PAYLOAD", "9090"}, {"CAVE", "auto"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Applied);
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Skipped);       // stub signature detected
}

TEST(bp_cave_expect_too_short_errors)
{
    auto img = MakePe(Bytes({0x90, 0x90, 0x90, 0x90}));
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "9090"}, {"PAYLOAD", "90"}, {"CAVE", "auto"}};   // 2 < 5
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Error);
}

TEST(bp_cave_fixed_nonzero_errors)
{
    // Point CAVE at the site itself (non-zero) → refuses to clobber real code.
    auto img = MakePe(Bytes({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}));
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "b801000000c3"}, {"PAYLOAD", "9090"}, {"CAVE", "0x401000"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Error);
}

TEST(bp_not_a_pe_errors)
{
    std::vector<uint8_t> junk(64, 0xAB);
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "ab"}, {"REPLACE", "00"}};
    std::string msg;
    CHECK(ApplyOne(p, junk, NoVars, msg) == Result::Error);
}

// ---- Reproduction proof (gated): the engine must reproduce the hand-made WOXL derivative exes byte-for-byte ---
// These are the exact BinaryPatch descriptors migrated into the Wipeout XL package (No-CD for both exes + the
// blit-queue Cave for NET-WOXL, transcribed from tools/woxl_blitqueue_patch.py). Runs only when VG_WOXL_REPRO_DIR
// points at a dir holding pristine/{sp,mp} and patched/{sp,mp} — inert in CI, a real-binary check locally.
namespace
{
std::vector<uint8_t> ReadFile(const std::string &p)
{
    std::ifstream in(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// The migrated SP No-CD (Dege's crack) — 6 byte edits.
const std::vector<nlohmann::ordered_json> kSpNoCd = {
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x3f0"},   {"EXPECT","0000000000000000000000"},{"REPLACE","33db8718538b1bff5308c3"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x431f00"},{"EXPECT","508b18ff5308"},{"REPLACE","909090909090"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x432795"},{"EXPECT","a1"},{"REPLACE","b8"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x43279a"},{"EXPECT","508b18ff5308"},{"REPLACE","909090909090"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x45ea1d"},{"EXPECT","e81822fdff"},{"REPLACE","b801000000"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x4671fd"},{"EXPECT","ff5008"},{"REPLACE","589090"}},
};
// The migrated MP No-CD (5 flag flips) + the blit-queue crash fix (Cave).
const std::vector<nlohmann::ordered_json> kMpNoCdBlit = {
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x44a45c"},{"EXPECT","01"},{"REPLACE","00"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x44a4e6"},{"EXPECT","01"},{"REPLACE","00"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x44a5a2"},{"EXPECT","01"},{"REPLACE","00"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x44a770"},{"EXPECT","01"},{"REPLACE","00"}},
    {{"TYPE","BinaryPatch"},{"MODE","Replace"},{"OFFSET","0x44a7a7"},{"EXPECT","01"},{"REPLACE","00"}},
    {{"TYPE","BinaryPatch"},{"MODE","Cave"},{"OFFSET","0x44be08"},{"CAVE","0x4cf360"},
     {"EXPECT","c705d07d4d0001000000"},
     {"PAYLOAD","5031c0a390d09000a394d09000a398d09000a39cd0900058"}},
};

bool ReproOne(const std::string &pristine, const std::string &patched,
              const std::vector<nlohmann::ordered_json> &patches)
{
    std::vector<uint8_t> img = ReadFile(pristine);
    if (img.empty()) return false;
    for (const auto &p : patches)
    {
        std::string msg;
        if (ApplyOne(p, img, NoVars, msg) != Result::Applied)
        { std::fprintf(stderr, "    repro apply failed: %s\n", msg.c_str()); return false; }
    }
    return img == ReadFile(patched);
}
}  // namespace

TEST(bp_reproduces_woxl_derivatives)
{
    const char *dir = std::getenv("VG_WOXL_REPRO_DIR");
    if (!dir) return;   // gated: no fixtures, nothing to prove here
    const std::string d = dir;
    CHECK(ReproOne(d + "/pristine/sp/Wipeout2.exe", d + "/patched/sp/Wipeout2.exe", kSpNoCd));
    CHECK(ReproOne(d + "/pristine/mp/NET-WOXL.EXE", d + "/patched/mp/NET-WOXL.EXE", kMpNoCdBlit));
}
