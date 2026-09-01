#include "binarypatch.h"
#include "commonutils.h"   // Log*
#include "varsubst.h"      // field-level %token% render (Poke VALUE etc.)

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace
{
// ---- hex helpers -------------------------------------------------------------------------------------------
// Parse a hex string ("c705 d0 7d" — whitespace ignored) into bytes. Returns nullopt on a non-hex / odd-length
// string so a typo in a package fails loud instead of silently patching garbage.
std::optional<std::vector<uint8_t>> ParseHex(const std::string &In)
{
    std::string h;
    for (char c : In) if (!std::isspace(static_cast<unsigned char>(c))) h += c;
    if (h.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2)
    {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Parse a signature with `??` wildcards ("8b ?? 2d ??") into bytes + a same-length mask (true = must match).
struct Pattern { std::vector<uint8_t> Bytes; std::vector<bool> Mask; };
std::optional<Pattern> ParsePattern(const std::string &In)
{
    Pattern p;
    std::istringstream ss(In);
    std::string tok;
    while (ss >> tok)
    {
        if (tok == "??" || tok == "?")
        { p.Bytes.push_back(0); p.Mask.push_back(false); continue; }
        if (tok.size() != 2) return std::nullopt;
        auto b = ParseHex(tok);
        if (!b || b->size() != 1) return std::nullopt;
        p.Bytes.push_back((*b)[0]); p.Mask.push_back(true);
    }
    return p.Bytes.empty() ? std::nullopt : std::optional<Pattern>(p);
}

std::string ToHex(const uint8_t *p, size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 0xF]; }
    return s;
}

std::string VaStr(uint32_t va)
{
    char b[11];
    std::snprintf(b, sizeof(b), "0x%08x", va);
    return b;
}

// ---- minimal PE section table (VA <-> file offset) ---------------------------------------------------------
struct Section { uint32_t Va, VSize, RawPtr, RawSize, Chars; };
struct PeInfo  { uint32_t ImageBase = 0; std::vector<Section> Sections; };

std::optional<PeInfo> ParsePe(const std::vector<uint8_t> &d)
{
    auto rd32 = [&](size_t o) -> uint32_t {
        return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24);
    };
    auto rd16 = [&](size_t o) -> uint16_t { return uint16_t(d[o] | (d[o + 1] << 8)); };
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return std::nullopt;
    const uint32_t pe = rd32(0x3C);
    if (pe + 24 > d.size() || d[pe] != 'P' || d[pe + 1] != 'E') return std::nullopt;
    const uint16_t nsec    = rd16(pe + 6);
    const uint16_t optSize = rd16(pe + 20);
    const size_t   opt     = pe + 24;
    const uint16_t magic   = rd16(opt);
    PeInfo info;
    info.ImageBase = (magic == 0x20B) ? rd32(opt + 24) : rd32(opt + 28);   // PE32+ vs PE32
    const size_t secTab = opt + optSize;
    for (uint16_t i = 0; i < nsec; ++i)
    {
        const size_t o = secTab + size_t(i) * 40;
        if (o + 40 > d.size()) return std::nullopt;
        info.Sections.push_back({ rd32(o + 12), rd32(o + 8), rd32(o + 20), rd32(o + 16), rd32(o + 36) });
    }
    return info;
}

// A VA (image-base-relative absolute) → file offset, using the section it falls in.
std::optional<size_t> VaToOff(const PeInfo &pe, uint32_t va)
{
    const uint32_t rva = va - pe.ImageBase;
    for (const auto &s : pe.Sections)
        if (rva >= s.Va && rva < s.Va + std::max(s.VSize, s.RawSize))
            return size_t(rva - s.Va + s.RawPtr);
    return std::nullopt;
}
std::optional<uint32_t> OffToVa(const PeInfo &pe, size_t off)
{
    for (const auto &s : pe.Sections)
        if (off >= s.RawPtr && off < s.RawPtr + s.RawSize)
            return uint32_t(off - s.RawPtr + s.Va + pe.ImageBase);
    return std::nullopt;
}

uint32_t ParseNum(const std::string &s)
{
    return uint32_t(std::stoul(s, nullptr, (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) ? 16 : 10));
}

// Resolve the patch SITE (file offset) from ANCHOR (unique signature search) or OFFSET (VA or raw offset).
// Sets Err on failure. Returns nullopt on any error.
std::optional<size_t> ResolveSite(const nlohmann::ordered_json &P, const std::vector<uint8_t> &img,
                                  const PeInfo &pe, std::string &Err)
{
    if (P.contains("ANCHOR"))
    {
        auto pat = ParsePattern(P.value("ANCHOR", std::string()));
        if (!pat) { Err = "ANCHOR is not a valid hex signature"; return std::nullopt; }
        const size_t n = pat->Bytes.size();
        size_t found = std::string::npos, count = 0;
        for (size_t i = 0; i + n <= img.size(); ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (pat->Mask[j] && img[i + j] != pat->Bytes[j]) { ok = false; break; }
            if (ok) { if (count == 0) found = i; ++count; if (count > 1) break; }
        }
        if (count == 0) { Err = "ANCHOR matched nothing"; return std::nullopt; }
        if (count > 1)  { Err = "ANCHOR is ambiguous (matches >1 site) — make it longer"; return std::nullopt; }
        return found;
    }
    if (P.contains("OFFSET"))
    {
        uint32_t v;
        try { v = ParseNum(P.value("OFFSET", std::string())); }
        catch (...) { Err = "OFFSET is not a number"; return std::nullopt; }
        if (v >= pe.ImageBase)   // looks like a VA
        {
            auto off = VaToOff(pe, v);
            if (!off) { Err = "OFFSET VA 0x" + P.value("OFFSET", std::string()) + " is outside every section"; return std::nullopt; }
            return off;
        }
        return size_t(v);        // raw file offset
    }
    Err = "neither ANCHOR nor OFFSET given";
    return std::nullopt;
}

// The 5-byte relative jmp E9 encoding: from `from_va` (address of the jmp itself) to `to_va`.
std::array<uint8_t, 5> JmpRel32(uint32_t from_va, uint32_t to_va)
{
    const uint32_t rel = to_va - (from_va + 5);
    return { 0xE9, uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24) };
}
}  // namespace

BinaryPatch::Result BinaryPatch::ApplyOne(const nlohmann::ordered_json &Patch, std::vector<uint8_t> &Image,
                                          const std::map<std::string, std::string> &Vars, std::string &Msg)
{
    const std::string Mode = Patch.value("MODE", std::string());

    auto pe = ParsePe(Image);
    if (!pe) { Msg = "target is not a PE image"; return Result::Error; }

    std::string Err;
    auto siteOpt = ResolveSite(Patch, Image, *pe, Err);
    if (!siteOpt) { Msg = Err; return Result::Error; }
    const size_t site = *siteOpt;

    // EXPECT: the guard. Required for Replace/Cave (also defines the region/displaced length); optional for Poke.
    std::vector<uint8_t> expect;
    if (Patch.contains("EXPECT"))
    {
        auto e = ParseHex(Patch.value("EXPECT", std::string()));
        if (!e) { Msg = "EXPECT is not valid hex"; return Result::Error; }
        expect = *e;
        if (site + expect.size() > Image.size()) { Msg = "EXPECT runs past end of file"; return Result::Error; }
    }

    auto RenderField = [&](const char *K) {
        std::string v = Patch.value(K, std::string());
        VarSubst::StringVariableSubstitution(v, Vars);
        return v;
    };

    // ---- Replace / Poke: overwrite bytes at the site -------------------------------------------------------
    if (Mode == "Replace" || Mode == "Poke")
    {
        auto bytesOpt = ParseHex(RenderField(Mode == "Replace" ? "REPLACE" : "VALUE"));
        if (!bytesOpt) { Msg = std::string(Mode == "Replace" ? "REPLACE" : "VALUE") + " is not valid hex"; return Result::Error; }
        const std::vector<uint8_t> &newBytes = *bytesOpt;

        // Region length: EXPECT length for Replace (NOP-pad a shorter REPLACE), else the written length.
        const size_t region = !expect.empty() ? expect.size() : newBytes.size();
        if (newBytes.size() > region) { Msg = "REPLACE/VALUE is longer than EXPECT region"; return Result::Error; }
        if (site + region > Image.size()) { Msg = "patch region runs past end of file"; return Result::Error; }

        // Idempotency + guard, checked against the EXACT bytes we would write (REPLACE + NOP pad).
        std::vector<uint8_t> want(newBytes);
        want.resize(region, 0x90);
        if (std::equal(want.begin(), want.end(), Image.begin() + site))
        { Msg = "already patched at " + VaStr(OffToVa(*pe, site).value_or(0)); return Result::Skipped; }
        if (!expect.empty() && !std::equal(expect.begin(), expect.end(), Image.begin() + site))
        {
            Msg = "EXPECT mismatch: file has " + ToHex(Image.data() + site, expect.size())
                + " not " + ToHex(expect.data(), expect.size()) + " (wrong or foreign-patched binary)";
            return Result::Error;
        }
        std::copy(want.begin(), want.end(), Image.begin() + site);
        Msg = Mode + " " + std::to_string(region) + " byte(s)";
        return Result::Applied;
    }

    // ---- Cave: jmp trampoline (generalises woxl_blitqueue_patch.py) ----------------------------------------
    if (Mode == "Cave")
    {
        if (expect.size() < 5) { Msg = "Cave EXPECT must be >=5 bytes (room for a jmp rel32)"; return Result::Error; }
        auto payloadOpt = ParseHex(RenderField("PAYLOAD"));
        if (!payloadOpt) { Msg = "PAYLOAD is not valid hex"; return Result::Error; }
        const std::vector<uint8_t> &payload = *payloadOpt;

        // Already-patched signature: our site stub is `E9 rel32` + NOP pad to len(EXPECT). Detect and skip so a
        // persisted writelayer re-launch is a no-op (auto-cave discovery would otherwise refuse the non-zero cave).
        if (Image[site] == 0xE9 &&
            std::all_of(Image.begin() + site + 5, Image.begin() + site + expect.size(),
                        [](uint8_t b) { return b == 0x90; }))
        { Msg = "already caved"; return Result::Skipped; }
        if (!std::equal(expect.begin(), expect.end(), Image.begin() + site))
        {
            Msg = "EXPECT mismatch at cave site: file has " + ToHex(Image.data() + site, expect.size())
                + " not " + ToHex(expect.data(), expect.size());
            return Result::Error;
        }

        const size_t caveLen = payload.size() + expect.size() + 5;   // payload + displaced + jmp-back

        // Locate the cave: a fixed VA, or "auto" = the first zero run of caveLen in an executable section.
        const std::string caveSpec = Patch.value("CAVE", std::string("auto"));
        size_t caveOff = std::string::npos;
        if (caveSpec.empty() || caveSpec == "auto")
        {
            for (const auto &s : pe->Sections)
            {
                if (!(s.Chars & 0x20000000u)) continue;             // IMAGE_SCN_MEM_EXECUTE
                const size_t begin = s.RawPtr, end = size_t(s.RawPtr) + s.RawSize;
                size_t run = 0;
                for (size_t i = begin; i < end && i < Image.size(); ++i)
                {
                    if (Image[i] == 0) { if (++run >= caveLen) { caveOff = i - caveLen + 1; break; } }
                    else run = 0;
                }
                if (caveOff != std::string::npos) break;
            }
            if (caveOff == std::string::npos)
            { Msg = "no zero-run cave of " + std::to_string(caveLen) + " bytes in any executable section"; return Result::Error; }
        }
        else
        {
            uint32_t v;
            try { v = ParseNum(caveSpec); } catch (...) { Msg = "CAVE is not 'auto' or a number"; return Result::Error; }
            auto off = (v >= pe->ImageBase) ? VaToOff(*pe, v) : std::optional<size_t>(v);
            if (!off) { Msg = "CAVE VA outside every section"; return Result::Error; }
            caveOff = *off;
            if (caveOff + caveLen > Image.size()) { Msg = "CAVE runs past end of file"; return Result::Error; }
            if (std::any_of(Image.begin() + caveOff, Image.begin() + caveOff + caveLen, [](uint8_t b) { return b != 0; }))
            { Msg = "CAVE at the given address is not free (non-zero)"; return Result::Error; }
        }

        const uint32_t siteVa = *OffToVa(*pe, site);
        const uint32_t caveVa = *OffToVa(*pe, caveOff);

        // Build the cave body: PAYLOAD, then the displaced original bytes, then jmp back to just past the stub.
        std::vector<uint8_t> cave = payload;
        cave.insert(cave.end(), expect.begin(), expect.end());
        const auto back = JmpRel32(caveVa + uint32_t(payload.size() + expect.size()), siteVa + uint32_t(expect.size()));
        cave.insert(cave.end(), back.begin(), back.end());
        std::copy(cave.begin(), cave.end(), Image.begin() + caveOff);

        // Site stub: jmp cave, then NOP-fill the rest of the displaced region.
        const auto stub = JmpRel32(siteVa, caveVa);
        std::copy(stub.begin(), stub.end(), Image.begin() + site);
        std::fill(Image.begin() + site + 5, Image.begin() + site + expect.size(), uint8_t(0x90));

        Msg = "caved " + std::to_string(caveLen) + " bytes at " + VaStr(caveVa)
            + " (displaced " + std::to_string(expect.size()) + " at " + VaStr(siteVa) + ")";
        return Result::Applied;
    }

    Msg = "unknown MODE '" + Mode + "'";
    return Result::Error;
}

bool BinaryPatch::ProcessBinaryPatches(struct ContainerParams &ContainerParams)
{
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    bool Ok = true;
    size_t Attempted = 0, Applied = 0, Skipped = 0, Failed = 0;

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (Sub.value("TYPE", std::string()) != "BinaryPatch") continue;
        // APPLY=="memory" is handled by vglobby at process-start (the only launch path holding a child handle);
        // "prefix" (default) is on-disk into the mounted writelayer.
        if (Sub.value("APPLY", std::string("prefix")) == "memory") continue;
        ++Attempted;

        std::string File = Sub.value("FILE", std::string());
        VarSubst::StringVariableSubstitution(File, Vars);
        if (std::filesystem::path(File).is_absolute())
            LogWarn("BinaryPatch::ProcessBinaryPatches",
                    "FILE '" + File + "' is ABSOLUTE — it escapes the runtime mount root; author it relative.");
        const std::filesystem::path FilePath = ContainerParams.RuntimePath / File;

        std::vector<uint8_t> Image;
        {
            std::ifstream In(FilePath, std::ios::binary);
            if (!In) { LogErr("BinaryPatch::ProcessBinaryPatches", "cannot open '" + FilePath.string() + "'"); ++Failed; Ok = false; continue; }
            Image.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
        }

        std::string Msg;
        const Result R = ApplyOne(Sub, Image, Vars, Msg);
        if (R == Result::Error)
        {
            ++Failed; Ok = false;
            LogErr("BinaryPatch::ProcessBinaryPatches",
                   "BinaryPatch FAILED — mode=" + Sub.value("MODE", std::string("(none)"))
                   + " file='" + File + "': " + Msg);
            continue;
        }
        if (R == Result::Skipped) { ++Skipped; LogOut("BinaryPatch::ProcessBinaryPatches", File + ": " + Msg); continue; }

        // Applied — write it back (COW into the writelayer; the pristine content zip is untouched).
        std::ofstream Out(FilePath, std::ios::binary | std::ios::trunc);
        if (!Out) { LogErr("BinaryPatch::ProcessBinaryPatches", "cannot write '" + FilePath.string() + "'"); ++Failed; Ok = false; continue; }
        Out.write(reinterpret_cast<const char *>(Image.data()), std::streamsize(Image.size()));
        ++Applied;
        LogOut("BinaryPatch::ProcessBinaryPatches", File + ": " + Msg);
    }

    if (Attempted > 0)
        LogOut("BinaryPatch::ProcessBinaryPatches",
               std::to_string(Applied) + " applied, " + std::to_string(Skipped) + " already-patched, "
               + std::to_string(Failed) + " FAILED of " + std::to_string(Attempted) + " patch(es)");
    return Ok;
}
