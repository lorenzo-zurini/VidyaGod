#include "vgtest.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "binarypatch.h"
#include "commonutils.h"   // Diagnostics — what the pass warned of
#include "layerspec.h"   // VidyaGodFS: the spec the planner fills, opened as the mount opens it
#include "overlay.h"
#include "vfsops.h"

#include <set>
#include <zip.h>

#include <chrono>
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

// A cave given as a file offset outside every section (here, zeroes in the PE headers) has no address to jump to.
// Teeth: dereference the empty lookup (as it did) and the patch is "applied" with a jump to a garbage address.
TEST(bp_cave_outside_every_section_errors)
{
    auto img = MakePe(Bytes({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}));
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "b801000000c3"}, {"PAYLOAD", "9090"}, {"CAVE", "0x300"}};
    std::string msg;
    CHECK(ApplyOne(p, img, NoVars, msg) == Result::Error);
    CHECK(msg.find("outside every section") != std::string::npos);
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

// An ELF (Unleashed Recompiled's Flatpak build) is patched by ANCHOR and raw OFFSET: a file needs no PE layout for that.
TEST(a_file_that_is_not_a_pe_is_patched_by_anchor_and_raw_offset)
{
    std::vector<uint8_t> elf(64, 0xAB);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    const char lit[] = "/var/data";
    std::memcpy(elf.data() + 20, lit, sizeof lit);          // with its NUL
    nlohmann::ordered_json a{{"MODE", "Replace"}, {"ANCHOR", "2f 76 61 72 2f 64 61 74 61 00"},
                             {"EXPECT", "2f7661722f64617461"}, {"REPLACE", "2e2f2e2f2e2f2e2f2e"}};
    std::string msg;
    CHECK(ApplyOne(a, elf, NoVars, msg) == Result::Applied);
    CHECK(std::memcmp(elf.data() + 20, "././././.", 9) == 0);
    CHECK_EQ(int(elf[29]), 0);
    nlohmann::ordered_json o{{"MODE", "Replace"}, {"OFFSET", "0x3c"}, {"EXPECT", "abab"}, {"REPLACE", "0102"}};
    CHECK(ApplyOne(o, elf, NoVars, msg) == Result::Applied);
    CHECK_EQ(int(elf[0x3c]), 1);
    CHECK_EQ(int(elf[0x3d]), 2);
}

// No NOP padding outside a PE: a shorter REPLACE in an ELF or data file is refused, the file untouched.
TEST(a_short_replace_in_a_file_that_is_not_a_pe_errors)
{
    std::vector<uint8_t> elf(64, 0xAB);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "16"}, {"EXPECT", "abab"}, {"REPLACE", "00"}};
    std::string msg;
    CHECK(ApplyOne(p, elf, NoVars, msg) == Result::Error);
    CHECK_EQ(int(elf[16]), 0xAB);
    CHECK_EQ(int(elf[17]), 0xAB);
}

TEST(an_offset_past_the_end_of_a_file_that_is_not_a_pe_errors)
{
    std::vector<uint8_t> junk(64, 0xAB);
    nlohmann::ordered_json p{{"MODE", "Replace"}, {"OFFSET", "0x401000"},
                             {"EXPECT", "ab"}, {"REPLACE", "00"}};
    std::string msg;
    CHECK(ApplyOne(p, junk, NoVars, msg) == Result::Error);
}

TEST(a_cave_needs_a_pe_image)
{
    std::vector<uint8_t> junk(256, 0);
    junk[16] = 0x8B; junk[17] = 0x44; junk[18] = 0x24; junk[19] = 0x08; junk[20] = 0xC3;
    nlohmann::ordered_json p{{"MODE", "Cave"}, {"OFFSET", "16"}, {"EXPECT", "8b442408c3"}, {"PAYLOAD", "90"}};
    std::string msg;
    CHECK(ApplyOne(p, junk, NoVars, msg) == Result::Error);
    CHECK(msg.find("PE") != std::string::npos);
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

//A minimal, valid PE32 — BinaryPatch refuses anything else, so an escape test whose target is NOT a PE passes
//whether or not the guard exists: the patch fails on the PE check before it would have written. Same layout as
//the launch-matrix fixture's: one .text section at RVA 0x1000 over ImageBase 0x400000, first 64 bytes a ramp.
static std::vector<uint8_t> MinimalPe()
{
    std::vector<uint8_t> D(0x800, 0);
    auto W32 = [&](size_t O, uint32_t V) { D[O] = uint8_t(V); D[O+1] = uint8_t(V>>8); D[O+2] = uint8_t(V>>16); D[O+3] = uint8_t(V>>24); };
    auto W16 = [&](size_t O, uint16_t V) { D[O] = uint8_t(V); D[O+1] = uint8_t(V>>8); };
    D[0] = 'M'; D[1] = 'Z';
    W32(0x3C, 0x80);                                   // e_lfanew
    D[0x80] = 'P'; D[0x81] = 'E';
    W16(0x84, 0x014C);                                 // Machine i386
    W16(0x86, 1);                                      // NumberOfSections
    W16(0x94, 0xE0);                                   // SizeOfOptionalHeader
    W16(0x96, 0x0102);                                 // Characteristics
    W16(0x98, 0x10B);                                  // PE32
    W32(0x98 + 28, 0x400000);                          // ImageBase
    const size_t Sec = 0x98 + 0xE0;
    std::memcpy(&D[Sec], ".text\0\0\0", 8);
    W32(Sec +  8, 0x400);                              // VirtualSize
    W32(Sec + 12, 0x1000);                             // VirtualAddress
    W32(Sec + 16, 0x400);                              // SizeOfRawData
    W32(Sec + 20, 0x400);                              // PointerToRawData
    W32(Sec + 36, 0x60000020);                         // code | execute | read
    for (int I = 0; I < 64; ++I) D[0x400 + (size_t)I] = uint8_t(I);
    return D;
}

//A BinaryPatch FILE is untrusted input exactly like a FileEdit's — a package arrives from a peer by CID — and
//the escape check used to be a second copy living in this TU with no test at all: deleting it broke nothing.
//It is one shared function now, and this is the assertion that keeps the patch side honest about using it.
TEST(a_binarypatch_path_that_climbs_out_of_the_runtime_is_refused)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_escape";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME";
    std::filesystem::create_directories(Runtime);
    //A REAL PE, so that without the guard the patch would genuinely apply and change these bytes.
    const std::filesystem::path Outside = Root / "outside.exe";
    const std::vector<uint8_t> Pristine = MinimalPe();
    { std::ofstream O(Outside, std::ios::binary); O.write((const char *)Pristine.data(), (std::streamsize)Pristine.size()); }

    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE","../outside.exe"},
                               {"EDITS", nlohmann::ordered_json::array()},
                               {"MODE","Poke"},{"OFFSET","0x0"},{"VALUE","ff"}}});

    CHECK(!BinaryPatch::ProcessBinaryPatches(CP));       // refused, and reported as a failure
    std::vector<uint8_t> After;
    {   // closed before the cleanup: Windows will not remove a file that is still open
        std::ifstream In(Outside, std::ios::binary);
        After.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    CHECK(After == Pristine);                            // and byte-for-byte untouched
    std::filesystem::remove_all(Root);
}

//And the on-disk half: a patch target inside the runtime that is a link the game left in a kept folder (game.exe ->
//a file of the user's) is never patched through it. Teeth: drop the LinkLeadsOut check (the outside file changes).
TEST(a_binarypatch_never_writes_through_a_link_out_of_the_runtime)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_link";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME";
    std::filesystem::create_directories(Runtime / "saves");
    const std::filesystem::path Outside = Root / "outside.exe";
    const std::vector<uint8_t> Pristine = MinimalPe();
    { std::ofstream O(Outside, std::ios::binary); O.write((const char *)Pristine.data(), (std::streamsize)Pristine.size()); }
    std::filesystem::create_symlink(Outside, Runtime / "saves/game.exe");

    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE","saves/game.exe"},
                               {"EDITS", nlohmann::ordered_json::array()},
                               {"MODE","Poke"},{"OFFSET","0x0"},{"VALUE","ff"}}});
    CHECK(!BinaryPatch::ProcessBinaryPatches(CP));
    std::vector<uint8_t> After;
    {
        std::ifstream In(Outside, std::ios::binary);
        After.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    CHECK(After == Pristine);
    std::filesystem::remove_all(Root);
}

//The patch side of the same rule, which was unpinned: an ABSOLUTE FILE already inside the runtime must be
//used as is. Stripping its leading separator turned "%RuntimePath%/game/x.exe" into
//"<runtime>/home/.../RUNTIME/game/x.exe", a path that exists nowhere — so the patch silently did not apply.
TEST(an_absolute_binarypatch_path_already_inside_the_runtime_is_used_as_is)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_absin";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME";
    std::filesystem::create_directories(Runtime / "game");
    const std::filesystem::path Exe = Runtime / "game" / "x.exe";
    { const std::vector<uint8_t> P = MinimalPe();
      std::ofstream O(Exe, std::ios::binary); O.write((const char *)P.data(), (std::streamsize)P.size()); }

    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE", Exe.string()},   // absolute, inside the runtime
                               {"MODE","Poke"},{"OFFSET","0x401000"},{"VALUE","ff"}}});

    CHECK(BinaryPatch::ProcessBinaryPatches(CP));
    std::vector<uint8_t> After;
    {   // closed before the cleanup: Windows will not remove a file that is still open
        std::ifstream In(Exe, std::ios::binary);
        After.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
    }
    CHECK(After.size() > 0x400);
    if (After.size() > 0x400) CHECK_EQ((int)After[0x400], 0xff);   // the patch landed
    std::filesystem::remove_all(Root);
}

// Or sets bits in a flag byte several options share (UserPatch keeps six installer checkboxes as bits of one byte):
// each option ORs its own bits, so any subset composes in any order; bits already set are an idempotent skip; an
// EXPECT is refused (the byte legitimately carries other options' bits). Teeth: overwrite instead of OR (the second
// option wipes the first's bit); treat "already set" as an error; accept EXPECT.
TEST(bp_or_sets_bits_and_composes)
{
    auto img = MakePe(Bytes({0x80, 0x00, 0x00}));   // bit 0x80 already there (another option, or the original)
    nlohmann::ordered_json a{{"MODE", "Or"}, {"OFFSET", "0x401000"}, {"VALUE", "04"}};
    nlohmann::ordered_json b{{"MODE", "Or"}, {"OFFSET", "0x401000"}, {"VALUE", "03"}};
    std::string msg;
    CHECK(ApplyOne(a, img, NoVars, msg) == Result::Applied);
    CHECK(ApplyOne(b, img, NoVars, msg) == Result::Applied);
    CHECK_EQ(int(img[kTextOff]), 0x87);
    auto img2 = MakePe(Bytes({0x80, 0x00, 0x00}));
    CHECK(ApplyOne(b, img2, NoVars, msg) == Result::Applied);
    CHECK(ApplyOne(a, img2, NoVars, msg) == Result::Applied);
    CHECK(img2 == img);                                                     // order does not matter
    CHECK(ApplyOne(a, img, NoVars, msg) == Result::Skipped);                // already set
    CHECK_EQ(int(img[kTextOff]), 0x87);
    nlohmann::ordered_json g{{"MODE", "Or"}, {"OFFSET", "0x401001"}, {"VALUE", "01"}, {"EXPECT", "00"}};
    CHECK(ApplyOne(g, img, NoVars, msg) == Result::Error);
    CHECK_EQ(int(img[kTextOff + 1]), 0x00);
}

// ReplaceAll rewrites EVERY match of EXPECT, in a file that is no PE (a sound archive's WAV headers): ops run in order,
// so a later one matches what an earlier one wrote (the sound fix restores a few short files that way); none left but
// the result there is the idempotent skip; neither there is the wrong file; it takes no site. Teeth: stop at the
// first match; treat "already patched" as an error; accept an ANCHOR; accept a REPLACE of another length.
TEST(bp_replaceall_rewrites_every_match_in_order)
{
    std::vector<uint8_t> img = Bytes({0x52, 0xAA, 0xBB, 0x00, 0xAA, 0xBB, 0x01, 0xAA, 0xBB});
    nlohmann::ordered_json a{{"MODE", "ReplaceAll"}, {"EXPECT", "aabb"}, {"REPLACE", "1122"}};
    nlohmann::ordered_json b{{"MODE", "ReplaceAll"}, {"EXPECT", "521122"}, {"REPLACE", "52aabb"}};   // matches a's output
    std::string msg;
    CHECK(ApplyOne(a, img, NoVars, msg) == Result::Applied);
    CHECK(img == Bytes({0x52, 0x11, 0x22, 0x00, 0x11, 0x22, 0x01, 0x11, 0x22}));
    CHECK(ApplyOne(b, img, NoVars, msg) == Result::Applied);
    CHECK(img == Bytes({0x52, 0xAA, 0xBB, 0x00, 0x11, 0x22, 0x01, 0x11, 0x22}));
    CHECK(ApplyOne(b, img, NoVars, msg) == Result::Skipped);                  // nothing left, the result is there
    std::vector<uint8_t> other = Bytes({0x01, 0x02, 0x03});
    CHECK(ApplyOne(a, other, NoVars, msg) == Result::Error);                 // neither: the wrong file
    nlohmann::ordered_json sited{{"MODE", "ReplaceAll"}, {"ANCHOR", "aabb"}, {"EXPECT", "aabb"}, {"REPLACE", "1122"}};
    CHECK(ApplyOne(sited, img, NoVars, msg) == Result::Error);
    nlohmann::ordered_json longer{{"MODE", "ReplaceAll"}, {"EXPECT", "1122"}, {"REPLACE", "112233"}};
    CHECK(ApplyOne(longer, img, NoVars, msg) == Result::Error);
}

// A launch reads each patched file once and writes it once for its run of ops (nineteen ops over a 200 MB archive
// were read and written nineteen times), while the ops still apply in order and every file gets its own. Teeth: drop
// the flush at a file change (A's ops lost) or at the end (B's lost); forget to reload on a change (B written into A).
TEST(bp_ops_on_several_files_apply_in_order_each_to_its_own)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_batch";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME";
    std::filesystem::create_directories(Runtime);
    auto Put = [&](const char *N, std::vector<uint8_t> B) { std::ofstream O(Runtime / N, std::ios::binary); O.write((const char *)B.data(), (std::streamsize)B.size()); };
    auto Get = [&](const char *N) { std::ifstream In(Runtime / N, std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()); };
    Put("a.afs", Bytes({0xAA, 0x00, 0xAA}));
    Put("b.afs", Bytes({0xBB, 0x00, 0xBB, 0x00}));
    auto Op = [](const char *F, const char *E, const char *R) {
        return nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE",F},{"MODE","ReplaceAll"},{"EXPECT",E},{"REPLACE",R}}; };
    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({Op("a.afs","aa","11"), Op("a.afs","1100","2200"),
                                                           Op("b.afs","bb","33"), Op("a.afs","22","44")});
    CHECK(BinaryPatch::ProcessBinaryPatches(CP));
    CHECK(Get("a.afs") == Bytes({0x44, 0x00, 0x11}));
    CHECK(Get("b.afs") == Bytes({0x33, 0x00, 0x33, 0x00}));
    std::filesystem::remove_all(Root);
}

// A fix that ships as a tool (a batch file running a byte-replacer) becomes ops, and the ops must do exactly what the
// tool did. Runs only when VG_BP_REPRO_DIR holds ops.json ([{FILE, OPS:[op...]}]), pristine/<FILE> and the tool's
// output at expected/<FILE>: each file is patched in a scratch runtime and compared byte for byte. Inert in CI.
TEST(bp_reproduces_a_tools_output)
{
    const char *dir = std::getenv("VG_BP_REPRO_DIR");
    if (!dir) return;   // gated: no fixtures, nothing to prove here
    const std::filesystem::path D = dir, Runtime = std::filesystem::temp_directory_path() / "vg_bp_repro";
    std::filesystem::remove_all(Runtime);
    nlohmann::ordered_json Ops;
    { std::ifstream In(D / "ops.json"); Ops = nlohmann::ordered_json::parse(In); }
    ContainerParams CP(Runtime / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array();
    for (const auto &F : Ops)
    {
        const std::string File = F.value("FILE", std::string());
        std::filesystem::create_directories((Runtime / File).parent_path());
        std::filesystem::copy_file(D / "pristine" / File, Runtime / File);
        for (auto Op : F["OPS"]) { Op["TYPE"] = "BinaryPatch"; Op["FILE"] = File; CP.SubComponentsArray.push_back(Op); }
    }
    CHECK(BinaryPatch::ProcessBinaryPatches(CP));
    for (const auto &F : Ops)
    {
        const std::string File = F.value("FILE", std::string());
        CHECK(ReadFile((Runtime / File).string()) == ReadFile((D / "expected" / File).string()));
    }
    std::filesystem::remove_all(Runtime);
}

// ---- a file read only where the patches look (SparseImage) ----------------------------------------------------

namespace
{
//Runs Ops in order on Start twice — held whole (MemoryImage) and read where needed in chunks of Chunk bytes
//(SparseImage) — and checks each op's result and message agree and the bytes end the same.
void SameEitherWay(const std::vector<uint8_t> &Start, const std::vector<nlohmann::ordered_json> &Ops, size_t Chunk)
{
    std::vector<uint8_t> Whole = Start;
    BinaryPatch::SparseImage Sparse(Start.size(), [&](void *B, size_t N, uint64_t Off) -> long long {
        if (Off >= Start.size()) return 0;
        const size_t K = std::min<size_t>(N, Start.size() - size_t(Off));
        std::memcpy(B, Start.data() + Off, K);
        return (long long)K;
    }, Chunk);
    for (const auto &Op : Ops)
    {
        std::string M1, M2;
        const Result R1 = ApplyOne(Op, Whole, NoVars, M1);
        const Result R2 = ApplyOne(Op, Sparse, NoVars, M2);
        CHECK(R1 == R2);
        CHECK_EQ(M1, M2);
    }
    std::vector<uint8_t> Seen(Start.size());
    CHECK(Sparse.Read(0, Seen.size(), Seen.data()));
    CHECK(Seen == Whole);
    std::vector<uint8_t> Laid = Start;                       // what the pass after the mount writes: the runs
    for (const auto &[At, B] : Sparse.Written()) std::copy(B.begin(), B.end(), Laid.begin() + std::ptrdiff_t(At));
    CHECK(Laid == Whole);
}
}

// A file read where the patches look patches exactly as one held whole, whatever the scan's chunk (matches across a
// chunk's edge, overlapping matches, a pattern led by wildcards, every mode, ops seeing earlier ops' bytes). Teeth: a
// scan that drops a match on a chunk's edge; overlapping ReplaceAll matches; a read that misses earlier writes.
TEST(bp_a_file_read_where_needed_patches_as_one_held_whole)
{
    std::vector<uint8_t> Code(0x300, 0x90);
    const std::vector<uint8_t> Sig = Bytes({0x8B, 0x45, 0x08, 0x85, 0xC0, 0x74, 0x10});
    std::copy(Sig.begin(), Sig.end(), Code.begin() + 0x101);
    for (size_t I = 0x200; I < 0x240; ++I) Code[I] = 0xAA;                 // "aaaa…": overlapping matches of "aa aa"
    Code[0x250] = 0x12; Code[0x251] = 0x34; Code[0x252] = 0x12; Code[0x253] = 0x34;
    const std::vector<uint8_t> Pe = MakePe(Code);
    const std::vector<nlohmann::ordered_json> Ops = {
        {{"MODE", "Replace"}, {"ANCHOR", "8b 45 08 85 c0 74 ??"}, {"EXPECT", "8b450885c07410"}, {"REPLACE", "8b450885c0eb10"}},
        {{"MODE", "ReplaceAll"}, {"EXPECT", "aaaa"}, {"REPLACE", "aabb"}},
        {{"MODE", "ReplaceAll"}, {"EXPECT", "bbaa"}, {"REPLACE", "cccc"}},       // over what the last one wrote
        {{"MODE", "ReplaceAll"}, {"EXPECT", "aaaa"}, {"REPLACE", "aabb"}},
        {{"MODE", "Poke"}, {"OFFSET", "0x401010"}, {"VALUE", "0102"}},
        {{"MODE", "Or"}, {"OFFSET", "0x401011"}, {"VALUE", "f0"}},
        {{"MODE", "Replace"}, {"ANCHOR", "?? ?? 12 34 12 34"}, {"EXPECT", "aaaa12341234"}, {"REPLACE", "aaaa56785678"}},
        {{"MODE", "Replace"}, {"ANCHOR", "?? 90 90 90"}, {"EXPECT", "90909090"}, {"REPLACE", "c3909090"}},   // ambiguous
        {{"MODE", "Cave"}, {"OFFSET", "0x401101"}, {"EXPECT", "8b45088598"}, {"PAYLOAD", "90"}},          // a wrong guard
        {{"MODE", "Cave"}, {"OFFSET", "0x401101"}, {"EXPECT", "8b45088585"}, {"PAYLOAD", "6690"}},
        {{"MODE", "Cave"}, {"OFFSET", "0x401101"}, {"EXPECT", "8b45088585"}, {"PAYLOAD", "6690"}},        // caved: skipped
        {{"MODE", "ReplaceAll"}, {"EXPECT", "deadbeef"}, {"REPLACE", "00000000"}},                       // nowhere
        {{"MODE", "Poke"}, {"OFFSET", "0x9999999"}, {"VALUE", "01"}} };                                  // past the end
    for (const size_t Chunk : { (size_t)1, (size_t)2, (size_t)3, (size_t)7, (size_t)64, (size_t)4096, (size_t)4 << 20 })
        SameEitherWay(Pe, Ops, Chunk);
    std::vector<uint8_t> Data(1000);
    for (size_t I = 0; I < Data.size(); ++I) Data[I] = uint8_t(I * 7);
    Data[997] = 0x05;
    for (const size_t Chunk : { (size_t)1, (size_t)5, (size_t)999, (size_t)1000, (size_t)1001 })
        SameEitherWay(Data, { {{"MODE", "Poke"}, {"OFFSET", "997"}, {"EXPECT", "05"}, {"VALUE", "07"}},
                              {{"MODE", "Replace"}, {"ANCHOR", "07 ?? ??"}, {"EXPECT", "07"}, {"REPLACE", "08"}} }, Chunk);
}

// What a file read where needed was written keeps one run per stretch: writes that overlap or adjoin merge, a later
// one wins, and a read sees them over the file. Teeth: overlapping runs kept apart (the older one read back).
TEST(bp_a_sparse_images_writes_merge_into_runs)
{
    const std::vector<uint8_t> File(32, 0x00);
    BinaryPatch::SparseImage S(File.size(), [&](void *B, size_t N, uint64_t Off) -> long long {
        std::memcpy(B, File.data() + Off, N); return (long long)N; });
    const uint8_t A[] = { 1, 1, 1, 1 }, B[] = { 2, 2 }, C[] = { 3 }, D[] = { 4, 4 };
    S.Write(4, A, 4);    // 4..7
    S.Write(6, B, 2);    // over 6..7
    S.Write(8, C, 1);    // adjoins at 8
    CHECK_EQ(S.Written().size(), (size_t)1);                            // one run, 4..8
    S.Write(20, D, 2);   // apart
    S.Write(3, C, 1);    // adjoins at 3
    CHECK_EQ(S.Written().size(), (size_t)2);
    CHECK(S.Written().count(3) && S.Written().at(3) == Bytes({3, 1, 1, 2, 2, 3}));
    CHECK(S.Written().count(20) && S.Written().at(20) == Bytes({4, 4}));
    uint8_t Out[10];
    CHECK(S.Read(2, 10, Out));
    CHECK(std::vector<uint8_t>(Out, Out + 10) == Bytes({0, 3, 1, 1, 2, 2, 3, 0, 0, 0}));
    CHECK(!S.Read(30, 3, Out));                                        // past the end
    S.Write(0, D, 2);
    S.Write(1, A, 4);                                                  // over two runs' worth: one run 0..8
    CHECK(S.Written().count(0) && S.Written().at(0).size() == 9);
}

// Before the mount, a patch on a file a lower layer serves — a disc image placed whole, a program in a zip — goes into
// the spec, resolved against the file as the mount serves it, and the mount reads it patched while the file on disk is
// untouched. A file the writelayer serves (the user's copy), one under a kept folder and a patch whose guard does not
// match stay out (written, or reported, after the mount). Teeth: plan nothing; plan a writelayer copy; plan a failed
// patch; a wrong offset (the site the mount serves the bytes at).
TEST(bp_a_patch_on_a_file_the_layers_serve_is_planned_into_the_spec)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_readtime";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME", WL = Root / "WL", Src = Root / "SRC", Kept = Root / "KEPT";
    for (const auto &D : { Runtime, WL, Src, Kept }) std::filesystem::create_directories(D);
    auto Put = [](const std::filesystem::path &P, const std::vector<uint8_t> &B) {
        std::filesystem::create_directories(P.parent_path());
        std::ofstream O(P, std::ios::binary); O.write((const char *)B.data(), (std::streamsize)B.size()); };
    auto Get = [](const std::filesystem::path &P) {
        std::ifstream In(P, std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()); };
    std::vector<uint8_t> Iso(300000, 0x11);
    Iso[0x24924] = 0x05;
    Put(Src / "JSRF.iso", Iso);
    Put(Src / "Other.iso", Iso);
    const std::vector<uint8_t> Exe = MakePe(Bytes({0x8B, 0x45, 0x08, 0x85, 0xC0, 0x74, 0x10}));
    {
        int Err = 0;
        zip_t *Z = zip_open((Src / "game.zip").string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &Err);
        zip_source_t *S = zip_source_buffer(Z, Exe.data(), Exe.size(), 0);
        const zip_int64_t I = zip_file_add(Z, "bin/game.exe", S, ZIP_FL_ENC_UTF_8);
        zip_set_file_compression(Z, I, ZIP_CM_STORE, 0);
        zip_close(Z);
    }
    Put(Src / "cfg/saved.bin", Iso);                 // the package's copy …
    Put(WL / "cfg/saved.bin", Iso);                  // … and the user's, which the mount serves
    Put(Src / "save/slot.bin", Iso);
    nlohmann::ordered_json Spec = {
        {"mountpoint", Runtime.string()}, {"writelayer", WL.string()},
        {"layers", nlohmann::ordered_json::array({
            {{"type", "file"}, {"source", (Src / "JSRF.iso").string()}, {"target", "media"}},
            {{"type", "file"}, {"source", (Src / "Other.iso").string()}, {"target", "media"}},
            {{"type", "zip"}, {"source", (Src / "game.zip").string()}, {"target", ""}},
            {{"type", "dir"}, {"source", (Src / "cfg").string()}, {"target", "cfg"}},
            {{"type", "dir"}, {"source", (Src / "save").string()}, {"target", "save"}, {"rw", true}} })} };
    auto Op = [](const char *F, nlohmann::ordered_json O) { O["TYPE"] = "BinaryPatch"; O["FILE"] = F; return O; };
    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        Op("media/JSRF.iso", {{"MODE", "Poke"}, {"OFFSET", "0x24924"}, {"EXPECT", "05"}, {"VALUE", "07"}}),
        Op("bin/game.exe", {{"MODE", "Replace"}, {"ANCHOR", "85 c0 74 ??"}, {"EXPECT", "85c07410"}, {"REPLACE", "85c0eb10"}}),
        Op("cfg/saved.bin", {{"MODE", "Poke"}, {"OFFSET", "0x24924"}, {"EXPECT", "05"}, {"VALUE", "07"}}),
        Op("save/slot.bin", {{"MODE", "Poke"}, {"OFFSET", "0x24924"}, {"EXPECT", "05"}, {"VALUE", "07"}}),
        Op("media/Other.iso", {{"MODE", "Poke"}, {"OFFSET", "0x10"}, {"VALUE", "07"}}),          // fine, but …
        Op("media/Other.iso", {{"MODE", "Poke"}, {"OFFSET", "0x24924"}, {"EXPECT", "09"}, {"VALUE", "07"}}),   // … this fails
        Op("bin/game.exe", {{"MODE", "Poke"}, {"OFFSET", "0x401000"}, {"VALUE", "cc"}}) });
    std::map<std::filesystem::path, std::string> Left;
    BinaryPatch::PlanReadTime(CP, Spec, &Left);
    CHECK(Left.size() == 3);
    CHECK(Left[Runtime / "cfg/saved.bin"].find("copy of its own") != std::string::npos);
    CHECK(Left[Runtime / "save/slot.bin"].find("copy of its own") != std::string::npos);
    CHECK(Left[Runtime / "media/Other.iso"].find("failed") != std::string::npos);
    CHECK(Spec.contains("patches"));
    std::set<std::string> Paths;
    for (const auto &P : Spec.value("patches", nlohmann::ordered_json::array())) Paths.insert(P.value("path", std::string()));
    CHECK(Paths == std::set<std::string>({ "media/JSRF.iso", "bin/game.exe" }));
    std::set<std::string> Planned;
    for (const auto &[P, Runs] : CP.ReadTimePatched) Planned.insert(P.string());
    CHECK(Planned == std::set<std::string>({ (Runtime / "media/JSRF.iso").string(), (Runtime / "bin/game.exe").string() }));
    CHECK(CP.ReadTimePatched[Runtime / "media/JSRF.iso"] == (std::map<uint64_t, std::vector<uint8_t>>{ { 0x24924, Bytes({0x07}) } }));
    //The mount reads them patched; the files themselves are as they were.
    ::Spec S;
    std::string Err;
    VfsState V;
    CHECK(ParseSpecText(Spec.dump(), S, Err) && V.Init(S, Err));
    auto Read = [&](const std::string &P, uint64_t Off, size_t N) {
        OpenFile *F = nullptr;
        std::vector<uint8_t> B(N);
        if (VfsOpen(V, P, 0, F) != 0 || !F || VfsRead(F, B.data(), N, Off) != (ssize_t)N) B.clear();
        if (F) VfsRelease(F);
        return B;
    };
    CHECK(Read("media/JSRF.iso", 0x24923, 3) == Bytes({0x11, 0x07, 0x11}));
    CHECK(Read("bin/game.exe", 0x400, 7) == Bytes({0xCC, 0x45, 0x08, 0x85, 0xC0, 0xEB, 0x10}));
    CHECK(Read("media/Other.iso", 0x10, 1) == Bytes({0x11}));          // the whole file left to the pass after
    CHECK(Get(Src / "JSRF.iso") == Iso);
    std::filesystem::remove_all(Root);
}

// After the mount, a file planned for it to serve patched is checked by the bytes it serves, its ops not run again: two
// Replaces in turn on one site (05→06, then 06→07) would find, over the served 07, neither the first one's old bytes
// nor its result. A planned file the mount serves otherwise (a copy of its own) is patched like any, said as a
// warning (the copy the plan saves is made after all). Teeth: run the ops over a planned file (the chain fails); trust
// the plan without the check (the copy left unpatched); no warning.
TEST(bp_a_file_the_mount_serves_patched_is_checked_not_patched_again)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_served";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME";
    std::filesystem::create_directories(Runtime);
    auto Put = [&](const char *N, std::vector<uint8_t> B) { std::ofstream O(Runtime / N, std::ios::binary); O.write((const char *)B.data(), (std::streamsize)B.size()); };
    auto Get = [&](const char *N) { std::ifstream In(Runtime / N, std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()); };
    auto Op = [](const char *F, const char *E, const char *R) {
        return nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE",F},{"MODE","Replace"},{"OFFSET","1"},{"EXPECT",E},{"REPLACE",R}}; };
    Put("a.bin", Bytes({0x00, 0x07, 0x00}));          // as the mount serves it: the chain's result in
    Put("b.bin", Bytes({0x00, 0x05, 0x00}));          // a copy of its own: unpatched
    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({Op("a.bin","05","06"), Op("a.bin","06","07"),
                                                           Op("b.bin","05","06"), Op("b.bin","06","07")});
    CP.ReadTimePatched[Runtime / "a.bin"] = { { 1, Bytes({0x07}) } };
    CP.ReadTimePatched[Runtime / "b.bin"] = { { 1, Bytes({0x07}) } };
    Diagnostics::Begin();
    CHECK(BinaryPatch::ProcessBinaryPatches(CP));
    const auto Said = Diagnostics::Take();
    CHECK_EQ(Said.Warnings, (size_t)1);
    CHECK_EQ(Said.Errors, (size_t)0);
    CHECK(Get("a.bin") == Bytes({0x00, 0x07, 0x00}));
    CHECK(Get("b.bin") == Bytes({0x00, 0x07, 0x00}));
    std::filesystem::remove_all(Root);
}

// Writes end to end grow one run in place: a ReplaceAll over a stretch of repeats (a disc image's zero padding) makes
// hundreds of thousands of adjoining writes, and a run copied whole at each was quadratic (19 s for 1 MiB). Teeth: copy
// the run at each write (over the time); keep adjoining runs apart (not one run).
TEST(bp_adjoining_writes_grow_one_run_in_linear_time)
{
    const uint64_t Size = uint64_t(1) << 20;
    BinaryPatch::SparseImage S(Size, [](void *B, size_t N, uint64_t) -> long long { std::memset(B, 0, N); return (long long)N; });
    const uint8_t Four[] = { 1, 1, 1, 1 };
    const auto Began = std::chrono::steady_clock::now();
    for (uint64_t At = 0; At + 4 <= Size; At += 4) S.Write(At, Four, 4);
    const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Began).count();
    CHECK(Ms < 2000);
    CHECK_EQ(S.Written().size(), (size_t)1);
    CHECK(S.Written().begin()->second.size() == Size);
    //A whole ReplaceAll of zeros over it: one pass, one run.
    BinaryPatch::SparseImage Z(Size, [](void *B, size_t N, uint64_t) -> long long { std::memset(B, 0, N); return (long long)N; });
    std::string Msg;
    const auto Began2 = std::chrono::steady_clock::now();
    CHECK(ApplyOne(nlohmann::ordered_json{{"MODE", "ReplaceAll"}, {"EXPECT", "00000000"}, {"REPLACE", "01010101"}}, Z, NoVars, Msg) == Result::Applied);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Began2).count() < 3000);
    CHECK_EQ(Z.Written().size(), (size_t)1);
}

// An OFFSET in a file past 4 GiB (a dual-layer disc image) is that offset, not its low 32 bits; a number with junk after
// it is no number. Teeth: parse 32 bits (the poke lands at 0x10); stop at the junk (0x10 again).
TEST(bp_an_offset_past_4_gib_is_kept_whole)
{
    const uint64_t Size = (uint64_t(5) << 30);
    BinaryPatch::SparseImage S(Size, [](void *B, size_t N, uint64_t) -> long long { std::memset(B, 0x05, N); return (long long)N; });
    std::string Msg;
    CHECK(ApplyOne(nlohmann::ordered_json{{"MODE", "Poke"}, {"OFFSET", "0x100000010"}, {"EXPECT", "05"}, {"VALUE", "07"}}, S, NoVars, Msg) == Result::Applied);
    CHECK(S.Written().size() == 1 && S.Written().begin()->first == 0x100000010ull);
    CHECK(ApplyOne(nlohmann::ordered_json{{"MODE", "Poke"}, {"OFFSET", "0x10zz"}, {"VALUE", "07"}}, S, NoVars, Msg) == Result::Error);
    CHECK(ApplyOne(nlohmann::ordered_json{{"MODE", "Poke"}, {"OFFSET", "0x600000000"}, {"VALUE", "07"}}, S, NoVars, Msg) == Result::Error);
}

// Two ops naming one file by two spellings, on a mount that matches names whatever their case, patch one file: the
// second sees the first's bytes (two Or ops on one flag byte make both bits). Teeth: an image per spelling (one bit lost).
TEST(bp_two_spellings_of_one_file_are_planned_as_one)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_bp_spell";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Runtime = Root / "RUNTIME", Src = Root / "SRC";
    std::filesystem::create_directories(Runtime);
    std::filesystem::create_directories(Src);
    { std::ofstream O(Src / "Flags.bin", std::ios::binary); O << std::string(16, '\0'); }
    nlohmann::ordered_json Spec = { {"mountpoint", Runtime.string()}, {"caseInsensitive", true},
        {"layers", nlohmann::ordered_json::array({ {{"type", "file"}, {"source", (Src / "Flags.bin").string()}, {"target", "data"}} })} };
    auto Op = [](const char *F, const char *V) {
        return nlohmann::ordered_json{{"TYPE","BinaryPatch"},{"FILE",F},{"MODE","Or"},{"OFFSET","4"},{"VALUE",V}}; };
    ContainerParams CP(Root / "PKG");
    CP.RuntimePath = Runtime;
    CP.SubComponentsArray = nlohmann::ordered_json::array({ Op("data/Flags.bin", "01"), Op("DATA/flags.BIN", "02") });
    BinaryPatch::PlanReadTime(CP, Spec);
    CHECK(Spec.contains("patches") && Spec["patches"].size() == 1);
    if (Spec.contains("patches") && Spec["patches"].size() == 1) CHECK_EQ(Spec["patches"][0].value("bytes", std::string()), std::string("03"));
    CHECK_EQ(CP.ReadTimePatched.size(), (size_t)2);
    std::filesystem::remove_all(Root);
}
