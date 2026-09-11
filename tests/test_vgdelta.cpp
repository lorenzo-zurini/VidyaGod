#include "vgtest.h"
#include "vgdelta.h"      // VidyaGodFS/src — the shared .vgdelta format (generate + apply)

#include <cstring>
#include <memory>
#include <random>
#include <vector>

using vgdelta::GenerateDelta;
using vgdelta::DeltaByteSource;

namespace {

std::vector<uint8_t> Rand(std::mt19937_64 &rng, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto &b : v) b = (uint8_t)rng();
    return v;
}

// Reconstruct target-over-base and CHECK it equals `expect`, hammering full + odd straddling + 1-byte reads.
bool RoundTrip(const std::vector<uint8_t> &base, const std::vector<uint8_t> &expect,
               std::shared_ptr<ByteSource> baseSrc) {
    auto d = GenerateDelta(base.data(), base.size(), expect.data(), expect.size());

    // Whole-view byte verification goes through the SHARED vgdelta::VerifyDelta (the same routine
    // --convert-delta-chain uses in production), so test and tool can never drift apart.
    {
        MemByteSource Target(expect);
        std::string Verr;
        if (!vgdelta::VerifyDelta(d, baseSrc, Target, Verr)) return false;
    }

    std::string err;
    auto ds = DeltaByteSource::Create(std::make_shared<MemByteSource>(d), baseSrc, err, true);
    if (!ds) return false;
    if (ds->size() != expect.size()) return false;

    for (size_t step : {(size_t)1, (size_t)777, (size_t)65536, (size_t)65535}) {
        for (size_t off = 0; off < expect.size(); off += step) {
            size_t n = std::min(step, expect.size() - off);
            std::vector<uint8_t> chunk(n);
            if (ds->pread(chunk.data(), n, off) != (ssize_t)n) return false;
            if (std::memcmp(chunk.data(), expect.data() + off, n) != 0) return false;
        }
    }
    uint8_t x;
    return ds->pread(&x, 1, expect.size()) == 0;   // clean EOF
}

std::shared_ptr<ByteSource> Mem(const std::vector<uint8_t> &v) { return std::make_shared<MemByteSource>(v); }

} // namespace

TEST(vgdelta_identical_and_degenerate)
{
    std::mt19937_64 rng(1);
    std::vector<uint8_t> empty;
    CHECK(RoundTrip(empty, empty, Mem(empty)));                     // empty -> empty
    auto b = Rand(rng, 40000);
    CHECK(RoundTrip(b, b, Mem(b)));                                 // identical
    CHECK(RoundTrip(b, empty, Mem(b)));                             // -> empty
    CHECK(RoundTrip(empty, Rand(rng, 4000), Mem(empty)));           // all-literal (no base)
    auto one = Rand(rng, 1);
    CHECK(RoundTrip(one, Rand(rng, 1), Mem(one)));                  // 1 -> 1
    auto small = Rand(rng, 500);                                    // sub-anchor edit
    auto small2 = small; small2[100] ^= 1;
    CHECK(RoundTrip(small, small2, Mem(small)));
}

TEST(vgdelta_small_edit_is_tiny)
{
    std::mt19937_64 rng(2);
    auto base = Rand(rng, 400000);
    auto tgt = base; for (int i = 0; i < 40; ++i) tgt[200000 + i] ^= 0xFF;
    CHECK(RoundTrip(base, tgt, Mem(base)));
    auto d = GenerateDelta(base.data(), base.size(), tgt.data(), tgt.size());
    CHECK(d.size() < 5000);   // a 40-byte change must not cost anywhere near the full file
}

// Progress is reported and Cancel actually stops the work. Multi-GB deltas are the normal case (a 3.7GB
// Minecraft chain), so a conversion that cannot be interrupted is a hostage situation — this pins that the
// abort path returns empty, and that the poll fires often enough on the shape real content has.
TEST(vgdelta_progress_reports_and_cancel_aborts)
{
    // A NEAR-IDENTICAL target, which is the shape that actually distinguishes the two implementations — and the
    // shape real content has (a game plus a patch, one Minecraft version over the previous). Here the matcher
    // takes long COPY jumps, so `p` leaps over whole 4MiB boundaries: an alignment test (`p % kPollEvery == 0`)
    // lands on one essentially never, freezing the bar and making Cancel unreachable for the whole scan, while
    // a next-threshold test still fires once per 4MiB of PROGRESS.
    //
    // A fully random target does NOT distinguish them: `p` then advances one byte at a time and hits every
    // boundary exactly, so both implementations report identically. An earlier version of this test used that
    // fixture and passed against the bug it claimed to pin.
    std::mt19937_64 rng(7);
    auto basev = Rand(rng, 20u << 20);
    auto &base = basev;                     // the name the rest of this test already uses
    auto tgt  = basev;
    for (size_t off = 1u << 20; off + 64 < tgt.size(); off += (3u << 20))     // a few small edits, far apart
        for (int i = 0; i < 64; ++i) tgt[off + (size_t)i] = (uint8_t)(rng() & 0xff);

    // Reporting: both stages are seen, `done` never exceeds `total`, and the scan reports REPEATEDLY.
    int scanCalls = 0, compressCalls = 0;
    bool sane = true;
    uint64_t lastScan = 0; bool monotonic = true;
    auto d = GenerateDelta(base.data(), base.size(), tgt.data(), tgt.size(), vgdelta::DEFAULT_BLOCK, {},
                           [&](int stage, uint64_t done, uint64_t total) {
                               if (done > total) sane = false;
                               if (stage == 0) { ++scanCalls; if (done < lastScan) monotonic = false; lastScan = done; }
                               else ++compressCalls;
                               return true;
                           });
    CHECK(!d.empty());
    CHECK(sane);
    CHECK(monotonic);
    CHECK(scanCalls >= 4);       // 20MB / 4MiB thresholds — a long scan must report as it goes
    CHECK(compressCalls >= 1);

    // Stage 2 (VERIFY) reports and cancels too. It streams the whole reconstruction a SECOND time, so leaving
    // it uninstrumented meant a finished-looking bar and a dead Cancel button for the longest stretch of a
    // multi-GB conversion — which is the entire justification for this API existing.
    {
        auto baseSrc = Mem(basev);
        MemByteSource tgtSrc(tgt);
        int verifyCalls = 0;
        std::string verr;
        CHECK(vgdelta::VerifyDelta(d, baseSrc, tgtSrc, verr,
                                   [&](int stage, uint64_t done, uint64_t total) {
                                       CHECK_EQ(stage, 2);
                                       CHECK(done <= total);
                                       ++verifyCalls;
                                       return true;
                                   }));
        CHECK(verifyCalls >= 2);          // 20MB in 8MiB chunks

        int seenV = 0;
        std::string verr2;
        CHECK(!vgdelta::VerifyDelta(d, baseSrc, tgtSrc, verr2,
                                    [&](int, uint64_t, uint64_t) { return ++seenV < 2; }));
        CHECK_EQ(verr2, std::string("cancelled"));   // a refusal, not a byte-mismatch verdict
    }

    // Cancelling MID-SCAN aborts — not just on the very first call, which would pass even if the abort were
    // only honoured before any work started.
    int seen = 0;
    auto cancelled = GenerateDelta(base.data(), base.size(), tgt.data(), tgt.size(), vgdelta::DEFAULT_BLOCK, {},
                                   [&](int stage, uint64_t, uint64_t) { return !(stage == 0 && ++seen >= 3); });
    CHECK(cancelled.empty());
    CHECK(seen == 3);            // it stopped at the cancel, rather than running the scan to completion
}

// The editor's "-> dir" on a delta reconstructs the content by streaming a DeltaByteSource to a file, then
// unpacks THAT and deletes the original. So a reconstruction that silently diverges would destroy the only
// copy of the content — this pins that streaming the composed source yields the target byte-for-byte, that a
// chain of deltas composes, and that the base is actually verified rather than assumed.
TEST(vgdelta_reconstruct_streams_target_exactly)
{
    std::mt19937_64 rng(11);
    auto v1 = Rand(rng, 300000);
    auto v2 = v1; for (int i = 0; i < 900; ++i) v2[50000 + i] ^= 0x3C;
    auto v3 = v2; for (int i = 0; i < 700; ++i) v3[210000 + i] ^= 0x5A;

    auto d2 = GenerateDelta(v1.data(), v1.size(), v2.data(), v2.size());
    auto d3 = GenerateDelta(v2.data(), v2.size(), v3.data(), v3.size());
    CHECK(!d2.empty()); CHECK(!d3.empty());

    std::string err;
    auto base = Mem(v1);
    auto s2 = vgdelta::DeltaByteSource::Create(Mem(d2), base, err, /*verifyBase=*/true);
    CHECK(s2 != nullptr);
    // Composition: a delta over a delta collapses to ONE flat map over the ultimate base, so chains do not nest.
    auto s3 = vgdelta::DeltaByteSource::Create(Mem(d3), s2, err, /*verifyBase=*/false);
    CHECK(s3 != nullptr);

    // Stream it the way the editor does — chunked pread into a buffer — and compare to the real target.
    auto Stream = [](const std::shared_ptr<ByteSource> &src) {
        std::vector<uint8_t> out;
        std::vector<uint8_t> buf(4096);
        const uint64_t total = src->size();
        for (uint64_t off = 0; off < total; ) {
            ssize_t r = src->pread(buf.data(), std::min<uint64_t>(buf.size(), total - off), off);
            if (r <= 0) break;
            out.insert(out.end(), buf.begin(), buf.begin() + r);
            off += (uint64_t)r;
        }
        return out;
    };
    CHECK(Stream(s2) == v2);
    CHECK(Stream(s3) == v3);   // two deltas deep, still exact

    // verifyBase must actually verify: composing over the WRONG base has to be refused, not quietly produce a
    // plausible archive full of garbage.
    auto wrong = Rand(rng, 300000);
    std::string werr;
    auto bad = vgdelta::DeltaByteSource::Create(Mem(d2), Mem(wrong), werr, /*verifyBase=*/true);
    CHECK(bad == nullptr);
    CHECK(!werr.empty());
}

TEST(vgdelta_moves_reorder_merge)
{
    std::mt19937_64 rng(3);
    auto A = Rand(rng, 80000), B = Rand(rng, 60000), C = Rand(rng, 90000);
    std::vector<uint8_t> base; for (auto *p : {&A, &B, &C}) base.insert(base.end(), p->begin(), p->end());

    // reorder + a slice of A relocated INTO the middle of C's region + a new literal chunk
    auto lit = Rand(rng, 12000);
    std::vector<uint8_t> tgt;
    tgt.insert(tgt.end(), C.begin(), C.end());
    tgt.insert(tgt.end(), A.begin() + 5000, A.begin() + 35000);   // 30KB slice of A moved
    tgt.insert(tgt.end(), lit.begin(), lit.end());
    tgt.insert(tgt.end(), B.begin(), B.end());
    CHECK(RoundTrip(base, tgt, Mem(base)));
    auto d = GenerateDelta(base.data(), base.size(), tgt.data(), tgt.size());
    CHECK(d.size() < lit.size() + 20000);   // only the new literal (+overhead); all moves are COPYs

    // overlapping copies: repeat the SAME base range many times
    std::vector<uint8_t> rep;
    for (int i = 0; i < 15; ++i) rep.insert(rep.end(), C.begin() + 1000, C.begin() + 1000 + 20000);
    CHECK(RoundTrip(base, rep, Mem(base)));
}

TEST(vgdelta_chain_three_deep)
{
    std::mt19937_64 rng(4);
    auto v1 = Rand(rng, 300000);
    auto v2 = v1; for (int i = 0; i < 200; ++i) v2[100000 + i] = (uint8_t)rng();
    auto v3 = v2; for (int i = 0; i < 250; ++i) v3[200000 + i] = (uint8_t)rng();

    auto src1 = Mem(v1);
    auto d2 = GenerateDelta(v1.data(), v1.size(), v2.data(), v2.size());
    std::string e;
    auto src2 = DeltaByteSource::Create(std::make_shared<MemByteSource>(d2), src1, e, true);
    CHECK(src2 != nullptr);
    if (!src2) return;
    std::vector<uint8_t> got2(src2->size());
    CHECK(src2->pread(got2.data(), got2.size(), 0) == (ssize_t)v2.size());
    CHECK(got2 == v2);

    // d3 cut against v2's RECONSTRUCTED view, then read v3 through the 2-deep chain
    auto d3 = GenerateDelta(got2.data(), got2.size(), v3.data(), v3.size());
    auto src3 = DeltaByteSource::Create(std::make_shared<MemByteSource>(d3), src2, e, false);
    CHECK(src3 != nullptr);
    if (!src3) return;
    std::vector<uint8_t> got3(src3->size());
    CHECK(src3->pread(got3.data(), got3.size(), 0) == (ssize_t)v3.size());
    CHECK(got3 == v3);
}

TEST(vgdelta_rejects_wrong_base)
{
    std::mt19937_64 rng(5);
    auto base = Rand(rng, 30000);
    auto tgt = base; tgt[100] ^= 1;
    auto d = GenerateDelta(base.data(), base.size(), tgt.data(), tgt.size());
    std::string e;
    // wrong-content base of the right size → caught by the base-hash check
    auto wrong = Rand(rng, 30000);
    CHECK(DeltaByteSource::Create(std::make_shared<MemByteSource>(d), Mem(wrong), e, true) == nullptr);
    // wrong-size base → caught cheaply even without hashing
    auto shortb = Rand(rng, 29000);
    CHECK(DeltaByteSource::Create(std::make_shared<MemByteSource>(d), Mem(shortb), e, false) == nullptr);
}
