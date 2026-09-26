#include "cid.h"

#include <array>
#include <cstdint>

namespace Cid
{

namespace {

//FIPS 180-4 SHA-256.
constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t Rotr(uint32_t X, int N) { return (X >> N) | (X << (32 - N)); }

void Block(std::array<uint32_t, 8> &H, const unsigned char *P)
{
    uint32_t W[64];
    for (int I = 0; I < 16; ++I)
        W[I] = (uint32_t(P[4 * I]) << 24) | (uint32_t(P[4 * I + 1]) << 16) | (uint32_t(P[4 * I + 2]) << 8) | uint32_t(P[4 * I + 3]);
    for (int I = 16; I < 64; ++I)
    {
        const uint32_t S0 = Rotr(W[I - 15], 7) ^ Rotr(W[I - 15], 18) ^ (W[I - 15] >> 3);
        const uint32_t S1 = Rotr(W[I - 2], 17) ^ Rotr(W[I - 2], 19) ^ (W[I - 2] >> 10);
        W[I] = W[I - 16] + S0 + W[I - 7] + S1;
    }
    uint32_t A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], Hh = H[7];
    for (int I = 0; I < 64; ++I)
    {
        const uint32_t S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
        const uint32_t Ch = (E & F) ^ (~E & G);
        const uint32_t T1 = Hh + S1 + Ch + K[I] + W[I];
        const uint32_t S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
        const uint32_t Mj = (A & B) ^ (A & C) ^ (B & C);
        const uint32_t T2 = S0 + Mj;
        Hh = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
    }
    H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
}

std::string Base32Lower(const std::string &In)       // RFC 4648 alphabet, lower case, no padding (multibase 'b')
{
    static const char *Alpha = "abcdefghijklmnopqrstuvwxyz234567";
    std::string Out;
    uint32_t Buf = 0;
    int Bits = 0;
    for (unsigned char C : In)
    {
        Buf = (Buf << 8) | C;
        Bits += 8;
        while (Bits >= 5) { Out += Alpha[(Buf >> (Bits - 5)) & 31]; Bits -= 5; }
    }
    if (Bits > 0) Out += Alpha[(Buf << (5 - Bits)) & 31];
    return Out;
}

} // namespace

std::string Sha256(const std::string &Bytes)
{
    std::array<uint32_t, 8> H = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const auto *P = reinterpret_cast<const unsigned char *>(Bytes.data());
    const size_t N = Bytes.size();
    size_t I = 0;
    for (; I + 64 <= N; I += 64) Block(H, P + I);
    unsigned char Tail[128] = {};
    const size_t Rest = N - I;
    for (size_t J = 0; J < Rest; ++J) Tail[J] = P[I + J];
    Tail[Rest] = 0x80;
    const size_t TailLen = Rest + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t BitLen = uint64_t(N) * 8;
    for (int J = 0; J < 8; ++J) Tail[TailLen - 1 - J] = static_cast<unsigned char>(BitLen >> (8 * J));
    Block(H, Tail);
    if (TailLen == 128) Block(H, Tail + 64);
    std::string Out(32, '\0');
    for (int J = 0; J < 8; ++J)
        for (int B = 0; B < 4; ++B) Out[4 * J + B] = static_cast<char>(H[J] >> (24 - 8 * B));
    return Out;
}

std::string Canonical(const nlohmann::ordered_json &Node)
{
    //nlohmann::json keeps object keys in a std::map — sorted by bytes, which for UTF-8 is code-point order — and
    //dump() with no indent writes no insignificant whitespace, UTF-8 as-is.
    nlohmann::json J = Node;
    if (J.is_object()) { J.erase("CID"); J.erase("POS"); }
    return J.dump();
}

std::string OfBytes(const std::string &Bytes, std::string *Error)
{
    if (Bytes.size() > 256 * 1024)
    {
        if (Error) *Error = "node is " + std::to_string(Bytes.size()) + " bytes — over one 256 KiB block (a chunked UnixFS file)";
        return std::string();
    }
    // CIDv1 (0x01) · raw (0x55) · multihash sha2-256 (0x12, 32 bytes)
    return "b" + Base32Lower(std::string("\x01\x55\x12\x20", 4) + Sha256(Bytes));
}

std::string OfNode(const nlohmann::ordered_json &Node, std::string *Error)
{
    return OfBytes(Canonical(Node), Error);
}

} // namespace Cid
