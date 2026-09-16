#include "pkggraph.h"

#include "commonutils.h"   // Log

#include "pkglayout.h"   // PkgLayout::ComputeUnplaced — the shared auto-layout

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

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

Graph Build(const json &NodesArray, const json *Layout)
{
    Graph G;
    if (!NodesArray.is_array()) return G;

    std::map<std::string, int> ById;
    for (const auto &N : NodesArray)
    {
        //NOT skipped: G.Nodes[i] MUST correspond to NodesArray[i]. The link pass indexes NodesArray by
        //G.Nodes position, and the canvas writes a node's position back at that index — so one non-object in
        //the array would shift every wire and every saved position by one. A malformed entry becomes an empty
        //placeholder node instead, which renders as an obviously-broken box rather than silently skewing the
        //graph. (LoadNodes does not produce these; a hand-edited file can.)
        Node Nd;
        Nd.Index = (int)G.Nodes.size();
        if (!N.is_object()) { Nd.Height = EstimateHeight(N); G.Nodes.push_back(std::move(Nd)); continue; }
        //TOTAL: the editor renders raw on-disk JSON, deliberately — it is the tool you open to fix a node the
        //format rejects — so any of these may be the wrong type, and value() throws on a mismatch.
        auto Str = [&](const char *K, const char *Def) {
            return (N.contains(K) && N[K].is_string()) ? N[K].get<std::string>() : std::string(Def);
        };
        Nd.Id    = Str("NODE_ID", "");
        Nd.Type  = Str("TYPE", "Group");
        Nd.Form  = Str("FORM", "");
        //Position resolves in three steps, weakest first: the node's own POS (the author's published default),
        //then the caller's Layout override (this machine's own drags, held in GlobalConfig), then — for
        //whatever is still unplaced — the computed layout below. A node carrying POS therefore opens where the
        //author put it, and moving it never writes to the package.
        auto ReadPos = [&](const json &P, const char *Which) {
            if (!P.is_array() || P.size() != 2 || !P[0].is_number() || !P[1].is_number())
            {
                //Recorded like the out-of-range shape below. "POS": ["10","20"] or a three-element POS is just
                //as much a declaration that will be silently replaced at publish, and the comment below
                //promises Build hands back what it refused — it has to mean every refusal, not one of them.
                //NOT P.dump().substr(): dump() serialises the WHOLE value first, and a package fetched from a
                //content source can carry a megabytes-long array here — allocated on every cache rebuild,
                //which is every keystroke. Describe the shape instead; the shape is the whole complaint.
                G.RejectedPositions.push_back(
                    {Nd.Index, Nd.Id, Which, std::string("a ") + P.type_name()
                                       + (P.is_array() ? " of " + std::to_string(P.size()) + " element(s)" : "")
                                       + ", not two numbers"});
                return false;
            }
            const double Px = P[0].get<double>(), Py = P[1].get<double>();
            //A coordinate no layout could have produced is not a position, it is corruption — and accepting it
            //here is what made it permanent. The canvas cannot draw a node at 5e9 (it fails every viewport
            //test), cannot select it, and cannot drag it back; the read-back downstream then refused to write
            //the value it was handed, re-seeded from this same number, and refused it again every frame.
            //Rejected at the door instead, which leaves HasPos false so PkgLayout places the node somewhere
            //the author can actually see and fix it. The bound is absurd rather than tight: real graphs are
            //tens of thousands of units across, and `get<float>()` on 1e300 quietly yields inf.
            if (!std::isfinite(Px) || !std::isfinite(Py) || std::abs(Px) > 1.0e7 || std::abs(Py) > 1.0e7)
            {
                //RECORDED, not logged. Dropping a declared position silently means the author's layout
                //disappears, publish re-stamps a computed one over it and the package's Meta-CID changes with
                //"stamped POS into N file(s)" as the only trace — so it has to be reported. But Build is
                //called on every cache rebuild, which is every KEYSTROKE, and logging here made one corrupt
                //node emit a 772-byte warning per character typed anywhere in the package (500 such nodes:
                //386 KB of stderr and 500 Warnings on the diagnostics tally, per keystroke). Build stays pure
                //and hands the facts back; the canvas, which already keeps a warned-about set, says it once.
                //
                //%g, not std::to_string: the latter prints 1e300 as 308 digits of peer-controlled text.
                char Buf[64];
                std::snprintf(Buf, sizeof(Buf), "%g, %g", Px, Py);
                G.RejectedPositions.push_back({Nd.Index, Nd.Id, Which, Buf});
                return false;
            }
            Nd.X = (float)Px; Nd.Y = (float)Py; Nd.HasPos = true;
            return true;
        };
        if (N.contains("POS")) ReadPos(N["POS"], "its own POS");
        if (Layout && Layout->is_object() && !Nd.Id.empty() && Layout->contains(Nd.Id))
            ReadPos((*Layout)[Nd.Id], "this machine's saved layout");
        Nd.Height = EstimateHeight(N);
        if (!Nd.Id.empty()) ById[Nd.Id] = Nd.Index;
        G.Nodes.push_back(std::move(Nd));
    }

    std::set<std::string> SeenExternal;
    for (int I = 0; I < (int)G.Nodes.size(); ++I)
    {
        const json &N = NodesArray[I];
        if (!N.contains("PARENTS") || !N["PARENTS"].is_array()) continue;
        int Slot = -1;
        for (const auto &P : N["PARENTS"])
        {
            ++Slot;                                  // counts EVERY entry, including the skipped ones, so the
            if (!P.is_string()) continue;            // slot always addresses the real array index
            const std::string Pid = P.get<std::string>();
            if (Pid.empty()) continue;
            Link L; L.ChildIndex = I; L.Slot = Slot;
            auto It = ById.find(Pid);
            if (It != ById.end()) L.ParentIndex = It->second;
            else
            {
                //Out-of-bundle parent (a shared library or runner) — a reference chip, not a box we own.
                L.ExternalId = Pid;
                if (SeenExternal.insert(Pid).second) G.Externals.push_back(Pid);
            }
            G.Links.push_back(std::move(L));
        }
    }

    //Anything still unplaced gets the computed layout. It lives in PkgLayout because the same function has
    //to run at publish time to STAMP POS into the nodes — one algorithm, so what an author sees on the canvas
    //is exactly what a peer opening the published package sees.
    PkgLayout::ComputeUnplaced(G);

    return G;
}

bool SetPos(json &Layout, const std::string &NodeId, float X, float Y)
{
    if (NodeId.empty()) return false;
    if (!Layout.is_object()) Layout = json::object();
    Layout[NodeId] = json::array({X, Y});
    return true;
}

// ---- type vocabulary ------------------------------------------------------

const std::vector<std::string> &AllTypes()
{
    static const std::vector<std::string> T = {
        "Content", "RegEdit", "FileEdit", "BinaryPatch", "DllOverride",
        "DeclarePersist", "CustomVar", "DeclareExec", "DeclareLibraryItem", "Group"
    };
    return T;
}

const char *TypeHelp(const std::string &Type)
{
    if (Type == "Content")            return "Files mounted into the runtime - a zip, a directory, a single file, or a delta over one.";
    if (Type == "RegEdit")            return "Registry keys and values written into the prefix, per architecture.";
    if (Type == "FileEdit")           return "Text edits applied to a file in the runtime (whole-file, key=value, or append).";
    if (Type == "BinaryPatch")        return "Byte patches over the PRISTINE executable, guarded by an EXPECT check.";
    if (Type == "DllOverride")        return "Which DLLs resolve native vs builtin (wine's n,b notation).";
    if (Type == "DeclarePersist")     return "What survives the run: a file path or registry key is promoted to a named durable TARGET under the instance.";
    if (Type == "CustomVar")          return "A variable the player sets before launch; substituted as %KEY% wherever it is used.";
    if (Type == "DeclareExec")        return "What to run. No GUEST => this is a launchable; with GUEST => it is a runner providing those platforms.";
    if (Type == "DeclareLibraryItem") return "The library tile: title, UID and cover. Parent of the launchables it groups.";
    if (Type == "Group")              return "Pure composition - no payload, exists only to gather PARENTS under one name.";
    return "";
}

void TypeColour(const std::string &Type, int &R, int &G, int &B)
{
    // Content = blue, transforms = amber, identity = green, composition = grey.
    if (Type == "Content")                                       { R = 46;  G = 96;  B = 148; return; }
    if (Type == "RegEdit" || Type == "FileEdit" ||
        Type == "BinaryPatch" || Type == "DllOverride")          { R = 136; G = 92;  B = 36;  return; }
    if (Type == "DeclarePersist" || Type == "CustomVar")         { R = 92;  G = 68;  B = 128; return; }
    if (Type == "DeclareExec" || Type == "DeclareLibraryItem")   { R = 46;  G = 118; B = 78;  return; }
    R = 74; G = 80; B = 88;
}

json NewPayload(const std::string &Type)
{
    // Required keys present from the start, so a fresh node is never malformed — the old editor created
    // content layers with no TARGET at all, which is what made Tonic Trouble die as "create process: 2".
    if (Type == "Content")
        return json::object({{"TYPE","Content"},{"FORM","zip"},{"PATH",""},
                             {"TARGET","%PrefixRoot%/drive_c/%PackageUID%"}});
    if (Type == "RegEdit")
        return json::object({{"TYPE","RegEdit"},{"EDITS", json::array({
            json::object({{"ARCHITECTURE", json::array({"32"})}}) })}});
    if (Type == "FileEdit")
        return json::object({{"TYPE","FileEdit"},{"FILE",""},{"EDITS", json::array()}});
    if (Type == "BinaryPatch")
        return json::object({{"TYPE","BinaryPatch"},{"FILE",""},{"EDITS", json::array()}});
    if (Type == "DllOverride")
        return json::object({{"TYPE","DllOverride"},{"OVERRIDES", json::object()}});
    if (Type == "DeclarePersist")
        return json::object({{"TYPE","DeclarePersist"},{"SCOPE","file"},{"PATH",""},{"TARGET",""},{"CLOUD",true}});
    if (Type == "CustomVar")
        return json::object({{"TYPE","CustomVar"},{"KEY",""},{"DEFAULT",""}});
    if (Type == "DeclareExec")
        return json::object({{"TYPE","DeclareExec"},{"HOST","win32"},
                             {"PATH","%PrefixRoot%/drive_c/%PackageUID%/"},{"ARGS", json::array()}});
    if (Type == "DeclareLibraryItem")
        return json::object({{"TYPE","DeclareLibraryItem"},{"UID",""},{"TITLE",""}});
    return json::object({{"TYPE","Group"}});
}

// ---- payload schema -------------------------------------------------------

namespace {

const std::vector<std::pair<const char *, const char *>> ArchOpts   = {{"32","32-bit"},{"64","64-bit"}};
const std::vector<std::pair<const char *, const char *>> FormOpts   = {{"zip","zip"},{"dir","directory"},
                                                                       {"file","single file"},{"delta","delta (.vgdelta)"}};
const std::vector<std::pair<const char *, const char *>> DllOpts    = {{"n,b","native, then builtin"},{"b,n","builtin, then native"},
                                                                       {"n","native only"},{"b","builtin only"},{"d","disabled"}};
const std::vector<std::pair<const char *, const char *>> FModeOpts  = {{"ConfigWrite","key = value"},{"Overwrite","whole file"},
                                                                       {"AppendLine","append a line"}};
const std::vector<std::pair<const char *, const char *>> BModeOpts  = {{"Replace","Replace"},{"Cave","Cave"},{"Poke","Poke"}};
const std::vector<std::pair<const char *, const char *>> ApplyOpts  = {{"prefix","on disk (writelayer)"},{"memory","live process"}};
const std::vector<std::pair<const char *, const char *>> CtrlOpts   = {{"enum","dropdown"},{"int","number"},{"text","text"}};
const std::vector<std::pair<const char *, const char *>> ScopeOpts  = {{"file","file path"},{"registry","registry key"}};

std::vector<Field> MakeFields(const std::string &Type)
{
    if (Type == "Content")
        return {
            {"FORM",        "Form",        FieldKind::Enum,       "",  FormOpts, {}},
            {"PATH",        "Path",        FieldKind::Text,       "file in this bundle", {}, {}},
            {"TARGET",      "Target",      FieldKind::Text,       "%PrefixRoot%/drive_c/%PackageUID%", {}, {}},
            {"SUBMOUNTS",   "Submounts",   FieldKind::StringList, "source/path:dest/path", {}, {}},
            {"BASE_TARGETS", "Base targets", FieldKind::StringListKeepEmpty, "delta only - concatenated, in order; a blank line is the mount root", {}, {}},
        };
    if (Type == "RegEdit")
        return {{"EDITS", "Registry", FieldKind::RegEdits, "", {}, {}}};
    if (Type == "FileEdit")
        return {
            {"FILE",     "File",     FieldKind::Text,  "path in the runtime - RELATIVE to the pass base", {}, {}},
            {"OVERRIDE", "Override pass", FieldKind::Check, "run after content is mounted", {}, {}},
            {"EDITS",    "Edits",    FieldKind::ObjArray, "", {}, {
                {"MODE",  "Mode",  FieldKind::Enum, "", FModeOpts, {}},
                {"KEY",   "Key",   FieldKind::Text, "ConfigWrite only", {}, {}},
                {"VALUE", "Value", FieldKind::Text, "", {}, {}},
                {"WHEN",  "When",  FieldKind::Text, "condition - inert when false", {}, {}},
            }},
        };
    if (Type == "BinaryPatch")
        //Every field the patch engine reads. An earlier table offered Cave and Poke in the MODE combo while
        //giving no way to supply their PAYLOAD/VALUE, so picking either produced a node that validation then
        //rejected with no way to fix it from the editor — and the four CAVE patches already in the library
        //rendered as patches with no body.
        return {
            {"FILE",  "File",    FieldKind::Text,     "exe in the runtime", {}, {}},
            {"EDITS", "Patches", FieldKind::ObjArray, "", {}, {
                {"MODE",    "Mode",    FieldKind::Enum, "", BModeOpts, {}},
                {"OFFSET",  "Offset",  FieldKind::Text, "0x... (VA or file offset)", {}, {}},
                {"ANCHOR",  "Anchor",  FieldKind::Text, "hex signature, ?? = wildcard (instead of Offset)", {}, {}},
                {"EXPECT",  "Expect",  FieldKind::Text, "pristine bytes (the guard)", {}, {}},
                {"REPLACE", "Replace", FieldKind::Text, "Replace: new bytes", {}, {}},
                {"VALUE",   "Value",   FieldKind::Text, "Poke: the scalar to write", {}, {}},
                {"PAYLOAD", "Payload", FieldKind::Text, "Cave: the cave body (hex)", {}, {}},
                {"CAVE",    "Cave at", FieldKind::Text, "Cave: 'auto' or a fixed VA", {}, {}},
                {"APPLY",   "Apply",   FieldKind::Enum, "", ApplyOpts, {}},
                {"COMMENT", "Comment", FieldKind::Text, "what this patch does", {}, {}},
                {"WHEN",    "When",    FieldKind::Text, "condition - inert when false", {}, {}},
            }},
        };
    if (Type == "DllOverride")
        return {{"OVERRIDES", "Overrides", FieldKind::KeyValue, "dll -> resolution order", DllOpts, {}}};
    if (Type == "DeclarePersist")
        return {
            {"SCOPE",  "Scope",  FieldKind::Enum,  "", ScopeOpts, {}},
            {"PATH",   "Path",   FieldKind::Text,  "runtime path, or HKCU\\Software\\... - empty = the whole runtime", {}, {}},
            {"TARGET", "Target", FieldKind::Text,  "durable name under the instance (defaults to the path's last segment)", {}, {}},
            {"CLOUD",  "Cloud sync", FieldKind::Check, "include in Cloud Saves (off for machine-specific data like shader caches)", {}, {}},
        };
    if (Type == "CustomVar")
        return {
            {"KEY",     "Key",     FieldKind::Text, "used as %KEY%", {}, {}},
            {"DEFAULT", "Default", FieldKind::Text, "", {}, {}},
            {"COMMENT", "Comment", FieldKind::Text, "", {}, {}},
            //WHEN is NODE-level and drawn by the envelope for every type — listing it here too rendered two
            //widgets bound to the same key.
        };
    if (Type == "DeclareExec")
        return {
            {"HOST",        "Host",        FieldKind::Text,       "the platform this needs (win32 / linux64 / ...)", {}, {}},
            {"GUEST",       "Guest",       FieldKind::StringList, "platforms this PROVIDES - set => runner, empty => launchable", {}, {}},
            {"PATH",        "Path",        FieldKind::Text,       "the exe/ROM, anchored", {}, {}},
            {"ARGS",        "Args",        FieldKind::StringList, "one per line", {}, {}},
            {"LABEL",       "Label",       FieldKind::Text,       "shown in the variant picker", {}, {}},
            {"RECOMMENDED", "Recommended", FieldKind::Check,      "default variant of its tile", {}, {}},
            {"WORKDIR",     "Work dir",    FieldKind::Text,       "", {}, {}},
            {"ENV",         "Env",         FieldKind::KeyValue,   "", {}, {}},
            {"ENV_REMOVE",  "Env remove",  FieldKind::StringList, "", {}, {}},
            {"CONTENT_ROOT","Content root",FieldKind::Text,       "runner only", {}, {}},
            {"PREFIX_GENERATE", "Generate prefix", FieldKind::Check, "runner only - needs a wine/proton prefix", {}, {}},
            {"UNIFIED_RUNTIME", "Unified runtime", FieldKind::Check, "runner only - mount the build INTO the game runtime", {}, {}},
            {"RUNNER",      "Pin runner", FieldKind::Text,       "launchable only - a runner NODE_ID to prefer", {}, {}},
        };
    if (Type == "DeclareLibraryItem")
        return {
            {"TITLE", "Title", FieldKind::Text,     "the library tile's name", {}, {}},
            {"UID",   "UID",   FieldKind::Text,     "stable numeric id - keys saves and settings", {}, {}},
            {"COVER", "Cover", FieldKind::Cover,    "", {}, {}},
            {"META",  "Meta",  FieldKind::KeyValue, "catalog metadata", {}, {}},
        };
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

// ---- drawn height ---------------------------------------------------------

namespace {

//One label-and-widget line, and one plain text line. Two constants rather than one because a node is mostly
//widget rows with a few text lines, and the difference compounds over the 59-row RegEdits this exists for.
//Calibrated against ImNodes::GetNodeDimensions in the GUI suite, not guessed.
constexpr float kRowPx     = 19.0f;   // label + widget + item spacing
constexpr float kTextPx    = 17.0f;   // a bare text line
constexpr float kTitlePx   = 34.0f;   // the type title bar
constexpr float kChromePx  = 26.0f;   // imnodes' own node padding, top and bottom together
//An ImGui::Separator() costs exactly ItemSpacing.y: SeparatorEx sets thickness_for_layout to 0 for a 1px
//line, and the style is untouched StyleColorsDark, so this is 4px everywhere with no context dependence.
//An earlier version charged 6 here and 2 in the RegEdits arm, described that as measured, and was really two
//compensating errors — the RegEdits total came out right while its decomposition was wrong, and the ObjArray
//arm over-charged 4px per entry. Splitting the button row out makes both arms say what they mean.
constexpr float kSepPx     =  4.0f;   // an ImGui::Separator() between batched entries
constexpr float kBtnPx     = 17.0f;   // a row holding SmallButtons (no frame padding, unlike kRowPx)

//Rows a StringList spends: one for the label line, plus the multiline box when the value has several lines.
//The box is 16px per line, capped at 6 lines.
//
//LINES, not entries. drawField renders the joined text and counts newlines in it, and the join does not escape
//a newline INSIDE an entry — so one array element carrying embedded newlines is a multiline box that an
//entry count calls a single-line input. Measured on a DeclareExec whose GUEST/ARGS/ENV_REMOVE each held one
//such entry: drawn 682px against an estimated 542, a 140px shortfall — 50px past the layout's whole RowGap,
//i.e. a real box-on-box overlap, stamped into POS at publish time. Any package this canvas did not author can
//contain one, because ARGS and friends are unvalidated strings.
} // namespace

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

namespace {

float StringListPx(const json &V, bool ForceMultiline)
{
    int Lines = 0;
    if (V.is_array())
    {
        for (const auto &E : *V.get_ptr<const json::array_t *>())
            if (E.is_string())
                Lines += 1 + (int)std::count(E.get_ref<const std::string &>().begin(),
                                             E.get_ref<const std::string &>().end(), '\n');
    }
    else if (V.is_string() && !V.get<std::string>().empty())
    {
        Lines = 1 + (int)std::count(V.get_ref<const std::string &>().begin(),
                                    V.get_ref<const std::string &>().end(), '\n');
    }
    if (Lines <= 1 && !ForceMultiline) return kRowPx;
    return kRowPx + 16.0f * (float)std::min(Lines + 1, 6);
}

float FieldPx(const json &Node, const Field &F)
{
    const json *V = (Node.is_object() && Node.contains(F.Key)) ? &Node[F.Key] : nullptr;
    switch (F.Kind)
    {
    case FieldKind::Text:
    case FieldKind::Enum:
    case FieldKind::Check:
    case FieldKind::Cover:
        return kRowPx;
    case FieldKind::StringList:
    case FieldKind::StringListKeepEmpty:
        //A value of the wrong shape draws as ONE line — the label, then a short description as disabled text
        //and no widget at all (drawField refuses to edit it). Through the SHARED predicate, so "wrong shape"
        //cannot come to mean two different things on the two sides of it.
        if (StringListFault(V) != kStringListOk) return kTextPx;
        //KeepEmpty is ALWAYS a box: a blank line is data there, so drawField never collapses it to a
        //single-line input.
        return StringListPx(V ? *V : json(), F.Kind == FieldKind::StringListKeepEmpty);
    case FieldKind::KeyValue:
    {
        const size_t N = (V && V->is_object()) ? V->size() : 0;
        return kTextPx + (float)N * kRowPx + kBtnPx;            // label, one row per pair, the add row
    }
    case FieldKind::ObjArray:
    {
        const size_t N = (V && V->is_array()) ? V->size() : 0;
        //drawField reads a batched payload CAPPED at 12 entries and prints a "... and N more" line instead —
        //77 BinaryPatches must not turn the node into a wall, and the height must agree with that or the
        //layout reserves a screenful of space nothing occupies.
        const size_t Shown = std::min<size_t>(N, 12);
        float Px = kTextPx;                                     // "Label (N)"
        for (size_t I = 0; I < Shown; ++I)
        {
            Px += kSepPx;                                       // the separator between entries
            //Same malformed-entry branch the canvas draws: one disabled line and a remove button.
            if (!V->at(I).is_object()) { Px += kTextPx + kBtnPx; continue; }
            for (const Field &S : F.Sub) Px += FieldPx(V->at(I), S);
            Px += kBtnPx;                                       // the entry's remove button
        }
        if (N > Shown) Px += kTextPx;                           // "... and N more"
        return Px + kBtnPx;                                     // the add button
    }
    case FieldKind::RegEdits:
    {
        //The one payload with no cap at all: a registry group draws every row it holds, and the codec
        //libraries ship nodes with 59 of them.
        if (!V || !V->is_array()) return kRowPx;
        float Px = 0.0f;
        for (const json &E : *V)
        {
            //A malformed entry short-circuits to a separator and one disabled line, and nothing else —
            //drawRegEdits has that branch precisely because this editor is what you open a broken package
            //with. Charging it a full entry was 19.6px per entry too much, enough to trip the slack bound on
            //a node made entirely of them.
            if (!E.is_object()) { Px += kSepPx + kTextPx; continue; }
            //Otherwise, exactly what drawRegEdits emits: a Separator, the "views" label with the 32/64
            //checkboxes and "override pass" all on one SameLine chain, the value rows, and the
            //"+ value" / "remove group" SmallButtons — also one line, also SameLine.
            //
            //This arm has now been wrong in three different ways, each invisible until a case with enough
            //ENTRIES existed: two rows for the checkbox line (+19/entry), one row and no separator
            //(-2/entry), and a correct line plus a leftover duplicate of the button row (+19/entry again).
            //Per-entry errors are the ones that hide, because a single-entry node absorbs any of them.
            Px += kSepPx + kRowPx + kBtnPx;
            //The row count the canvas will draw. Counted rather than built: RegRowsOf materialises three
            //std::strings per row, and on a graph of 500 nodes carrying 59 rows each that was 29,500 RegRow
            //structs per rebuild — 1.45 ms of a 3.58 ms Build, 40% of it, for a number. CountRegRows walks
            //the same tree by the same rules and allocates nothing.
            Px += (float)CountRegRows(E) * kRowPx;
        }
        return Px + kBtnPx;                                     // the trailing "+ group" button
    }
    }
    return kRowPx;
}

} // namespace

float EstimateHeight(const json &Node)
{
    //A NODES entry that is not an object is a PLACEHOLDER, and it draws as one: a title bar and a single
    //disabled line saying so (see PkgCanvas::drawNode). Falling through to the payload arms below charged it
    //a full Group's worth of chrome, and leaving Height at its default 0 — which is what Build did, since it
    //push_backs the placeholder before this is ever called — gave PkgLayout no vertical space for it at all,
    //so the next node in the column was stacked on top of it.
    if (!Node.is_object()) return kChromePx + kTitlePx + kTextPx;
    const std::string Type = (Node.is_object() && Node.contains("TYPE") && Node["TYPE"].is_string())
                                 ? Node["TYPE"].get<std::string>() : std::string("Group");
    float Px = kChromePx + kTitlePx;
    Px += kTextPx;                                   // the "depends on / used by" pin row
    //The "not wired yet" note, which drawNode shows on any non-launchable nothing depends on. Counted ALWAYS,
    //even though a wired node does not draw it: whether a node has dependents is a property of the GRAPH, not
    //of the node, and EstimateHeight is payload-only on purpose. One text line of slack on a wired node is the
    //cheap direction to be wrong in — the expensive one is a node drawn taller than the space reserved for it.
    Px += kTextPx;
    Px += kRowPx;                                    // the id row
    //"node options" counted as though it were OPEN, always — its three rows plus the tree line. Whether it is
    //open is not a property of the payload at all: it opens itself when TOGGLE/WHEN/EXCLUDE is set, and after
    //that the author can open or close it on any node, with the state living in ImGui's own per-window storage.
    //Predicting it is therefore impossible and guessing it is unsafe in the one direction that matters —
    //opening a collapsed node would make it 57px taller than the space the layout reserved, and it would
    //overlap its neighbour. Three rows of slack on every node is the price of that never happening.
    Px += kTextPx + 3.0f * kRowPx;
    for (const Field &F : FieldsFor(Type))
    {
        //The same skip drawPayload makes, and for the same reason: BASE_TARGETS belongs to a delta, and a
        //Content zip or dir never draws it. Charging it on every Content node reserved a row that nothing
        //occupies — harmless for overlap, but this number is STAMPED into the package at publish, so an
        //estimate that disagrees with what is drawn is a hole in the published layout on every peer's canvas.
        //FORM is payload, so the skip stays as deterministic as the rest of this function.
        if (F.Key == std::string("BASE_TARGETS")
            && ((Node.contains("FORM") && Node["FORM"].is_string()) ? Node["FORM"].get<std::string>()
                                                                    : std::string()) != "delta") continue;
        Px += FieldPx(Node, F);
    }
    //The action row: a Separator, then one SmallButton line per wrap. drawActions starts a new line whenever
    //the next button would pass the node width, and a Content zip with a deflate hint and a zip parent has
    //five of them — but how many actions a node offers depends on HOST facts (a hint saying this zip is
    //DEFLATE-compressed), which are not in the payload and not knowable here. One line, and the reserve above
    //absorbs a second; a node offering three rows of actions can still overflow.
    Px += kSepPx + kBtnPx;
    //Room for a couple of the validation warnings the canvas draws ON a node. They are host state rather than
    //payload — the same node has none at publish time, which is when this number is stamped — and there can be
    //any number of them, so they cannot be counted properly here.
    //
    //MEASURED, 2026-09-14 (theEstimatedNodeHeightMatchesTheDrawnOne prints every shape): the leftover slack is
    //31px on the tightest shapes (a bare Group, any RegEdit) and 48-65px on the rest, against a warning line
    //of 17px. So this reservation buys ONE warning outright and the second overruns the estimate by 3px on a
    //Group — the "six findings" figure an earlier version of this comment gave was attached to the wrong
    //quantity. Overrunning the ESTIMATE is not overlap: PkgLayout pitches by Height + RowGap, so a node has
    //31 + 90 = 121px, about seven warning lines, before it reaches the node beneath it. That is the number
    //that matters, and it is why two lines is the right reservation rather than a generous one.
    Px += 2.0f * kTextPx;
    return Px;
}

// ---- node actions ---------------------------------------------------------

std::vector<Action> ActionsFor(const json &Node, const Graph &G, int Index,
                               const std::vector<std::string> &Hints)
{
    std::vector<Action> A;
    const std::string Type = (Node.is_object() && Node.contains("TYPE") && Node["TYPE"].is_string())
                                 ? Node["TYPE"].get<std::string>() : std::string("Group");
    auto HasHint = [&](const char *H) {
        return std::find(Hints.begin(), Hints.end(), H) != Hints.end();
    };

    if (Type == "Content")
    {
        const std::string Form = (Node.contains("FORM") && Node["FORM"].is_string())
                                     ? Node["FORM"].get<std::string>() : std::string();
        A.push_back({"browse", "browse...", "Pick the file or folder this layer supplies.", false});
        if (Form == "dir")
            A.push_back({"to_zip", "-> zip", "Pack this directory as an uncompressed (STORE) zip and delete the folder.", true});
        if (Form == "zip")
        {
            A.push_back({"to_dir", "-> dir", "Unpack this zip to a folder and delete the zip.", true});
            if (HasHint("deflate"))
                A.push_back({"restore", "! re-store", "This zip is DEFLATE-compressed and cannot mount - re-pack it uncompressed.", true});
            //A delta needs a base: only offered when a PARENT is itself content to diff against.
            //Must match what the action actually requires: a parent that is Content AND a zip. Offering it on
            //a delta or dir parent only to refuse afterwards is a button that lies.
            for (const Link &L : G.Links)
                if (L.ChildIndex == Index && L.ParentIndex >= 0
                    && G.Nodes[L.ParentIndex].Type == "Content" && G.Nodes[L.ParentIndex].Form == "zip")
                { A.push_back({"to_delta", "-> delta", "Store this as a binary delta against its parent's content.", true}); break; }
        }
        //The exact inverse of "-> delta": reconstruct the full archive and go back to being a plain zip. From
        //there the ordinary zip actions (-> dir, re-store) apply, so there is one reverse conversion rather
        //than a second path that happens to end in a folder.
        if (Form == "delta")
            A.push_back({"undelta", "undelta", "Reconstruct the full archive from this delta and store it as a plain zip again.", true});
    }
    else if (Type == "RegEdit")
    {
        A.push_back({"capture_reg", "capture registry", "Open regedit on this point of the chain and capture what changes.", true});
        A.push_back({"import_reg", "import .reg", "Read a .reg file into this node's keys.", false});
    }
    else if (Type == "CustomVar")
    {
        A.push_back({"find_usages", "find usages", "Which nodes in the closure reference this %KEY%.", false});
    }
    else if (Type == "DeclareExec")
    {
        //No GUEST ⇒ terminal ⇒ launchable: it is the thing you can actually run.
        const bool IsRunner = Node.contains("GUEST") && Node["GUEST"].is_array() && !Node["GUEST"].empty();
        if (!IsRunner) A.push_back({"test_launch", "test launch", "Open the pre-launch window for this launchable and run it.", false});
    }
    else if (Type == "DeclareLibraryItem")
    {
        A.push_back({"browse_cover", "cover...", "Pick the cover image for this tile.", false});
    }

    //Available ANYWHERE along the chain: open a live runtime built from this node's closure, run an installer,
    //and capture what it wrote — the captures become NEW nodes parented here.
    A.push_back({"capture_setup", "capture setup", "Run an installer on a live runtime at this point and capture the files and registry it writes.", true});
    if (Type != "Content")
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
    //Preserve the ORIGINAL key order. The rebuild emits the non-hive fields first and the hives after, so an
    //entry authored the other way round came back with the same content in a different order — same size,
    //different bytes, and therefore a different content CID for every peer, from touching one row.
    json Ordered = json::object();
    for (const auto &[K, V] : Entry.items()) if (Kept.contains(K)) Ordered[K] = Kept[K];
    for (const auto &[K, V] : Kept.items())  if (!Ordered.contains(K)) Ordered[K] = V;
    Entry = std::move(Ordered);
}

} // namespace PkgGraph
