#include "pkggraph.h"

#include "pkglayout.h"   // PkgLayout::ComputeUnplaced — the shared auto-layout

#include <algorithm>
#include <functional>
#include <map>
#include <set>

using json = nlohmann::ordered_json;

namespace PkgGraph
{

// ---- graph ----------------------------------------------------------------

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
        if (!N.is_object()) { G.Nodes.push_back(std::move(Nd)); continue; }
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
        auto ReadPos = [&](const json &P) {
            if (!P.is_array() || P.size() != 2 || !P[0].is_number() || !P[1].is_number()) return false;
            Nd.X = P[0].get<float>(); Nd.Y = P[1].get<float>(); Nd.HasPos = true;
            return true;
        };
        if (N.contains("POS")) ReadPos(N["POS"]);
        if (Layout && Layout->is_object() && !Nd.Id.empty() && Layout->contains(Nd.Id))
            ReadPos((*Layout)[Nd.Id]);
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
        "Persist", "CustomVar", "DeclareExec", "DeclareLibraryItem", "Group"
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
    if (Type == "Persist")            return "What survives the run: KEEP promotes paths/registry keys, DROP makes them ephemeral.";
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
    if (Type == "Persist" || Type == "CustomVar")                { R = 92;  G = 68;  B = 128; return; }
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
    if (Type == "Persist")
        return json::object({{"TYPE","Persist"},{"KEEP", json::array()},{"DROP", json::array()}});
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
    if (Type == "Persist")
        return {
            {"KEEP", "Keep", FieldKind::StringList, "path, or HKCU\\Software\\... - one per line", {}, {}},
            {"DROP", "Drop", FieldKind::StringList, "paths made ephemeral", {}, {}},
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
constexpr float kRowPx     = 23.0f;   // label + widget + item spacing
constexpr float kTextPx    = 17.0f;   // a bare text line
constexpr float kTitlePx   = 34.0f;   // the type title bar
constexpr float kChromePx  = 26.0f;   // imnodes' own node padding, top and bottom together

//Rows a StringList spends: one for the label line, plus the multiline box when the value has several lines.
//Mirrors drawField exactly — the box is 16px per line, capped at 6 lines.
float StringListPx(const json &V, bool ForceMultiline)
{
    int Lines = 0;
    if (V.is_array()) Lines = (int)V.size();
    else if (V.is_string() && !V.get<std::string>().empty()) Lines = 1;
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
        return StringListPx(V ? *V : json(), false);
    case FieldKind::StringListKeepEmpty:
        //Always a box: a blank line is data here, so drawField never collapses it to a single-line input.
        return StringListPx(V ? *V : json(), true);
    case FieldKind::KeyValue:
    {
        const size_t N = (V && V->is_object()) ? V->size() : 0;
        return kTextPx + (float)N * kRowPx + kRowPx;            // label, one row per pair, the add row
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
            Px += 6.0f;                                         // the separator between entries
            for (const Field &S : F.Sub) Px += FieldPx(V->at(I), S);
            Px += kRowPx;                                       // the entry's remove button
        }
        if (N > Shown) Px += kTextPx;                           // "... and N more"
        return Px + kRowPx;                                     // the add button
    }
    case FieldKind::RegEdits:
    {
        //The one payload with no cap at all: a registry group draws every row it holds, and the codec
        //libraries ship nodes with 59 of them.
        if (!V || !V->is_array()) return kRowPx;
        float Px = 0.0f;
        for (const json &E : *V)
        {
            Px += kRowPx * 2.0f;                                // "views" and the override checkbox
            //The row count the canvas will draw, from the SAME flattening it draws from — not a second
            //reading of the tree that could disagree with it.
            Px += (float)RegRowsOf(E).size() * kRowPx;
            Px += kRowPx;                                       // "+ value" / "remove group"
        }
        return Px + kRowPx;                                     // the add-group row
    }
    }
    return kRowPx;
}

} // namespace

float EstimateHeight(const json &Node)
{
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
    for (const Field &F : FieldsFor(Type)) Px += FieldPx(Node, F);
    Px += kRowPx;                                    // the action buttons
    //NOT counted: the validation warnings the canvas draws on a node while you are looking at it. They are
    //host state rather than payload — the same node has none at publish time, which is when this number is
    //stamped — and they are unbounded. The slack above absorbs a line or two of them.
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
