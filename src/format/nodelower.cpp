#include "nodelower.h"

#include <optional>
#include "contentfile.h"
#include "commonutils.h"
#include "fold.h"
#include "varsubst.h"
#include "keynames.h"

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

bool IsBinaryMode(const std::string &M) { return M == "Replace" || M == "ReplaceAll" || M == "Cave" || M == "Poke" || M == "Or"; }

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
                       const ordered_json &Source, const ordered_json &Size, const ordered_json &Submounts,
                       const ordered_json &BaseSources = ordered_json())
{
    ordered_json L = { {"TYPE", VfsType(Kind)}, {"PATH", Payload} };
    if (!Target.empty()) L["TARGET"] = Target;
    if (BaseSources.is_array()) L["BASE_SOURCES"] = BaseSources;   // a delta's bases, by SOURCE CID, in order
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

//A KEEP entry's DeclarePersists: a registry key kept for both views is two keys in the hive, one per ARCH.
std::vector<ordered_json> PersistOps(const std::string &Address, const ordered_json &Val)
{
    const ordered_json P = PersistOp(Address, Val);
    if (P["SCOPE"] != "registry" || !Val.is_object() || !Val.contains("ARCH")) return { P };
    std::vector<ordered_json> Out;
    for (const auto &A : Val["ARCH"])
    {
        ordered_json O = P;
        O["ARCHITECTURE"] = A;
        Out.push_back(std::move(O));
    }
    return Out;
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
        {"ZIP",   {"SOURCE", "SIZE", "TARGET"}}, {"FILE",  {"SOURCE", "SIZE", "TARGET"}},
        {"DELTA", {"SOURCE", "SIZE", "TARGET", "BASE_SOURCES"}}, {"DIR",   {"TARGET"}},
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
    //A delta names what it was built over: the SOURCE CIDs of the layers whose bytes, concatenated in order, are its
    //base. Required — there is no implicit base — and a CID named twice is a concatenation nobody built.
    if (T == "DELTA")
    {
        if (!L.contains("BASE_SOURCES")) return "DELTA needs BASE_SOURCES: the SOURCE CIDs of the layers it was built over";
        const auto &B = L["BASE_SOURCES"];
        if (!StrArray(B) || B.empty()) return "BASE_SOURCES must be a non-empty list of CIDs";
        std::set<std::string> Seen;
        for (const auto &E : B)
        {
            const std::string C = E.get<std::string>();
            if (C.empty()) return "BASE_SOURCES has an empty CID";
            if (!Seen.insert(C).second) return "BASE_SOURCES names '" + C + "' twice";
        }
    }
    const ordered_json &P = L[T];
    if (VfsType(T))
    {
        if (!P.is_string() || P.get<std::string>().empty()) return T + " needs a file name";
        if (const std::string Why = FileNameFault(P.get<std::string>()); !Why.empty()) return T + " " + Why;
        if (L.contains("SOURCE") && !L["SOURCE"].is_string()) return "SOURCE must be a CID (a string)";
        if (L.contains("SIZE") && !L["SIZE"].is_number()) return "SIZE must be a number";
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
            //Delete removes the TARGET (a file or a folder) and nothing else: a value or a site would be read by nothing.
            if (O["MODE"] == "Delete")
                for (const auto &[K, V] : O.items())
                    if (K != "MODE" && K != "COMMENT" && K != "WHEN")
                        return "a Delete op takes no '" + K + "' (it removes its TARGET; only COMMENT and WHEN go with it)";
        }
    }
    else if (T == "REG")
    {
        if (!P.is_object()) return "REG must be a hive tree";
        //ARCH is mandatory: without it a 32-bit game's HKLM\Software keys land in the 64-bit view and the game never
        //sees them (Thief 3 hung, RollerCoaster Tycoon 2 asked for its CD, Colin McRae 2005 crashed, 2026-10-26).
        if (!L.contains("ARCH")) return "REG needs ARCH: the bitness of the program that reads it ([\"32\"] and/or [\"64\"])";
        if (!StrArray(L["ARCH"]) || L["ARCH"].empty()) return "ARCH must be a list of \"32\"/\"64\"";
        for (const auto &A : L["ARCH"]) if (A != "32" && A != "64") return "ARCH must be a list of \"32\"/\"64\"";
        for (const auto &[Hive, Keys] : P.items())
            if (Keys.is_object())
                for (const auto &[K, Sub] : Keys.items())
                    if (Sub.is_object() && Sub.contains("Wow6432Node"))   // ARCH places the 32-bit view; never by hand
                        return "REG must not name Wow6432Node (ARCH [\"32\"] puts the keys there)";
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
            if (D.contains("UI") && D["UI"].contains("CONTROL"))
            {
                static const std::set<std::string> Controls = {"bool", "int", "float", "text", "enum", "secret", "key"};
                const auto &C = D["UI"]["CONTROL"];
                if (!C.is_string() || !Controls.count(C.get<std::string>()))
                    return "VARS." + K + ".UI.CONTROL must be one of bool, int, float, text, enum, secret, key";
                //A key option holds a key name (spec 08 §8.3); a computed DEFAULT is checked at launch.
                const std::string Def = D.value("DEFAULT", std::string());
                if (C == "key" && Def.find('%') == std::string::npos && !KeyNames::IsValue(Def))
                    return "VARS." + K + ".DEFAULT '" + Def + "' is not a key name (spec 08 §8.3)";
            }
            if (D.contains("EVAL") && !D["EVAL"].is_boolean()) return "VARS." + K + ".EVAL must be true or false";
            //A MAP table (spec 08 §8.3): strings to strings, read through %KEY:map:TABLE%. It is data, never an option.
            if (D.contains("MAP"))
            {
                if (!D["MAP"].is_object()) return "VARS." + K + ".MAP must be {value: mapped value}";
                for (const auto &[E, V] : D["MAP"].items())
                    if (!V.is_string()) return "VARS." + K + ".MAP." + E + " must be a string";
                if (D.contains("UI")) return "VARS." + K + " is a MAP table: a table has no UI";
            }
            if (D.contains("ELSE") && (!D["ELSE"].is_string() || !D.contains("MAP")))
                return "VARS." + K + ".ELSE must be a string, on a MAP table";
        }
    }
    else if (T == "ENV" || T == "DLL")
    {
        if (!P.is_object()) return T + " must be {name: value}";
        for (const auto &[K, V] : P.items())
        {
            if (K.empty()) return T + " has an entry with no name";      // a DLL "" overrides nothing; an ENV "" sets nothing
            if (!V.is_string() && !V.is_null()) return T + "." + K + " must be a string or null";
        }
    }
    else if (T == "EXEC")
    {
        if (!P.is_array() || P.empty()) return "EXEC must be a non-empty list of entries";
        std::set<std::string> Labels;
        for (const auto &E : P)
        {
            if (!E.is_object() || !E.contains("LABEL") || !E["LABEL"].is_string()) return "every EXEC entry needs a LABEL";
            if (!Labels.insert(E["LABEL"].get<std::string>()).second) return "two EXEC entries share the LABEL '" + E["LABEL"].get<std::string>() + "'";
            for (const char *F : {"HOST", "EXE", "WORKDIR", "CONTENT_ROOT", "RECOMMENDEDRUNNER"})
                if (E.contains(F) && !E[F].is_string()) return std::string("EXEC ") + F + " must be a string";
            if (E.contains("RECOMMENDEDRUNNER") && E.contains("GUEST") && E["GUEST"].is_array() && !E["GUEST"].empty())
                return "EXEC RECOMMENDEDRUNNER is a game's: a runner entry recommends no runner";
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
                    //A guest path is on a drive (C:\x), or spelled from a variable: %RuntimePath% for a guest that IS the
                    //runtime, another anchor, a VAR. Anything else ("n64/x", "/opt/x") is no place in any guest.
                    const std::string G = V.get<std::string>();
                    const bool OnDrive = G.size() >= 2 && std::isalpha(static_cast<unsigned char>(G[0])) && G[1] == ':';
                    if (!OnDrive && (G.empty() || G.front() != '%'))
                        return "GUEST_ROOTS " + K + " is '" + G + "' — a guest path is on a drive DRIVES lays out (C:\\…) or starts at %RuntimePath%";
                    //An anchor on a drive the runner does not lay out would land at a literal "E:" folder.
                    if (OnDrive)
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
                    //A cover folds whole (one image), so it names its file itself.
                    const ordered_json &C = Ti["COVER"];
                    if (!C.contains("FILE")) return "TILE.COVER needs its FILE (a cover replaces the one beneath it whole)";
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
            //What a package keeps is the user's files and registry. A variable's value is never kept: what a user
            //picks lives in the instance, so a VARS/ address has nothing to keep.
            const std::string Ns = K.substr(0, K.find('/'));
            if (Ns != "FILES" && Ns != "REG") return "KEEP address '" + K + "' must be in FILES or REG";
            if (!V.is_boolean() && !V.is_object()) return "KEEP." + K + " must be true, false or {NAME, CLOUD, ARCH}";
            //ARCH: the bitness of the program that reads the key, as on a REG layer. A 32-bit program's HKLM\Software
            //is a different key (under Wow6432Node), so a key kept there names its view; without it nothing is kept.
            if (V.is_object() && V.contains("ARCH"))
            {
                const auto &Ar = V["ARCH"];
                if (Ns != "REG") return "KEEP address '" + K + "': ARCH is for a registry key";
                if (!StrArray(Ar) || Ar.empty() || Ar.size() > 2) return "KEEP." + K + ".ARCH must be a list of \"32\" and/or \"64\"";
                for (const auto &A : Ar) if (A != "32" && A != "64") return "KEEP." + K + ".ARCH must be a list of \"32\" and/or \"64\"";
                if (Ar.size() == 2 && Ar[0] == Ar[1]) return "KEEP." + K + ".ARCH names one bitness twice";
            }
            if (Ns == "REG")
            {
                //The segments as the key is named (a doubled separator names no extra key, and '\\' separates too).
                std::vector<std::string> Seg;
                std::string Cur;
                for (const char C : (K.size() > 4 ? K.substr(4) : std::string()) + "/")
                    if (C == '/' || C == '\\') { if (!Cur.empty()) Seg.push_back(LowerAscii(Cur)); Cur.clear(); }
                    else Cur.push_back(C);
                //ARCH places the 32-bit view, as on a REG layer; a kept address never names it by hand.
                for (const std::string &G : Seg)
                    if (G == "wow6432node") return "KEEP address '" + K + "' must not name Wow6432Node (ARCH [\"32\"] keeps the key there)";
                if (!(V.is_boolean() && !V.get<bool>()) && !(V.is_object() && V.contains("ARCH"))
                    && Seg.size() >= 2 && (Seg[0] == "hklm" || Seg[0] == "hkey_local_machine") && Seg[1] == "software")
                    return "KEEP address '" + K + "' needs ARCH ([\"32\"] for a 32-bit program, as on its REG layer): "
                           "a 32-bit program's HKLM\\Software is a different key";
            }
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

std::string CheckNode(const nlohmann::ordered_json &J, const std::string &NodeId, const std::string &Label)
{
    const std::string Tag = "node '" + NodeId + "'" + (Label.empty() || Label == NodeId ? std::string() : " (" + Label + ")");
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
        {
            Ops.push_back(ContentOp(T, Str(L[T]), FilesPath(L), L.value("SOURCE", ordered_json()),
                                    L.value("SIZE", ordered_json()), ordered_json(), L.value("BASE_SOURCES", ordered_json())));
        }
        else if (T == "EDIT")
            for (const auto &O : L["EDIT"]) Ops.push_back(EditOp(O, FilesPath(L)));
        else if (T == "REG")
        {
            //No ARCH (a node folded without being checked: CheckLayer requires one) is the key as named.
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
                if (!V.is_boolean() || V.get<bool>()) for (auto &O : PersistOps(A, V)) Ops.push_back(std::move(O));
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
        for (const char *F : {"WORKDIR", "LABEL", "RECOMMENDEDRUNNER"})
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

std::string KeptUnder(const nlohmann::ordered_json &Keep, const std::string &Address)
{
    if (!Keep.is_object()) return std::string();
    //Normalised first ("a/../Saves", "./Saves", "a//b" are the places they name); the launch judges the RESOLVED path
    //again (FileEdits::KeptPathMet), which also sees a place spelled from another anchor or a variable.
    std::string A = LowerAscii(std::filesystem::path(Address).lexically_normal().generic_string());
    while (!A.empty() && A.back() == '/') A.pop_back();
    for (const auto &[K, V] : Keep.items())
    {
        if (V.is_boolean() && !V.get<bool>()) continue;                  // a take-back keeps nothing
        std::string Kl = LowerAscii(K);
        while (!Kl.empty() && Kl.back() == '/') Kl.pop_back();
        Kl = Kl.substr(0, Kl.find('#'));                                  // a kept key lives in its file
        if (HasWildcard(Kl))
        {
            //A pattern keeps matching files in one folder: the Delete meets it on one of those files, or on that
            //folder or anything holding it.
            const std::string Dir = Kl.substr(0, Kl.rfind('/'));
            const size_t S = A.rfind('/');
            if ((S != std::string::npos && A.substr(0, S) == Dir && WildcardMatch(Kl.substr(Kl.rfind('/') + 1), A.substr(S + 1)))
                || A == Dir || Dir.rfind(A + "/", 0) == 0)
                return K;
            continue;
        }
        //The kept address itself, anything inside it (a kept folder is served live, taken back or not), or a folder
        //holding it.
        if (A == Kl || A.rfind(Kl + "/", 0) == 0 || Kl.rfind(A + "/", 0) == 0) return K;
    }
    return std::string();
}

std::string EditAddress(const nlohmann::ordered_json &Op, const std::string &Target)
{
    const std::string Mode = Str(Op.value("MODE", ordered_json()));
    return "FILES/" + Target + (Mode == "ConfigWrite" || Mode == "JsonSet" || Mode == "XmlSet" ? "#" + Str(Op.value("KEY", ordered_json())) : std::string());
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

std::vector<std::pair<size_t, nlohmann::ordered_json>> ContentOps(const Fold::Plan &P)
{
    std::vector<std::pair<size_t, ordered_json>> Out;
    const auto Cid = [](const Fold::Item &I) { return I.Source.is_string() ? I.Source.get<std::string>() : std::string(); };
    const auto Archive = [](const Fold::Item &I) { return I.Kind == "ZIP" || I.Kind == "DELTA"; };
    //Where each content item mounts: whole at its target, or (seen through a TAKE) the submounts its takes leave —
    //none when nothing of it is taken.
    struct Shape { std::string Path; std::optional<ordered_json> Mounts; bool Shown = true; };
    std::vector<Shape> Sh(P.Seq.size());
    for (size_t J = 0; J < P.Seq.size(); ++J)
    {
        const Fold::Item &I = P.Seq[J];
        if (I.Kind == "EDIT") continue;
        Sh[J].Path = (VarSubst::RuntimeSourced(I.Payload) || I.Dir.empty())
                   ? I.Payload
                   : ContentFile::Path(I.Dir, I.Payload, Cid(I)).generic_string();   // '/' on every host: the plan is
                                                                                    // data; a bumped name when versions share it
        if (I.View.is_null()) continue;
        //Seen through TAKE: only what is taken mounts, where TAKE puts it — as submounts (the FS's "src:dst" view of
        //a source). A FILE is its bundle directory with the one file submounted, so a TAKE can rename it too.
        const std::string Name = std::filesystem::path(I.Payload).filename().string();
        const std::string OwnAddr = I.Own.empty() ? std::string("FILES") : "FILES/" + I.Own;
        std::vector<Mount> Ms;
        //The source is the file as it sits on disk (a bumped name when versions share it); the address keeps the
        //name the node gives it, which is what TAKE selects and places.
        if (I.Kind == "FILE") Ms.push_back({ std::filesystem::path(Sh[J].Path).filename().string(), OwnAddr + "/" + Name });
        else Ms.push_back({ std::string(), OwnAddr });
        Ms = ApplyView(Ms, I.View);
        Sh[J].Shown = !Ms.empty();
        ordered_json Mounts = ordered_json::array();
        for (const auto &[Src, Addr] : Ms)
            Mounts.push_back(Src + ":" + (Addr.size() > 6 ? Addr.substr(6) : std::string()));   // strip "FILES/"
        Sh[J].Mounts = std::move(Mounts);
    }
    //A file a delta in this launch reads is mounted even when no TAKE shows it — for its bytes only, once, unless it
    //is shown somewhere already (one CID is one file). Only a zip or a delta has bytes a delta can read.
    std::set<std::string> Have;
    for (size_t J = 0; J < P.Seq.size(); ++J) if (Archive(P.Seq[J]) && Sh[J].Shown && !Cid(P.Seq[J]).empty()) Have.insert(Cid(P.Seq[J]));
    std::vector<bool> BytesOnly(P.Seq.size(), false);
    std::vector<std::string> Want;
    for (size_t J = 0; J < P.Seq.size(); ++J)
        if (P.Seq[J].Kind == "DELTA" && Sh[J].Shown && P.Seq[J].BaseSources.is_array())
            for (const auto &C : P.Seq[J].BaseSources) if (C.is_string()) Want.push_back(C.get<std::string>());
    while (!Want.empty())
    {
        const std::string C = Want.back();
        Want.pop_back();
        if (!Have.insert(C).second) continue;
        for (size_t J = 0; J < P.Seq.size(); ++J)
            if (Archive(P.Seq[J]) && Cid(P.Seq[J]) == C)
            {
                BytesOnly[J] = true;
                if (P.Seq[J].Kind == "DELTA" && P.Seq[J].BaseSources.is_array())
                    for (const auto &B : P.Seq[J].BaseSources) if (B.is_string()) Want.push_back(B.get<std::string>());
                break;
            }
    }
    for (size_t J = 0; J < P.Seq.size(); ++J)
    {
        const Fold::Item &I = P.Seq[J];
        if (I.Kind == "EDIT") continue;
        if (!Sh[J].Shown)
        {
            if (!BytesOnly[J]) continue;                                      // nothing of it is taken, nothing reads it
            ordered_json L = ContentOp(I.Kind, Sh[J].Path, I.Target, I.Source, I.Size, ordered_json(), I.BaseSources);
            L["BYTES_ONLY"] = true;
            Out.emplace_back(J, std::move(L));
            continue;
        }
        if (!Sh[J].Mounts)
            Out.emplace_back(J, ContentOp(I.Kind, Sh[J].Path, I.Target, I.Source, I.Size, ordered_json(), I.BaseSources));
        else if (I.Kind == "FILE")
            Out.emplace_back(J, ContentOp("DIR", std::filesystem::path(Sh[J].Path).parent_path().generic_string(), I.Target,
                                          ordered_json(), ordered_json(), *Sh[J].Mounts));
        else
            Out.emplace_back(J, ContentOp(I.Kind, Sh[J].Path, I.Target, I.Source, I.Size, *Sh[J].Mounts, I.BaseSources));
    }
    return Out;
}

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

    std::map<size_t, ordered_json> Content;           // a content item's op, by its place in the sequence
    for (auto &[J, Op] : ContentOps(P)) Content.emplace(J, std::move(Op));
    for (size_t J = 0; J < P.Seq.size(); ++J)
    {
        const Fold::Item &I = P.Seq[J];
        if (I.Kind == "EDIT")
        {
            for (const auto &O : I.Ops)
            {
                ordered_json L = EditOp(O, I.Target);
                const std::string Addr = EditAddress(O, I.Target);
                //An EDIT applies to the value beneath it: the composed files, so after the mount. On a user-owned
                //address it is the package's default — applied while the user has no saved copy of the file.
                //(A BinaryPatch always runs after the mount.)
                if (L["TYPE"] == "FileEdit")
                {
                    L["OVERRIDE"] = true;
                    //A Delete never reaches the user's state (a removal in a kept folder would be stored with their
                    //saves): one that would is carried with the kept address it meets, and the launch refuses it.
                    if (Str(O.value("MODE", ordered_json())) == "Delete")
                    {
                        if (const std::string K = KeptUnder(P.Keep, Addr); !K.empty()) L["KEPT"] = K;
                    }
                    else if (UserOwned(P.Keep, Addr)) L["IF_UNSAVED"] = true;
                }
                Out.push_back(std::move(L));
            }
            continue;
        }
        if (const auto It = Content.find(J); It != Content.end()) Out.push_back(std::move(It->second));
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
        if (!V.is_boolean() || V.get<bool>()) for (auto &O : PersistOps(A, V)) Out.push_back(std::move(O));
    return Out;
}

} // namespace NodeLower
