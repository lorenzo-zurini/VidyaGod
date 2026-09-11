#include "nodelower.h"

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

//The real work. Every read below assumes a well-typed payload; Lower() converts any type mismatch into a
//refusal, so this is free to be written for the shape the format defines rather than defensively field by field.
static ordered_json LowerOrThrow(const ordered_json &J, const std::string &NodeId, std::string &Err)
{
    Err.clear();
    ordered_json Out = ordered_json::array();
    const std::string T = J.value("TYPE", std::string());
    //A node-level WHEN gates the whole contribution. It used to be carried only by the three types whose
    //CopyIf list happened to mention it, so on the other six a conditional layer applied unconditionally —
    //inert-looking in the file, live at launch. Applied to every emitted layer at the end instead.
    const bool HasWhen = J.contains("WHEN");     // type already enforced above, so presence is enough
    auto Fail = [&](const std::string &Why) { Err = "node '" + NodeId + "': " + Why; return ordered_json::array(); };
    //A non-string WHEN was silently IGNORED: HasWhen was `contains && is_string`, so the layer applied
    //UNCONDITIONALLY — the exact "inert-looking in the file, live at launch" failure this mechanism exists to
    //prevent, and `"WHEN": true` is a plausible slip because the field reads like a boolean.
    //
    //The generic TypeOk sweep would refuse it too; this is here for the DIAGNOSTIC, which names the field and
    //what it should be instead of "malformed payload (type must be string, but is boolean)".
    if (J.contains("WHEN") && !J["WHEN"].is_string())
        return Fail("WHEN must be a string (a condition), not " + std::string(J["WHEN"].type_name()));

    if (T == "Content")
    {
        const std::string Form = J.value("FORM", std::string());
        const char *VfsType = VfsTypeForForm(Form);
        if (!VfsType) return Fail("Content has unknown FORM '" + Form + "'");
        ordered_json L = {{"TYPE", VfsType}};
        CopyIf(J, L, {"PATH", "TARGET", "SOURCE", "SUBMOUNTS", "COMMENT"});   // WHEN: applied at the tail
        //BASE_TARGET is an array (a delta may dedup against a CONCATENATION of bases). vidyagodfs carries
        //the plural as LayerSpec::baseTargets; the single-entry form is a plain cross-target delta.
        if (J.contains("BASE_TARGET"))
        {
            const auto &B = J["BASE_TARGET"];
            if (B.is_string())                              L["BASE_TARGET"]  = B;
            else if (B.is_array() && B.size() == 1)         L["BASE_TARGET"]  = B[0];
            else if (B.is_array() && B.size() > 1)          L["BASE_TARGETS"] = B;
            //An EMPTY array fell through every branch and silently dropped the base — a delta with nothing to
            //reconstruct against, reported by nobody. Say it instead of omitting the key.
            else if (B.is_array())                          return Fail("BASE_TARGET is an empty array (omit it "
                                                                        "to base the delta on its own TARGET)");
            else                                            return Fail("BASE_TARGET must be a string or array");
        }
        Out.push_back(std::move(L));
    }
    else if (T == "RegEdit")
    {
        if (!J.contains("EDITS") || !J["EDITS"].is_array()) return Fail("RegEdit has no EDITS array");
        for (const auto &E : J["EDITS"])
        {
            if (!E.is_object()) return Fail("RegEdit EDITS entry is not an object");
            const bool Override = E.value("OVERRIDE", false);
            //An entry's own WHEN gates THAT entry (the node's WHEN still gates everything — see the tail of
            //this function). It used to be skipped as a non-hive key and then never emitted anywhere, so a
            //conditional registry write applied unconditionally: "%NETMODE% == host" and its join-mode
            //counterpart both fired on every launch and the second overwrote the first. Inert-looking in the
            //file, live at launch — the exact failure this lowering exists to prevent.
            const std::string EntryWhen = E.value("WHEN", std::string());
            //ARCHITECTURE is arrayable: the same key tree written into several registry views. One
            //layer per view, since the executor's RegEdit applies to exactly one.
            std::vector<ordered_json> Arches;
            if (E.contains("ARCHITECTURE") && E["ARCHITECTURE"].is_array())
                for (const auto &A : E["ARCHITECTURE"]) Arches.push_back(A);
            //A STRING here ("ARCHITECTURE": "32") is an authoring mistake the schema does not allow, and it
            //used to vanish twice over — not an array, so no view was selected, and skipped as a hive name, so
            //nothing complained. The edit then landed in the un-redirected view. Refuse it.
            else if (E.contains("ARCHITECTURE") && !E["ARCHITECTURE"].is_null())
                return Fail("RegEdit ARCHITECTURE must be an ARRAY of \"32\"/\"64\"");
            if (Arches.empty()) Arches.push_back(ordered_json(nullptr));
            for (const ordered_json &Arch : Arches)
                for (const auto &[Hive, Tree] : E.items())
                {
                    //COMMENT is a legal, heavily-used field on FileEdit/BinaryPatch EDITS entries, so reaching
                    //for it on a registry entry is natural — and it was a hard refusal ("hive 'COMMENT' is not
                    //an object") rather than the note the author meant.
                    if (Hive == "ARCHITECTURE" || Hive == "OVERRIDE" || Hive == "WHEN" || Hive == "COMMENT")
                        continue;
                    if (!Tree.is_object()) return Fail("RegEdit hive '" + Hive + "' is not an object");
                    EmitRegTree(Hive, Tree, Arch, Override, EntryWhen, Out);
                }
        }
    }
    else if (T == "BinaryPatch" || T == "FileEdit")
    {
        if (!J.contains("EDITS") || !J["EDITS"].is_array()) return Fail(T + " has no EDITS array");
        for (const auto &E : J["EDITS"])
        {
            if (!E.is_object()) return Fail(T + " EDITS entry is not an object");
            ordered_json L = E;
            L["TYPE"] = T;
            if (J.contains("FILE")) L["FILE"] = J["FILE"];
            if (T == "FileEdit" && J.value("OVERRIDE", false)) L["OVERRIDE"] = true;
            Out.push_back(std::move(L));
        }
    }
    else if (T == "DllOverride")
    {
        if (!J.contains("OVERRIDES") || !J["OVERRIDES"].is_object()) return Fail("DllOverride has no OVERRIDES object");
        for (const auto &[Dll, Order] : J["OVERRIDES"].items())
        {
            //Package JSON arrives from peers: a non-string here must be a refused node, not an exception
            //thrown out of BuildNodeIndex at startup.
            if (!Order.is_string()) return Fail("DllOverride order for '" + Dll + "' is not a string");
            std::string Spec = Dll;
            Spec += '=';
            Spec += Order.get<std::string>();
            Out.push_back({{"TYPE", "DllOverride"}, {"DLLOVERRIDE", Spec}});
        }
    }
    else if (T == "Persist")
    {
        //REFUSES, where every other branch already did. Persist was the one type that silently skipped a
        //malformed payload — and the shape it skipped is the PRE-FLAT SPELLING, `"KEEP": "path"` as a bare
        //string, which is exactly what an author hand-writing a flat node reaches for. A Persist node that
        //emits zero layers is legitimate (`"DROP": []` ships on every runner), so nothing downstream could
        //tell "keeps nothing on purpose" from "keeps nothing because I could not read it" — the same silent
        //save loss, arrived at from the authoring side instead of the engine side.
        for (const char *Which : {"KEEP", "DROP"})
        {
            if (!J.contains(Which)) continue;
            if (!J[Which].is_array())
                return Fail(std::string(Which) + " must be an ARRAY of targets, not "
                            + J[Which].type_name() + " (a single target is a one-element array)");
            for (const auto &P : J[Which])
            {
                if (!P.is_string()) return Fail(std::string(Which) + " entries must be strings");
                if (P.get<std::string>().empty()) return Fail(std::string(Which) + " has an empty target");
                Out.push_back({{"TYPE", "Persist"}, {Which, P}});
            }
        }
    }
    else if (T == "CustomVar")
    {
        ordered_json L = {{"TYPE", "CustomVar"}};
        CopyIf(J, L, {"KEY", "DEFAULT", "COMMENT", "UI"});                    // WHEN: applied at the tail
        Out.push_back(std::move(L));
    }
    else if (T == "DeclareLibraryItem")
    {
        ordered_json L = {{"TYPE", "DeclareLibraryItem"}};
        CopyIf(J, L, {"UID", "TITLE", "COVER"});
        //Descriptive catalog fields live in an opaque META bag on disk (the engine names only a handful of
        //them); the tile consumers read them flat, so they re-join the layer here.
        if (J.contains("META") && J["META"].is_object())
            for (const auto &[K, V] : J["META"].items()) L[K] = V;
        Out.push_back(std::move(L));
    }
    else if (T == "DeclareExec")
    {
        //Exec and runner are ONE declaration: HOST is the platform I need, GUEST the platforms I provide.
        //No GUEST ⇒ nothing runs inside me ⇒ I am the terminal link, i.e. a launchable.
        const bool IsRunner = J.contains("GUEST") && J["GUEST"].is_array() && !J["GUEST"].empty();
        ordered_json L = {{"TYPE", IsRunner ? "DeclareRunner" : "DeclareExec"}};
        if (IsRunner)
        {
            L["HOST"]  = J.value("HOST", std::string());
            L["GUEST"] = J["GUEST"];
            L["EXECUTABLE"] = J.value("PATH", std::string());
            L["ARGS"] = J.contains("ARGS") ? J["ARGS"] : ordered_json::array();
            L["ENV"]  = J.contains("ENV")  ? J["ENV"]  : ordered_json::object();
            L["REMOVE_ENV"] = J.contains("ENV_REMOVE") ? J["ENV_REMOVE"] : ordered_json::array();
            CopyIf(J, L, {"CONTENT_ROOT", "PREFIX_GENERATE", "UNIFIED_RUNTIME"});  // WHEN: applied at the tail
        }
        else
        {
            L["PLATFORM"] = J.value("HOST", std::string());
            if (J.contains("PATH")) L["CONTENTPATH"] = J["PATH"];
            if (J.contains("ARGS")) L["EXEARGS"]     = J["ARGS"];
            CopyIf(J, L, {"LABEL", "RECOMMENDED", "WORKDIR", "RUNNER"});           // WHEN: applied at the tail
        }
        Out.push_back(std::move(L));
    }
    else if (T == "Group")
    {
        //Pure composition: no payload, exists only to carry PARENTS (and its TOGGLE/EXCLUDE). "This module IS
        //these parents" — e.g. nfsu2_asiloader = [nfsu2_nocd, asiloader], referenced by three other nodes.
        //Every other TYPE contributes a layer, so without this the node would vanish and its referrers would
        //silently lose the edge — which is how three NFS games lost their ASI loader in the first migration.
    }
    else if (T.empty()) return Fail("no TYPE (a payload-less composition node must say TYPE \"Group\")");
    else                return Fail("unknown TYPE '" + T + "'");

    //The node's WHEN gates every layer it produced, applied in ONE place rather than by each type's copier —
    //it used to be carried only by the types whose CopyIf list happened to mention it, so on the others a
    //conditional node applied unconditionally. Where a layer carries its OWN condition (a RegEdit entry's, a
    //FileEdit/BinaryPatch edit's) BOTH must hold, so they are ANDed: taking either alone silently widens or
    //narrows what the author wrote. No copier may also copy WHEN, or the node's own text doubles back on it.
    if (HasWhen)
    {
        //TRIMMED, like the layer's own below: a whitespace-only node condition composed to "(   ) && (B)",
        //which the parser rejects — and an unparseable WHEN fails OPEN, silently discarding B as well.
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
            //TRIMMED before the emptiness test: a whitespace-only condition composed to "(A) && (  )", which
            //the parser rejects — and an unparseable WHEN FAILS OPEN, so the node's real condition silently
            //evaporated and the layer applied always.
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
        {"TYPE",Str},{"PATH",Str},{"TARGET",Str},{"BASE_TARGET",Str},{"REGPATH",Str},{"DLLOVERRIDE",Str},
        {"FILE",Str},{"MODE",Str},{"OFFSET",Str},{"EXPECT",Str},{"REPLACE",Str},{"VALUE",Str},{"PAYLOAD",Str},
        {"CAVE",Str},{"ANCHOR",Str},{"APPLY",Str},{"KEY",Str},{"DEFAULT",Str},{"COMMENT",Str},{"WHEN",Str},
        {"PLATFORM",Str},{"HOST",Str},{"EXECUTABLE",Str},{"CONTENTPATH",Str},{"WORKDIR",Str},{"LABEL",Str},
        {"RUNNER",Str},{"UID",Str},{"TITLE",Str},{"CONTENT_ROOT",Str},{"ARCHITECTURE",Str},{"KEEP",Str},
        {"DROP",Str},{"CID",Str},{"CONTROL",Str},{"GROUP",Str},
        {"MIN",Num},{"MAX",Num},
        {"OVERRIDE",Bool},{"RECOMMENDED",Bool},{"PREFIX_GENERATE",Bool},{"UNIFIED_RUNTIME",Bool},
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

} // namespace NodeLower
