#include "launchtree.h"
#include "treelayer.h"
#include "viewmode.h"
#include "manifestmodel.h"   // MakeVfsSpecLayer — the spec entry the launch builds from the same op
#include "nodelower.h"       // ContentOps — what the launch mounts
#include "zipscan.h"         // VidyaGodFS: a zip's central directory, from any byte source
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

LaunchTree LaunchTree::Of(const Fold::Plan &P)
{
    LaunchTree L;
    nlohmann::ordered_json Layers = nlohmann::ordered_json::array();
    for (const auto &[J, Op] : NodeLower::ContentOps(P))
    {
        nlohmann::ordered_json X = ManifestModel::MakeVfsSpecLayer(Op, Op.value("PATH", std::string()), Op.value("TARGET", std::string()));
        if (X.is_null()) continue;
        L.LayerOf[J] = L.ItemOf.size();
        L.ItemOf.push_back(J);
        Layers.push_back(std::move(X));
    }
    const nlohmann::ordered_json Spec = { {"mountpoint", "/"}, {"readonly", true}, {"layers", Layers} };   // never mounted
    std::string Err;
    const bool Parsed = ParseSpecText(Spec.dump(), L.S, Err);
    if (!Parsed) { L.S = {}; L.ItemOf.clear(); L.LayerOf.clear(); }
    L.T = PlanTree(L.S);
    if (!Parsed) L.T.problems.push_back("the mount plan is malformed (" + Err + ")");   // the lowering's own shapes always parse
    return L;
}

const std::vector<std::string> *ArchiveNames::Of(const LaunchTree &L, size_t Layer, bool Folders)
{
    if (Layer >= L.S.layers.size()) return nullptr;
    const LayerSpec &X = L.S.layers[Layer];
    if (X.type == LayerType::View || X.type == LayerType::Tree)
    {   //a view's names are its listing's, a tree's its table's (each its own file: no base read)
        if (Folders) { const auto It = All.find(X.source); if (It != All.end()) return &It->second; }
        else { const auto It = Files.find(X.source); if (It != Files.end()) return &It->second; }
        if (Failed.count(X.source)) return nullptr;
        std::vector<std::string> A, F;
        std::string Err;
        auto Src = std::make_shared<MmapByteSource>();
        bool Ok = Src->Map(X.source) && (X.type == LayerType::Tree || Src->n <= (256ull << 20));   // a listing over 256 MiB is refused, as a mount refuses it (a tree: its table's own limits)
        if (Ok && X.type == LayerType::View)
        {
            std::vector<coding::ViewEntry> V;
            Ok = coding::ReadViewListing(std::string(reinterpret_cast<const char *>(Src->p), (size_t)Src->n), V, Err);
            for (const auto &E : V) { A.push_back(E.dir ? E.path + "/" : E.path); if (!E.dir) F.push_back(E.path); }
        }
        else if (Ok)
        {
            vgtree::Opened Tr;
            Ok = vgtree::OpenTree(Src, Tr, Err);
            for (const auto &E : Tr.table.entries) { A.push_back(E.kind == vgtree::Entry::Dir ? E.path + "/" : E.path); if (E.kind != vgtree::Entry::Dir) F.push_back(E.path); }
        }
        if (!Ok) { Failed.insert(X.source); return nullptr; }
        All.emplace(X.source, std::move(A)); Files.emplace(X.source, std::move(F));
        return Folders ? &All[X.source] : &Files[X.source];
    }
    if (X.type != LayerType::Zip && X.type != LayerType::Delta) return nullptr;
    //A zip is its file; a delta, its file over the bases it names.
    const auto Key = [](const LayerSpec &Y) {
        std::string K = Y.source;
        for (const std::string &B : Y.bases) K += '\x1f' + B;
        return K;
    };
    const auto Ready = [&]() -> const std::vector<std::string> * {
        if (Folders) { const auto It = All.find(Key(X)); return It == All.end() ? nullptr : &It->second; }
        const auto It = Files.find(Key(X)); return It == Files.end() ? nullptr : &It->second;
    };
    if (const auto *R = Ready()) return R;
    if (Failed.count(Key(X))) return nullptr;
    const auto List = [&](const std::string &K, const std::shared_ptr<ByteSource> &Src) {
        if (All.count(K)) return;
        std::vector<std::string> A, F;
        if (!zipscan::WalkCentralDir(*Src, [&](const std::string &Name, uint16_t, uint64_t) {
                A.push_back(Name);
                std::string N = Name;
                std::replace(N.begin(), N.end(), '\\', '/');
                if (!N.empty() && N.back() != '/') F.push_back(std::move(N));
                return true; }))
            return;
        All.emplace(K, std::move(A));
        Files.emplace(K, std::move(F));
    };
    LayerBytes Bytes(L.S, L.T, std::vector<bool>(L.S.layers.size(), false));
    Bytes.OnBuilt = [&](size_t I, const std::shared_ptr<ByteSource> &Src) {
        if (L.S.layers[I].type == LayerType::Delta) List(Key(L.S.layers[I]), Src);   // a chain's members, on the way up
    };
    if (const std::shared_ptr<ByteSource> Src = Bytes.Get(Layer)) List(Key(X), Src);
    for (size_t I = 0; I < L.S.layers.size(); ++I)           // every delta on the way down that its files failed
        if (Bytes.Failed(I) && !L.T.broken[I]) Failed.insert(Key(L.S.layers[I]));
    return Ready();
}

namespace
{
uint32_t U32(const std::vector<uint8_t> &B, size_t At) { return At + 4 <= B.size() ? zipscan::Rd32(B.data() + At) : 0; }
uint64_t Fnv(const uint8_t *P, size_t N, uint64_t H = 1469598103934665603ull) { for (size_t I = 0; I < N; ++I) { H ^= P[I]; H *= 1099511628211ull; } return H; }
std::string Lower(std::string S) { for (char &C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); return S; }
std::string Hex(uint64_t V) { char B[24]; std::snprintf(B, sizeof B, "%016llx", static_cast<unsigned long long>(V)); return B; }
}

char InnerUnits::KindOf(const std::string &Name)
{
    const std::string N = Lower(Name);
    const auto Ends = [&](const char *E) { const size_t K = std::strlen(E); return N.size() >= K && N.compare(N.size() - K, K, E) == 0; };
    if (Ends(".bsa")) return 'b';
    if (Ends(".esm") || Ends(".esp") || Ends(".esl")) return 'p';
    return 0;
}

//BSA 103 (Oblivion), 104 (Fallout 3/NV, Skyrim), 105 (Skyrim SE): folder records, then each folder's name and file
//records, then the file names in record order. A member's value is its record's size and offset (Of prefixes the
//archive's CID: the same archive, the same member; another archive, another write).
bool InnerUnits::BsaMembers(const std::vector<uint8_t> &B, std::vector<InnerUnit> &Out)
{
    if (B.size() < 36 || std::memcmp(B.data(), "BSA\0", 4) != 0) return false;
    const uint32_t Ver = U32(B, 4), Flags = U32(B, 12), Nf = U32(B, 16), Nfiles = U32(B, 20);
    if ((Ver != 103 && Ver != 104 && Ver != 105) || !(Flags & 1) || !(Flags & 2)) return false;   // names: folders and files
    const size_t FolderRec = Ver == 105 ? 24 : 16;
    if (Nf > (1u << 20) || Nfiles > (1u << 24)) return false;
    size_t Q = 36 + static_cast<size_t>(Nf) * FolderRec;
    std::vector<std::pair<std::string, std::string>> Recs;                 // folder, the record's size + offset
    for (uint32_t F = 0; F < Nf; ++F)
    {
        const uint32_t Count = U32(B, 36 + F * FolderRec + 8);
        if (Q >= B.size()) return false;
        const size_t Len = B[Q];
        if (Q + 1 + Len > B.size() || Len == 0) return false;
        std::string Folder(reinterpret_cast<const char *>(B.data() + Q + 1), Len - 1);
        Q += 1 + Len;
        for (uint32_t K = 0; K < Count; ++K, Q += 16)
        {
            if (Q + 16 > B.size()) return false;
            Recs.push_back({ Folder, Hex((static_cast<uint64_t>(U32(B, Q + 8)) << 32) | U32(B, Q + 12)) });
        }
    }
    if (Recs.size() != Nfiles) return false;
    for (auto &[Folder, V] : Recs)
    {
        const void *Z = Q < B.size() ? std::memchr(B.data() + Q, 0, B.size() - Q) : nullptr;
        if (!Z) return false;
        const size_t E = static_cast<size_t>(static_cast<const uint8_t *>(Z) - B.data());
        std::string Path = Folder + "/" + std::string(reinterpret_cast<const char *>(B.data() + Q), E - Q);
        std::replace(Path.begin(), Path.end(), '\\', '/');
        Out.push_back({ Path, V });
        Q = E + 1;
    }
    return true;
}

//TES4-family plugins: a TES4 header record (its MAST subrecords: the masters, in order), then groups (GRUP, 24 or 20
//bytes, their size counting the header) holding records (a 24-byte header from Fallout 3 on, 20 in Oblivion; then its
//data). A FormID's high byte is its master's index; equal to the number of masters, the plugin itself.
bool InnerUnits::PluginRecords(const std::vector<uint8_t> &B, const std::string &Name, std::vector<InnerUnit> &Out)
{
    if (B.size() < 24 || std::memcmp(B.data(), "TES4", 4) != 0) return false;
    const uint32_t HeadSize = U32(B, 4);
    //Oblivion's records have 20-byte headers: its TES4's HEDR follows at 20
    const size_t Hdr = (B.size() > 24 && std::memcmp(B.data() + 20, "HEDR", 4) == 0) ? 20 : 24;
    if (Hdr + HeadSize > B.size()) return false;
    std::vector<std::string> Masters;
    for (size_t P = Hdr; P + 6 <= Hdr + HeadSize;)
    {
        const size_t L = static_cast<size_t>(B[P + 4]) | (static_cast<size_t>(B[P + 5]) << 8);
        if (P + 6 + L > Hdr + HeadSize) return false;
        if (std::memcmp(B.data() + P, "MAST", 4) == 0)
            Masters.push_back(Lower(std::string(reinterpret_cast<const char *>(B.data() + P + 6), L ? L - 1 : 0)));
        P += 6 + L;
    }
    const std::string Self = Lower(std::filesystem::path(Name).filename().string());
    size_t P = Hdr + HeadSize;
    while (P + Hdr <= B.size())
    {
        const uint32_t Size = U32(B, P + 4);
        if (std::memcmp(B.data() + P, "GRUP", 4) == 0) { if (Size < Hdr) return false; P += Hdr; continue; }   // its records follow
        const uint32_t Id = U32(B, P + 12);
        if (P + Hdr + Size > B.size()) return false;
        const uint32_t M = Id >> 24;
        char Fid[16]; std::snprintf(Fid, sizeof Fid, "%06X", Id & 0xFFFFFFu);
        Out.push_back({ "RECORD/" + (M < Masters.size() ? Masters[M] : Self) + "/" + Fid,
                        Hex(Fnv(B.data() + P + 8, 4, Fnv(B.data() + P + Hdr, Size, Fnv(B.data() + P, 4)))) });   // type, flags, data
        P += Hdr + Size;
    }
    return P == B.size();
}

const std::vector<InnerUnit> *InnerUnits::Of(const LaunchTree &L, size_t Layer, const std::string &Member)
{
    if (Layer >= L.S.layers.size()) return nullptr;
    const LayerSpec &X = L.S.layers[Layer];
    const std::string Name = Member.empty() ? (X.type == LayerType::Coded ? X.name : X.source) : Member;
    const char Kind = KindOf(Name);
    if (!Kind) return nullptr;
    std::string Key = X.source + '\x1f' + Member;
    for (const std::string &B : X.bases) Key += '\x1f' + B;
    if (const auto It = Units.find(Key); It != Units.end()) return &It->second;
    if (Failed.count(Key)) return nullptr;
    LayerBytes Bytes(L.S, L.T, std::vector<bool>(L.S.layers.size(), false));
    std::shared_ptr<ByteSource> Src = Bytes.Get(Layer);
    uint64_t At = 0, N = Src ? Src->size() : 0;
    if (Src && !Member.empty())                                          // a member of a zip (or a delta's rebuilt zip): stored
    {
        bool Found = false;
        zipscan::WalkCentralDirFull(*Src, [&](const zipscan::CdEntry &E) {
            std::string Nm = E.utf8Name; std::replace(Nm.begin(), Nm.end(), '\\', '/');
            if (Nm != Member) return true;
            if (E.method != 0) return false;
            const uint64_t D = zipscan::LocalDataOffset(*Src, E.lhOffset);
            if (D == UINT64_MAX) return false;
            At = D; N = E.uncompSize; Found = true; return false; });
        if (!Found) Src.reset();
    }
    //a coded archive: its content begins with the archive's header region as it is (the BSA description's content), so
    //its names are read there, never through its record (that would run its encoder family at every launch)
    if (Src && Member.empty() && X.type == LayerType::Coded && Kind == 'b' && !L.T.bases[Layer].empty())
    {
        LayerBytes Cb(L.S, L.T, std::vector<bool>(L.S.layers.size(), false));
        if (auto C = Cb.Get(L.T.bases[Layer][0]); C && C->size() >= 4)
        {
            char Mg[4] = {0};
            if (C->preadAll(Mg, 4, 0) && std::memcmp(Mg, "BSA\0", 4) == 0) { Src = C; At = 0; N = C->size(); }
            else Src.reset();
        }
        else Src.reset();
    }
    //a plugin is read whole; an archive only as far as its names (they precede every member). What a zip's directory
    //claims is checked against the file (an untrusted zip must not make this allocate what is not there), and no file
    //gives more than a few million units.
    std::vector<uint8_t> B;
    std::vector<InnerUnit> U;
    bool Ok = false;
    if (Src && (At > Src->size() || N > Src->size() - At)) Src.reset();
    if (Src && N <= (Kind == 'p' ? (1ull << 30) : UINT64_MAX))
    {
        if (Kind == 'b')
        {
            std::vector<uint8_t> H(static_cast<size_t>(std::min<uint64_t>(N, 36)));
            if (Src->preadAll(H.data(), H.size(), At) && H.size() == 36)
            {
                const uint32_t Ver = U32(H, 4), Nf = U32(H, 16), Nfiles = U32(H, 20), Tfn = U32(H, 24), Tfin = U32(H, 28);
                const uint64_t Region = 36 + static_cast<uint64_t>(Nf) * (Ver == 105 ? 24 : 16) + Tfn + Nf + static_cast<uint64_t>(Nfiles) * 16 + Tfin;
                if (Region <= N && Region < (1ull << 31)) { B.resize(static_cast<size_t>(Region)); Ok = Src->preadAll(B.data(), B.size(), At) && BsaMembers(B, U); }
            }
        }
        else { B.resize(static_cast<size_t>(N)); Ok = Src->preadAll(B.data(), B.size(), At) && PluginRecords(B, Name, U); }
    }
    if (!Ok || U.size() > (4u << 20)) { Failed.insert(Key); return nullptr; }
    //an archive's member is the same write only from the same archive (its bytes are named by the archive's CID)
    if (Kind == 'b') for (InnerUnit &X2 : U) X2.Value = X.cid + ":" + Member + ":" + X2.Value;
    return &Units.emplace(Key, std::move(U)).first->second;
}
