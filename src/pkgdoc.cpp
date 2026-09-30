#include "pkgdoc.h"

#include "cid.h"
#include "pkggraph.h"        // AddNodeRef / RemoveRefs: where a reference goes in a node's LAYERS
#include "manifestmodel.h"   // IsNodeObject: which *.json in a package dir is a node

#include <algorithm>
#include <cstdio>
#include <functional>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <fstream>
#include <random>
#include <sstream>

namespace PkgDoc
{

namespace fs = std::filesystem;

// ---- references ---------------------------------------------------------------------------------------------------

void ForEachRef(const json &Node, const std::function<void(const std::string &)> &Fn)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return;
    for (const json &L : Node["LAYERS"])
    {
        if (!L.is_object()) continue;
        for (const char *K : {"NODE", "NOT"})
            if (L.contains(K) && L[K].is_string()) Fn(L[K].get<std::string>());
        if (L.contains("ANY") && L["ANY"].is_array())
            for (const json &M : L["ANY"]) if (M.is_string()) Fn(M.get<std::string>());
    }
}

json RemapRefs(const json &Node, const std::map<std::string, std::string> &M)
{
    json Out = Node;
    if (M.empty() || !Out.is_object() || !Out.contains("LAYERS") || !Out["LAYERS"].is_array()) return Out;
    auto Swap = [&](json &V) {
        if (!V.is_string()) return;
        const auto It = M.find(V.get<std::string>());
        if (It != M.end()) V = It->second;
    };
    for (json &L : Out["LAYERS"])
    {
        if (!L.is_object()) continue;
        for (const char *K : {"NODE", "NOT"}) if (L.contains(K)) Swap(L[K]);
        if (L.contains("ANY") && L["ANY"].is_array()) for (json &V : L["ANY"]) Swap(V);
    }
    return Out;
}

bool LooksLikeCid(const std::string &S)
{
    if (S.size() < 50 || S.rfind("bafk", 0) != 0) return false;
    for (char C : S) if (!((C >= 'a' && C <= 'z') || (C >= '2' && C <= '7'))) return false;   // base32 lower
    return true;
}

namespace {

bool ReadFile(const fs::path &P, std::string &Out)
{
    std::ifstream In(P, std::ios::binary);
    if (!In) return false;
    std::ostringstream S; S << In.rdbuf();
    Out = S.str();
    return true;
}

//Write through a temporary sibling, flushed to the disk, and rename over; then flush the folder (POSIX), so the new
//name survives a power cut: a crash mid-save leaves the old file or the new one, never half and never neither.
bool WriteFileAtomic(const fs::path &P, const std::string &Bytes, std::string &Err)
{
    const fs::path Tmp = P.string() + ".tmp-save";
    std::FILE *F = std::fopen(Tmp.string().c_str(), "wb");
    if (!F) { Err = "cannot write " + Tmp.string(); return false; }
    const bool Wrote = std::fwrite(Bytes.data(), 1, Bytes.size(), F) == Bytes.size() && std::fflush(F) == 0;
#ifdef _WIN32
    const bool Synced = Wrote && _commit(_fileno(F)) == 0;
#else
    const bool Synced = Wrote && ::fsync(fileno(F)) == 0;
#endif
    const bool Closed = std::fclose(F) == 0;
    std::error_code Ec;
    if (!Wrote || !Synced || !Closed) { Err = "could not write " + Tmp.string() + " (disk full?)"; fs::remove(Tmp, Ec); return false; }
    fs::rename(Tmp, P, Ec);
    if (Ec) { Err = "cannot rename " + Tmp.string() + " -> " + P.string() + ": " + Ec.message(); fs::remove(Tmp, Ec); return false; }
#ifndef _WIN32
    if (const int D = ::open(P.parent_path().string().c_str(), O_RDONLY | O_DIRECTORY); D >= 0) { ::fsync(D); ::close(D); }
#endif
    return true;
}

std::string Stem(const fs::path &P) { return P.stem().string(); }

//Every node file under Root named by a CID, skipping friends' landed copies (`_friend_*`, never ours to rewrite)
//and the package being saved (Skip).
struct LibFile { fs::path Path; json Node; };
//False, with Err, if any part of Root could not be read: a save that cannot see every package cannot know which of
//them name the nodes it renames, so it does not start.
bool GatherLibrary(const fs::path &Root, const fs::path &Skip, std::vector<LibFile> &Out, std::string &Err)
{
    std::error_code Ec;
    if (Root.empty() || !fs::is_directory(Root, Ec)) return true;
    const fs::path SkipCanon = fs::weakly_canonical(Skip, Ec);
    auto It = fs::recursive_directory_iterator(Root, Ec);
    if (Ec) { Err = "cannot read " + Root.string() + ": " + Ec.message(); return false; }
    for (; It != fs::recursive_directory_iterator(); It.increment(Ec))
    {
        if (Ec) { Err = "cannot read the library under " + Root.string() + ": " + Ec.message(); return false; }
        const fs::path P = It->path();
        std::error_code E2;
        if (It->is_directory(E2))
        {
            if (P.filename().string().rfind("_friend_", 0) == 0 || fs::weakly_canonical(P, E2) == SkipCanon)
                It.disable_recursion_pending();
            continue;
        }
        if (P.extension() != ".json" || !LooksLikeCid(Stem(P))) continue;
        std::string Bytes;
        if (!ReadFile(P, Bytes)) { Err = "cannot read " + P.string(); return false; }
        json J = json::parse(Bytes, nullptr, /*allow_exceptions=*/false);
        if (J.is_discarded() || !ManifestModel::IsNodeObject(J)) continue;
        Out.push_back({P, std::move(J)});
    }
    if (Ec) { Err = "cannot read the library under " + Root.string() + ": " + Ec.message(); return false; }
    return true;
}

//Every string in J equal to a key of M, renamed. True if anything changed.
bool RenameStrings(json &J, const std::map<std::string, std::string> &M)
{
    bool Any = false;
    if (J.is_string())
    {
        const auto It = M.find(J.get<std::string>());
        if (It != M.end()) { J = It->second; return true; }
    }
    else if (J.is_array()) for (json &E : J) Any |= RenameStrings(E, M);
    else if (J.is_object()) for (auto &[K, V] : J.items()) Any |= RenameStrings(V, M);
    return Any;
}

} // namespace

// ---- loading ------------------------------------------------------------------------------------------------------

std::vector<std::string> Document::Load(const fs::path &Dir)
{
    std::vector<std::string> Odd;
    Entries.clear();
    LoadedFiles.clear();
    BadJson.clear();
    std::map<std::string, Pos> Declared;                  // a node's own POS, used where this machine has none
    std::vector<fs::path> Files;
    std::error_code Ec;
    for (const auto &E : fs::directory_iterator(Dir, Ec))
        if (E.is_regular_file(Ec) && E.path().extension() == ".json") Files.push_back(E.path());
    std::sort(Files.begin(), Files.end());
    std::set<std::string> Seen;
    for (const fs::path &P : Files)
    {
        std::string Bytes;
        if (!ReadFile(P, Bytes)) { Odd.push_back(P.filename().string() + ": cannot be read"); continue; }
        json J = json::parse(Bytes, nullptr, false);
        if (J.is_discarded())
        {
            BadJson.push_back(P.filename().string());
            Odd.push_back(P.filename().string() + ": not valid JSON - left alone, and not part of the package");
            continue;
        }
        if (!ManifestModel::IsNodeObject(J)) continue;   // not a node: never ours to touch
        const std::string Name = Stem(P);
        std::string H;
        if (LooksLikeCid(Name))
        {
            H = Name;
            std::string Err;
            const std::string Actual = Cid::OfNode(J, &Err);
            if (!Actual.empty() && Actual != Name)
                Odd.push_back(P.filename().string() + ": its content hashes to " + Actual
                              + ", not to its name - saving the package renames it");
        }
        else if (J.contains("CID") && J["CID"].is_string() && !J["CID"].get<std::string>().empty())
            H = J["CID"].get<std::string>();
        else
            H = Cid::OfNode(J);
        if (H.empty()) H = NewDraftHandle();
        if (!Seen.insert(H).second)
        {
            Odd.push_back(P.filename().string() + ": the same node as another file here (" + H + ") - not loaded twice");
            continue;
        }
        if (J.contains("POS") && J["POS"].is_array() && J["POS"].size() == 2 && J["POS"][0].is_number() && J["POS"][1].is_number())
            Declared[H] = Pos{J["POS"][0].get<float>(), J["POS"][1].get<float>()};
        J.erase("CID");
        J.erase("POS");                                   // presentation: the canvas holds it, never the node
        Entries.push_back({H, std::make_shared<const json>(std::move(J)), P.filename().string()});
        LoadedFiles.insert(P.filename().string());
    }
    Where = std::move(Declared);
    UndoStack.clear(); RedoStack.clear();
    Changed = false;
    ++Rev;
    ContentRev = SavedContentRev = NextContentRev++;
    Before = Take();
    return Odd;
}

void Document::Reset(std::vector<std::pair<std::string, json>> Nodes)
{
    Entries.clear();
    LoadedFiles.clear();
    Where.clear();
    for (auto &[H, J] : Nodes)
        Entries.push_back({H.empty() ? NewDraftHandle() : H, std::make_shared<const json>(std::move(J)), std::string()});
    UndoStack.clear(); RedoStack.clear();
    Changed = false;
    ++Rev;
    ContentRev = SavedContentRev = NextContentRev++;       // a fresh start: nothing to save yet
    Before = Take();
}

// ---- reading ------------------------------------------------------------------------------------------------------

int Document::IndexOf(const std::string &H) const
{
    for (int I = 0; I < Count(); ++I) if (Entries[(size_t)I].Handle == H) return I;
    return -1;
}

std::map<std::string, std::string> Document::Labels() const
{
    std::map<std::string, std::string> Out;
    for (const Entry &E : Entries)
        if (E.Node->is_object() && E.Node->contains("LABEL") && (*E.Node)["LABEL"].is_string())
            Out[E.Handle] = (*E.Node)["LABEL"].get<std::string>();
    return Out;
}

// ---- editing ------------------------------------------------------------------------------------------------------

void Document::Touch(bool Content)
{
    ++Rev;
    Changed = true;
    if (Content) ContentRev = NextContentRev++;
}

void Document::Replace(int I, json N)
{
    if (I < 0 || I >= Count()) return;
    if (*Entries[(size_t)I].Node == N) return;            // an unchanged write is not an edit
    Entries[(size_t)I].Node = std::make_shared<const json>(std::move(N));
    Touch(true);
}

int Document::Add(json N, const std::string &Handle)
{
    std::string H = Handle.empty() || IndexOf(Handle) >= 0 ? NewDraftHandle() : Handle;
    Entries.push_back({H, std::make_shared<const json>(std::move(N)), std::string()});
    Touch(true);
    return Count() - 1;
}

bool Document::Remove(int I)
{
    if (I < 0 || I >= Count()) return false;
    const std::string H = Entries[(size_t)I].Handle;
    Entries.erase(Entries.begin() + I);
    for (Entry &E : Entries)
    {
        json N = *E.Node;
        if (PkgGraph::RemoveRefs(N, H)) E.Node = std::make_shared<const json>(std::move(N));
    }
    Where.erase(H);
    Touch(true);
    return true;
}

bool Document::Link(int Child, const std::string &Parent, RefKind Kind)
{
    if (Child < 0 || Child >= Count() || Parent.empty() || Parent == Handle(Child)) return false;
    json N = Node(Child);
    bool Did = false;
    if (Kind == RefKind::Node) Did = PkgGraph::AddNodeRef(N, Parent);
    else
    {
        if (!N.contains("LAYERS") || !N["LAYERS"].is_array()) N["LAYERS"] = json::array();
        if (Kind == RefKind::Not)
        {
            bool Has = false;
            for (const json &L : N["LAYERS"]) if (L.is_object() && L.contains("NOT") && L["NOT"] == Parent) Has = true;
            if (!Has) { N["LAYERS"].push_back(json{{"NOT", Parent}}); Did = true; }
        }
        else
        {
            json *Any = nullptr;
            for (json &L : N["LAYERS"]) if (L.is_object() && L.contains("ANY") && L["ANY"].is_array()) { Any = &L; break; }
            if (!Any) { N["LAYERS"].push_back(json{{"ANY", json::array({Parent})}}); Did = true; }
            else if (std::find((*Any)["ANY"].begin(), (*Any)["ANY"].end(), json(Parent)) == (*Any)["ANY"].end())
            { (*Any)["ANY"].push_back(Parent); Did = true; }
        }
    }
    if (Did) Replace(Child, std::move(N));
    return Did;
}

// ---- positions ----------------------------------------------------------------------------------------------------

void Document::SetPos(const std::string &H, Pos P)
{
    auto It = Where.find(H);
    if (It != Where.end() && It->second.X == P.X && It->second.Y == P.Y) return;
    Where[H] = P;
    Touch(false);
}

void Document::ClearPos(const std::string &H)
{
    if (Where.erase(H)) Touch(false);
}

void Document::SetPositions(std::map<std::string, Pos> P)
{
    for (auto &[H, V] : P) Where[H] = V;                 // this machine's layout wins over a node's own POS
    ++Rev;
    Before.Where = Where;                                 // loading the layout is not an undoable step
}

// ---- undo ---------------------------------------------------------------------------------------------------------

void Document::Commit()
{
    if (!Changed) return;
    UndoStack.push_back(std::move(Before));
    if (UndoStack.size() > 200) UndoStack.erase(UndoStack.begin());
    RedoStack.clear();
    Before = Take();
    Changed = false;
}

void Document::Restore(const Snapshot &S)
{
    Entries = S.Entries;
    Where = S.Where;
    ContentRev = S.ContentRev;
    ++Rev;
}

bool Document::Undo()
{
    Commit();                                             // edits not yet committed are the step undone
    if (UndoStack.empty()) return false;
    RedoStack.push_back(Take());
    Restore(UndoStack.back());
    UndoStack.pop_back();
    Before = Take();
    return true;
}

bool Document::Redo()
{
    Commit();
    if (RedoStack.empty()) return false;
    UndoStack.push_back(Take());
    Restore(RedoStack.back());
    RedoStack.pop_back();
    Before = Take();
    return true;
}

std::map<std::string, std::string> Document::TakeRenames()
{
    std::map<std::string, std::string> Out;
    Out.swap(Renames);
    return Out;
}

std::string Document::NewDraftHandle() const
{
    static std::mt19937_64 Rng(std::random_device{}());
    for (;;)
    {
        char Buf[32];
        std::snprintf(Buf, sizeof Buf, "draft-%012llx", (unsigned long long)(Rng() & 0xffffffffffffULL));
        if (IndexOf(Buf) < 0) return Buf;
    }
}

void Document::RenameIn(std::vector<Entry> &E, std::map<std::string, Pos> &W, const std::map<std::string, std::string> &M)
{
    for (Entry &X : E)
    {
        if (const auto It = M.find(X.Handle); It != M.end()) X.Handle = It->second;
        bool Refers = false;
        ForEachRef(*X.Node, [&](const std::string &R) { if (M.count(R)) Refers = true; });
        if (Refers) X.Node = std::make_shared<const json>(RemapRefs(*X.Node, M));
    }
    std::map<std::string, Pos> NW;
    for (auto &[H, P] : W) { const auto It = M.find(H); NW[It == M.end() ? H : It->second] = P; }
    W.swap(NW);
}

// ---- saving -------------------------------------------------------------------------------------------------------

SaveReport Document::Save(const fs::path &Dir, const fs::path &LibraryRoot, const fs::path &UserDataRoot)
{
    std::vector<fs::path> Roots;
    if (!LibraryRoot.empty()) Roots.push_back(LibraryRoot);
    return Save(Dir, Roots, UserDataRoot);
}

//Saving plans everything in memory first — every file to write, rename and remove, here and across the library —
//and refuses before touching the disk when it cannot be done right: a circle, two nodes that would be one file, a
//library it could not read in full. Then it writes (journalled: a failed write undoes the ones before it), and only
//once every write has landed removes what they replace.
SaveReport Document::Save(const fs::path &Dir, const std::vector<fs::path> &Roots, const fs::path &UserDataRoot)
{
    SaveReport R;
    Commit();
    auto LabelOf = [](const json &N, const std::string &Fallback) {
        return N.is_object() && N.contains("LABEL") && N["LABEL"].is_string() ? N["LABEL"].get<std::string>() : Fallback;
    };
    for (const auto &Part : Dir)
        if (Part.string().rfind("_friend_", 0) == 0)
        { R.Error = "This is a friend's package (" + Dir.string() + "): it is theirs, and is never rewritten here."; return R; }

    // 1. This package's nodes, in the order they contain each other — a circle cannot be named (a node's name is its
    //    content, and its content names the others), so it is refused with the nodes in it.
    {
        std::map<std::string, int> ByHandle;
        for (int I = 0; I < Count(); ++I) ByHandle[Handle(I)] = I;
        std::vector<int> Missing((size_t)Count(), 0);
        std::vector<std::vector<int>> Users((size_t)Count());
        for (int I = 0; I < Count(); ++I)
        {
            std::set<int> Deps;
            ForEachRef(Node(I), [&](const std::string &Ref) { if (const auto It = ByHandle.find(Ref); It != ByHandle.end()) Deps.insert(It->second); });
            for (int D : Deps) { Users[(size_t)D].push_back(I); ++Missing[(size_t)I]; }
        }
        std::vector<int> Ready;
        int Done = 0;
        for (int I = 0; I < Count(); ++I) if (!Missing[(size_t)I]) Ready.push_back(I);
        while (!Ready.empty())
        {
            const int I = Ready.back(); Ready.pop_back(); ++Done;
            for (int U : Users[(size_t)I]) if (--Missing[(size_t)U] == 0) Ready.push_back(U);
        }
        if (Done != Count())
        {
            std::string Names;
            for (int I = 0; I < Count(); ++I)
                if (Missing[(size_t)I] > 0) Names += (Names.empty() ? "" : ", ") + LabelOf(Node(I), Handle(I));
            R.Error = "These nodes contain each other in a circle, so they cannot be saved (a node is named by its content, "
                      "and its content names the others): " + Names + ". Remove one of the wires between them.";
            return R;
        }
    }

    // 2. Everything else that could name them: every package under the roots (friends' excepted) and the instances.
    std::vector<LibFile> Lib;
    {
        std::set<fs::path> Seen;
        for (const fs::path &Root : Roots)
        {
            std::string Err;
            if (!GatherLibrary(Root, Dir, Lib, Err)) { R.Error = "Nothing was saved: " + Err; return R; }
        }
        std::vector<LibFile> Unique;
        for (LibFile &F : Lib)
        {
            std::error_code Ec;
            const fs::path C = fs::weakly_canonical(F.Path, Ec);
            if (Seen.insert(Ec ? F.Path : C).second) Unique.push_back(std::move(F));
        }
        Lib.swap(Unique);
        std::sort(Lib.begin(), Lib.end(), [](const LibFile &A, const LibFile &B) { return A.Path < B.Path; });
    }
    struct Instance { fs::path Path; json J; };
    std::vector<Instance> Instances;
    {
        std::error_code Ec, Probe;
        if (!UserDataRoot.empty() && fs::is_directory(UserDataRoot, Probe))
            for (const auto &Pkg : fs::directory_iterator(UserDataRoot, Ec))
            {
                if (!Pkg.is_directory(Probe)) continue;
                for (const auto &Inst : fs::directory_iterator(Pkg.path(), Ec))
                {
                    const fs::path Cfg = Inst.path() / "instance.json";
                    if (!Inst.is_directory(Probe) || !fs::exists(Cfg, Probe)) continue;
                    std::string B;
                    if (!ReadFile(Cfg, B)) { R.Error = "Nothing was saved: cannot read " + Cfg.string(); return R; }
                    json J = json::parse(B, nullptr, false);
                    if (!J.is_discarded()) Instances.push_back({Cfg, std::move(J)});
                }
                if (Ec) break;
            }
        if (Ec) { R.Error = "Nothing was saved: cannot read the instances under " + UserDataRoot.string() + ": " + Ec.message(); return R; }
    }

    // 3. One graph: this package's nodes and every library node that contains one of them (or a copy of one, or a
    //    node that does, transitively). Each is named only once everything it contains has its final name — through
    //    other packages too (this package's node P can contain E in another package, which contains this package's X).
    std::map<std::string, std::vector<size_t>> ByName, Referrers;
    for (size_t K = 0; K < Lib.size(); ++K)
    {
        ByName[Stem(Lib[K].Path)].push_back(K);
        ForEachRef(Lib[K].Node, [&](const std::string &Ref) { Referrers[Ref].push_back(K); });
    }
    std::set<size_t> LibPending;
    {
        std::vector<std::string> Frontier;
        for (int I = 0; I < Count(); ++I) Frontier.push_back(Handle(I));
        while (!Frontier.empty())
        {
            const std::string C = Frontier.back();
            Frontier.pop_back();
            for (const auto *Hits : {&ByName[C], &Referrers[C]})
                for (size_t K : *Hits)
                    if (LibPending.insert(K).second) Frontier.push_back(Stem(Lib[K].Path));
        }
    }
    std::set<int> DocPending;
    for (int I = 0; I < Count(); ++I) DocPending.insert(I);
    std::map<std::string, int> PendingNames;              // name -> pending items carrying it (copies share one)
    std::set<std::string> DocPendingNames;
    for (int I : DocPending) { ++PendingNames[Handle(I)]; DocPendingNames.insert(Handle(I)); }
    for (size_t K : LibPending) ++PendingNames[Stem(Lib[K].Path)];
    auto Settle = [&](const std::string &Name) { if (--PendingNames[Name] == 0) PendingNames.erase(Name); };

    std::map<std::string, std::string> M;                 // old name -> new CID, everything that changes
    std::map<std::string, std::string> Bytes;             // cid -> canonical bytes
    std::vector<std::string> CidOf((size_t)Count());
    std::map<std::string, int> MintedBy;                  // cid -> the node of this package it names
    struct Write { fs::path Path; std::string Bytes; };
    std::vector<Write> Writes;
    std::vector<fs::path> Replaced;
    std::map<std::string, std::string> LibNew;            // library file path -> its new cid (renamed ones)
    while (!DocPending.empty() || !LibPending.empty())
    {
        bool Progress = false;
        auto Waits = [&](const json &N) {
            bool W = false;
            ForEachRef(N, [&](const std::string &Ref) { if (PendingNames.count(Ref)) W = true; });
            return W;
        };
        for (auto It = DocPending.begin(); It != DocPending.end();)
        {
            const int I = *It;
            if (Waits(Node(I))) { ++It; continue; }
            const json Out = RemapRefs(Node(I), M);
            std::string Err;
            const std::string C = Cid::OfNode(Out, &Err);
            if (C.empty()) { R.Error = "Node '" + LabelOf(Node(I), Handle(I)) + "' cannot be saved: " + Err; return R; }
            if (const auto Twin = MintedBy.find(C); Twin != MintedBy.end())
            {
                R.Error = "'" + LabelOf(Node(Twin->second), Handle(Twin->second)) + "' and '" + LabelOf(Node(I), Handle(I))
                        + "' are the same node, byte for byte, so they would be one file. Delete one, or make them differ.";
                return R;
            }
            MintedBy[C] = I;
            CidOf[(size_t)I] = C;
            Bytes[C] = Cid::Canonical(Out);
            if (Handle(I) != C) M[Handle(I)] = C;
            Settle(Handle(I));
            DocPendingNames.erase(Handle(I));
            It = DocPending.erase(It);
            Progress = true;
        }
        for (auto It = LibPending.begin(); It != LibPending.end();)
        {
            const size_t K = *It;
            const std::string Old = Stem(Lib[K].Path);
            if (DocPendingNames.count(Old) || Waits(Lib[K].Node)) { ++It; continue; }   // a copy follows its original
            std::string New;
            if (const auto Copy = M.find(Old); Copy != M.end() && Bytes.count(Copy->second)) New = Copy->second;
            else
            {
                const json Out = RemapRefs(Lib[K].Node, M);
                std::string Err;
                New = Cid::OfNode(Out, &Err);
                if (New.empty()) { R.Error = "Nothing was saved: " + Lib[K].Path.string() + ": " + Err; return R; }
                Bytes[New] = Cid::Canonical(Out);
            }
            if (New != Old)
            {
                M[Old] = New;
                LibNew[Lib[K].Path.string()] = New;
                Writes.push_back({Lib[K].Path.parent_path() / (New + ".json"), Bytes[New]});
                Replaced.push_back(Lib[K].Path);
                ++R.Cascaded;
            }
            Settle(Old);
            It = LibPending.erase(It);
            Progress = true;
        }
        if (!Progress)
        {
            std::string Names;
            int Shown = 0;
            for (int I : DocPending) if (Shown++ < 6) Names += (Names.empty() ? "" : ", ") + LabelOf(Node(I), Handle(I));
            for (size_t K : LibPending) if (Shown++ < 6) Names += (Names.empty() ? "" : ", ") + Lib[K].Path.string();
            R.Error = "Nothing was saved: these nodes contain each other in a circle, through other packages — " + Names;
            return R;
        }
    }
    R.Renamed = M;

    // 4. This package's files: every node under its CID; what the edits replace and what was deleted goes.
    std::set<std::string> Claimed;
    for (int I = 0; I < Count(); ++I)
    {
        Writes.push_back({Dir / (CidOf[(size_t)I] + ".json"), Bytes[CidOf[(size_t)I]]});
        Claimed.insert(CidOf[(size_t)I] + ".json");
        if (!File(I).empty() && File(I) != CidOf[(size_t)I] + ".json") Replaced.push_back(Dir / File(I));
    }
    //What still names a node, once everything is renamed: the library's nodes (as they will be) and the instances.
    std::set<std::string> NamedElsewhere;
    for (const LibFile &F : Lib) ForEachRef(RemapRefs(F.Node, M), [&](const std::string &Ref) { NamedElsewhere.insert(Ref); });
    for (Instance &In : Instances)
    {
        json J = In.J;
        RenameStrings(J, M);
        std::function<void(const json &)> Collect = [&](const json &V) {
            if (V.is_string()) NamedElsewhere.insert(V.get<std::string>());
            else if (V.is_array() || V.is_object()) for (const auto &E : V) Collect(E);
        };
        Collect(J);
    }
    for (const std::string &F : LoadedFiles)
    {
        if (Claimed.count(F) || std::find(Replaced.begin(), Replaced.end(), Dir / F) != Replaced.end()) continue;
        const std::string C = fs::path(F).stem().string();
        bool CopyElsewhere = false;                        // another package keeps this node's file under its name
        for (size_t K : ByName[C]) if (!LibNew.count(Lib[K].Path.string())) CopyElsewhere = true;
        if (NamedElsewhere.count(C) && !CopyElsewhere)
        {
            R.Kept.push_back(C);
            R.Log.push_back("kept " + (Dir / F).string() + ": deleted here, but another package or an instance still names "
                            "it, and this is its only copy");
        }
        else Replaced.push_back(Dir / F);
    }
    for (Instance &In : Instances)
    {
        json J = In.J;
        if (!RenameStrings(J, M)) continue;
        Writes.push_back({In.Path, J.dump(4)});
        ++R.InstancesUpdated;
    }

    // 5. Write, journalled: a write that fails undoes every one before it, and nothing is removed.
    struct Undo { fs::path Path; bool Existed; std::string Old; };
    std::vector<Undo> Journal;
    std::set<fs::path> Final;                             // every file the package state ends up with, written or not
    for (const Write &W : Writes) Final.insert(W.Path);
    for (const Write &W : Writes)
    {
        std::string Have;
        const bool Existed = ReadFile(W.Path, Have);
        if (Existed && Have == W.Bytes) continue;         // already there, byte for byte
        std::string Err;
        if (!WriteFileAtomic(W.Path, W.Bytes, Err))
        {
            for (auto It = Journal.rbegin(); It != Journal.rend(); ++It)
            {
                std::string E2;
                std::error_code Ec;
                if (It->Existed) WriteFileAtomic(It->Path, It->Old, E2);
                else fs::remove(It->Path, Ec);
            }
            R.Error = "Nothing was saved: " + Err;
            R.Written = 0; R.Cascaded = 0; R.InstancesUpdated = 0; R.Renamed.clear(); R.Kept.clear(); R.Log.clear();
            return R;
        }
        Journal.push_back({W.Path, Existed, std::move(Have)});
        ++R.Written;
        R.Log.push_back((W.Path.filename() == "instance.json" ? "renamed node references in " : "wrote ") + W.Path.string());
    }

    // 6. Every write landed: remove what they replace — never a file something ends up as.
    std::set<fs::path> Gone;
    for (const fs::path &P : Replaced)
    {
        if (Final.count(P) || !Gone.insert(P).second) continue;
        std::error_code Ec;
        if (fs::remove(P, Ec)) { ++R.Removed; R.Log.push_back("removed " + P.string()); }
        else if (Ec)
        {
            R.Warnings.push_back("could not remove " + P.string() + " (" + Ec.message() + "): the old version of the node is "
                                 "still there beside the new one - remove it by hand");
            R.Log.push_back(R.Warnings.back());
        }
    }

    // 7. The document is now what is on disk: handles are the CIDs, in the undo history too.
    for (int I = 0; I < Count(); ++I) Entries[(size_t)I].File = CidOf[(size_t)I] + ".json";
    LoadedFiles.clear();
    std::error_code Ec;
    for (const auto &E : fs::directory_iterator(Dir, Ec))
        if (E.path().extension() == ".json" && LooksLikeCid(Stem(E.path()))) LoadedFiles.insert(E.path().filename().string());
    RenameIn(Entries, Where, M);
    for (Snapshot &S : UndoStack) RenameIn(S.Entries, S.Where, M);
    for (Snapshot &S : RedoStack) RenameIn(S.Entries, S.Where, M);
    RenameIn(Before.Entries, Before.Where, M);
    SavedContentRev = ContentRev;
    ++Rev;
    for (auto &[Old, New] : M) Renames[Old] = New;
    R.Ok = true;
    return R;
}

}
