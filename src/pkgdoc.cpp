#include "pkgdoc.h"

#include "cid.h"
#include "pkggraph.h"        // AddNodeRef / RemoveRefs: where a reference goes in a node's LAYERS
#include "manifestmodel.h"   // IsNodeObject: which *.json in a package dir is a node

#include <algorithm>
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

//Write through a temporary sibling and rename over: a crash mid-save leaves the old file or the new one, never half.
bool WriteFileAtomic(const fs::path &P, const std::string &Bytes, std::string &Err)
{
    const fs::path Tmp = P.string() + ".tmp-save";
    {
        std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
        if (!Out) { Err = "cannot write " + Tmp.string(); return false; }
        Out.write(Bytes.data(), (std::streamsize)Bytes.size());
        if (!Out) { Err = "short write to " + Tmp.string(); return false; }
    }
    std::error_code Ec;
    fs::rename(Tmp, P, Ec);
    if (Ec) { Err = "cannot rename " + Tmp.string() + " -> " + P.string() + ": " + Ec.message(); fs::remove(Tmp, Ec); return false; }
    return true;
}

std::string Stem(const fs::path &P) { return P.stem().string(); }

//Every node file under Root named by a CID, skipping friends' landed copies (`_friend_*`, never ours to rewrite)
//and the package being saved (Skip).
struct LibFile { fs::path Path; json Node; };
std::vector<LibFile> GatherLibrary(const fs::path &Root, const fs::path &Skip)
{
    std::vector<LibFile> Out;
    std::error_code Ec;
    if (Root.empty() || !fs::is_directory(Root, Ec)) return Out;
    const fs::path SkipCanon = fs::weakly_canonical(Skip, Ec);
    for (auto It = fs::recursive_directory_iterator(Root, fs::directory_options::skip_permission_denied, Ec);
         It != fs::recursive_directory_iterator(); It.increment(Ec))
    {
        if (Ec) break;
        const fs::path &P = It->path();
        if (It->is_directory(Ec))
        {
            std::error_code E2;
            if (P.filename().string().rfind("_friend_", 0) == 0 || fs::weakly_canonical(P, E2) == SkipCanon)
                It.disable_recursion_pending();
            continue;
        }
        if (P.extension() != ".json" || !LooksLikeCid(Stem(P))) continue;
        std::string Bytes;
        if (!ReadFile(P, Bytes)) continue;
        json J = json::parse(Bytes, nullptr, /*allow_exceptions=*/false);
        if (J.is_discarded() || !ManifestModel::IsNodeObject(J)) continue;
        Out.push_back({P, std::move(J)});
    }
    std::sort(Out.begin(), Out.end(), [](const LibFile &A, const LibFile &B) { return A.Path < B.Path; });
    return Out;
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
    SaveReport R;
    Commit();

    // 1. Order the package's nodes so every node comes after what it names: a node's name includes its references'.
    std::map<std::string, int> ByHandle;
    for (int I = 0; I < Count(); ++I) ByHandle[Handle(I)] = I;
    std::vector<std::vector<int>> Needs((size_t)Count());
    std::vector<int> Missing((size_t)Count(), 0);
    std::vector<std::vector<int>> Users((size_t)Count());
    for (int I = 0; I < Count(); ++I)
    {
        std::set<int> Deps;
        ForEachRef(Node(I), [&](const std::string &Ref) {
            const auto It = ByHandle.find(Ref);
            if (It != ByHandle.end()) Deps.insert(It->second);
        });
        for (int D : Deps) { Users[(size_t)D].push_back(I); ++Missing[(size_t)I]; }
    }
    std::vector<int> Order, Ready;
    for (int I = 0; I < Count(); ++I) if (!Missing[(size_t)I]) Ready.push_back(I);
    while (!Ready.empty())
    {
        const int I = Ready.back(); Ready.pop_back();
        Order.push_back(I);
        for (int U : Users[(size_t)I]) if (--Missing[(size_t)U] == 0) Ready.push_back(U);
    }
    if ((int)Order.size() != Count())
    {
        std::string Names;
        for (int I = 0; I < Count(); ++I)
            if (Missing[(size_t)I] > 0)
            {
                const json &N = Node(I);
                const std::string L = N.is_object() && N.contains("LABEL") && N["LABEL"].is_string() ? N["LABEL"].get<std::string>() : Handle(I);
                Names += (Names.empty() ? "" : ", ") + L;
            }
        R.Error = "These nodes contain each other in a circle, so they cannot be saved (a node is named by its content, "
                  "and its content names the others): " + Names + ". Remove one of the wires between them.";
        return R;
    }

    // 2. Name each node by its bytes, with the references it makes already renamed.
    std::map<std::string, std::string> Minted;            // handle -> cid
    std::map<std::string, std::string> Bytes;             // cid -> canonical bytes
    std::vector<std::string> CidOf((size_t)Count());
    for (int I : Order)
    {
        const json Out = RemapRefs(Node(I), Minted);
        std::string Err;
        const std::string C = Cid::OfNode(Out, &Err);
        if (C.empty())
        {
            const json &N = Node(I);
            R.Error = "Node '" + (N.contains("LABEL") && N["LABEL"].is_string() ? N["LABEL"].get<std::string>() : Handle(I))
                    + "' cannot be saved: " + Err;
            return R;
        }
        Minted[Handle(I)] = C;
        CidOf[(size_t)I] = C;
        Bytes[C] = Cid::Canonical(Out);
    }

    // 3. Write this package's changed files; remember the ones they replace.
    std::error_code Ec;
    std::set<fs::path> Written, Replaced;
    auto Put = [&](const fs::path &P, const std::string &B) {
        std::string Have;
        if (ReadFile(P, Have) && Have == B) return true;  // already there, byte for byte
        std::string Err;
        if (!WriteFileAtomic(P, B, Err)) { R.Error = Err; return false; }
        ++R.Written;
        R.Log.push_back("wrote " + P.string());
        Written.insert(P);
        return true;
    };
    for (int I = 0; I < Count(); ++I)
    {
        const fs::path Target = Dir / (CidOf[(size_t)I] + ".json");
        if (!Put(Target, Bytes[CidOf[(size_t)I]])) return R;
        const std::string &Old = File(I);
        if (!Old.empty() && Old != Target.filename().string()) Replaced.insert(Dir / Old);
        if (Handle(I) != CidOf[(size_t)I]) R.Renamed[Handle(I)] = CidOf[(size_t)I];
    }

    // Nodes removed in the editor: their files go — unless another package still names them (checked below, once
    // the library has been read), because deleting a node others contain would leave them naming nothing.
    std::set<std::string> Claimed;
    for (int I = 0; I < Count(); ++I) Claimed.insert(CidOf[(size_t)I] + ".json");
    std::vector<std::string> Dropped;
    for (const std::string &F : LoadedFiles)
        if (!Claimed.count(F) && !Replaced.count(Dir / F)) Dropped.push_back(F);

    // 4. Follow the renames through the library: referrers are re-minted (recursively), copies renamed.
    std::map<std::string, std::string> M;                 // old cid -> new cid, across the library
    for (auto &[Old, New] : R.Renamed) if (LooksLikeCid(Old)) M[Old] = New;
    if (!M.empty() && !LibraryRoot.empty())
    {
        std::vector<LibFile> Lib = GatherLibrary(LibraryRoot, Dir);
        //Which files change: copies of a renamed node, and every node containing a changed one, transitively. A node
        //is re-minted only once everything it contains that changes has its final name — else a node reached twice
        //(a diamond: it contains a changed node directly AND through another) would be written naming the other's
        //OLD name. CIDs cannot form a circle, so this order always exists.
        std::map<std::string, std::vector<size_t>> ByName, Referrers;
        for (size_t K = 0; K < Lib.size(); ++K)
        {
            ByName[Stem(Lib[K].Path)].push_back(K);
            ForEachRef(Lib[K].Node, [&](const std::string &Ref) { Referrers[Ref].push_back(K); });
        }
        std::set<size_t> Pending;
        std::vector<std::string> Frontier;
        for (auto &[Old, New] : M) Frontier.push_back(Old);
        while (!Frontier.empty())
        {
            const std::string C = Frontier.back();
            Frontier.pop_back();
            for (const auto *Hits : {&ByName[C], &Referrers[C]})
                for (size_t K : *Hits)
                    if (Pending.insert(K).second) Frontier.push_back(Stem(Lib[K].Path));
        }
        std::map<std::string, int> PendingNames;          // name -> pending files carrying it (copies share one)
        for (size_t K : Pending) ++PendingNames[Stem(Lib[K].Path)];
        while (!Pending.empty())
        {
            std::vector<size_t> Final;
            for (size_t K : Pending)
            {
                bool Waits = false;
                ForEachRef(Lib[K].Node, [&](const std::string &Ref) { if (PendingNames.count(Ref)) Waits = true; });
                if (!Waits) Final.push_back(K);
            }
            if (Final.empty()) { R.Error = "The library's nodes contain each other in a circle: cannot follow the renames"; return R; }
            for (size_t K : Final)
            {
                Pending.erase(K);
                const std::string OldName = Stem(Lib[K].Path);
                if (--PendingNames[OldName] == 0) PendingNames.erase(OldName);
                const auto Copy = M.find(OldName);
                std::string NewName, NewBytes;
                if (Copy != M.end() && Bytes.count(Copy->second))
                {
                    NewName = Copy->second;                // another package keeps a copy of a node renamed already
                    NewBytes = Bytes[NewName];
                }
                else
                {
                    const json Out = RemapRefs(Lib[K].Node, M);
                    std::string Err;
                    NewName = Cid::OfNode(Out, &Err);
                    if (NewName.empty()) { R.Error = Lib[K].Path.string() + ": " + Err; return R; }
                    NewBytes = Cid::Canonical(Out);
                    Bytes[NewName] = NewBytes;
                }
                if (NewName == OldName) continue;
                if (!Put(Lib[K].Path.parent_path() / (NewName + ".json"), NewBytes)) return R;
                Replaced.insert(Lib[K].Path);
                M[OldName] = NewName;
                R.Renamed[OldName] = NewName;
                ++R.Cascaded;
            }
        }
    }

    if (!Dropped.empty())
    {
        std::set<std::string> NamedElsewhere;
        for (const LibFile &F : GatherLibrary(LibraryRoot, Dir))
            ForEachRef(F.Node, [&](const std::string &Ref) { NamedElsewhere.insert(Ref); });
        for (const std::string &F : Dropped)
        {
            const std::string C = fs::path(F).stem().string();
            if (NamedElsewhere.count(C) && !M.count(C))
                R.Log.push_back("kept " + (Dir / F).string() + ": removed here, but another package still contains it");
            else Replaced.insert(Dir / F);
        }
    }

    // 5. Instances remember nodes by CID (the grafts ticked per tile, the runner chain): follow the renames there.
    if (!M.empty() && !UserDataRoot.empty() && fs::is_directory(UserDataRoot, Ec))
        for (const auto &Pkg : fs::directory_iterator(UserDataRoot, Ec))
        {
            if (!Pkg.is_directory(Ec)) continue;
            for (const auto &Inst : fs::directory_iterator(Pkg.path(), Ec))
            {
                const fs::path Cfg = Inst.path() / "instance.json";
                std::string B;
                if (!Inst.is_directory(Ec) || !ReadFile(Cfg, B)) continue;
                json J = json::parse(B, nullptr, false);
                if (J.is_discarded() || !RenameStrings(J, M)) continue;
                std::string Err;
                if (!WriteFileAtomic(Cfg, J.dump(4), Err)) { R.Error = Err; return R; }
                ++R.InstancesUpdated;
                R.Log.push_back("renamed node references in " + Cfg.string());
            }
        }

    // 6. Every write landed: only now remove what they replace.
    for (const fs::path &P : Replaced)
    {
        if (Written.count(P)) continue;                   // a replaced name that is also a new one (two swapped)
        bool StillOurs = false;
        for (int I = 0; I < Count(); ++I) if (Dir / (CidOf[(size_t)I] + ".json") == P) StillOurs = true;
        if (StillOurs) continue;
        if (fs::remove(P, Ec)) { ++R.Removed; R.Log.push_back("removed " + P.string()); }
    }

    // 7. The document is now what is on disk: handles are the CIDs, in the undo history too.
    for (int I = 0; I < Count(); ++I) Entries[(size_t)I].File = CidOf[(size_t)I] + ".json";
    LoadedFiles.clear();
    for (const auto &E : fs::directory_iterator(Dir, Ec))
        if (E.path().extension() == ".json" && LooksLikeCid(Stem(E.path()))) LoadedFiles.insert(E.path().filename().string());
    std::map<std::string, std::string> Here;
    for (auto &[Old, New] : R.Renamed) Here[Old] = New;
    RenameIn(Entries, Where, Here);
    for (Snapshot &S : UndoStack) RenameIn(S.Entries, S.Where, Here);
    for (Snapshot &S : RedoStack) RenameIn(S.Entries, S.Where, Here);
    RenameIn(Before.Entries, Before.Where, Here);
    SavedContentRev = ContentRev;
    ++Rev;
    for (auto &[Old, New] : Here) Renames[Old] = New;
    R.Ok = true;
    return R;
}

}
