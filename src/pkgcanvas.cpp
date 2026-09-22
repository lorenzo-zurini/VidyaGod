#include "pkgcanvas.h"

#include "commonutils.h"   // Log

//imnodes_internal.h pulls in imgui_internal.h, which insists on this before imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_stdlib.h"
#include "imnodes.h"
#include "imnodes_internal.h"   // GImNodes: the canvas rect and the live click interaction

#include <algorithm>
#include <cmath>
#include <map>
#include <cfloat>
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

//A tooltip raised from INSIDE the editor. ImGui places a tooltip at io.MousePos, which for the duration of the
//editor is the cursor inverse-transformed into world space — so at 0.5x the tooltip lands twice as far from the
//origin as the pointer that raised it and gets clamped into a corner of the screen, and at 3x it lands a third
//of the way back. A tooltip is screen furniture like the minimap, so it is placed at the REAL cursor. (The
//window it makes carries neither the Popup nor the ChildWindow flag, so imgui leaves its ParentWindow null and
//the nested-window walk below never sees it — this is the only thing that can fix it.)
void EditorTooltip(const ImVec2 &RealMouse, const ImVec2 &ScreenPos, const ImVec2 &ScreenSize, const char *Text)
{
    //The cursor is handed BACK for the duration of the call rather than the window being pinned. Pinning it
    //with SetNextWindowPos sets window_pos_set_by_api, which makes imgui skip both FindBestWindowPosForPopup
    //(the code that flips a tooltip to the other side of the cursor at a screen edge) and ClampWindowPos — so
    //a tip raised near the right or bottom edge ran off-screen with nothing to pull it back. Giving imgui the
    //real position instead lets all of that work exactly as it does everywhere else in the app.
    //The VIEWPORT goes back with it, for the same reason. While the editor lays out, the viewport is pointed
    //at the world region being drawn so that in-node popups flip and clamp in the space their widgets live in
    //— but a tooltip is screen furniture placed at the real cursor, so it has to be kept inside the real
    //screen. Handing both back for the duration of the call is the whole of it.
    ImGuiIO &Io = ImGui::GetIO();
    ImGuiViewport *VP = ImGui::GetMainViewport();
    const ImVec2 WasMouse = Io.MousePos, WasPos = VP->Pos, WasSize = VP->Size;
    Io.MousePos = RealMouse;
    VP->Pos = ScreenPos; VP->Size = ScreenSize;
    VP->WorkPos = ScreenPos; VP->WorkSize = ScreenSize;
    ImGui::SetTooltip("%s", Text);
    Io.MousePos = WasMouse;
    VP->Pos = WasPos; VP->Size = WasSize;
    VP->WorkPos = WasPos; VP->WorkSize = WasSize;
}

//The canvas borrows several pieces of global imgui state for the duration of the editor — the cursor position
//and delta it hands imnodes, a widened clip rect for submission, and the main viewport (so in-node popups
//place themselves in the space their widgets are laid out in) — and every one must be handed back on EVERY
//exit from frame(). This guard is that. imnodes' own canvas rectangle is imnodes state rather than imgui's
//and is restored beside the call that changes it. A throw escaping frame() still leaves imnodes mid-scope with
//its draw-list splitter split, which no RAII here can repair; the value is that what THIS file borrowed is
//always returned, and the viewport in particular feeds the backend's projection matrix.
struct EditorIoGuard
{
    ImGuiIO       &Io;
    ImGuiViewport *VP;
    ImVec2         Pos, Delta, VPos, VSize, VWorkPos, VWorkSize;
    bool           Clip = false;
    explicit EditorIoGuard(ImGuiIO &I)
        : Io(I), VP(ImGui::GetMainViewport()), Pos(I.MousePos), Delta(I.MouseDelta),
          VPos(VP->Pos), VSize(VP->Size), VWorkPos(VP->WorkPos), VWorkSize(VP->WorkSize) {}
    void popClip() { if (Clip) { ImGui::PopClipRect(); Clip = false; } }
    void restoreViewport()
    { VP->Pos = VPos; VP->Size = VSize; VP->WorkPos = VWorkPos; VP->WorkSize = VWorkSize; }
    //The viewport is in here rather than relying on "there is no early return between" — SetupDrawData takes
    //the backend's projection straight from it, so reaching Render() with it still pointed at the world region
    //draws the WHOLE application frame at the wrong scale. Restoring it twice (here and after EndNodeEditor)
    //costs four assignments; not restoring it once costs the frame.
    ~EditorIoGuard()
    {
        popClip();
        Io.MousePos = Pos; Io.MouseDelta = Delta;
        restoreViewport();
    }
    EditorIoGuard(const EditorIoGuard &) = delete;
    EditorIoGuard &operator=(const EditorIoGuard &) = delete;
};

//A node's size for the overview: its last MEASURED size, or the layout's estimate for one culling has never let
//on screen. The minimap draws every node, those included, and after a document swap that is all of them.
inline ImVec2 NodeSize(const std::map<std::string, ImVec2> &Dims, const PkgGraph::Node &N)
{
    const auto It = Dims.find(N.Id);
    if (It != Dims.end()) return It->second;
    //Nothing measured yet — after a document swap that is EVERY node, and a culled one is never measured at
    //all, so it would keep the placeholder forever. The layout already estimated this node's height from its
    //payload; using it means a 2950px BinaryPatch shows as a tall box in the overview rather than a stub.
    return ImVec2(330.0f, N.Height > 1.0f ? N.Height : 140.0f);
}

//One uniform node body, so the graph tiles predictably and long ids/paths scroll inside their field instead of
//stretching the box. The old editor's forms grew to their longest string and the canvas inherited that.
constexpr float kNodeWidth  = 330.0f;
//Node and field widths are CONSTANTS and must stay that way: zoom is a transform over the geometry imnodes
//emits, so scaling these as well multiplied the two together and nodes grew with the square of the zoom.
constexpr float kLabelCol   = 96.0f;
constexpr float kFieldWidth = 228.0f;

//A node-width separator. ImGui::Separator() DRAWS across the window's content-region (not the imnodes node's), so
//inside a node the line shot hundreds of px past the box — but its LAYOUT cost is width-0 + ItemSpacing.y, which the
//height estimator (pkggraph kSepPx) models exactly. So keep the real Separator (height + zoom-scaling unchanged) and
//just CLIP its drawing to kNodeWidth: the overrun is cut, nothing else moves.

//TOTAL readers. The editor loads raw JSON off disk — deliberately, since it is the tool you open to FIX a
//node the format rejects — so every field here may be any type. nlohmann's value() THROWS on a mismatch, and
//a throw out of frame() escapes with ImGui scopes still open: the next paint then segfaults on the half-open
//state. `"WHEN": true` and `"OVERRIDE": "true"` are both plausible slips, and NodeLower has a dedicated
//diagnostic telling the author to come here and fix exactly those.
std::string StrOf(const json &N, const char *Key, const std::string &Def = {})
{
    return (N.is_object() && N.contains(Key) && N[Key].is_string()) ? N[Key].get<std::string>() : Def;
}
// Model C: a node is IDENTIFIED for wiring by its stored "CID" handle — the CID it last minted to (or a stable
// "draft-…" handle for a node not yet published). This is the authoring handle PARENTS/LIBRARYITEM reference and
// everything (indexOf, canvas buffers, running state) keys on; it stays STABLE across an edit session (edits leave
// it stale-on-purpose until Publish re-mints + write-backs). LABEL is PURELY COSMETIC — the pretty display name.
std::string Handle(const json &N) { return StrOf(N, "CID"); }
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

//Zoom bounds: below ~0.2 a node is a smudge, above ~3 one node fills the screen and panning is easier.
static constexpr float kMinZoom = 0.2f;
static constexpr float kMaxZoom = 3.0f;

struct PkgCanvasState
{
    //Viewport culling + the minimap. They used to be in tension — imnodes' minimap re-drew the WHOLE graph
    //from the nodes submitted, so it cost as much again as the graph and could not coexist with culling. The
    //minimap is ours now and draws from G.Nodes, so culling is unconditional, the minimap is always available,
    //and the toolbar reports how much of the graph is actually on screen.
    //Zoom, done as a VIEW TRANSFORM: imnodes is handed unscaled world coordinates and an unscaled style, and
    //the vertices it emits are scaled about the canvas origin afterwards. Nothing the document or the layout
    //holds is ever in a zoomed unit, so looking at the graph cannot edit it — by construction, not by a guard.
    float Zoom        = 1.0f;
    //The wheel sets a TARGET and the view WALKS to it. A notch is a 10% multiplicative jump applied whole,
    //which reads as a teleport; easing over ~120 ms reads as zooming. Kept separate from Zoom so that
    //setZoom() — the toolbar, a test, "reset" — still lands instantly, where an animation would be a lie.
    float ZoomTarget  = 1.0f;
    bool  Zooming     = false;
    //setZoom happens outside a frame, where the viewport is not known; the recentre it wants is done next
    //frame, and needs the scale it came from as well as the one it went to.
    float RecentreFromZoom = 0.0f;   // the zoom the pending recentre is coming FROM; 0 = none pending
    //The anchor the easing holds still: a screen offset from the canvas origin, and the WORLD point that was
    //under it when the gesture started. The pan is re-solved from these EVERY eased frame — easing the zoom
    //alone would let the point under the cursor drift away while the animation runs, which is the one thing a
    //cursor-anchored zoom must not do.
    ImVec2 ZoomAnchor{0, 0};
    ImVec2 ZoomAnchorWorld{0, 0};
    //Bounds of the transformed surface (min x, min y, max x, max y) from the last frame. Computed while the
    //view transform walks the vertices, so it costs nothing, and it is the only way to observe the transform
    //from outside — imnodes' own reported sizes are deliberately unscaled now.
    ImVec4 SurfaceBounds{0, 0, 0, 0};
    //Last frame's minimap rectangle, the widest clip rect the canvas drew through, and the canvas viewport.
    //All three are screen-space furniture that zoom must not move; recorded so a test can say so.
    ImVec4 MiniMapRect{0, 0, 0, 0};
    ImVec4 SurfaceClip{0, 0, 0, 0};
    ImVec4 ViewportRect{0, 0, 0, 0};
    int    SurfaceVertices = 0;
    //The rectangle the overview drew for each node this frame, by node index.
    std::vector<ImVec4> MiniBoxes;
    //The rectangle in-node popups were placed against last frame (position + size), in the editor's own
    //units: the region being laid out, enlarged to fit a dropdown. Never the screen.
    ImVec4 PopupExtent{0, 0, 0, 0};
    //The main viewport and its work rect at the point post-editor windows are submitted — the minimap child
    //and the delete-confirmation modal. Both must be back in SCREEN space by then.
    ImVec4 PostEditorViewport{0, 0, 0, 0};
    ImVec4 PostEditorWorkRect{0, 0, 0, 0};
    //The cursor position actually handed to the editor. Zoom scales the emitted geometry, so input must be
    //inverse-transformed on the way IN or every click lands where the node would have been drawn unzoomed.
    //Recorded because that mapping is otherwise invisible — and a click landing on the wrong node is the
    //quietest way for this whole approach to be wrong.
    ImVec2 EditorMouse{0, 0};
    //The REAL cursor, kept for the things inside the editor that are screen furniture rather than content —
    //a tooltip is placed by imgui at io.MousePos, which in there is the world cursor.
    ImVec2 RealMouse{0, 0};
    //The real screen viewport, kept for the same reason: while the editor runs the viewport is pointed at the
    //world region being laid out, and screen furniture raised from inside it needs the real one back.
    ImVec2 ScreenViewportPos{0, 0};
    ImVec2 ScreenViewportSize{0, 0};
    //imnodes DESTROYS every node not submitted during a frame (ObjectPoolUpdate) and re-creates it at
    //Origin(0,0) the next time it is begun. With viewport culling that happens constantly, so "have I ever
    //seeded this node" is the wrong question — the right one is "was it submitted LAST frame", which is the
    //only thing that says whether imnodes still remembers where it goes.
    std::set<std::string> DrawnLast;
    //Last frame's selected node indices — read after EndNodeEditor, where asking imnodes is legal.
    std::set<int>         SelectedLast;
    bool  ShowMiniMap = true;
    int  VisibleNodes = 0;
    int  VisibleLinks = 0;   // links actually submitted this frame (incl. via a crossing-link proxy)
    //Measured node sizes by NODE_ID. The minimap draws the WHOLE graph, including the nodes culling never
    //submitted — and a node that was never submitted has no size imnodes can be asked for. Whatever was
    //measured the last time it WAS on screen is kept here; a node never yet seen falls back to a nominal box.
    std::map<std::string, ImVec2> NodeDims;
    //Node ids already warned about an impossible position — the warning is worth one line, not one per frame.
    //Keyed "<id>@<source>@<value>", so the only thing ever suppressed is a message identical to one already
    //printed. Maintained by removeNode and renameNode BY PREFIX — the composite shape is why erasing a bare id
    //silently did nothing — and deliberately NOT cleared by invalidateGraph, which runs on every keystroke.
    std::set<std::string> WarnedPos;

    json *Doc = nullptr;
    //The canvas-position sidecar (NODE_ID -> [x,y]), owned by the model and persisted to
    //GlobalConfig under EDITORLAYOUT, keyed by bundle path. Never the document: see PkgGraph::Build
    //(a node's own POS is the published default). Null = positions are not persisted.
    json *Layout = nullptr;
    PkgCanvas::SaveFn Save;
    PkgCanvas::SaveFn SaveLayoutOnly;   // used when a frame changed ONLY positions
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
    std::vector<std::string> OfferedExternals;               // chips placed by the picker, not yet wired (by handle)
    std::map<std::string, std::string> ExternalLabels;       // handle → pretty label, for chip display (from the picker)
    std::string ExternalFilter;
    float SidePanel = 360.0f;
    std::string AddType = "VFSLayer";

    json &Nodes()
    {
        if (!Doc->contains("NODES") || !(*Doc)["NODES"].is_array()) (*Doc)["NODES"] = json::array();
        return (*Doc)["NODES"];
    }
};

PkgCanvas::PkgCanvas(json *doc, SaveFn save, QObject *parent, json *layout, SaveFn saveLayoutOnly)
    : QObject(parent), m_s(std::make_unique<PkgCanvasState>())
{
    m_s->Doc = doc;
    m_s->Layout = layout;
    m_s->Save = std::move(save);
    m_s->SaveLayoutOnly = std::move(saveLayoutOnly);
}

PkgCanvas::~PkgCanvas() = default;

//Dropping imnodes' own selection, but only where there IS an imnodes context. invalidateGraph runs from
//model-level paths (a reload, a rebuild) that can happen with no canvas realised at all — calling into
//imnodes there dereferences a null context and takes the process down, which is how test_packageeditormodel
//started segfaulting.
void PkgCanvas::clearSelection()
{
    m_s->Selected = -1;
    m_s->SelectedLast.clear();
    if (m_s->Ctx) ImNodes::ClearNodeSelection();
}

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
    clearSelection();
    m_s->ConfirmDelete = -1;
    //...and imnodes' own selection, plus the snapshot culling reads. Clearing only our copy left the stale
    //INDEX in both: the next frame force-draws every index in SelectedLast (a selected node is never culled),
    //which makes Drawn[stale] true — so the Drawn guard, the only thing between a stale index and Selected,
    //passes precisely BECAUSE of the force-draw, and Selected is restored to an index that now addresses a
    //different node.
    //
    //And the measured sizes, for the same reason removeNode drops them: this is also the path a whole
    //DOCUMENT SWAP takes, and auto-generated ids repeat across packages ("content", "group", "declareexec"),
    //so a second package's nodes would inherit the first one's boxes in the overview. Unlike the maps above
    //this one grows without bound, because nothing else ever removes an entry on this path.
    m_s->NodeDims.clear();

    //AND THE SEED MAP. This is the document-REPLACEMENT path — the JSON view's Save, PackageEditor's
    //LoadNodes — so a node's declared POS can be different on the other side of it, while imnodes is still
    //holding the position from before. seedNodePosition re-pushes only when the index moved or the node was
    //not drawn last frame, and a reload changes neither: so imnodes kept the stale origin, flushPositions
    //compared it against the freshly-built graph, decided the node had been DRAGGED there, wrote the old
    //coordinate into the layout sidecar and marked it for saving. Editing a POS in the JSON view therefore
    //reverted itself and persisted the value it had just replaced — into GlobalConfig, which outranks the
    //package's own POS for the rest of that bundle's life. Clearing this makes the next frame push every
    //drawn node from the rebuilt cache, which is what "the document changed underneath you" has to mean.
    //
    //Safe for a drag, because a drag does not come through here: the per-keystroke and per-edit path is
    //MarkDirty(), which only drops CacheValid. This function has exactly one caller (PackageEditor::BuildUI).
    m_s->Seeded.clear();
    //NOT the warned-about set. The reason previously given for that — "this runs on every keystroke" — is
    //false (that is MarkDirty); the real one is that its key carries the VALUE, so the only thing a stale
    //entry can ever suppress is a message character-identical to one already printed. Clearing it here would
    //buy a duplicate of that same line every time the editor rebuilds its UI, and nothing else.
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
        if (Handle((*m_s->Doc)["NODES"][I]) == nodeId) return I;
    return -1;
}

Graph PkgCanvas::graph() const { return Build(m_s->Nodes(), m_s->Layout); }

float PkgCanvas::zoom() const { return m_s->Zoom; }
void  PkgCanvas::setZoom(float Z)
{
    const float New = std::clamp(Z, kMinZoom, kMaxZoom);
    //An explicit set is not a gesture: it lands on the frame it is asked for, and cancels any easing in
    //flight (otherwise "reset" would be overridden by the tail of the wheel motion that prompted it).
    m_s->ZoomTarget = New;
    m_s->Zooming    = false;
    if (New == m_s->Zoom) return;
    const float Old = m_s->Zoom;
    m_s->Zoom = New;
    //Anchored on the middle of the viewport, applied on the next frame because the viewport is only known
    //there. Without it "reset" from 3x left the pan untouched and the view landed on a different part of the
    //graph than the one you were looking at. The zoom it is coming FROM has to be carried across — solving for
    //the new pan needs both scales, and an earlier version that used the new one twice reduced to Pan = Pan
    //and silently did nothing at all. (No Seeded.clear() any more: it carried a comment claiming every pushed
    //position was in the old scale, which stopped being true when zoom became a view transform — imnodes holds
    //world coordinates. All it did was re-push N positions and stomp an in-flight drag.)
    //Only if nothing is already pending. Two setZoom calls before a frame would otherwise leave the SECOND
    //call's "from" against a pan that still belongs to the FIRST one's scale: measured at 203px of drift for
    //setZoom(2) followed by setZoom(0.5) in the same frame, against 0px when a frame runs between them.
    if (m_s->RecentreFromZoom <= 0.0f) m_s->RecentreFromZoom = Old;
}
std::string PkgCanvas::selectedNodeId() const
{
    auto &Ns = m_s->Nodes();
    if (m_s->Selected < 0 || m_s->Selected >= (int)Ns.size()) return std::string();
    return Ns[m_s->Selected].value("LABEL", std::string());
}

//A live JSON edit already persisted the change; the canvas only needs to rebuild its cached graph from the doc,
//keeping the selection index (a content edit does not move the node). MarkDirty is per-frame private; this is its
//public, selection-preserving cousin for an external editor.
void PkgCanvas::refreshFromDocument() { m_s->CacheValid = false; }

int  PkgCanvas::visibleNodes() const { return m_s->VisibleNodes; }
int  PkgCanvas::visibleLinks() const { return m_s->VisibleLinks; }
void PkgCanvas::editorMouse(float &X, float &Y) const { X = m_s->EditorMouse.x; Y = m_s->EditorMouse.y; }

void PkgCanvas::surfaceBounds(float &MinX, float &MinY, float &MaxX, float &MaxY) const
{
    MinX = m_s->SurfaceBounds.x; MinY = m_s->SurfaceBounds.y;
    MaxX = m_s->SurfaceBounds.z; MaxY = m_s->SurfaceBounds.w;
}
bool PkgCanvas::miniMap() const      { return m_s->ShowMiniMap; }
void PkgCanvas::setMiniMap(bool On)  { m_s->ShowMiniMap = On; }
void PkgCanvas::miniMapRect(float &MinX, float &MinY, float &MaxX, float &MaxY) const
{ MinX = m_s->MiniMapRect.x; MinY = m_s->MiniMapRect.y; MaxX = m_s->MiniMapRect.z; MaxY = m_s->MiniMapRect.w; }
void PkgCanvas::surfaceClip(float &MinX, float &MinY, float &MaxX, float &MaxY) const
{ MinX = m_s->SurfaceClip.x; MinY = m_s->SurfaceClip.y; MaxX = m_s->SurfaceClip.z; MaxY = m_s->SurfaceClip.w; }
//How many vertices the canvas emitted last frame. The only direct evidence that a node's CONTENTS were drawn
//rather than culled — a node whose fields imgui threw away still leaves its box behind, so every bound and
//size looks perfectly healthy while the box is empty.
int PkgCanvas::surfaceVertices() const { return m_s->SurfaceVertices; }
int PkgCanvas::cachedNodeSizes() const { return (int)m_s->NodeDims.size(); }
void PkgCanvas::popupExtent(float &X, float &Y, float &W, float &H) const
{ X = m_s->PopupExtent.x; Y = m_s->PopupExtent.y; W = m_s->PopupExtent.z; H = m_s->PopupExtent.w; }
void PkgCanvas::postEditorViewport(float &X, float &Y, float &W, float &H, float &WX, float &WY,
                                   float &WW, float &WH) const
{
    X = m_s->PostEditorViewport.x; Y = m_s->PostEditorViewport.y;
    W = m_s->PostEditorViewport.z; H = m_s->PostEditorViewport.w;
    WX = m_s->PostEditorWorkRect.x; WY = m_s->PostEditorWorkRect.y;
    WW = m_s->PostEditorWorkRect.z; WH = m_s->PostEditorWorkRect.w;
}
void PkgCanvas::miniMapNodeBox(int Index, float &X, float &Y, float &W, float &H) const
{
    X = Y = W = H = 0.0f;
    if (Index < 0 || Index >= (int)m_s->MiniBoxes.size()) return;
    const ImVec4 &R = m_s->MiniBoxes[(size_t)Index];
    X = R.x; Y = R.y; W = R.z - R.x; H = R.w - R.y;
}
void PkgCanvas::canvasViewport(float &MinX, float &MinY, float &MaxX, float &MaxY) const
{ MinX = m_s->ViewportRect.x; MinY = m_s->ViewportRect.y; MaxX = m_s->ViewportRect.z; MaxY = m_s->ViewportRect.w; }
//VALIDATED: this is public, and a selection is an INDEX into a document that can be replaced underneath it.
void PkgCanvas::selectNode(int index)
{
    m_s->Selected = (index >= 0 && index < (int)m_s->Nodes().size()) ? index : -1;
}

int PkgCanvas::addNode(const std::string &type, float x, float y)
{
    json N = NewPayload(type);
    // A readable default cosmetic name (LABEL). LABEL is cosmetic now, so it need NOT be unique — the counter just
    // keeps freshly-dropped nodes visually distinct until the author names them.
    std::string Base = type == "Group" ? "group" : type;
    for (char &C : Base) C = (char)std::tolower((unsigned char)C);
    std::string Label = Base;
    for (int K = 2; ; ++K)
    {
        bool Taken = false;
        for (const auto &Nn : m_s->Nodes()) if (StrOf(Nn, "LABEL") == Label) { Taken = true; break; }
        if (!Taken) break;
        Label = Base + "_" + std::to_string(K);
    }
    // A GLOBALLY-unique, stable draft HANDLE. A new node has no CID until the next Publish mints it (the cascade is
    // fully deferred): the draft handle is what PARENTS reference meanwhile and Publish remaps to the real CID via
    // HandleToCid + StampNodeCids. It MUST be globally unique (publish is library-wide) — a per-bundle counter would
    // give every fresh bundle the same handle and cross-wire them at the first mint. Stored in "CID" (stripped at
    // freeze, so it never ships). The retry loop guards the astronomically-unlikely in-bundle collision.
    std::string Draft;
    do { Draft = MakeDraftHandle(); } while (indexOf(Draft) >= 0);
    json Out = json::object({{"PARENTS", json::array()}});
    for (const auto &[K, V] : N.items()) Out[K] = V;
    Out["CID"] = Draft;
    if (!Out.contains("LABEL")) Out["LABEL"] = Label;
    if (m_s->Layout) { SetPos(*m_s->Layout, Draft, x, y); m_s->PosDirty = true; }
    m_s->Nodes().push_back(std::move(Out));
    m_s->MarkDirty();
    return (int)m_s->Nodes().size() - 1;
}

bool PkgCanvas::removeNode(int index)
{
    json &Ns = m_s->Nodes();
    if (index < 0 || index >= (int)Ns.size()) return false;
    const std::string Id = Handle(Ns[index]);
    //Deleting shifts every later index, so every seeded position is now against the wrong node. Drop them all
    //and let the next frame re-seed from the layout sidecar.
    m_s->Seeded.clear();
    //...and take this node's canvas state with it, or a later node reusing the id inherits a stale position,
    //a stale busy state and stale registry edit buffers.
    if (m_s->Layout && m_s->Layout->is_object()) { m_s->Layout->erase(Id); m_s->PosDirty = true; }
    m_s->Running.erase(Id);
    m_s->Hints.erase(Id);
    m_s->Seeded.erase(Id);
    m_s->NodeDims.erase(Id);
    //By PREFIX: the key is "<id>@<source>@<value>", the same shape RegBuf uses below, so erasing the bare id
    //removes nothing at all. An id containing '@' can reach another node's keys through this — the same
    //hazard the RegBuf loop below carries for '#'. Accepted for a suppression cache (the cost is a warning
    //printed twice or once too few) where it would not be for the position and buffer maps.
    for (auto It = m_s->WarnedPos.begin(); It != m_s->WarnedPos.end(); )
        It = (It->rfind(Id + "@", 0) == 0) ? m_s->WarnedPos.erase(It) : std::next(It);
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
    //imnodes' own selection set stores INDICES, which every later node's index has just shifted under, and it
    //never validates them: leaving it populated makes the next frame report a selection for whichever node
    //inherited the index — and since a selected node is never culled, that wrong index can never be culled
    //away either, so it persists for the rest of the session.
    clearSelection();
    m_s->MarkDirty();
    return true;
}

bool PkgCanvas::connect(int parentIndex, int childIndex)
{
    json &Ns = m_s->Nodes();
    if (parentIndex < 0 || childIndex < 0 || parentIndex >= (int)Ns.size() || childIndex >= (int)Ns.size()) return false;
    if (parentIndex == childIndex) return false;
    const std::string Pid = Handle(Ns[parentIndex]);
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
    const std::string Pid = Handle(Ns[parentIndex]);
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
    if (Handle(Ns[childIndex]) == parentId) return false;
    if (!Ns[childIndex].contains("PARENTS") || !Ns[childIndex]["PARENTS"].is_array())
        Ns[childIndex]["PARENTS"] = json::array();
    for (const auto &P : Ns[childIndex]["PARENTS"]) if (P.is_string() && P.get<std::string>() == parentId) return false;
    Ns[childIndex]["PARENTS"].push_back(parentId);
    m_s->MarkDirty();
    return true;
}

void PkgCanvas::offerExternal(const std::string &parentId, const std::string &label)
{
    if (parentId.empty()) return;
    auto &O = m_s->OfferedExternals;
    if (std::find(O.begin(), O.end(), parentId) == O.end()) O.push_back(parentId);
    if (!label.empty()) m_s->ExternalLabels[parentId] = label;   // remember the pretty name for the chip
}

bool PkgCanvas::renameNode(int index, const std::string &newId)
{
    json &Ns = m_s->Nodes();
    if (index < 0 || index >= (int)Ns.size()) return false;
    //A NODES entry that is not an object has no LABEL to set, and writing one through operator[](string) throws
    //type_error.305 — out of frame(), out of paintGL, which has no catch. Build keeps such an entry as a
    //placeholder so indices stay aligned, so this is reachable from any malformed or peer-authored package.
    if (!Ns[index].is_object()) return false;
    // Model C: "rename" sets the node's COSMETIC LABEL. It is NOT the wiring handle — the handle is the node's CID
    // (derived, not user-set) — so NOTHING else moves: no reference re-pointing (refs are CIDs, unchanged by a
    // rename), and no canvas position / Seeded / Running / Hints / NodeDims / RegBuf / WarnedPos migration (all keyed
    // by the stable handle, which the rename never touches). A blank label is allowed (the node then shows its short
    // CID); duplicate labels are allowed (LABEL is cosmetic — RoC/TFT "v1.21b" are legitimate namesakes).
    if (StrOf(Ns[index], "LABEL") == newId) return false;   // no-op (also stops a per-keystroke rebuild storm)
    Ns[index]["LABEL"] = newId;
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
        //A value of the WRONG SHAPE is shown, not hidden. ListToText yields "" for anything that is not an
        //array AND silently drops any entry that is not a string, so a hand-written `"ARGS": "not a list"` or
        //`"ARGS": [5]` drew an EMPTY box — identical to an unset field — and the first character typed into it
        //replaced the value. That is the same destruction the KeyValue and Cover writers were taught to
        //refuse, arriving through a widget instead of a button, and it is worse here because the editor is the
        //tool you open to REPAIR such a node: it showed you nothing was wrong.
        //
        //ELEMENTS as well as the container, because the round trip is what destroys: ListToText drops the
        //entry, the author types, TextToList writes back what is left, and the dropped entry is gone. A list
        //of the right shape with one wrong entry is not a lesser case of this — it is the likelier one.
        const json *Val = (Node.is_object() && Node.contains(F.Key)) ? &Node[F.Key] : nullptr;
        //THE SHARED PREDICATE, not a copy of it. PkgGraph::FieldPx has to charge one line for exactly the
        //values this draws one line for, and when this scan lived only here the two disagreed by 163px on a
        //list with one bad entry — a hole reserved in the POS stamped into the package at publish.
        const int BadAt = PkgGraph::StringListFault(Val);
        if (BadAt != PkgGraph::kStringListOk)
        {
            //DESCRIBED, never dumped. This runs on every frame of every visible node, and a package from a
            //content source can carry megabytes in any field: `dump()` here is that many bytes allocated and
            //measured sixty times a second, and the resulting single unwrapped line drew the node clean across
            //the column to its right. Same hazard PkgGraph::Build refuses for the same reason, one path hotter.
            const std::string What = (BadAt >= 0)
                ? "entry " + std::to_string(BadAt) + " is " + PkgGraph::DescribeValue(Val->at((size_t)BadAt))
                : PkgGraph::DescribeValue(*Val);   // kStringListNotAList: describe the value itself
            ImGui::TextUnformatted(F.Label); ImGui::SameLine(kLabelCol);
            ImGui::TextDisabled("%s", What.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BadAt >= 0
                                      ? "Every entry of this list has to be a string. Fix it in the JSON view."
                                      : "This is not a list. Fix it in the JSON view - editing here would "
                                        "replace it.");
            break;
        }
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
            if (PkgGraph::WritableObject(Node, F.Key))
            {
                Node[F.Key].erase(Rename.first);
                if (PkgGraph::WriteSubKey(Node, F.Key, Rename.second, V)) m_s->MarkDirty();
            }
        }
        if (ImGui::SmallButton("+ add")
            && PkgGraph::WriteSubKey(Node, F.Key, "", F.Options.empty() ? "" : F.Options.front().first))
            m_s->MarkDirty();
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
            //A batched entry that is not an OBJECT is malformed content, and this editor exists to open
            //malformed content. Drawing its fields anyway means the first write — `Node[F.Key] = V` on a JSON
            //string — throws type_error.305 out of paintGL, which has no catch: one click on a MODE dropdown
            //terminates the app. drawRegEdits already guards exactly this shape; this arm did not.
            if (!Arr[I].is_object())
            {
                ImGui::TextDisabled("(malformed entry - fix it in the JSON view)");
                if (ImGui::SmallButton("remove")) Del = I;
                ImGui::PopID();
                continue;
            }
            for (const Field &S : F.Sub) drawField(Arr[I], S, Index);
            if (F.VarUI) drawCustomVarUI(Arr[I]);   // a batched CustomVar entry carries its own launcher UI facet
            if (ImGui::SmallButton("remove")) Del = I;
            ImGui::PopID();
        }
        if ((int)Arr.size() > Cap) ImGui::TextDisabled("... and %d more (edit in the JSON view)", (int)Arr.size() - Cap);
        if (Del >= 0) { Arr.erase(Del); m_s->MarkDirty(); }
        //Refuses rather than overwrites when the value is there but is not an array. The Cover and KeyValue
        //writers next door were taught to refuse a malformed value in the same sweep that left this one
        //destroying it: a hand-edited object here was replaced by an empty array on one click, MarkDirty'd,
        //and saved — silent data loss in the editor you opened to REPAIR the package.
        const bool Replaceable = !Node.contains(F.Key) || Node[F.Key].is_null() || Node[F.Key].is_array();
        if (!Replaceable) ImGui::TextDisabled("(this field is not a list - fix it in the JSON view)");
        else if (ImGui::SmallButton("+ add entry"))
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
            //The `else` used to be unconditional, so a COVER that was a number, an array or a bool threw on
            //the FIRST KEYSTROKE — no click needed, and the comment above stopped at the string case.
            if (Node.contains(F.Key) && Node[F.Key].is_string()) { Node[F.Key] = P; m_s->MarkDirty(); }
            else if (PkgGraph::WriteSubKey(Node, F.Key, "PATH", P))          m_s->MarkDirty();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("browse")) m_s->Pending = {Handle(Node), "browse_cover"};
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
        const std::string BufKey = Handle(Node) + "#" + std::to_string(E);
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
    //Same refusal: a hand-edited `"EDITS": {"HKLM": {...}}` lost its entire registry tree to one click here.
    const bool EditsReplaceable = !Node.is_object() || !Node.contains("EDITS")
                               || Node["EDITS"].is_null() || Node["EDITS"].is_array();
    if (!EditsReplaceable) ImGui::TextDisabled("(EDITS is not a list - fix it in the JSON view)");
    else if (ImGui::SmallButton("+ group"))
    {
        if (!HasEdits) Node["EDITS"] = json::array();
        Node["EDITS"].push_back(json::object({{"ARCHITECTURE", json::array({"32"})}}));
        m_s->MarkDirty();
    }
}

void PkgCanvas::drawActions(json &Node, int Index, const Graph &G)
{
    const std::string Id = Handle(Node);
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
        if (ImGui::IsItemHovered()) EditorTooltip(m_s->RealMouse, m_s->ScreenViewportPos, m_s->ScreenViewportSize, Acts[I].Tip);
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
    if (!ImGui::TreeNodeEx("node options")) return;   // no SpanAvailWidth: it spans the WINDOW, overrunning the node

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
    if (ImGui::InputTextWithHint("##excl", "mutually-exclusive LABELs", &Ex))
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
    //A CustomVar's UI facet is now drawn PER VARS ENTRY inside the ObjArray loop (Field::VarUI), not per node.
}

//The CustomVar UI facet — the thing whose PRESENCE makes the var user-facing (08-variables.md). The generic field
//table is flat (KEY→one JSON key) and can't express a nested object that toggles in and out, so it is drawn here,
//the way RegEdit's hive is. "Visible" is the presence of the UI object; unchecking it removes UI (→ hidden, the var
//resolves from DEFAULT). This restores editor control over visibility, which was JSON-only after the refactor.
void PkgCanvas::drawCustomVarUI(json &Node)
{
    ImGui::PushID("uifacet");
    bool Visible = Node.contains("UI") && Node["UI"].is_object();
    if (ImGui::Checkbox("Visible in launcher", &Visible))
    {
        PkgGraph::SetVarVisible(Node, Visible);
        m_s->MarkDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shown as a control in the pre-launch dialog. Off = internal binding: it resolves from "
                          "DEFAULT (or another var) and never appears.");
    if (!Visible) { ImGui::PopID(); return; }

    json &UI = Node["UI"];
    auto TextRow = [&](const char *Key, const char *Label, const char *Hint) {
        std::string V = UI.value(Key, std::string());
        ImGui::TextUnformatted(Label); ImGui::SameLine(kLabelCol); ImGui::SetNextItemWidth(kFieldWidth);
        if (ImGui::InputTextWithHint((std::string("##") + Key).c_str(), Hint, &V)) { UI[Key] = V; m_s->MarkDirty(); }
    };
    TextRow("LABEL", "Label", "shown in the dialog");

    static const char *Controls[] = {"text", "bool", "int", "float", "enum", "secret"};
    std::string Ctl = UI.value("CONTROL", std::string("text"));
    ImGui::TextUnformatted("Control"); ImGui::SameLine(kLabelCol); ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::BeginCombo("##ctl", Ctl.c_str()))
    {
        for (const char *C : Controls) if (ImGui::Selectable(C, Ctl == C)) { UI["CONTROL"] = std::string(C); m_s->MarkDirty(); }
        ImGui::EndCombo();
    }
    Ctl = UI.value("CONTROL", std::string("text"));

    auto IntRow = [&](const char *Key, const char *Label) {
        int V = UI.contains(Key) && UI[Key].is_number_integer() ? UI[Key].get<int>() : 0;
        ImGui::TextUnformatted(Label); ImGui::SameLine(kLabelCol); ImGui::SetNextItemWidth(kFieldWidth);
        if (ImGui::InputInt((std::string("##") + Key).c_str(), &V)) { UI[Key] = V; m_s->MarkDirty(); }
    };
    if (Ctl == "int" || Ctl == "float") { IntRow("MIN", "Min"); IntRow("MAX", "Max"); }
    else if (Ctl == "text" || Ctl == "secret") TextRow("PATTERN", "Pattern", "regex the value must match (optional)");
    if (Ctl == "secret")
    {
        //POOL: seed values one per line (a CD-key list). The runtime draws one at first launch; the user can
        //overwrite. Stored as a string array.
        std::string Text;
        if (UI.contains("POOL") && UI["POOL"].is_array())
            for (const auto &V : UI["POOL"]) if (V.is_string()) Text += V.get<std::string>() + "\n";
        ImGui::TextUnformatted("Pool"); ImGui::SameLine(kLabelCol);
        const int Lines = (int)std::count(Text.begin(), Text.end(), '\n') + 1;
        if (ImGui::InputTextMultiline("##pool", &Text, ImVec2(kFieldWidth, 16.0f * (float)std::min(Lines + 1, 6))))
        {
            nlohmann::ordered_json Arr = nlohmann::ordered_json::array();
            std::stringstream SS(Text); std::string Line;
            while (std::getline(SS, Line)) { if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                                             if (!Line.empty()) Arr.push_back(Line); }
            UI["POOL"] = std::move(Arr); m_s->MarkDirty();
        }
    }
    else if (Ctl == "enum")
    {
        //CHOICES editor: one "value" or "Label = value" per line ↔ the array of {LABEL,VALUE} (bare string = both).
        std::string Text;
        if (UI.contains("CHOICES") && UI["CHOICES"].is_array())
            for (const auto &O : UI["CHOICES"])
            {
                if (O.is_string()) Text += O.get<std::string>() + "\n";
                else if (O.is_object())
                {
                    const std::string L = O.value("LABEL", std::string()), Va = O.value("VALUE", std::string());
                    Text += (L.empty() || L == Va) ? (Va + "\n") : (L + " = " + Va + "\n");
                }
            }
        ImGui::TextUnformatted("Choices"); ImGui::SameLine(kLabelCol);
        int Lines = (int)std::count(Text.begin(), Text.end(), '\n') + 1;
        if (ImGui::InputTextMultiline("##choices", &Text, ImVec2(kFieldWidth, 16.0f * (float)std::min(Lines + 1, 6))))
        {
            json Arr = json::array();
            std::stringstream SS(Text); std::string Line;
            while (std::getline(SS, Line))
            {
                //trim
                size_t A = Line.find_first_not_of(" \t"); if (A == std::string::npos) continue;
                size_t B = Line.find_last_not_of(" \t\r"); Line = Line.substr(A, B - A + 1);
                const size_t Eq = Line.find('=');
                if (Eq == std::string::npos) Arr.push_back(Line);
                else
                {
                    auto Trim = [](std::string X){ size_t a=X.find_first_not_of(" \t"); size_t b=X.find_last_not_of(" \t");
                                                   return a==std::string::npos?std::string():X.substr(a,b-a+1); };
                    Arr.push_back(json{{"LABEL", Trim(Line.substr(0, Eq))}, {"VALUE", Trim(Line.substr(Eq + 1))}});
                }
            }
            UI["CHOICES"] = std::move(Arr); m_s->MarkDirty();
        }
    }
    TextRow("GROUP", "Group", "collapsible section in the dialog");
    ImGui::PopID();
}

// ---- node rendering -------------------------------------------------------

void PkgCanvas::drawNode(int Index, Graph &G)
{
    //Bounds FIRST. nlohmann's non-const operator[](size_type) GROWS the array with nulls up to the index — the
    //hazard this file documents elsewhere — so binding the reference before the check made the check
    //unfalsifiable and would have appended nulls to the document had it ever been reachable.
    json &Ns = m_s->Nodes();
    if (Index < 0 || Index >= (int)Ns.size()) return;

    //A NODES entry that is not an object is kept as a placeholder so indices stay aligned (PkgGraph::Build),
    //and it used to be rendered like any other node — where one character in the id box reaches
    //`Ns[index]["LABEL"] = ...` and drawEnvelope reaches `Node.erase("TOGGLE")`, i.e. type_error.305 and
    //307 out of paintGL, which has no catch. It draws as a box that says what is wrong and offers nothing to
    //touch — but it is a NORMAL node in every other respect: same colour pushes and pops (an early return
    //between the pushes and the pops leaked three ImNodesColElement per frame, forever, on a canvas left open
    //with one malformed node in it), and the same position seeding, or it is drawn at the grid origin while
    //the layout, the culling test and the overview all place it in its computed slot.
    const bool Malformed = !Ns[Index].is_object();
    json &Node = Ns[Index];
    const std::string Type = Malformed ? std::string("Group") : StrOf(Node, "TYPE", "Group");
    const std::string Id   = Malformed ? std::string()        : Handle(Node);   // wiring handle (CID): Issues/Running keys

    int R, Gc, B;
    TypeColour(Type, R, Gc, B);
    ImNodes::PushColorStyle(ImNodesCol_TitleBar,         IM_COL32(R, Gc, B, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered,  IM_COL32(R + 26, Gc + 26, B + 26, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(R + 40, Gc + 40, B + 40, 255));

    ImNodes::BeginNode(Index);
    if (Malformed)
    {
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted("malformed node");
        ImNodes::EndNodeTitleBar();
        ImGui::Dummy(ImVec2(kNodeWidth, 1.0f));
        ImGui::TextDisabled("this entry is not a JSON object - fix it in the JSON view");
        ImNodes::EndNode();
        seedNodePosition(Index, G);
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        return;
    }

    ImNodes::BeginNodeTitleBar();
    // Title = the cosmetic name (LABEL) when the node has one, else the TYPE. The title-bar COLOUR already encodes
    // the type, so a named node reads as its pretty name; an unnamed one falls back to its type.
    { const std::string Nm = StrOf(Node, "LABEL"); ImGui::TextUnformatted(Nm.empty() ? Type.c_str() : Nm.c_str()); }
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
    // The editable field is the node's COSMETIC name (LABEL), NOT the handle (the handle is the derived CID, shown
    // read-only below). Blank is allowed — the node then shows its short CID. renameNode just sets LABEL.
    std::string EditLabel = StrOf(Node, "LABEL");
    ImGui::TextUnformatted("name"); ImGui::SameLine(kLabelCol);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (m_s->Running.find(Id) == m_s->Running.end()
        && ImGui::InputText("##name", &EditLabel)) renameNode(Index, EditLabel);
    // The identity (CID) handle, read-only, so the author can see/copy what this node resolves to. A never-minted
    // draft shows its "draft-…" handle until the next Publish assigns a real CID.
    if (!Id.empty())
    {
        ImGui::TextDisabled("cid"); ImGui::SameLine(kLabelCol);
        ImGui::TextDisabled("%s", Id.size() > 20 ? (Id.substr(0, 12) + "…" + Id.substr(Id.size() - 6)).c_str() : Id.c_str());
    }

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

    seedNodePosition(Index, G);
}

// Seed imnodes with this node's position the first time it is drawn, and re-seed if its index moved.
// Previously this ran only for nodes with NO stored position, so a saved layout was never restored: every
// node rendered at imnodes' default origin and the next mouse-release wrote [0,0] over the whole bundle.
//
// Its own function because the malformed-node placeholder needs it too — it returns early, and without this
// it was drawn at the grid origin while the layout, the viewport culling and the overview all placed it in
// its computed slot: the box you could see vanished when you panned to where it supposedly was.
void PkgCanvas::seedNodePosition(int Index, const Graph &G)
{
    const std::string Id = G.Nodes[(size_t)Index].Id;
    //A malformed placeholder has no id. Every such node would share Seeded[""] and DrawnLast[""] and re-seed
    //each other every frame; it has nothing to read back anyway, so it is simply pushed to its layout slot.
    if (Id.empty())
    {
        ImNodes::SetNodeGridSpacePos(Index, ImVec2(G.Nodes[(size_t)Index].X, G.Nodes[(size_t)Index].Y));
        return;
    }
    auto Sit = m_s->Seeded.find(Id);
    //Re-seed when the index moved (a different node now owns this id) OR when the node was not submitted last
    //frame: in that case imnodes destroyed it and BeginNode just created a fresh one at (0,0). Skipping the
    //re-seed there is how a node that scrolled out and back collapsed to the origin — and the read-back then
    //wrote that origin into the layout.
    if (Sit == m_s->Seeded.end() || Sit->second != Index || !m_s->DrawnLast.count(Id))
    {
        //UNSCALED. Zoom is a view transform applied to the emitted geometry, not to the coordinates —
        //imnodes only ever sees world space, so nothing about zoom can reach the document.
        ImNodes::SetNodeGridSpacePos(Index, ImVec2(G.Nodes[Index].X, G.Nodes[Index].Y));
        m_s->Seeded[Id] = Index;
    }
}

void PkgCanvas::syncLinks(const Graph &G, const std::vector<char> &Drawn)
{
    m_s->VisibleLinks = 0;
    //A link can only be submitted when BOTH of its endpoints were submitted this frame: imnodes resolves a
    //link through the attribute ids, and an id that was never begun this frame has no position to draw to.
    auto EndsDrawn = [&](const Link &L) {
        const bool ChildOk  = L.ChildIndex  >= 0 && L.ChildIndex  < (int)Drawn.size() && Drawn[(size_t)L.ChildIndex];
        //An external parent is a chip, always submitted, so only the in-bundle end needs checking.
        const bool ParentOk = L.ParentIndex < 0
                              || (L.ParentIndex < (int)Drawn.size() && Drawn[(size_t)L.ParentIndex]);
        return ChildOk && ParentOk;
    };
    // The link id must address the exact PARENTS entry it came from. G.Links SKIPS empty/non-string entries,
    // so a running counter over it drifts from the real array index and a detach would erase a different
    // parent. Recover the true index instead.
    for (const Link &L : G.Links)
    {
        if (!EndsDrawn(L)) continue;
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
        ++m_s->VisibleLinks;
    }
}

void PkgCanvas::flushPositions(Graph &G, const std::vector<char> &Drawn)
{
    json &Ns = m_s->Nodes();
    for (int I = 0; I < (int)G.Nodes.size() && I < (int)Ns.size(); ++I)
    {
        // Only read back a node SUBMITTED THIS FRAME. Two separate disasters otherwise, both from culling:
        // imnodes has already freed every unsubmitted node by the time this runs (it is called after
        // EndNodeEditor), so GetNodeGridSpacePos does ObjectPoolFind -> -1 and then indexes Pool[-1] — an
        // out-of-bounds read whose assert is compiled out in a release build; and a node re-created this frame
        // at (0,0) would report a "drag" to the origin and persist it.
        if (I >= (int)Drawn.size() || !Drawn[(size_t)I]) continue;
        auto Sit = m_s->Seeded.find(G.Nodes[I].Id);
        if (Sit == m_s->Seeded.end() || Sit->second != I) continue;
        //No zoom arithmetic at all. imnodes was given world coordinates and returns world coordinates, so a
        //position can only change because something MOVED the node. The tolerance is the grid-snap floor, not
        //a noise filter — there is no longer any noise to filter.
        const ImVec2 P = ImNodes::GetNodeGridSpacePos(I);
        //A sink that persists whatever it is handed is how one bad frame became permanent: a position written
        //here goes into GlobalConfig and wins over the package's own POS forever, and a node at -3.4e38 can
        //never be drawn, selected or dragged back. imnodes' arithmetic runs on a cursor position we supply, so
        //this is our own output coming back — validate it before it is durable. The bound is absurd rather
        //than tight (a real graph is tens of thousands of units across, not millions) so it can only ever
        //catch a value that is already nonsense.
        if (!std::isfinite(P.x) || !std::isfinite(P.y)
            || std::abs(P.x) > 1.0e7f || std::abs(P.y) > 1.0e7f)
        {
            //Refusing to WRITE it is only half the job: imnodes is still holding the bad value and the node is
            //still submitted every frame (culling reads our own X/Y), so it would be drawn off at that
            //coordinate for the rest of the session with the document looking healthy. Dropping the seed makes
            //drawNode push our position back next frame. Said once per node per distinct bad coordinate, keyed
            //in the SAME "<id>@<source>@<value>" shape the Build-rejection warning uses — two producers with
            //two key shapes in one set is how prefix maintenance once covered one and missed the other.
            if (m_s->WarnedPos.insert(G.Nodes[I].Id + "@the editor@" + std::to_string(P.x) + "," + std::to_string(P.y)).second)
            {
                Log(LogLevel::WARN, "PkgCanvas::flushPositions",
                    "refused an impossible position from the editor for node '"
                        + PkgGraph::SafeId(G.Nodes[I].Id) + "' - re-seeding it");
            }
            m_s->Seeded.erase(G.Nodes[I].Id);
            continue;
        }
        //Size is measured on the same terms and for the same reason: it is only knowable while the node is
        //submitted, and the minimap needs it for nodes that are not.
        m_s->NodeDims[G.Nodes[I].Id] = ImNodes::GetNodeDimensions(I);
        const float Tol = 0.25f;
        if (std::abs(P.x - G.Nodes[I].X) > Tol || std::abs(P.y - G.Nodes[I].Y) > Tol)
        {
            if (m_s->Layout) { SetPos(*m_s->Layout, G.Nodes[I].Id, P.x, P.y); m_s->PosDirty = true; }
            G.Nodes[I].X = P.x;
            G.Nodes[I].Y = P.y;
        }
    }
}

// ---- frame ----------------------------------------------------------------

void PkgCanvas::drawToolbar()
{
    // ＋ Add ▾ — one dropdown, types grouped (payload / declare / composition), each with its help tooltip.
    // Replaces the old inline spew of a SmallButton per type, which ran off the toolbar.
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##addnode");
    if (ImGui::BeginPopup("##addnode"))
    {
        struct Grp { const char *Title; std::vector<const char *> Types; };
        static const std::vector<Grp> Groups = {
            {"Payload",     {"VFSLayer", "RegEdit", "FileEdit", "BinaryPatch", "DllOverride", "DeclarePersist", "CustomVar"}},
            {"Declare",     {"DeclareExec", "DeclareLibraryItem"}},
            {"Composition", {"Group"}},
        };
        std::string Pick;
        for (const Grp &Gp : Groups)
        {
            ImGui::SeparatorText(Gp.Title);
            for (const char *T : Gp.Types)
            {
                if (ImGui::Selectable(T)) Pick = T;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TypeHelp(T));
            }
        }
        if (!Pick.empty())
        {
            const ImVec2 Origin = ImNodes::EditorContextGetPanning();
            m_s->Selected = addNode(Pick, 80.0f - Origin.x, 80.0f - Origin.y);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("zoom %.0f%%", (double)(m_s->Zoom * 100.0f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("mouse wheel over the canvas");
    ImGui::SameLine();
    if (ImGui::SmallButton("reset##zoom")) setZoom(1.0f);
    ImGui::SameLine();
    bool Mm = m_s->ShowMiniMap;
    if (ImGui::Checkbox("minimap", &Mm)) setMiniMap(Mm);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("An overview of the whole graph, including the nodes off-screen. Click or drag it to move the view.");
    ImGui::SameLine();
    ImGui::TextDisabled("%d/%d shown", m_s->VisibleNodes, (int)m_s->Nodes().size());

    // Bring in a node from ANOTHER bundle so there is a chip to drag a wire from. Every package in the library
    // depends on at least one out-of-bundle node (a runner, MediaStack_MS, asiloader), so without this the
    // canvas cannot author a real package at all — which is what setKnownIds was always for.
    ImGui::SameLine();
    if (ImGui::SmallButton("external...")) ImGui::OpenPopup("##external");
    if (ImGui::BeginPopup("##external"))
    {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##extfilter", "filter", &m_s->ExternalFilter);
        const auto Ids = m_s->KnownIds ? m_s->KnownIds()
                                        : std::vector<std::pair<std::string, std::string>>();   // {handle, label}
        std::set<std::string> Mine;
        for (const auto &N : m_s->Nodes()) Mine.insert(Handle(N));
        int Shown = 0;
        for (const auto &[Hnd, Lbl] : Ids)
        {
            if (Mine.count(Hnd)) continue;                   // already in this bundle: not external
            // Filter matches the human LABEL or the CID handle, so you can search by name.
            if (!m_s->ExternalFilter.empty()
                && Lbl.find(m_s->ExternalFilter) == std::string::npos
                && Hnd.find(m_s->ExternalFilter) == std::string::npos) continue;
            if (++Shown > 40) { ImGui::TextDisabled("(narrow the filter)"); break; }
            // Show the pretty LABEL; "##<handle>" keeps the Selectable id unique without showing the hash.
            if (ImGui::Selectable((Lbl + "##" + Hnd).c_str())) { offerExternal(Hnd, Lbl); ImGui::CloseCurrentPopup(); }
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
        //Build refuses a declared position no layout could have produced and hands back what it refused. Said
        //ONCE per node here rather than logged there, because this runs on every cache rebuild — which is
        //every keystroke — and the same node would otherwise warn per character typed anywhere in the package.
        for (const PkgGraph::RejectedPosition &R : m_s->Cache.RejectedPositions)
            //Keyed on the VALUE as well as the node and the source. invalidateGraph is the document-swap path
            //but it is ALSO every cache invalidation — every keystroke — so clearing the set there (which an
            //earlier version did, to stop a stale id suppressing a different package's node) put the
            //per-keystroke flood straight back. With the value in the key the only thing ever suppressed is a
            //message character-identical to one already printed, which is the thing dedup is for.
            if (m_s->WarnedPos.insert(R.NodeId + "@" + R.Source + "@" + R.Value).second)
                Log(LogLevel::WARN, "PkgCanvas",
                    "node '" + PkgGraph::SafeId(R.NodeId) + "': " + R.Source + " gives " + R.Value
                        + ", which no layout could have produced - that declaration is ignored");
        m_s->HasDependent.clear();
        for (const Link &L : m_s->Cache.Links) if (L.ParentIndex >= 0) m_s->HasDependent.insert(L.ParentIndex);
        m_s->CacheValid = true;
    }
    Graph &G = m_s->Cache;
    //Culling is unconditional now. It used to stand down whenever the minimap was on, because IMNODES' minimap
    //draws from the nodes SUBMITTED this frame — culling would have reduced the overview to a copy of the
    //viewport. The minimap below is ours and draws from G.Nodes, so it shows the whole graph no matter what was
    //submitted, and the two stopped being in tension. (It also stopped costing 1.7 s of a 3.75 s frame on the
    //2775-node bundle: it is one rectangle per node out of an array we already hold, not a second full render.)

    ImGuiIO &IO = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(IO.DisplaySize);
    ImGui::Begin("##pkgcanvas", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                     | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    drawToolbar();
    ImGui::Separator();

    //---- zoom ------------------------------------------------------------------------------------------
    //imnodes has no zoom of its own, so it is composed from three things it does expose: the STYLE (node
    //padding, rounding, border/link thickness, every pin dimension), node GRID positions pushed pre-scaled,
    //and panning. Scaling only the font and our own field widths — the first attempt — left the boxes, pins
    //and wires at 1:1 and read as "zoom does nothing".
    //
    //Hover is judged HERE, before BeginNodeEditor: afterwards a plain IsWindowHovered() is false while the
    //cursor is over imnodes' inner child window, so the wheel would never fire.
    const bool CanvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows
                                                      | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 CanvasOrigin = ImGui::GetCursorScreenPos();
    //The viewport the canvas occupies on screen. Fixed: zoom changes what is drawn INSIDE it, never its size.
    const ImVec2 CanvasAvail  = ImGui::GetContentRegionAvail();
    const ImVec4 CanvasRect(CanvasOrigin.x, CanvasOrigin.y,
                            CanvasOrigin.x + CanvasAvail.x, CanvasOrigin.y + CanvasAvail.y);
    m_s->ViewportRect = CanvasRect;
    //---- zoom easing -----------------------------------------------------------------------------------
    //Walk toward the target the wheel set, re-pinning the anchor at every step so the point under the cursor
    //never moves during the animation. Exponential, so it is frame-rate independent and has no overshoot.
    //An interaction in flight owns the pan. The anchor is re-solved from a point captured at the wheel event,
    //so re-applying it every eased frame DISCARDED whatever the user panned or dragged in the meantime — a
    //middle-drag started one frame after a wheel notch ended up exactly where the ease wanted it, with the
    //drag thrown away. The zoom still finishes; it just stops steering the view while someone else is.
    const bool Interacting =
        ImNodes::EditorContextGet().ClickInteraction.Type != ImNodesClickInteractionType_None;
    if (m_s->Zooming)
    {
        const float Dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        m_s->Zoom += (m_s->ZoomTarget - m_s->Zoom) * (1.0f - std::exp(-Dt * 18.0f));
        if (std::abs(m_s->ZoomTarget - m_s->Zoom) < 0.002f) { m_s->Zoom = m_s->ZoomTarget; m_s->Zooming = false; }
        if (!Interacting)
            ImNodes::EditorContextResetPanning(ImVec2(m_s->ZoomAnchor.x / m_s->Zoom - m_s->ZoomAnchorWorld.x,
                                                      m_s->ZoomAnchor.y / m_s->Zoom - m_s->ZoomAnchorWorld.y));
    }
    //The recentre setZoom asked for, now that the viewport is known: hold the middle of the view still. A node
    //at world w is drawn at origin + (pan + w) * zoom, so the world point at the centre under the OLD scale is
    //Half/Old - Pan, and the pan that puts it back at the centre under the new one is Half/New minus that.
    //Both scales appear, and that is the whole content of this: using the new scale on both sides cancels to
    //Pan = Pan, which is what the first version of this did while claiming to recentre.
    if (m_s->RecentreFromZoom > 0.0f)
    {
        const float Old = m_s->RecentreFromZoom;
        m_s->RecentreFromZoom = 0.0f;
        const ImVec2 Pan = ImNodes::EditorContextGetPanning();
        const ImVec2 Half(CanvasAvail.x * 0.5f, CanvasAvail.y * 0.5f);
        const ImVec2 World(Half.x / Old - Pan.x, Half.y / Old - Pan.y);
        ImNodes::EditorContextResetPanning(ImVec2(Half.x / m_s->Zoom - World.x,
                                                  Half.y / m_s->Zoom - World.y));
    }

    //---- minimap layout (computed BEFORE the editor, drawn after it) -------------------------------------
    //Ours, not imnodes'. Two things ruled the built-in one out: it draws during EndNodeEditor, so its vertices
    //land inside the range the view transform scales and the overview goes sliding out of its corner as you
    //zoom; and it draws from the nodes SUBMITTED this frame, so it cannot coexist with culling. This one is
    //screen furniture painted after the transform, from G.Nodes, so it is fixed to its corner at every zoom
    //and shows the whole graph regardless of what was drawn.
    //
    //The RECTANGLE is needed here, before the editor runs, because a click on the minimap must not also reach
    //imnodes — and imnodes consumes the mouse inside EndNodeEditor, long before our widget exists.
    ImVec4 MiniRect(0, 0, 0, 0);
    m_s->MiniMapRect = ImVec4(0, 0, 0, 0);
    //Cleared whether or not the overview draws this frame. Left stale it hands back the LAST minimap frame's
    //rectangles, indexed by a node index that may now belong to a different node or to none — and a test that
    //forgot to switch the minimap on would read a previous test's geometry and pass.
    //
    //But SIZED only when the overview is on. `assign` was zeroing one ImVec4 per node on every frame of every
    //canvas, minimap or not — 44 KB of memset per frame on the Minecraft bundle's 2775 nodes, for an array
    //whose only reader is miniMapNodeBox. clear() keeps the capacity, costs nothing, and is not weaker: the
    //accessor is bounds-checked and an empty array answers all-zero, which is exactly what its contract
    //promises for a frame the overview did not draw.
    if (m_s->ShowMiniMap) m_s->MiniBoxes.assign(m_s->Cache.Nodes.size(), ImVec4(0, 0, 0, 0));
    else                  m_s->MiniBoxes.clear();
    ImVec2 MiniWorldMin(0, 0), MiniWorldMax(0, 0);
    float  MiniScale   = 0.0f;
    bool   MiniHovered = false;
    if (m_s->ShowMiniMap && !G.Nodes.empty() && CanvasAvail.x > 32.0f && CanvasAvail.y > 32.0f)
    {
        float MinX = 1e30f, MinY = 1e30f, MaxX = -1e30f, MaxY = -1e30f;
        for (const Node &N : G.Nodes)
        {
            const ImVec2 D = NodeSize(m_s->NodeDims, N);
            MinX = std::min(MinX, N.X);         MinY = std::min(MinY, N.Y);
            MaxX = std::max(MaxX, N.X + D.x);   MaxY = std::max(MaxY, N.Y + D.y);
        }
        MiniWorldMin = ImVec2(MinX, MinY);
        MiniWorldMax = ImVec2(MaxX, MaxY);
        const float Pad = 8.0f, Inset = 4.0f;
        const float BoxW = std::min(CanvasAvail.x - 2 * Pad, std::max(120.0f, CanvasAvail.x * 0.18f));
        const float BoxH = std::min(CanvasAvail.y - 2 * Pad, std::max(90.0f,  CanvasAvail.y * 0.18f));
        MiniRect  = ImVec4(CanvasRect.z - Pad - BoxW, CanvasRect.w - Pad - BoxH,
                           CanvasRect.z - Pad,        CanvasRect.w - Pad);
        m_s->MiniMapRect = MiniRect;
        MiniScale = std::min((BoxW - 2 * Inset) / std::max(1.0f, MaxX - MinX),
                             (BoxH - 2 * Inset) / std::max(1.0f, MaxY - MinY));
        const ImVec2 M = ImGui::GetIO().MousePos;
        MiniHovered = CanvasHovered && M.x >= MiniRect.x && M.x <= MiniRect.z
                                    && M.y >= MiniRect.y && M.y <= MiniRect.w;
    }

    //---- the editor's view of the mouse ----------------------------------------------------------------
    //The whole of the input side of zoom. imnodes holds world coordinates, so it must be handed the cursor in
    //world coordinates too, or every click lands where the node would have been drawn unzoomed.
    ImGuiIO &ZIO = ImGui::GetIO();
    EditorIoGuard IoGuard(ZIO);
    const ImVec2 RealMouse = ZIO.MousePos;
    const ImVec2 RealDelta = ZIO.MouseDelta;
    const float  ViewZoom  = m_s->Zoom;
    //The region of that world space the viewport shows: the viewport divided by the zoom about the canvas
    //origin. Used three times below — as imnodes' canvas rectangle, as the submission clip, and as the space
    //the off-canvas sentinel has to be outside of — because all three are the same question.
    const bool WideClip = ViewZoom < 1.0f;
    const ImVec2 SubClipMin(CanvasOrigin.x + (CanvasRect.x - CanvasOrigin.x) / ViewZoom,
                            CanvasOrigin.y + (CanvasRect.y - CanvasOrigin.y) / ViewZoom);
    const ImVec2 SubClipMax(CanvasOrigin.x + (CanvasRect.z - CanvasOrigin.x) / ViewZoom,
                            CanvasOrigin.y + (CanvasRect.w - CanvasOrigin.y) / ViewZoom);
    if (ViewZoom != 1.0f && RealMouse.x > -FLT_MAX)
        ZIO.MousePos = ImVec2(CanvasOrigin.x + (RealMouse.x - CanvasOrigin.x) / ViewZoom,
                              CanvasOrigin.y + (RealMouse.y - CanvasOrigin.y) / ViewZoom);
    //The DELTA has to be scaled as well, and forgetting it made panning wrong by exactly the zoom factor.
    //imnodes pans with `editor.Panning += io.MouseDelta`, and Panning is in world units while MouseDelta was
    //measured in screen pixels during NewFrame — rewriting MousePos does not touch it. So at 0.5x the graph
    //crawled at half the speed of the cursor and at 3x it bolted, and nothing you grabbed stayed under the
    //pointer. Node dragging is unaffected either way: that reads MousePos absolutely, not the delta.
    if (ViewZoom != 1.0f) ZIO.MouseDelta = ImVec2(RealDelta.x / ViewZoom, RealDelta.y / ViewZoom);
    //Over the minimap the editor is shown a cursor parked OFF the canvas, so no new interaction can start
    //underneath the overview — imnodes resolves hovers and begins drags inside EndNodeEditor, long before our
    //own widget could claim the click.
    //
    //Only while nothing is in flight, and never again with a value like -FLT_MAX. TranslateSelectedNodes
    //computes a dragged node's origin ABSOLUTELY from this position and is reached whether or not the cursor
    //is over the minimap, so lying about it mid-drag wrote -3.4e38 straight into the node, into the saved
    //layout, and into GlobalConfig — a node that can never be drawn, selected or recovered from the UI again.
    //An interaction already under way must therefore see the truth; only its BEGINNING is suppressed, which is
    //exactly what imnodes' own IsMiniMapHovered guard does. What actually stops the click is MouseInCanvas()
    //gating hover resolution, so the sentinel only has to be OUTSIDE the canvas rectangle — it is an ordinary
    //world coordinate, not an impossible one, chosen finite and near so that a future path reaching the drag
    //code would displace a node by a visible distance rather than to infinity.
    if (MiniHovered && !Interacting)
        ZIO.MousePos = ImVec2(SubClipMin.x - 10000.0f, SubClipMin.y - 10000.0f);
    m_s->EditorMouse = ZIO.MousePos;
    m_s->RealMouse   = RealMouse;
    m_s->ScreenViewportPos  = ImGui::GetMainViewport()->Pos;
    m_s->ScreenViewportSize = ImGui::GetMainViewport()->Size;

    //imnodes' own grid is drawn INSIDE BeginNodeEditor, before the first vertex the transform can reach, so
    //it kept a fixed 32 px screen pitch and translated 1:1 with the panning while the content translated Z:1.
    //The graph slid across its own grid on every pan at any zoom but 1, and the grid offered no scale cue
    //whatsoever. Ours is drawn a few lines below, as ordinary content, so it simply scales and pans with
    //everything else.
    ImNodes::GetStyle().Flags &= ~ImNodesStyleFlags_GridLines;
    //Auto-panning (dragging a node past the edge) adds speed * dt to the panning, which is in WORLD units, so
    //on screen it runs Z times too fast. Divided here so the edge scrolls at the same rate at every zoom.
    ImNodes::GetIO().AutoPanningSpeed = 1000.0f / std::max(0.05f, ViewZoom);

    ImNodes::BeginNodeEditor();
    //imnodes decides "is the mouse in the canvas" by testing the position we just handed it against the
    //canvas rectangle it measured in SCREEN space. Those are two different spaces the moment the zoom is not
    //1, and the result was that only the top-left Z-by-Z fraction of the canvas responded to anything: at 0.47x
    //roughly four fifths of the visible graph could not be clicked, hovered, dragged, box-selected or panned,
    //and a node dragged past that invisible edge triggered imnodes' auto-pan and ran away with the view. Tell
    //it where the canvas is in the space the cursor is actually in and all of that follows.
    //imnodes' rectangle only while the two spaces actually differ — at 1:1 its own is already right (the
    //child's content region, inset by the 1px border) — and restored after EndNodeEditor, because a WORLD
    //rectangle left in a global between frames is a trap for any later caller comparing it to a screen cursor.
    const ImRect CanvasRectWas = GImNodes->CanvasRectScreenSpace;
    if (ViewZoom != 1.0f) GImNodes->CanvasRectScreenSpace = ImRect(SubClipMin, SubClipMax);

    //And what "the screen" means while the editor lays out. A popup opened inside a node — every combo is one
    //— is positioned by imgui from the widget's rect, which in here is WORLD space, against the viewport rect,
    //which is SCREEN space. At any zoom but 1 those disagree, and imgui then decides there is no room below a
    //widget that has plenty and flips the dropdown far above it: measured at 0.5x with the node low in the
    //view, a combo popup opened 245px away from the widget that owns it. Pointing the viewport at the region
    //the editor is actually laying out in puts that decision back in one space; the transform then maps the
    //result to where the widget is drawn. Restored immediately after the editor, before anything screen-space
    //(the minimap) is drawn. Applied at EVERY zoom, 1.0 included: there the region IS the canvas viewport, so
    //the only effect is that a dropdown near the bottom of the canvas stays inside the canvas instead of
    //hanging over the toolbar, and skipping it would leave a discontinuity at exactly 1.0 for no reason.
    //
    //A popup is placed against this rectangle and PINNED TO ITS CORNER if it does not fit, so the rectangle
    //has to be at least as big as a dropdown: ~136 world units for a combo's eight items, and a field's own
    //width. Both caps are in WORLD units — the editor lays out with an unscaled font — which an earlier
    //version got backwards, demanding 300 SCREEN-equivalent units and so switching the whole repoint off at
    //3x on an ordinary canvas.
    //
    //ENLARGED to the cap rather than standing down. Standing down was a single boolean governing two
    //independent caps: a narrow-but-tall canvas lost the vertical repoint it did not need to lose, putting
    //every dropdown back in screen space. Growing the rectangle about the region's centre keeps the
    //repoint in both axes at every zoom and on every canvas, and there is no branch left to be untested.
    ImGuiViewport *VPort = ImGui::GetMainViewport();
    const ImVec2 VWorld(SubClipMax.x - SubClipMin.x, SubClipMax.y - SubClipMin.y);
    const ImVec2 VMin(kFieldWidth + 80.0f, 200.0f);
    const ImVec2 VSize(std::max(VWorld.x, VMin.x), std::max(VWorld.y, VMin.y));
    VPort->Pos  = ImVec2(SubClipMin.x - (VSize.x - VWorld.x) * 0.5f,
                         SubClipMin.y - (VSize.y - VWorld.y) * 0.5f);
    VPort->Size = VSize;
    //WorkPos/WorkSize describe the same viewport minus any menu bars, and leaving them in screen space while
    //Pos/Size move to world space means GetMainRect() and GetWorkRect() answer in different coordinate
    //systems — ImGui::Begin clamps against one while FindBestWindowPosForPopup reads the other.
    VPort->WorkPos  = VPort->Pos;
    VPort->WorkSize = VPort->Size;
    m_s->PopupExtent = ImVec4(VPort->Pos.x, VPort->Pos.y, VPort->Size.x, VPort->Size.y);
    //The editor draws into the scrolling CHILD's draw list, not the parent's. Keep the pointer and the
    //high-water marks: everything appended between here and EndNodeEditor is the surface to transform.
    ImDrawList *Surface = ImGui::GetWindowDrawList();
    const int SurfVtx0 = Surface->VtxBuffer.Size;
    //---- the submission clip --------------------------------------------------------------------------
    //imgui culls a widget against the clip rect AT SUBMISSION TIME, and submission happens in UNSCALED
    //coordinates — the transform runs afterwards. So zoomed out, every node whose unscaled position lies past
    //the right or bottom edge of the viewport had its FIELDS culled, and the transform then faithfully scaled
    //the empty box into view: nodes drawn as blank rectangles with a title bar and nothing inside. Seen on the
    //live canvas at 47%, invisible to every geometry assertion, because the box itself is drawn by imnodes and
    //measures perfectly correct while its contents are missing.
    //
    //The region that MAPS INTO the viewport is the viewport divided by the zoom about the canvas origin, so
    //that is what submission must be clipped to. ImGui::PushClipRect — the window one, not the draw list's —
    //also sets window->ClipRect, which is the rectangle the cull test actually reads. Scaling this rectangle
    //by Z about the origin gives the viewport back exactly, so the transform below needs no special case.
    //The EXACT rectangle imgui ended up with, read back rather than recomputed. The transform below has to
    //tell this rect apart from the editor child's own, and comparing two independently-derived floats within
    //a pixel is a guess: the child's clip is the viewport inset by exactly the 1 px imnodes happens to pad
    //with, and there is a narrow band of Z where "viewport / Z" lands within a pixel of it and the two swap
    //identities. Reading the stack back makes the test an identity check.
    ImVec4 PushedClip(0, 0, 0, 0);
    if (WideClip)
    {
        ImGui::PushClipRect(SubClipMin, SubClipMax, false);
        IoGuard.Clip = true;
        PushedClip   = Surface->_ClipRectStack.back();
    }
    //NO command high-water mark, and that is not an oversight. imnodes draws through an ImDrawListSplitter
    //(links under nodes, the click interaction on top) and ChannelsMerge REBUILDS the command buffer from the
    //split point on EndNodeEditor — so an index captured here names nothing afterwards. Measured: the mark
    //read 1 while every one of the 9252 emitted elements landed in command 0, which meant the clip-rect pass
    //below had been walking an empty range and doing precisely nothing. Vertices are NOT reordered by the
    //merge (only indices and commands are), so the vertex mark above is sound; commands are handled by walking
    //the whole buffer, which is safe because this is the editor child's OWN draw list.

    //The grid, in the same unscaled space every node is submitted in: lines every GridSpacing world units,
    //offset by the panning, covering the region that maps into the viewport. Drawn immediately after
    //BeginNodeEditor, which is imnodes' own base "canvas grid" draw channel, so it stays under every node.
    {
        const float  Pitch = ImNodes::GetStyle().GridSpacing;
        const ImU32  Col   = ImNodes::GetStyle().Colors[ImNodesCol_GridLine];
        const ImVec2 Pan0  = ImNodes::EditorContextGetPanning();
        const float  X0    = CanvasOrigin.x + Pan0.x, Y0 = CanvasOrigin.y + Pan0.y;
        //The grid is scaled by the view transform below (on-screen pitch = Step * ViewZoom), so a FIXED world
        //Step mapped to sub-pixel spacing when zoomed out — which moiréd — and the old line-count cap made the
        //density POP in coarse jumps. Instead pick the world Step by DOUBLING from the base pitch until its
        //on-screen spacing clears a floor (~24 px): density stays in a stable ~24-48 px band at every zoom, and
        //because each step is a power-of-two multiple of the base the coarser grid is a strict, aligned SUBSET
        //of the finer one (lines drop out cleanly, nothing shifts).
        float Step = Pitch;
        while (Step * ViewZoom < 24.0f) Step *= 2.0f;
        for (float X = X0 + Step * std::ceil((SubClipMin.x - X0) / Step); X <= SubClipMax.x; X += Step)
            Surface->AddLine(ImVec2(X, SubClipMin.y), ImVec2(X, SubClipMax.y), Col);
        for (float Y = Y0 + Step * std::ceil((SubClipMin.y - Y0) / Step); Y <= SubClipMax.y; Y += Step)
            Surface->AddLine(ImVec2(SubClipMin.x, Y), ImVec2(SubClipMax.x, Y), Col);
    }
    //Everything measured from here on is CONTENT. The grid spans the region that maps onto the viewport at
    //every zoom, so counting it makes the reported bounds a constant the size of the canvas and the vertex
    //count a large fixed number — which silently turned "the geometry grew with the zoom" into a comparison
    //of two rounding errors, and would have let a frame that drew no nodes at all pass as healthy.
    const int ContentVtx0 = Surface->VtxBuffer.Size;

    //---- viewport culling ------------------------------------------------------------------------------
    //Submitting every node and every wire regardless of where the view is costs what the graph costs, not
    //what the SCREEN costs, and the library's biggest bundle is 2775 nodes / 39k links: measured at 3.75 s
    //PER FRAME (0.27 fps) — the editor opened and then could not be used. Drawing only what is near the
    //viewport makes the cost proportional to what is actually visible.
    //
    //Culling is safe precisely because a node that is not drawn is already handled everywhere else: drawNode
    //seeds imnodes with the position the FIRST time it draws a node, and the position read-back skips any
    //node it has not seeded at this index. Our own G.Nodes[].X/Y is the authority either way.
    const ImVec2 Pan    = ImNodes::EditorContextGetPanning();
    const ImVec2 Canvas = ImGui::GetWindowSize();
    //One node body plus slack, so a node is drawn slightly before it scrolls in and its wires never pop.
    //SCALED BY ZOOM, because the margin is a screen-space distance but a node's on-screen SIZE grows with
    //zoom: at 3x a tall node (a RegEdit with dozens of entries) whose ORIGIN sits above the viewport still
    //fills the screen, and a fixed margin culls it while the user is looking straight at it.
    const float MarginX = 700.0f * std::max(1.0f, m_s->Zoom);
    const float MarginY = 500.0f * std::max(1.0f, m_s->Zoom);
    auto OnScreen = [&](int I) {
        //World space, then scaled to screen: a node at world (x,y) is drawn at origin + (pan + world) * zoom.
        const float X = (G.Nodes[I].X + Pan.x) * m_s->Zoom, Y = (G.Nodes[I].Y + Pan.y) * m_s->Zoom;
        //Its BOX, not just its origin. A node is as tall as its payload makes it, and testing the origin alone
        //culls one whose top has scrolled past the edge while the rest of it still fills the screen: measured
        //on a 12-entry BinaryPatch (2950px tall), the canvas went blank for 2400px of panning with the node
        //covering the entire viewport, unclickable. The height is already computed for the layout; this is the
        //other place that needs it.
        const float H = (G.Nodes[I].Height > 1.0f ? G.Nodes[I].Height : 200.0f) * m_s->Zoom;
        return X > -MarginX && Y + H > -MarginY && X < Canvas.x + MarginX && Y < Canvas.y + MarginY;
    };
    //A SELECTED node is never culled. imnodes keeps a culled node's index in SelectedNodeIndices with no
    //liveness check, so once its pool slot is reused: GetSelectedNodes reports the new occupant's id, and
    //TranslateSelectedNodes drags it — moving nodes the user never selected and persisting that.
    //
    //Read from LAST frame's snapshot, not from imnodes now: NumSelectedNodes/GetSelectedNodes assert
    //CurrentScope == None (imnodes.cpp), and we are inside BeginNodeEditor here. The assert is compiled out in
    //this project's default build, which is the only reason calling it here appeared to work — a Debug build
    //aborted on the editor's first frame. The snapshot is taken after EndNodeEditor below.
    const std::set<int> &Selected = m_s->SelectedLast;
    std::vector<char> Drawn((size_t)G.Nodes.size(), 0);
    int Visible = 0;
    for (int I = 0; I < (int)G.Nodes.size(); ++I)
        if (OnScreen(I) || Selected.count(I)) { drawNode(I, G); Drawn[(size_t)I] = 1; ++Visible; }
    m_s->VisibleNodes = Visible;
    //Recorded AFTER the node loop and BEFORE anything reads it back: this is the set imnodes will still know
    //about on the next frame.
    {
        std::set<std::string> Now;
        for (int I = 0; I < (int)G.Nodes.size(); ++I) if (Drawn[(size_t)I]) Now.insert(G.Nodes[I].Id);
        m_s->DrawnLast.swap(Now);
    }

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
        // Show the pretty label the picker gave us; otherwise a short form of the CID handle (never the raw hash).
        const auto Lit = m_s->ExternalLabels.find(Chips[E]);
        const std::string Disp = (Lit != m_s->ExternalLabels.end() && !Lit->second.empty()) ? Lit->second
                               : (Chips[E].size() > 14 ? (Chips[E].substr(0, 8) + "…" + Chips[E].substr(Chips[E].size() - 4))
                                                       : Chips[E]);
        ImGui::TextUnformatted(Disp.c_str());
        ImNodes::EndOutputAttribute();
        ImNodes::EndNode();
        ImNodes::PopColorStyle();
    }

    //---- keep links visible independent of node culling (geometric, Minecraft-style frustum cull) --------------
    //imnodes only draws a link between SUBMITTED pins, so a wire whose endpoint node was culled vanished — even
    //when the wire itself crosses the viewport (two far-apart nodes both off-screen, the link passing through the
    //middle). For any link whose SEGMENT crosses the viewport but whose endpoint is culled, submit a minimal
    //off-screen proxy so the pin exists and imnodes draws the wire (correctly transformed — a hand-drawn line is
    //not). Cost: one cheap segment/viewport test per link (O(links), the order node culling already is); a proxy
    //is submitted only for a culled endpoint of a CROSSING link, never for one whose wire misses the view.
    {
        const float VX0 = -MarginX, VY0 = -MarginY, VX1 = Canvas.x + MarginX, VY1 = Canvas.y + MarginY;
        auto Center = [&](int I) {
            const ImVec2 D = NodeSize(m_s->NodeDims, G.Nodes[I]);
            return ImVec2((G.Nodes[I].X + D.x * 0.5f + Pan.x) * m_s->Zoom,
                          (G.Nodes[I].Y + D.y * 0.5f + Pan.y) * m_s->Zoom);
        };
        //Liang-Barsky: does the segment A-B touch the viewport rect at all?
        auto SegHitsView = [&](ImVec2 A, ImVec2 B) {
            float t0 = 0.0f, t1 = 1.0f; const float dx = B.x - A.x, dy = B.y - A.y;
            const float p[4] = {-dx, dx, -dy, dy};
            const float q[4] = {A.x - VX0, VX1 - A.x, A.y - VY0, VY1 - A.y};
            for (int i = 0; i < 4; ++i) {
                if (p[i] == 0.0f) { if (q[i] < 0.0f) return false; }
                else { const float r = q[i] / p[i];
                       if (p[i] < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
                       else             { if (r < t0) return false; if (r < t1) t1 = r; } }
            }
            return true;
        };
        auto SubmitProxy = [&](int I) {
            if (I < 0 || I >= (int)Drawn.size() || Drawn[(size_t)I]) return;
            //Sized with the layout estimate so the readback records a sensible dimension (an empty node would
            //stamp a tiny size into NodeDims and shrink the node in the minimap); it is off-screen so unseen.
            ImNodes::SetNodeGridSpacePos(I, ImVec2(G.Nodes[I].X, G.Nodes[I].Y));
            ImNodes::BeginNode(I);
            ImNodes::BeginInputAttribute(InPin(I)); ImNodes::EndInputAttribute();
            ImGui::Dummy(NodeSize(m_s->NodeDims, G.Nodes[I]));
            ImNodes::BeginOutputAttribute(OutPin(I)); ImNodes::EndOutputAttribute();
            ImNodes::EndNode();
            Drawn[(size_t)I] = 1;
        };
        for (const Link &L : G.Links) {
            if (L.ChildIndex < 0 || L.ChildIndex >= (int)G.Nodes.size()) continue;
            const bool ChildDrawn  = Drawn[(size_t)L.ChildIndex];
            const bool ParentExt   = L.ParentIndex < 0;                       // external chip — always submitted
            const bool ParentDrawn = ParentExt || (L.ParentIndex < (int)Drawn.size() && Drawn[(size_t)L.ParentIndex]);
            if ((ChildDrawn && ParentDrawn) || ParentExt) continue;          // already drawable / nothing to proxy
            if (!SegHitsView(Center(L.ChildIndex), Center(L.ParentIndex))) continue;   // wire misses the view → skip
            SubmitProxy(L.ChildIndex);
            SubmitProxy(L.ParentIndex);
        }
    }

    syncLinks(G, Drawn);
    IoGuard.popClip();
    ImNodes::EndNodeEditor();
    GImNodes->CanvasRectScreenSpace = CanvasRectWas;
    //Both pairs, from what was actually saved. Restoring only Pos/Size left WorkPos/WorkSize pointing at the
    //world region for the whole post-editor section — where the minimap child and the delete-confirmation
    //modal are submitted, and where ImGui::Begin clamps a window against the work rect while
    //FindBestWindowPosForPopup reads the main one: two coordinate systems inside one placement path.
    IoGuard.restoreViewport();
    ZIO.MousePos   = RealMouse;                     // input goes back to screen space for everything else
    ZIO.MouseDelta = RealDelta;                     // (the guard repeats this on any exit that skips it)
    //---- THE VIEW TRANSFORM ------------------------------------------------------------------------
    //Scale everything the editor just emitted, about the canvas origin. This is the whole of zoom: the
    //document is untouched and imnodes never hears about it, so "looking at the graph cannot edit it" holds
    //by construction rather than by a guard.
    if (Surface)
    {
        const float Z = ViewZoom;
        const ImVec2 O = CanvasOrigin;
        float MinX = 1e30f, MinY = 1e30f, MaxX = -1e30f, MaxY = -1e30f;
        for (int V = SurfVtx0; V < Surface->VtxBuffer.Size; ++V)
        {
            ImDrawVert &Vt = Surface->VtxBuffer[V];
            if (Z != 1.0f)
            {
                Vt.pos.x = O.x + (Vt.pos.x - O.x) * Z;
                Vt.pos.y = O.y + (Vt.pos.y - O.y) * Z;
            }
            //Transform everything, measure only the content. The grid is transformed with the rest — that is
            //what makes it scale — but it must not be part of what the bounds report.
            if (V < ContentVtx0) continue;
            MinX = std::min(MinX, Vt.pos.x); MaxX = std::max(MaxX, Vt.pos.x);
            MinY = std::min(MinY, Vt.pos.y); MaxY = std::max(MaxY, Vt.pos.y);
        }
        m_s->SurfaceBounds = (MaxX >= MinX) ? ImVec4(MinX, MinY, MaxX, MaxY) : ImVec4(0, 0, 0, 0);
        m_s->SurfaceVertices = Surface->VtxBuffer.Size - ContentVtx0;

        //Clip rects need two DIFFERENT treatments, and treating them alike is what made the canvas unusable
        //zoomed out. A rect that IS the canvas viewport must stay exactly the viewport — scaling that one
        //shrank the drawable and pannable area into a corner. A rect INSIDE the content (a field clipping its
        //own text) is content: it scales, then is intersected with the viewport so nothing escapes.
        if (Z != 1.0f)
        {
            const ImVec4 View = CanvasRect;
            for (int C = 0; C < Surface->CmdBuffer.Size; ++C)
            {
                ImVec4 &R = Surface->CmdBuffer[C].ClipRect;
                //Three kinds of rectangle reach this loop and each needs a different answer.
                //
                //  * The one WE pushed for submission (zoomed out only). It is the viewport divided by the
                //    zoom, so it looks viewport-like and then some — but it is in PRE-transform space and
                //    scaling it lands exactly on the viewport, which is the whole point. It must be scaled.
                //  * The editor child's OWN clip: already the viewport, in POST-transform space, used by
                //    everything imnodes draws after we pop ours. Left exactly as imgui set it — replacing it
                //    with our own idea of the viewport shaved the window border off the drawable area, and
                //    SCALING it would shrink the canvas you can draw and pan in along with the zoom, which is
                //    the bug this branch exists to prevent.
                //  * Anything narrower: content clipping its own text. Scaled, then held inside the viewport.
                const bool IsPushed = WideClip && R.x == PushedClip.x && R.y == PushedClip.y
                                                 && R.z == PushedClip.z && R.w == PushedClip.w;
                const bool IsViewport = !IsPushed
                                     && R.x <= View.x + 1.0f && R.y <= View.y + 1.0f
                                     && R.z >= View.z - 1.0f && R.w >= View.w - 1.0f;
                if (IsViewport) continue;
                const ImVec4 Sc(O.x + (R.x - O.x) * Z, O.y + (R.y - O.y) * Z,
                                O.x + (R.z - O.x) * Z, O.y + (R.w - O.y) * Z);
                R = ImVec4(std::max(Sc.x, View.x), std::max(Sc.y, View.y),
                           std::min(Sc.z, View.z), std::min(Sc.w, View.w));
                if (R.z < R.x) R.z = R.x;
                if (R.w < R.y) R.w = R.y;
            }
        }
        //The widest clip rect the canvas ended up drawing through: the interactive surface. Recorded after the
        //loop above, so it is what was really used and not what imgui emitted before the transform saw it.
        float Cx0 = 1e30f, Cy0 = 1e30f, Cx1 = -1e30f, Cy1 = -1e30f;
        for (int C = 0; C < Surface->CmdBuffer.Size; ++C)
        {
            //Only commands that actually DRAW. imgui leaves an empty command behind at every clip-stack pop
            //(EndChild's, here), carrying the enclosing window's rectangle — counting those would report a
            //surface wider than anything the canvas ever painted through.
            if (Surface->CmdBuffer[C].ElemCount == 0) continue;
            const ImVec4 &R = Surface->CmdBuffer[C].ClipRect;
            Cx0 = std::min(Cx0, R.x); Cy0 = std::min(Cy0, R.y);
            Cx1 = std::max(Cx1, R.z); Cy1 = std::max(Cy1, R.w);
        }
        m_s->SurfaceClip = (Cx1 >= Cx0) ? ImVec4(Cx0, Cy0, Cx1, Cy1) : ImVec4(0, 0, 0, 0);

        //A widget that opens a CHILD WINDOW gets its own draw list, which the loops above never touch.
        //InputTextMultiline does — every StringList field with more than one line (SUBMOUNTS, ARGS, ENV,
        //BASE_TARGETS) — so the node body moved and scaled while the text inside it stayed at its 1:1
        //position, painted over whatever node had moved there. Same transform, whole list: these windows
        //exist only inside the editor, so every vertex in them is canvas content.
        ImGuiContext &Ctx = *ImGui::GetCurrentContext();
        for (int W = 0; W < Ctx.Windows.Size; ++W)
        {
            ImGuiWindow *Win = Ctx.Windows[W];
            if (!Win->Active || Win->DrawList == Surface) continue;
            bool Inside = false;
            for (ImGuiWindow *P = Win->ParentWindow; P; P = P->ParentWindow)
                if (P->DrawList == Surface) { Inside = true; break; }
            if (!Inside) continue;
            if (Z == 1.0f) continue;                      // nothing to move, and nothing to clamp either
            ImDrawList *DL = Win->DrawList;
            for (int V = 0; V < DL->VtxBuffer.Size; ++V)
            {
                ImDrawVert &Vt = DL->VtxBuffer[V];
                Vt.pos.x = O.x + (Vt.pos.x - O.x) * Z;
                Vt.pos.y = O.y + (Vt.pos.y - O.y) * Z;
            }
            for (int C = 0; C < DL->CmdBuffer.Size; ++C)
            {
                ImVec4 &R = DL->CmdBuffer[C].ClipRect;
                R = ImVec4(O.x + (R.x - O.x) * Z, O.y + (R.y - O.y) * Z,
                           O.x + (R.z - O.x) * Z, O.y + (R.w - O.y) * Z);
                R = ImVec4(std::max(R.x, CanvasRect.x), std::max(R.y, CanvasRect.y),
                           std::min(R.z, CanvasRect.z), std::min(R.w, CanvasRect.w));
                if (R.z < R.x) R.z = R.x;
                if (R.w < R.y) R.w = R.y;
            }
            //And the rectangle the window is HIT-TESTED by. Moving only the pixels draws the field in the
            //right place and leaves it clickable in the old one — which is worse than leaving it alone, since
            //nothing is drawn where it still responds. imgui picks the hovered window in NewFrame, from the
            //REAL cursor against OuterRectClipped, before frame() runs and therefore beyond the reach of the
            //cursor hijack; it reads the value left from the previous frame (imgui.cpp documents that lag), so
            //writing the drawn rectangle here is exactly what the next frame will test against. The item-level
            //test inside the window compares the hijacked world cursor against world-space item rects and
            //already agreed; the window was the only thing out of step.
            Win->OuterRectClipped = ImRect(ImVec2(O.x + (Win->OuterRectClipped.Min.x - O.x) * Z,
                                                  O.y + (Win->OuterRectClipped.Min.y - O.y) * Z),
                                           ImVec2(O.x + (Win->OuterRectClipped.Max.x - O.x) * Z,
                                                  O.y + (Win->OuterRectClipped.Max.y - O.y) * Z));

            //NOT nudged back onto the display, though an earlier version did. Transforming a popup moves it,
            //and the temptation is to translate it back — but its ITEMS hit-test in the unscaled space imgui
            //laid them out in, against the world cursor, which is exactly consistent with where the transform
            //draws them: point at a drawn item and the world cursor lands on its rect. Translating the pixels
            //and the window rectangle without the item rects breaks that correspondence, and measurably did:
            //at 3x the dropdown responded in a 13px band 130px below the sliver it was drawn in. The clip
            //clamp above already keeps a popup inside the canvas; being clipped is the cost, and it is the
            //cheaper one.
        }
    }

    //Wheel = zoom, anchored at the CURSOR so what you point at stays put. No modifier: this is the gesture
    //people try first, and gating it behind ctrl made the feature invisible. Panning stays on middle-drag.
    //Not over the minimap — there the wheel belongs to the overview, and zooming the canvas from a click-to-pan
    //widget is a gesture nobody asked for.
    if (CanvasHovered && !MiniHovered && ImGui::GetIO().MouseWheel != 0.0f)
    {
        //The wheel moves the TARGET; the easing at the top of the frame moves the view. Notches that arrive
        //while a previous one is still playing compound on the target, so a fast flick still travels the full
        //distance instead of being swallowed by the animation.
        const float Nz = std::clamp(m_s->ZoomTarget * std::pow(1.1f, ImGui::GetIO().MouseWheel),
                                    kMinZoom, kMaxZoom);
        if (Nz != m_s->ZoomTarget)
        {
            //Record the WORLD point under the cursor. A node at world w is drawn at origin + (pan + w) * zoom,
            //so solving that for pan at any later zoom gives the pan that holds w under the same screen pixel
            //— which the easing then does once per frame until it arrives.
            const ImVec2 M = ImGui::GetIO().MousePos;
            const ImVec2 Rel(M.x - CanvasOrigin.x, M.y - CanvasOrigin.y);
            const ImVec2 NowPan = ImNodes::EditorContextGetPanning();
            m_s->ZoomAnchor      = Rel;
            m_s->ZoomAnchorWorld = ImVec2(Rel.x / m_s->Zoom - NowPan.x, Rel.y / m_s->Zoom - NowPan.y);
            m_s->ZoomTarget      = Nz;
            m_s->Zooming         = true;
        }
    }

    //---- the minimap ------------------------------------------------------------------------------------
    //Drawn AFTER the view transform, into a child of its own so it renders above the editor's child window
    //(a draw list appended to the parent would end up UNDERNEATH the canvas background). Screen furniture:
    //its rectangle came from the canvas viewport, never from the zoom, so it holds its corner at 0.2x and 3x
    //alike — the built-in one rode the transform and slid off the screen.
    //The viewport as it stands at the point post-editor windows are submitted. Recorded because that is the
    //only place the restore is observable: imgui's NewFrame recomputes the main viewport every frame, so a
    //viewport left in world space at the END of frame() is invisible from outside — while WITHIN the frame it
    //is what ImGui::Begin clamps this child, and the delete-confirmation modal, against.
    {
        const ImGuiViewport *VPNow = ImGui::GetMainViewport();
        m_s->PostEditorViewport = ImVec4(VPNow->Pos.x, VPNow->Pos.y, VPNow->Size.x, VPNow->Size.y);
        m_s->PostEditorWorkRect = ImVec4(VPNow->WorkPos.x, VPNow->WorkPos.y,
                                         VPNow->WorkSize.x, VPNow->WorkSize.y);
    }
    if (MiniScale > 0.0f)
    {
        const ImVec2 Size(MiniRect.z - MiniRect.x, MiniRect.w - MiniRect.y);
        ImGui::SetCursorScreenPos(ImVec2(MiniRect.x, MiniRect.y));
        ImGui::BeginChild("##minimap", Size, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                              | ImGuiWindowFlags_NoMove);
        ImDrawList *DL = ImGui::GetWindowDrawList();
        const ImVec2 A(MiniRect.x, MiniRect.y), B(MiniRect.z, MiniRect.w);
        DL->AddRectFilled(A, B, IM_COL32(20, 22, 26, 220), 3.0f);
        //World -> minimap, centred in the box so a graph whose aspect differs from the box is not stretched.
        const float SpanX = (MiniWorldMax.x - MiniWorldMin.x) * MiniScale;
        const float SpanY = (MiniWorldMax.y - MiniWorldMin.y) * MiniScale;
        const ImVec2 Off(A.x + (Size.x - SpanX) * 0.5f, A.y + (Size.y - SpanY) * 0.5f);
        auto ToMini = [&](float Wx, float Wy) {
            return ImVec2(Off.x + (Wx - MiniWorldMin.x) * MiniScale,
                          Off.y + (Wy - MiniWorldMin.y) * MiniScale);
        };
        for (int I = 0; I < (int)G.Nodes.size(); ++I)
        {
            const Node &N = G.Nodes[(size_t)I];
            const ImVec2 D = NodeSize(m_s->NodeDims, N);
            const ImVec2 P0 = ToMini(N.X, N.Y), P1raw = ToMini(N.X + D.x, N.Y + D.y);
            //Never smaller than a pixel: at minecraft's 27520x18760 a node is a fraction of one, and a
            //rectangle that rounds away leaves a blank overview of a graph that is definitely there.
            const ImVec2 P1(std::max(P1raw.x, P0.x + 1.0f), std::max(P1raw.y, P0.y + 1.0f));
            const bool Sel = m_s->SelectedLast.count(I) != 0;
            DL->AddRectFilled(P0, P1, Sel ? IM_COL32(255, 190, 80, 255) : IM_COL32(130, 145, 165, 200));
            //Bounds-checked. The sizes cannot disagree today (this loop walks the very array MiniBoxes was
            //sized from), but an unguarded write into a parallel vector is a buffer overrun the moment they
            //ever do — and the crash is in the renderer, nowhere near the cause.
            if (I < (int)m_s->MiniBoxes.size()) m_s->MiniBoxes[(size_t)I] = ImVec4(P0.x, P0.y, P1.x, P1.y);
        }
        //What the canvas is actually looking at. A node at world w is at origin + (pan + w) * zoom, so the
        //visible world rectangle is (-pan) to (avail / zoom - pan).
        {
            const ImVec2 ViewPan = ImNodes::EditorContextGetPanning();
            const ImVec2 V0 = ToMini(-ViewPan.x, -ViewPan.y);
            const ImVec2 V1 = ToMini(CanvasAvail.x / m_s->Zoom - ViewPan.x,
                                     CanvasAvail.y / m_s->Zoom - ViewPan.y);
            DL->AddRect(V0, V1, IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.5f);
        }
        DL->AddRect(A, B, IM_COL32(90, 100, 115, 255), 3.0f);

        //Click or drag anywhere in the box to centre the view there. The button is the whole child, so the
        //minimap swallows its own input rather than leaving it to fall through to the canvas.
        ImGui::SetCursorScreenPos(A);
        ImGui::InvisibleButton("##minimapdrag", Size);
        if (ImGui::IsItemActive() && MiniScale > 0.0f)
        {
            const ImVec2 M = ImGui::GetIO().MousePos;
            const ImVec2 W(MiniWorldMin.x + (M.x - Off.x) / MiniScale,
                           MiniWorldMin.y + (M.y - Off.y) / MiniScale);
            //Solve pan for "that world point sits at the centre of the viewport".
            ImNodes::EditorContextResetPanning(ImVec2(CanvasAvail.x * 0.5f / m_s->Zoom - W.x,
                                                      CanvasAvail.y * 0.5f / m_s->Zoom - W.y));
            m_s->Zooming = false;   // a deliberate pan wins over a zoom still easing
        }
        ImGui::EndChild();
    }
    //Selection snapshot for the NEXT frame's culling — legal only out here, with the editor scope closed.
    {
        std::set<int> Now;
        const int SelCount = ImNodes::NumSelectedNodes();
        if (SelCount > 0)
        {
            std::vector<int> Sel((size_t)SelCount, 0);
            ImNodes::GetSelectedNodes(Sel.data());
            for (int S : Sel) if (S >= 0 && S < (int)G.Nodes.size()) Now.insert(S);
        }
        m_s->SelectedLast.swap(Now);
    }

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
    //Only trust a selection whose node was SUBMITTED THIS FRAME. imnodes frees a culled node's pool slot but
    //leaves its index in SelectedNodeIndices, and GetSelectedNodes reads Pool[thatIndex].Id — so once another
    //node reuses the slot, the "selection" silently becomes a node the user never clicked, and every
    //selection-scoped action (the JSON panel, delete) points at it.
    if (ImNodes::NumSelectedNodes() == 1)
    {
        ImNodes::GetSelectedNodes(&Sel);
        if (Sel >= 0 && Sel < kExternalBase && Sel < (int)Drawn.size() && Drawn[(size_t)Sel])
            m_s->Selected = Sel;
    }
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
        && !isBusy(Handle(m_s->Nodes()[m_s->Selected])))
        m_s->ConfirmDelete = m_s->Selected;
    if (m_s->ConfirmDelete >= 0)
    {
        ImGui::OpenPopup("Delete node?");
        if (ImGui::BeginPopupModal("Delete node?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const int D = m_s->ConfirmDelete;
            const json &Ns2 = m_s->Nodes();
            const std::string DId = (D >= 0 && D < (int)Ns2.size()) ? Handle(Ns2[D]) : std::string();
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

    flushPositions(G, Drawn);
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
    const bool DocChanged = m_s->Dirty && (Released || !ImGui::IsAnyItemActive());
    const bool PosOnly     = !DocChanged && m_s->PosDirty && Released;
    if (DocChanged || PosOnly)
    {
        m_s->Dirty = false; m_s->PosDirty = false;
        //A drag changed no node file — positions are not in them — so a position-only change saves only the
        //layout where the caller gave us a way to. The full save rewrites every .json in the bundle, which on
        //the biggest one is 2775 write-and-rename cycles for moving one box.
        if (PosOnly && m_s->SaveLayoutOnly) m_s->SaveLayoutOnly();
        else if (m_s->Save)                 m_s->Save();
        emit documentChanged();
    }
}
