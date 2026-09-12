// Generates a real .vgdelta for the launch-matrix fixture.
//
// The fixture needs deltas that actually RECONSTRUCT, not stubs: a stub proves the plan handles a delta layer
// and nothing about whether the mount can rebuild the bytes. Several bases are concatenated in the order given,
// which is exactly what a multi-base delta composes over at mount time — so the fixture's BASE_TARGETS order is
// under test too, not just assumed.
//
//   make_delta <out.vgdelta> <target.zip> <base1> [base2 ...]
//
// Verifies before writing, using the same vgdelta::VerifyDelta the app's own converter uses, so a fixture can
// never ship a delta that does not rebuild.

#include "vgdelta.h"
#include "bytesource.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

static std::vector<uint8_t> ReadAll(const std::string &P, bool &Ok)
{
    std::ifstream In(P, std::ios::binary);
    if (!In) { Ok = false; return {}; }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: make_delta <out> <target> <base...>\n"); return 2; }
    bool Ok = true;
    const std::vector<uint8_t> Target = ReadAll(argv[2], Ok);
    std::vector<uint8_t> Base;
    for (int I = 3; I < argc; ++I)
    {
        const std::vector<uint8_t> B = ReadAll(argv[I], Ok);
        Base.insert(Base.end(), B.begin(), B.end());
    }
    if (!Ok) { std::fprintf(stderr, "make_delta: could not read an input\n"); return 2; }

    const std::vector<uint8_t> D = vgdelta::GenerateDelta(Base.data(), Base.size(), Target.data(), Target.size());
    if (D.empty()) { std::fprintf(stderr, "make_delta: GenerateDelta produced nothing\n"); return 1; }

    std::string Err;
    auto BaseSrc = std::make_shared<MemByteSource>(Base);
    MemByteSource TargetSrc(Target);
    if (!vgdelta::VerifyDelta(D, BaseSrc, TargetSrc, Err))
    { std::fprintf(stderr, "make_delta: delta does not reconstruct: %s\n", Err.c_str()); return 1; }

    std::ofstream Out(argv[1], std::ios::binary);
    if (!Out) { std::fprintf(stderr, "make_delta: cannot write %s\n", argv[1]); return 1; }
    Out.write(reinterpret_cast<const char *>(D.data()), (std::streamsize)D.size());
    return 0;
}
