#include "qrcode.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace {

//Error correction M, versions 1–10: codewords of error correction per block, and blocks.
constexpr int kEccPerBlock[11] = { -1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26 };
constexpr int kBlocks[11]      = { -1,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5 };
constexpr int kMaxVersion = 10;
constexpr int kEccFormatBits = 0;   // M

//Modules that carry data or error correction (everything but the function patterns).
int RawModules(int Ver)
{
    int R = (16 * Ver + 128) * Ver + 64;
    if (Ver >= 2)
    {
        const int Align = Ver / 7 + 2;
        R -= (25 * Align - 10) * Align - 55;
        if (Ver >= 7) R -= 36;
    }
    return R;
}
int DataCodewords(int Ver) { return RawModules(Ver) / 8 - kEccPerBlock[Ver] * kBlocks[Ver]; }

uint8_t Mul(uint8_t X, uint8_t Y)   // GF(2^8) mod x^8 + x^4 + x^3 + x^2 + 1
{
    int Z = 0;
    for (int I = 7; I >= 0; --I)
    {
        Z = (Z << 1) ^ ((Z >> 7) * 0x11D);
        Z ^= ((Y >> I) & 1) * X;
    }
    return static_cast<uint8_t>(Z);
}

std::vector<uint8_t> Divisor(int Degree)
{
    std::vector<uint8_t> R(static_cast<size_t>(Degree), 0);
    R.back() = 1;
    uint8_t Root = 1;
    for (int I = 0; I < Degree; ++I)
    {
        for (size_t J = 0; J < R.size(); ++J)
        {
            R[J] = Mul(R[J], Root);
            if (J + 1 < R.size()) R[J] ^= R[J + 1];
        }
        Root = Mul(Root, 0x02);
    }
    return R;
}

std::vector<uint8_t> Remainder(const std::vector<uint8_t> &Data, const std::vector<uint8_t> &Div)
{
    std::vector<uint8_t> R(Div.size(), 0);
    for (uint8_t B : Data)
    {
        const uint8_t F = B ^ R.front();
        R.erase(R.begin());
        R.push_back(0);
        for (size_t I = 0; I < R.size(); ++I) R[I] ^= Mul(Div[I], F);
    }
    return R;
}

struct Grid
{
    int N;
    std::vector<std::vector<bool>> M, Fn;   // modules, and which are function patterns
    explicit Grid(int Ver) : N(Ver * 4 + 17), M(N, std::vector<bool>(N)), Fn(N, std::vector<bool>(N)) {}
    void Set(int X, int Y, bool Dark) { M[Y][X] = Dark; Fn[Y][X] = true; }
};

void Finder(Grid &G, int X, int Y)
{
    for (int Dy = -4; Dy <= 4; ++Dy)
        for (int Dx = -4; Dx <= 4; ++Dx)
        {
            const int D = std::max(std::abs(Dx), std::abs(Dy)), Xx = X + Dx, Yy = Y + Dy;
            if (Xx >= 0 && Xx < G.N && Yy >= 0 && Yy < G.N) G.Set(Xx, Yy, D != 2 && D != 4);
        }
}

void Alignment(Grid &G, int X, int Y)
{
    for (int Dy = -2; Dy <= 2; ++Dy)
        for (int Dx = -2; Dx <= 2; ++Dx) G.Set(X + Dx, Y + Dy, std::max(std::abs(Dx), std::abs(Dy)) != 1);
}

std::vector<int> AlignmentPositions(int Ver)
{
    if (Ver == 1) return {};
    const int Count = Ver / 7 + 2, N = Ver * 4 + 17;
    const int Step = (Ver * 4 + Count * 2 + 1) / (Count * 2 - 2) * 2;
    std::vector<int> R = { 6 };
    for (int Pos = N - 7; static_cast<int>(R.size()) < Count; Pos -= Step) R.insert(R.begin() + 1, Pos);
    return R;
}

void FormatBits(Grid &G, int Mask)
{
    const int Data = kEccFormatBits << 3 | Mask;
    int Rem = Data;
    for (int I = 0; I < 10; ++I) Rem = (Rem << 1) ^ ((Rem >> 9) * 0x537);
    const int Bits = (Data << 10 | Rem) ^ 0x5412;
    const auto Bit = [Bits](int I) { return ((Bits >> I) & 1) != 0; };
    for (int I = 0; I <= 5; ++I) G.Set(8, I, Bit(I));
    G.Set(8, 7, Bit(6));
    G.Set(8, 8, Bit(7));
    G.Set(7, 8, Bit(8));
    for (int I = 9; I < 15; ++I) G.Set(14 - I, 8, Bit(I));
    for (int I = 0; I < 8; ++I) G.Set(G.N - 1 - I, 8, Bit(I));
    for (int I = 8; I < 15; ++I) G.Set(8, G.N - 15 + I, Bit(I));
    G.Set(8, G.N - 8, true);   // the dark module
}

void VersionBits(Grid &G, int Ver)
{
    if (Ver < 7) return;
    int Rem = Ver;
    for (int I = 0; I < 12; ++I) Rem = (Rem << 1) ^ ((Rem >> 11) * 0x1F25);
    const long Bits = static_cast<long>(Ver) << 12 | Rem;
    for (int I = 0; I < 18; ++I)
    {
        const bool B = ((Bits >> I) & 1) != 0;
        const int A = G.N - 11 + I % 3, C = I / 3;
        G.Set(A, C, B);
        G.Set(C, A, B);
    }
}

void FunctionPatterns(Grid &G, int Ver)
{
    for (int I = 0; I < G.N; ++I) { G.Set(6, I, I % 2 == 0); G.Set(I, 6, I % 2 == 0); }
    Finder(G, 3, 3);
    Finder(G, G.N - 4, 3);
    Finder(G, 3, G.N - 4);
    const std::vector<int> A = AlignmentPositions(Ver);
    const int Last = static_cast<int>(A.size()) - 1;
    for (int I = 0; I <= Last; ++I)
        for (int J = 0; J <= Last; ++J)
            if (!((I == 0 && J == 0) || (I == 0 && J == Last) || (I == Last && J == 0))) Alignment(G, A[I], A[J]);
    FormatBits(G, 0);   // reserved now, written with the mask chosen
    VersionBits(G, Ver);
}

void PlaceCodewords(Grid &G, const std::vector<uint8_t> &Data)
{
    size_t I = 0;
    for (int Right = G.N - 1; Right >= 1; Right -= 2)
    {
        if (Right == 6) Right = 5;
        for (int V = 0; V < G.N; ++V)
            for (int J = 0; J < 2; ++J)
            {
                const int X = Right - J;
                const bool Up = ((Right + 1) & 2) == 0;
                const int Y = Up ? G.N - 1 - V : V;
                if (!G.Fn[Y][X] && I < Data.size() * 8)
                {
                    G.M[Y][X] = ((Data[I >> 3] >> (7 - (I & 7))) & 1) != 0;
                    ++I;
                }
            }
    }
}

void ApplyMask(Grid &G, int Mask)
{
    for (int Y = 0; Y < G.N; ++Y)
        for (int X = 0; X < G.N; ++X)
        {
            bool Inv = false;
            switch (Mask)
            {
            case 0: Inv = (X + Y) % 2 == 0; break;
            case 1: Inv = Y % 2 == 0; break;
            case 2: Inv = X % 3 == 0; break;
            case 3: Inv = (X + Y) % 3 == 0; break;
            case 4: Inv = (X / 3 + Y / 2) % 2 == 0; break;
            case 5: Inv = X * Y % 2 + X * Y % 3 == 0; break;
            case 6: Inv = (X * Y % 2 + X * Y % 3) % 2 == 0; break;
            default: Inv = ((X + Y) % 2 + X * Y % 3) % 2 == 0; break;
            }
            if (Inv && !G.Fn[Y][X]) G.M[Y][X] = !G.M[Y][X];
        }
}

//How hard the symbol is to read (the standard's four rules): the mask with the least is the one used.
long Penalty(const Grid &G)
{
    long P = 0;
    const auto At = [&G](int X, int Y, bool Col) { return Col ? G.M[X][Y] : G.M[Y][X]; };
    for (int Col = 0; Col < 2; ++Col)
        for (int Y = 0; Y < G.N; ++Y)
        {
            int Run = 1;
            for (int X = 1; X <= G.N; ++X)
            {
                if (X < G.N && At(X, Y, Col) == At(X - 1, Y, Col)) { ++Run; continue; }
                if (Run >= 5) P += 3 + (Run - 5);
                Run = 1;
            }
            //A finder's look-alike (1:1:3:1:1 with four light modules beside it), outside the symbol light.
            static const bool Look[2][11] = { { 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1 } };
            for (int X = -10; X < G.N; ++X)
                for (const auto &L : Look)
                {
                    bool Hit = true;
                    for (int K = 0; K < 11 && Hit; ++K)
                    {
                        const int Xx = X + K;
                        const bool Dark = Xx >= 0 && Xx < G.N && At(Xx, Y, Col);
                        Hit = Dark == L[K];
                    }
                    if (Hit) P += 40;
                }
        }
    for (int Y = 0; Y + 1 < G.N; ++Y)
        for (int X = 0; X + 1 < G.N; ++X)
        {
            const bool C = G.M[Y][X];
            if (C == G.M[Y][X + 1] && C == G.M[Y + 1][X] && C == G.M[Y + 1][X + 1]) P += 3;
        }
    long Dark = 0;
    for (const auto &Row : G.M) Dark += std::count(Row.begin(), Row.end(), true);
    const long Total = static_cast<long>(G.N) * G.N;
    const long K = (std::labs(Dark * 20 - Total * 10) + Total - 1) / Total - 1;
    return P + K * 10;
}

} // namespace

std::vector<std::vector<bool>> QrCode::Encode(const std::string &Text)
{
    int Ver = 1;
    for (; Ver <= kMaxVersion; ++Ver)
    {
        const int CountBits = Ver <= 9 ? 8 : 16;
        if (4 + CountBits + 8 * static_cast<int>(Text.size()) <= DataCodewords(Ver) * 8) break;
    }
    if (Ver > kMaxVersion) return {};

    //The bit stream: byte mode, the length, the bytes, a terminator, padding to the capacity.
    std::vector<bool> Bits;
    const auto Put = [&Bits](unsigned V, int N) { for (int I = N - 1; I >= 0; --I) Bits.push_back(((V >> I) & 1) != 0); };
    Put(0x4, 4);
    Put(static_cast<unsigned>(Text.size()), Ver <= 9 ? 8 : 16);
    for (unsigned char C : Text) Put(C, 8);
    const size_t Capacity = static_cast<size_t>(DataCodewords(Ver)) * 8;
    Put(0, static_cast<int>(std::min<size_t>(4, Capacity - Bits.size())));
    Put(0, static_cast<int>((8 - Bits.size() % 8) % 8));
    for (unsigned Pad = 0xEC; Bits.size() < Capacity; Pad ^= 0xEC ^ 0x11) Put(Pad, 8);
    std::vector<uint8_t> Data(Bits.size() / 8, 0);
    for (size_t I = 0; I < Bits.size(); ++I) if (Bits[I]) Data[I >> 3] |= static_cast<uint8_t>(1 << (7 - (I & 7)));

    //Split into blocks, each with its error correction, then interleaved.
    const int NumBlocks = kBlocks[Ver], EccLen = kEccPerBlock[Ver], Raw = RawModules(Ver) / 8;
    const int Short = NumBlocks - Raw % NumBlocks, ShortLen = Raw / NumBlocks;
    const std::vector<uint8_t> Div = Divisor(EccLen);
    std::vector<std::vector<uint8_t>> Blocks;
    for (int I = 0, K = 0; I < NumBlocks; ++I)
    {
        const int Len = ShortLen - EccLen + (I < Short ? 0 : 1);
        std::vector<uint8_t> B(Data.begin() + K, Data.begin() + K + Len);
        K += Len;
        const std::vector<uint8_t> Ecc = Remainder(B, Div);
        if (I < Short) B.push_back(0);
        B.insert(B.end(), Ecc.begin(), Ecc.end());
        Blocks.push_back(std::move(B));
    }
    std::vector<uint8_t> All;
    for (size_t I = 0; I < Blocks[0].size(); ++I)
        for (size_t J = 0; J < Blocks.size(); ++J)
            if (I != static_cast<size_t>(ShortLen - EccLen) || J >= static_cast<size_t>(Short)) All.push_back(Blocks[J][I]);

    Grid G(Ver);
    FunctionPatterns(G, Ver);
    PlaceCodewords(G, All);
    int Best = 0;
    long BestP = -1;
    for (int Mask = 0; Mask < 8; ++Mask)
    {
        ApplyMask(G, Mask);
        FormatBits(G, Mask);
        const long P = Penalty(G);
        if (BestP < 0 || P < BestP) { BestP = P; Best = Mask; }
        ApplyMask(G, Mask);   // undone: a mask is its own inverse
    }
    ApplyMask(G, Best);
    FormatBits(G, Best);
    return G.M;
}
