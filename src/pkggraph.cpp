#include "pkggraph.h"

#include "commonutils.h"   // Log

#include "fold.h"        // Fold::TypeOf — a layer's one type key

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using json = nlohmann::ordered_json;

namespace PkgGraph
{

// ---- graph ----------------------------------------------------------------

//An id safe to put in a log line. NODE_ID is arbitrary on-disk or peer JSON with no character validation,
//and a log line is how this codebase reports verdicts — a control character in one can forge a record, and an
//escape sequence can drive the terminal reading it. Strips every C0 control (not just the three whitespace
//ones) and caps the length. NOT a claim that the whole codebase is hardened: the other sites that log an id
//are listed in the review that prompted this and are untouched; this covers the two that this work added.
std::string SafeId(const std::string &Id)
{
    std::string Out;
    Out.reserve(std::min<size_t>(Id.size(), 96));
    int Owed = 0;                       // continuation bytes the last lead byte still expects
    for (size_t I = 0; I < Id.size(); ++I)
    {
        const unsigned char C = static_cast<unsigned char>(Id[I]);
        //Truncate only on a character BOUNDARY. Cutting at a fixed byte count can land inside a multi-byte
        //sequence and emit a lone lead byte, which is invalid UTF-8 — a log line a JSON or terminal consumer
        //downstream can choke on, produced BY the sanitiser.
        //
        //But a continuation byte is only allowed past the cap when a LEAD byte is still owed one. Exempting
        //every 0x80..0xBF byte unconditionally meant 96 'x' followed by a thousand orphan continuations
        //produced a 1096-byte output: no cap at all, and the tail was itself invalid UTF-8, passed through
        //verbatim because those bytes are >= 0x20. Every caller today feeds an id that came through
        //nlohmann::parse, which rejects invalid UTF-8 — but this function's own header calls its input hostile
        //and promises a cap, so it keeps the promise without relying on who happens to call it.
        //A byte that is not a continuation STARTS a character, so nothing is owed at it — including the case
        //that closed the previous hole and opened this one: a run of lead bytes, each setting a debt the next
        //one never paid, so `Owed` never returned to zero and the cap never fired. Measured before this fix:
        //95 'x' followed by a thousand 0xF0 came back as 2095 bytes, the lone leads passed through verbatim
        //because they are >= 0x20. Zero the debt FIRST, test the cap, then record what this byte owes.
        const bool IsCont = (C & 0xC0) == 0x80;
        if (!IsCont) Owed = 0;
        if (Out.size() >= 96 && Owed == 0) { Out += "..."; break; }
        if (IsCont)                  { if (Owed) --Owed; }
        else if ((C & 0xE0) == 0xC0) Owed = 1;
        else if ((C & 0xF0) == 0xE0) Owed = 2;
        else if ((C & 0xF8) == 0xF0) Owed = 3;
        //C1 controls too, not just C0. They arrive as UTF-8 (0xC2 0x80..0x9F) and 0x9B is CSI — the same
        //escape introducer ESC-[ is, so an id carrying it can move the cursor and colour the terminal of
        //anyone reading these logs. Filtering only bytes < 0x20 let every one of them straight through.
        if (C == 0xC2 && I + 1 < Id.size()
            && static_cast<unsigned char>(Id[I + 1]) >= 0x80 && static_cast<unsigned char>(Id[I + 1]) <= 0x9F)
        { Out += '?'; ++I; Owed = 0; continue; }   //a whole character was consumed; it owes nothing
        Out += (C < 0x20 || C == 0x7f) ? '?' : Id[I];
    }
    return Out;
}

int StringListFault(const json *V)
{
    //Absent and null are not faults: they are an unset field, which the editor materialises on the first
    //keystroke because there is nothing there to lose.
    if (!V || V->is_null()) return kStringListOk;
    if (!V->is_array())     return kStringListNotAList;
    //ELEMENTS too, because the ROUND TRIP is what destroys. ListToText silently drops an entry that is not a
    //string, the author types one character, TextToList writes back what is left, and the dropped entry is
    //gone — through the same widget, without the value ever having looked wrong.
    //A null ENTRY is a fault like any other. Absent and null are "nothing to lose" for the FIELD, because
    //there is no list there at all; inside a list, a null is an entry the author wrote and ListToText drops,
    //which is the same silent destruction as any other wrong type.
    for (int I = 0; I < (int)V->size(); ++I)
        if (!V->at((size_t)I).is_string()) return I;
    return kStringListOk;
}

std::string DescribeValue(const json &V, size_t MaxChars)
{
    if (V.is_object()) return "an object of " + std::to_string(V.size()) + " key(s)";
    if (V.is_array())  return "a list of " + std::to_string(V.size()) + " entry(s)";
    if (V.is_null())   return "null";
    //Numbers and booleans are bounded by their own type, so dump() is safe on them and nothing else here.
    //get_ref, not get<std::string>(): the latter COPIES, so a 10 MB malformed string field was a 10 MB
    //allocation per frame per visible node — half the hazard this function exists to avoid, reintroduced
    //inside it. Only the prefix is ever needed, so only the prefix is taken.
    std::string S = V.is_string() ? V.get_ref<const std::string &>().substr(0, MaxChars + 4) : V.dump();
    if (S.size() > MaxChars)
    {
        //Cut on a character boundary, for the same reason SafeId does: a lone lead byte is invalid UTF-8
        //produced by the thing whose job is to make the value safe to show.
        size_t Cut = MaxChars;
        while (Cut > 0 && (static_cast<unsigned char>(S[Cut]) & 0xC0) == 0x80) --Cut;
        S.resize(Cut);
        S += "...";
    }
    return V.is_string() ? "\"" + SafeId(S) + "\"" : SafeId(S);
}

bool WritableObject(json &Node, const char *Key)
{
    if (!Node.is_object()) return false;
    if (!Node.contains(Key) || Node[Key].is_null()) { Node[Key] = json::object(); return true; }
    return Node[Key].is_object();
}

bool WriteSubKey(json &Node, const char *Key, const std::string &Sub, const json &Value)
{
    if (!WritableObject(Node, Key)) return false;
    Node[Key][Sub] = Value;
    return true;
}

Graph Build(const std::vector<NodeRef> &Nodes)
{
    Graph G;
    std::map<std::string, int> ByHandle;
    for (const NodeRef &R : Nodes)
    {
        //NOT skipped when malformed: G.Nodes[i] MUST correspond to Nodes[i] (the canvas addresses both by index).
        Node Nd;
        Nd.Index = (int)G.Nodes.size();
        Nd.Id = R.Handle;
        if (R.Json && R.Json->is_object())
        {
            Nd.Type = KindOf(*R.Json);
            //The node's first content layer is its Form: the delta-base match and the content actions key on it.
            for (const std::string &T : LayerTypes(*R.Json))
                if (T == "ZIP" || T == "DIR" || T == "FILE" || T == "DELTA")
                { Nd.Form = T; for (char &C : Nd.Form) C = (char)std::tolower((unsigned char)C); break; }
        }
        if (!Nd.Id.empty()) ByHandle[Nd.Id] = Nd.Index;
        G.Nodes.push_back(std::move(Nd));
    }

    std::set<std::string> SeenExternal;
    for (int I = 0; I < (int)Nodes.size(); ++I)
    {
        if (!Nodes[(size_t)I].Json) continue;
        const json &N = *Nodes[(size_t)I].Json;
        if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) continue;
        //One wire per REFERENCE, flattened in LAYERS order (EraseRef walks the same order). TOTAL over malformed
        //layers: a wrong-shaped reference OCCUPIES a slot but draws no wire — so a slot always addresses the real
        //reference, and detaching never erases a neighbour.
        int Slot = -1, LayerIndex = -1;
        auto Wire = [&](const json &Ref, int Member, bool Any, bool Not) {
            ++Slot;
            if (!Ref.is_string() || Ref.get<std::string>().empty()) return;
            const std::string Pid = Ref.get<std::string>();
            Link L; L.ChildIndex = I; L.Slot = Slot; L.Layer = LayerIndex; L.Member = Member; L.Any = Any; L.Not = Not;
            const auto It = ByHandle.find(Pid);
            if (It != ByHandle.end()) L.ParentIndex = It->second;
            else
            {
                L.ExternalId = Pid;                      // another package's node: drawn as a reference chip
                if (SeenExternal.insert(Pid).second) G.Externals.push_back(Pid);
            }
            G.Links.push_back(std::move(L));
        };
        for (const auto &Ly : N["LAYERS"])
        {
            ++LayerIndex;
            const std::string T = LayerType(Ly);
            if (T == "NODE") Wire(Ly["NODE"], -1, false, false);
            else if (T == "NOT") Wire(Ly["NOT"], -1, false, true);
            else if (T == "ANY")
            {
                if (!Ly["ANY"].is_array()) { Wire(json(), -1, true, false); continue; }   // malformed: a slot, no wire
                int M = -1;
                for (const auto &Mm : Ly["ANY"]) { ++M; Wire(Mm, M, true, false); }
            }
        }
    }
    for (Node &Nd : G.Nodes) Nd.HasDeltaBase = Nd.Form == "zip" && !DeltaBase(Nodes, Nd.Index).empty();
    return G;
}

// ---- layer vocabulary -----------------------------------------------------

const std::vector<TypeGroup> &TypeGroups()
{
    static const std::vector<TypeGroup> G = {
        {"Content", {"ZIP", "DIR", "FILE", "DELTA"}},
        {"Transforms", {"EDIT", "REG", "DLL", "ENV"}},
        {"Facts", {"VARS", "KEEP"}},
        {"Entries", {"EXEC"}},
        {"Composition", {"NODE", "ANY", "NOT"}},
    };
    return G;
}

std::string LayerType(const json &Layer) { return Fold::TypeOf(Layer); }

std::vector<std::string> LayerTypes(const json &Node)
{
    std::vector<std::string> Out;
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return Out;
    for (const auto &L : Node["LAYERS"]) Out.push_back(LayerType(L));
    return Out;
}

int ContentIndex(const json &Node)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return -1;
    for (int I = 0; I < (int)Node["LAYERS"].size(); ++I)
    {
        const std::string T = LayerType(Node["LAYERS"][(size_t)I]);
        if (T == "ZIP" || T == "FILE" || T == "DELTA" || T == "DIR") return I;
    }
    return -1;
}

std::string ContentType(const json &Node)
{
    const int I = ContentIndex(Node);
    return I < 0 ? std::string() : LayerType(Node["LAYERS"][(size_t)I]);
}

static std::string StrField(const json &O, const std::string &K)
{
    return (O.is_object() && O.contains(K) && O[K].is_string()) ? O[K].get<std::string>() : std::string();
}

std::string ContentName(const json &Node)
{
    const int I = ContentIndex(Node);
    return I < 0 ? std::string() : StrField(Node["LAYERS"][(size_t)I], ContentType(Node));
}

std::string ContentTarget(const json &Node)
{
    const int I = ContentIndex(Node);
    return I < 0 ? std::string() : StrField(Node["LAYERS"][(size_t)I], "TARGET");
}

std::string DeltaBase(const std::vector<NodeRef> &Nodes, int Index)
{
    if (Index < 0 || Index >= (int)Nodes.size() || !Nodes[(size_t)Index].Json) return {};
    const json &N = *Nodes[(size_t)Index].Json;
    const int Own = ContentIndex(N);
    if (Own < 0 || ContentName(N).empty()) return {};   // nothing of its own to diff yet
    const std::string Target = ContentTarget(N);
    for (int I = Own - 1; I >= 0; --I)
    {
        const json &L = N["LAYERS"][(size_t)I];
        if (LayerType(L) != "NODE" || L.contains("TAKE") || L.contains("TARGET")) continue;
        const std::string Ref = StrField(L, "NODE");
        for (const NodeRef &C : Nodes)
            if (!Ref.empty() && C.Handle == Ref && C.Json && ContentType(*C.Json) == "ZIP" && ContentTarget(*C.Json) == Target
                && !ContentName(*C.Json).empty())
                return ContentName(*C.Json);
    }
    return {};
}

std::string KindOf(const json &Node)
{
    bool Refs = false;
    for (const std::string &T : LayerTypes(Node))
    {
        if (T.empty()) continue;
        if (T == "NODE" || T == "ANY" || T == "NOT") { Refs = true; continue; }
        return T;
    }
    return Refs ? std::string("NODE") : std::string();
}

const char *TypeHelp(const std::string &Type)
{
    if (Type == "ZIP")   return "An archive mounted at TARGET (STORE zips only - they are mounted, never unpacked).";
    if (Type == "DIR")   return "A folder mounted at TARGET - local content, or a %RunnerMount%/... path at launch.";
    if (Type == "FILE")  return "One file mounted INTO the TARGET directory under its own name.";
    if (Type == "DELTA") return "A binary delta rebuilding an archive from the nearest content beneath it at TARGET.";
    if (Type == "NODE")  return "Contain another node here: its layers fold in at this position. TAKE selects/renames, TARGET places.";
    if (Type == "ANY")   return "A requirement: one of these nodes must be present. First in the list = a graft onto them.";
    if (Type == "NOT")   return "An exclusion: this node must NOT be present - the row is blocked if it is.";
    if (Type == "EDIT")  return "Edits to one file at its position in the fold: key = value, whole file, append, or byte patches.";
    if (Type == "REG")   return "Registry keys and values, per architecture view; later wins per value, null deletes.";
    if (Type == "DLL")   return "Which DLLs resolve native vs builtin (wine's n,b notation); null removes an override.";
    if (Type == "ENV")   return "Process environment: later wins a name, null removes it.";
    if (Type == "VARS")  return "Variables the player sets before launch; substituted as %KEY% wherever they are used.";
    if (Type == "KEEP")  return "What the user owns: an address kept across launches (true / {NAME, CLOUD}), or taken back (false).";
    if (Type == "EXEC")  return "Entries - what to run. No GUEST => a game entry (a TILE puts it on the shelf); GUEST => a runner.";
    if (Type.empty())    return "A node with no layers yet.";
    return "";
}

const char *TypeGist(const std::string &Type)
{
    if (Type == "ZIP")   return "the game's files, from a zip";
    if (Type == "DIR")   return "a folder of files";
    if (Type == "FILE")  return "one file";
    if (Type == "DELTA") return "a patch onto the files beneath";
    if (Type == "NODE")  return "contain another node";
    if (Type == "ANY")   return "needs one of these nodes";
    if (Type == "NOT")   return "never together with a node";
    if (Type == "EDIT")  return "change a file: a setting, bytes...";
    if (Type == "REG")   return "registry settings";
    if (Type == "DLL")   return "native or built-in DLLs";
    if (Type == "ENV")   return "environment variables";
    if (Type == "VARS")  return "options the player picks";
    if (Type == "KEEP")  return "saves and settings to keep";
    if (Type == "EXEC")  return "what to run, and its tile";
    if (Type.empty())    return "nothing yet";
    return "";
}

void TypeColour(const std::string &Kind, int &R, int &G, int &B)
{
    // Content = blue, transforms = amber, facts = purple, entries = green, composition = grey.
    if (Kind == "ZIP" || Kind == "DIR" || Kind == "FILE" || Kind == "DELTA") { R = 46;  G = 96;  B = 148; return; }
    if (Kind == "EDIT" || Kind == "REG" || Kind == "DLL" || Kind == "ENV")   { R = 136; G = 92;  B = 36;  return; }
    if (Kind == "KEEP" || Kind == "VARS")                                  { R = 92;  G = 68;  B = 128; return; }
    if (Kind == "EXEC")                                                    { R = 46;  G = 118; B = 78;  return; }
    R = 74; G = 80; B = 88;
}

//A layer's starter value: required keys present from the start, so a fresh layer is never malformed — the old
//editor created content layers with no TARGET at all, which is what made Tonic Trouble die as "create process: 2".
//SOURCE on content is stamped by publish, not seeded here.
json NewLayer(const std::string &T)
{
    if (T == "ZIP" || T == "FILE" || T == "DELTA") return json::object({{T, ""}, {"TARGET", "FILES/%GameDir%"}});
    if (T == "DIR")   return json::object({{"DIR", ""}, {"TARGET", "FILES/%GameDir%"}});
    if (T == "NODE")  return json::object({{"NODE", ""}});
    if (T == "ANY")   return json::object({{"ANY", json::array()}});
    if (T == "NOT")   return json::object({{"NOT", ""}});
    if (T == "EDIT")  return json::object({{"EDIT", json::array({ json::object({{"MODE","ConfigWrite"},{"KEY",""},{"VALUE",""}}) })},
                                           {"TARGET", "FILES/%GameDir%/"}});
    if (T == "REG")   return json::object({{"REG", json::object()}, {"ARCH", json::array({"32"})}});
    if (T == "DLL")   return json::object({{"DLL", json::object()}});
    if (T == "ENV")   return json::object({{"ENV", json::object()}});
    if (T == "VARS")  return json::object({{"VARS", json::object()}});
    if (T == "KEEP")  return json::object({{"KEEP", json::object()}});
    if (T == "EXEC")  return json::object({{"EXEC", json::array({ json::object({{"LABEL","Play"},{"HOST","win32"},
                                                                                {"EXE","%GameDir%/"},{"ARGS", json::array()}}) })}});
    return json();
}

json NewPayload(const std::string &Type)
{
    //An unknown type yields a node with no layers rather than a null layer every reader would trip over — the
    //palette only offers real types, so this is a guard for callers, not a feature.
    json N = json::object({{"LAYERS", json::array()}});
    const json L = NewLayer(Type);
    if (!L.is_null()) N["LAYERS"].push_back(L);
    return N;
}

bool AddLayer(json &Node, const std::string &Type)
{
    const json L = NewLayer(Type);
    if (!Node.is_object() || L.is_null()) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;   // malformed: refused, never replaced
    Node["LAYERS"].push_back(L);
    return true;
}

bool MoveLayer(json &Node, int I, int Delta)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    const int J = I + Delta;
    if (I < 0 || J < 0 || I >= (int)Ls.size() || J >= (int)Ls.size() || I == J) return false;
    std::swap(Ls[(size_t)I], Ls[(size_t)J]);
    return true;
}

bool MoveLayerTo(json &Node, int From, int At)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    const int N = (int)Ls.size();
    if (From < 0 || From >= N) return false;
    At = std::clamp(At, 0, N);
    if (At == From || At == From + 1) return false;      // already there
    json L = Ls[(size_t)From];
    Ls.erase((size_t)From);
    if (At > From) --At;                                 // the removal shifted what came after it
    Ls.insert(Ls.begin() + At, std::move(L));
    return true;
}

bool LayerNames(const json &L, const std::string &Ref)
{
    if (!L.is_object()) return false;
    auto Is = [&](const char *K) { return L.contains(K) && L[K].is_string() && L[K].get<std::string>() == Ref; };
    if (Is("NODE") || Is("NOT")) return true;
    if (L.contains("ANY") && L["ANY"].is_array())
        for (const json &M : L["ANY"]) if (M.is_string() && M.get<std::string>() == Ref) return true;
    return false;
}

bool MoveLayerAcross(json &Src, int From, json &Dst, const std::string &DstHandle, int At, bool Copy)
{
    if (!Src.is_object() || !Dst.is_object() || !Src.contains("LAYERS") || !Src["LAYERS"].is_array()) return false;
    json &Ls = Src["LAYERS"];
    if (From < 0 || From >= (int)Ls.size()) return false;
    if (!DstHandle.empty() && LayerNames(Ls[(size_t)From], DstHandle)) return false;
    if (!Dst.contains("LAYERS") || Dst["LAYERS"].is_null()) Dst["LAYERS"] = json::array();
    if (!Dst["LAYERS"].is_array()) return false;
    json &Ld = Dst["LAYERS"];
    At = std::clamp(At, 0, (int)Ld.size());
    Ld.insert(Ld.begin() + At, Ls[(size_t)From]);
    if (!Copy) Ls.erase((size_t)From);
    return true;
}

bool EraseRef(json &Node, int Slot)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array() || Slot < 0) return false;
    json &Ls = Node["LAYERS"];
    int Ord = -1;
    for (size_t I = 0; I < Ls.size(); ++I)
    {
        const std::string T = LayerType(Ls[I]);
        if (T == "NODE" || T == "NOT") { if (++Ord == Slot) { Ls.erase(I); return true; } }
        else if (T == "ANY")
        {
            json &A = Ls[I]["ANY"];
            if (!A.is_array()) { if (++Ord == Slot) { Ls.erase(I); return true; } continue; }
            for (size_t M = 0; M < A.size(); ++M)
            {
                if (++Ord != Slot) continue;
                A.erase(M);
                if (A.empty()) Ls.erase(I);
                return true;
            }
        }
    }
    return false;
}

bool AddNodeRef(json &Node, const std::string &Ref)
{
    if (!Node.is_object() || Ref.empty()) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    size_t At = 0;
    for (const auto &L : Ls)
    {
        if (LayerType(L) == "NODE" && L["NODE"].is_string() && L["NODE"].get<std::string>() == Ref) return false;
    }
    while (At < Ls.size() && LayerType(Ls[At]) == "NODE") ++At;
    Ls.insert(Ls.begin() + (long)At, json::object({{"NODE", Ref}}));
    return true;
}

bool InsertRef(json &Node, const std::string &Type, const std::string &Ref, int At)
{
    if (At < 0 && Type == "NODE") return AddNodeRef(Node, Ref);
    if (!Node.is_object() || Ref.empty() || (Type != "NODE" && Type != "NOT")) return false;
    if (!Node.contains("LAYERS") || Node["LAYERS"].is_null()) Node["LAYERS"] = json::array();
    if (!Node["LAYERS"].is_array()) return false;
    json &Ls = Node["LAYERS"];
    for (const auto &L : Ls)
        if (LayerType(L) == Type && L[Type].is_string() && L[Type].get<std::string>() == Ref) return false;
    At = At < 0 ? (int)Ls.size() : std::min(At, (int)Ls.size());
    Ls.insert(Ls.begin() + At, json::object({{Type, Ref}}));
    return true;
}

bool RemoveRefs(json &Node, const std::string &Ref)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    json Kept = json::array();
    bool Changed = false;
    auto Names = [&](const json &V) { return V.is_string() && V.get<std::string>() == Ref; };
    for (const auto &L : Node["LAYERS"])
    {
        const std::string T = LayerType(L);
        if ((T == "NODE" && Names(L["NODE"])) || (T == "NOT" && Names(L["NOT"]))) { Changed = true; continue; }
        if (T == "ANY" && L["ANY"].is_array())
        {
            json A = json::array();
            for (const auto &M : L["ANY"]) { if (Names(M)) { Changed = true; continue; } A.push_back(M); }
            if (A.empty() && !L["ANY"].empty()) continue;
            json L2 = L; L2["ANY"] = std::move(A);
            Kept.push_back(std::move(L2));
            continue;
        }
        Kept.push_back(L);
    }
    if (Changed) Node["LAYERS"] = std::move(Kept);
    return Changed;
}

std::string TakeToText(const json &Take)
{
    std::string Out;
    if (!Take.is_array()) return Out;
    for (const auto &S : Take)
    {
        if (S.is_string()) Out += S.get<std::string>();
        else if (S.is_array() && S.size() == 2 && S[0].is_string() && S[1].is_string())
            Out += S[0].get<std::string>() + " => " + S[1].get<std::string>();
        else continue;
        Out += "\n";
    }
    if (!Out.empty()) Out.pop_back();
    return Out;
}

json TextToTake(const std::string &Text)
{
    json Out = json::array();
    size_t Pos = 0;
    while (Pos <= Text.size())
    {
        size_t E = Text.find('\n', Pos);
        if (E == std::string::npos) E = Text.size();
        std::string Line = Text.substr(Pos, E - Pos);
        Pos = E + 1;
        auto Trim = [](std::string X) {
            const size_t A = X.find_first_not_of(" \t\r"), B = X.find_last_not_of(" \t\r");
            return A == std::string::npos ? std::string() : X.substr(A, B - A + 1);
        };
        Line = Trim(Line);
        if (Line.empty()) { if (E == Text.size()) break; continue; }
        const size_t Arrow = Line.find("=>");
        if (Arrow == std::string::npos) Out.push_back(Line);
        else Out.push_back(json::array({ Trim(Line.substr(0, Arrow)), Trim(Line.substr(Arrow + 2)) }));
        if (E == Text.size()) break;
    }
    return Out;
}

bool TakeFault(const json *Take)
{
    if (!Take || Take->is_null()) return false;
    if (!Take->is_array()) return true;
    for (const auto &S : *Take)
        if (!S.is_string() && !(S.is_array() && S.size() == 2 && S[0].is_string() && S[1].is_string())) return true;
    return false;
}

// ---- payload schema -------------------------------------------------------

bool FieldApplies(const Field &F, const json &Entry)
{
    if (!F.OnlyModes || !*F.OnlyModes || (Entry.is_object() && Entry.contains(F.Key))) return true;
    const std::string Mode = Entry.is_object() && Entry.contains("MODE") && Entry["MODE"].is_string() ? Entry["MODE"].get<std::string>() : std::string();
    std::istringstream Ss(F.OnlyModes);
    for (std::string M; Ss >> M;) if (M == Mode) return true;
    return false;
}

namespace {

const std::vector<std::pair<const char *, const char *>> DllOpts    = {{"n,b","native, then builtin"},{"b,n","builtin, then native"},
                                                                       {"n","native only"},{"b","builtin only"},{"","disabled"}};
const std::vector<std::pair<const char *, const char *>> ModeOpts   = {{"ConfigWrite","key = value"},{"Overwrite","whole file"},
                                                                       {"AppendLine","append a line"},{"JsonSet","JSON: set a key"},{"XmlSet","XML: set a value"},
                                                                       {"Delete","remove the file or folder"},
                                                                       {"Replace","bytes: replace"},{"ReplaceAll","bytes: replace every match"},
                                                                       {"Poke","bytes: poke"},{"Or","bytes: set bits"},{"Cave","bytes: cave"}};

//Every layer ends with its gate and its note.
std::vector<Field> WithGate(std::vector<Field> F)
{
    F.push_back({"WHEN",    "When",    FieldKind::Text, "condition - the layer is inert when false", {}, {}});
    F.push_back({"COMMENT", "Comment", FieldKind::Text, "why this layer is here", {}, {}});
    F.push_back({"LABEL",   "Label",   FieldKind::Text, "the row's name - blank: from its WHEN variable / payload", {}, {}});
    F.push_back({"SECTION", "Section", FieldKind::Text, "the row's folder - a path, / nests; blank: from WHEN", {}, {}});
    return F;
}

std::vector<Field> Content(const char *Key, const char *Label, const char *Hint, bool Source)
{
    std::vector<Field> F = {
        {Key,         Label,       FieldKind::Text,       Hint, {}, {}},
        {"TARGET",    "Target",    FieldKind::Text,       "FILES/%GameDir%/... - where it lands", {}, {}},
    };
    if (Source) F.push_back({"SOURCE", "Source", FieldKind::Text, "CID - stamped by publish", {}, {}});
    return WithGate(std::move(F));
}

std::vector<Field> MakeFields(const std::string &Type)
{
    if (Type == "ZIP")   return Content("ZIP",   "Zip",   "archive in this bundle", true);
    if (Type == "FILE")  return Content("FILE",  "File",  "file in this bundle", true);
    if (Type == "DELTA") return Content("DELTA", "Delta", ".vgdelta in this bundle", true);
    if (Type == "DIR")   return Content("DIR",   "Dir",   "folder in this bundle, or %RunnerMount%/...", false);
    if (Type == "NODE")
        return WithGate({
            {"NODE",   "Node",   FieldKind::Text,     "wired on the canvas - the contained node's CID", {}, {}},
            {"TAKE",   "Take",   FieldKind::TakeList, "FILES/a/b.dll, FILES/dir/ (contents), X => Y (rename)", {}, {}},
            {"TARGET", "Target", FieldKind::Text,     "FILES/... - where its files land", {}, {}},
        });
    if (Type == "ANY")
        return WithGate({{"ANY", "Any of", FieldKind::StringList, "node CIDs - one must be present", {}, {}}});
    if (Type == "NOT")
        return WithGate({{"NOT", "Not with", FieldKind::Text, "a node CID that must not be present", {}, {}}});
    if (Type == "EDIT")
        //Every field the edit engine reads: text modes (key = value, whole file, append) and byte modes
        //(replace, poke, cave). An earlier table offered Cave and Poke without their PAYLOAD/VALUE, so picking
        //either produced a node validation then rejected with no way to fix it from the editor.
        return WithGate({
            {"TARGET", "File", FieldKind::Text, "FILES/%GameDir%/... - the file edited (Delete: the file or folder removed)", {}, {}},
            {"EDIT",   "Ops",  FieldKind::ObjArray, "", {}, {
                {"MODE",    "Mode",    FieldKind::Enum, "", ModeOpts, {}},
                {"SECTION", "Section", FieldKind::Text, "(not read by any mode)", {}, {}, false, "-"},
                {"KEY",     "Key",     FieldKind::Text, "ConfigWrite: the line's start (Resolution=) / JsonSet: /json/pointer / XmlSet: /root/el[@k='v']/@attr", {}, {}, false, "ConfigWrite JsonSet XmlSet"},
                {"VALUE",   "Value",   FieldKind::Text, "text value / JsonSet: JSON (else a string) / Poke: the scalar / Or: the bits", {}, {}, false,
                 "ConfigWrite Overwrite AppendLine JsonSet XmlSet Poke Or"},
                {"OFFSET",  "Offset",  FieldKind::Text, "0x... (VA or file offset) - or give Anchor", {}, {}, false, "Replace Poke Or Cave"},
                {"ANCHOR",  "Anchor",  FieldKind::Text, "hex signature, ?? = wildcard - instead of Offset", {}, {}, false, "Replace Poke Or Cave"},
                {"EXPECT",  "Expect",  FieldKind::Text, "the original bytes (the guard; for ReplaceAll, the bytes to find)", {}, {}, false, "Replace ReplaceAll Poke Cave"},
                {"REPLACE", "Replace", FieldKind::Text, "the new bytes", {}, {}, false, "Replace ReplaceAll"},
                {"PAYLOAD", "Payload", FieldKind::Text, "the cave's code (hex)", {}, {}, false, "Cave"},
                {"CAVE",    "Cave at", FieldKind::Text, "'auto' or a fixed VA", {}, {}, false, "Cave"},
                {"COMMENT", "Comment", FieldKind::Text, "what this op does", {}, {}},
                {"WHEN",    "When",    FieldKind::Text, "condition - the op is inert when false", {}, {}},
            }},
        });
    if (Type == "REG")
        return WithGate({
            {"ARCH", "Views", FieldKind::Arch,    "", {}, {}},
            {"REG",  "Keys",  FieldKind::RegTree, "", {}, {}},
        });
    if (Type == "DLL")
        return WithGate({{"DLL", "Overrides", FieldKind::KeyValue, "dll -> resolution order", DllOpts, {}}});
    if (Type == "ENV")
        return WithGate({{"ENV", "Env", FieldKind::KeyValue, "name -> value; later wins", {}, {}}});
    if (Type == "VARS")
        return WithGate({
            {"VARS", "Variables", FieldKind::VarMap, "", {}, {
                {"DEFAULT", "Default", FieldKind::Text, "", {}, {}},
                {"COMMENT", "Comment", FieldKind::Text, "", {}, {}},
                {"WHEN",    "When",    FieldKind::Text, "condition - the value is empty when false", {}, {}},
            }, /*VarUI=*/true},
        });
    if (Type == "KEEP")
        return WithGate({{"KEEP", "Keep", FieldKind::KeepMap, "FILES/... or REG/... - owned by the user", {}, {}}});
    if (Type == "EXEC")
        //Each entry folds per LABEL. No GUEST ⇒ a game entry (a TILE puts it on the shelf); GUEST ⇒ a runner.
        return WithGate({
            {"EXEC", "Entries", FieldKind::ObjArray, "", {}, {
                {"LABEL",        "Label",        FieldKind::Text,       "entries fold by label (\"Play\" = the game)", {}, {}},
                {"HOST",         "Host",         FieldKind::Text,       "the platform this needs (win32 / linux64 / ...)", {}, {}},
                {"GUEST",        "Guest",        FieldKind::StringList, "platforms this PROVIDES - set => runner", {}, {}},
                {"EXE",          "Exe",          FieldKind::Text,       "the program, by anchor (%GameDir%/...)", {}, {}},
                {"ARGS",         "Args",         FieldKind::StringList, "one per line", {}, {}},
                {"WORKDIR",      "Work dir",     FieldKind::Text,       "", {}, {}},
                {"CONTENT_ROOT", "Content root", FieldKind::Text,       "runner only", {}, {}},
                {"PREFIX_GENERATE", "Generate prefix", FieldKind::Check, "runner only - needs a wine/proton prefix", {}, {}},
                {"UNIFIED_RUNTIME", "Unified runtime", FieldKind::Check, "runner only - mount the build INTO the game runtime", {}, {}},
                {"GUEST_ROOTS",  "Guest roots",  FieldKind::KeyValue,   "runner only - %GameDir% -> C:\\%PackageUID%", {}, {}},
                {"DRIVES",       "Drives",       FieldKind::KeyValue,   "runner only - C: -> %PrefixRoot%/drive_c", {}, {}},
                {"TILE",         "Tile",         FieldKind::Object,     "", {}, {
                    {"UID",       "UID",        FieldKind::Text,     "stable id - one UID = one card", {}, {}},
                    {"PARENTUID", "Parent UID", FieldKind::Text,     "the family (base game) this tile nests under", {}, {}},
                    {"TITLE",     "Title",      FieldKind::Text,     "the card's name", {}, {}},
                    {"COVER",     "Cover",      FieldKind::Cover,    "", {}, {}},
                    {"META",      "Meta",       FieldKind::KeyValue, "catalog metadata", {}, {}},
                }},
            }},
        });
    return {};
}

} // namespace

void SetVarVisible(nlohmann::ordered_json &node, bool visible)
{
    if (!node.is_object()) return;
    if (visible)
    {
        if (!node.contains("UI") || !node["UI"].is_object())
            node["UI"] = nlohmann::ordered_json{{"CONTROL", "text"}, {"LABEL", node.value("KEY", std::string())}};
    }
    else
    {
        node.erase("UI");
    }
}

const std::vector<Field> &FieldsFor(const std::string &Type)
{
    static std::map<std::string, std::vector<Field>> Cache;
    auto It = Cache.find(Type);
    if (It == Cache.end()) It = Cache.emplace(Type, MakeFields(Type)).first;
    return It->second;
}

// ---- registry rows ---------------------------------------------------------

//Rows one registry TREE contributes, by exactly the rules FlattenTree walks: a row per non-object value, one
//"create this key" row for a key with no children at all, then the subkeys. Pinned against the real flattening
//by the_registry_row_count_matches_the_flattening, because two readings of the same tree that drift apart is
//precisely how the estimate would start under-reserving again.
size_t CountRegTree(const json &Tree)
{
    size_t Rows = 0;
    bool AnyValue = false;
    for (const auto &[K, V] : Tree.items())
        if (!V.is_object()) { ++Rows; AnyValue = true; }
    if (!AnyValue && Tree.empty()) ++Rows;
    for (const auto &[K, V] : Tree.items())
        if (V.is_object()) Rows += CountRegTree(V);
    return Rows;
}

//One RegEdit ENTRY: its hives are the object-valued members, everything else on it is a scalar field.
size_t CountRegRows(const json &Entry)
{
    if (!Entry.is_object()) return 0;
    size_t Rows = 0;
    for (const auto &[K, V] : Entry.items())
        if (V.is_object()) Rows += CountRegTree(V);
    return Rows;
}


// ---- the layer tree -------------------------------------------------------

namespace {
void CollectFacetsOf(const json &N, VarFacets &Out);
}

VarFacets CollectVarFacets(const json &NodesArray)
{
    VarFacets Out;
    if (!NodesArray.is_array()) return Out;
    for (const json &N : NodesArray) CollectFacetsOf(N, Out);
    return Out;
}

VarFacets CollectVarFacets(const std::vector<NodeRef> &Nodes)
{
    VarFacets Out;
    for (const NodeRef &R : Nodes) if (R.Json) CollectFacetsOf(*R.Json, Out);
    return Out;
}

namespace {
void CollectFacetsOf(const json &N, VarFacets &Out)
{
    {
        if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) return;
        for (const json &L : N["LAYERS"])
        {
            if (!L.is_object() || !L.contains("VARS") || !L["VARS"].is_object()) continue;
            for (const auto &[K, D] : L["VARS"].items())
            {
                if (!D.is_object() || !D.contains("UI") || !D["UI"].is_object()) continue;
                const json &UI = D["UI"];
                auto S = [&](const char *F) { return UI.contains(F) && UI[F].is_string() ? UI[F].get<std::string>() : std::string(); };
                VarFacet F{S("LABEL"), S("SECTION"), S("CONTROL"), {}};
                if (UI.contains("CHOICES") && UI["CHOICES"].is_array())
                    for (const json &C : UI["CHOICES"])
                        if (C.is_object() && C.contains("VALUE") && C["VALUE"].is_string())
                            F.Choices[C["VALUE"].get<std::string>()] =
                                C.contains("LABEL") && C["LABEL"].is_string() ? C["LABEL"].get<std::string>() : C["VALUE"].get<std::string>();
                Out[K] = std::move(F);                    // a later declaration of the same key wins, as in the fold
            }
        }
    }
}
}

namespace {

std::string StrField(const json &O, const char *K)
{
    return O.is_object() && O.contains(K) && O[K].is_string() ? O[K].get<std::string>() : std::string();
}

std::string Trim(std::string S)
{
    while (!S.empty() && (S.back() == ' ' || S.back() == '\t')) S.pop_back();
    size_t I = 0;
    while (I < S.size() && (S[I] == ' ' || S[I] == '\t')) ++I;
    return S.substr(I);
}

std::string BaseName(const std::string &P)
{
    std::string S = P;
    while (!S.empty() && (S.back() == '/' || S.back() == '\\')) S.pop_back();
    const size_t Slash = S.find_last_of("/\\");
    return Slash == std::string::npos ? S : S.substr(Slash + 1);
}

std::string Join(const std::vector<std::string> &V, const char *Sep, size_t Max = 4)
{
    std::string Out;
    for (size_t I = 0; I < V.size() && I < Max; ++I) Out += (I ? Sep : "") + V[I];
    if (V.size() > Max) Out += std::string(Sep) + "+" + std::to_string(V.size() - Max);
    return Out;
}

std::string RefName(const json &R, const LabelFn &NodeLabel)
{
    if (!R.is_string()) return "?";
    const std::string Ref = R.get<std::string>();
    const std::string L = NodeLabel ? NodeLabel(Ref) : std::string();
    return !L.empty() ? L : (Ref.size() > 16 ? Ref.substr(0, 10) + ".." + Ref.substr(Ref.size() - 4) : Ref);
}

//Where a placed layer lands, without the FILES/ namespace: "%GameDir%/age2_x1".
std::string Where(const json &L)
{
    std::string T = StrField(L, "TARGET");
    if (T.rfind("FILES/", 0) == 0) T = T.substr(6);
    return T;
}

std::string PayloadSummary(const json &L, const std::string &T, const LabelFn &NodeLabel)
{
    const json &V = L[T];
    auto Keys = [&]() { std::vector<std::string> K; if (V.is_object()) for (const auto &[Key, X] : V.items()) K.push_back(Key); return K; };
    if (T == "ZIP" || T == "FILE" || T == "DELTA" || T == "DIR")
    {
        const std::string W = Where(L);
        return BaseName(V.is_string() ? V.get<std::string>() : std::string("?")) + (W.empty() ? "" : " -> " + W);
    }
    if (T == "NODE")
    {
        std::string S = RefName(V, NodeLabel);
        if (L.contains("TAKE") && L["TAKE"].is_array()) S += " (" + std::to_string(L["TAKE"].size()) + " taken)";
        const std::string W = Where(L);
        return W.empty() ? S : S + " -> " + W;
    }
    if (T == "EDIT")
        return BaseName(Where(L)) + " (" + std::to_string(V.is_array() ? V.size() : 0) + " ops)";
    if (T == "REG")
    {
        //The deepest single path from the hive down: where the layer writes, e.g. HKLM\...\Age of Empires\2.0.
        std::vector<std::string> Path;
        const json *At = &V;
        while (At->is_object() && At->size() == 1 && At->begin().value().is_object())
        { Path.push_back(At->begin().key()); At = &At->begin().value(); }
        if (Path.empty()) return Join(Keys(), ", ");
        if (Path.size() > 3) Path.erase(Path.begin() + 1, Path.end() - 2), Path.insert(Path.begin() + 1, "..");
        return Join(Path, "\\", 8);
    }
    if (T == "KEEP")
    {
        std::vector<std::string> K;
        for (const std::string &A : Keys()) K.push_back(BaseName(A) + (A.size() && A.back() == '/' ? "/" : ""));
        return Join(K, ", ");
    }
    if (T == "EXEC")
    {
        std::vector<std::string> K;
        if (V.is_array()) for (const json &E : V) K.push_back(StrField(E, "LABEL").empty() ? "?" : StrField(E, "LABEL"));
        return "Run: " + Join(K, ", ");
    }
    if (T == "ANY")
    {
        std::vector<std::string> K;
        if (V.is_array()) for (const json &R : V) K.push_back(RefName(R, NodeLabel));
        return "Needs " + Join(K, " or ");
    }
    if (T == "NOT") return "Not with " + RefName(V, NodeLabel);
    return Join(Keys(), ", ");                    // VARS, ENV, DLL: their names
}

//"%up_style%==widescreen" → ("up_style", "widescreen"); anything more than one comparison of one variable → the
//first variable named, no value.
std::pair<std::string, std::string> WhenVar(const std::string &When)
{
    const size_t A = When.find('%');
    if (A == std::string::npos) return {};
    const size_t B = When.find('%', A + 1);
    if (B == std::string::npos) return {};
    std::string Key = When.substr(A + 1, B - A - 1);
    if (const size_t C = Key.find(':'); C != std::string::npos) Key = Key.substr(0, C);
    const std::string Rest = Trim(When.substr(B + 1));
    if (Trim(When.substr(0, A)).empty() && Rest.rfind("==", 0) == 0)
    {
        const std::string Val = Trim(Rest.substr(2));
        if (!Val.empty() && Val.find_first_of(" %&|()!=<>") == std::string::npos) return {Key, Val};
    }
    return {Key, std::string()};
}

} // namespace

std::vector<LayerItem> LayerItems(const json &Node, const VarFacets &Facets, const LabelFn &NodeLabel)
{
    std::vector<LayerItem> Out;
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return Out;
    const json &Ls = Node["LAYERS"];
    for (size_t I = 0; I < Ls.size(); ++I)
    {
        const json &L = Ls[I];
        LayerItem It;
        It.Layer = (int)I;
        It.Type  = LayerType(L);
        if (It.Type.empty()) { It.Title = "(malformed layer)"; Out.push_back(std::move(It)); continue; }
        It.Summary = PayloadSummary(L, It.Type, NodeLabel);
        std::string Derived, DerivedSection;
        if (const std::string When = StrField(L, "WHEN"); !When.empty())
        {
            const auto [Key, Val] = WhenVar(When);
            const auto F = Facets.find(Key);
            if (F != Facets.end() && !F->second.Label.empty())
            {
                const VarFacet &Fa = F->second;
                Derived = Fa.Label;
                if (!Val.empty())
                {
                    if (Fa.Control == "bool") { if (Val != "1" && Val != "true") Derived += ": off"; }
                    else if (const auto C = Fa.Choices.find(Val); C != Fa.Choices.end()) Derived += ": " + C->second;
                    else Derived += " = " + Val;
                }
                DerivedSection = Fa.Section;
            }
        }
        const std::string Label = StrField(L, "LABEL");
        It.Title   = !Label.empty() ? Label : !Derived.empty() ? Derived : It.Summary;
        //The path without empty segments ("/a//b" is "a/b"), so the tree and the row count agree on its folders.
        const std::string Section = StrField(L, "SECTION");
        std::string Seg;
        std::stringstream SS(!Section.empty() ? Section : DerivedSection);
        while (std::getline(SS, Seg, '/')) if (!Seg.empty()) It.Section += (It.Section.empty() ? "" : "/") + Seg;
        Out.push_back(std::move(It));
    }
    return Out;
}

int TopLevelRows(const std::vector<LayerItem> &Items)
{
    std::set<std::string> Heads;
    int Rows = 0;
    for (const LayerItem &It : Items)
    {
        if (It.Section.empty()) { ++Rows; continue; }
        if (Heads.insert(It.Section.substr(0, It.Section.find('/'))).second) ++Rows;
    }
    return Rows;
}

std::string EntrySummary(const json &E)
{
    if (!E.is_object()) return "(malformed entry)";
    const std::string Mode = StrField(E, "MODE");
    if (Mode.empty()) return StrField(E, "LABEL");
    std::string Site = StrField(E, "OFFSET");
    if (Site.empty() && !StrField(E, "ANCHOR").empty()) Site = "@" + StrField(E, "ANCHOR").substr(0, 12);
    if (Mode == "ConfigWrite")
    {
        const std::string Sec = StrField(E, "SECTION");
        return (Sec.empty() ? "" : "[" + Sec + "] ") + StrField(E, "KEY") + " = " + StrField(E, "VALUE");
    }
    if (Mode == "JsonSet" || Mode == "XmlSet") return StrField(E, "KEY") + " = " + StrField(E, "VALUE");
    auto Short = [](const std::string &H) { return H.size() > 12 ? H.substr(0, 10) + ".." : H; };
    if (Mode == "Replace") return Site + " " + Short(StrField(E, "EXPECT")) + " -> " + Short(StrField(E, "REPLACE"));
    if (Mode == "ReplaceAll") return "every " + Short(StrField(E, "EXPECT")) + " -> " + Short(StrField(E, "REPLACE"));
    if (Mode == "Or" || Mode == "Poke") return Site + " " + Mode + " " + Short(StrField(E, "VALUE"));
    if (Mode == "Cave") return Site + " Cave " + std::to_string(StrField(E, "PAYLOAD").size() / 2) + " bytes";
    return Mode;
}

std::string EntryTitle(const json &E)
{
    const std::string L = StrField(E, "LABEL");
    if (!L.empty()) return L;
    const std::string C = StrField(E, "COMMENT");
    return !C.empty() ? C : EntrySummary(E);
}

std::string VarTitle(const std::string &Key, const json &Decl)
{
    const std::string L = Decl.is_object() && Decl.contains("UI") ? StrField(Decl["UI"], "LABEL") : std::string();
    return L.empty() ? Key : L + " (" + Key + ")";
}


// ---- node actions ---------------------------------------------------------

std::string PlatformWords(const std::string &T)
{
    static const std::map<std::string, std::string> W = {
        {"win32", "Windows 32-bit"}, {"win64", "Windows 64-bit"}, {"linux64", "Linux"}, {"linux32", "Linux 32-bit"}, {"dos", "DOS"},
        {"snes", "Super Nintendo"}, {"nes", "NES"}, {"n64", "Nintendo 64"}, {"gc", "GameCube"}, {"wii", "Wii"}, {"ps1", "PlayStation"},
        {"ps2", "PlayStation 2"}, {"psp", "PSP"}, {"gba", "Game Boy Advance"}, {"gb", "Game Boy"}, {"genesis", "Mega Drive"},
        {"custom", "its own runner"}};
    const auto It = W.find(T);
    return It == W.end() ? T : It->second;
}

std::string Describe(const json &Node, const LabelFn &Label)
{
    if (!Node.is_object()) return {};
    auto Name = [&](const std::string &R) { const std::string L = Label ? Label(R) : std::string(); return "\"" + (L.empty() ? R.substr(0, 12) : L) + "\""; };
    auto Base = [](const std::string &P) { return P.substr(P.find_last_of('/') + 1); };
    auto List = [](const std::vector<std::string> &V) {
        std::string Out;
        for (size_t I = 0; I < V.size(); ++I) Out += (I ? (I + 1 == V.size() ? " and " : ", ") : "") + V[I];
        return Out;
    };
    std::vector<std::string> Parts, Files, Contains, Rides, Never, Keeps;
    int Edits = 0, Regs = 0, Dlls = 0, Envs = 0, Vars = 0;
    if (Node.contains("LAYERS") && Node["LAYERS"].is_array())
        for (const json &L : Node["LAYERS"])
        {
            const std::string T = LayerType(L);
            if (T == "ZIP" || T == "DIR" || T == "FILE" || T == "DELTA") { const std::string F = StrField(L, T.c_str()); if (!F.empty()) Files.push_back(F); }
            else if (T == "NODE") Contains.push_back(Name(StrField(L, "NODE")));
            else if (T == "NOT") Never.push_back(Name(StrField(L, "NOT")));
            else if (T == "ANY" && L["ANY"].is_array()) for (const json &M : L["ANY"]) { if (M.is_string()) Rides.push_back(Name(M.get<std::string>())); }
            else if (T == "EDIT") Edits += L["EDIT"].is_array() ? (int)L["EDIT"].size() : 1;
            else if (T == "REG") ++Regs;
            else if (T == "DLL") ++Dlls;
            else if (T == "ENV") ++Envs;
            else if (T == "VARS") Vars += L["VARS"].is_object() ? (int)L["VARS"].size() : 1;
            else if (T == "KEEP")
            {
                std::string K = StrField(L, "KEEP");
                for (const char *A : {"%Documents%", "%AppData%", "%LocalAppData%", "%UserProfile%", "%GameDir%"})
                    if (K.rfind(A, 0) == 0) { K = PlatformWords(std::string(A).substr(1, std::strlen(A) - 2)) + K.substr(std::strlen(A)); break; }
                if (!K.empty()) Keeps.push_back(K.rfind("GameDir", 0) == 0 ? "the game folder" + K.substr(7) : K);
            }
            else if (T == "EXEC" && L["EXEC"].is_array())
                for (const json &E : L["EXEC"])
                {
                    if (!E.is_object()) continue;
                    const std::string Exe = StrField(E, "EXE"), Host = StrField(E, "HOST");
                    if (E.contains("GUEST") && E["GUEST"].is_array())
                    {
                        std::vector<std::string> G;
                        for (const json &X : E["GUEST"]) if (X.is_string()) G.push_back(PlatformWords(X.get<std::string>()));
                        Parts.push_back("plays " + List(G) + " games" + (Host.empty() ? std::string() : " on " + PlatformWords(Host)));
                    }
                    else if (!Exe.empty() && Exe.back() != '/')
                        Parts.push_back("runs " + Base(Exe) + (Host.empty() ? std::string() : " on " + PlatformWords(Host)));
                    else if (!Host.empty()) Parts.push_back("runs on " + PlatformWords(Host) + " (no program yet)");
                    if (E.contains("TILE") && E["TILE"].is_object() && !StrField(E["TILE"], "TITLE").empty())
                        Parts.push_back("shown as \"" + StrField(E["TILE"], "TITLE") + "\"");
                }
        }
    if (Files.size() == 1) Parts.push_back("files from " + Files[0]);
    else if (Files.size() > 1) Parts.push_back("files from " + std::to_string(Files.size()) + " sources");
    if (!Contains.empty()) Parts.push_back("contains " + List(Contains));
    if (!Rides.empty()) Parts.push_back("rides on " + List(Rides));
    if (!Never.empty()) Parts.push_back("never with " + List(Never));
    if (Edits) Parts.push_back("changes " + std::to_string(Edits) + (Edits == 1 ? " setting" : " settings"));
    if (Regs) Parts.push_back("sets registry values");
    if (Dlls) Parts.push_back("picks DLLs");
    if (Envs) Parts.push_back("sets environment variables");
    if (Vars) Parts.push_back("lets the player choose " + std::to_string(Vars) + (Vars == 1 ? " option" : " options"));
    if (!Keeps.empty()) Parts.push_back("keeps " + List(Keeps) + " across plays");
    std::string Out;
    if (const std::string V = StrField(Node, "VARIANT"); !V.empty()) Out = "Version \"" + V + "\": ";
    for (size_t I = 0; I < Parts.size(); ++I) Out += (I ? "; " : "") + Parts[I];
    if (Parts.empty()) Out += "nothing yet";
    if (!Out.empty() && Out[0] >= 'a' && Out[0] <= 'z') Out[0] = (char)(Out[0] - 32);
    return Out;
}

std::vector<FoldStep> FoldOrder(const std::string &Root, const std::function<const json *(const std::string &)> &Find, const LabelFn &Label)
{
    std::vector<FoldStep> Out;
    std::set<std::string> OnPath;
    std::function<void(const std::string &, int)> Walk = [&](const std::string &H, int Depth) {
        const json *N = Find ? Find(H) : nullptr;
        const std::string NL = Label ? Label(H) : std::string();
        if (!N || !N->is_object() || !N->contains("LAYERS") || !(*N)["LAYERS"].is_array()) return;
        if (!OnPath.insert(H).second) return;           // a cycle: walked once
        for (int I = 0; I < (int)(*N)["LAYERS"].size(); ++I)
        {
            const json &L = (*N)["LAYERS"][(size_t)I];
            const std::string T = LayerType(L);
            if (T == "NODE")
            {
                const std::string R = StrField(L, "NODE");
                if (Find && Find(R)) { Walk(R, Depth + 1); continue; }
                Out.push_back({Depth, H, NL, I, T, Label && !Label(R).empty() ? Label(R) : R, true});
                continue;
            }
            std::string Title = StrField(L, "LABEL");
            if (Title.empty() && (T == "ZIP" || T == "DIR" || T == "FILE" || T == "DELTA")) Title = StrField(L, T.c_str());
            if (Title.empty() && T == "EXEC" && L["EXEC"].is_array())
                for (const json &E : L["EXEC"]) if (const std::string El = StrField(E, "LABEL"); !El.empty()) Title += (Title.empty() ? "" : ", ") + El;
            if (Title.empty()) Title = TypeGist(T);
            Out.push_back({Depth, H, NL, I, T, Title, false});
        }
        OnPath.erase(H);
    };
    Walk(Root, 0);
    return Out;
}

ShelfCard CardFor(const std::string &Root, const std::vector<std::string> &PackageNodes, const std::function<const json *(const std::string &)> &Find)
{
    ShelfCard C;
    //The tile the root's fold presents: the first entry carrying one, its own layers before what it contains.
    std::set<std::string> Seen;
    std::function<bool(const std::string &)> FindTile = [&](const std::string &H) {
        const json *N = Find ? Find(H) : nullptr;
        if (!N || !Seen.insert(H).second || !N->is_object() || !N->contains("LAYERS") || !(*N)["LAYERS"].is_array()) return false;
        for (const json &L : (*N)["LAYERS"])
            if (L.is_object() && L.contains("EXEC") && L["EXEC"].is_array())
                for (const json &E : L["EXEC"])
                    if (E.is_object() && E.contains("TILE") && E["TILE"].is_object())
                    {
                        const json &T = E["TILE"];
                        C.Title = StrField(T, "TITLE"); C.Uid = StrField(T, "UID");
                        if (T.contains("COVER")) C.Cover = T["COVER"].is_string() ? T["COVER"].get<std::string>() : StrField(T["COVER"], "FILE");
                        return true;
                    }
        for (const json &L : (*N)["LAYERS"])
            if (LayerType(L) == "NODE" && FindTile(StrField(L, "NODE"))) return true;
        return false;
    };
    FindTile(Root);
    std::set<std::string> Recommended;
    for (const std::string &H : PackageNodes)
        if (const json *N = Find ? Find(H) : nullptr; N && N->is_object() && N->contains("RECOMMENDED") && (*N)["RECOMMENDED"].is_array())
            for (const json &U : (*N)["RECOMMENDED"]) if (U == json(C.Uid)) Recommended.insert(H);
    for (const std::string &H : PackageNodes)
        if (const json *N = Find ? Find(H) : nullptr; N && !StrField(*N, "VARIANT").empty())
            C.Versions.push_back({StrField(*N, "VARIANT"), Recommended.count(H) > 0});
    return C;
}

json RecipeNode(Recipe R, const std::string &Title, const std::string &Uid, const std::string &Platform)
{
    //A library is files games graft (a runtime's DLLs, fonts): waiting for them, with no tile and no place of its
    //own - the game that grafts it says where they land (TARGET on its NODE layer).
    if (R == Recipe::Library)
        return json{{"LABEL", Title}, {"LAYERS", json::array({ json{{"ZIP", ""}} })}};
    json Play = {{"LABEL", "Play"}};
    if (R == Recipe::Dos) Play["HOST"] = "dos";
    else if (R == Recipe::Console && !Platform.empty()) Play["HOST"] = Platform;
    Play["TILE"] = {{"UID", Uid}, {"TITLE", Title}};
    return json{{"LABEL", Title}, {"VARIANT", "Original"},
                {"LAYERS", json::array({ NewLayer("ZIP"),
                                         json{{"COMMENT", "The game's tile (title, cover, details) on its Play entry: every version of it presents this."},
                                              {"EXEC", json::array({Play})}} })}};
}

json VersionNode(const std::string &Base, const std::string &BaseLabel)
{
    return json{{"LABEL", BaseLabel.empty() ? std::string("Another version") : BaseLabel + " - another version"}, {"VARIANT", "New version"},
                {"LAYERS", json::array({ json{{"NODE", Base}}, NewLayer("ZIP"),
                                         json{{"COMMENT", "Its own program: the Play entry it contains, changed where this version differs."},
                                              {"EXEC", json::array({ json{{"LABEL", "Play"}} })}} })}};
}

bool ChangeUid(json &N, const std::string &Old, const std::string &New)
{
    if (!N.is_object() || Old.empty() || Old == New) return false;
    bool Changed = false;
    if (N.contains("RECOMMENDED") && N["RECOMMENDED"].is_array())
        for (json &R : N["RECOMMENDED"]) if (R == Old) { R = New; Changed = true; }
    if (!N.contains("LAYERS") || !N["LAYERS"].is_array()) return Changed;
    for (json &L : N["LAYERS"])
    {
        if (!L.is_object() || !L.contains("EXEC") || !L["EXEC"].is_array()) continue;
        for (json &E : L["EXEC"])
        {
            if (!E.is_object() || !E.contains("TILE") || !E["TILE"].is_object()) continue;
            for (const char *K : {"UID", "PARENTUID"})
                if (E["TILE"].contains(K) && E["TILE"][K] == Old) { E["TILE"][K] = New; Changed = true; }
        }
    }
    return Changed;
}

json ModNode(const std::string &Base, const std::string &BaseLabel)
{
    return json{{"LABEL", BaseLabel.empty() ? std::string("A mod") : "A mod for " + BaseLabel},
                {"LAYERS", json::array({ json{{"ANY", json::array({Base})}}, NewLayer("ZIP") })}};
}

//The row key of N's game entry still to finish: the first with no HOST or, when Folder, the first whose Exe is a
//folder. "" when it has none.
static std::string UnfinishedEntry(const json &N, bool Folder)
{
    if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) return {};
    for (size_t L = 0; L < N["LAYERS"].size(); ++L)
    {
        const json &Ly = N["LAYERS"][L];
        if (!Ly.is_object() || !Ly.contains("EXEC") || !Ly["EXEC"].is_array()) continue;
        for (size_t I = 0; I < Ly["EXEC"].size(); ++I)
        {
            const json &E = Ly["EXEC"][I];
            if (!E.is_object() || E.contains("GUEST")) continue;
            const std::string X = StrField(E, "EXE");
            if (Folder ? (!X.empty() && (X.back() == '/' || X.back() == '\\')) : StrField(E, "HOST").empty())
                return "L" + std::to_string(L) + "/EXEC/" + std::to_string(I);
        }
    }
    return {};
}

Fix FixFor(const std::string &M, const json *Node)
{
    Fix F;
    if (M.find("needs a file name") != std::string::npos)
    { F.Waiting = true; F.Note = "waiting for its files"; F.Label = "bring in files..."; F.Action = "bring_in"; }
    else if (M.find("has no effective entry to run") != std::string::npos)
    {
        //An entry with no Host is one the author started: finish it, rather than a new layer folding over it.
        const std::string K = Node ? UnfinishedEntry(*Node, false) : std::string();
        if (K.empty()) { F.Label = "+ EXEC layer"; F.Action = "add_layer:EXEC"; }
        else { F.Label = "choose its Host"; F.Action = "open_entry:" + K; }
    }
    else if (M.find("must be a non-empty list of entries") != std::string::npos && M.rfind("layer ", 0) == 0)
    {
        //Its last entry removed, a layer that ran something runs nothing: offered gone (its number from the message).
        F.Label = "remove the empty layer"; F.Action = "remove_layer:" + M.substr(6, M.find_first_not_of("0123456789", 6) - 6);
    }
    else if (M.find("is used by no node") != std::string::npos)
    { F.Label = "delete unused files"; F.Action = "delete_unused"; }
    else if (M.find("case conflict across layers") != std::string::npos)
    { F.Label = "fix case"; F.Action = "fix_case"; }
    else if (const size_t D = M.find("DIR '"); D != std::string::npos && M.find("is an unzipped authoring layer") != std::string::npos)
    {
        //Any of the node's folders, not only its first content layer (a patch captured over the game's zip is a second).
        //The panel says it after the node's name, the node itself without.
        const size_t B = D + 5, E = M.find("' is an unzipped", B);
        if (E != std::string::npos) { F.Label = "-> zip"; F.Action = "to_zip:" + M.substr(B, E - B); }
    }
    else if (const size_t K = M.find("KEEP '"); K != std::string::npos && M.find("which takes the name first - it would not be kept") != std::string::npos)
    {
        //The address the message names first is the one the launch would drop.
        const size_t B = K + 6, E = M.find('\'', B);
        if (E != std::string::npos) { F.Label = "name it apart"; F.Action = "keep_name:FILES/" + M.substr(B, E - B); }
    }
    else if (M.find("runs a folder") != std::string::npos)
    {
        const std::string Entry = Node ? UnfinishedEntry(*Node, true) : std::string();
        if (!Entry.empty()) { F.Label = "pick its program"; F.Action = "open_entry:" + Entry; }
    }
    return F;
}

bool NameKeepApart(json &Node, const std::string &Address)
{
    if (!Node.is_object() || !Node.contains("LAYERS") || !Node["LAYERS"].is_array()) return false;
    //Its segments under the place (%GameDir%/...): the first and the last name it ("Expansion Saved Games"); one
    //directly under the place takes the place's name instead.
    std::vector<std::string> Seg;
    std::istringstream Ss(Address);
    for (std::string P; std::getline(Ss, P, '/');) if (!P.empty()) Seg.push_back(P);
    if (Seg.size() < 3) return false;                     // FILES/<place>/<leaf> at least
    std::string Top = Seg.size() > 3 ? Seg[2] : Seg[1];
    Top.erase(std::remove(Top.begin(), Top.end(), '%'), Top.end());
    const std::string Name = Top + " " + Seg.back();
    for (json &L : Node["LAYERS"])
        if (L.is_object() && L.contains("KEEP") && L["KEEP"].is_object() && L["KEEP"].contains(Address))
        {
            json &V = L["KEEP"][Address];
            if (V.is_object()) V["NAME"] = Name; else V = json{{"NAME", Name}};
            return true;
        }
    //Kept by a node it contains (a base game's Save/): a KEEP of its own over it, which folds by address.
    Node["LAYERS"].push_back(json{{"COMMENT", "Stored apart from another kept folder of the same name."},
                                  {"KEEP", {{Address, {{"NAME", Name}}}}}});
    return true;
}

std::string NextStep(const std::vector<const json *> &Nodes)
{
    bool Files = false, Game = false, Runs = false, Tile = false, Variant = false, Waiting = false;
    //Every tile needs a cover somewhere among the nodes (a variant often carries only the UID of a tile at the base).
    std::map<std::string, std::string> Uncovered;          // UID -> its title
    std::set<std::string> Covered;
    for (const json *N : Nodes)
    {
        if (!N || !N->is_object() || !N->contains("LAYERS") || !(*N)["LAYERS"].is_array()) continue;
        if (!StrField(*N, "VARIANT").empty()) Variant = true;
        for (const json &L : (*N)["LAYERS"])
        {
            if (!L.is_object()) continue;
            for (const char *T : {"ZIP", "DIR", "FILE", "DELTA", "NODE"})   // its own files, or another node's
                if (L.contains(T) && L[T].is_string()) (L[T].get<std::string>().empty() ? Waiting : Files) = true;
            if (!L.contains("EXEC") || !L["EXEC"].is_array()) continue;
            for (const json &E : L["EXEC"])
            {
                if (!E.is_object()) continue;
                if (E.contains("GUEST")) return {};       // a runner's package: none of this applies
                Game = true;
                const std::string Exe = StrField(E, "EXE");
                if (!StrField(E, "HOST").empty() && !Exe.empty() && Exe.back() != '/') Runs = true;
                if (!E.contains("TILE") || !E["TILE"].is_object()) continue;
                Tile = true;
                const json &T = E["TILE"];
                const std::string Uid = StrField(T, "UID");
                if (T.contains("COVER") && ((T["COVER"].is_string() && !T["COVER"].get<std::string>().empty())
                                            || (T["COVER"].is_object() && !T["COVER"].empty()))) Covered.insert(Uid);
                else if (std::string &Title = Uncovered[Uid]; Title.empty()) Title = StrField(T, "TITLE");
            }
        }
    }
    if (!Files) return Waiting ? "add the game's files: \"bring in files...\" on the node - or drop the game's folder onto it"
                               : "add the game's files: + layer > ZIP, then actions... > bring in files...";
    if (!Game) return {};
    if (!Runs) return "say what runs: in the Play entry, choose its Host and pick its Exe";
    if (!Tile) return "put it on the shelf: + Tile on the Play entry";
    if (!Variant) return "name this version for the shelf: Properties > Variant (e.g. Original)";
    for (const auto &[Uid, Title] : Uncovered)
        if (!Covered.count(Uid))
            return Covered.empty() ? "give its card a cover: actions... > cover... on the tile's node"
                                   : "give " + (Title.empty() ? "tile " + Uid : "\"" + Title + "\"") + " a cover too: actions... > cover... on its node";
    return {};
}

std::vector<Action> ActionsFor(const json &Node, bool HasDeltaBase, const std::vector<std::string> &Hints)
{
    std::vector<Action> A;
    auto HasHint = [&](const char *H) {
        return std::find(Hints.begin(), Hints.end(), H) != Hints.end();
    };
    const std::vector<std::string> Types = LayerTypes(Node);
    auto Has = [&](const char *T) { return std::find(Types.begin(), Types.end(), T) != Types.end(); };
    //Content actions act on the node's first content layer — its Form.
    std::string Form;
    for (const std::string &T : Types)
        if (T == "ZIP" || T == "DIR" || T == "FILE" || T == "DELTA")
        { Form = T; for (char &C : Form) C = (char)std::tolower((unsigned char)C); break; }

    //Files come in on any node: its first ones, a fix or add-on over the ones it has (it asks), a mod's own.
    A.push_back({"bring_in", "bring in files...", "Pick files anywhere on this computer: a folder is packed into an "
                 "uncompressed zip in this package (the folder is left as it is), a zip is copied in. On a node that "
                 "already has files, they go on top as a new layer or replace them - it asks.", true});
    if (!Form.empty())
    {
        A.push_back({"browse", "browse...", "Pick the file or folder this layer supplies, from this package's folder.", false});
        if (Form == "dir")
            A.push_back({"to_zip", "-> zip", "Pack this directory as an uncompressed (STORE) zip and delete the folder.", true});
        if (Form == "zip")
        {
            A.push_back({"to_dir", "-> dir", "Unpack this zip to a folder and delete the zip.", true});
            if (HasHint("deflate"))
                A.push_back({"restore", "! re-store", "This zip is DEFLATE-compressed and cannot mount - re-pack it uncompressed.", true});
            //A delta needs a base: only offered when a node it CONTAINS is itself a zip to diff against. Must
            //match what the action actually requires, or it is a button that lies.
            if (HasDeltaBase)
                A.push_back({"to_delta", "-> delta", "Store this as a binary delta against the zip beneath it at the same target.", true});
        }
        //The exact inverse of "-> delta": reconstruct the full archive and go back to being a plain zip.
        if (Form == "delta")
            A.push_back({"undelta", "undelta", "Reconstruct the full archive from this delta and store it as a plain zip again.", true});
    }
    if (Has("REG"))
    {
        A.push_back({"capture_reg", "capture registry", "Open regedit on this point of the chain and capture what changes.", true});
        A.push_back({"import_reg", "import .reg", "Read a .reg file into this node's keys.", false});
    }
    if (Has("VARS"))
        A.push_back({"find_usages", "find usages", "Which nodes in the closure reference this node's %KEY%s.", false});
    //An entry without GUEST is a game entry: the thing you can actually run. One with a TILE has a cover.
    bool Launchable = false, Tile = false;
    if (Node.is_object() && Node.contains("LAYERS") && Node["LAYERS"].is_array())
        for (const auto &L : Node["LAYERS"])
            if (LayerType(L) == "EXEC" && L["EXEC"].is_array())
                for (const auto &E : L["EXEC"])
                {
                    if (!E.is_object()) continue;
                    if (!(E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty())) Launchable = true;
                    if (E.contains("TILE") && E["TILE"].is_object()) Tile = true;
                }
    if (Launchable) A.push_back({"test_launch", "test launch", "Open the pre-launch window for this launchable and run it.", false});
    if (Tile) A.push_back({"browse_cover", "cover...", "Pick the cover image for this tile.", false});

    //Available ANYWHERE along the chain: open a live runtime built from this node's resolution, run an installer,
    //and capture what it wrote — the captures become NEW nodes containing this one.
    A.push_back({"capture_setup", "capture setup", "Run an installer on a live runtime at this point and capture the files and registry it writes.", true});
    if (Form.empty())
        A.push_back({"browse_files", "browse files", "Open a file manager on a live runtime at this point and capture what you add.", true});
    return A;
}

// ---- flattened registry editing -------------------------------------------

namespace {

void FlattenTree(const std::string &Path, const json &Tree, std::vector<RegRow> &Out)
{
    bool AnyValue = false;
    for (const auto &[K, V] : Tree.items())
        if (!V.is_object())
        {
            //Remember whether it WAS a string, so a round trip through the editor does not retype a number or
            //a bool into a string just because the row shows text.
            RegRow R{Path, K, V.is_string() ? V.get<std::string>() : V.dump()};
            R.WasString = V.is_string();
            Out.push_back(std::move(R));
            AnyValue = true;
        }
    //create-key-only: flagged, not inferred from an empty NAME — the empty name is the key's DEFAULT value.
    if (!AnyValue && Tree.empty()) { RegRow R{Path, "", ""}; R.KeyOnly = true; Out.push_back(std::move(R)); }
    for (const auto &[K, V] : Tree.items())
        if (V.is_object()) FlattenTree(Path.empty() ? K : Path + "\\" + K, V, Out);
}

} // namespace

std::vector<RegRow> RegRowsOf(const json &Entry)
{
    std::vector<RegRow> Rows;
    if (!Entry.is_object()) return Rows;
    for (const auto &[Hive, Tree] : Entry.items())
    {
        if (!Tree.is_object()) continue;      // ARCHITECTURE/OVERRIDE/WHEN/COMMENT and anything else scalar
        FlattenTree(Hive, Tree, Rows);
    }
    return Rows;
}

// Rebuilt to Orig's key order, at every level: Orig's keys first, where they were, then the new ones.
static json OrderedLike(const json &Rebuilt, const json &Orig)
{
    if (!Rebuilt.is_object() || !Orig.is_object()) return Rebuilt;
    json Out = json::object();
    for (const auto &[K, V] : Orig.items()) if (Rebuilt.contains(K)) Out[K] = OrderedLike(Rebuilt[K], V);
    for (const auto &[K, V] : Rebuilt.items()) if (!Out.contains(K)) Out[K] = V;
    return Out;
}

void RegRowsInto(json &Entry, const std::vector<RegRow> &Rows)
{
    // Keep the non-hive fields, rebuild the trees from scratch.
    //Keep every NON-HIVE field, not an enumerated few: a hive is an OBJECT, everything else on the entry is
    //the author's (ARCHITECTURE, OVERRIDE, WHEN, COMMENT — which NodeLower goes out of its way to allow —
    //and whatever the format learns next). Enumerating them meant any row edit silently deleted the rest.
    json Kept = json::object();
    for (const auto &[K, V] : Entry.items())
        if (!V.is_object()) Kept[K] = V;

    for (const RegRow &R : Rows)
    {
        if (R.Path.empty()) continue;
        std::vector<std::string> Parts;
        for (size_t I = 0, S = 0; I <= R.Path.size(); ++I)
            if (I == R.Path.size() || R.Path[I] == '\\')
            { if (I > S) Parts.push_back(R.Path.substr(S, I - S)); S = I + 1; }
        if (Parts.empty()) continue;
        //A key path segment that collides with an existing VALUE of the same name. This is the SAME clash as the
        //one guarded below, arriving from the other side — and it is the likelier side, because rows are
        //flattened values-first: "HKLM\\A" name "B" is written before the row for key "HKLM\\A\\B" is walked.
        //Overwriting the string with an object here dropped the value with no diagnostic. Neither is a safe
        //guess, so keep what is there and let validation report the clash.
        json *Cur = &Kept;
        bool Clash = false;
        for (const std::string &P : Parts)
        {
            if (Cur->contains(P) && !(*Cur)[P].is_object()) { Clash = true; break; }
            if (!Cur->contains(P)) (*Cur)[P] = json::object();
            Cur = &(*Cur)[P];
        }
        if (Clash) continue;
        if (R.KeyOnly) continue;                  // the key itself was created by the walk above
        //And the mirror case: a value name that collides with an already-written subkey.
        if (Cur->contains(R.Name) && (*Cur)[R.Name].is_object()) continue;
        if (R.WasString) { (*Cur)[R.Name] = R.Value; continue; }
        //Restore the original JSON type where the text still parses as one; otherwise it is a string now
        //because the author made it one.
        json Parsed = json::parse(R.Value, nullptr, /*allow_exceptions=*/false);
        (*Cur)[R.Name] = (Parsed.is_discarded() || Parsed.is_object() || Parsed.is_array())
                             ? json(R.Value) : Parsed;
    }
    //Preserve the ORIGINAL key order, at every level. The rebuild emits the non-hive fields first and the hives after,
    //and within a key its values before its subkeys (rows are flattened values-first), so an entry written the other
    //way round — every received node: canonical JSON, keys sorted, "1.0" before "3D Card" — came back with the same
    //content in a different order: same size, different bytes, a file that no longer is the node its name says.
    Entry = OrderedLike(Kept, Entry);
}

} // namespace PkgGraph
