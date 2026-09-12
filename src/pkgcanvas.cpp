#include "pkgcanvas.h"

#include "imgui.h"
#include "imgui_stdlib.h"
#include "imnodes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using json = nlohmann::ordered_json;
using namespace PkgGraph;

namespace {

//A PARENTS edge needs a stable int id for imnodes. Slot = position in the child's PARENTS array.
constexpr int kMaxParents = 256;
inline int LinkId(int ChildIndex, int Slot) { return ChildIndex * kMaxParents + Slot; }
inline int LinkChild(int Id)                { return Id / kMaxParents; }
inline int LinkSlot(int Id)                 { return Id % kMaxParents; }

//External parents are drawn as chips; give them node ids past the real ones so imnodes keeps them distinct.
constexpr int kExternalBase = 100000;

//One uniform node body, so the graph tiles predictably and long ids/paths scroll inside their field instead of
//stretching the box. The old editor's forms grew to their longest string and the canvas inherited that.
constexpr float kNodeWidth  = 330.0f;
constexpr float kLabelCol   = 96.0f;
constexpr float kFieldWidth = 228.0f;

//TOTAL readers. The editor loads raw JSON off disk — deliberately, since it is the tool you open to FIX a
//node the format rejects — so every field here may be any type. nlohmann's value() THROWS on a mismatch, and
//a throw out of frame() escapes with ImGui scopes still open: the next paint then segfaults on the half-open
//state. `"WHEN": true` and `"OVERRIDE": "true"` are both plausible slips, and NodeLower has a dedicated
//diagnostic telling the author to come here and fix exactly those.
std::string StrOf(const json &N, const char *Key, const std::string &Def = {})
{
    return (N.is_object() && N.contains(Key) && N[Key].is_string()) ? N[Key].get<std::string>() : Def;
}
bool BoolOf(const json &N, const char *Key, bool Def = false)
{
    return (N.is_object() && N.contains(Key) && N[Key].is_boolean()) ? N[Key].get<bool>() : Def;
}

//NOTE on the one shape this cannot express: the text is entries joined by newlines with no terminator, so []
//and [""] both render as "". A LONE root base therefore shows as an empty box. It is PRESERVED — the list is
//only rewritten when its own box is edited, and editing an empty box is the user replacing it — but it cannot
//be typed from scratch here; author it as BASE_TARGETS: [""] (or let makeDelta write it). Adding a terminator
//to disambiguate was tried and is worse: ListToText(TextToList(t)) then stops being the identity, so the text
//grows a newline under the cursor while you type.
std::string ListToText(const json &Arr)
{
    std::string T;
    if (Arr.is_array())
        for (const auto &E : Arr) if (E.is_string()) { T += E.get<std::string>(); T += '\n'; }
    if (!T.empty() && T.back() == '\n') T.pop_back();
    return T;
}

//One line per entry, trimmed. `KeepEmpty` decides what a BLANK line means: for most lists it is typing noise
//and is dropped, but for a delta's BASE_TARGETS an empty entry is the MOUNT ROOT — a real target. Dropping it
//there made the root base untypeable, and silently deleted it from any node that already had one.
json TextToList(const std::string &T, bool KeepEmpty = false)
{
    json Arr = json::array();
    std::string Line;
    for (size_t I = 0; I <= T.size(); ++I)
    {
        if (I == T.size() || T[I] == '\n')
        {
            size_t B = Line.find_first_not_of(" \t\r"), E = Line.find_last_not_of(" \t\r");
            if (B != std::string::npos)   Arr.push_back(Line.substr(B, E - B + 1));
            //A blank line is an entry — INCLUDING the last one. ListToText writes no terminator, so "a\n" is
            //["a",""] and "a" is ["a"]; treating the final blank as a separator silently ate a TRAILING root
            //base, which is exactly the ["dxvk", ""] shape a prefix delta over the runner-mount root has.
            //Wholly empty text is the one exception: that is no entries, and it ERASES the key.
            else if (KeepEmpty && !T.empty()) Arr.push_back(std::string());
            Line.clear();
        }
        else Line += T[I];
    }
    return Arr;
}

} // namespace

struct PkgCanvasState
{
    json *Doc = nullptr;
    //The canvas-position sidecar (NODE_ID -> [x,y]), owned by the model and persisted to
    //<bundle>/LAYOUT.vglayout. Never the document: see PkgGraph::Build. Null = positions are not persisted.
    json *Layout = nullptr;
    PkgCanvas::SaveFn Save;
    PkgCanvas::KnownIdsFn KnownIds;
    PkgCanvas::ActionFn Action;
    ImNodesContext *Ctx = nullptr;
    std::map<std::string, std::vector<std::string>> Issues;   // NODE_ID → messages
    //A heavy action in flight on one node. Canvas-local and keyed by NODE_ID so it is immune to the node's
    //index moving, and so it never reaches the document.
    struct Busy { QString What; QString Detail; float Frac = -1.0f; bool Cancellable = false; };
    std::map<std::string, Busy> Running;
    std::map<std::string, std::vector<std::string>> Hints;   // NODE_ID → host-probed facts ("deflate")
    //imnodes keys nodes by the int we hand it (the array INDEX), and it has no persisted state of its own
    //(IniFilename is null). So a node's stored position has to be pushed in, and pushed in AGAIN whenever its index
    //moves — otherwise a delete slides every node's position onto its neighbour. Keyed by NODE_ID, which is
    //what actually identifies the node.
    std::map<std::string, int> Seeded;                       // NODE_ID -> the index its position was pushed at
    //RegEdit rows are a FLATTENING of the hive tree: committing one on every keystroke rebuilt the tree and
    //re-flattened it next frame, which reorders rows (values sort before subkeys) and merges two rows the
    //instant a half-typed path matches another. The row being edited then moved out from under the cursor.
    //So the rows an entry is being edited with live here, keyed "NODE_ID#entry", and are committed to the
    //tree when the field is DEACTIVATED - the same "commit on finish" rule the KeyValue rename uses.
    std::map<std::string, std::vector<PkgGraph::RegRow>> RegBuf;

    bool Dirty = false;            // document mutated this frame; persist on mouse-up
    //Every mutation goes through here, so the cached graph can never outlive the document it was built from.
    void MarkDirty() { Dirty = true; CacheValid = false; }
    bool PosDirty = false;
    int  Selected = -1;
    int  ConfirmDelete = -1;                                 // a Delete awaiting confirmation
    std::pair<std::string, std::string> Pending;              // an action clicked this frame, run after it
    //The graph is rebuilt from the document, which is O(nodes+links). Doing that at 60Hz on a 2775-node
    //bundle pins a core for no reason: the document only changes when something edits it.
    PkgGraph::Graph Cache;
    bool CacheValid = false;
    std::set<int> HasDependent;                              // precomputed once per rebuild
    std::map<int, int> LinkSlot;
    std::vector<std::string> OfferedExternals;               // chips placed by the picker, not yet wired
    std::string ExternalFilter;
    float SidePanel = 360.0f;
    std::string AddType = "Content";

    json &Nodes()
    {
        if (!Doc->contains("NODES") || !(*Doc)["NODES"].is_array()) (*Doc)["NODES"] = json::array();
        return (*Doc)["NODES"];
    }
};

PkgCanvas::PkgCanvas(json *doc, SaveFn save, QObject *parent, json *layout)
    : QObject(parent), m_s(std::make_unique<PkgCanvasState>())
{
    m_s->Doc = doc;
    m_s->Layout = layout;
    m_s->Save = std::move(save);
}

PkgCanvas::~PkgCanvas() = default;

void PkgCanvas::initContexts()
{
    m_s->Ctx = ImNodes::CreateContext();
    ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &ImGui::GetIO().KeyCtrl;
    ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);
}

void PkgCanvas::shutdownContexts()
{
    if (!m_s->Ctx) return;
    ImNodes::PopAttributeFlag();
    ImNodes::DestroyContext(m_s->Ctx);
    m_s->Ctx = nullptr;
}

void PkgCanvas::setKnownIds(KnownIdsFn fn)       { m_s->KnownIds = std::move(fn); }
//The document was replaced behind the canvas, so every INDEX we were holding is meaningless — including the
//selection, which the Delete key reads, and the imnodes position seeding, which is keyed by (id, index).
void PkgCanvas::invalidateGraph()
{
    m_s->CacheValid = false;
    m_s->Selected = -1;
    m_s->ConfirmDelete = -1;
}

void PkgCanvas::setNodeHints(const std::string &nodeId, const std::vector<std::string> &hints)
{ m_s->Hints[nodeId] = hints; }

void PkgCanvas::beginAction(const std::string &nodeId, const QString &what, bool cancellable)
{ m_s->Running[nodeId] = PkgCanvasState::Busy{what, {}, -1.0f, cancellable}; }

void PkgCanvas::setProgress(const std::string &nodeId, float fraction, const QString &detail)
{
    auto It = m_s->Running.find(nodeId);
    if (It == m_s->Running.end()) return;
    It->second.Frac = fraction;
    if (!detail.isNull()) It->second.Detail = detail;
}

void PkgCanvas::endAction(const std::string &nodeId) { m_s->Running.erase(nodeId); }

bool PkgCanvas::isBusy(const std::string &nodeId) const
{ return m_s->Running.find(nodeId) != m_s->Running.end(); }
void PkgCanvas::setActionHandler(ActionFn fn)    { m_s->Action   = std::move(fn); }

void PkgCanvas::setIssues(const std::vector<std::pair<std::string, std::string>> &issues)
{
    m_s->Issues.clear();
    for (const auto &[Id, Msg] : issues) m_s->Issues[Id].push_back(Msg);
}

int PkgCanvas::nodeCount() const
{
    return (m_s->Doc && m_s->Doc->contains("NODES") && (*m_s->Doc)["NODES"].is_array())
               ? (int)(*m_s->Doc)["NODES"].size() : 0;
}

int PkgCanvas::indexOf(const std::string &nodeId) const
{
    const int N = nodeCount();
    for (int I = 0; I < N; ++I)
        if (StrOf((*m_s->Doc)["NODES"][I], "NODE_ID") == nodeId) return I;
    return -1;
}

Graph PkgCanvas::graph() const { return Build(m_s->Nodes(), m_s->Layout); }
int  PkgCanvas::selectedNode() const { return m_s->Selected; }
//VALIDATED: this is public, and a selection is an INDEX into a document that can be replaced underneath it.
void PkgCanvas::selectNode(int index)
{
    m_s->Selected = (index >= 0 && index < (int)m_s->Nodes().size()) ? index : -1;
}

int PkgCanvas::addNode(const std::string &type, float x, float y)
{
    json N = NewPayload(type);
    // A unique-by-construction id: the old editor made every new node "new_node" and let the duplicate-id
    // warning sort it out later.
    std::string Base = type == "Group" ? "group" : type;
    for (char &C : Base) C = (char)std::tolower((unsigned char)C);
    std::string Id = Base;
    for (int K = 2; indexOf(Id) >= 0; ++K) Id = Base + "_" + std::to_string(K);
    json Out = json::object({{"NODE_ID", Id}, {"PARENTS", json::array()}});
    for (const auto &[K, V] : N.items()) Out[K] = V;
    if (m_s->Layout) { SetPos(*m_s->Layout, Id, x, y); m_s->PosDirty = true; }
    m_s->Nodes().push_back(std::move(Out));
    m_s->MarkDirty();
    return (int)m_s->Nodes().size() - 1;
}

bool PkgCanvas::removeNode(int index)
{
    json &Ns = m_s->Nodes();
    if (index < 0 || index >= (int)Ns.size()) return false;
    const std::string Id = StrOf(Ns[index], "NODE_ID");
    //Deleting shifts every later index, so every seeded position is now against the wrong node. Drop them all
    //and let the next frame re-seed from the layout sidecar.
    m_s->Seeded.clear();
    //...and take this node's canvas state with it, or a later node reusing the id inherits a stale position,
    //a stale busy state and stale registry edit buffers.
    if (m_s->Layout && m_s->Layout->is_object()) { m_s->Layout->erase(Id); m_s->PosDirty = true; }
    m_s->Running.erase(Id);
    m_s->Hints.erase(Id);
    m_s->Seeded.erase(Id);
    for (auto It = m_s->RegBuf.begin(); It != m_s->RegBuf.end(); )
        It = (It->first.rfind(Id + "#", 0) == 0) ? m_s->RegBuf.erase(It) : std::next(It);
    Ns.erase(index);
    // Drop every reference to it so the graph never carries a dangling parent after a delete.
    for (auto &N : Ns)
    {
        if (!N.contains("PARENTS") || !N["PARENTS"].is_array()) continue;
        json Keep = json::array();
        for (const auto &P : N["PARENTS"]) if (!(P.is_string() && P.get<std::string>() == Id)) Keep.push_back(P);
        N["PARENTS"] = std::move(Keep);
    }
    m_s->Selected = -1;
    m_s->MarkDirty();
    return true;
}

bool PkgCanvas::connect(int parentIndex, int childIndex)
{
    json &Ns = m_s->Nodes();
    if (parentIndex < 0 || childIndex < 0 || parentIndex >= (int)Ns.size() || childIndex >= (int)Ns.size()) return false;
    if (parentIndex == childIndex) return false;
    const std::string Pid = StrOf(Ns[parentIndex], "NODE_ID");
    if (Pid.empty()) return false;
    if (!Ns[childIndex].contains("PARENTS") || !Ns[childIndex]["PARENTS"].is_array())
        Ns[childIndex]["PARENTS"] = json::array();
    for (const auto &P : Ns[childIndex]["PARENTS"]) if (P.is_string() && P.get<std::string>() == Pid) return false;
    Ns[childIndex]["PARENTS"].push_back(Pid);
    m_s->MarkDirty();
    return true;
}

bool PkgCanvas::disconnect(int parentIndex, int childIndex)
{
    json &Ns = m_s->Nodes();
    if (parentIndex < 0 || childIndex < 0 || parentIndex >= (int)Ns.size() || childIndex >= (int)Ns.size()) return false;
    const std::string Pid = StrOf(Ns[parentIndex], "NODE_ID");
    if (!Ns[childIndex].contains("PARENTS") || !Ns[childIndex]["PARENTS"].is_array()) return false;
    json Keep = json::array();
    bool Removed = false;
    for (const auto &P : Ns[childIndex]["PARENTS"])
    {
        if (P.is_string() && P.get<std::string>() == Pid && !Removed) { Removed = true; continue; }
        Keep.push_back(P);
    }
    Ns[childIndex]["PARENTS"] = std::move(Keep);
    if (Removed) m_s->MarkDirty();
    return Removed;
}

bool PkgCanvas::connectExternal(const std::string &parentId, int childIndex)
{
    json &Ns = m_s->Nodes();
    if (parentId.empty() || childIndex < 0 || childIndex >= (int)Ns.size()) return false;
    if (StrOf(Ns[childIndex], "NODE_ID") == parentId) return false;
    if (!Ns[childIndex].contains("PARENTS") || !Ns[childIndex]["PARENTS"].is_array())
        Ns[childIndex]["PARENTS"] = json::array();
    for (const auto &P : Ns[childIndex]["PARENTS"]) if (P.is_string() && P.get<std::string>() == parentId) return false;
    Ns[childIndex]["PARENTS"].push_back(parentId);
    m_s->MarkDirty();
    return true;
}

void PkgCanvas::offerExternal(const std::string &parentId)
{
    if (parentId.empty()) return;
    auto &O = m_s->OfferedExternals;
    if (std::find(O.begin(), O.end(), parentId) == O.end()) O.push_back(parentId);
}

bool PkgCanvas::renameNode(int index, const std::string &newId)
{
    json &Ns = m_s->Nodes();
    if (index < 0 || index >= (int)Ns.size()) return false;
    const std::string Old = StrOf(Ns[index], "NODE_ID");
    if (Old == newId || newId.empty()) return false;
    // Refuse a name another node already owns. Both would map to <id>.json, SaveNodes would write them as one
    // array, and ScanBundleNodes keeps first-seen — silently dropping a node from the graph while the orphan
    // sweep removed the old file. Typing through a colliding name is transient, so this just does nothing
    // until the name is unique again.
    for (int I = 0; I < (int)Ns.size(); ++I)
        if (I != index && StrOf(Ns[I], "NODE_ID") == newId) return false;
    Ns[index]["NODE_ID"] = newId;
    //Re-point EVERY field that holds a NODE_ID, not just PARENTS. A rename that fixes only the edges leaves the
    //others pointing at a name nothing answers to: EXCLUDE silently stops excluding (two mutually-exclusive
    //variants both become selectable, caught later only as a WARNING), and a RUNNER pin silently falls back to
    //the default runner, which nothing catches at all.
    for (auto &N : Ns)
    {
        if (!N.is_object()) continue;
        for (const char *Key : {"PARENTS", "EXCLUDE"})
            if (N.contains(Key) && N[Key].is_array())
                for (auto &P : N[Key]) if (P.is_string() && P.get<std::string>() == Old) P = newId;
        if (StrOf(N, "RUNNER") == Old) N["RUNNER"] = newId;
    }
    //...and carry the canvas position across, or the box jumps to the auto-layout column on the first
    //character typed (renameNode runs per keystroke) and the sidecar accumulates a dead key per rename.
    if (m_s->Layout && m_s->Layout->is_object() && m_s->Layout->contains(Old))
    {
        (*m_s->Layout)[newId] = (*m_s->Layout)[Old];
        m_s->Layout->erase(Old);
        m_s->PosDirty = true;
    }
    //Seeded/Running/Hints/RegBuf are all keyed by NODE_ID too.
    auto Move = [&](auto &M) { auto It = M.find(Old); if (It == M.end()) return;
                               M[newId] = std::move(It->second); M.erase(It); };
    Move(m_s->Seeded); Move(m_s->Running); Move(m_s->Hints);
    //Collected first, then re-inserted: inserting into the map being iterated can land the new key AFTER the
    //cursor, where it matches the same prefix again (a NODE_ID containing '#' is enough) and the loop never ends.
    {
        std::vector<std::pair<std::string, std::vector<PkgGraph::RegRow>>> Moved;
        for (auto It = m_s->RegBuf.begin(); It != m_s->RegBuf.end(); )
        {
            if (It->first.rfind(Old + "#", 0) != 0) { ++It; continue; }
            Moved.emplace_back(newId + It->first.substr(Old.size()), std::move(It->second));
            It = m_s->RegBuf.erase(It);
        }
        for (auto &M : Moved) m_s->RegBuf[M.first] = std::move(M.second);
    }
    m_s->MarkDirty();
    return true;
}

// ---- payload rendering ----------------------------------------------------

void PkgCanvas::drawField(json &Node, const Field &F, int Index)
{
    ImGui::PushID(F.Key);
    const float W = kFieldWidth;
    switch (F.Kind)
    {
    case FieldKind::Text:
    {
        std::string V = StrOf(Node, F.Key);
        ImGui::TextUnformatted(F.Label); ImGui::SameLine(kLabelCol);
        ImGui::SetNextItemWidth(W);
        if (ImGui::InputTextWithHint("##v", F.Hint, &V)) { Node[F.Key] = V; m_s->MarkDirty(); }
        break;
    }
    case FieldKind::Enum:
    {
        const std::string Cur = Node.contains(F.Key) && Node[F.Key].is_string() ? Node[F.Key].get<std::string>() : std::string();
        const char *Shown = Cur.c_str();
        for (const auto &O : F.Options) if (Cur == O.first) Shown = O.second;
        ImGui::TextUnformatted(F.Label); ImGui::SameLine(kLabelCol);
        ImGui::SetNextItemWidth(W);
        if (ImGui::BeginCombo("##v", Shown))
        {
            for (const auto &O : F.Options)
                if (ImGui::Selectable(O.second, Cur == O.first)) { Node[F.Key] = O.first; m_s->MarkDirty(); }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldKind::Check:
    {
        bool V = BoolOf(Node, F.Key);
        if (ImGui::Checkbox(F.Label, &V)) { Node[F.Key] = V; m_s->MarkDirty(); }
        break;
    }
    case FieldKind::StringList:
    case FieldKind::StringListKeepEmpty:
    {
        const bool KeepEmpty = (F.Kind == FieldKind::StringListKeepEmpty);
        std::string T = ListToText(Node.contains(F.Key) ? Node[F.Key] : json::array());
        //A KeepEmpty field is ALWAYS multiline — not "when it has entries". A blank line cannot be typed into a
        //single-line input at all, so the field's own hint ("a blank line is the mount root") would be an
        //instruction the widget forbids; and a lone [""] renders as empty text, so a single-line box would show
        //only the hint and look identical to an unset field. Deciding per-value instead flips the widget between
        //the two shapes on the first keystroke, which deactivates it mid-edit and swallows the next key.
        const bool ForceMultiline = KeepEmpty;
        // An empty list gets a single line: a package's optional lists (submounts, base targets, args) are empty
        // far more often than not, and a stack of empty textareas is what made the node bodies tall and unreadable.
        const int Lines = (int)std::count(T.begin(), T.end(), '\n') + (T.empty() ? 0 : 1);
        ImGui::TextUnformatted(F.Label); ImGui::SameLine(kLabelCol);
        ImGui::SetNextItemWidth(kFieldWidth);
        //An emptied list ERASES the key rather than writing []. The two are not the same thing: BASE_TARGETS []
        //is a delta with no base and is refused outright, so clearing the box in the editor produced a node
        //the format rejects and the editor could not repair (the refusal says "omit it", and there was no way
        //to omit). drawEnvelope already erases WHEN/EXCLUDE this way; the list writer just never learned it.
        auto Write = [&](const std::string &Text) {
            json A = TextToList(Text, KeepEmpty);
            if (A.empty()) Node.erase(F.Key); else Node[F.Key] = std::move(A);
            m_s->MarkDirty();
        };
        if (Lines <= 1 && !ForceMultiline)
        {
            if (ImGui::InputTextWithHint("##v", F.Hint, &T)) Write(T);
        }
        else if (ImGui::InputTextMultiline("##v", &T, ImVec2(kFieldWidth, 16.0f * (float)std::min(Lines + 1, 6))))
            Write(T);
        break;
    }
    case FieldKind::KeyValue:
    {
        // Read-only view: rendering must not insert the container. Writing it in here is what stamped empty
        // objects onto every node in the bundle the first time anything was edited.
        static const json EmptyObj = json::object();
        const json &Map = (Node.contains(F.Key) && Node[F.Key].is_object()) ? Node[F.Key] : EmptyObj;
        ImGui::TextUnformatted(F.Label);
        std::string DelKey; std::pair<std::string, std::string> Rename;
        int Row = 0;
        for (const auto &[K, V] : Map.items())
        {
            // Identify the widget by ROW, not by the key being typed: keying on the text changed the widget's
            // id on every character, so imgui lost the active item and focus after each keystroke.
            ImGui::PushID(Row++);
            std::string Key = K, Val = V.is_string() ? V.get<std::string>() : V.dump();
            ImGui::SetNextItemWidth(120.0f);
            // ...and commit the rename only when the field is finished, so a half-typed name does not churn
            // the map (and erase the entry the moment it collides with an existing key).
            ImGui::InputText("##k", &Key);
            if (ImGui::IsItemDeactivatedAfterEdit() && Key != K && !Key.empty()
                && !Node[F.Key].contains(Key)) Rename = {K, Key};
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0f);
            if (!F.Options.empty())
            {
                const char *Shown = Val.c_str();
                for (const auto &O : F.Options) if (Val == O.first) Shown = O.second;
                if (ImGui::BeginCombo("##v", Shown))
                {
                    for (const auto &O : F.Options)
                        if (ImGui::Selectable(O.second, Val == O.first)) { Node[F.Key][K] = O.first; m_s->MarkDirty(); }
                    //An order the enum does not list (a multi-spec value) must stay editable, or picking
                    //anything silently discards it.
                    ImGui::Separator();
                    ImGui::SetNextItemWidth(150.0f);
                    std::string Free = Val;
                    if (ImGui::InputTextWithHint("##free", "custom", &Free) && Free != Val)
                    { Node[F.Key][K] = Free; m_s->MarkDirty(); }
                    ImGui::EndCombo();
                }
            }
            else if (ImGui::InputText("##v", &Val)) { Node[F.Key][K] = Val; m_s->MarkDirty(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) DelKey = K;
            ImGui::PopID();
        }
        if (!DelKey.empty()) { Node[F.Key].erase(DelKey); m_s->MarkDirty(); }
        if (!Rename.first.empty())
        {
            json V = Node[F.Key][Rename.first];
            Node[F.Key].erase(Rename.first);
            Node[F.Key][Rename.second] = V;
            m_s->MarkDirty();
        }
        if (ImGui::SmallButton("+ add"))
        { Node[F.Key][""] = F.Options.empty() ? "" : F.Options.front().first; m_s->MarkDirty(); }
        break;
    }
    case FieldKind::ObjArray:
    {
        //Not const: bound as a mutable reference so the draw loop below needs no second code path. It stays
        //empty because every loop over it is size-bounded and every WRITE goes through Node[F.Key].
        static json EmptyArr = json::array();
        const bool Present = Node.contains(F.Key) && Node[F.Key].is_array();
        json &Arr = Present ? Node[F.Key] : EmptyArr;
        ImGui::Text("%s (%d)", F.Label, (int)Arr.size());
        int Del = -1;
        //A batched payload is read capped — 77 BinaryPatches must not turn the node into a wall.
        const int Cap = 12;
        for (int I = 0; I < (int)Arr.size() && I < Cap; ++I)
        {
            ImGui::PushID(I);
            ImGui::Separator();
            for (const Field &S : F.Sub) drawField(Arr[I], S, Index);
            if (ImGui::SmallButton("remove")) Del = I;
            ImGui::PopID();
        }
        if ((int)Arr.size() > Cap) ImGui::TextDisabled("... and %d more (edit in the JSON view)", (int)Arr.size() - Cap);
        if (Del >= 0) { Arr.erase(Del); m_s->MarkDirty(); }
        if (ImGui::SmallButton("+ add entry"))
        {
            if (!Present) Node[F.Key] = json::array();      // materialise only when something is added
            json E = json::object();
            for (const Field &S : F.Sub) if (S.Kind == FieldKind::Enum && !S.Options.empty()) E[S.Key] = S.Options.front().first;
            Node[F.Key].push_back(std::move(E));
            m_s->MarkDirty();
        }
        break;
    }
    case FieldKind::RegEdits:
        drawRegEdits(Node, Index);
        break;
    case FieldKind::Cover:
    {
        //COVER is DUAL-FORM: a bare filename OR a {PATH, SOURCE} object. Both are legal (CoverCache handles
        //each, NodeLower accepts each), so the widget has to read BOTH — it read only the object form, so a
        //string cover rendered as an empty box, and the first keystroke wrote ["PATH"] into a string and threw.
        std::string P;
        if (Node.contains(F.Key))
        {
            if (Node[F.Key].is_object())      P = StrOf(Node[F.Key], "PATH");
            else if (Node[F.Key].is_string()) P = Node[F.Key].get<std::string>();
        }
        ImGui::TextUnformatted(F.Label); ImGui::SameLine(kLabelCol);
        ImGui::SetNextItemWidth(W - 60.0f);
        if (ImGui::InputTextWithHint("##v", "cover image in this bundle", &P))
        {
            //Keep whichever form the node already uses: promoting a string to an object would drop nothing
            //here, but it would change the package's bytes for a cosmetic edit.
            if (Node.contains(F.Key) && Node[F.Key].is_string()) Node[F.Key] = P;
            else                                                 Node[F.Key]["PATH"] = P;
            m_s->MarkDirty();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("browse")) m_s->Pending = {StrOf(Node, "NODE_ID"), "browse_cover"};
        break;
    }
    }
    ImGui::PopID();
}

void PkgCanvas::drawRegEdits(json &Node, int Index)
{
    (void)Index;
    static json EmptyEdits = json::array();      // see drawField/ObjArray: stays empty, writes go via Node
    const bool HasEdits = Node.contains("EDITS") && Node["EDITS"].is_array();
    json &Edits = HasEdits ? Node["EDITS"] : EmptyEdits;

    int DelEntry = -1;
    for (int E = 0; E < (int)Edits.size(); ++E)
    {
        ImGui::PushID(E);
        ImGui::Separator();
        // ARCHITECTURE is arrayable — the same tree written into several registry views.
        if (!Edits[E].is_object()) { ImGui::TextDisabled("(malformed entry - fix it in the JSON view)"); ImGui::PopID(); continue; }
        // Read without inserting: drawing a node must never dirty the document, or merely opening a package
        // rewrites every RegEdit on disk.
        json ArchRead = (Edits[E].contains("ARCHITECTURE") && Edits[E]["ARCHITECTURE"].is_array())
                            ? Edits[E]["ARCHITECTURE"] : json::array();
        const json &Arch = ArchRead;
        ImGui::TextUnformatted("views"); ImGui::SameLine(kLabelCol);
        for (const char *A : {"32", "64"})
        {
            bool On = false;
            for (const auto &X : Arch) if (X.is_string() && X.get<std::string>() == A) On = true;
            if (ImGui::Checkbox(A, &On))
            {
                json Next = json::array();
                for (const auto &X : Arch) if (!(X.is_string() && X.get<std::string>() == A)) Next.push_back(X);
                if (On) Next.push_back(A);
                Edits[E]["ARCHITECTURE"] = std::move(Next);
                m_s->MarkDirty();
            }
            ImGui::SameLine();
        }
        bool Ov = BoolOf(Edits[E], "OVERRIDE");
        if (ImGui::Checkbox("override pass", &Ov)) { Edits[E]["OVERRIDE"] = Ov; m_s->MarkDirty(); }

        // The registry IS a tree on disk; a human edits flat key paths. Round-trip through RegRows, but hold
        // the rows steady while a field is live (see RegBuf) and commit when it is finished.
        const std::string BufKey = StrOf(Node, "NODE_ID") + "#" + std::to_string(E);
        auto Buf = m_s->RegBuf.find(BufKey);
        std::vector<RegRow> Rows = (Buf != m_s->RegBuf.end()) ? Buf->second : RegRowsOf(Edits[E]);
        bool Commit = false, Editing = false;
        int DelRow = -1;
        for (int R = 0; R < (int)Rows.size(); ++R)
        {
            ImGui::PushID(R);
            auto Cell = [&](const char *Id, const char *Hint, std::string &Field, float W) {
                ImGui::SetNextItemWidth(W);
                ImGui::InputTextWithHint(Id, Hint, &Field);
                if (ImGui::IsItemActive()) Editing = true;
                if (ImGui::IsItemDeactivatedAfterEdit()) Commit = true;
            };
            Cell("##p", "HKLM\\Software\\...", Rows[R].Path, 150.0f);  ImGui::SameLine();
            const std::string WasName = Rows[R].Name, WasValue = Rows[R].Value;
            Cell("##n", "value", Rows[R].Name, 78.0f);                   ImGui::SameLine();
            Cell("##v", "data",  Rows[R].Value, 78.0f);                  ImGui::SameLine();
            //A "create this key, no values" row is still an editable row on screen — so the moment the author
            //types a name or a value into it, it STOPS being key-only. Without this the cells accepted the
            //keystrokes and RegRowsInto then dropped the whole row: the edit simply vanished.
            if (Rows[R].KeyOnly && (Rows[R].Name != WasName || Rows[R].Value != WasValue))
                Rows[R].KeyOnly = false;
            if (ImGui::SmallButton("x")) DelRow = R;
            ImGui::PopID();
        }
        if (DelRow >= 0) { Rows.erase(Rows.begin() + DelRow); Commit = true; }
        //"+ value" adds a row for a value the author is about to name. Until they do it is a KEY row, not an
        //empty DEFAULT value — writing the latter puts a spurious `@=""` into the prefix on every launch.
        if (ImGui::SmallButton("+ value"))
        { RegRow N{"HKLM\\Software\\", "", ""}; N.KeyOnly = true; Rows.push_back(std::move(N)); Commit = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("remove group")) DelEntry = E;
        if (Commit)
        {
            RegRowsInto(Edits[E], Rows);               // HasEdits is implied: the loop body only runs if it is
            m_s->MarkDirty();
            m_s->RegBuf.erase(BufKey);                 // re-read the (possibly reordered) tree next frame
        }
        else if (Editing) m_s->RegBuf[BufKey] = std::move(Rows);
        else m_s->RegBuf.erase(BufKey);
        ImGui::PopID();
    }
    if (DelEntry >= 0) { Edits.erase(DelEntry); m_s->MarkDirty(); }
    if (ImGui::SmallButton("+ group"))
    {
        if (!HasEdits) Node["EDITS"] = json::array();
        Node["EDITS"].push_back(json::object({{"ARCHITECTURE", json::array({"32"})}}));
        m_s->MarkDirty();
    }
}

void PkgCanvas::drawActions(json &Node, int Index, const Graph &G)
{
    const std::string Id = StrOf(Node, "NODE_ID");
    auto Run = m_s->Running.find(Id);
    if (Run != m_s->Running.end())
    {
        // Busy: the buttons are replaced by what is happening, so there is exactly one thing to look at.
        const PkgCanvasState::Busy &B = Run->second;
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", B.What.toUtf8().constData());
        if (B.Frac >= 0.0f)
            ImGui::ProgressBar(B.Frac, ImVec2(kFieldWidth, 14.0f),
                               B.Detail.isEmpty() ? nullptr : B.Detail.toUtf8().constData());
        else
        {
            //Indeterminate: a bar that sweeps rather than a fake percentage. An honest "working" beats a
            //progress number nothing is actually measuring.
            const float T = (float)ImGui::GetTime();
            const float P = 0.5f + 0.5f * std::sin(T * 3.0f);
            ImGui::ProgressBar(P, ImVec2(kFieldWidth, 14.0f), B.Detail.isEmpty() ? "working..." : B.Detail.toUtf8().constData());
        }
        if (B.Cancellable && ImGui::SmallButton("cancel")) emit cancelRequested(QString::fromStdString(Id));
        return;
    }

    const std::vector<std::string> &Hints = m_s->Hints[Id];
    const std::vector<PkgGraph::Action> Acts = PkgGraph::ActionsFor(Node, G, Index, Hints);
    if (Acts.empty()) return;
    ImGui::Separator();
    float Width = 0.0f;
    for (size_t I = 0; I < Acts.size(); ++I)
    {
        const float W = ImGui::CalcTextSize(Acts[I].Label).x + 14.0f;
        if (I && Width + W < kNodeWidth) ImGui::SameLine(); else Width = 0.0f;
        Width += W + 4.0f;
        //Recorded, not invoked: an action opens dialogs, and a nested Qt event loop inside an ImGui frame
        //lets the repaint timer re-enter NewFrame() with a node scope still open.
        if (ImGui::SmallButton(Acts[I].Label)) m_s->Pending = {Id, Acts[I].Id};
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Acts[I].Tip);
    }
}

// TOGGLE / WHEN / EXCLUDE belong to EVERY node regardless of TYPE, so they are drawn once here rather than
// repeated in ten per-type tables. Folded away when all three are at their defaults — which is the common case —
// and opened automatically when any is set, so a node that is conditional or off never hides the reason.
void PkgCanvas::drawEnvelope(json &Node)
{
    //THREE states, not two. ABSENT means "not user-toggleable at all"; "on" means "toggleable, starts on";
    //"off" means "toggleable, starts off" (ParseNode: Optional = !TOGGLE.empty()). Rendering absent and "on"
    //as the same thing, with "on" ERASING the key, silently converted a user-switchable module into a
    //permanently-on one — and left the combo still reading "on" afterwards. Eight shipping nodes are exactly
    //that shape (the Wipeout soundtrack and widescreen toggles, the NFSU2 extras, ...), and "on" was not
    //authorable from the canvas at all.
    const bool HasToggle     = Node.contains("TOGGLE") && Node["TOGGLE"].is_string();
    const std::string Toggle = HasToggle ? Node["TOGGLE"].get<std::string>() : std::string();
    const std::string When   = StrOf(Node, "WHEN");
    const bool HasExclude    = Node.contains("EXCLUDE") && Node["EXCLUDE"].is_array() && !Node["EXCLUDE"].empty();
    const bool Interesting   = HasToggle || !When.empty() || HasExclude;

    if (Interesting) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::TreeNodeEx("node options", ImGuiTreeNodeFlags_SpanAvailWidth)) return;

    const char *ToggleLabel = !HasToggle     ? "always on (not a toggle)"
                            : Toggle == "off" ? "toggle, starts OFF"
                                              : "toggle, starts on";
    ImGui::TextUnformatted("toggle"); ImGui::SameLine(kLabelCol);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::BeginCombo("##toggle", ToggleLabel))
    {
        if (ImGui::Selectable("always on (not a toggle)", !HasToggle))
        { Node.erase("TOGGLE"); m_s->MarkDirty(); }
        if (ImGui::Selectable("toggle, starts on", HasToggle && Toggle != "off"))
        { Node["TOGGLE"] = "on"; m_s->MarkDirty(); }
        if (ImGui::Selectable("toggle, starts OFF", HasToggle && Toggle == "off"))
        { Node["TOGGLE"] = "off"; m_s->MarkDirty(); }
        ImGui::EndCombo();
    }

    std::string W = When;
    ImGui::TextUnformatted("when"); ImGui::SameLine(kLabelCol);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputTextWithHint("##when", "condition - node is inert when false", &W))
    {
        if (W.empty()) Node.erase("WHEN"); else Node["WHEN"] = W;
        m_s->MarkDirty();
    }

    std::string Ex = ListToText(Node.contains("EXCLUDE") ? Node["EXCLUDE"] : json::array());
    ImGui::TextUnformatted("excludes"); ImGui::SameLine(kLabelCol);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputTextWithHint("##excl", "mutually-exclusive NODE_IDs", &Ex))
    {
        json A = TextToList(Ex);
        if (A.empty()) Node.erase("EXCLUDE"); else Node["EXCLUDE"] = std::move(A);
        m_s->MarkDirty();
    }
    ImGui::TreePop();
}

void PkgCanvas::drawPayload(json &Node, int Index)
{
    const std::string Type = StrOf(Node, "TYPE", "Group");
    const auto &Fields = FieldsFor(Type);
    if (Fields.empty()) { ImGui::TextDisabled("no payload - composition only"); return; }
    for (const Field &F : Fields)
    {
        //A delta's byte-bases are meaningless on any other FORM, and NodeLower now REFUSES them there — so
        //offering the box on every Content node is a two-click way to make a node that will not lower. The
        //refusal turned a silent no-op into a hard failure; leaving the trap in place would just relocate it.
        if (F.Key == std::string("BASE_TARGETS") && StrOf(Node, "FORM") != "delta") continue;
        drawField(Node, F, Index);
    }
}

// ---- node rendering -------------------------------------------------------

void PkgCanvas::drawNode(int Index, Graph &G)
{
    json &Node = m_s->Nodes()[Index];
    const std::string Type = StrOf(Node, "TYPE", "Group");
    const std::string Id   = StrOf(Node, "NODE_ID");

    int R, Gc, B;
    TypeColour(Type, R, Gc, B);
    ImNodes::PushColorStyle(ImNodesCol_TitleBar,         IM_COL32(R, Gc, B, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,  IM_COL32(R + 26, Gc + 26, B + 26, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(R + 40, Gc + 40, B + 40, 255));

    ImNodes::BeginNode(Index);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(Type.c_str());
    ImNodes::EndNodeTitleBar();

    // Nothing depends on this node yet, and it is not a launchable — so nothing mounts it. That is NORMAL
    // while authoring (you capture, then wire, then declare the exec last), so it is a note rather than an
    // error: the canvas makes the state visible instead of silently rewiring the graph to "fix" it.
    if (Type != "DeclareExec" && !m_s->HasDependent.count(Index))
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.68f, 1.0f), "not wired yet - nothing depends on this");

    // A node's problems are drawn ON the node, at the moment it becomes wrong.
    auto It = m_s->Issues.find(Id);
    if (It != m_s->Issues.end())
        for (const std::string &M : It->second)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f), "! %s", M.substr(0, 58).c_str());

    // Dependency pins: one IN accepting many parents, one OUT. Nothing flows along the wire — this is a
    // composition graph, not dataflow — so there is no per-column pin fan-out.
    ImNodes::BeginInputAttribute(InPin(Index));
    ImGui::TextUnformatted("depends on");
    ImNodes::EndInputAttribute();
    ImGui::SameLine();
    ImNodes::BeginOutputAttribute(OutPin(Index));
    ImGui::TextUnformatted("used by");
    ImNodes::EndOutputAttribute();

    ImGui::Dummy(ImVec2(kNodeWidth, 1.0f));
    std::string EditId = Id;
    ImGui::TextUnformatted("id"); ImGui::SameLine(kLabelCol);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (m_s->Running.find(Id) == m_s->Running.end()
        && ImGui::InputText("##id", &EditId) && !EditId.empty()) renameNode(Index, EditId);

    // A node running a heavy action is READ-ONLY: a zip conversion is rewriting the very file these fields
    // describe, so an edit landing mid-flight would describe a file that no longer exists.
    const bool Locked = m_s->Running.find(Id) != m_s->Running.end();
    if (Locked) ImGui::BeginDisabled();
    drawEnvelope(Node);
    drawPayload(Node, Index);
    if (Locked) ImGui::EndDisabled();
    drawActions(Node, Index, G);

    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();

    // Seed imnodes with this node's position the first time it is drawn, and re-seed if its index moved.
    // Previously this ran only for nodes with NO stored position, so a saved layout was never restored: every
    // node rendered at imnodes' default origin and the next mouse-release wrote [0,0] over the whole bundle.
    auto Sit = m_s->Seeded.find(Id);
    if (Sit == m_s->Seeded.end() || Sit->second != Index)
    {
        ImNodes::SetNodeGridSpacePos(Index, ImVec2(G.Nodes[Index].X, G.Nodes[Index].Y));
        m_s->Seeded[Id] = Index;
    }
}

void PkgCanvas::syncLinks(const Graph &G)
{
    // The link id must address the exact PARENTS entry it came from. G.Links SKIPS empty/non-string entries,
    // so a running counter over it drifts from the real array index and a detach would erase a different
    // parent. Recover the true index instead.
    m_s->LinkSlot.clear();
    for (const Link &L : G.Links)
    {
        //The link id must address the exact PARENTS entry the edge came from. Build already knows it, so it is
        //carried on the Link — recovering it here meant a linear scan of the child's PARENTS with a
        //std::string construction per comparison, EVERY FRAME: on the Minecraft bundle (39k links, 902 nodes
        //with 75 parents each) millions of allocations at 60Hz, which is the same shape already hoisted out of
        //DepthOf. An earlier attempt used a per-child running counter, which drifts past skipped entries and
        //silently dropped every wire after the first on a multi-parent node.
        //A slot at or past kMaxParents would alias into the NEXT child's id space, so ctrl-click-detaching
        //that wire would erase a PARENTS entry from a DIFFERENT node. The live maximum is 75, so this is
        //headroom rather than a live bug — but silence at the boundary is how it would stop being one.
        //Reported ONCE per node, not appended per frame: Issues is cleared only by setIssues, so an
        //unconditional push_back here grew the map by a duplicate string every frame at 60Hz.
        const int S = L.Slot;
        if (S >= kMaxParents)
        {
            std::vector<std::string> &Msgs = m_s->Issues[G.Nodes[L.ChildIndex].Id];
            const std::string M = "more than " + std::to_string(kMaxParents)
                                + " parents - later wires are not drawn";
            if (std::find(Msgs.begin(), Msgs.end(), M) == Msgs.end()) Msgs.push_back(M);
            continue;
        }
        const int From = (L.ParentIndex >= 0) ? OutPin(L.ParentIndex)
                                              : OutPin(kExternalBase + (int)(std::find(G.Externals.begin(),
                                                    G.Externals.end(), L.ExternalId) - G.Externals.begin()));
        ImNodes::Link(LinkId(L.ChildIndex, S), From, InPin(L.ChildIndex));
    }
}

void PkgCanvas::flushPositions(Graph &G)
{
    json &Ns = m_s->Nodes();
    for (int I = 0; I < (int)G.Nodes.size() && I < (int)Ns.size(); ++I)
    {
        // Only read back a node we have already seeded THIS session at THIS index. Reading imnodes before it
        // has been told where the node goes yields its default origin, which is how a layout got overwritten
        // with zeroes. A node that was never drawn (scrolled out on the first frame) is left alone.
        auto Sit = m_s->Seeded.find(G.Nodes[I].Id);
        if (Sit == m_s->Seeded.end() || Sit->second != I) continue;
        const ImVec2 P = ImNodes::GetNodeGridSpacePos(I);
        if (std::abs(P.x - G.Nodes[I].X) > 0.5f || std::abs(P.y - G.Nodes[I].Y) > 0.5f)
        {
            if (m_s->Layout) { SetPos(*m_s->Layout, G.Nodes[I].Id, P.x, P.y); m_s->PosDirty = true; }
        }
    }
}

// ---- frame ----------------------------------------------------------------

void PkgCanvas::drawToolbar()
{
    ImGui::TextUnformatted("add:");
    for (const std::string &T : AllTypes())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(T.c_str()))
        {
            const ImVec2 Origin = ImNodes::EditorContextGetPanning();
            m_s->Selected = addNode(T, 80.0f - Origin.x, 80.0f - Origin.y);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TypeHelp(T));
    }
    // Bring in a node from ANOTHER bundle so there is a chip to drag a wire from. Every package in the library
    // depends on at least one out-of-bundle node (a runner, MediaStack_MS, asiloader), so without this the
    // canvas cannot author a real package at all — which is what setKnownIds was always for.
    ImGui::SameLine();
    if (ImGui::SmallButton("external...")) ImGui::OpenPopup("##external");
    if (ImGui::BeginPopup("##external"))
    {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##extfilter", "filter", &m_s->ExternalFilter);
        const std::vector<std::string> Ids = m_s->KnownIds ? m_s->KnownIds() : std::vector<std::string>();
        std::set<std::string> Mine;
        for (const auto &N : m_s->Nodes()) Mine.insert(StrOf(N, "NODE_ID"));
        int Shown = 0;
        for (const std::string &Id : Ids)
        {
            if (Mine.count(Id)) continue;                    // already in this bundle: not external
            if (!m_s->ExternalFilter.empty() && Id.find(m_s->ExternalFilter) == std::string::npos) continue;
            if (++Shown > 40) { ImGui::TextDisabled("(narrow the filter)"); break; }
            if (ImGui::Selectable(Id.c_str())) { offerExternal(Id); ImGui::CloseCurrentPopup(); }
        }
        if (!Shown) ImGui::TextDisabled(m_s->KnownIds ? "nothing matches" : "(no catalog available)");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   |  drag from \"used by\" to \"depends on\"  -  ctrl-click a wire to detach  -  del removes a node");
}

void PkgCanvas::frame()
{
    if (!m_s->Doc) return;
    if (!m_s->CacheValid)
    {
        m_s->Cache = Build(m_s->Nodes(), m_s->Layout);
        m_s->HasDependent.clear();
        for (const Link &L : m_s->Cache.Links) if (L.ParentIndex >= 0) m_s->HasDependent.insert(L.ParentIndex);
        m_s->CacheValid = true;
    }
    Graph &G = m_s->Cache;

    ImGuiIO &IO = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(IO.DisplaySize);
    ImGui::Begin("##pkgcanvas", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawToolbar();
    ImGui::Separator();

    ImNodes::BeginNodeEditor();

    for (int I = 0; I < (int)G.Nodes.size(); ++I) drawNode(I, G);

    // Out-of-bundle parents: a reference chip, not a box we own — you cannot edit another package from here.
    std::vector<std::string> Chips = G.Externals;
    for (const std::string &X : m_s->OfferedExternals)
        if (std::find(Chips.begin(), Chips.end(), X) == Chips.end()) Chips.push_back(X);
    for (int E = 0; E < (int)Chips.size(); ++E)
    {
        const int Nid = kExternalBase + E;
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(58, 58, 64, 255));
        ImNodes::BeginNode(Nid);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextDisabled("external");
        ImNodes::EndNodeTitleBar();
        ImNodes::BeginOutputAttribute(OutPin(Nid));
        ImGui::TextUnformatted(Chips[E].c_str());
        ImNodes::EndOutputAttribute();
        ImNodes::EndNode();
        ImNodes::PopColorStyle();
    }

    syncLinks(G);
    ImNodes::MiniMap(0.18f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();

    // ---- interactions ----
    int StartAttr = 0, EndAttr = 0;
    if (ImNodes::IsLinkCreated(&StartAttr, &EndAttr))
    {
        const int PNode = PinNode(StartAttr), CNode = PinNode(EndAttr);
        if (CNode < kExternalBase)
        {
            if (PNode < kExternalBase) connect(PNode, CNode);
            else
            {
                // Dragged from an external chip: record the dependency by ID. The chip is a reference to a node
                // in another bundle, so there is no index to point at.
                const int E = PNode - kExternalBase;
                if (E >= 0 && E < (int)Chips.size()) connectExternal(Chips[E], CNode);
            }
        }
    }
    int DeadLink = 0;
    if (ImNodes::IsLinkDestroyed(&DeadLink))
    {
        const int Child = LinkChild(DeadLink), Slot = LinkSlot(DeadLink);
        json &Ns = m_s->Nodes();
        if (Child >= 0 && Child < (int)Ns.size() && Ns[Child].contains("PARENTS")
            && Slot < (int)Ns[Child]["PARENTS"].size())
        { Ns[Child]["PARENTS"].erase(Slot); m_s->MarkDirty(); }
    }

    int Sel = -1;
    if (ImNodes::NumSelectedNodes() == 1) { ImNodes::GetSelectedNodes(&Sel); if (Sel < kExternalBase) m_s->Selected = Sel; }
    // Delete drops the node AND every edge pointing at it, with no undo. Ask once — a mis-keyed Delete on a
    // shared library node silently unhooks every dependent, which is the failure this whole schema exists to
    // make visible rather than silent.
    //...and never while a conversion is in flight on it: the worker finishes against a node that no longer
    //exists, so it deletes the source it was replacing and drops the replacement on the floor.
    //BOUNDS-CHECKED. `Selected` survives a reload/invalidate, and nlohmann's non-const operator[](size_type)
    //FILLS THE ARRAY WITH NULLS up to the index — so a stale selection did not just read garbage, it grew the
    //document with nulls that SaveNodes would then write to disk, and threw on the read.
    if (m_s->Selected >= 0 && m_s->Selected < (int)m_s->Nodes().size()
        && ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::IsAnyItemActive()
        && !isBusy(StrOf(m_s->Nodes()[m_s->Selected], "NODE_ID")))
        m_s->ConfirmDelete = m_s->Selected;
    if (m_s->ConfirmDelete >= 0)
    {
        ImGui::OpenPopup("Delete node?");
        if (ImGui::BeginPopupModal("Delete node?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const int D = m_s->ConfirmDelete;
            const json &Ns2 = m_s->Nodes();
            const std::string DId = (D >= 0 && D < (int)Ns2.size()) ? StrOf(Ns2[D], "NODE_ID") : std::string();
            int Dependents = 0;
            for (const Link &L : G.Links) if (L.ParentIndex == D) ++Dependents;
            ImGui::Text("Delete '%s'?", DId.c_str());
            if (Dependents > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.28f, 1.0f),
                                   "%d node(s) depend on it - those wires are removed too.", Dependents);
            ImGui::Separator();
            if (ImGui::Button("Delete")) { removeNode(D); m_s->ConfirmDelete = -1; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { m_s->ConfirmDelete = -1; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        else m_s->ConfirmDelete = -1;   // dismissed with ESC / a click outside: do not reopen next frame
    }

    flushPositions(G);
    ImGui::End();

    // Persist on mouse-up rather than per keystroke: one write per gesture, and the JSON view never sees a
    // half-typed id.
    // The frame is closed. NOW it is safe to run an action that may open a modal.
    if (!m_s->Pending.first.empty())
    {
        const auto P = m_s->Pending;
        m_s->Pending = {};
        //ONE dispatch, never both. These actions delete files, so two live wires to the same dispatcher means a
        //host that wires both runs every conversion twice — the second against a source the first just removed.
        //The direct handler wins where it is set (that is the explicit opt-in); otherwise the signal.
        if (m_s->Action) m_s->Action(P.first, P.second);
        else emit nodeAction(QString::fromStdString(P.first), QString::fromStdString(P.second));
    }

    const bool Released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if ((m_s->Dirty && (Released || !ImGui::IsAnyItemActive())) || (m_s->PosDirty && Released))
    {
        m_s->Dirty = false; m_s->PosDirty = false;
        if (m_s->Save) m_s->Save();
        emit documentChanged();
    }
}
