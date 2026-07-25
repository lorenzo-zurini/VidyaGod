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
    std::string err;
    auto ds = DeltaByteSource::Create(std::make_shared<MemByteSource>(d), baseSrc, err, true);
    if (!ds) return false;
    if (ds->size() != expect.size()) return false;

    std::vector<uint8_t> full(expect.size());
    if (!expect.empty() && ds->pread(full.data(), full.size(), 0) != (ssize_t)expect.size()) return false;
    if (full != expect) return false;

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
