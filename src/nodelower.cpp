#include "nodelower.h"

#include <set>
#include <vector>

using nlohmann::ordered_json;

namespace NodeLower
{

namespace {

//A Content node's FORM selects how its PATH is interpreted; the executor still distinguishes these by
//layer TYPE, so the four collapse on disk and re-expand here.
const char *VfsTypeForForm(const std::string &Form)
{
    if (Form == "zip")   return "VFSZipLayer";
    if (Form == "dir")   return "VFSDirLayer";
    if (Form == "file")  return "VFSFileLayer";
    if (Form == "delta") return "VFSDeltaLayer";
    return nullptr;
}

void CopyIf(const ordered_json &From, ordered_json &To, std::initializer_list<const char *> Keys)
{
    for (const char *K : Keys) if (From.contains(K)) To[K] = From[K];
}

//Walk one hive tree, emitting a RegEdit layer per key path that carries values (or that is an EMPTY
//object — "create this key, no values", which the old schema spelled as a null KEYVALUES).
//A key may hold values AND subkeys at once, so both are handled at every level.
void EmitRegTree(const std::string &Path, const ordered_json &Tree, const ordered_json &Arch,
                 bool Override, const std::string &When, ordered_json &Out)
{
    ordered_json Values = ordered_json::object();
    std::vector<std::pair<std::string, const ordered_json *>> Subkeys;
    for (const auto &[K, V] : Tree.items())
    {
        if (V.is_object()) Subkeys.emplace_back(K, &V);
        else               Values[K] = V;
    }
    if (!Values.empty() || (Subkeys.empty() && Tree.empty()))
    {
        ordered_json L = {{"TYPE", "RegEdit"}, {"REGPATH", Path}, {"KEYVALUES", Values}};
        if (!Arch.is_null()) L["ARCHITECTURE"] = Arch;
        if (Override)        L["OVERRIDE"]     = true;
        if (!When.empty())   L["WHEN"]         = When;
        Out.push_back(std::move(L));
    }
    for (const auto &[K, V] : Subkeys)
    {
        std::string Child = Path;
        Child += '\\';
        Child += K;
        EmitRegTree(Child, *V, Arch, Override, When, Out);
    }
}

} // namespace

//A raw ENTRYPOINTS entry → the engine's exec block. Exec and runner are ONE declaration: HOST is the platform I
//need, GUEST the platforms I provide. No GUEST ⇒ nothing runs inside me ⇒ I am the terminal link, a launchable.
static ordered_json LowerEntrypointOrThrow(const ordered_json &E, const std::string &NodeId, std::string &Err)
{
    Err.clear();
    auto Fail = [&](const std::string &Why) { Err = "node '" + NodeId + "': ENTRYPOINTS entry " + Why; return ordered_json(); };
    if (!E.is_object()) return Fail("is not an object");
    if (E.contains("GUEST") && !E["GUEST"].is_array()) return Fail("GUEST must be a list of platforms");
    if (!E.contains("HOST") || !E["HOST"].is_string()) return Fail("has no HOST platform");
    if (E.contains("WHEN"))
        return Fail("carries a WHEN — an entrypoint is never conditional (its payload becomes the node's identity at index time). Use two entries, or two variants.");
    if (E.contains("RECOMMENDED"))
        return Fail("carries RECOMMENDED — that is a node facet (prefer me among my siblings), not an entry field; the first entry is the default");
    if (E.contains("ENV") || E.contains("ENV_REMOVE"))
        return Fail("carries ENV/ENV_REMOVE — the environment is a NODE section that folds along the chain (like the registry), not an entry field; move it up to the node");
    const bool IsRunner = E.contains("GUEST") && !E["GUEST"].empty();
    ordered_json L = ordered_json::object();
    if (IsRunner)
    {
        L["HOST"]  = E["HOST"];
        L["GUEST"] = E["GUEST"];
        L["EXECUTABLE"] = E.value("PATH", std::string());
        L["ARGS"] = E.contains("ARGS") ? E["ARGS"] : ordered_json::array();
        CopyIf(E, L, {"CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME", "LABEL"});
    }
    else
    {
        L["PLATFORM"] = E["HOST"];
        if (E.contains("PATH")) L["CONTENTPATH"] = E["PATH"];
        if (E.contains("ARGS")) L["EXEARGS"]     = E["ARGS"];
        CopyIf(E, L, {"LABEL", "WORKDIR", "RUNNER"});
    }
    return L;
}

//The real work. Every read below assumes a well-typed payload; Lower() converts any type mismatch into a
//refusal, so this is free to be written for the shape the format defines rather than defensively field by field.
static ordered_json LowerOrThrow(const ordered_json &J, const std::string &NodeId, std::string &Err)
{
    Err.clear();
    ordered_json Out = ordered_json::array();
    auto Fail = [&](const std::string &Why) { Err = "node '" + NodeId + "': " + Why; return ordered_json::array(); };
    //A node-level WHEN gates the whole contribution; applied to every emitted layer at the end.
    //A non-string WHEN used to be silently IGNORED, so the layer applied UNCONDITIONALLY — the exact "inert-
    //looking in the file, live at launch" failure this mechanism exists to prevent.
    const bool HasWhen = J.contains("WHEN");
    if (J.contains("WHEN") && !J["WHEN"].is_string())
        return Fail("WHEN must be a string (a condition), not " + std::string(J["WHEN"].type_name()));

    //STRICT VOCABULARY: a key the format does not define is refused. Without this a typo'd payload key
    //("LAYER", "PATCHS") is a node that validates clean and applies nothing — the one silent failure the
    //whole front-end exists to make impossible.
    static const std::set<std::string> Known = {
        "CID", "LABEL", "WHEN", "TOGGLE", "PUBLISH", "POS", "COMMENT", "TILE", "ENTRYPOINTS", "OVER", "VARIANT", "RECOMMENDED",
        "LAYERS", "PATCHES", "FILEEDITS", "REGEDITS", "DLLOVERRIDES", "VARS", "PERSISTS", "ENV", "ENV_REMOVE",
    };
    for (const auto &[K, V] : J.items())
        if (!Known.count(K))
            return Fail("unknown field '" + K + "' (a node is CID/LABEL/WHEN/TOGGLE/PUBLISH/TILE/ENTRYPOINTS/OVER + "
                        "LAYERS/PATCHES/FILEEDITS/REGEDITS/DLLOVERRIDES/VARS/PERSISTS/ENV/ENV_REMOVE)");
    //ENV / ENV_REMOVE are node sections (the environment folds along the chain); ParseNode reads them, the
    //lowerer only refuses the wrong shape so a typo is never a value that silently fails to apply.
    if (J.contains("ENV"))
    {
        if (!J["ENV"].is_object()) return Fail("ENV must be an object (name -> value)");
        for (const auto &[K, V] : J["ENV"].items()) if (!V.is_string()) return Fail("ENV." + K + " must be a string (quote the number)");
    }
    if (J.contains("ENV_REMOVE"))
    {
        if (!J["ENV_REMOVE"].is_array()) return Fail("ENV_REMOVE must be a list of names");
        for (const auto &V : J["ENV_REMOVE"]) if (!V.is_string() || V.get<std::string>().empty()) return Fail("ENV_REMOVE entries are names (strings)");
    }

    //An EMPTY payload array is a node that says it contributes something and contributes nothing — refused, like
    //an unknown key. A node with NO payload arrays at all is fine: it is just a node (composition, a tile, an
    //entrypoint).
    auto ArrayOf = [&](const char *Key, const ordered_json *&Arr) -> bool {
        Arr = nullptr;
        if (!J.contains(Key)) return true;
        if (!J[Key].is_array()) { Fail(std::string(Key) + " must be a list, not " + std::string(J[Key].type_name())); return false; }
        if (J[Key].empty())     { Fail(std::string(Key) + " is empty (a node with an empty payload contributes nothing — remove the key or add an entry)"); return false; }
        Arr = &J[Key];
        return true;
    };
    const ordered_json *Arr = nullptr;

    //LAYERS — VFS content. FORM selects how PATH is interpreted; the executor distinguishes the four by layer
    //TYPE, so they re-expand here. Order = mount/precedence order.
    if (!ArrayOf("LAYERS", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &Ly : *Arr)
        {
            if (!Ly.is_object()) return Fail("LAYERS entry is not an object");
            if (Ly.contains("WHEN") && !Ly["WHEN"].is_string())
                return Fail("LAYERS entry WHEN must be a string (a condition), not " + std::string(Ly["WHEN"].type_name()));
            const std::string Form = Ly.value("FORM", std::string());
            const char *VfsType = VfsTypeForForm(Form);
            if (!VfsType) return Fail("LAYERS entry has unknown FORM '" + Form + "'");
            ordered_json L = {{"TYPE", VfsType}};
            CopyIf(Ly, L, {"PATH", "TARGET", "SOURCE", "SUBMOUNTS", "COMMENT", "WHEN"});   // node WHEN ANDed at the tail
            //BASE_TARGETS is a delta's byte-base(s): ALWAYS a list, because the base may be the CONCATENATION of
            //several composed views. "" is a real target (the mount root) a lone string could not tell apart
            //from "no base declared".
            if (Ly.contains("BASE_TARGET"))
                return Fail("BASE_TARGET does not exist — a delta's base(s) are BASE_TARGETS, always a list");
            if (Ly.contains("BASE_TARGETS") && Form != "delta")
                return Fail("BASE_TARGETS is a delta's byte-base — it means nothing on FORM \"" + Form + "\"");
            if (Ly.contains("BASE_TARGETS"))
            {
                const auto &B = Ly["BASE_TARGETS"];
                if (!B.is_array()) return Fail("BASE_TARGETS must be an array of mount targets, not " + std::string(B.type_name()));
                if (B.empty()) return Fail("BASE_TARGETS is an empty array (omit it to base the delta on its own TARGET)");
                for (const auto &E : B) if (!E.is_string()) return Fail("BASE_TARGETS entries must be strings");
                L["BASE_TARGETS"] = B;
            }
            Out.push_back(std::move(L));
        }

    //PATCHES — one entry per FILE: {FILE, EDITS:[{MODE, OFFSET|ANCHOR, EXPECT, REPLACE|VALUE|PAYLOAD, …}]}.
    if (!ArrayOf("PATCHES", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &P : *Arr)
        {
            if (!P.is_object()) return Fail("PATCHES entry is not an object");
            if (!P.contains("FILE") || !P["FILE"].is_string()) return Fail("PATCHES entry has no FILE");
            if (!P.contains("EDITS") || !P["EDITS"].is_array() || P["EDITS"].empty()) return Fail("PATCHES entry has no EDITS");
            for (const auto &E : P["EDITS"])
            {
                if (!E.is_object()) return Fail("PATCHES EDITS entry is not an object");
                ordered_json L = E;
                L["TYPE"] = "BinaryPatch";
                L["FILE"] = P["FILE"];
                Out.push_back(std::move(L));
            }
        }

    //FILEEDITS — one entry per FILE: {FILE, EDITS:[{MODE, …}], OVERRIDE?}.
    if (!ArrayOf("FILEEDITS", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &F : *Arr)
        {
            if (!F.is_object()) return Fail("FILEEDITS entry is not an object");
            if (!F.contains("FILE") || !F["FILE"].is_string()) return Fail("FILEEDITS entry has no FILE");
            if (!F.contains("EDITS") || !F["EDITS"].is_array() || F["EDITS"].empty()) return Fail("FILEEDITS entry has no EDITS");
            if (F.contains("OVERRIDE") && !F["OVERRIDE"].is_boolean()) return Fail("FILEEDITS OVERRIDE must be a boolean");
            for (const auto &E : F["EDITS"])
            {
                if (!E.is_object()) return Fail("FILEEDITS EDITS entry is not an object");
                ordered_json L = E;
                L["TYPE"] = "FileEdit";
                L["FILE"] = F["FILE"];
                if (F.value("OVERRIDE", false)) L["OVERRIDE"] = true;
                Out.push_back(std::move(L));
            }
        }

    //REGEDITS — each entry a hive tree ({HKLM:{…}, ARCHITECTURE?, OVERRIDE?, WHEN?}); one RegEdit layer per key
    //path that carries values, per architecture view.
    if (!ArrayOf("REGEDITS", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &E : *Arr)
        {
            if (!E.is_object()) return Fail("REGEDITS entry is not an object");
            const bool Override = E.value("OVERRIDE", false);
            const std::string EntryWhen = E.value("WHEN", std::string());
            std::vector<ordered_json> Arches;
            if (E.contains("ARCHITECTURE") && E["ARCHITECTURE"].is_array())
                for (const auto &A : E["ARCHITECTURE"]) Arches.push_back(A);
            //A STRING here ("ARCHITECTURE": "32") is an authoring mistake the schema does not allow, and it
            //used to vanish twice over. Refuse it.
            else if (E.contains("ARCHITECTURE") && !E["ARCHITECTURE"].is_null())
                return Fail("REGEDITS ARCHITECTURE must be an ARRAY of \"32\"/\"64\"");
            if (Arches.empty()) Arches.push_back(ordered_json(nullptr));
            for (const ordered_json &Arch : Arches)
                for (const auto &[Hive, Tree] : E.items())
                {
                    if (Hive == "ARCHITECTURE" || Hive == "OVERRIDE" || Hive == "WHEN" || Hive == "COMMENT") continue;
                    if (!Tree.is_object()) return Fail("REGEDITS hive '" + Hive + "' is not an object");
                    EmitRegTree(Hive, Tree, Arch, Override, EntryWhen, Out);
                }
        }

    //DLLOVERRIDES — a {dll: order} map; one DllOverride layer per entry.
    if (J.contains("DLLOVERRIDES"))
    {
        if (!J["DLLOVERRIDES"].is_object()) return Fail("DLLOVERRIDES must be a {dll: order} object");
        if (J["DLLOVERRIDES"].empty()) return Fail("DLLOVERRIDES is empty (remove the key or add an override)");
        for (const auto &[Dll, Order] : J["DLLOVERRIDES"].items())
        {
            if (!Order.is_string()) return Fail("DLLOVERRIDES order for '" + Dll + "' is not a string");
            std::string Spec = Dll; Spec += '='; Spec += Order.get<std::string>();
            Out.push_back({{"TYPE", "DllOverride"}, {"DLLOVERRIDE", Spec}});
        }
    }

    //VARS — resolution is a global KEY namespace (override by closure order), so N vars in one node resolve
    //identically to N one-var nodes; each entry lowers to one CustomVar layer.
    if (!ArrayOf("VARS", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &V : *Arr)
        {
            if (!V.is_object()) return Fail("VARS entry is not an object");
            if (V.contains("WHEN") && !V["WHEN"].is_string())
                return Fail("VARS entry WHEN must be a string (a condition), not " + std::string(V["WHEN"].type_name()));
            if (!V.contains("KEY") || !V["KEY"].is_string() || V["KEY"].get<std::string>().empty())
                return Fail("VARS entry has no KEY");
            ordered_json L = {{"TYPE", "CustomVar"}};
            CopyIf(V, L, {"KEY", "DEFAULT", "COMMENT", "UI", "WHEN"});         // node WHEN ANDed at the tail
            Out.push_back(std::move(L));
        }

    //PERSISTS — SCOPE=file|registry (default file); PATH is the runtime source; TARGET the durable subdir; CLOUD
    //the future Cloud-Saves flag. Each entry lowers to one DeclarePersist layer.
    if (!ArrayOf("PERSISTS", Arr)) return ordered_json::array();
    if (Arr)
        for (const auto &Pe : *Arr)
        {
            if (!Pe.is_object()) return Fail("PERSISTS entry is not an object");
            if (Pe.contains("WHEN") && !Pe["WHEN"].is_string())
                return Fail("PERSISTS entry WHEN must be a string (a condition), not " + std::string(Pe["WHEN"].type_name()));
            nlohmann::ordered_json P = {{"TYPE", "DeclarePersist"}};
            if (Pe.contains("SCOPE"))  { if (!Pe["SCOPE"].is_string())  return Fail("SCOPE must be a string (file|registry)"); P["SCOPE"]  = Pe["SCOPE"]; }
            if (Pe.contains("PATH"))   { if (!Pe["PATH"].is_string())   return Fail("PATH must be a string");                  P["PATH"]   = Pe["PATH"]; }
            if (Pe.contains("TARGET")) { if (!Pe["TARGET"].is_string()) return Fail("TARGET must be a string");                P["TARGET"] = Pe["TARGET"]; }
            if (Pe.contains("CLOUD"))  { if (!Pe["CLOUD"].is_boolean()) return Fail("CLOUD must be a boolean");                P["CLOUD"]  = Pe["CLOUD"]; }
            CopyIf(Pe, P, {"WHEN"});                                       // node WHEN ANDed at the tail
            Out.push_back(std::move(P));
        }

    //The node's WHEN gates every layer it produced, applied in ONE place. Where a layer carries its OWN
    //condition BOTH must hold, so they are ANDed. No copier may also copy WHEN, or the node's own text doubles
    //back on it.
    if (HasWhen)
    {
        std::string NodeWhen = J.value("WHEN", std::string());
        {
            const size_t NB = NodeWhen.find_first_not_of(" \t\r\n");
            NodeWhen = (NB == std::string::npos) ? std::string()
                     : NodeWhen.substr(NB, NodeWhen.find_last_not_of(" \t\r\n") - NB + 1);
        }
        for (auto &L : Out)
        {
            if (!L.is_object()) continue;
            std::string Own = L.value("WHEN", std::string());
            const size_t B = Own.find_first_not_of(" \t\r\n");
            Own = (B == std::string::npos) ? std::string() : Own.substr(B, Own.find_last_not_of(" \t\r\n") - B + 1);
            if (NodeWhen.empty()) { if (Own.empty()) L.erase("WHEN"); continue; }
            L["WHEN"] = Own.empty() ? NodeWhen : ("(" + NodeWhen + ") && (" + Own + ")");
        }
    }
    return Out;
}

//THE entry point, and the only place that may throw-check the payload.
//
//A node file is UNTRUSTED INPUT: it arrives from a peer over IPFS, or from an author's typo. nlohmann's
//value()/get() throw type_error on a mismatch ("TYPE": 5, "OVERRIDE": "true"), and nothing above this catches
//it — BuildNodeIndex runs at startup, so one such node in a fetched package source aborted the process before
//the GUI existed, on every launch, with no way to remove the source from inside the app. A remote, persistent
//denial of service.
//
//Guarded HERE rather than field by field: hardening the seven reads that throw today leaves the eighth one
//added tomorrow unguarded, and the comment on the DllOverride check already claimed the property that the
//other reads did not have. One choke point makes it true for the whole function by construction.
//The TYPE every key the engine reads is required to have. Lowering copies payload fields VERBATIM out of
//untrusted JSON (a node file arrives from a peer, or from an author's typo), and every consumer downstream —
//ParseNode's identity derivation, LayerLocator, ValidateNodeGraph, the executor — then reads them with
//nlohmann's .value(), which THROWS on a mismatch. Those reads happen outside any guard and BuildNodeIndex runs
//at STARTUP, inside MainWindow's constructor: one such node in a fetched package source and no window ever
//appears, so the source cannot be removed from inside the app.
//
//Guarding each reader is the losing move — there are a dozen and a half, and the next one added is unguarded
//again. The property that holds the line is: WHAT LOWERING EMITS IS WELL-TYPED. Checked once, here.
//
//STRUCTURAL, not flat: a first version checked only top-level keys, so `"SOURCE": {"CID": 5}` sailed through
//and LayerLocator aborted the app exactly as before. It recurses into the NAMED structural sub-payloads and
//checks the ELEMENT type of the string arrays.
//
//It deliberately does NOT blanket-recurse. KEYVALUES, ENV and a tile's hoisted META are AUTHOR-KEYED BAGS —
//a registry value legitimately named "PATH" or "TYPE" would be rejected by a generic walk, which is the same
//wrong-rule mistake that COVER (dual-form: a filename OR an object) already caught against the live library.
namespace TypeCheck {

enum Kind { Str, Bool, Num, Arr, Obj, StrArr };

const std::map<std::string, Kind> &Table()
{
    static const std::map<std::string, Kind> T = {
        {"TYPE",Str},{"PATH",Str},{"TARGET",Str},{"REGPATH",Str},{"DLLOVERRIDE",Str},
        {"FILE",Str},{"MODE",Str},{"OFFSET",Str},{"EXPECT",Str},{"REPLACE",Str},{"VALUE",Str},{"PAYLOAD",Str},
        {"CAVE",Str},{"ANCHOR",Str},{"APPLY",Str},{"KEY",Str},{"DEFAULT",Str},{"COMMENT",Str},{"WHEN",Str},
        {"PLATFORM",Str},{"HOST",Str},{"EXECUTABLE",Str},{"CONTENTPATH",Str},{"WORKDIR",Str},{"LABEL",Str},
        {"RUNNER",Str},{"UID",Str},{"TITLE",Str},{"CONTENT_ROOT",Str},{"ARCHITECTURE",Str},
        {"CID",Str},{"CONTROL",Str},{"GROUP",Str},{"SCOPE",Str},
        {"MIN",Num},{"MAX",Num},{"SIZE",Num},
        {"OVERRIDE",Bool},{"RECOMMENDED",Bool},{"PREFIX_GENERATE",Bool},{"UNIFIED_RUNTIME",Bool},{"CLOUD",Bool},
        {"GUEST",StrArr},{"EXEARGS",StrArr},{"ARGS",StrArr},{"REMOVE_ENV",StrArr},{"SUBMOUNTS",StrArr},
        {"BASE_TARGETS",StrArr},{"CHOICES",Arr},
        {"SOURCE",Obj},{"ENV",Obj},{"KEYVALUES",Obj},{"UI",Obj},
    };
    return T;
}

//The sub-objects worth descending into BY KEY: their keys are schema, so the table applies inside them.
bool Structural(const std::string &Key)
{
    return Key == "SOURCE" || Key == "COVER" || Key == "UI" || Key == "CHOICES";
}

//The AUTHOR-KEYED BAGS. Their KEYS are arbitrary (a registry value may legitimately be named "PATH", an env
//var "TYPE"), so the table cannot apply — but their VALUES are all read as strings, so the rule for them is
//a value SHAPE rule instead. Without this the whitelist was blind by construction: a tile's META is hoisted
//FLAT onto the layer, so `"UMUID": 12345` landed at top level where an unlisted key is simply skipped, and
//the launch aborted reading it. Adding UMUID to the table would have closed exactly one of five and
//guaranteed a ninth round of this.
bool Bag(const std::string &Key)
{
    return Key == "KEYVALUES" || Key == "ENV" || Key == "OVERRIDES";
}

//Returns "" when fine, else the offending key.
//`TopLevel` is true for a layer itself and false inside a schema sub-object. It matters for exactly one rule:
//a tile's META bag is hoisted FLAT onto the layer, so an unlisted key there is AUTHOR DATA read as a string —
//while an unlisted key inside SOURCE/UI/COVER is simply a key this table does not know yet, and constraining
//it would be the COVER mistake again.
std::string Bad(const ordered_json &Node, bool TopLevel = true)
{
    if (!Node.is_object()) return {};
    for (const auto &[K, V] : Node.items())
    {
        auto It = Table().find(K);
        if (It != Table().end())
        {
            bool Ok = true;
            switch (It->second)
            {
                case Str:    Ok = V.is_string();  break;
                case Bool:   Ok = V.is_boolean(); break;
                case Num:    Ok = V.is_number();  break;
                case Arr:    Ok = V.is_array();   break;
                case Obj:    Ok = V.is_object();  break;
                case StrArr: Ok = V.is_array();
                             if (Ok) for (const auto &E : V) if (!E.is_string()) { Ok = false; break; }
                             break;
            }
            if (!Ok) return K;
        }
        if (Bag(K) && V.is_object())
        {
            for (const auto &[BK, BV] : V.items())
                if (!BV.is_string() && !BV.is_null()) return K + "." + BK;
            continue;
        }
        if (Structural(K))
        {
            if (V.is_object()) { const std::string B = Bad(V, false); if (!B.empty()) return B; }
            else if (V.is_array())
                for (const auto &E : V)
                {
                    //A CHOICES element may be a bare string shorthand OR an object; only the object form has
                    //fields to check, and calling value() on the string form is what threw in the picker.
                    if (!E.is_object() && !E.is_string()) return K;
                    const std::string B = Bad(E, false);
                    if (!B.empty()) return B;
                }
            continue;
        }
        //An UNLISTED key AT TOP LEVEL is a hoisted META field (see above): author data that every reader takes
        //as a string. Inside a schema sub-object an unlisted key is just one this table has not learned yet.
        if (TopLevel && !V.is_string() && !V.is_null() && !V.is_boolean() && !V.is_array() && !V.is_object())
            return K;
    }
    return {};
}

} // namespace TypeCheck

ordered_json Lower(const ordered_json &J, const std::string &NodeId, std::string &Err)
{
    try
    {
        ordered_json Out = LowerOrThrow(J, NodeId, Err);
        if (!Err.empty()) return Out;
        //The emitted layers are the engine's input from here on, so they must be well-typed BEFORE anything
        //reads them — see TypeOk. A mismatch is a refusal naming the field, not a crash three frames later.
        for (const auto &L : Out)
        {
            const std::string Bad = TypeCheck::Bad(L);
            if (!Bad.empty())
            {
                Err = "node '" + NodeId + "': field " + Bad + " has the wrong type";
                return ordered_json::array();
            }
        }
        return Out;
    }
    catch (const std::exception &E)
    {
        Err = "node '" + NodeId + "': malformed payload (" + E.what() + ")";
        return ordered_json::array();
    }
}

ordered_json LowerEntrypoint(const ordered_json &Entry, const std::string &NodeId, std::string &Err)
{
    try
    {
        ordered_json Out = LowerEntrypointOrThrow(Entry, NodeId, Err);
        if (!Err.empty()) return ordered_json();
        const std::string Bad = TypeCheck::Bad(Out);
        if (!Bad.empty()) { Err = "node '" + NodeId + "': ENTRYPOINTS field " + Bad + " has the wrong type"; return ordered_json(); }
        return Out;
    }
    catch (const std::exception &E)
    {
        Err = "node '" + NodeId + "': malformed ENTRYPOINTS entry (" + E.what() + ")";
        return ordered_json();
    }
}

} // namespace NodeLower
