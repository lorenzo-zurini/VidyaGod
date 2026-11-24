#include "binarypatch.h"
#include "fileedits.h"   // FileEdits::PathEscapesBase — one copy of the escape check, not two
#include "commonutils.h"   // Log*
#include "varsubst.h"      // field-level %token% render (Poke VALUE etc.)
#include "layerspec.h"     // VidyaGodFS: the mount's spec — PlanReadTime opens the layers as the mount will
#include "overlay.h"
#include "vfsops.h"
#include "hostio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>   // boyer_moore_horspool_searcher
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
struct PeInfo  { uint32_t ImageBase = 0; std::vector<Section> Sections; bool Real = false; };   // Real: a PE image (else a flat file)

using BinaryPatch::Image;

//N bytes at Off, or nullopt past the end or on a read error.
std::optional<std::vector<uint8_t>> ReadAt(const Image &Img, uint64_t Off, size_t N)
{
    std::vector<uint8_t> Out(N);
    if (N && !Img.Read(Off, N, Out.data())) return std::nullopt;
    return Out;
}

std::optional<PeInfo> ParsePe(const Image &Img)
{
    const auto Hdr = ReadAt(Img, 0, 0x40);
    if (!Hdr || (*Hdr)[0] != 'M' || (*Hdr)[1] != 'Z') return std::nullopt;
    auto rd32 = [](const uint8_t *d) { return uint32_t(d[0] | (d[1] << 8) | (d[2] << 16) | (uint32_t(d[3]) << 24)); };
    auto rd16 = [](const uint8_t *d) { return uint16_t(d[0] | (d[1] << 8)); };
    const uint32_t pe = rd32(Hdr->data() + 0x3C);
    const auto Nt = ReadAt(Img, pe, 24 + 2);                    // signature + file header + the optional header's magic
    if (!Nt || (*Nt)[0] != 'P' || (*Nt)[1] != 'E') return std::nullopt;
    const uint16_t nsec    = rd16(Nt->data() + 6);
    const uint16_t optSize = rd16(Nt->data() + 20);
    const uint64_t opt     = uint64_t(pe) + 24;
    const uint16_t magic   = rd16(Nt->data() + 24);
    const auto Base = ReadAt(Img, opt + (magic == 0x20B ? 24 : 28), 4);   // PE32+ vs PE32
    if (!Base) return std::nullopt;
    PeInfo info;
    info.ImageBase = rd32(Base->data());
    info.Real = true;
    const auto Tab = ReadAt(Img, opt + optSize, size_t(nsec) * 40);
    if (!Tab) return std::nullopt;
    for (uint16_t i = 0; i < nsec; ++i)
    {
        const uint8_t *o = Tab->data() + size_t(i) * 40;
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

//A whole number, hex ("0x84924") or decimal, all of it: 64 bits (an offset in a disc image of gigabytes), never cut short.
uint64_t ParseNum(const std::string &s)
{
    size_t Used = 0;
    const uint64_t V = std::stoull(s, &Used, (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) ? 16 : 10);
    if (Used != s.size()) throw std::invalid_argument(s);
    return V;
}
//A virtual address in a PE image: at or above its image base, within 32 bits. Anything else is a raw file offset.
bool IsVa(const PeInfo &pe, uint64_t v) { return pe.Real && v >= pe.ImageBase && v <= 0xFFFFFFFFull; }

//The whole file once, a chunk at a time, each chunk read with the Overlap bytes after it: Fn(Base, Bytes, N, Starts)
//sees the file's bytes [Base, Base + N), and a match of up to Overlap + 1 bytes starting at one of the first Starts
//of them lies wholly inside — every start is offered once. False on a read error or when Fn says stop.
bool ForEachChunk(const Image &Img, size_t Overlap, const std::function<bool(uint64_t, const uint8_t *, size_t, size_t)> &Fn, bool &ReadOk)
{
    ReadOk = true;
    const uint64_t Size = Img.Size();
    const size_t Chunk = std::max<size_t>(1, Img.ScanChunk());
    std::vector<uint8_t> Buf;
    for (uint64_t Base = 0; Base < Size; Base += Chunk)
    {
        const size_t N = size_t(std::min<uint64_t>(uint64_t(Chunk) + Overlap, Size - Base));
        Buf.resize(N);
        if (!Img.Read(Base, N, Buf.data())) { ReadOk = false; return false; }
        if (!Fn(Base, Buf.data(), N, size_t(std::min<uint64_t>(Chunk, Size - Base)))) return false;
    }
    return true;
}

//Where Pat (with its mask: false = any byte) starts, in order; at most Limit of them. False on a read error.
bool FindMasked(const Image &Img, const Pattern &Pat, size_t Limit, std::vector<uint64_t> &At)
{
    const size_t n = Pat.Bytes.size();
    size_t First = 0;                                     // the first byte that must match: found with memchr
    while (First < n && !Pat.Mask[First]) ++First;
    bool ReadOk = true;
    ForEachChunk(Img, n - 1, [&](uint64_t Base, const uint8_t *B, size_t N, size_t Starts) {
        if (N < n) return true;
        const size_t Last = std::min(Starts - 1, N - n);   // the last start this chunk offers
        for (size_t i = 0; i <= Last;)
        {
            if (First < n)
            {
                const void *Hit = std::memchr(B + i + First, Pat.Bytes[First], Last - i + 1);
                if (!Hit) break;
                i = size_t(static_cast<const uint8_t *>(Hit) - B) - First;
            }
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
                if (Pat.Mask[j] && B[i + j] != Pat.Bytes[j]) { ok = false; break; }
            if (ok) { At.push_back(Base + i); if (At.size() >= Limit) return false; }
            ++i;
        }
        return true;
    }, ReadOk);
    return ReadOk;
}

//Where Pat starts, in order, never overlapping (a match resumes the search after itself); at most Limit. False on a
//read error.
bool FindExact(const Image &Img, const std::vector<uint8_t> &Pat, size_t Limit, std::vector<uint64_t> &At)
{
    const size_t n = Pat.size();
    uint64_t Next = 0;                                    // the first start the matches so far leave free
    const std::boyer_moore_horspool_searcher Find(Pat.begin(), Pat.end());
    bool ReadOk = true;
    ForEachChunk(Img, n - 1, [&](uint64_t Base, const uint8_t *B, size_t N, size_t Starts) {
        const size_t End = std::min(N, Starts + n - 1);  // a match found here starts within this chunk
        size_t From = Next > Base ? size_t(std::min<uint64_t>(Next - Base, End)) : 0;
        while (From < End)
        {
            const uint8_t *Hit = std::search(B + From, B + End, Find);
            if (Hit == B + End) break;
            const size_t i = size_t(Hit - B);
            At.push_back(Base + i);
            if (At.size() >= Limit) return false;
            Next = Base + i + n;
            From = i + n;
        }
        return true;
    }, ReadOk);
    return ReadOk;
}

// Resolve the patch SITE (file offset) from ANCHOR (unique signature search) or OFFSET (VA or raw offset).
// Sets Err on failure. Returns nullopt on any error.
std::optional<size_t> ResolveSite(const nlohmann::ordered_json &P, const Image &img, const PeInfo &pe, std::string &Err)
{
    if (P.contains("ANCHOR"))
    {
        auto pat = ParsePattern(P.value("ANCHOR", std::string()));
        if (!pat) { Err = "ANCHOR is not a valid hex signature"; return std::nullopt; }
        std::vector<uint64_t> At;
        if (!FindMasked(img, *pat, 2, At)) { Err = "the file could not be read"; return std::nullopt; }
        if (At.empty())   { Err = "ANCHOR matched nothing"; return std::nullopt; }
        if (At.size() > 1) { Err = "ANCHOR is ambiguous (matches >1 site) — make it longer"; return std::nullopt; }
        return size_t(At[0]);
    }
    if (P.contains("OFFSET"))
    {
        uint64_t v;
        try { v = ParseNum(P.value("OFFSET", std::string())); }
        catch (...) { Err = "OFFSET is not a number"; return std::nullopt; }
        if (IsVa(pe, v))         // a VA
        {
            auto off = VaToOff(pe, uint32_t(v));
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

bool BinaryPatch::MemoryImage::Read(uint64_t Off, size_t N, uint8_t *Out) const
{
    if (Off > Bytes.size() || N > Bytes.size() - Off) return false;
    std::copy_n(Bytes.begin() + std::ptrdiff_t(Off), N, Out);
    return true;
}

void BinaryPatch::MemoryImage::Write(uint64_t Off, const uint8_t *P, size_t N)
{
    std::copy_n(P, N, Bytes.begin() + std::ptrdiff_t(Off));
}

bool BinaryPatch::SparseImage::Read(uint64_t Off, size_t N, uint8_t *Out) const
{
    if (Off > Bytes || N > Bytes - Off || !From) return false;
    for (size_t Got = 0; Got < N;)
    {
        const long long R = From(Out + Got, N - Got, Off + Got);
        if (R <= 0) return false;
        Got += size_t(R);
    }
    //What the patches so far wrote, over what the file holds.
    auto It = Runs.upper_bound(Off);
    if (It != Runs.begin()) --It;
    for (; It != Runs.end() && It->first < Off + N; ++It)
    {
        const uint64_t RunEnd = It->first + It->second.size();
        if (RunEnd <= Off) continue;
        const uint64_t A = std::max(Off, It->first), B = std::min(Off + N, RunEnd);
        std::copy_n(It->second.begin() + std::ptrdiff_t(A - It->first), size_t(B - A), Out + (A - Off));
    }
    return true;
}

void BinaryPatch::SparseImage::Write(uint64_t Off, const uint8_t *P, size_t N)
{
    if (N == 0) return;
    //Into the run that reaches Off (extended in place: writes end to end — a ReplaceAll's adjoining matches — grow one
    //run, amortised, never copy it again), else a new run; then every later run the new bytes reach or adjoin is
    //folded in, its bytes beyond the new ones kept.
    const uint64_t End = Off + N;
    auto Run = Runs.upper_bound(Off);
    if (Run != Runs.begin() && std::prev(Run)->first + std::prev(Run)->second.size() >= Off) --Run;
    else Run = Runs.emplace(Off, std::vector<uint8_t>()).first;
    std::vector<uint8_t> &V = Run->second;
    const uint64_t Base = Run->first;
    if (V.size() < End - Base) V.resize(size_t(End - Base));
    std::copy_n(P, N, V.begin() + std::ptrdiff_t(Off - Base));
    for (auto Next = std::next(Run); Next != Runs.end() && Next->first <= Base + V.size();)
    {
        const uint64_t NextEnd = Next->first + Next->second.size();
        if (NextEnd > Base + V.size())
        {
            const uint64_t From = Base + V.size();               // its bytes past what the run already holds
            V.insert(V.end(), Next->second.begin() + std::ptrdiff_t(From - Next->first), Next->second.end());
        }
        Next = Runs.erase(Next);
    }
}

BinaryPatch::Result BinaryPatch::ApplyOne(const nlohmann::ordered_json &Patch, std::vector<uint8_t> &Bytes,
                                          const std::map<std::string, std::string> &Vars, std::string &Msg)
{
    MemoryImage Img(Bytes);
    return ApplyOne(Patch, Img, Vars, Msg);
}

BinaryPatch::Result BinaryPatch::ApplyOne(const nlohmann::ordered_json &Patch, Image &Img,
                                          const std::map<std::string, std::string> &Vars, std::string &Msg)
{
    const std::string Mode = Patch.value("MODE", std::string());
    const uint64_t Size = Img.Size();
    auto At = [&](uint64_t Off, size_t N) { return ReadAt(Img, Off, N); };
    auto Put = [&](uint64_t Off, const std::vector<uint8_t> &B) { Img.Write(Off, B.data(), B.size()); };

    //A file that is not a PE image (an ELF, a data file) has no image base and no sections: ANCHOR and a raw OFFSET
    //still name a site, and only a Cave (which needs an executable section and virtual addresses) refuses it.
    const std::optional<PeInfo> Parsed = ParsePe(Img);
    const bool IsPe = Parsed.has_value();
    const PeInfo Flat{};                      // not a PE: every OFFSET is a raw file offset
    const PeInfo *pe = IsPe ? &*Parsed : &Flat;

    // ---- ReplaceAll: every occurrence of a pattern -------------------------------------------------------------
    // A fix that rewrites one header wherever it occurs (a sound archive's thousands of WAV headers): no site, the
    // EXPECT bytes are the pattern and every match becomes REPLACE, of the same length. Ops run in order, so a later
    // one may match what an earlier one wrote (as the fix's own tool did). None left but the result there = skipped.
    if (Mode == "ReplaceAll")
    {
        if (Patch.contains("ANCHOR") || Patch.contains("OFFSET")) { Msg = "ReplaceAll takes no ANCHOR or OFFSET (it patches every match of EXPECT)"; return Result::Error; }
        std::string Rep = Patch.value("REPLACE", std::string());
        VarSubst::StringVariableSubstitution(Rep, Vars);
        auto e = ParseHex(Patch.value("EXPECT", std::string()));
        auto r = ParseHex(Rep);
        if (!e || e->empty()) { Msg = "EXPECT (the pattern) is not valid hex"; return Result::Error; }
        if (!r) { Msg = "REPLACE is not valid hex"; return Result::Error; }
        if (r->size() != e->size()) { Msg = "REPLACE must be as long as EXPECT"; return Result::Error; }
        if (*r == *e) { Msg = "REPLACE equals EXPECT"; return Result::Error; }
        std::vector<uint64_t> Found;
        if (!FindExact(Img, *e, SIZE_MAX, Found)) { Msg = "the file could not be read"; return Result::Error; }
        if (Found.empty())
        {
            std::vector<uint64_t> Done;
            if (!FindExact(Img, *r, 1, Done)) { Msg = "the file could not be read"; return Result::Error; }
            if (!Done.empty()) { Msg = "already patched (no " + ToHex(e->data(), std::min<size_t>(e->size(), 8)) + ".. left)"; return Result::Skipped; }
            Msg = "EXPECT matched nothing (wrong or foreign-patched file)";
            return Result::Error;
        }
        for (const uint64_t A : Found) Put(A, *r);
        Msg = "ReplaceAll " + std::to_string(Found.size()) + " match(es)";
        return Result::Applied;
    }

    std::string Err;
    auto siteOpt = ResolveSite(Patch, Img, *pe, Err);
    if (!siteOpt) { Msg = Err; return Result::Error; }
    const size_t site = *siteOpt;

    // EXPECT: the guard. Required for Replace/Cave (also defines the region/displaced length); optional for Poke.
    std::vector<uint8_t> expect;
    if (Patch.contains("EXPECT"))
    {
        auto e = ParseHex(Patch.value("EXPECT", std::string()));
        if (!e) { Msg = "EXPECT is not valid hex"; return Result::Error; }
        expect = *e;
        if (site > Size || expect.size() > Size - site) { Msg = "EXPECT runs past end of file"; return Result::Error; }
    }

    auto RenderField = [&](const char *K) {
        std::string v = Patch.value(K, std::string());
        VarSubst::StringVariableSubstitution(v, Vars);
        return v;
    };
    auto Where = [&] { return IsPe ? VaStr(OffToVa(*pe, site).value_or(0)) : "offset " + std::to_string(site); };
    const auto Unreadable = [&] { Msg = "the file could not be read at " + Where(); return Result::Error; };

    // ---- Or: set bits at the site ----------------------------------------------------------------------------
    // A flag byte several options share (UserPatch keeps six installer checkboxes as bits of one byte): each option
    // ORs its own bits in, so any combination composes in any order. No EXPECT — the other options' bits may already
    // be there; bits already set are the idempotent skip.
    if (Mode == "Or")
    {
        if (!expect.empty()) { Msg = "Or takes no EXPECT (the byte also carries other options' bits)"; return Result::Error; }
        auto bitsOpt = ParseHex(RenderField("VALUE"));
        if (!bitsOpt || bitsOpt->empty()) { Msg = "VALUE is not valid hex"; return Result::Error; }
        const std::vector<uint8_t> &bits = *bitsOpt;
        if (site > Size || bits.size() > Size - site) { Msg = "patch region runs past end of file"; return Result::Error; }
        auto Have = At(site, bits.size());
        if (!Have) return Unreadable();
        bool set = true;
        for (size_t i = 0; i < bits.size(); ++i) if (((*Have)[i] & bits[i]) != bits[i]) set = false;
        if (set) { Msg = "bits already set at " + Where(); return Result::Skipped; }
        for (size_t i = 0; i < bits.size(); ++i) (*Have)[i] |= bits[i];
        Put(site, *Have);
        Msg = "Or " + std::to_string(bits.size()) + " byte(s)";
        return Result::Applied;
    }

    // ---- Replace / Poke: overwrite bytes at the site -------------------------------------------------------
    if (Mode == "Replace" || Mode == "Poke")
    {
        auto bytesOpt = ParseHex(RenderField(Mode == "Replace" ? "REPLACE" : "VALUE"));
        if (!bytesOpt) { Msg = std::string(Mode == "Replace" ? "REPLACE" : "VALUE") + " is not valid hex"; return Result::Error; }
        const std::vector<uint8_t> &newBytes = *bytesOpt;

        // Region length: EXPECT length for Replace (NOP-pad a shorter REPLACE), else the written length.
        const size_t region = !expect.empty() ? expect.size() : newBytes.size();
        if (newBytes.size() > region) { Msg = "REPLACE/VALUE is longer than EXPECT region"; return Result::Error; }
        //The 0x90 pad is an x86 NOP: code only. In a file that is not a PE (an ELF's strings, a data file) it would
        //write junk bytes, so a REPLACE there must cover its whole EXPECT.
        if (!IsPe && newBytes.size() < region) { Msg = "REPLACE is shorter than EXPECT in a file that is not a PE image (no NOP padding outside code): give it the full length"; return Result::Error; }
        if (site > Size || region > Size - site) { Msg = "patch region runs past end of file"; return Result::Error; }
        auto Have = At(site, region);
        if (!Have) return Unreadable();

        // Idempotency + guard, checked against the EXACT bytes we would write (REPLACE + NOP pad).
        std::vector<uint8_t> want(newBytes);
        want.resize(region, 0x90);
        if (*Have == want) { Msg = "already patched at " + Where(); return Result::Skipped; }
        if (!expect.empty() && *Have != expect)
        {
            Msg = "EXPECT mismatch: file has " + ToHex(Have->data(), expect.size())
                + " not " + ToHex(expect.data(), expect.size()) + " (wrong or foreign-patched binary)";
            return Result::Error;
        }
        Put(site, want);
        Msg = Mode + " " + std::to_string(region) + " byte(s)";
        return Result::Applied;
    }

    // ---- Cave: jmp trampoline (generalises woxl_blitqueue_patch.py) ----------------------------------------
    if (Mode == "Cave")
    {
        if (!IsPe) { Msg = "Cave needs a PE image (an executable section to hold the cave)"; return Result::Error; }
        if (expect.size() < 5) { Msg = "Cave EXPECT must be >=5 bytes (room for a jmp rel32)"; return Result::Error; }
        auto payloadOpt = ParseHex(RenderField("PAYLOAD"));
        if (!payloadOpt) { Msg = "PAYLOAD is not valid hex"; return Result::Error; }
        const std::vector<uint8_t> &payload = *payloadOpt;
        auto Have = At(site, expect.size());
        if (!Have) return Unreadable();

        // Already-patched signature: our site stub is `E9 rel32` + NOP pad to len(EXPECT). Detect and skip so a
        // persisted writelayer re-launch is a no-op (auto-cave discovery would otherwise refuse the non-zero cave).
        if ((*Have)[0] == 0xE9 && std::all_of(Have->begin() + 5, Have->end(), [](uint8_t b) { return b == 0x90; }))
        { Msg = "already caved"; return Result::Skipped; }
        if (*Have != expect)
        {
            Msg = "EXPECT mismatch at cave site: file has " + ToHex(Have->data(), expect.size())
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
                const uint64_t begin = std::min<uint64_t>(s.RawPtr, Size), end = std::min<uint64_t>(uint64_t(s.RawPtr) + s.RawSize, Size);
                auto Sec = At(begin, size_t(end - begin));
                if (!Sec) return Unreadable();
                size_t run = 0;
                for (size_t i = 0; i < Sec->size(); ++i)
                {
                    if ((*Sec)[i] == 0) { if (++run >= caveLen) { caveOff = size_t(begin) + i - caveLen + 1; break; } }
                    else run = 0;
                }
                if (caveOff != std::string::npos) break;
            }
            if (caveOff == std::string::npos)
            { Msg = "no zero-run cave of " + std::to_string(caveLen) + " bytes in any executable section"; return Result::Error; }
        }
        else
        {
            uint64_t v;
            try { v = ParseNum(caveSpec); } catch (...) { Msg = "CAVE is not 'auto' or a number"; return Result::Error; }
            auto off = IsVa(*pe, v) ? VaToOff(*pe, uint32_t(v)) : std::optional<size_t>(size_t(v));
            if (!off) { Msg = "CAVE VA outside every section"; return Result::Error; }
            caveOff = *off;
            if (caveOff > Size || caveLen > Size - caveOff) { Msg = "CAVE runs past end of file"; return Result::Error; }
            auto Free = At(caveOff, caveLen);
            if (!Free) return Unreadable();
            if (std::any_of(Free->begin(), Free->end(), [](uint8_t b) { return b != 0; }))
            { Msg = "CAVE at the given address is not free (non-zero)"; return Result::Error; }
        }

        //Both ends of the jumps need an address: a site or cave given as a file offset outside every section (in the
        //headers, in overlay data past the last section) has none.
        const std::optional<uint32_t> SiteVa = OffToVa(*pe, site), CaveVa = OffToVa(*pe, caveOff);
        if (!SiteVa) { Msg = "the cave site is outside every section"; return Result::Error; }
        if (!CaveVa) { Msg = "CAVE is outside every section"; return Result::Error; }
        const uint32_t siteVa = *SiteVa, caveVa = *CaveVa;

        // Build the cave body: PAYLOAD, then the displaced original bytes, then jmp back to just past the stub.
        std::vector<uint8_t> cave = payload;
        cave.insert(cave.end(), expect.begin(), expect.end());
        const auto back = JmpRel32(caveVa + uint32_t(payload.size() + expect.size()), siteVa + uint32_t(expect.size()));
        cave.insert(cave.end(), back.begin(), back.end());
        Put(caveOff, cave);

        // Site stub: jmp cave, then NOP-fill the rest of the displaced region.
        const auto stub = JmpRel32(siteVa, caveVa);
        std::vector<uint8_t> Site(expect.size(), uint8_t(0x90));
        std::copy(stub.begin(), stub.end(), Site.begin());
        Put(site, Site);

        Msg = "caved " + std::to_string(caveLen) + " bytes at " + VaStr(caveVa)
            + " (displaced " + std::to_string(expect.size()) + " at " + VaStr(siteVa) + ")";
        return Result::Applied;
    }

    Msg = "unknown MODE '" + Mode + "'";
    return Result::Error;
}



namespace
{
//Where a BinaryPatch's FILE lands under the runtime: nullopt when it is refused (it leaves the runtime, or runs through
//a link out of it). Report: say so (the pass after the mount); the plan before it only looks. Links: refuse one leading
//out of the runtime (a file the mount serves patched is never written, so its path is not asked).
std::optional<std::filesystem::path> PatchTarget(const ContainerParams &CP, const nlohmann::ordered_json &Sub,
                                                 const std::map<std::string, std::string> &Vars, bool Report, std::string &File,
                                                 bool Links = true)
{
    File = Sub.value("FILE", std::string());
    VarSubst::StringVariableSubstitution(File, Vars);
    //An edit path is ALWAYS relative to its pass base (spec ch.15), so a path that is not is re-anchored
    //rather than followed. The overwhelmingly common way to get one is the house style
    //"%PrefixRoot%/drive_c/..." under a runner where PrefixRoot is "" (native), which yields
    //"/drive_c/...": BasePath / File then discards the base entirely and the edit lands outside the
    //runtime. Worms 4 shipped exactly that and printed one line nobody read; the launch-matrix fixture
    //reproduced it with every edit failing at once.
    //
    //Tested with a LEADING-SEPARATOR check rather than is_absolute(), because is_absolute() is false for
    //"/drive_c/x" on WINDOWS (a root-directory with no root-name) — so the one case this exists for would
    //not have been caught there at all, while "C:/Users/..." would have been "re-anchored" by a strip loop
    //that removes nothing, logging a claim that never happened.
    {
        const std::string Before = File;
        //Strip ONLY when the absolute path does not already land inside the base. "%RuntimePath%/game/x"
        //substitutes to a real absolute path that IS inside it — stripping that one turned
        //"/home/.../RUNTIME/game/x" into "<base>/home/.../RUNTIME/game/x", a path that exists nowhere,
        //where before it resolved correctly. Only the PrefixRoot-is-empty shape ("/drive_c/...") needs
        //re-anchoring, and that one does escape.
        std::filesystem::path AlreadyInside;
        if (std::filesystem::path(File).is_absolute()
            && !FileEdits::PathEscapesBase(std::filesystem::path(File), CP.RuntimePath, AlreadyInside))
        {
            File = AlreadyInside.string();
        }
        else
        while (!File.empty() && (File.front() == '/' || File.front() == '\\')) File.erase(File.begin());
        if (Report && File != Before)
            LogWarn("BinaryPatch::ProcessBinaryPatches",
                    "FILE '" + Before + "' is root-relative — an edit path is relative to its base, so it "
                    "was re-anchored to '" + File + "' under the runtime mount root. Author it relative (a leading %variable% "
                    "that is empty on this runner is the usual cause).");
        else if (Report && std::filesystem::path(File).is_absolute())
            LogWarn("BinaryPatch::ProcessBinaryPatches",
                    "FILE '" + File + "' is ABSOLUTE and escapes the runtime mount root — the edit will not land where "
                    "anything reads it. Author it relative to the base.");
    }
    std::filesystem::path FilePath;
    if (FileEdits::PathEscapesBase(CP.RuntimePath / File, CP.RuntimePath, FilePath))
    {
        if (Report)
            LogErr("BinaryPatch::ProcessBinaryPatches",
                   "FILE '" + File + "' resolves OUTSIDE the runtime mount root ('"
                   + CP.RuntimePath.string() + "') — refused. A patch may only write inside it.");
        return std::nullopt;
    }
    if (std::string Why; Report && Links && FileEdits::LinkLeadsOut(CP.RuntimePath, FilePath, Why))
    {
        LogErr("BinaryPatch::ProcessBinaryPatches", "FILE '" + File + "' is not patched: " + Why
               + " — a patch never writes through a link (a game can leave one in a kept folder).");
        return std::nullopt;
    }
    return FilePath;
}

bool PrefixPatch(const nlohmann::ordered_json &Sub)
{
    // APPLY=="memory" is handled by vglobby at process-start (the only launch path holding a child handle);
    // "prefix" (default) is on the file.
    return Sub.value("TYPE", std::string()) == "BinaryPatch" && Sub.value("APPLY", std::string("prefix")) != "memory";
}

//An op that looks through the whole file: an ANCHOR (found, and found once), a ReplaceAll (every match).
bool Scans(const nlohmann::ordered_json &Sub)
{
    return Sub.contains("ANCHOR") || Sub.value("MODE", std::string()) == "ReplaceAll";
}

//A file a scanning op looks through is read once, whole, when it is no larger than this: each scan op reads the whole
//file (a second ANCHOR match makes it ambiguous; ReplaceAll finds every match), and nineteen ops over a 200 MB archive
//must not be nineteen passes. A larger one (a disc image) is scanned from the file, one pass per scan op.
constexpr uint64_t kHoldWhole = uint64_t(512) << 20;

//An image of a file Size bytes long read by R: held whole first when Whole (and small enough).
std::unique_ptr<BinaryPatch::SparseImage> ImageOf(uint64_t Size, BinaryPatch::SparseImage::Reader R, bool Whole)
{
    if (!Whole || Size > kHoldWhole) return std::make_unique<BinaryPatch::SparseImage>(Size, std::move(R));
    auto Bytes = std::make_shared<std::vector<uint8_t>>(size_t(Size));
    for (uint64_t Got = 0; Got < Size;)
    {
        const long long N = R(Bytes->data() + Got, size_t(Size - Got), Got);
        if (N <= 0) return std::make_unique<BinaryPatch::SparseImage>(Size, std::move(R));   // read where needed instead
        Got += uint64_t(N);
    }
    return std::make_unique<BinaryPatch::SparseImage>(Size, [Bytes](void *B, size_t N, uint64_t Off) -> long long {
        if (Off >= Bytes->size()) return 0;
        const size_t K = std::min<size_t>(N, Bytes->size() - size_t(Off));
        std::memcpy(B, Bytes->data() + Off, K);
        return (long long)K;
    });
}
}

void BinaryPatch::PlanReadTime(struct ContainerParams &CP, nlohmann::ordered_json &SpecJson,
                               std::map<std::filesystem::path, std::string> *Left)
{
    CP.ReadTimePatched.clear();
    const std::map<std::string, std::string> Vars = CP.GetVariablesMap();
    std::vector<std::pair<std::filesystem::path, const nlohmann::ordered_json *>> Ops;
    for (const auto &Sub : CP.SubComponentsArray)
    {
        if (!PrefixPatch(Sub)) continue;
        std::string File;
        if (const auto P = PatchTarget(CP, Sub, Vars, /*Report=*/false, File)) Ops.emplace_back(*P, &Sub);
    }
    if (Ops.empty()) return;

    //The files as the mount will serve them: the same spec, opened here. A writelayer that is not there yet holds no
    //copy, and is not made by looking.
    const auto Began = std::chrono::steady_clock::now();
    Spec S;
    std::string Err;
    VfsState V;
    if (ParseSpecText(SpecJson.dump(), S, Err))
    {
        std::error_code Ec;
        if (!S.writelayer.empty() && !std::filesystem::exists(S.writelayer, Ec)) S.writelayer.clear();
    }
    if (!Err.empty() || !V.Init(S, Err))
    {
        LogWarn("BinaryPatch::PlanReadTime", "the layers could not be opened to plan the patches (" + Err + ") — they are written after the mount");
        return;
    }
    //One target per file the mount serves: two spellings of one name on a mount that matches names whatever their
    //case are one file, and its ops see each other's bytes in their order.
    struct Target
    {
        std::string VPath;
        std::set<std::filesystem::path> Paths;          // the spellings the ops name it by
        OpenFile *Open = nullptr;
        std::unique_ptr<SparseImage> Img;
        std::string Why;                                // why it is left to the pass after the mount
        size_t Changed = 0, Already = 0;
    };
    std::map<std::string, Target> Files;
    std::map<std::string, bool> AnyScan;
    std::vector<std::pair<std::string, const nlohmann::ordered_json *>> Keyed;
    for (const auto &[Path, Sub] : Ops)
    {
        std::string VPath = NormalizeVPath(Path.lexically_relative(CP.RuntimePath).generic_string());
        if (S.caseInsensitive) VPath = CanonicalCase(V, VPath);
        const std::string Key = S.caseInsensitive ? FoldCase(VPath) : VPath;
        Target &T = Files[Key];
        if (T.VPath.empty()) T.VPath = VPath;
        T.Paths.insert(Path);
        AnyScan[Key] = AnyScan[Key] || Scans(*Sub);
        Keyed.emplace_back(Key, Sub);
    }
    std::map<std::string, size_t> LastOp;              // a target's last op: its whole-file copy is let go after it
    for (size_t I = 0; I < Keyed.size(); ++I) LastOp[Keyed[I].first] = I;
    for (size_t I = 0; I < Keyed.size(); ++I)
    {
        const auto &[Key, Sub] = Keyed[I];
        Target &T = Files[Key];
        const auto Done = [&] { if (LastOp[Key] == I && T.Img) T.Img->LetGo(); };
        if (!T.Why.empty()) continue;
        if (!T.Img)
        {
            //Only a file a lower layer serves, as its own bytes: a copy in the writelayer or a kept folder is the
            //user's, and a link is left to the pass after the mount (which refuses one leading out).
            const ResolveResult R = Resolve(V, T.VPath);
            HostIO::Stat St;
            VfsAttr A;
            if (T.VPath.empty() || R.kind == HitKind::None) T.Why = "not in the layers";
            else if (R.rwPassthrough || R.kind == HitKind::WriteLayer) T.Why = "a copy of its own serves it (the writelayer, a kept folder)";
            else if (R.kind == HitKind::ZipEntry ? (!R.zipEntry || R.zipEntry->isDir || R.zipEntry->symlink)
                     : R.kind == HitKind::ImplicitDir || HostIO::Lstat(R.hostPath, St) != 0 || St.isSymlink || St.isDir)
                T.Why = "not a plain file of a layer (a folder, a link)";
            else if (VfsGetattr(V, T.VPath, A) != 0 || A.isDir || VfsOpen(V, T.VPath, O_RDONLY, T.Open) != 0 || !T.Open)
                T.Why = "it could not be opened";
            if (!T.Why.empty()) continue;
            OpenFile *F = T.Open;
            T.Img = ImageOf(A.size, [F](void *B, size_t N, uint64_t Off) { return (long long)VfsRead(F, B, N, Off); }, AnyScan[Key]);
        }
        std::string Msg;
        const Result R = ApplyOne(*Sub, *T.Img, Vars, Msg);
        Done();
        if (R == Result::Error) { T.Why = "a patch on it failed (" + Msg + ")"; T.Img->LetGo(); continue; }   // reported by the pass after
        ++(R == Result::Applied ? T.Changed : T.Already);
        LogOut("BinaryPatch::PlanReadTime", T.VPath + ": " + Msg);
    }
    size_t Changed = 0, Already = 0, FilesPlanned = 0;
    for (auto &[Key, T] : Files)
    {
        if (T.Open) VfsRelease(T.Open);
        if (!T.Why.empty())
        {
            if (Left) for (const auto &P : T.Paths) (*Left)[P] = T.Why;
            continue;
        }
        if (!T.Img || T.Img->Written().empty()) continue;  // nothing to change: already patched in the file itself
        for (const auto &[At, B] : T.Img->Written())
            SpecJson["patches"].push_back({ {"path", T.VPath}, {"at", At}, {"bytes", ToHex(B.data(), B.size())} });
        for (const auto &P : T.Paths) CP.ReadTimePatched[P] = T.Img->Written();
        Changed += T.Changed;
        Already += T.Already;
        ++FilesPlanned;
    }
    LogOut("BinaryPatch::PlanReadTime", std::to_string(Changed) + " patch(es) on " + std::to_string(FilesPlanned)
           + " file(s) are served as the mount reads them, nothing copied (" + std::to_string(Already) + " already in the files; "
           + std::to_string(Files.size() - FilesPlanned) + " file(s) left to the pass after the mount; planned in "
           + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Began).count()) + " ms)");
}

bool BinaryPatch::ProcessBinaryPatches(struct ContainerParams &ContainerParams)
{
    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    bool Ok = true;
    size_t Attempted = 0, Applied = 0, Skipped = 0, Failed = 0, ReadTime = 0;

    //A file planned for the mount to serve patched (PlanReadTime): checked by reading the planned bytes through the
    //mount. Its ops are not run again — over a file already patched, an op whose site a later op patched again (two
    //Replaces in turn) finds neither its old bytes nor its result. One the mount serves otherwise (a copy of its own,
    //made after the plan: an override edit) is patched below like any file.
    std::set<std::filesystem::path> Served;
    for (const auto &[Path, Runs] : ContainerParams.ReadTimePatched)
    {
        std::ifstream F(Path, std::ios::binary);
        bool Same = static_cast<bool>(F);
        for (auto It = Runs.begin(); Same && It != Runs.end(); ++It)
        {
            std::vector<uint8_t> Got(It->second.size());
            F.seekg(std::streamoff(It->first));
            F.read(reinterpret_cast<char *>(Got.data()), std::streamsize(Got.size()));
            Same = F.gcount() == std::streamsize(Got.size()) && Got == It->second;
        }
        if (Same) Served.insert(Path);
        else LogWarn("BinaryPatch::ProcessBinaryPatches", Path.string() + " was planned for the mount to serve patched, but "
                     "it serves it otherwise (a copy of its own?) — its patches are written, copying the file");
    }

    //A file is read where its patches look (its header, the sites, a pass per scan op — or once, whole, when it is
    //small and an op scans), once for the run of ops on it, and only the bytes they change are written: a one-byte
    //fix to a disc image of gigabytes reads and writes bytes, not the image. Ops still apply in order, each seeing
    //what the ones before it wrote.
    std::filesystem::path OpenPath;
    std::unique_ptr<SparseImage> Img;
    size_t Pending = 0;                                  // ops applied to Img, not yet on disk
    auto Flush = [&]() {
        if (!Img) return;
        const std::map<uint64_t, std::vector<uint8_t>> Runs = Img->Written();
        Img.reset();                                     // its reader closed before the file is opened to write
        if (Pending == 0) return;
        //Written in place: COW into the writelayer, the pristine layer untouched.
        std::fstream Out(OpenPath, std::ios::binary | std::ios::in | std::ios::out);
        for (const auto &[At, B] : Runs)
        {
            if (!Out) break;
            Out.seekp(std::streamoff(At));
            Out.write(reinterpret_cast<const char *>(B.data()), std::streamsize(B.size()));
        }
        if (Out) Out.flush();
        if (!Out) { LogErr("BinaryPatch::ProcessBinaryPatches", "cannot write '" + OpenPath.string() + "'"); Failed += Pending; Applied -= Pending; Ok = false; }
        Pending = 0;
    };

    for (auto &Sub : ContainerParams.SubComponentsArray)
    {
        if (!PrefixPatch(Sub)) continue;
        ++Attempted;

        std::string File;
        //A file the mount serves patched passes before anything else is asked of it (a link it is reached through is
        //the mount's own, and is not written through).
        const std::optional<std::filesystem::path> Target = PatchTarget(ContainerParams, Sub, Vars, /*Report=*/true, File, /*Links=*/false);
        if (!Target) { ++Failed; Ok = false; continue; }
        if (Served.count(*Target)) { ++ReadTime; continue; }
        if (std::string Why; FileEdits::LinkLeadsOut(ContainerParams.RuntimePath, *Target, Why))
        {
            LogErr("BinaryPatch::ProcessBinaryPatches", "FILE '" + File + "' is not patched: " + Why
                   + " — a patch never writes through a link (a game can leave one in a kept folder).");
            ++Failed; Ok = false;
            continue;
        }
        const std::filesystem::path &FilePath = *Target;

        if (FilePath != OpenPath)
        {
            Flush();
            OpenPath.clear();
            std::error_code Ec;
            const uint64_t Size = std::filesystem::file_size(FilePath, Ec);
            auto F = std::make_shared<std::ifstream>(FilePath, std::ios::binary);
            if (Ec || !*F) { LogErr("BinaryPatch::ProcessBinaryPatches", "cannot open '" + FilePath.string() + "'"); ++Failed; Ok = false; continue; }
            bool AnyScan = false;
            for (const auto &O : ContainerParams.SubComponentsArray)
            {
                std::string OF;
                if (PrefixPatch(O) && Scans(O))
                    if (const auto P = PatchTarget(ContainerParams, O, Vars, /*Report=*/false, OF); P && *P == FilePath) AnyScan = true;
            }
            Img = ImageOf(Size, [F](void *B, size_t N, uint64_t Off) -> long long {
                F->clear();
                F->seekg(std::streamoff(Off));
                F->read(static_cast<char *>(B), std::streamsize(N));
                return F->bad() ? -1 : (long long)F->gcount();
            }, AnyScan);
            OpenPath = FilePath;
        }

        std::string Msg;
        const Result R = ApplyOne(Sub, *Img, Vars, Msg);
        if (R == Result::Error)
        {
            ++Failed; Ok = false;
            LogErr("BinaryPatch::ProcessBinaryPatches",
                   "BinaryPatch FAILED — mode=" + Sub.value("MODE", std::string("(none)"))
                   + " file='" + File + "': " + Msg);
            continue;
        }
        if (R == Result::Skipped) { ++Skipped; LogOut("BinaryPatch::ProcessBinaryPatches", File + ": " + Msg); continue; }

        ++Applied; ++Pending;
        LogOut("BinaryPatch::ProcessBinaryPatches", File + ": " + Msg);
    }
    Flush();

    if (Attempted > 0)
        LogOut("BinaryPatch::ProcessBinaryPatches",
               std::to_string(ReadTime) + " served at read time, " + std::to_string(Applied) + " written, "
               + std::to_string(Skipped) + " already-patched, " + std::to_string(Failed) + " FAILED of "
               + std::to_string(Attempted) + " patch(es)");
    return Ok;
}
