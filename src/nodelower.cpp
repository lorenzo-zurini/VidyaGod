#include "nodelower.h"
#include "commonutils.h"
#include "fold.h"
#include "varsubst.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <vector>

using nlohmann::ordered_json;

namespace NodeLower
{

namespace {

std::string Str(const ordered_json &V) { return V.is_string() ? V.get<std::string>() : std::string(); }

std::string LowerAscii(std::string S)
{
    std::transform(S.begin(), S.end(), S.begin(), [](unsigned char C) { return static_cast<char>(std::tolower(C)); });
    return S;
}

//The FILES address of a layer's TARGET, namespace stripped ("FILES/C:/x" -> "C:/x", "FILES" -> "").
std::string FilesPath(const ordered_json &L)
{
    const std::string T = L.contains("TARGET") ? Str(L["TARGET"]) : std::string("FILES");
    if (T == "FILES") return std::string();
    return T.rfind("FILES/", 0) == 0 ? T.substr(6) : T;
}

const char *VfsType(const std::string &T)
{
    if (T == "ZIP")   return "VFSZipLayer";
    if (T == "DIR")   return "VFSDirLayer";
    if (T == "FILE")  return "VFSFileLayer";
    if (T == "DELTA") return "VFSDeltaLayer";
    return nullptr;
}

bool IsBinaryMode(const std::string &M) { return M == "Replace" || M == "Cave" || M == "Poke" || M == "Or"; }

//One hive tree -> a RegEdit per key path that carries values (or is an empty object: "create this key").
void EmitRegTree(const std::string &Path, const ordered_json &Tree, const ordered_json &Arch, ordered_json &Out)
{
    ordered_json Values = ordered_json::object();
    std::vector<std::pair<std::string, const ordered_json *>> Subkeys;
    for (const auto &[K, V] : Tree.items())
    {
        if (V.is_object()) Subkeys.emplace_back(K, &V);
        else if (!V.is_null()) Values[K] = V;
    }
    if (!Values.empty() || (Subkeys.empty() && Tree.empty()))
    {
        ordered_json L = { {"TYPE", "RegEdit"}, {"REGPATH", Path}, {"KEYVALUES", Values} };
        if (!Arch.is_null()) L["ARCHITECTURE"] = Arch;
        Out.push_back(std::move(L));
    }
    for (const auto &[K, V] : Subkeys) EmitRegTree(Path.empty() ? K : Path + "\\" + K, *V, Arch, Out);
}

ordered_json ContentOp(const std::string &Kind, const std::string &Payload, const std::string &Target,
                       const ordered_json &Source, const ordered_json &Size, const ordered_json &Submounts)
{
    ordered_json L = { {"TYPE", VfsType(Kind)}, {"PATH", Payload} };
    if (!Target.empty()) L["TARGET"] = Target;
    if (Source.is_string())
    {
        L["SOURCE"] = { {"TYPE", "ipfs"}, {"CID", Source} };
        if (Size.is_number()) L["SOURCE"]["SIZE"] = Size;
    }
    if (Submounts.is_array() && !Submounts.empty()) L["SUBMOUNTS"] = Submounts;
    return L;
}

//An EDIT op -> the engine op (FileEdit for text, BinaryPatch for bytes) on File.
ordered_json EditOp(const ordered_json &Op, const std::string &File)
{
    ordered_json L = Op;
    L["TYPE"] = IsBinaryMode(Str(Op.value("MODE", ordered_json()))) ? "BinaryPatch" : "FileEdit";
    L["FILE"] = File;
    return L;
}

//A KEEP address -> a DeclarePersist (the persistence engine's primitive).
ordered_json PersistOp(const std::string &Address, const ordered_json &Val)
{
    const size_t S = Address.find('/');
    const std::string Ns = Address.substr(0, S), Rest = S == std::string::npos ? std::string() : Address.substr(S + 1);
    ordered_json P = { {"TYPE", "DeclarePersist"} };
    if (Ns == "REG")
    {
        std::string K = Rest;
        std::replace(K.begin(), K.end(), '/', '\\');
        P["SCOPE"] = "registry";
        P["PATH"] = K;
    }
    else
    {
        P["SCOPE"] = "file";
        P["PATH"] = Rest;
    }
    if (Val.is_object())
    {
        if (Val.contains("NAME")) P["TARGET"] = Val["NAME"];
        if (Val.contains("CLOUD")) P["CLOUD"] = Val["CLOUD"];
    }
    return P;
}

// ---- the vocabulary ------------------------------------------------------------------------------------------
bool StrArray(const ordered_json &V)
{
    if (!V.is_array()) return false;
    for (const auto &E : V) if (!E.is_string()) return false;
    return true;
}

//A package file name: relative to the node's own package dir, or a runtime path anchored at a %VARIABLE%. A node
//arrives from a peer — an absolute name or a ".." would make a DOWNLOAD write outside the package. "" if fine.
std::string FileNameFault(const std::string &F)
{
    for (size_t A = 0, B; A <= F.size(); A = B + 1)
    {
        B = F.find_first_of("/\\", A);
        if (B == std::string::npos) B = F.size();
        if (F.compare(A, B - A, "..") == 0) return "file name must stay inside the package (no \"..\")";
    }
    const bool Anchored = !F.empty() && F.front() == '%';
    const bool Absolute = !F.empty() && (F.front() == '/' || F.front() == '\\' || (F.size() >= 2 && F[1] == ':'));
    if (Absolute && !Anchored) return "file name must be relative to the package (or anchored at a %VARIABLE%)";
    return std::string();
}

std::string CheckLayer(const ordered_json &L)
{
    if (!L.is_object()) return "is not an object";
    const std::string T = Fold::TypeOf(L);
    if (T.empty()) return "needs exactly one type key (ZIP FILE DELTA DIR NODE EDIT REG VARS ENV DLL EXEC KEEP ANY NOT)";
    static const std::map<std::string, std::set<std::string>> Allowed = {
        {"ZIP",   {"SOURCE", "SIZE", "TARGET", "SUBMOUNTS"}}, {"FILE",  {"SOURCE", "SIZE", "TARGET", "SUBMOUNTS"}},
        {"DELTA", {"SOURCE", "SIZE", "TARGET", "SUBMOUNTS"}}, {"DIR",   {"TARGET", "SUBMOUNTS"}},
        {"NODE",  {"TAKE", "TARGET"}}, {"EDIT", {"TARGET"}}, {"REG", {"ARCH"}},
        {"VARS", {}}, {"ENV", {}}, {"DLL", {}}, {"EXEC", {}}, {"KEEP", {}}, {"ANY", {}}, {"NOT", {}},
    };
    for (const auto &[K, V] : L.items())
    {
        if (K == T || K == "WHEN" || K == "COMMENT" || K == "LABEL" || K == "SECTION") continue;
        if (!Allowed.at(T).count(K)) return T + " layer has an unknown field '" + K + "'";
    }
    if (L.contains("WHEN") && !L["WHEN"].is_string()) return "WHEN must be a string (a condition)";
    if (L.contains("COMMENT") && !L["COMMENT"].is_string()) return "COMMENT must be a string";
    //LABEL + SECTION: what the layer is called and where it sits in the editor's tree ('/' nests). Presentation only —
    //the fold never reads them. Absent, the editor names the layer from its WHEN variable's UI facet (else its payload).
    if (L.contains("LABEL") && (!L["LABEL"].is_string() || L["LABEL"].get<std::string>().empty()))
        return "LABEL must be a non-empty string";
    if (L.contains("SECTION") && (!L["SECTION"].is_string() || L["SECTION"].get<std::string>().empty()))
        return "SECTION must be a non-empty path";
    if (L.contains("TARGET"))
    {
        const std::string Tg = Str(L["TARGET"]);
        if (!L["TARGET"].is_string()) return "TARGET must be a string";
        if (T != "NODE" && Tg != "FILES" && Tg.rfind("FILES/", 0) != 0) return T + " TARGET must name a FILES address";
    }
    const ordered_json &P = L[T];
    if (VfsType(T))
    {
        if (!P.is_string() || P.get<std::string>().empty()) return T + " needs a file name";
        if (const std::string Why = FileNameFault(P.get<std::string>()); !Why.empty()) return T + " " + Why;
        if (L.contains("SOURCE") && !L["SOURCE"].is_string()) return "SOURCE must be a CID (a string)";
        if (L.contains("SIZE") && !L["SIZE"].is_number()) return "SIZE must be a number";
        if (L.contains("SUBMOUNTS") && !StrArray(L["SUBMOUNTS"])) return "SUBMOUNTS must be a list of \"src:dst\" strings";
    }
    else if (T == "NODE")
    {
        if (!P.is_string() || P.get<std::string>().empty()) return "NODE must be a node CID";
        if (L.contains("TAKE"))
        {
            if (!L["TAKE"].is_array()) return "TAKE must be a list";
            //Where each named selection lands (a leaf keeps its last segment; a pair lands at its new name): two landing
            //on one address would leave the winner to match order. A whole namespace or a folder's contents lands many
            //addresses, merged like any other layers, and is not a collision.
            std::map<std::string, std::string> Landed;
            for (const auto &S : L["TAKE"])
            {
                if (!S.is_string() && !(S.is_array() && S.size() == 2 && S[0].is_string() && S[1].is_string()))
                    return "a TAKE selection is an address or an [address, new name] pair";
                const std::string Src = Str(S.is_array() ? S[0] : S);
                const std::string Ns = Src.substr(0, Src.find('/'));
                std::string At;
                if (S.is_array())
                {
                    At = Str(S[1]);
                    if (At.compare(0, Ns.size() + 1, Ns + "/") != 0) At = Ns + "/" + At;
                }
                else if (Src.find('/') != std::string::npos && Src.back() != '/')
                    At = Ns + "/" + Src.substr(Src.rfind('/') + 1);
                while (!At.empty() && At.back() == '/') At.pop_back();
                if (At.empty()) continue;
                if (Ns == "FILES" || Ns == "REG") At = LowerAscii(At);   // file systems and the registry ignore case
                if (const auto [It, Fresh] = Landed.emplace(At, Src); !Fresh)
                    return "TAKE selections '" + It->second + "' and '" + Src + "' both land at " + At;
            }
        }
        if (L.contains("TARGET") && Str(L["TARGET"]) != "FILES" && Str(L["TARGET"]).rfind("FILES/", 0) != 0)
            return "a NODE TARGET places files — it must name a FILES address";
    }
    else if (T == "EDIT")
    {
        if (!P.is_array() || P.empty()) return "EDIT needs a non-empty list of ops";
        if (!L.contains("TARGET") || FilesPath(L).empty()) return "EDIT needs a TARGET file";
        for (const auto &O : P)
        {
            if (!O.is_object() || !O.contains("MODE") || !O["MODE"].is_string()) return "every EDIT op needs a MODE";
            static const std::set<std::string> Text = { "MODE", "OFFSET", "EXPECT", "REPLACE", "VALUE", "PAYLOAD", "CAVE",
                                                        "ANCHOR", "KEY", "SECTION", "APPLY", "COMMENT", "WHEN" };
            for (const auto &[K, V] : O.items())
            {
                if (Text.count(K) && !V.is_string()) return "EDIT op field '" + K + "' must be a string";
                if (!V.is_string() && !V.is_boolean() && !V.is_number()) return "EDIT op field '" + K + "' must be a scalar";
            }
        }
    }
    else if (T == "REG")
    {
        if (!P.is_object()) return "REG must be a hive tree";
        if (L.contains("ARCH"))
        {
            if (!StrArray(L["ARCH"])) return "ARCH must be a list of \"32\"/\"64\"";
            for (const auto &A : L["ARCH"]) if (A != "32" && A != "64") return "ARCH must be a list of \"32\"/\"64\"";
        }
    }
    else if (T == "VARS")
    {
        if (!P.is_object()) return "VARS must be {KEY: declaration}";
        for (const auto &[K, D] : P.items())
        {
            if (K.empty() || !D.is_object()) return "VARS." + K + " must be a declaration object";
            for (const char *F : {"DEFAULT", "COMMENT", "WHEN"})
                if (D.contains(F) && !D[F].is_string()) return "VARS." + K + "." + F + " must be a string";
            if (D.contains("UI") && !D["UI"].is_object()) return "VARS." + K + ".UI must be an object";
            if (D.contains("UI") && D["UI"].contains("SECTION") && !D["UI"]["SECTION"].is_string())
                return "VARS." + K + ".UI.SECTION must be a path string";
            if (D.contains("EVAL") && !D["EVAL"].is_boolean()) return "VARS." + K + ".EVAL must be true or false";
        }
    }
    else if (T == "ENV" || T == "DLL")
    {
        if (!P.is_object()) return T + " must be {name: value}";
        for (const auto &[K, V] : P.items()) if (!V.is_string() && !V.is_null()) return T + "." + K + " must be a string or null";
    }
    else if (T == "EXEC")
    {
        if (!P.is_array() || P.empty()) return "EXEC must be a non-empty list of entries";
        std::set<std::string> Labels;
        for (const auto &E : P)
        {
            if (!E.is_object() || !E.contains("LABEL") || !E["LABEL"].is_string()) return "every EXEC entry needs a LABEL";
            if (!Labels.insert(E["LABEL"].get<std::string>()).second) return "two EXEC entries share the LABEL '" + E["LABEL"].get<std::string>() + "'";
            for (const char *F : {"HOST", "EXE", "WORKDIR", "CONTENT_ROOT"})
                if (E.contains(F) && !E[F].is_string()) return std::string("EXEC ") + F + " must be a string";
            for (const char *F : {"GUEST", "ARGS"})
                if (E.contains(F) && !E[F].is_null() && !StrArray(E[F])) return std::string("EXEC ") + F + " must be a list of strings";
            for (const char *F : {"PREFIX_GENERATE", "UNIFIED_RUNTIME"})
                if (E.contains(F) && !E[F].is_boolean()) return std::string("EXEC ") + F + " must be true or false";
            //A prefix runner's guest: GUEST_ROOTS {%Anchor%: the guest (Windows) path} — what a package's %GameDir%,
            //%SysDir32%, … are inside it — and DRIVES {drive: layout path}, where each drive letter lives in its layout.
            for (const char *F : {"GUEST_ROOTS", "DRIVES"})
                if (E.contains(F))
                {
                    if (!E[F].is_object()) return std::string(F) + " must be an object of paths";
                    for (const auto &[K, V] : E[F].items())
                        if (!V.is_string()) return std::string(F) + "." + K + " must be a path";
                }
            if (E.contains("GUEST_ROOTS"))
                for (const auto &[K, V] : E["GUEST_ROOTS"].items())
                {
                    if (K.size() < 3 || K.front() != '%' || K.back() != '%') return "GUEST_ROOTS keys are %Anchor%s ('" + K + "' is not)";
                    //An anchor on a drive the runner does not lay out would land at a literal "E:" folder.
                    const std::string G = V.get<std::string>();
                    if (G.size() >= 2 && std::isalpha(static_cast<unsigned char>(G[0])) && G[1] == ':')
                    {
                        bool Mapped = false;
                        if (E.contains("DRIVES"))
                            for (const auto &[D, To] : E["DRIVES"].items())
                                Mapped |= D.size() == 2 && D[1] == ':' && std::tolower(static_cast<unsigned char>(D[0])) == std::tolower(static_cast<unsigned char>(G[0]));
                        if (!Mapped) return "GUEST_ROOTS " + K + " is on drive " + G.substr(0, 2) + ", which DRIVES does not lay out";
                    }
                }
            //Anchors may be spelled from each other, never in a circle: "%A%" = "%A%\\x" (or A from B from A) has no
            //place — resolving it would only grow. A runner is shared JSON; refuse it here, before any launch reads it.
            if (E.contains("GUEST_ROOTS"))
            {
                const auto &Roots = E["GUEST_ROOTS"];
                std::map<std::string, int> State;                          // 1 = on the path, 2 = done
                std::string Cycle;
                std::function<bool(const std::string &)> Acyclic = [&](const std::string &A) -> bool {
                    State[A] = 1;
                    for (const std::string &Dep : VarSubst::TokenKeys(Roots[A].get<std::string>()))
                    {
                        const std::string D = "%" + Dep + "%";
                        if (!Roots.contains(D)) continue;
                        if (State[D] == 1) { Cycle = A + " → " + D; return false; }
                        if (State[D] == 0 && !Acyclic(D)) return false;
                    }
                    State[A] = 2;
                    return true;
                };
                for (const auto &[K, V] : Roots.items())
                    if (State[K] == 0 && !Acyclic(K)) return "GUEST_ROOTS anchors are spelled from each other in a circle (" + Cycle + ")";
            }
            if (E.contains("TILE"))
            {
                const auto &Ti = E["TILE"];
                if (!Ti.is_object()) return "TILE must be an object";
                for (const char *F : {"UID", "PARENTUID", "TITLE"})
                    if (Ti.contains(F) && !Ti[F].is_string()) return std::string("TILE.") + F + " must be a string";
                if (Ti.contains("META") && !Ti["META"].is_object()) return "TILE.META must be an object";
                if (Ti.contains("COVER") && !Ti["COVER"].is_object()) return "TILE.COVER must be {FILE, SOURCE, SIZE}";
                if (Ti.contains("COVER"))
                {
                    const ordered_json &C = Ti["COVER"];
                    if (C.contains("FILE") && (!C["FILE"].is_string() || C["FILE"].get<std::string>().empty()))
                        return "TILE.COVER.FILE must be a file name";
                    if (C.contains("FILE"))
                        if (const std::string Why = FileNameFault(C["FILE"].get<std::string>()); !Why.empty()) return "TILE.COVER " + Why;
                    if (C.contains("SOURCE") && !C["SOURCE"].is_string()) return "TILE.COVER.SOURCE must be a CID (a string)";
                }
            }
        }
    }
    else if (T == "KEEP")
    {
        if (!P.is_object()) return "KEEP must be {address: true | false | options}";
        for (const auto &[K, V] : P.items())
        {
            if (K.rfind("FILES", 0) != 0 && K.rfind("REG", 0) != 0 && K.rfind("VARS", 0) != 0)
                return "KEEP address '" + K + "' must be in FILES, REG or VARS";
            if (!V.is_boolean() && !V.is_object()) return "KEEP." + K + " must be true, false or {NAME, CLOUD}";
            //A pattern (* or ?) names files in ONE folder: only a FILES address, only in its last segment.
            if (HasWildcard(K))
            {
                if (K.rfind("FILES/", 0) != 0) return "KEEP address '" + K + "': a pattern (* ?) is only for FILES";
                std::string Parent = K.substr(0, K.find_last_of('/'));
                if (HasWildcard(Parent)) return "KEEP address '" + K + "': a pattern (* ?) may only be in the last segment";
                if (K.back() == '/') return "KEEP address '" + K + "': a pattern names files, not a folder";
            }
        }
    }
    else if (T == "ANY")
    {
        if (!StrArray(P) || P.empty()) return "ANY must be a non-empty list of node CIDs";
    }
    else if (T == "NOT")
    {
        if (!P.is_string() || P.get<std::string>().empty()) return "NOT must be a node CID";
    }
    return std::string();
}

} // namespace

std::string CheckNode(const nlohmann::ordered_json &J, const std::string &NodeId)
{
    const std::string Tag = "node '" + NodeId + "'";
    if (!J.is_object()) return Tag + ": not an object";
    static const std::set<std::string> Fields = { "CID", "LABEL", "POS", "COMMENT", "VARIANT", "RECOMMENDED", "SECTION", "LAYERS" };
    for (const auto &[K, V] : J.items())
        if (!Fields.count(K)) return Tag + ": unknown field '" + K + "' (a node is CID LABEL VARIANT RECOMMENDED SECTION LAYERS)";
    if (J.contains("LABEL") && !J["LABEL"].is_string()) return Tag + ": LABEL must be a string";
    if (J.contains("VARIANT") && (!J["VARIANT"].is_string() || J["VARIANT"].get<std::string>().empty()))
        return Tag + ": VARIANT must be a non-empty name";
    if (J.contains("RECOMMENDED") && !StrArray(J["RECOMMENDED"])) return Tag + ": RECOMMENDED must be a list of tile UIDs";
    //SECTION: where a graft sits in the pre-launch graft tree, a path ("Soundtrack", "Mods/Graphics"; '/' nests).
    if (J.contains("SECTION") && (!J["SECTION"].is_string() || J["SECTION"].get<std::string>().empty()))
        return Tag + ": SECTION must be a non-empty path";
    if (!J.contains("LAYERS") || !J["LAYERS"].is_array()) return Tag + ": LAYERS must be a list";
    for (size_t I = 0; I < J["LAYERS"].size(); ++I)
    {
        const std::string Why = CheckLayer(J["LAYERS"][I]);
        if (!Why.empty()) return Tag + ": layer " + std::to_string(I) + " — " + Why;
    }
    return std::string();
}

nlohmann::ordered_json LowerNode(const nlohmann::ordered_json &J, const std::string &NodeId)
{
    ordered_json Out = ordered_json::array();
    if (!CheckNode(J, NodeId).empty()) return Out;
    for (const auto &L : J["LAYERS"])
    {
        const std::string T = Fold::TypeOf(L);
        ordered_json Ops = ordered_json::array();
        if (VfsType(T))
            Ops.push_back(ContentOp(T, Str(L[T]), FilesPath(L), L.value("SOURCE", ordered_json()),
                                    L.value("SIZE", ordered_json()), L.value("SUBMOUNTS", ordered_json())));
        else if (T == "EDIT")
            for (const auto &O : L["EDIT"]) Ops.push_back(EditOp(O, FilesPath(L)));
        else if (T == "REG")
        {
            const ordered_json Arches = (L.contains("ARCH") && !L["ARCH"].empty()) ? L["ARCH"] : ordered_json::array({ nullptr });
            for (const auto &A : Arches) EmitRegTree(std::string(), L["REG"], A, Ops);
        }
        else if (T == "DLL")
        {
            for (const auto &[N, O] : L["DLL"].items())
                if (O.is_string()) Ops.push_back({ {"TYPE", "DllOverride"}, {"DLLOVERRIDE", N + "=" + O.get<std::string>()} });
        }
        else if (T == "VARS")
        {
            for (const auto &[K, D] : L["VARS"].items())
            {
                ordered_json V = { {"TYPE", "CustomVar"}, {"KEY", K} };
                for (const auto &[F, X] : D.items()) V[F] = X;
                Ops.push_back(std::move(V));
            }
        }
        else if (T == "KEEP")
        {
            for (const auto &[A, V] : L["KEEP"].items())
                if (!V.is_boolean() || V.get<bool>()) Ops.push_back(PersistOp(A, V));
        }
        for (auto &O : Ops)
        {
            if (L.contains("WHEN") && O.value("TYPE", std::string()) != "CustomVar") O["WHEN"] = L["WHEN"];
            Out.push_back(std::move(O));
        }
    }
    return Out;
}

nlohmann::ordered_json LowerEntry(const nlohmann::ordered_json &E)
{
    ordered_json L = ordered_json::object();
    if (!E.is_object()) return L;
    const bool Runner = E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty();
    if (Runner)
    {
        L["HOST"] = E.value("HOST", ordered_json(""));
        L["GUEST"] = E["GUEST"];
        L["EXECUTABLE"] = E.value("EXE", ordered_json(""));
        L["ARGS"] = E.contains("ARGS") && E["ARGS"].is_array() ? E["ARGS"] : ordered_json::array();
        for (const char *F : {"CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME", "GUEST_ROOTS", "DRIVES", "LABEL"})
            if (E.contains(F)) L[F] = E[F];
    }
    else
    {
        L["PLATFORM"] = E.value("HOST", ordered_json(""));
        if (E.contains("EXE")) L["CONTENTPATH"] = E["EXE"];
        if (E.contains("ARGS") && E["ARGS"].is_array()) L["EXEARGS"] = E["ARGS"];
        for (const char *F : {"WORKDIR", "LABEL"})
            if (E.contains(F)) L[F] = E[F];
    }
    return L;
}

bool UserOwned(const nlohmann::ordered_json &Keep, const std::string &Address)
{
    if (!Keep.is_object()) return false;
    const std::string A = LowerAscii(Address);
    size_t Best = 0;
    bool Owned = false, Any = false;
    for (const auto &[K, V] : Keep.items())
    {
        std::string Kl = LowerAscii(K);
        while (!Kl.empty() && Kl.back() == '/') Kl.pop_back();
        const std::string Hashless = A.substr(0, A.find('#'));
        const bool Covers = A == Kl || A.rfind(Kl + "/", 0) == 0 || A.rfind(Kl + "#", 0) == 0
                         || (HasWildcard(Kl) && Kl.rfind('/') != std::string::npos && Hashless.rfind('/') != std::string::npos
                             && Kl.substr(0, Kl.rfind('/')) == Hashless.substr(0, Hashless.rfind('/'))
                             && WildcardMatch(Kl.substr(Kl.rfind('/') + 1), Hashless.substr(Hashless.rfind('/') + 1)));   // a pattern keep: player*.hki, files in its own folder
        if (!Covers || (Any && Kl.size() < Best)) continue;
        Best = Kl.size(); Any = true;
        Owned = !(V.is_boolean() && !V.get<bool>());
    }
    return Owned;
}

namespace {

//A content layer seen through one TAKE, as mounts: (the path inside the layer, the FILES address it lands at). Each
//selection keeps the part of the layer it covers — the whole layer when it lies inside the selection, the selection
//alone when that lies inside the layer — named as TAKE names it (left-stripped, renamed). Empty: nothing is taken.
using Mount = std::pair<std::string, std::string>;
std::vector<Mount> ApplyTake(const std::vector<Mount> &In, const ordered_json &Take)
{
    if (Take.is_null() || (Take.is_array() && Take.empty())) return In;
    std::vector<Mount> Out;
    const auto Add = [&](const Mount &M) { if (std::find(Out.begin(), Out.end(), M) == Out.end()) Out.push_back(M); };
    for (const auto &[Src, A] : In)
        for (const auto &Sel : Take)
        {
            const bool Pair = Sel.is_array() && Sel.size() == 2;
            const std::string S = Pair ? Str(Sel[0]) : Sel.is_string() ? Sel.get<std::string>() : std::string();
            const std::string Ns = S.substr(0, S.find('/'));
            if (Ns != "FILES") continue;                                    // facts are not files
            std::string Base = S;
            while (!Base.empty() && Base.back() == '/') Base.pop_back();
            const bool Contents = !Pair && Base.size() != S.size();
            if (A == Base || A.rfind(Base + "/", 0) == 0)                   // the layer lies inside the selection
            {
                if (Contents && A == Base) Add({ Src, Ns });
                else if (auto T = Fold::TakeView(ordered_json::array({ Sel }), A)) Add({ Src, *T });
            }
            else if (Base.rfind(A + "/", 0) == 0)                           // the selection lies inside the layer
            {
                const std::string Rest = Base.substr(A.size() + 1);
                std::string To;
                if (Pair)
                {
                    To = Str(Sel[1]);
                    if (To.rfind(Ns + "/", 0) != 0) To = Ns + "/" + To;
                }
                else To = Contents ? Ns : Ns + "/" + Base.substr(Base.rfind('/') + 1);
                Add({ Src.empty() ? Rest : Src + "/" + Rest, To });
            }
        }
    return Out;
}

//Through a whole view ([TAKE, TARGET] steps, innermost first): each step's take, then its placement — the addresses
//end in the root's namespace.
std::vector<Mount> ApplyView(std::vector<Mount> Ms, const ordered_json &View)
{
    for (const auto &Step : View)
    {
        Ms = ApplyTake(Ms, Step[0]);
        const std::string At = Str(Step[1]);
        if (At.empty()) continue;
        for (auto &M : Ms) M.second = "FILES/" + Fold::PlaceUnder(M.second.size() > 6 ? M.second.substr(6) : std::string(), At);
    }
    return Ms;
}

} // namespace

nlohmann::ordered_json LowerPlan(const Fold::Plan &P)
{
    ordered_json Out = ordered_json::array();

    //Package-owned inside a user-owned subtree: re-applied after the user's state is restored.
    const auto TakenBack = [&](const std::string &Address) {
        std::string Parent = Address;
        const size_t Hash = Parent.find('#');
        if (Hash != std::string::npos) Parent = Parent.substr(0, Hash);
        bool Inside = false;
        for (const auto &[K, V] : P.Keep.items())
        {
            std::string Kl = LowerAscii(K);
            while (!Kl.empty() && Kl.back() == '/') Kl.pop_back();
            if (!(V.is_boolean() && !V.get<bool>()) && LowerAscii(Parent).rfind(Kl + "/", 0) == 0) Inside = true;
        }
        return Inside && !UserOwned(P.Keep, Address);
    };

    for (const Fold::Item &I : P.Seq)
    {
        if (I.Kind == "EDIT")
        {
            for (const auto &O : I.Ops)
            {
                ordered_json L = EditOp(O, I.Target);
                const std::string Addr = "FILES/" + I.Target
                                       + (Str(O.value("MODE", ordered_json())) == "ConfigWrite" ? "#" + Str(O.value("KEY", ordered_json())) : std::string());
                //An EDIT applies to the value beneath it: the composed files, so after the mount. On a user-owned
                //address it is the package's default — applied while the user has no saved copy of the file.
                //(A BinaryPatch always runs after the mount.)
                if (L["TYPE"] == "FileEdit")
                {
                    L["OVERRIDE"] = true;
                    if (UserOwned(P.Keep, Addr)) L["IF_UNSAVED"] = true;
                }
                Out.push_back(std::move(L));
            }
            continue;
        }
        const std::string Path = (I.Payload.find('%') != std::string::npos || I.Dir.empty())
                               ? I.Payload : (std::filesystem::path(I.Dir) / I.Payload).string();
        const ordered_json &Sub = I.Submounts;
        if (I.View.is_null())
        {
            Out.push_back(ContentOp(I.Kind, Path, I.Target, I.Source, I.Size, Sub));
            continue;
        }
        //Seen through TAKE: only what is taken mounts, where TAKE puts it — as submounts (the FS's "src:dst" view of
        //a source). A FILE is its bundle directory with the one file submounted, so a TAKE can rename it too.
        const std::string Name = std::filesystem::path(I.Payload).filename().string();
        const std::string OwnAddr = I.Own.empty() ? std::string("FILES") : "FILES/" + I.Own;
        std::vector<Mount> Ms;
        if (I.Kind == "FILE") Ms.push_back({ Name, OwnAddr + "/" + Name });
        else if (I.Submounts.is_array() && !I.Submounts.empty())
            for (const auto &S : I.Submounts)
            {
                const std::string X = Str(S);
                const size_t C = X.find(':');
                if (C != std::string::npos) Ms.push_back({ X.substr(0, C), "FILES/" + X.substr(C + 1) });
            }
        else Ms.push_back({ std::string(), OwnAddr });
        Ms = ApplyView(Ms, I.View);
        if (Ms.empty()) continue;                                            // nothing of this layer is taken
        ordered_json Mounts = ordered_json::array();
        for (const auto &[Src, Addr] : Ms)
            Mounts.push_back(Src + ":" + (Addr.size() > 6 ? Addr.substr(6) : std::string()));   // strip "FILES/"
        if (I.Kind == "FILE")
            Out.push_back(ContentOp("DIR", std::filesystem::path(Path).parent_path().string(), I.Target, ordered_json(), ordered_json(), Mounts));
        else
            Out.push_back(ContentOp(I.Kind, Path, I.Target, I.Source, I.Size, Mounts));
    }
    //The folded registry, one RegEdit per (architecture, key, ownership) — values in fold order within a key. A value
    //is judged by its own address (REG/<key>/<value>): one the package takes back inside a kept key (UserPatch's
    //options in a key whose volumes are the user's) goes to the key's OVERRIDE RegEdit, re-applied after the user's
    //state is restored.
    std::map<std::string, ordered_json> ByKey;
    std::vector<std::string> KeyOrder;
    for (const auto &[K, V] : P.Reg)
    {
        const std::string Arch = V.Arch.is_null() ? std::string() : Str(V.Arch);
        std::string A = "REG/" + V.Path + "/" + V.Name;
        std::replace(A.begin(), A.end(), '\\', '/');
        const bool Over = TakenBack(A);
        const std::string Kk = Arch + '\x1f' + LowerAscii(V.Path) + '\x1f' + (Over ? "o" : "b");
        auto It = ByKey.find(Kk);
        if (It == ByKey.end())
        {
            ordered_json R = { {"TYPE", "RegEdit"}, {"REGPATH", V.Path}, {"KEYVALUES", ordered_json::object()} };
            if (!V.Arch.is_null()) R["ARCHITECTURE"] = V.Arch;
            if (Over) R["OVERRIDE"] = true;
            It = ByKey.emplace(Kk, std::move(R)).first;
            KeyOrder.push_back(Kk);
        }
        It->second["KEYVALUES"][V.Name] = V.Value;
    }
    for (const auto &[K, V] : P.RegKeys)
    {
        const std::string Arch = V.first.is_null() ? std::string() : Str(V.first);
        const std::string Kk = Arch + '\x1f' + LowerAscii(V.second) + '\x1f' + "b";
        if (ByKey.count(Kk) || ByKey.count(Kk.substr(0, Kk.size() - 1) + "o")) continue;
        ordered_json R = { {"TYPE", "RegEdit"}, {"REGPATH", V.second}, {"KEYVALUES", ordered_json::object()} };
        if (!V.first.is_null()) R["ARCHITECTURE"] = V.first;
        ByKey.emplace(Kk, std::move(R));
        KeyOrder.push_back(Kk);
    }
    for (const auto &K : KeyOrder) Out.push_back(ByKey[K]);
    for (const auto &[N, O] : P.Dll.items())
        if (O.is_string()) Out.push_back({ {"TYPE", "DllOverride"}, {"DLLOVERRIDE", N + "=" + O.get<std::string>()} });
    for (const auto &[K, D] : P.Decls.items())
    {
        ordered_json V = { {"TYPE", "CustomVar"}, {"KEY", K} };
        if (D.is_object()) for (const auto &[F, X] : D.items()) V[F] = X;
        Out.push_back(std::move(V));
    }
    for (const auto &[A, V] : P.Keep.items())
        if (!V.is_boolean() || V.get<bool>()) Out.push_back(PersistOp(A, V));
    return Out;
}

} // namespace NodeLower
