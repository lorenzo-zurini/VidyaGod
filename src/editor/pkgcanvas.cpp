#include "pkgcanvas.h"
#include "noderefs.h"

#include "pkgform.h"
#include "pkglayout.h"
#include "commonutils.h"

#include "imgui.h"
#include "imgui_internal.h"   // the window list, to keep hover order = draw order among node windows
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <functional>

using json = nlohmann::ordered_json;
using PkgDoc::Document;

namespace {
#include "../../third_party/fonts/editor_fonts.inc"

ImFont *GBold = nullptr;
ImFont *GMono = nullptr;

// ---- geometry, in WORLD units (one unit = one pixel at 100%) -------------------------------------------------------
constexpr float kFont     = 14.0f;    // body text
constexpr float kNodeW    = 360.0f;   // a node's width
constexpr float kPadX     = 9.0f;     // body padding, left and right
constexpr float kPadY     = 6.0f;     // body padding, top and bottom
constexpr float kTitleH   = 30.0f;    // the title bar
constexpr float kChipW    = 250.0f;   // another package's node
constexpr float kChipH    = 46.0f;
constexpr float kPortR    = 6.0f;     // port radius
constexpr float kLodZoom  = 0.42f;    // below this, nodes are boxes with names
constexpr float kMinZoom  = 0.08f, kMaxZoom = 2.5f;
constexpr float kColumnGap = 130.0f;  // clear space between layout columns (room for the wires)
constexpr float kNodeGap  = 24.0f;    // clear space kept around a node placed by hand

float RowH() { return kFont + 2.0f * 3.0f + 4.0f; }   // FramePadding.y 3, ItemSpacing.y 4 (see PushNodeStyle)

ImU32 Col(int R, int G, int B, int A = 255) { return IM_COL32(std::clamp(R, 0, 255), std::clamp(G, 0, 255), std::clamp(B, 0, 255), A); }

std::string StrOf(const json &N, const char *K)
{
    return (N.is_object() && N.contains(K) && N[K].is_string()) ? N[K].get<std::string>() : std::string();
}

//The layer types to pick from, grouped as the canvas colours them, each with what it is for beside it (the keyword
//alone says nothing to someone new to the format) and its full help on hover. Picked: the type ("" = empty node).
bool TypePicker(std::string &Picked, bool WithEmpty)
{
    const float GistX = ImGui::CalcTextSize("DELTA").x + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    bool Chose = false;
    auto Row = [&](const std::string &Type) {
        if (ImGui::Selectable(Type.empty() ? "empty" : Type.c_str())) { Picked = Type; Chose = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PkgGraph::TypeHelp(Type));
        ImGui::SameLine(GistX);
        ImGui::TextDisabled("%s", PkgGraph::TypeGist(Type));
    };
    for (const PkgGraph::TypeGroup &G : PkgGraph::TypeGroups())
    {
        ImGui::SeparatorText(G.Title);
        for (const std::string &T : G.Types) Row(T);
    }
    if (WithEmpty) Row("");                               // a node with nothing in it yet, under Composition
    return Chose;
}

std::string ShortCid(const std::string &C)
{
    return C.size() > 18 ? C.substr(0, 10) + "\xE2\x80\xA6" + C.substr(C.size() - 5) : C;
}

bool IsGraft(const json &N)
{
    return N.is_object() && N.contains("LAYERS") && N["LAYERS"].is_array() && !N["LAYERS"].empty()
        && N["LAYERS"][0].is_object() && N["LAYERS"][0].contains("ANY");
}

bool HasExec(const json &N)
{
    if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) return false;
    for (const json &L : N["LAYERS"]) if (L.is_object() && L.contains("EXEC")) return true;
    return false;
}

//A node carrying a game's tile: an EXEC entry with a TILE.
static bool HasTile(const json &N)
{
    if (!HasExec(N)) return false;
    for (const json &L : N["LAYERS"])
        if (L.is_object() && L.contains("EXEC") && L["EXEC"].is_array())
            for (const json &E : L["EXEC"]) if (E.is_object() && E.contains("TILE") && E["TILE"].is_object()) return true;
    return false;
}

//Each wire its own colour, so crossing wires can be told apart: the node it leaves picks a starting hue (from its
//name: stable across saves, which rename by CID), and its wires step from it by the golden ratio in their order -
//wires leaving one node are always far apart in hue. One saturation and brightness that read on the dark canvas;
//red is left to exclusions (NOT).
ImU32 WireHue(const std::string &From, int Rank)
{
    uint32_t Hh = 2166136261u;                           // FNV-1a
    for (const char Ch : From) { Hh ^= (unsigned char)Ch; Hh *= 16777619u; }
    float Hue = std::fmod((float)(Hh % 1000u) / 1000.0f + 0.618034f * (float)Rank, 1.0f);
    Hue = 0.08f + Hue * 0.84f;                           // 0.08..0.92: never the exclusion red
    float R, G, B;
    ImGui::ColorConvertHSVtoRGB(Hue, 0.55f, 1.0f, R, G, B);
    return IM_COL32((int)(R * 255), (int)(G * 255), (int)(B * 255), 255);
}

//Cubic bezier point.
ImVec2 Bez(const ImVec2 &A, const ImVec2 &B, const ImVec2 &C, const ImVec2 &D, float T)
{
    const float U = 1.0f - T;
    const float W0 = U * U * U, W1 = 3 * U * U * T, W2 = 3 * U * T * T, W3 = T * T * T;
    return ImVec2(W0 * A.x + W1 * B.x + W2 * C.x + W3 * D.x, W0 * A.y + W1 * B.y + W2 * C.y + W3 * D.y);
}

float DistToSegment(const ImVec2 &P, const ImVec2 &A, const ImVec2 &B)
{
    const ImVec2 AB(B.x - A.x, B.y - A.y), AP(P.x - A.x, P.y - A.y);
    const float L = AB.x * AB.x + AB.y * AB.y;
    const float T = L > 0.0f ? std::clamp((AP.x * AB.x + AP.y * AB.y) / L, 0.0f, 1.0f) : 0.0f;
    const float Dx = A.x + AB.x * T - P.x, Dy = A.y + AB.y * T - P.y;
    return std::sqrt(Dx * Dx + Dy * Dy);
}

bool Overlaps(const ImVec4 &A, const ImVec4 &B) { return A.x < B.z && B.x < A.z && A.y < B.w && B.y < A.w; }

} // namespace

// ================================================================================================================
// State
// ================================================================================================================

struct NodeView
{
    int Index = 0;                      // position in Views
    float Est = kChipH;                 // estimated folded height (world units) until a drawn height is measured
    std::string Handle;
    int Doc = -1;                       // >= 0: the document's node; -1: another package's node (a chip)
    std::string Title, Kind, Package;   // Package: the chip's package name
    std::string Summary;                // zoomed out, under the name: its layers' kinds and its shelf name
    bool Graft = false, Variant = false, Launchable = false, Unwired = false;
    float W = kNodeW;
};

//A layer's row as drawn last: its layer and its top and bottom, in world units below the node's top. Wires plug into
//the row holding their reference; a wire being dropped inserts between rows.
struct RowSpan { int Layer; float Y0, Y1; };

struct WireView
{
    int From = -1, To = -1;             // views: the parent (contained / required / excluded) and the child
    PkgGraph::Link L;
};

struct PkgCanvasState
{
    Document *Doc = nullptr;

    // ---- the graph, rebuilt when the document changes ----
    uint64_t BuiltRev = 0;
    std::vector<NodeView> Views;
    std::map<std::string, int> ViewOf;
    std::vector<WireView> Wires;
    PkgGraph::VarFacets Facets;
    std::map<std::string, std::string> Labels;           // handle -> LABEL (this package), for wires and pickers
    std::map<std::string, PkgCanvas::External> ExtCache;
    std::vector<std::string> Offered;                    // library nodes brought in by the picker, not yet wired
    std::string Structure;                               // nodes + wires: a change re-runs the layout
    //The layout's position for nodes the document does not place. STABLE: once a node has been shown somewhere it
    //stays there when the graph changes (a new wire must not rearrange the package under the author's hands); only
    //nodes without one are laid out, clear of everything already placed. A FULL layout (open, Tidy, the first
    //measurement while the view is untouched) starts over.
    std::map<std::string, ImVec2> AutoPos;
    bool LayoutValid = false;
    bool FullLayout = true;
    std::map<std::string, float> Measured;               // drawn height (world units), by handle
    std::set<std::string> MeasuredOnce;                  // first measurement corrects the layout; later ones do not

    // ---- the view ----
    ImVec2 Cam{0, 0};                                    // world point at the canvas's top-left
    float Z = 1.0f, ZTarget = 1.0f;
    bool Zooming = false;
    ImVec2 ZAnchorScreen{0, 0}, ZAnchorWorld{0, 0};
    bool FramePending = true;                            // frame the graph on the first frame
    ImVec4 Canvas{0, 0, 0, 0};                           // last frame's canvas rect (screen)
    bool ShowMini = true;
    bool PackageOpen = true;                             // false: the host has no package; the empty canvas offers one
    std::string NextStep;                                // what the package still needs (PkgGraph::NextStep), as of...
    uint64_t NextStepRev = ~0ull;                        // ...this content revision
    ImVec4 MiniRect{0, 0, 0, 0};

    // ---- selection and interaction ----
    std::set<std::string> Sel;
    std::string SelWireChild; int SelWireSlot = -1;
    enum class Mode : uint8_t { Idle, Pan, Box, Drag, Link } M = Mode::Idle;
    ImVec2 PressScreen{0, 0};
    ImVec2 DragDelta{0, 0};                              // world offset of the selection being dragged
    std::string LinkHandle; bool LinkFromOut = true;     // the port a wire is being dragged from
    std::string DropTarget;                              // the node the dragged wire would land on
    std::string HoverNode, HoverWireChild; int HoverWireSlot = -1;
    std::string PortHover;                               // the node whose port is under the cursor
    std::set<std::string> LinkBlocked;                   // nodes the dragged wire may not land on (a cycle, itself)
    int LinkAt = -1;                                     // where in the drop target the reference goes (-1: usual)
    std::string WireNewFrom; bool WireNewOut = true;     // a wire let go on the empty canvas: the node it came from...
    ImVec2 WireNewWorld{0, 0};                           // ...and where the new node goes
    struct LayerDrop { std::string From; int FromLayer = -1; std::string To; int At = 0; bool Copy = false; } Drop;
    std::pair<std::string, std::string> MergeReq;        // "merge this node into that one", done after the frame
    std::string RemoveReq;                               // the node menu's "Delete", done after the frame: the node is still being drawn
    std::map<std::string, std::vector<RowSpan>> Rows;    // each node's layer rows, as drawn last
    std::string QuickText; ImVec2 QuickWorld{0, 0};      // quick add: what is typed, where the node goes
    bool FileDropHover = false;                          // files from the file manager are over the canvas
    int TourStep = -1;                                   // the first-time tour's card on screen (-1: none)
    bool AutoFrame = true;                               // re-frame after the first layouts, until the user moves the view
    bool RightPressedOnBg = false;
    ImVec2 ContextWorld{0, 0};
    std::string Renaming, RenameBuf, RenameFrom;   // RenameFrom: the LABEL the rename started from
    bool RenameFocus = false;
    std::string Search;
    bool SearchFocus = false;
    std::string OfferFilter;
    std::string LastSelEmitted = "\x01";

    // ---- per-node view state ----
    std::set<std::string> Open;                          // "<handle>|<key>" folds that are open
    std::set<std::string> ExpandAll;                     // nodes to unfold completely on their next draw
    std::string FoldNode;                                // the node whose folds are being drawn
    std::map<std::string, std::vector<PkgGraph::RegRow>> RegBuf;   // "<handle>|reg#<layer>"
    std::vector<std::string> ZOrder;                     // handles, bottom -> top

    // ---- host facts ----
    PkgCanvas::ExternalFn ExternalLookup;
    PkgCanvas::OffersFn Offers;
    PkgCanvas::ImageFn Image;
    std::function<std::vector<std::string>()> PlatformsFn;
    std::vector<std::string> Platforms;
    bool PlatformsAsked = false;
    std::map<std::string, std::vector<std::string>> Issues, Hints;
    struct Busy { QString What, Detail; float Frac = -1.0f; bool Cancellable = false; };
    std::map<std::string, Busy> Running;
    std::pair<std::string, std::string> Pending;         // an action clicked this frame, dispatched after it

    // ---- last frame, for introspection ----
    std::map<std::string, ImVec4> DrawnRect;
    std::set<std::string> CoversDrawn;                   // chips drawn with their cover this frame
    int VisibleNodes = 0, VisibleWires = 0;
    bool Animating = false;
    std::vector<ImVec4> Rects;                           // this frame's world rect of every view (see WorldRect)
    bool RectsValid = false;

    ImVec2 W2S(const ImVec2 &W) const { return ImVec2(Canvas.x + (W.x - Cam.x) * Z, Canvas.y + (W.y - Cam.y) * Z); }
    ImVec2 S2W(const ImVec2 &S) const { return ImVec2(Cam.x + (S.x - Canvas.x) / Z, Cam.y + (S.y - Canvas.y) / Z); }
};

static void RefreshEstimates(PkgCanvasState &S);

// ================================================================================================================
// Fonts and style
// ================================================================================================================

void PkgCanvas::InstallFontsAndStyle()
{
    ImGuiIO &Io = ImGui::GetIO();
    ImFontConfig Cfg;
    Cfg.OversampleH = 2;
    Cfg.PixelSnapH = false;
    Io.Fonts->Clear();
    Io.Fonts->AddFontFromMemoryCompressedTTF(NotoSansRegular_compressed_data, (int)NotoSansRegular_compressed_size, kFont, &Cfg);
    GBold = Io.Fonts->AddFontFromMemoryCompressedTTF(NotoSansBold_compressed_data, (int)NotoSansBold_compressed_size, kFont, &Cfg);
    GMono = Io.Fonts->AddFontFromMemoryCompressedTTF(NotoSansMono_compressed_data, (int)NotoSansMono_compressed_size, kFont, &Cfg);

    ImGui::StyleColorsDark();
    ImGuiStyle &S = ImGui::GetStyle();
    S.WindowPadding = ImVec2(8, 6);
    S.FramePadding = ImVec2(6, 3);
    S.ItemSpacing = ImVec2(6, 4);
    S.ItemInnerSpacing = ImVec2(4, 4);
    S.IndentSpacing = 16.0f;
    S.FrameRounding = 3.0f;
    S.PopupRounding = 4.0f;
    S.WindowRounding = 0.0f;
    S.ChildRounding = 6.0f;
    S.GrabRounding = 3.0f;
    S.ScrollbarSize = 12.0f;
    S.WindowBorderSize = 0.0f;
    S.PopupBorderSize = 1.0f;
    ImVec4 *C = S.Colors;
    C[ImGuiCol_WindowBg]        = ImVec4(0.105f, 0.110f, 0.125f, 1.0f);
    C[ImGuiCol_ChildBg]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    C[ImGuiCol_PopupBg]         = ImVec4(0.135f, 0.140f, 0.160f, 0.98f);
    C[ImGuiCol_Border]          = ImVec4(0.30f, 0.32f, 0.37f, 0.60f);
    C[ImGuiCol_FrameBg]         = ImVec4(0.180f, 0.190f, 0.215f, 1.0f);
    C[ImGuiCol_FrameBgHovered]  = ImVec4(0.230f, 0.245f, 0.280f, 1.0f);
    C[ImGuiCol_FrameBgActive]   = ImVec4(0.260f, 0.280f, 0.320f, 1.0f);
    C[ImGuiCol_Button]          = ImVec4(0.220f, 0.240f, 0.285f, 1.0f);
    C[ImGuiCol_ButtonHovered]   = ImVec4(0.290f, 0.330f, 0.410f, 1.0f);
    C[ImGuiCol_ButtonActive]    = ImVec4(0.330f, 0.400f, 0.520f, 1.0f);
    C[ImGuiCol_Header]          = ImVec4(0.230f, 0.260f, 0.320f, 0.55f);
    C[ImGuiCol_HeaderHovered]   = ImVec4(0.270f, 0.310f, 0.390f, 0.80f);
    C[ImGuiCol_HeaderActive]    = ImVec4(0.300f, 0.360f, 0.460f, 1.0f);
    C[ImGuiCol_Text]            = ImVec4(0.900f, 0.910f, 0.930f, 1.0f);
    C[ImGuiCol_TextDisabled]    = ImVec4(0.560f, 0.580f, 0.620f, 1.0f);
    C[ImGuiCol_CheckMark]       = ImVec4(0.520f, 0.720f, 1.000f, 1.0f);
    C[ImGuiCol_SliderGrab]      = ImVec4(0.520f, 0.720f, 1.000f, 1.0f);
    C[ImGuiCol_Separator]       = ImVec4(0.300f, 0.320f, 0.370f, 0.60f);
}

ImFont *PkgCanvas::MonoFont() { return GMono; }

// ================================================================================================================
// Construction and host facts
// ================================================================================================================

PkgCanvas::PkgCanvas(Document *doc, QObject *parent) : QObject(parent), m_s(std::make_unique<PkgCanvasState>())
{
    m_s->Doc = doc;
}

PkgCanvas::~PkgCanvas() = default;

void PkgCanvas::setExternalLookup(ExternalFn fn) { m_s->ExternalLookup = std::move(fn); m_s->ExtCache.clear(); m_s->BuiltRev = 0; }
void PkgCanvas::setOffers(OffersFn fn)            { m_s->Offers = std::move(fn); }
void PkgCanvas::setImageLookup(ImageFn fn) { m_s->Image = std::move(fn); }
void PkgCanvas::setPlatforms(std::function<std::vector<std::string>()> fn) { m_s->PlatformsFn = std::move(fn); m_s->PlatformsAsked = false; }
void PkgCanvas::setIssues(const std::map<std::string, std::vector<std::string>> &I) { m_s->Issues = I; RefreshEstimates(*m_s); }
void PkgCanvas::setNodeHints(const std::string &H, const std::vector<std::string> &Hints) { m_s->Hints[H] = Hints; }
void PkgCanvas::beginAction(const std::string &H, const QString &What, bool Cancellable)
{ m_s->Running[H] = PkgCanvasState::Busy{What, QString(), -1.0f, Cancellable}; RefreshEstimates(*m_s); }
void PkgCanvas::setProgress(const std::string &H, float F, const QString &Detail)
{
    auto It = m_s->Running.find(H);
    if (It == m_s->Running.end()) return;
    It->second.Frac = F;
    It->second.Detail = Detail;
}
void PkgCanvas::endAction(const std::string &H) { m_s->Running.erase(H); RefreshEstimates(*m_s); }
bool PkgCanvas::isBusy(const std::string &H) const { return m_s->Running.count(H) != 0; }
void PkgCanvas::setPackageOpen(bool Open) { m_s->PackageOpen = Open; }

void PkgCanvas::documentReset()
{
    m_s->BuiltRev = 0;
    m_s->LayoutValid = false;
    m_s->FullLayout = true;
    m_s->AutoPos.clear();
    m_s->Measured.clear(); m_s->MeasuredOnce.clear();
    m_s->Sel.clear(); m_s->SelWireSlot = -1;
    m_s->Open.clear(); m_s->RegBuf.clear(); m_s->ZOrder.clear();
    m_s->Renaming.clear();
    m_s->FramePending = true;
    m_s->AutoFrame = true;
}

void PkgCanvas::applyRenames(const std::map<std::string, std::string> &R)
{
    if (R.empty()) return;
    auto Swap = [&](const std::string &H) { const auto It = R.find(H); return It == R.end() ? H : It->second; };
    auto Prefixed = [&](const std::string &K) {                      // "<handle>|rest"
        const size_t Bar = K.find('|');
        return Bar == std::string::npos ? K : Swap(K.substr(0, Bar)) + K.substr(Bar);
    };
    std::set<std::string> Sel, Open, Exp;
    for (const auto &H : m_s->Sel) Sel.insert(Swap(H));
    for (const auto &K : m_s->Open) Open.insert(Prefixed(K));
    for (const auto &K : m_s->ExpandAll) Exp.insert(Prefixed(K));
    m_s->Sel.swap(Sel); m_s->Open.swap(Open); m_s->ExpandAll.swap(Exp);
    std::map<std::string, float> Me;
    for (auto &[H, V] : m_s->Measured) Me[Swap(H)] = V;
    m_s->Measured.swap(Me);
    std::set<std::string> Mo;
    for (const auto &H : m_s->MeasuredOnce) Mo.insert(Swap(H));
    m_s->MeasuredOnce.swap(Mo);
    std::map<std::string, ImVec2> Ap;
    for (auto &[H, V] : m_s->AutoPos) Ap[Swap(H)] = V;
    m_s->AutoPos.swap(Ap);
    for (auto &H : m_s->ZOrder) H = Swap(H);
    decltype(m_s->Issues) Is;
    for (auto &[H, V] : m_s->Issues) Is[Swap(H)] = V;
    m_s->Issues.swap(Is);
    decltype(m_s->Hints) Hs;
    for (auto &[H, V] : m_s->Hints) Hs[Swap(H)] = V;
    m_s->Hints.swap(Hs);
    decltype(m_s->Running) Rn;
    for (auto &[H, V] : m_s->Running) Rn[Swap(H)] = V;
    m_s->Running.swap(Rn);
    m_s->RegBuf.clear();
    if (!m_s->SelWireChild.empty()) m_s->SelWireChild = Swap(m_s->SelWireChild);
    m_s->Renaming = m_s->Renaming.empty() ? std::string() : Swap(m_s->Renaming);
    m_s->Structure.clear();                                          // same structure: keep the layout, rebuild the rest
    m_s->BuiltRev = 0;
    m_s->LayoutValid = true;
}

// ================================================================================================================
// The graph: views, wires, layout
// ================================================================================================================

namespace {

std::string NodeTitle(const json &N, const std::string &Handle)
{
    const std::string L = StrOf(N, "LABEL");
    if (!L.empty()) return L;
    const std::string K = PkgGraph::KindOf(N);
    return (K.empty() ? std::string("node") : K) + " " + ShortCid(Handle);
}

//How tall a node is drawn folded, in world units: the title, then one row each for its properties, its issues, the
//top level of its layer tree, and its footer (two when busy). The renderer uses exactly these rows.
float EstimateHeight(const json &N, const PkgGraph::VarFacets &Facets, size_t Issues, bool Busy)
{
    int Rows = 1 + (int)Issues + 1;                                     // properties, issues, footer
    const auto Items = PkgGraph::LayerItems(N, Facets);
    Rows += std::max(1, PkgGraph::TopLevelRows(Items));
    if (Busy) Rows += 1;
    return kTitleH + kPadY * 2.0f + (float)Rows * RowH() - 4.0f;
}

} // namespace

//Base, or "Base 2", "Base 3"... — the first no node of the package is called. A new or duplicated node differs from
//every other by its name at least: two byte-identical nodes would be one file, and saving refuses them.
static std::string FreeLabel(const PkgDoc::Document &D, const std::string &Base)
{
    const auto Labels = D.Labels();
    std::string Label = Base;
    for (int K = 2; ; ++K)
    {
        bool Taken = false;
        for (const auto &[H, L] : Labels) if (L == Label) { Taken = true; break; }
        if (!Taken) return Label;
        Label = Base + " " + std::to_string(K);
    }
}

static void Rebuild(PkgCanvasState &S)
{
    S.RectsValid = false;                                // views are about to be renumbered
    Document &D = *S.Doc;
    std::vector<PkgGraph::NodeRef> Refs;
    Refs.reserve((size_t)D.Count());
    for (int I = 0; I < D.Count(); ++I) Refs.push_back({D.Handle(I), &D.Node(I)});
    const PkgGraph::Graph G = PkgGraph::Build(Refs);
    S.Facets = PkgGraph::CollectVarFacets(Refs);
    S.Labels = D.Labels();

    S.Views.clear(); S.ViewOf.clear(); S.Wires.clear();
    std::set<int> Contained;
    for (const PkgGraph::Link &L : G.Links) if (L.ParentIndex >= 0) Contained.insert(L.ParentIndex);
    for (int I = 0; I < D.Count(); ++I)
    {
        NodeView V;
        V.Handle = D.Handle(I);
        V.Doc = I;
        V.Title = NodeTitle(D.Node(I), V.Handle);
        V.Kind = PkgGraph::KindOf(D.Node(I));
        V.Graft = IsGraft(D.Node(I));
        V.Variant = !StrOf(D.Node(I), "VARIANT").empty();
        V.Launchable = HasExec(D.Node(I));
        V.Unwired = !Contained.count(I) && !V.Launchable && !V.Graft;
        {
            //"ZIP ×2 · REG · EXEC — Patch 2.0a (No-CD)": what a box says when its rows are too small to read.
            std::vector<std::pair<std::string, int>> Kinds;
            for (const std::string &T : PkgGraph::LayerTypes(D.Node(I)))
            {
                if (T.empty()) continue;
                auto It = std::find_if(Kinds.begin(), Kinds.end(), [&T](const auto &K) { return K.first == T; });
                if (It == Kinds.end()) Kinds.push_back({T, 1}); else ++It->second;
            }
            for (const auto &[T, N] : Kinds) V.Summary += (V.Summary.empty() ? "" : " \xC2\xB7 ") + T + (N > 1 ? " \xC3\x97" + std::to_string(N) : "");
            if (const std::string Shelf = StrOf(D.Node(I), "VARIANT"); !Shelf.empty()) V.Summary += " \xE2\x80\x94 " + Shelf;
        }
        V.Index = (int)S.Views.size();
        S.ViewOf[V.Handle] = V.Index;
        S.Views.push_back(std::move(V));
    }
    std::vector<std::string> Externals = G.Externals;
    for (const std::string &O : S.Offered)
        if (!S.ViewOf.count(O) && std::find(Externals.begin(), Externals.end(), O) == Externals.end()) Externals.push_back(O);
    for (const std::string &E : Externals)
    {
        auto It = S.ExtCache.find(E);
        if (It == S.ExtCache.end())
            It = S.ExtCache.emplace(E, S.ExternalLookup ? S.ExternalLookup(E) : PkgCanvas::External{}).first;
        NodeView V;
        V.Handle = E;
        V.Title = It->second.Label.empty() ? ShortCid(E) : It->second.Label;
        V.Package = It->second.Package.empty() ? std::string("another package") : It->second.Package;
        V.W = kChipW;
        V.Index = (int)S.Views.size();
        S.ViewOf[E] = V.Index;
        S.Views.push_back(std::move(V));
    }
    std::string Structure;
    for (const NodeView &V : S.Views) Structure += V.Handle + ";";
    for (const PkgGraph::Link &L : G.Links)
    {
        WireView W;
        W.L = L;
        W.To = L.ChildIndex;
        W.From = L.ParentIndex >= 0 ? L.ParentIndex : S.ViewOf.count(L.ExternalId) ? S.ViewOf[L.ExternalId] : -1;
        if (W.From < 0) continue;
        Structure += std::to_string(W.From) + ">" + std::to_string(W.To) + ";";
        S.Wires.push_back(W);
    }
    if (Structure != S.Structure) { S.Structure = Structure; S.LayoutValid = false; }
    //Z-order: keep what exists, add new nodes on top, drop the gone.
    std::vector<std::string> Z;
    for (const std::string &H : S.ZOrder) if (S.ViewOf.count(H)) Z.push_back(H);
    std::set<std::string> In(Z.begin(), Z.end());
    for (const NodeView &V : S.Views) if (!In.count(V.Handle)) Z.push_back(V.Handle);
    S.ZOrder.swap(Z);
    for (auto It = S.Sel.begin(); It != S.Sel.end();) It = S.ViewOf.count(*It) ? std::next(It) : S.Sel.erase(It);
    S.BuiltRev = D.Revision();
    RefreshEstimates(S);
}

static float ViewHeight(const PkgCanvasState &S, const NodeView &V)
{
    if (V.Doc < 0) return kChipH;
    const auto M = S.Measured.find(V.Handle);
    return M != S.Measured.end() ? M->second : V.Est;
}

//The folded-height estimate of every node, recomputed when what it depends on changes (the document, the issues, a
//running action) — never per frame: it walks the node's layer tree, and wires, ports and culling ask for heights
//tens of thousands of times a frame on a big package.
static void RefreshEstimates(PkgCanvasState &S)
{
    //Views built for an older document (another package was just loaded) index nodes it no longer has; the next
    //Rebuild measures them again.
    if (S.BuiltRev != S.Doc->Revision()) return;
    for (NodeView &V : S.Views)
    {
        if (V.Doc < 0) { V.Est = kChipH; continue; }
        const auto Is = S.Issues.find(V.Handle);
        V.Est = EstimateHeight(S.Doc->Node(V.Doc), S.Facets, Is == S.Issues.end() ? 0 : Is->second.size(), S.Running.count(V.Handle) != 0);
    }
}

//Lay out every node (and chip); nodes this machine placed keep their place, and a laid-out node never lands on a
//placed one (pushed down its column until clear).
static void Layout(PkgCanvasState &S)
{
    PkgGraph::Graph G;
    for (int I = 0; I < (int)S.Views.size(); ++I)
    {
        PkgGraph::Node N;
        N.Index = I;
        N.Id = S.Views[(size_t)I].Handle;
        N.Height = ViewHeight(S, S.Views[(size_t)I]);
        G.Nodes.push_back(N);
    }
    for (const WireView &W : S.Wires)
    {
        PkgGraph::Link L;
        L.ParentIndex = W.From;
        L.ChildIndex = W.To;
        G.Links.push_back(L);
    }
    PkgLayout::Options O;
    O.ColumnStep = kNodeW + kColumnGap;
    O.RowGap = 36.0f;
    O.BandGap = 160.0f;
    PkgLayout::Compute(G, O);
    const auto &Placed = S.Doc->Positions();
    if (S.FullLayout) S.AutoPos.clear();
    std::vector<ImVec4> Taken;
    for (int I = 0; I < (int)S.Views.size(); ++I)
    {
        const NodeView &V = S.Views[(size_t)I];
        const auto P = Placed.find(V.Handle);
        if (P != Placed.end()) Taken.push_back(ImVec4(P->second.X, P->second.Y, P->second.X + V.W, P->second.Y + ViewHeight(S, V)));
        else if (const auto Ap = S.AutoPos.find(V.Handle); Ap != S.AutoPos.end())
            Taken.push_back(ImVec4(Ap->second.x, Ap->second.y, Ap->second.x + V.W, Ap->second.y + ViewHeight(S, V)));
    }
    for (int I = 0; I < (int)S.Views.size(); ++I)
    {
        const NodeView &V = S.Views[(size_t)I];
        if (Placed.count(V.Handle) || S.AutoPos.count(V.Handle)) continue;   // it has a place already
        ImVec2 P(G.Nodes[(size_t)I].X, G.Nodes[(size_t)I].Y);
        //A chip is narrower than a node: centre it in its column's width, so its wire leaves from where the column's
        //nodes' wires do.
        if (V.Doc < 0) P.x += kNodeW - kChipW;
        const float H = ViewHeight(S, V);
        for (int Guard = 0; Guard < 1000; ++Guard)
        {
            bool Hit = false;
            for (const ImVec4 &T : Taken)
                if (Overlaps(ImVec4(P.x, P.y, P.x + V.W, P.y + H), ImVec4(T.x - 20, T.y - 20, T.z + 20, T.w + 20)))
                { P.y = T.w + 36.0f; Hit = true; }
            if (!Hit) break;
        }
        S.AutoPos[V.Handle] = P;
        Taken.push_back(ImVec4(P.x, P.y, P.x + V.W, P.y + H));
    }
    S.LayoutValid = true;
    S.FullLayout = false;
}

//Where a view is in the world right now: the document's position, else the layout's, plus a drag in progress.
static ImVec2 WorldPos(const PkgCanvasState &S, const NodeView &V)
{
    ImVec2 P(0, 0);
    const auto &Placed = S.Doc->Positions();
    const auto It = Placed.find(V.Handle);
    if (It != Placed.end()) P = ImVec2(It->second.X, It->second.Y);
    else if (const auto A = S.AutoPos.find(V.Handle); A != S.AutoPos.end()) P = A->second;
    if (S.M == PkgCanvasState::Mode::Drag && S.Sel.count(V.Handle)) { P.x += S.DragDelta.x; P.y += S.DragDelta.y; }
    return P;
}

static ImVec4 ComputeRect(const PkgCanvasState &S, const NodeView &V)
{
    const ImVec2 P = WorldPos(S, V);
    return ImVec4(P.x, P.y, P.x + V.W, P.y + ViewHeight(S, V));
}

//Where a view is this frame. Every node's rectangle is computed once per frame (Rects) — wires, ports, culling and the
//minimap all ask — and recomputed outside a frame (the public API).
static ImVec4 WorldRect(const PkgCanvasState &S, const NodeView &V)
{
    if (S.RectsValid && V.Index < (int)S.Rects.size()) return S.Rects[(size_t)V.Index];
    return ComputeRect(S, V);
}

//Where a new node goes: At, or the nearest spot below it (then further right) where a node of this size overlaps no
//node already drawn - a new node never lands on top of another and hides it.
static ImVec2 FreeSpot(const PkgCanvasState &S, ImVec2 At, float H = 220.0f)
{
    const float Gap = 24.0f;
    auto Free = [&](ImVec2 P) {
        const ImVec4 Me(P.x - Gap, P.y - Gap, P.x + kNodeW + Gap, P.y + H + Gap);
        for (const NodeView &V : S.Views)
        {
            const ImVec4 R = WorldRect(S, V);
            if (Me.x < R.z && R.x < Me.z && Me.y < R.w && R.y < Me.w) return false;
        }
        return true;
    };
    for (int Col = 0; Col < 8; ++Col)
        for (int Row = 0; Row < 24; ++Row)
        {
            const ImVec2 P(At.x + (float)Col * (kNodeW + Gap * 2.0f), At.y + (float)Row * 40.0f);
            if (Free(P)) return P;
        }
    return At;
}

static void UpdateRects(PkgCanvasState &S)
{
    S.RectsValid = false;
    S.Rects.resize(S.Views.size());
    for (const NodeView &V : S.Views) S.Rects[(size_t)V.Index] = ComputeRect(S, V);
    S.RectsValid = true;
}

// ================================================================================================================
// Public view API
// ================================================================================================================

float PkgCanvas::zoom() const { return m_s->Z; }

void PkgCanvas::setZoom(float Z)
{
    Z = std::clamp(Z, kMinZoom, kMaxZoom);
    const ImVec2 Mid((m_s->Canvas.x + m_s->Canvas.z) * 0.5f, (m_s->Canvas.y + m_s->Canvas.w) * 0.5f);
    const ImVec2 W = m_s->S2W(Mid);
    m_s->Z = m_s->ZTarget = Z;
    m_s->Zooming = false;
    m_s->Cam = ImVec2(W.x - (Mid.x - m_s->Canvas.x) / Z, W.y - (Mid.y - m_s->Canvas.y) / Z);
    m_s->FramePending = false;
    m_s->AutoFrame = false;
}

void PkgCanvas::setCamera(float X, float Y) { m_s->Cam = ImVec2(X, Y); m_s->FramePending = false; m_s->AutoFrame = false; m_s->Zooming = false; }
void PkgCanvas::cameraPos(float &X, float &Y) const { X = m_s->Cam.x; Y = m_s->Cam.y; }

static void FrameRect(PkgCanvasState &S, ImVec4 R)
{
    const float CW = std::max(100.0f, S.Canvas.z - S.Canvas.x), CH = std::max(100.0f, S.Canvas.w - S.Canvas.y);
    if (R.z <= R.x || R.w <= R.y) return;
    const float Pad = 60.0f;
    const float Z = std::clamp(std::min((CW - 2 * Pad) / (R.z - R.x), (CH - 2 * Pad) / (R.w - R.y)), kMinZoom, 1.0f);
    S.Z = S.ZTarget = Z;
    S.Zooming = false;
    S.Cam = ImVec2((R.x + R.z) * 0.5f - CW * 0.5f / Z, (R.y + R.w) * 0.5f - CH * 0.5f / Z);
}

static ImVec4 BoundsOf(const PkgCanvasState &S, const std::set<std::string> *Only)
{
    ImVec4 B(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const NodeView &V : S.Views)
    {
        if (Only && !Only->count(V.Handle)) continue;
        const ImVec4 R = WorldRect(S, V);
        B = ImVec4(std::min(B.x, R.x), std::min(B.y, R.y), std::max(B.z, R.z), std::max(B.w, R.w));
    }
    return B;
}

void PkgCanvas::frameAll()
{
    if (m_s->BuiltRev != m_s->Doc->Revision()) Rebuild(*m_s);
    if (!m_s->LayoutValid) Layout(*m_s);
    if (m_s->Views.empty()) { m_s->Cam = ImVec2(0, 0); m_s->Z = m_s->ZTarget = 1.0f; return; }
    if (m_s->Canvas.z <= m_s->Canvas.x) { m_s->FramePending = true; return; }
    FrameRect(*m_s, BoundsOf(*m_s, nullptr));
    m_s->FramePending = false;
}

void PkgCanvas::frameSelection()
{
    if (m_s->Sel.empty()) { frameAll(); return; }
    FrameRect(*m_s, BoundsOf(*m_s, &m_s->Sel));
}

void PkgCanvas::tidyLayout()
{
    for (const NodeView &V : m_s->Views) m_s->Doc->ClearPos(V.Handle);
    m_s->Doc->Commit();
    m_s->LayoutValid = false;
    m_s->FullLayout = true;
    emit documentEdited();
}

void PkgCanvas::setExpanded(const std::string &H, bool Open)
{
    const std::string Pre = H + "|";
    if (Open) { m_s->ExpandAll.insert(Pre); return; }
    m_s->ExpandAll.erase(Pre);
    for (auto It = m_s->Open.lower_bound(Pre); It != m_s->Open.end() && It->rfind(Pre, 0) == 0;) It = m_s->Open.erase(It);
}

std::string PkgCanvas::selectedHandle() const
{
    return m_s->Sel.size() == 1 ? *m_s->Sel.begin() : std::string();
}
std::set<std::string> PkgCanvas::selection() const { return m_s->Sel; }

void PkgCanvas::select(const std::string &H, bool Frame)
{
    if (m_s->BuiltRev != m_s->Doc->Revision()) Rebuild(*m_s);
    m_s->Sel.clear();
    m_s->SelWireSlot = -1;
    if (m_s->ViewOf.count(H))
    {
        m_s->Sel.insert(H);
        auto &Z = m_s->ZOrder;
        Z.erase(std::remove(Z.begin(), Z.end(), H), Z.end());
        Z.push_back(H);
        if (Frame)
        {
            if (!m_s->LayoutValid) Layout(*m_s);
            const ImVec4 R = WorldRect(*m_s, m_s->Views[(size_t)m_s->ViewOf[H]]);
            //Centre it, keeping the zoom unless it would not fit.
            const float CW = m_s->Canvas.z - m_s->Canvas.x, CH = m_s->Canvas.w - m_s->Canvas.y;
            if (CW > 0 && CH > 0)
            {
                if ((R.z - R.x) * m_s->Z > CW * 0.9f || m_s->Z < kLodZoom) m_s->Z = m_s->ZTarget = std::min(1.0f, CW * 0.6f / (R.z - R.x));
                m_s->Cam = ImVec2((R.x + R.z) * 0.5f - CW * 0.5f / m_s->Z, R.y - CH * 0.25f / m_s->Z);
                m_s->FramePending = false;
            }
        }
    }
}

int PkgCanvas::addNode(const std::string &Type, float X, float Y, const std::string &RefersTo)
{
    json N = PkgGraph::NewPayload(Type);
    if (!RefersTo.empty() && m_s->Doc->IndexOf(RefersTo) >= 0 && !N["LAYERS"].empty())
    {
        json &L = N["LAYERS"][0];
        if (L.contains("NODE")) L["NODE"] = RefersTo;
        else if (L.contains("NOT")) L["NOT"] = RefersTo;
        else if (L.contains("ANY")) L["ANY"] = json::array({RefersTo});
    }
    json Out = json::object();
    std::string Base = Type.empty() ? std::string("New node") : "New " + Type;
    Out["LABEL"] = FreeLabel(*m_s->Doc, Base);
    for (const auto &[K, V] : N.items()) Out[K] = V;
    const int I = m_s->Doc->Add(std::move(Out));
    //Where asked, unless that lands on a node: then straight down, below whatever it would cover. A new node is
    //opened for filling in, so room is kept for its rows, not just its title.
    const float Room = std::max(EstimateHeight(m_s->Doc->Node(I), m_s->Facets, 0, false), 14.0f * RowH());
    for (int Guard = 0; Guard < 256; ++Guard)
    {
        const ImVec4 Want(X - kNodeGap, Y - kNodeGap, X + kNodeW + kNodeGap, Y + Room + kNodeGap);
        float Below = -FLT_MAX;
        for (const NodeView &V : m_s->Views)
            if (const ImVec4 R = ComputeRect(*m_s, V); Overlaps(R, Want)) Below = std::max(Below, R.w);
        if (Below == -FLT_MAX) break;
        Y = Below + kNodeGap;
    }
    m_s->Doc->SetPos(m_s->Doc->Handle(I), {X, Y});
    m_s->Doc->Commit();
    Rebuild(*m_s);
    select(m_s->Doc->Handle(I));
    emit documentEdited();
    return I;
}

static std::set<std::string> Reach(const Document &D, const std::string &From, bool Up);

bool PkgCanvas::MoveLayer(const std::string &From, int FromLayer, const std::string &To, int At, bool Copy, std::string *Why)
{
    Document &D = *m_s->Doc;
    const int Fi = D.IndexOf(From), Ti = D.IndexOf(To);
    if (Fi < 0 || Ti < 0) return false;
    json Src = D.Node(Fi);
    if (!Src.is_object() || !Src.contains("LAYERS") || !Src["LAYERS"].is_array() || FromLayer < 0 || FromLayer >= (int)Src["LAYERS"].size())
        return false;
    const json Layer = Src["LAYERS"][(size_t)FromLayer];
    //A reference may not land where it would make a node contain itself.
    for (int J = 0; J < D.Count(); ++J)
        if (NodeRefs::Names(Layer, D.Handle(J)) && (J == Ti || Reach(D, D.Handle(J), false).count(To)))
        { if (Why) *Why = "Not there: it would make \"" + NodeTitle(D.Node(Ti), To) + "\" contain itself."; return false; }
    if (Fi == Ti)
    {
        if (Copy)
        {
            json &Ls = Src["LAYERS"];
            Ls.insert(Ls.begin() + std::clamp(At, 0, (int)Ls.size()), Layer);
        }
        else if (!PkgGraph::MoveLayerTo(Src, FromLayer, At)) return false;
        D.Replace(Fi, std::move(Src));
    }
    else
    {
        json Dst = D.Node(Ti);
        if (!PkgGraph::MoveLayerAcross(Src, FromLayer, Dst, To, At, Copy)) return false;
        if (!Copy) D.Replace(Fi, std::move(Src));
        D.Replace(Ti, std::move(Dst));
    }
    m_s->RegBuf.clear();                                 // rows renumbered: buffers keyed by layer are stale
    return true;
}

bool PkgCanvas::removeNodes(const std::set<std::string> &Handles)
{
    bool Any = false;
    for (const std::string &H : Handles)
    {
        if (isBusy(H)) continue;                         // a conversion is rewriting its file right now
        const int I = m_s->Doc->IndexOf(H);
        if (I >= 0) { m_s->Doc->Remove(I); Any = true; }
        else if (std::find(m_s->Offered.begin(), m_s->Offered.end(), H) != m_s->Offered.end())
        { m_s->Offered.erase(std::remove(m_s->Offered.begin(), m_s->Offered.end(), H), m_s->Offered.end()); m_s->BuiltRev = 0; }
    }
    if (!Any) return false;
    m_s->Doc->Commit();
    m_s->Sel.clear();
    emit documentEdited();
    emit statusMessage(QString("Deleted %1 node(s) - Ctrl+Z to undo").arg(Handles.size()));
    return true;
}

void PkgCanvas::undo() { if (m_s->Doc->Undo()) { m_s->RegBuf.clear(); emit documentEdited(); } }
void PkgCanvas::redo() { if (m_s->Doc->Redo()) { m_s->RegBuf.clear(); emit documentEdited(); } }

bool PkgCanvas::nodeRect(const std::string &H, float &X0, float &Y0, float &X1, float &Y1) const
{
    const auto It = m_s->DrawnRect.find(H);
    if (It == m_s->DrawnRect.end()) return false;
    X0 = It->second.x; Y0 = It->second.y; X1 = It->second.z; Y1 = It->second.w;
    return true;
}

static ImVec2 PortScreen(const PkgCanvasState &S, const NodeView &V, bool Out)
{
    const ImVec4 R = WorldRect(S, V);
    const float Y = R.y + (V.Doc < 0 ? kChipH * 0.5f : kTitleH * 0.5f);
    return S.W2S(ImVec2(Out ? R.z : R.x, Y));
}

//Where a reference in layer `Layer` of V plugs in: on V's left edge, level with that layer's row (its title port
//until the node has been drawn once).
static ImVec2 RowPort(const PkgCanvasState &S, const NodeView &V, int Layer)
{
    ImVec2 P = PortScreen(S, V, false);
    if (const auto It = S.Rows.find(V.Handle); It != S.Rows.end())
        for (const RowSpan &R : It->second)
            if (R.Layer == Layer) { P.y = S.W2S(ImVec2(0, WorldRect(S, V).y + (R.Y0 + R.Y1) * 0.5f)).y; break; }
    return P;
}

//Every node a node reaches through its references (NODE, ANY, NOT), within this package; Up: every node that reaches it.
static std::set<std::string> Reach(const Document &D, const std::string &From, bool Up)
{
    std::map<std::string, std::vector<std::string>> Refs;
    for (int I = 0; I < D.Count(); ++I)
    {
        const json &N = D.Node(I);
        if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) continue;
        for (const json &L : N["LAYERS"])
            for (int J = 0; J < D.Count(); ++J)
                if (J != I && NodeRefs::Names(L, D.Handle(J)))
                {
                    if (Up) Refs[D.Handle(J)].push_back(D.Handle(I));
                    else Refs[D.Handle(I)].push_back(D.Handle(J));
                }
    }
    std::set<std::string> Seen;
    std::vector<std::string> Todo{From};
    while (!Todo.empty())
    {
        const std::string H = Todo.back(); Todo.pop_back();
        for (const std::string &R : Refs[H]) if (Seen.insert(R).second) Todo.push_back(R);
    }
    return Seen;
}

std::string PkgCanvas::nodeAt(float X, float Y) const
{
    for (auto It = m_s->ZOrder.rbegin(); It != m_s->ZOrder.rend(); ++It)
    {
        const auto R = m_s->DrawnRect.find(*It);
        const auto V = m_s->ViewOf.find(*It);
        if (R == m_s->DrawnRect.end() || V == m_s->ViewOf.end() || m_s->Views[(size_t)V->second].Doc < 0) continue;
        if (X >= R->second.x && X <= R->second.z && Y >= R->second.y && Y <= R->second.w) return *It;
    }
    return {};
}

void PkgCanvas::screenToWorld(float X, float Y, float &Wx, float &Wy) const
{
    const ImVec2 W = m_s->S2W(ImVec2(X, Y));
    Wx = W.x; Wy = W.y;
}

bool PkgCanvas::packageOpen() const { return m_s->PackageOpen; }
void PkgCanvas::setFileDropHover(bool On) { m_s->FileDropHover = On; }
void PkgCanvas::startTour() { m_s->TourStep = 0; }
bool PkgCanvas::touring() const { return m_s->TourStep >= 0; }

bool PkgCanvas::rowPos(const std::string &H, int Layer, float &X, float &Y) const
{
    const auto It = m_s->ViewOf.find(H);
    if (It == m_s->ViewOf.end()) return false;
    const ImVec2 P = RowPort(*m_s, m_s->Views[(size_t)It->second], Layer);
    X = P.x; Y = P.y;
    return true;
}

bool PkgCanvas::portPos(const std::string &H, bool Out, float &X, float &Y) const
{
    const auto It = m_s->ViewOf.find(H);
    if (It == m_s->ViewOf.end()) return false;
    const ImVec2 P = PortScreen(*m_s, m_s->Views[(size_t)It->second], Out);
    X = P.x; Y = P.y;
    return true;
}

bool PkgCanvas::canvasRect(float &X0, float &Y0, float &X1, float &Y1) const
{
    X0 = m_s->Canvas.x; Y0 = m_s->Canvas.y; X1 = m_s->Canvas.z; Y1 = m_s->Canvas.w;
    return X1 > X0;
}

int PkgCanvas::visibleNodes() const { return m_s->VisibleNodes; }
int PkgCanvas::visibleWires() const { return m_s->VisibleWires; }
bool PkgCanvas::coverShown(const std::string &H) const { return m_s->CoversDrawn.count(H) != 0; }
int PkgCanvas::externalCount() const
{
    int N = 0;
    for (const NodeView &V : m_s->Views) if (V.Doc < 0) ++N;
    return N;
}
std::string PkgCanvas::externalLabel(const std::string &C) const
{
    const auto It = m_s->ViewOf.find(C);
    if (It == m_s->ViewOf.end() || m_s->Views[(size_t)It->second].Doc >= 0) return {};
    const NodeView &V = m_s->Views[(size_t)It->second];
    return V.Package + ": " + V.Title;
}

bool PkgCanvas::worldRect(const std::string &H, float &X, float &Y, float &W, float &Hh) const
{
    const auto It = m_s->ViewOf.find(H);
    if (It == m_s->ViewOf.end()) return false;
    const ImVec4 R = WorldRect(*m_s, m_s->Views[(size_t)It->second]);
    X = R.x; Y = R.y; W = R.z - R.x; Hh = R.w - R.y;
    return true;
}

bool PkgCanvas::wantsFrames() const
{
    return m_s->Animating || m_s->Zooming || m_s->M != PkgCanvasState::Mode::Idle || !m_s->Running.empty()
        || (ImGui::GetCurrentContext() && (ImGui::IsAnyItemActive() || ImGui::GetCurrentContext()->OpenPopupStack.Size > 0));
}

// ================================================================================================================
// Drawing
// ================================================================================================================

namespace {

struct NodeStyle
{
    int Vars = 0, Cols = 0;
    void Push(float Z, ImU32 Bg)
    {
        auto V2 = [&](ImGuiStyleVar V, float X, float Y) { ImGui::PushStyleVar(V, ImVec2(X * Z, Y * Z)); ++Vars; };
        auto V1 = [&](ImGuiStyleVar V, float X) { ImGui::PushStyleVar(V, X * Z); ++Vars; };
        V2(ImGuiStyleVar_WindowPadding, 0, 0);
        V2(ImGuiStyleVar_FramePadding, 6, 3);
        V2(ImGuiStyleVar_ItemSpacing, 6, 4);
        V2(ImGuiStyleVar_ItemInnerSpacing, 4, 4);
        V1(ImGuiStyleVar_IndentSpacing, 16);
        V1(ImGuiStyleVar_FrameRounding, 3);
        V1(ImGuiStyleVar_ChildRounding, 7);
        V1(ImGuiStyleVar_GrabMinSize, 10);
        V1(ImGuiStyleVar_ScrollbarSize, 10);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f); ++Vars;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg); ++Cols;
        ImGui::PushFont(nullptr, kFont * Z);
    }
    void Pop() { ImGui::PopFont(); ImGui::PopStyleColor(Cols); ImGui::PopStyleVar(Vars); }
};

void DrawPort(ImDrawList *DL, ImVec2 C, float R, ImU32 Fill, bool Hot)
{
    DL->AddCircleFilled(C, R * (Hot ? 1.35f : 1.0f), IM_COL32(24, 26, 31, 255));
    DL->AddCircleFilled(C, R * (Hot ? 1.1f : 0.78f), Fill);
}

void Arrow(ImDrawList *DL, ImVec2 Tip, ImVec2 From, float Size, ImU32 C)
{
    ImVec2 D(Tip.x - From.x, Tip.y - From.y);
    const float L = std::sqrt(D.x * D.x + D.y * D.y);
    if (L < 1e-3f) return;
    D = ImVec2(D.x / L, D.y / L);
    const ImVec2 N(-D.y, D.x);
    DL->AddTriangleFilled(Tip, ImVec2(Tip.x - D.x * Size + N.x * Size * 0.55f, Tip.y - D.y * Size + N.y * Size * 0.55f),
                          ImVec2(Tip.x - D.x * Size - N.x * Size * 0.55f, Tip.y - D.y * Size - N.y * Size * 0.55f), C);
}

} // namespace

// ---- one node (a child window) -------------------------------------------------------------------------------------

static void DrawNode(PkgCanvas &Self, PkgCanvasState &S, int Vi, std::vector<ImGuiWindow *> &Windows)
{
    NodeView &V = S.Views[(size_t)Vi];
    Document &D = *S.Doc;
    const float Z = S.Z;
    const ImVec4 WR = WorldRect(S, V);
    const ImVec2 P0 = S.W2S(ImVec2(WR.x, WR.y));
    const bool Selected = S.Sel.count(V.Handle) != 0;
    int R, G, B;
    PkgGraph::TypeColour(V.Kind, R, G, B);

    NodeStyle St;
    St.Push(Z, IM_COL32(34, 36, 42, 245));
    ImGui::SetCursorScreenPos(P0);
    ImGui::BeginChild(("##node" + V.Handle).c_str(), ImVec2(V.W * Z, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
    Windows.push_back(ImGui::GetCurrentWindow());
    ImDrawList *DL = ImGui::GetWindowDrawList();
    const ImVec2 WinPos = ImGui::GetWindowPos();
    const float Wd = V.W * Z, Th = kTitleH * Z;
    //Hover is what a dragged wire drops on, so it counts while the canvas owns the mouse.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        S.HoverNode = V.Handle;

    //Clicking anywhere on a node selects it and brings it to the front (its widgets still get the click).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)))
    {
        if (ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(0)) { if (!S.Sel.erase(V.Handle)) S.Sel.insert(V.Handle); }
        else if (!Selected) { S.Sel.clear(); S.Sel.insert(V.Handle); }
        S.SelWireSlot = -1;
        S.ZOrder.erase(std::remove(S.ZOrder.begin(), S.ZOrder.end(), V.Handle), S.ZOrder.end());
        S.ZOrder.push_back(V.Handle);
    }

    // ---- title bar ----
    const ImRect Full(WinPos, ImVec2(WinPos.x + Wd, WinPos.y + ImGui::GetWindowHeight()));
    DL->PushClipRect(ImVec2(S.Canvas.x, S.Canvas.y), ImVec2(S.Canvas.z, S.Canvas.w), false);
    DL->AddRectFilled(WinPos, ImVec2(WinPos.x + Wd, WinPos.y + Th), Col(R, G, B), 7.0f * Z, ImDrawFlags_RoundCornersTop);
    ImGui::PushClipRect(WinPos, ImVec2(WinPos.x + Wd, WinPos.y + Th), false);
    ImGui::SetCursorPos(ImVec2(0, 0));
    //A port under the cursor wins the click (it starts a wire): the title's drag stands down for it.
    if (S.PortHover == V.Handle) ImGui::Dummy(ImVec2(Wd, Th));
    else
    {
        ImGui::SetNextItemAllowOverlap();                // the Test Play button on it takes its own clicks
        ImGui::InvisibleButton("##title", ImVec2(Wd, Th), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    }
    const bool TitleHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(0)) { S.M = PkgCanvasState::Mode::Drag; S.DragDelta = ImVec2(0, 0); S.PressScreen = ImGui::GetIO().MousePos; }
    if (TitleHovered && ImGui::IsMouseDoubleClicked(0) && V.Doc >= 0)
    { S.Renaming = V.Handle; S.RenameBuf = S.RenameFrom = StrOf(D.Node(V.Doc), "LABEL"); S.RenameFocus = true; S.M = PkgCanvasState::Mode::Idle; }
    if (TitleHovered && S.M == PkgCanvasState::Mode::Idle)
    {
        std::string Tip = V.Title + "\n" + (V.Kind.empty() ? std::string("empty node") : V.Kind + " node") + "   " + V.Handle;
        if (V.Graft) Tip += "\nA graft: it rides on the node its ANY names, ticked in the pre-launch window.";
        if (V.Variant) Tip += "\nOn the shelf as \"" + StrOf(D.Node(V.Doc), "VARIANT") + "\".";
        if (V.Unwired) Tip += "\nNothing contains this node yet - drag a wire from its right port into the node that should.";
        Tip += "\n\ndrag to move - double-click to rename - right-click for more";
        ImGui::SetTooltip("%s", Tip.c_str());
    }
    if (ImGui::BeginPopupContextItem("##nodemenu"))
    {
        if (ImGui::MenuItem("Rename", "double-click")) { S.Renaming = V.Handle; S.RenameBuf = S.RenameFrom = StrOf(D.Node(V.Doc), "LABEL"); S.RenameFocus = true; }
        if (ImGui::MenuItem("Open all rows", "E")) Self.setExpanded(V.Handle, true);
        if (ImGui::MenuItem("Fold all rows", "E")) Self.setExpanded(V.Handle, false);
        ImGui::Separator();
        if (ImGui::BeginMenu("Add layer"))
        {
            std::string T;
            if (TypePicker(T, false))
            {
                json N = D.Node(V.Doc);
                if (PkgGraph::AddLayer(N, T))
                {
                    const int At = (int)N["LAYERS"].size() - 1;
                    D.Replace(V.Doc, std::move(N));
                    S.Open.insert(V.Handle + "|L" + std::to_string(At));
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            json N = D.Node(V.Doc);
            if (N.is_object()) N["LABEL"] = FreeLabel(D, StrOf(N, "LABEL") + " copy");
            const int I = D.Add(std::move(N));
            D.SetPos(D.Handle(I), {WR.x + 40.0f, WR.y + 40.0f});
            S.Sel = {D.Handle(I)};
        }
        //Consolidate: this node's layers go into another, which takes its place everywhere.
        if (ImGui::BeginMenu("Merge into", D.Count() > 1))
        {
            for (int I = 0; I < D.Count(); ++I)
                if (I != V.Doc && ImGui::MenuItem((NodeTitle(D.Node(I), D.Handle(I)) + "##merge" + D.Handle(I)).c_str()))
                    S.MergeReq = {V.Handle, D.Handle(I)};
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move every layer of this node into another node, which then takes its place in the package");
        if (ImGui::BeginMenu("Compare with", D.Count() > 1))
        {
            for (int I = 0; I < D.Count(); ++I)
                if (I != V.Doc && ImGui::MenuItem((NodeTitle(D.Node(I), D.Handle(I)) + "##cmp" + D.Handle(I)).c_str()))
                    S.Pending = {V.Handle, "compare:" + D.Handle(I)};
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("What differs between this node and another: the layers each folds, the files each gives the game");
        if (ImGui::MenuItem("Copy CID")) ImGui::SetClipboardText(V.Handle.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) S.RemoveReq = V.Handle;
        ImGui::EndPopup();
    }
    //Test Play: a version or an entry runs from its title bar, through the same pre-launch window a player uses.
    float PlayW = 0.0f;
    if ((V.Variant || V.Launchable) && S.Renaming != V.Handle && !S.Running.count(V.Handle))
    {
        const float Bh = Th - 8.0f * Z;
        PlayW = Bh + 6.0f * Z;
        ImGui::SetCursorScreenPos(ImVec2(WinPos.x + Wd - PlayW - 8.0f * Z, WinPos.y + 4.0f * Z));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.75f, 0.45f, 0.9f));
        if (ImGui::Button("##testplay", ImVec2(Bh + 6.0f * Z, Bh))) S.Pending = {V.Handle, "test_launch"};
        ImGui::PopStyleColor(2);
        {   //A play triangle, drawn: the canvas's font has no such glyph.
            const ImVec2 Mn = ImGui::GetItemRectMin(), Mx = ImGui::GetItemRectMax(), Ct((Mn.x + Mx.x) * 0.5f, (Mn.y + Mx.y) * 0.5f);
            const float Rr = (Mx.y - Mn.y) * 0.28f;
            DL->AddTriangleFilled(ImVec2(Ct.x - Rr * 0.8f, Ct.y - Rr), ImVec2(Ct.x - Rr * 0.8f, Ct.y + Rr), ImVec2(Ct.x + Rr, Ct.y), IM_COL32(235, 245, 238, 255));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Test Play: save the package and open its pre-launch window for this version");
    }
    //Title text: the name in bold, badges right-aligned.
    std::string Badge = V.Graft ? "graft" : V.Variant ? "variant" : V.Launchable ? "entry" : "";
    float BadgeW = PlayW;
    ImGui::PushFont(GBold, kFont * Z);
    const float TextY = WinPos.y + (Th - ImGui::GetFontSize()) * 0.5f;
    if (!Badge.empty())
    {
        ImGui::PushFont(nullptr, kFont * 0.8f * Z);
        const ImVec2 BS = ImGui::CalcTextSize(Badge.c_str());
        const ImVec2 B0(WinPos.x + Wd - PlayW - (BS.x + 12.0f * Z) - 14.0f * Z, WinPos.y + (Th - BS.y - 4.0f * Z) * 0.5f);
        BadgeW = PlayW + BS.x + 12.0f * Z;
        DL->AddRectFilled(B0, ImVec2(B0.x + BS.x + 12.0f * Z, B0.y + BS.y + 4.0f * Z), IM_COL32(0, 0, 0, 70), 8.0f * Z);
        DL->AddText(ImVec2(B0.x + 6.0f * Z, B0.y + 2.0f * Z), IM_COL32(235, 238, 245, 230), Badge.c_str());
        ImGui::PopFont();
    }
    //A name changed elsewhere while its rename box was open (the JSON pane, undo) ends the rename: the box would show
    //the old name and, once it lost focus, write it back over the new one.
    if (S.Renaming == V.Handle && V.Doc >= 0 && StrOf(D.Node(V.Doc), "LABEL") != S.RenameFrom) S.Renaming.clear();
    if (S.Renaming == V.Handle)
    {
        ImGui::SetCursorScreenPos(ImVec2(WinPos.x + 12.0f * Z, WinPos.y + (Th - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(Wd - 24.0f * Z - BadgeW);
        if (S.RenameFocus) { ImGui::SetKeyboardFocusHere(); S.RenameFocus = false; }
        const bool Enter = ImGui::InputText("##rename", &S.RenameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (Enter || ImGui::IsItemDeactivated())
        {
            if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && V.Doc >= 0 && S.RenameBuf != StrOf(D.Node(V.Doc), "LABEL"))
            {
                json N = D.Node(V.Doc);
                if (N.is_object()) { if (S.RenameBuf.empty()) N.erase("LABEL"); else N["LABEL"] = S.RenameBuf; D.Replace(V.Doc, std::move(N)); }
            }
            S.Renaming.clear();
        }
    }
    else
    {
        const std::string T = PkgForm::FitWidth(V.Title, Wd - 30.0f * Z - BadgeW);
        DL->AddText(ImVec2(WinPos.x + 12.0f * Z, TextY), IM_COL32(248, 249, 252, 255), T.c_str());
    }
    ImGui::PopFont();
    ImGui::PopClipRect();

    // ---- body ----
    ImGui::SetCursorPos(ImVec2(0, Th + kPadY * Z));
    ImGui::Indent(kPadX * Z);
    const bool Busy = S.Running.count(V.Handle) != 0;
    //What the node does, in words, before its rows say it in fields.
    {
        const std::string Said = PkgGraph::Describe(D.Node(V.Doc), [&S](const std::string &Ref) {
            if (const auto It = S.Labels.find(Ref); It != S.Labels.end()) return It->second;
            if (const auto It = S.ExtCache.find(Ref); It != S.ExtCache.end()) return It->second.Label;
            return std::string();
        });
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.70f, 0.78f, 1.0f));
        ImGui::TextUnformatted(PkgForm::FitWidth(Said, Wd - 2.0f * kPadX * Z).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Said.c_str());
    }
    if (const auto Is = S.Issues.find(V.Handle); Is != S.Issues.end())
        for (size_t Mi = 0; Mi < Is->second.size(); ++Mi)
        {
            const std::string &M = Is->second[Mi];
            const PkgGraph::Fix Fx = PkgGraph::FixFor(M, &D.Node(V.Doc));
            const float BtnW = Fx.Action.empty() ? 0.0f : ImGui::CalcTextSize(Fx.Label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::AlignTextToFramePadding();
            //Waiting for a step the author is about to take is not a problem: said calmly, with the step beside it.
            ImGui::PushStyleColor(ImGuiCol_Text, Fx.Waiting ? ImVec4(0.62f, 0.66f, 0.74f, 1.0f) : ImVec4(1.0f, 0.74f, 0.32f, 1.0f));
            ImGui::TextUnformatted(PkgForm::FitWidth((Fx.Waiting ? "\xE2\x80\xA6 " + Fx.Note : "! " + M), Wd - 2.0f * kPadX * Z - BtnW).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", M.c_str());
            if (!Fx.Action.empty())
            {
                ImGui::SameLine();
                ImGui::PushID((int)Mi);
                if (ImGui::SmallButton(Fx.Label.c_str()))
                {
                    if (Fx.Action.rfind("add_layer:", 0) == 0)
                    {
                        json N = D.Node(V.Doc);
                        if (PkgGraph::AddLayer(N, Fx.Action.substr(10)))
                        {
                            const int At = (int)N["LAYERS"].size() - 1;
                            D.Replace(V.Doc, std::move(N));
                            S.Open.insert(V.Handle + "|L" + std::to_string(At));
                        }
                    }
                    else if (Fx.Action.rfind("remove_layer:", 0) == 0)
                    {
                        json N = D.Node(V.Doc);
                        const size_t At = std::strtoul(Fx.Action.c_str() + 13, nullptr, 10);
                        if (N.contains("LAYERS") && N["LAYERS"].is_array() && At < N["LAYERS"].size())
                        { N["LAYERS"].erase(At); D.Replace(V.Doc, std::move(N)); }
                    }
                    else if (Fx.Action.rfind("keep_name:", 0) == 0)
                    {
                        json N = D.Node(V.Doc);
                        if (PkgGraph::NameKeepApart(N, Fx.Action.substr(10))) D.Replace(V.Doc, std::move(N));
                    }
                    else if (Fx.Action.rfind("open_entry:", 0) == 0)
                    {
                        const std::string K = Fx.Action.substr(11);
                        S.Open.insert(V.Handle + "|" + K.substr(0, K.find('/')));   // its layer, then the entry
                        S.Open.insert(V.Handle + "|" + K);
                    }
                    else S.Pending = {V.Handle, Fx.Action};
                }
                ImGui::PopID();
            }
        }
    json Work = D.Node(V.Doc);                            // edited here, handed back only if something changed
    bool Dirty = false;
    std::vector<RowSpan> MyRows;
    if (Busy) ImGui::BeginDisabled();
    if (Work.is_object())
    {
        PkgForm::Host H;
        H.Z = Z;
        H.PadRight = kPadX;
        S.FoldNode = V.Handle;
        H.Fold = [&S](const std::string &Key, const std::string &Label) {
            const std::string K = S.FoldNode + "|" + Key;
            bool Was = S.Open.count(K) > 0;
            if (!Was && S.ExpandAll.count(S.FoldNode + "|")) { S.Open.insert(K); Was = true; }
            ImGui::AlignTextToFramePadding();
            ImGui::SetNextItemOpen(Was, ImGuiCond_Always);
            const bool Now = ImGui::TreeNodeEx((Label + "###" + Key).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (Now != Was) { if (Now) S.Open.insert(K); else S.Open.erase(K); }
            return Now;
        };
        H.RefLabel = [&S](const std::string &Ref) {
            if (const auto It = S.Labels.find(Ref); It != S.Labels.end()) return It->second;
            if (const auto It = S.ExtCache.find(Ref); It != S.ExtCache.end()) return It->second.Label;
            return std::string();
        };
        H.Facets = &S.Facets;
        H.Platforms = [&S]() -> const std::vector<std::string> & {
            if (!S.PlatformsAsked && S.PlatformsFn) S.Platforms = S.PlatformsFn();
            S.PlatformsAsked = true;
            return S.Platforms;
        };
        H.Dirty = [&Dirty] { Dirty = true; };
        const std::string Handle = V.Handle;
        H.Request = [&S, Handle](const std::string &A) { S.Pending = {Handle, A}; };
        std::map<std::string, std::vector<PkgGraph::RegRow>> Reg;   // this node's slice of the edit buffers
        const std::string Pre = V.Handle + "|";
        for (auto It = S.RegBuf.lower_bound(Pre); It != S.RegBuf.end() && It->first.rfind(Pre, 0) == 0; ++It) Reg[It->first.substr(Pre.size())] = It->second;
        H.RegBuf = &Reg;
        H.Node = V.Handle;
        H.Row = [&MyRows, WinY = WinPos.y, Z](int L, float Y0, float Y1) { MyRows.push_back({L, (Y0 - WinY) / Z, (Y1 - WinY) / Z}); };
        H.LayerDropped = [&S, Handle](const std::string &From, int FromLayer, int At, bool Copy) { S.Drop = {From, FromLayer, Handle, At, Copy}; };
        PkgForm::Body(Work, H);
        for (auto It = S.RegBuf.lower_bound(Pre); It != S.RegBuf.end() && It->first.rfind(Pre, 0) == 0;) It = S.RegBuf.erase(It);
        for (auto &[K, Rows] : Reg) S.RegBuf[Pre + K] = std::move(Rows);
    }
    else
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("This node is not a JSON object - fix it in the JSON view.");
    }
    if (Busy) ImGui::EndDisabled();
    S.Rows[V.Handle] = std::move(MyRows);
    //While a layer is being dragged, the node's foot takes it as its last layer (a row takes it before or after itself).
    if (const ImGuiPayload *Pl = ImGui::GetDragDropPayload(); Pl && Pl->IsDataType(PkgForm::kLayerPayload) && !Busy)
    {
        const json &Nn = D.Node(V.Doc);
        const int Count = Nn.is_object() && Nn.contains("LAYERS") && Nn["LAYERS"].is_array() ? (int)Nn["LAYERS"].size() : 0;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.29f, 0.12f, 0.9f));
        ImGui::Button("drop here: the last layer##layerfoot", ImVec2(Wd - 2.0f * kPadX * Z, 0));
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload *P = ImGui::AcceptDragDropPayload(PkgForm::kLayerPayload))
            {
                const auto *Lp = static_cast<const PkgForm::LayerPayload *>(P->Data);
                S.Drop = {Lp->Node, Lp->Layer, V.Handle, Count, ImGui::GetIO().KeyCtrl};
            }
            ImGui::EndDragDropTarget();
        }
    }
    S.ExpandAll.erase(V.Handle + "|");                   // "open all" unfolds everything in one frame
    if (Dirty && Work.is_object()) D.Replace(V.Doc, std::move(Work));

    // ---- footer: add a layer, actions, or what is running ----
    if (Busy)
    {
        const auto &Bz = S.Running[V.Handle];
        const float Frac = Bz.Frac >= 0.0f ? Bz.Frac : 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 3.0f);
        const std::string Txt = (Bz.What + (Bz.Detail.isEmpty() ? QString() : " - " + Bz.Detail)).toStdString();
        ImGui::ProgressBar(Frac, ImVec2(Wd - 2.0f * kPadX * Z - (Bz.Cancellable ? 70.0f * Z : 0.0f), 0), Txt.c_str());
        if (Bz.Cancellable) { ImGui::SameLine(); if (ImGui::Button("cancel")) emit Self.cancelRequested(QString::fromStdString(V.Handle)); }
    }
    if (ImGui::Button("+ layer")) ImGui::OpenPopup("##addlayer");
    if (ImGui::BeginPopup("##addlayer"))
    {
        std::string T;
        if (TypePicker(T, false))
        {
            json N = D.Node(V.Doc);
            if (PkgGraph::AddLayer(N, T))
            {
                const int At = N.contains("LAYERS") && N["LAYERS"].is_array() ? (int)N["LAYERS"].size() - 1 : 0;
                D.Replace(V.Doc, std::move(N));
                S.Open.insert(V.Handle + "|L" + std::to_string(At));   // a new layer opens, ready to fill in
            }
        }
        ImGui::EndPopup();
    }
    const std::vector<std::string> NoHints;
    const auto Hi = S.Hints.find(V.Handle);
    std::vector<PkgGraph::NodeRef> Refs;
    const std::vector<PkgGraph::Action> Acts = Busy ? std::vector<PkgGraph::Action>() :
        PkgGraph::ActionsFor(D.Node(V.Doc), [&] {
            if (PkgGraph::ContentType(D.Node(V.Doc)) != "ZIP") return false;
            for (int I = 0; I < D.Count(); ++I) Refs.push_back({D.Handle(I), &D.Node(I)});
            return !PkgGraph::DeltaBase(Refs, V.Doc).empty();
        }(), Hi == S.Hints.end() ? NoHints : Hi->second);
    if (!Acts.empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("actions...")) ImGui::OpenPopup("##actions");
        if (ImGui::BeginPopup("##actions"))
        {
            for (const PkgGraph::Action &A : Acts)
            {
                //Recorded, not invoked: an action opens dialogs, and a nested Qt event loop must not run inside a frame.
                if (ImGui::Selectable(A.Label)) S.Pending = {V.Handle, A.Id};
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", A.Tip);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::Dummy(ImVec2(0, kPadY * Z - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Unindent(kPadX * Z);

    //Selection outline and the ports, over the border (the draw list's clip was widened above).
    const float Hh = ImGui::GetWindowHeight();
    if (Selected || S.DropTarget == V.Handle)
        DL->AddRect(ImVec2(WinPos.x - 1.5f, WinPos.y - 1.5f), ImVec2(WinPos.x + Wd + 1.5f, WinPos.y + Hh + 1.5f),
                    S.DropTarget == V.Handle ? IM_COL32(120, 220, 140, 255) : IM_COL32(255, 196, 90, 255), 8.0f * Z, 0, 2.5f);
    const ImVec2 In(WinPos.x, WinPos.y + Th * 0.5f), Out(WinPos.x + Wd, WinPos.y + Th * 0.5f);
    const ImVec2 M = ImGui::GetIO().MousePos;
    auto Near = [&](ImVec2 P) { return std::hypot(M.x - P.x, M.y - P.y) <= std::max(9.0f, kPortR * Z * 1.6f); };
    DrawPort(DL, In, std::max(3.5f, kPortR * Z), IM_COL32(150, 190, 255, 255), Near(In));
    //A socket on every row that holds a reference, in its relation's colour: the wire plugs in exactly there.
    if (const json &Nn = D.Node(V.Doc); Nn.is_object() && Nn.contains("LAYERS") && Nn["LAYERS"].is_array())
        for (const RowSpan &Rw : S.Rows[V.Handle])
        {
            if (Rw.Layer < 0 || Rw.Layer >= (int)Nn["LAYERS"].size()) continue;
            const std::string T = PkgGraph::LayerType(Nn["LAYERS"][(size_t)Rw.Layer]);
            if (T != "NODE" && T != "ANY" && T != "NOT") continue;
            const ImU32 C = T == "NOT" ? IM_COL32(230, 110, 110, 255) : T == "ANY" ? IM_COL32(120, 205, 140, 255) : IM_COL32(110, 160, 255, 255);
            const ImVec2 P(WinPos.x, WinPos.y + (Rw.Y0 + Rw.Y1) * 0.5f * Z);
            const bool Lit = (S.HoverWireChild == V.Handle || S.SelWireChild == V.Handle) && [&] {
                for (const WireView &W : S.Wires)
                    if (S.Views[(size_t)W.To].Handle == V.Handle && W.L.Layer == Rw.Layer
                        && (W.L.Slot == S.HoverWireSlot || W.L.Slot == S.SelWireSlot)) return true;
                return false;
            }();
            DrawPort(DL, P, std::max(2.5f, kPortR * 0.75f * Z), C, Lit);
            if (Lit) DL->AddRect(ImVec2(WinPos.x + 2.0f, WinPos.y + Rw.Y0 * Z), ImVec2(WinPos.x + Wd - 2.0f, WinPos.y + Rw.Y1 * Z), C, 4.0f * Z, 0, 1.5f);
        }
    //Files from the file manager over this node: it is where they would land.
    if (S.FileDropHover && S.HoverNode == V.Handle)
    {
        DL->AddRect(ImVec2(WinPos.x - 1.5f, WinPos.y - 1.5f), ImVec2(WinPos.x + Wd + 1.5f, WinPos.y + Hh + 1.5f), IM_COL32(120, 220, 140, 255), 8.0f * Z, 0, 3.0f);
        ImGui::SetTooltip("Drop to add: a game's folder or zip (its files), an image (the cover), a .reg export");
    }
    //A wire is being dragged: where it cannot go (a cycle, itself) is dimmed; where it can, outlined.
    if (S.M == PkgCanvasState::Mode::Link && V.Handle != S.LinkHandle)
    {
        if (S.LinkBlocked.count(V.Handle))
            DL->AddRectFilled(WinPos, ImVec2(WinPos.x + Wd, WinPos.y + Hh), IM_COL32(12, 13, 16, 170), 7.0f * Z);
        else if (S.DropTarget != V.Handle)
            DL->AddRect(WinPos, ImVec2(WinPos.x + Wd, WinPos.y + Hh), IM_COL32(120, 220, 140, 110), 7.0f * Z, 0, 1.5f);
    }
    DrawPort(DL, Out, std::max(3.5f, kPortR * Z), V.Unwired ? IM_COL32(150, 150, 160, 255) : IM_COL32(150, 190, 255, 255), Near(Out));
    DL->PopClipRect();

    S.DrawnRect[V.Handle] = ImVec4(WinPos.x, WinPos.y, WinPos.x + Wd, WinPos.y + Hh);
    const float WorldH = Hh / Z;
    //A self-sizing window knows its height only once it has laid its content out: not on the frame it (re)appears,
    //nor while it is still hidden for measuring or auto-fitting. Measured then, a node is a few pixels tall and the
    //layout stacks everything on top of everything.
    const ImGuiWindow *Me0 = ImGui::GetCurrentWindow();
    const bool Settled = !Me0->Appearing && Me0->HiddenFramesCannotSkipItems <= 0 && Me0->AutoFitFramesY <= 0;
    ImGui::EndChild();
    St.Pop();

    //The drawn height, for the layout and the culling. The first measurement corrects the estimate the layout used.
    if (!Settled) return;
    auto &Me = S.Measured[V.Handle];
    if (std::abs(Me - WorldH) > 0.5f)
    {
        Me = WorldH;
        if (!S.MeasuredOnce.count(V.Handle) && !D.Positions().count(V.Handle) && S.M == PkgCanvasState::Mode::Idle && S.AutoFrame)
        { S.LayoutValid = false; S.FullLayout = true; }
    }
    S.MeasuredOnce.insert(V.Handle);
}

// ---- a node as a box (zoomed out) or another package's node (a chip): drawn, hit-tested by hand ------------------

static void DrawBox(PkgCanvasState &S, ImDrawList *DL, const NodeView &V)
{
    const ImVec4 WR = WorldRect(S, V);
    const ImVec2 A = S.W2S(ImVec2(WR.x, WR.y)), B = S.W2S(ImVec2(WR.z, WR.w));
    const float Z = S.Z;
    const bool Sel = S.Sel.count(V.Handle) != 0, Hot = S.HoverNode == V.Handle;
    if (V.Doc < 0)
    {
        //A chip: another package's node. Dashed-looking border, package name above its label.
        DL->AddRectFilled(A, B, IM_COL32(40, 42, 50, 240), 7.0f * Z);
        DL->AddRect(A, B, Sel ? IM_COL32(255, 196, 90, 255) : Hot ? IM_COL32(170, 180, 200, 255) : IM_COL32(95, 102, 118, 255),
                    7.0f * Z, 0, Sel ? 2.5f : 1.2f);
        //Zoomed out the chip's name is what matters: its own font would be unreadable, so the name alone is drawn at a
        //readable size (as a box's is), clipped to the chip.
        DL->PushClipRect(A, B, true);
        //The game's cover at the chip's left, when its node presents one: "contains Black & White 2" reads at a glance.
        float TextX = 10.0f * Z;
        if (const auto Ec = S.ExtCache.find(V.Handle); Ec != S.ExtCache.end() && !Ec->second.Cover.empty() && S.Image)
        {
            unsigned long long Tex = 0;
            float Iw = 0, Ih = 0;
            if (S.Image(Ec->second.Cover, Tex, Iw, Ih) && Iw > 0 && Ih > 0)
            {
                const float H = (B.y - A.y) - 6.0f * Z, W = H * Iw / Ih;
                DL->AddImageRounded(ImTextureRef((ImTextureID)Tex), ImVec2(A.x + 3.0f * Z, A.y + 3.0f * Z), ImVec2(A.x + 3.0f * Z + W, A.y + 3.0f * Z + H),
                                    ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f * Z);
                TextX = W + 10.0f * Z;
                S.CoversDrawn.insert(V.Handle);
            }
        }
        if (S.Z >= kLodZoom)
        {
            ImGui::PushFont(nullptr, kFont * 0.8f * Z);
            const std::string P = PkgForm::FitWidth("\xE2\x86\x97 " + V.Package, (B.x - A.x) - TextX - 10.0f * Z);
            DL->AddText(ImVec2(A.x + TextX, A.y + 5.0f * Z), IM_COL32(150, 158, 175, 255), P.c_str());
            ImGui::PopFont();
            ImGui::PushFont(GBold, kFont * Z);
            const std::string T = PkgForm::FitWidth(V.Title, (B.x - A.x) - TextX - 10.0f * Z);
            DL->AddText(ImVec2(A.x + TextX, A.y + 21.0f * Z), IM_COL32(230, 233, 240, 255), T.c_str());
            ImGui::PopFont();
        }
        else
        {
            const float Fs = std::max(10.0f, std::min(kFont * 2.2f * Z, 22.0f));
            ImGui::PushFont(GBold, Fs);
            const std::string T = PkgForm::FitWidth(V.Title, (B.x - A.x) - 8.0f);
            DL->AddText(ImVec2(A.x + 4.0f, A.y + ((B.y - A.y) - Fs) * 0.5f), IM_COL32(215, 220, 230, 255), T.c_str());
            ImGui::PopFont();
        }
        DL->PopClipRect();
        DrawPort(DL, ImVec2(B.x, A.y + kChipH * 0.5f * Z), std::max(3.0f, kPortR * Z), IM_COL32(150, 190, 255, 255), false);
        S.DrawnRect[V.Handle] = ImVec4(A.x, A.y, B.x, B.y);
        return;
    }
    int R, G, Bc;
    PkgGraph::TypeColour(V.Kind, R, G, Bc);
    const float Th = kTitleH * Z;
    DL->AddRectFilled(A, B, IM_COL32(34, 36, 42, 245), 7.0f * Z);
    DL->AddRectFilled(A, ImVec2(B.x, A.y + Th), Col(R, G, Bc), 7.0f * Z, ImDrawFlags_RoundCornersTop);
    //Zoomed out the name is what matters: drawn larger than the node's own font would be, over the whole box.
    const float Fs = std::max(10.0f, std::min(kFont * 2.2f * Z, 26.0f));
    ImGui::PushFont(GBold, Fs);
    const std::string T = PkgForm::FitWidth(V.Title, (B.x - A.x) - 12.0f);
    const float Ty = (B.y - A.y) > Fs * 2.0f ? A.y + std::max(Th, Fs * 1.1f) : A.y + 2.0f;
    DL->PushClipRect(A, B, true);
    DL->AddText(ImVec2(A.x + 6.0f, Ty), IM_COL32(235, 238, 245, 255), T.c_str());
    ImGui::PopFont();
    //Under the name, when the box has room for a line more: what the node is made of.
    if (const float Ss = std::max(9.0f, Fs * 0.62f); !V.Summary.empty() && Ty + Fs * 1.25f + Ss < B.y)
    {
        ImGui::PushFont(nullptr, Ss);
        const std::string Sm = PkgForm::FitWidth(V.Summary, (B.x - A.x) - 12.0f);
        DL->AddText(ImVec2(A.x + 6.0f, Ty + Fs * 1.25f), IM_COL32(160, 168, 185, 255), Sm.c_str());
        ImGui::PopFont();
    }
    DL->PopClipRect();
    if (Sel || Hot) DL->AddRect(A, B, Sel ? IM_COL32(255, 196, 90, 255) : IM_COL32(170, 180, 200, 200), 7.0f * Z, 0, 2.0f);
    S.DrawnRect[V.Handle] = ImVec4(A.x, A.y, B.x, B.y);
}

// ---- wires -------------------------------------------------------------------------------------------------------

struct WireGeom { ImVec2 A, B, C, D; };

static WireGeom WireShape(const PkgCanvasState &S, const NodeView &From, const NodeView &To, int Layer)
{
    const ImVec2 A = PortScreen(S, From, true), D = RowPort(S, To, Layer);
    const float Dx = std::max(std::abs(D.x - A.x) * 0.5f, 60.0f * S.Z);
    return WireGeom{A, ImVec2(A.x + Dx, A.y), ImVec2(D.x - Dx, D.y), D};
}

static float WireDistance(const WireGeom &W, ImVec2 P)
{
    float Best = FLT_MAX;
    ImVec2 Prev = W.A;
    for (int K = 1; K <= 24; ++K)
    {
        const ImVec2 Q = Bez(W.A, W.B, W.C, W.D, (float)K / 24.0f);
        Best = std::min(Best, DistToSegment(P, Prev, Q));
        Prev = Q;
    }
    return Best;
}

static void DrawWires(PkgCanvasState &S, ImDrawList *DL, bool Hover)
{
    S.VisibleWires = 0;
    S.HoverWireSlot = -1;
    S.HoverWireChild.clear();
    const ImVec4 View(S.Canvas.x - 40, S.Canvas.y - 40, S.Canvas.z + 40, S.Canvas.w + 40);
    const ImVec2 M = ImGui::GetIO().MousePos;
    //One pass over the wires to find the ones any part of which can be on screen (by their control polygon's box),
    //then hover and drawing over those alone: a big package has tens of thousands of wires and a handful in view.
    struct Shown { const WireView *W; WireGeom G; };
    std::vector<Shown> Vis;
    for (const WireView &W : S.Wires)
    {
        const WireGeom G = WireShape(S, S.Views[(size_t)W.From], S.Views[(size_t)W.To], W.L.Layer);
        const ImVec4 Bb(std::min({G.A.x, G.B.x, G.C.x, G.D.x}), std::min({G.A.y, G.B.y, G.C.y, G.D.y}),
                        std::max({G.A.x, G.B.x, G.C.x, G.D.x}), std::max({G.A.y, G.B.y, G.C.y, G.D.y}));
        if (Overlaps(Bb, View)) Vis.push_back({&W, G});
    }
    S.VisibleWires = (int)Vis.size();
    //Each wire's place among the wires leaving its node, in the document's order: what spreads their colours.
    std::map<const WireView *, int> Rank;
    {
        std::map<int, int> Seen;
        for (const WireView &W : S.Wires) Rank[&W] = Seen[W.From]++;
    }
    if (Hover)
    {
        float BestD = 7.0f;
        for (const Shown &X : Vis)
        {
            const float Dd = WireDistance(X.G, M);
            if (Dd < BestD) { BestD = Dd; S.HoverWireChild = S.Views[(size_t)X.W->To].Handle; S.HoverWireSlot = X.W->L.Slot; }
        }
    }
    for (const Shown &X : Vis)
    {
        const WireView &W = *X.W;
        const WireGeom &G = X.G;
        const NodeView &F = S.Views[(size_t)W.From], &T = S.Views[(size_t)W.To];
        const bool Sel = S.SelWireChild == T.Handle && S.SelWireSlot == W.L.Slot;
        const bool Hot = S.HoverWireChild == T.Handle && S.HoverWireSlot == W.L.Slot;
        const bool Lit = Sel || Hot || S.Sel.count(F.Handle) || S.Sel.count(T.Handle);
        ImU32 C = W.L.Not ? IM_COL32(230, 110, 110, 255) : WireHue(F.Title, Rank[X.W]);
        if (!Lit) C = (C & 0x00FFFFFF) | (170u << 24);
        if (Sel) C = IM_COL32(255, 196, 90, 255);
        const float Th = std::max(1.2f, (Lit ? 2.6f : 1.8f) * std::sqrt(S.Z));
        //Segments by the curve's size ON SCREEN (about one per 18px, 4..40): imgui's default subdivides by its full
        //length, so a long wire crossing the view — a big package has thousands — cost thousands of segments each.
        const float Len = std::hypot(G.B.x - G.A.x, G.B.y - G.A.y) + std::hypot(G.C.x - G.B.x, G.C.y - G.B.y) + std::hypot(G.D.x - G.C.x, G.D.y - G.C.y);
        const int Segs = std::clamp((int)(Len / 18.0f), 4, 40);
        if (W.L.Any)
        {
            //Dashed: a requirement (one of these must be present), not containment.
            const int Dash = Segs + (Segs % 2);
            ImVec2 Prev = G.A;
            for (int K = 1; K <= Dash; ++K)
            {
                const ImVec2 Q = Bez(G.A, G.B, G.C, G.D, (float)K / (float)Dash);
                if (K % 2) DL->AddLine(Prev, Q, C, Th);
                Prev = Q;
            }
        }
        else DL->AddBezierCubic(G.A, G.B, G.C, G.D, C, Th, Segs);
        Arrow(DL, G.D, Bez(G.A, G.B, G.C, G.D, 0.93f), std::max(5.0f, 9.0f * S.Z), C);
        if (W.L.Not)
        {
            const ImVec2 Mid = Bez(G.A, G.B, G.C, G.D, 0.5f);
            const float R = std::max(4.0f, 6.0f * S.Z);
            DL->AddCircleFilled(Mid, R + 2, IM_COL32(24, 26, 31, 255));
            DL->AddLine(ImVec2(Mid.x - R * 0.6f, Mid.y - R * 0.6f), ImVec2(Mid.x + R * 0.6f, Mid.y + R * 0.6f), C, 2.0f);
            DL->AddLine(ImVec2(Mid.x - R * 0.6f, Mid.y + R * 0.6f), ImVec2(Mid.x + R * 0.6f, Mid.y - R * 0.6f), C, 2.0f);
        }
    }
}

// ================================================================================================================
// The frame
// ================================================================================================================

//The "+ Node" menu (the toolbar's, and the canvas's right-click "Add node here"): a type, or a recipe for this game
//(another version, a mod), placed at At — or beside the game it builds on — where it hides nothing, then opened to fill in.
void PkgCanvas::addMenu(ImVec2 At)
{
    PkgCanvasState &S = *m_s;
    Document &D = *S.Doc;
    std::string Type;
    //The game a recipe builds on: the selected node of this package, else its first version.
    std::string Base;
    if (const std::string Sh = selectedHandle(); !Sh.empty() && D.IndexOf(Sh) >= 0) Base = Sh;
    for (int I = 0; I < D.Count() && Base.empty(); ++I) if (!StrOf(D.Node(I), "VARIANT").empty()) Base = D.Handle(I);
    //No version yet: the game's files with its tile at the base (an installer captured, a disc's install) are what
    //the first version builds on.
    for (int I = 0; I < D.Count() && Base.empty(); ++I) if (HasTile(D.Node(I))) Base = D.Handle(I);
    const std::string BaseLabel = Base.empty() ? std::string() : NodeTitle(D.Node(D.IndexOf(Base)), Base);
    json Recipe;
    const bool Typed = TypePicker(Type, true);
    ImGui::SeparatorText("For this game");
    ImGui::BeginDisabled(Base.empty());
    if (ImGui::Selectable("another version")) Recipe = PkgGraph::VersionNode(Base, BaseLabel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Base.empty() ? "Make the game's first version first" : ("A source port, a patch, a fan update: contains \"" + BaseLabel + "\" and runs its own program").c_str());
    if (ImGui::Selectable("a mod for it")) Recipe = PkgGraph::ModNode(Base, BaseLabel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Base.empty() ? "Make the game's first version first" : ("Rides on \"" + BaseLabel + "\": ticked in the pre-launch window, its files over the game's").c_str());
    ImGui::EndDisabled();
    if (!Typed && Recipe.is_null()) return;
    //A version or a mod goes beside the game it builds on; any new node goes where it hides nothing.
    if (!Recipe.is_null() && S.ViewOf.count(Base))
    {
        const ImVec4 Br = WorldRect(S, S.Views[(size_t)S.ViewOf[Base]]);
        At = ImVec2(Br.z + 60.0f, Br.y);
    }
    At = FreeSpot(S, At);
    //A node made from the menu is ready to fill in: its rows open, its name selected for typing.
    int I;
    if (Recipe.is_null()) I = addNode(Type, At.x, At.y, selectedHandle());
    else { I = D.Add(std::move(Recipe)); D.SetPos(D.Handle(I), {At.x, At.y}); D.Commit(); emit documentEdited(); }
    const std::string Hn = D.Handle(I);
    setExpanded(Hn, true);
    //It is the node being worked on: selected (the JSON pane follows), and in view - a free spot can be off-screen.
    S.Sel = {Hn};
    S.SelWireSlot = -1;
    const float CW = (S.Canvas.z - S.Canvas.x) / S.Z, CH = (S.Canvas.w - S.Canvas.y) / S.Z;
    if (At.x < S.Cam.x || At.y < S.Cam.y || At.x + kNodeW > S.Cam.x + CW || At.y + kTitleH * 4 > S.Cam.y + CH)
        S.Cam = ImVec2(At.x + kNodeW * 0.5f - CW * 0.5f, At.y - CH * 0.25f);
    S.Renaming = Hn;
    S.RenameBuf = S.RenameFrom = StrOf(D.Node(I), "LABEL");
    S.RenameFocus = true;
    ImGui::CloseCurrentPopup();
}

//The toolbar row and its search results. Returns where the canvas starts (its top, in screen space).
float PkgCanvas::drawToolbar()
{
    PkgCanvasState &S = *m_s;
    Document &D = *S.Doc;
    ImGuiIO &Io = ImGui::GetIO();
    ImGui::SetCursorPos(ImVec2(8, 6));
    ImGui::BeginGroup();
    if (ImGui::Button("+ Node")) ImGui::OpenPopup("##addnode");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a node (or right-click the canvas where it should go)");
    if (ImGui::BeginPopup("##addnode"))
    {
        const ImVec2 C = S.S2W(ImVec2((S.Canvas.x + S.Canvas.z) * 0.5f - kNodeW * 0.5f * S.Z, (S.Canvas.y + S.Canvas.w) * 0.4f));
        addMenu(C);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Library node")) { ImGui::OpenPopup("##offers"); S.OfferFilter.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bring in a node from another package (a runner, a library...) to wire into this one");
    if (ImGui::BeginPopup("##offers"))
    {
        ImGui::SetNextItemWidth(320);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##offerfilter", "search by name or package", &S.OfferFilter);
        const std::vector<Offer> All = S.Offers ? S.Offers() : std::vector<Offer>();
        std::string F = S.OfferFilter;
        std::transform(F.begin(), F.end(), F.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        int Shown = 0;
        ImGui::BeginChild("##offerlist", ImVec2(420, 300));
        for (const Offer &O : All)
        {
            if (S.ViewOf.count(O.Cid) && S.Views[(size_t)S.ViewOf[O.Cid]].Doc >= 0) continue;   // one of ours
            std::string Hay = O.Label + " " + O.Package + " " + O.Cid;
            std::transform(Hay.begin(), Hay.end(), Hay.begin(), [](unsigned char C) { return (char)std::tolower(C); });
            if (!F.empty() && Hay.find(F) == std::string::npos) continue;
            if (++Shown > 200) { ImGui::TextDisabled("(narrow the search)"); break; }
            if (ImGui::Selectable((O.Label + "##" + O.Cid).c_str()))
            {
                if (std::find(S.Offered.begin(), S.Offered.end(), O.Cid) == S.Offered.end()) S.Offered.push_back(O.Cid);
                S.ExtCache[O.Cid] = S.ExternalLookup ? S.ExternalLookup(O.Cid) : External{O.Label, O.Package, std::string(), std::string()};
                S.BuiltRev = 0;
                Rebuild(S);
                Layout(S);
                select(O.Cid, true);
                emit statusMessage("Drag a wire from its right-hand port into the node that should contain it");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(300); ImGui::TextDisabled("%s", O.Package.c_str());
        }
        if (!Shown) ImGui::TextDisabled(S.Offers ? "nothing matches" : "(no library available)");
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 18);
    ImGui::SetNextItemWidth(230);
    if (S.SearchFocus) { ImGui::SetKeyboardFocusHere(); S.SearchFocus = false; }
    const bool Enter = ImGui::InputTextWithHint("##find", "find a node (Ctrl+F)", &S.Search, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool SearchActive = ImGui::IsItemActive();
    const ImVec2 SearchBL(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
    std::vector<std::string> Matches;
    if (!S.Search.empty())
    {
        std::string F = S.Search;
        std::transform(F.begin(), F.end(), F.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        //Best first: the name exactly, then names that start with it, then names (or CIDs) that contain it.
        std::vector<std::pair<int, std::string>> Ranked;
        for (const NodeView &V : S.Views)
        {
            std::string T = V.Title;
            std::transform(T.begin(), T.end(), T.begin(), [](unsigned char C) { return (char)std::tolower(C); });
            const int Rank = T == F ? 0 : T.rfind(F, 0) == 0 ? 1 : T.find(F) != std::string::npos ? 2
                           : V.Handle.find(S.Search) != std::string::npos ? 3 : -1;
            if (Rank >= 0) Ranked.push_back({Rank, V.Handle});
        }
        std::stable_sort(Ranked.begin(), Ranked.end(), [](const auto &A, const auto &B) { return A.first < B.first; });
        for (const auto &[R, H] : Ranked) Matches.push_back(H);
    }
    if (Enter && !Matches.empty()) { select(Matches.front(), true); S.Search.clear(); }
    ImGui::SameLine(0, 18);
    if (ImGui::Button("-")) setZoom(S.Z / 1.25f);
    ImGui::SameLine(0, 2);
    if (ImGui::Button((std::to_string((int)std::lround(S.Z * 100.0f)) + "%").c_str())) setZoom(1.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom (mouse wheel). Click for 100%%.");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("+")) setZoom(S.Z * 1.25f);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) frameAll();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the whole package (F frames the selection)");
    ImGui::SameLine();
    if (ImGui::Button("Tidy")) tidyLayout();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget where nodes were dragged: lay the whole package out again (undoable)");
    ImGui::SameLine();
    ImGui::Checkbox("Map", &S.ShowMini);
    {
        const std::string Status = std::to_string(D.Count()) + " nodes" + (S.Sel.empty() ? std::string() : " - " + std::to_string(S.Sel.size()) + " selected");
        const float W = ImGui::CalcTextSize(Status.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, Io.DisplaySize.x - W - 12));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", Status.c_str());
    }
    ImGui::EndGroup();
    const float Top = ImGui::GetItemRectMax().y + 6.0f;   // where the canvas starts
    //The search results, as a list under the box.
    if ((SearchActive || !Matches.empty()) && !S.Search.empty())
    {
        ImGui::SetNextWindowPos(SearchBL);
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 0), ImVec2(500, 320));
        ImGui::Begin("##findresults", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                                                   | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_Tooltip);
        if (Matches.empty()) ImGui::TextDisabled("no node matches");
        for (size_t K = 0; K < Matches.size() && K < 30; ++K)
        {
            const NodeView &V = S.Views[(size_t)S.ViewOf[Matches[K]]];
            ImGui::TextUnformatted(V.Title.c_str());
            ImGui::SameLine(); ImGui::TextDisabled("%s", V.Doc < 0 ? V.Package.c_str() : V.Kind.c_str());
        }
        if (!Matches.empty()) ImGui::TextDisabled("Enter: go to the first");
        ImGui::End();
    }
    return Top;
}

//The minimap in the canvas's corner (R0-R1, screen space): every node, where the view is, click or drag to move the
//view. Its window joins NodeWindows (input over it is not the canvas's). True while the mouse is over it.
bool PkgCanvas::drawMinimap(ImVec2 R0, ImVec2 R1, ImVec2 Mouse, std::vector<ImGuiWindow *> &NodeWindows)
{
    PkgCanvasState &S = *m_s;
    S.MiniRect = ImVec4(0, 0, 0, 0);
    bool MiniHovered = false;
    const ImVec4 Wb = S.Views.empty() ? ImVec4() : BoundsOf(S, nullptr);
    //Ticked, the map shows — with everything in view too (where the view is, in the whole): a ticked "Map" with no map
    //read as broken.
    if (S.ShowMini && !S.Views.empty() && R1.x - R0.x > 400 && R1.y - R0.y > 300)
    {
        const float MW = 220, MH = 150;
        const ImVec2 M0(R1.x - MW - 12, R1.y - MH - 12);
        S.MiniRect = ImVec4(M0.x, M0.y, M0.x + MW, M0.y + MH);
        const float Sc = std::min((MW - 12) / std::max(1.0f, Wb.z - Wb.x), (MH - 12) / std::max(1.0f, Wb.w - Wb.y));
        const ImVec2 Off(M0.x + (MW - (Wb.z - Wb.x) * Sc) * 0.5f, M0.y + (MH - (Wb.w - Wb.y) * Sc) * 0.5f);
        auto ToMini = [&](ImVec2 W) { return ImVec2(Off.x + (W.x - Wb.x) * Sc, Off.y + (W.y - Wb.y) * Sc); };
        ImGui::SetCursorScreenPos(M0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##minimap", ImVec2(MW, MH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        NodeWindows.push_back(ImGui::GetCurrentWindow());
        ImDrawList *MD = ImGui::GetWindowDrawList();
        MD->AddRectFilled(M0, ImVec2(M0.x + MW, M0.y + MH), IM_COL32(16, 17, 21, 230), 6.0f);
        for (const NodeView &V : S.Views)
        {
            const ImVec4 R = WorldRect(S, V);
            const ImVec2 A = ToMini(ImVec2(R.x, R.y)), B = ToMini(ImVec2(R.z, R.w));
            int Rc, Gc, Bc;
            PkgGraph::TypeColour(V.Kind, Rc, Gc, Bc);
            MD->AddRectFilled(A, ImVec2(std::max(B.x, A.x + 1.5f), std::max(B.y, A.y + 1.5f)),
                              S.Sel.count(V.Handle) ? IM_COL32(255, 196, 90, 255) : V.Doc < 0 ? IM_COL32(90, 96, 110, 255) : Col(Rc, Gc, Bc, 230));
        }
        const ImVec2 VA = ToMini(S.S2W(R0)), VB = ToMini(S.S2W(R1));
        MD->AddRect(VA, VB, IM_COL32(255, 255, 255, 200), 0, 0, 1.5f);
        MD->AddRect(M0, ImVec2(M0.x + MW, M0.y + MH), IM_COL32(80, 88, 104, 255), 6.0f);
        ImGui::SetCursorScreenPos(M0);
        ImGui::InvisibleButton("##minidrag", ImVec2(MW, MH));
        MiniHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemActive())
        {
            const ImVec2 W(Wb.x + (Mouse.x - Off.x) / Sc, Wb.y + (Mouse.y - Off.y) / Sc);
            S.Cam = ImVec2(W.x - (R1.x - R0.x) * 0.5f / S.Z, W.y - (R1.y - R0.y) * 0.5f / S.Z);
            S.Zooming = false;
        }
        if (MiniHovered) ImGui::SetTooltip("Click or drag to move the view");
        ImGui::EndChild();
    }
    return MiniHovered;
}

void PkgCanvas::frame()
{
    PkgCanvasState &S = *m_s;
    Document &D = *S.Doc;
    if (S.BuiltRev != D.Revision()) Rebuild(S);
    if (!S.LayoutValid && S.M == PkgCanvasState::Mode::Idle) { Layout(S); if (S.AutoFrame) S.FramePending = true; }
    UpdateRects(S);
    S.DrawnRect.clear();
    S.CoversDrawn.clear();
    S.Animating = false;
    ImGuiIO &Io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(Io.DisplaySize);
    //The canvas pans in world space, never by window scroll: a focused item lower than the panel (a new node's rename
    //box) made ImGui scroll the window to it, and the toolbar row slid out of view for good.
    ImGui::SetNextWindowScroll(ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##pkgcanvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse
                                             | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    const float Top = drawToolbar();

    // ---- the canvas region ------------------------------------------------------------------------------------
    S.Canvas = ImVec4(0, Top, Io.DisplaySize.x, Io.DisplaySize.y);
    if (S.FramePending) frameAll();
    const ImVec2 R0(S.Canvas.x, S.Canvas.y), R1(S.Canvas.z, S.Canvas.w);
    ImDrawList *DL = ImGui::GetWindowDrawList();
    DL->AddRectFilled(R0, R1, IM_COL32(24, 25, 29, 255));

    //Zoom easing: the wheel sets a target; the view walks to it holding the point under the cursor still.
    if (S.Zooming)
    {
        const float Dt = std::clamp(Io.DeltaTime, 0.0f, 0.1f);
        S.Z += (S.ZTarget - S.Z) * (1.0f - std::exp(-Dt * 18.0f));
        if (std::abs(S.ZTarget - S.Z) < 0.0015f) { S.Z = S.ZTarget; S.Zooming = false; }
        S.Cam = ImVec2(S.ZAnchorWorld.x - (S.ZAnchorScreen.x - S.Canvas.x) / S.Z, S.ZAnchorWorld.y - (S.ZAnchorScreen.y - S.Canvas.y) / S.Z);
        S.Animating = true;
    }

    //Background: one invisible button under everything; nodes are child windows on top of it and take their own input.
    ImGui::SetCursorScreenPos(R0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bg", ImVec2(std::max(1.0f, R1.x - R0.x), std::max(1.0f, R1.y - R0.y)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool BgHovered = ImGui::IsItemHovered();
    const ImGuiID BgId = ImGui::GetItemID();
    const ImVec2 Mouse = Io.MousePos;

    //Grid: world lines, doubling their spacing as you zoom out so it stays a texture, never a blur.
    DL->PushClipRect(R0, R1, true);
    {
        float Step = 32.0f;
        while (Step * S.Z < 18.0f) Step *= 2.0f;
        const ImU32 Minor = IM_COL32(255, 255, 255, 10), Major = IM_COL32(255, 255, 255, 22);
        const ImVec2 W0 = S.S2W(R0), W1 = S.S2W(R1);
        for (float X = std::floor(W0.x / Step) * Step; X <= W1.x; X += Step)
        {
            const float Sx = S.W2S(ImVec2(X, 0)).x;
            DL->AddLine(ImVec2(Sx, R0.y), ImVec2(Sx, R1.y), std::fmod(std::abs(X), Step * 4.0f) < 0.5f ? Major : Minor);
        }
        for (float Y = std::floor(W0.y / Step) * Step; Y <= W1.y; Y += Step)
        {
            const float Sy = S.W2S(ImVec2(0, Y)).y;
            DL->AddLine(ImVec2(R0.x, Sy), ImVec2(R1.x, Sy), std::fmod(std::abs(Y), Step * 4.0f) < 0.5f ? Major : Minor);
        }
    }

    //Hover among boxes and chips (drawn, not windows) is decided here; a node window claims it while drawn.
    const std::string HoverBefore = S.HoverNode;
    S.HoverNode.clear();
    const bool Lod = S.Z < kLodZoom;
    if (BgHovered)
        for (auto It = S.ZOrder.rbegin(); It != S.ZOrder.rend(); ++It)
        {
            const NodeView &V = S.Views[(size_t)S.ViewOf[*It]];
            if (!(Lod || V.Doc < 0)) continue;
            const ImVec4 WR = WorldRect(S, V);
            const ImVec2 A = S.W2S(ImVec2(WR.x, WR.y)), B = S.W2S(ImVec2(WR.z, WR.w));
            if (Mouse.x >= A.x && Mouse.x <= B.x && Mouse.y >= A.y && Mouse.y <= B.y) { S.HoverNode = V.Handle; break; }
        }
    //Ports: within reach of the cursor, a port wins over whatever it overlaps (it starts a wire).
    std::string PortHandle; bool PortOut = true;
    //(The background button being pressed does not count as busy: a port half outside its node is pressed THROUGH it.)
    if (S.M == PkgCanvasState::Mode::Idle && (!ImGui::IsAnyItemActive() || ImGui::GetActiveID() == BgId))
    {
        float Best = std::max(9.0f, kPortR * S.Z * 1.6f);
        const ImVec2 Mw = S.S2W(Mouse);
        const float Reach = Best / S.Z;
        for (const NodeView &V : S.Views)
        {
            const ImVec4 &Wr = WorldRect(S, V);
            if (Mw.x < Wr.x - Reach || Mw.x > Wr.z + Reach || Mw.y < Wr.y - Reach || Mw.y > Wr.w + Reach) continue;   // not near it
            for (bool Out : {true, false})
            {
                if (!Out && V.Doc < 0) continue;         // a chip only offers: it contains nothing here
                const ImVec2 P = PortScreen(S, V, Out);
                const float Dd = std::hypot(Mouse.x - P.x, Mouse.y - P.y);
                if (Dd < Best) { Best = Dd; PortHandle = V.Handle; PortOut = Out; }
            }
        }
            if (!PortHandle.empty()) ImGui::SetTooltip("%s", PortOut ? "Drag into the node that should contain this one"
                                                                   : "Drag onto the node this one should contain");
    }
    S.PortHover = PortHandle;

    DrawWires(S, DL, BgHovered && S.HoverNode.empty() && PortHandle.empty() && S.M == PkgCanvasState::Mode::Idle);
    if (!S.HoverWireChild.empty() && S.M == PkgCanvasState::Mode::Idle)
    {
        const auto Ci = S.ViewOf.find(S.HoverWireChild);
        if (Ci != S.ViewOf.end())
            for (const WireView &W : S.Wires)
                if (W.To == Ci->second && W.L.Slot == S.HoverWireSlot)
                {
                    const std::string &Pn = S.Views[(size_t)W.From].Title, &Cn = S.Views[(size_t)W.To].Title;
                    ImGui::SetTooltip("%s", (Cn + (W.L.Not ? " excludes " : W.L.Any ? " requires " : " contains ") + Pn
                                             + "\nclick to select - Del removes - right-click for more").c_str());
                }
    }

    // ---- nodes -------------------------------------------------------------------------------------------------
    //Node windows are clipped — drawn AND hit-tested — to the canvas region: imgui clips a child window by its parent's
    //clip rectangle when it begins, so without this a node scrolled up drew over (and took clicks from) the toolbar.
    ImGui::PushClipRect(R0, R1, true);
    std::vector<ImGuiWindow *> NodeWindows;
    int Visible = 0;
    const ImVec4 ViewW(S.S2W(R0).x - 20, S.S2W(R0).y - 20, S.S2W(R1).x + 20, S.S2W(R1).y + 20);
    for (const std::string &H : S.ZOrder)
    {
        const int Vi = S.ViewOf[H];
        const NodeView &V = S.Views[(size_t)Vi];
        //Culled by its real extent: a tall opened node stays drawn while any of it is on screen.
        if (!Overlaps(WorldRect(S, V), ViewW) && !(S.Renaming == H)) continue;
        ++Visible;
        if (V.Doc < 0 || Lod) DrawBox(S, DL, V);
        else
        {
            ImGui::PushID(Vi);
            DrawNode(*this, S, Vi, NodeWindows);
            ImGui::PopID();
            if (S.BuiltRev != D.Revision()) { Rebuild(S); break; }   // the node's own menu changed the graph
        }
    }
    S.VisibleNodes = Visible;
    ImGui::PopClipRect();
    if (S.HoverNode.empty() && !HoverBefore.empty() && S.M != PkgCanvasState::Mode::Idle) S.HoverNode = HoverBefore;

    //An empty package says what to do instead of showing nothing.
    if (S.Views.empty())
    {
        const char *Msg = S.PackageOpen ? "This package has no nodes yet." : "No package is open.";
        const char *Sub = S.PackageOpen ? "Right-click the canvas (or use + Node) to add one; + Library node brings in another package's."
                                        : "Start a new game's package, or open one from your library.";
        const ImVec2 M1 = ImGui::CalcTextSize(Msg), M2 = ImGui::CalcTextSize(Sub);
        const ImVec2 C((R0.x + R1.x) * 0.5f, (R0.y + R1.y) * 0.5f);
        DL->AddText(ImVec2(C.x - M1.x * 0.5f, C.y - M1.y - 4), IM_COL32(200, 205, 215, 255), Msg);
        DL->AddText(ImVec2(C.x - M2.x * 0.5f, C.y + 4), IM_COL32(140, 146, 160, 255), Sub);
        if (!S.PackageOpen)
        {
            const char *New = "New package...", *Open = "Open package...";
            const ImGuiStyle &St = ImGui::GetStyle();
            const float W1 = ImGui::CalcTextSize(New).x + St.FramePadding.x * 2, W2 = ImGui::CalcTextSize(Open).x + St.FramePadding.x * 2;
            ImGui::SetCursorScreenPos(ImVec2(C.x - (W1 + W2 + St.ItemSpacing.x) * 0.5f, C.y + M2.y + 18));
            if (ImGui::Button(New)) emit newPackageRequested();
            ImGui::SameLine();
            if (ImGui::Button(Open)) emit openPackagePickerRequested();
        }
    }

    // ---- the next step a game's package needs, until it has it ----
    if (S.PackageOpen && D.Count() > 0)
    {
        if (S.NextStepRev != D.ContentRevision())
        {
            std::vector<const json *> Ns;
            for (int I = 0; I < D.Count(); ++I) Ns.push_back(&D.Node(I));
            S.NextStep = PkgGraph::NextStep(Ns);
            S.NextStepRev = D.ContentRevision();
        }
        if (!S.NextStep.empty())
        {
            const std::string T = "Next: " + S.NextStep;
            const ImVec2 Ts = ImGui::CalcTextSize(T.c_str());
            const ImVec2 P(R0.x + 12.0f, R1.y - Ts.y - 20.0f);
            DL->AddRectFilled(ImVec2(P.x - 8, P.y - 5), ImVec2(P.x + Ts.x + 8, P.y + Ts.y + 5), IM_COL32(46, 118, 78, 230), 6.0f);
            DL->AddText(P, IM_COL32(240, 248, 240, 255), T.c_str());
        }
    }

    // ---- the first-time tour: one card at a time, in the canvas's middle ----
    if (S.TourStep >= 0)
    {
        static const char *Cards[][2] = {
            {"A package is one game", "Its files, how it runs, and its card on the shelf - built from nodes. Each node is a stack of layers, applied top to bottom: later wins."},
            {"Follow the green strip", "At the bottom left the canvas says what the package still needs, one step at a time, until the game is on the shelf."},
            {"Drop things on nodes", "Drag a game's folder or a zip from your file manager onto a node to bring its files in; drop an image to make it the cover, a .reg export for registry settings."},
            {"Add anything by typing", "Press / or double-click empty space, type what you want (\"registry\", \"saves\", \"version\"), Enter. Wires plug into the row where a node is contained: drag them between rows to choose where it folds in."},
            {"Try it", "The \xE2\x96\xB6 on a version runs it through the same pre-launch window players use. The tabs on the right show what the game will see, the fold in order, and the card."}};
        const int N = (int)(sizeof(Cards) / sizeof(Cards[0]));
        //High in the canvas: what it talks about (the empty canvas's buttons, the strip at the bottom) stays visible.
        const ImVec2 C((R0.x + R1.x) * 0.5f, R0.y + (R1.y - R0.y) * 0.08f);
        ImGui::SetNextWindowPos(C, ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(440, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.20f, 0.16f, 0.97f));
        if (ImGui::Begin("##tour", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextDisabled("%d of %d", S.TourStep + 1, N);
            ImGui::PushFont(GBold, kFont * 1.2f);
            ImGui::TextUnformatted(Cards[S.TourStep][0]);
            ImGui::PopFont();
            ImGui::PushTextWrapPos(420);
            ImGui::TextUnformatted(Cards[S.TourStep][1]);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            const bool Last = S.TourStep + 1 >= N;
            if (ImGui::Button(Last ? "Got it" : "Next")) { if (Last) { S.TourStep = -1; emit tourFinished(); } else ++S.TourStep; }
            if (!Last) { ImGui::SameLine(); if (ImGui::Button("Skip the tour")) { S.TourStep = -1; emit tourFinished(); } }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ---- overlays: the wire being dragged, the box being drawn ----
    ImDrawList *FG = ImGui::GetForegroundDrawList();
    FG->PushClipRect(R0, R1, true);
    S.DropTarget.clear();
    if (S.M == PkgCanvasState::Mode::Link)
    {
        const auto Vi = S.ViewOf.find(S.LinkHandle);
        if (Vi != S.ViewOf.end())
        {
            const ImVec2 A = PortScreen(S, S.Views[(size_t)Vi->second], S.LinkFromOut);
            const float Dx = std::max(std::abs(Mouse.x - A.x) * 0.5f, 50.0f * S.Z) * (S.LinkFromOut ? 1.0f : -1.0f);
            FG->AddBezierCubic(A, ImVec2(A.x + Dx, A.y), ImVec2(Mouse.x - Dx, Mouse.y), Mouse, IM_COL32(255, 210, 120, 255), 2.5f);
            const std::string T = S.HoverNode;
            S.LinkAt = -1;
            if (!T.empty() && T != S.LinkHandle && !S.LinkBlocked.count(T))
            {
                const NodeView &Tv = S.Views[(size_t)S.ViewOf[T]];
                if (S.LinkFromOut ? Tv.Doc >= 0 : true) S.DropTarget = T;
                //Dropped INTO a node: the reference goes between the rows the cursor is between, and a line says where.
                if (!S.DropTarget.empty() && S.LinkFromOut && Tv.Doc >= 0)
                {
                    const json &Nn = D.Node(Tv.Doc);
                    const int Count = Nn.is_object() && Nn.contains("LAYERS") && Nn["LAYERS"].is_array() ? (int)Nn["LAYERS"].size() : 0;
                    const ImVec4 Wr = WorldRect(S, Tv);
                    const float Off = S.S2W(Mouse).y - Wr.y;
                    const auto &Rs = S.Rows[T];
                    float LineY = -1.0f;
                    if (!Rs.empty())
                    {
                        S.LinkAt = Count;
                        LineY = Rs.back().Y1;
                        for (const RowSpan &R : Rs)
                            if (Off < (R.Y0 + R.Y1) * 0.5f) { S.LinkAt = R.Layer; LineY = R.Y0; break; }
                        FG->AddLine(S.W2S(ImVec2(Wr.x + 6.0f, Wr.y + LineY)), S.W2S(ImVec2(Wr.z - 6.0f, Wr.y + LineY)),
                                    IM_COL32(255, 210, 120, 255), 3.0f);
                    }
                }
            }
            std::string Tip;
            if (S.DropTarget.empty())
                Tip = !S.HoverNode.empty() && S.LinkBlocked.count(S.HoverNode) ? "Not here: a node never contains itself"
                    : S.HoverNode.empty() ? "Let go here to make a new node for it" : "Drop on a node";
            else
            {
                Tip = Io.KeyShift ? "requires one of" : Io.KeyAlt ? "excludes" : "contains (Shift: requires, Alt: excludes)";
                if (S.LinkAt >= 0) Tip += "\nas layer " + std::to_string(S.LinkAt + 1) + " - what comes later in a node wins";
            }
            ImGui::SetTooltip("%s", Tip.c_str());
        }
    }
    if (S.M == PkgCanvasState::Mode::Box)
    {
        FG->AddRectFilled(S.PressScreen, Mouse, IM_COL32(120, 170, 255, 40));
        FG->AddRect(S.PressScreen, Mouse, IM_COL32(120, 170, 255, 200));
    }
    FG->PopClipRect();
    DL->PopClipRect();

    const bool MiniHovered = drawMinimap(R0, R1, Mouse, NodeWindows);

    // ---- input ------------------------------------------------------------------------------------------------
    ImGuiContext &Ctx = *ImGui::GetCurrentContext();
    const bool OverCanvas = Mouse.x >= R0.x && Mouse.x < R1.x && Mouse.y >= R0.y && Mouse.y < R1.y;
    const bool CanvasOwnsHover = OverCanvas && Ctx.HoveredWindow &&
        (Ctx.HoveredWindow == ImGui::GetCurrentWindow() || std::find(NodeWindows.begin(), NodeWindows.end(), Ctx.HoveredWindow) != NodeWindows.end());
    //Wheel: zoom about the cursor — over the background or a node, not over a scrolling text box, a popup or the map.
    if (CanvasOwnsHover && !MiniHovered && Io.MouseWheel != 0.0f && Ctx.OpenPopupStack.Size == 0)
    {
        const float Nz = std::clamp(S.ZTarget * std::pow(1.15f, Io.MouseWheel), kMinZoom, kMaxZoom);
        if (Nz != S.ZTarget)
        {
            S.ZAnchorScreen = Mouse;
            S.ZAnchorWorld = S.S2W(Mouse);
            S.ZTarget = Nz;
            S.Zooming = true;
        }
    }
    //Any gesture on the canvas means the user has the view now: stop re-framing it for them.
    if (CanvasOwnsHover && (Io.MouseWheel != 0.0f || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)))
        S.AutoFrame = false;
    //Quick add: "/" over the canvas, or a double-click on its empty background - type what to add, Enter adds it there.
    if (S.M == PkgCanvasState::Mode::Idle && CanvasOwnsHover && !MiniHovered && Ctx.OpenPopupStack.Size == 0
        && ((BgHovered && S.HoverNode.empty() && ImGui::IsMouseDoubleClicked(0))
            || (!Io.WantTextInput && !ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_Slash, false))))
    {
        S.QuickText.clear();
        S.QuickWorld = S.S2W(Mouse);
        ImGui::OpenPopup("##quickadd");
    }
    //Starting a gesture.
    if (S.M == PkgCanvasState::Mode::Idle && CanvasOwnsHover && !MiniHovered)
    {
        if (!PortHandle.empty() && ImGui::IsMouseClicked(0))
        {
            S.M = PkgCanvasState::Mode::Link;
            S.LinkHandle = PortHandle;
            S.LinkFromOut = PortOut;
            //Out: the target will contain this node, so nothing this node contains may be it. In: this node will
            //contain the target, so nothing containing this node may be it.
            S.LinkBlocked = Reach(D, PortHandle, !PortOut);
            S.LinkBlocked.insert(PortHandle);
            ImGui::SetActiveID(BgId, ImGui::GetCurrentWindow());       // the canvas owns the drag, not a node widget
        }
        else if (ImGui::IsMouseClicked(2) || (ImGui::IsMouseClicked(0) && ImGui::IsKeyDown(ImGuiKey_Space)))
            S.M = PkgCanvasState::Mode::Pan;
        else if (BgHovered && ImGui::IsMouseClicked(0))
        {
            if (!S.HoverNode.empty())
            {
                //A box or a chip: select and drag it; double-click a chip to open its package, a box to zoom in on it.
                const NodeView &V = S.Views[(size_t)S.ViewOf[S.HoverNode]];
                if (Io.KeyCtrl) { if (!S.Sel.erase(V.Handle)) S.Sel.insert(V.Handle); }
                else if (!S.Sel.count(V.Handle)) S.Sel = {V.Handle};
                S.SelWireSlot = -1;
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    if (V.Doc < 0)
                    {
                        const auto E = S.ExtCache.find(V.Handle);
                        if (E != S.ExtCache.end() && !E->second.PackageDir.empty())
                            emit openPackageRequested(QString::fromStdString(E->second.PackageDir));
                    }
                    else select(V.Handle, true);
                }
                else { S.M = PkgCanvasState::Mode::Drag; S.DragDelta = ImVec2(0, 0); S.PressScreen = Mouse; }
            }
            else if (!S.HoverWireChild.empty())
            {
                S.SelWireChild = S.HoverWireChild;
                S.SelWireSlot = S.HoverWireSlot;
                S.Sel.clear();
            }
            else
            {
                if (!Io.KeyCtrl && !Io.KeyShift) { S.Sel.clear(); S.SelWireSlot = -1; }
                S.M = PkgCanvasState::Mode::Box;
                S.PressScreen = Mouse;
            }
        }
        else if (BgHovered && ImGui::IsMouseClicked(1)) { S.RightPressedOnBg = true; S.PressScreen = Mouse; }
    }
    //Right-drag on the background pans; a right click without a drag opens the menu.
    if (S.RightPressedOnBg && ImGui::IsMouseDown(1) && ImGui::IsMouseDragging(1, 4.0f)) { S.M = PkgCanvasState::Mode::Pan; S.RightPressedOnBg = false; }
    if (S.RightPressedOnBg && ImGui::IsMouseReleased(1))
    {
        S.RightPressedOnBg = false;
        S.ContextWorld = S.S2W(Mouse);
        if (!S.HoverWireChild.empty()) { S.SelWireChild = S.HoverWireChild; S.SelWireSlot = S.HoverWireSlot; ImGui::OpenPopup("##wiremenu"); }
        else if (!S.HoverNode.empty()) { if (!S.Sel.count(S.HoverNode)) S.Sel = {S.HoverNode}; ImGui::OpenPopup("##boxmenu"); }
        else ImGui::OpenPopup("##canvasmenu");
    }
    //Gestures in flight.
    switch (S.M)
    {
    case PkgCanvasState::Mode::Pan:
        S.Cam = ImVec2(S.Cam.x - Io.MouseDelta.x / S.Z, S.Cam.y - Io.MouseDelta.y / S.Z);
        S.Zooming = false;
        if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) S.M = PkgCanvasState::Mode::Idle;
        break;
    case PkgCanvasState::Mode::Drag:
        if (ImGui::IsMouseDown(0))
        {
            S.DragDelta = ImVec2((Mouse.x - S.PressScreen.x) / S.Z, (Mouse.y - S.PressScreen.y) / S.Z);
            //Auto-pan when dragging past the edge.
            const float Edge = 30.0f, Speed = 900.0f * Io.DeltaTime / S.Z;
            ImVec2 Pan(0, 0);
            if (Mouse.x < R0.x + Edge) Pan.x = -Speed; else if (Mouse.x > R1.x - Edge) Pan.x = Speed;
            if (Mouse.y < R0.y + Edge) Pan.y = -Speed; else if (Mouse.y > R1.y - Edge) Pan.y = Speed;
            if (Pan.x != 0 || Pan.y != 0) { S.Cam.x += Pan.x; S.Cam.y += Pan.y; S.PressScreen.x -= Pan.x * S.Z; S.PressScreen.y -= Pan.y * S.Z; }
        }
        else
        {
            if (std::abs(S.DragDelta.x) > 0.5f || std::abs(S.DragDelta.y) > 0.5f)
            {
                const ImVec2 Dd = S.DragDelta;
                S.M = PkgCanvasState::Mode::Idle;         // positions below are read without the drag applied
                for (const std::string &H : S.Sel)
                {
                    const auto It = S.ViewOf.find(H);
                    if (It == S.ViewOf.end()) continue;
                    const ImVec2 P = WorldPos(S, S.Views[(size_t)It->second]);
                    D.SetPos(H, {std::round(P.x + Dd.x), std::round(P.y + Dd.y)});
                }
                D.Commit();
                emit documentEdited();
            }
            S.M = PkgCanvasState::Mode::Idle;
            S.DragDelta = ImVec2(0, 0);
        }
        break;
    case PkgCanvasState::Mode::Box:
        if (!ImGui::IsMouseDown(0))
        {
            const ImVec2 A = S.S2W(ImVec2(std::min(S.PressScreen.x, Mouse.x), std::min(S.PressScreen.y, Mouse.y)));
            const ImVec2 B = S.S2W(ImVec2(std::max(S.PressScreen.x, Mouse.x), std::max(S.PressScreen.y, Mouse.y)));
            if (std::abs(Mouse.x - S.PressScreen.x) > 3 || std::abs(Mouse.y - S.PressScreen.y) > 3)
                for (const NodeView &V : S.Views)
                    if (Overlaps(WorldRect(S, V), ImVec4(A.x, A.y, B.x, B.y))) S.Sel.insert(V.Handle);
            S.M = PkgCanvasState::Mode::Idle;
        }
        break;
    case PkgCanvasState::Mode::Link:
        if (!ImGui::IsMouseDown(0))
        {
            if (!S.DropTarget.empty())
            {
                //Out-port → the drop target contains the source; in-port → the source contains the drop target.
                const std::string Child = S.LinkFromOut ? S.DropTarget : S.LinkHandle;
                const std::string Parent = S.LinkFromOut ? S.LinkHandle : S.DropTarget;
                const int Ci = D.IndexOf(Child);
                const Document::RefKind K = Io.KeyShift ? Document::RefKind::Any : Io.KeyAlt ? Document::RefKind::Not : Document::RefKind::Node;
                if (Ci >= 0 && D.Link(Ci, Parent, K, S.LinkFromOut ? S.LinkAt : -1))
                {
                    D.Commit();
                    S.Offered.erase(std::remove(S.Offered.begin(), S.Offered.end(), Parent), S.Offered.end());
                    emit documentEdited();
                }
                else if (Ci < 0) emit statusMessage("Another package's node cannot be changed here - wire it into one of this package's nodes");
                else emit statusMessage("Already wired");
            }
            else if (S.HoverNode.empty() && Mouse.x >= S.Canvas.x && Mouse.x <= S.Canvas.z && Mouse.y >= S.Canvas.y && Mouse.y <= S.Canvas.w)
            {
                //Let go on the empty canvas: a new node, already wired to where the wire came from.
                S.WireNewFrom = S.LinkHandle;
                S.WireNewOut = S.LinkFromOut;
                S.WireNewWorld = S.S2W(Mouse);
                ImGui::OpenPopup("##wirenew");
            }
            S.M = PkgCanvasState::Mode::Idle;
            S.LinkHandle.clear();
            ImGui::ClearActiveID();
        }
        break;
    case PkgCanvasState::Mode::Idle:
        break;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && S.M != PkgCanvasState::Mode::Idle)
    { S.M = PkgCanvasState::Mode::Idle; S.LinkHandle.clear(); S.DragDelta = ImVec2(0, 0); ImGui::ClearActiveID(); }

    // ---- menus ------------------------------------------------------------------------------------------------
    if (ImGui::BeginPopup("##quickadd"))
    {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(380);
        const bool QEnter = ImGui::InputTextWithHint("##quick", "add: a layer type, \"version\", \"mod\" or a library node", &S.QuickText,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        //Everything that can be added, each with the words it is found by; every typed word must be among them.
        struct Cand { std::string Shown, Hay, Type, Offer; int Recipe = 0; };
        std::vector<Cand> All;
        for (const PkgGraph::TypeGroup &G : PkgGraph::TypeGroups())
            for (const std::string &T : G.Types)
                All.push_back({T + "  -  " + PkgGraph::TypeGist(T), T + " " + PkgGraph::TypeGist(T) + " " + G.Title + " layer node", T, {}, 0});
        All.push_back({"empty node", "empty node nothing", "", {}, 0});
        std::string Base;
        if (const std::string Sh = selectedHandle(); !Sh.empty() && D.IndexOf(Sh) >= 0) Base = Sh;
        for (int I = 0; I < D.Count() && Base.empty(); ++I) if (!StrOf(D.Node(I), "VARIANT").empty()) Base = D.Handle(I);
        if (!Base.empty())
        {
            All.push_back({"another version of this game", "another version source port patch fan update recipe", {}, {}, 1});
            All.push_back({"a mod for this game", "mod graft recipe addon", {}, {}, 2});
        }
        std::string Q = S.QuickText;
        std::transform(Q.begin(), Q.end(), Q.begin(), [](unsigned char C) { return (char)std::tolower(C); });
        if (!Q.empty() && S.Offers)                       // the library only once something is typed: it is big
            for (const Offer &O : S.Offers()) All.push_back({O.Label + "  -  " + O.Package, O.Label + " " + O.Package + " library", {}, O.Cid, 0});
        std::vector<const Cand *> Hits;
        for (const Cand &C : All)
        {
            std::string Hay = C.Hay;
            std::transform(Hay.begin(), Hay.end(), Hay.begin(), [](unsigned char Ch) { return (char)std::tolower(Ch); });
            std::istringstream Words(Q);
            bool Ok = true;
            for (std::string W; Words >> W;) if (Hay.find(W) == std::string::npos) Ok = false;
            if (Ok) Hits.push_back(&C);
            if (Hits.size() >= 14) break;
        }
        const Cand *Pick = QEnter && !Hits.empty() ? Hits.front() : nullptr;
        for (const Cand *C : Hits) if (ImGui::Selectable((C->Shown + "##q" + C->Type + C->Offer + std::to_string(C->Recipe)).c_str())) Pick = C;
        if (Hits.empty()) ImGui::TextDisabled("nothing matches");
        if (Pick)
        {
            int I = -1;
            if (!Pick->Offer.empty())
            {
                if (std::find(S.Offered.begin(), S.Offered.end(), Pick->Offer) == S.Offered.end()) S.Offered.push_back(Pick->Offer);
                S.BuiltRev = 0;
                select(Pick->Offer, true);
                emit statusMessage("Drag a wire from its right-hand port into the node that should contain it");
            }
            else if (Pick->Recipe)
            {
                const std::string Bl = NodeTitle(D.Node(D.IndexOf(Base)), Base);
                I = D.Add(Pick->Recipe == 1 ? PkgGraph::VersionNode(Base, Bl) : PkgGraph::ModNode(Base, Bl));
                D.SetPos(D.Handle(I), {S.QuickWorld.x, S.QuickWorld.y});
                D.Commit();
                emit documentEdited();
            }
            else I = addNode(Pick->Type, S.QuickWorld.x, S.QuickWorld.y, selectedHandle());
            if (I >= 0)
            {
                const std::string Hn = D.Handle(I);
                setExpanded(Hn, true);
                S.Sel = {Hn};
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##wirenew"))
    {
        const auto Fi = S.ViewOf.find(S.WireNewFrom);
        const std::string Name = Fi == S.ViewOf.end() ? std::string("it") : "\"" + S.Views[(size_t)Fi->second].Title + "\"";
        ImGui::TextDisabled("%s", (std::string(S.WireNewOut ? "A new node containing " : "A new node inside ") + Name).c_str());
        std::string Type;
        if (Fi != S.ViewOf.end() && TypePicker(Type, true))
        {
            //A NODE made from a node's out-port IS the reference: filled with it, not a second layer beside an empty one.
            const bool Fills = S.WireNewOut && Type == "NODE";
            const int I = addNode(Type, S.WireNewWorld.x, S.WireNewWorld.y - kTitleH * 0.5f, Fills ? S.WireNewFrom : std::string());
            const std::string Hn = D.Handle(I);
            const int From = D.IndexOf(S.WireNewFrom);
            const bool Ok = Fills || (S.WireNewOut ? D.Link(I, S.WireNewFrom, Document::RefKind::Node) : From >= 0 && D.Link(From, Hn, Document::RefKind::Node));
            if (Ok) D.Commit();
            S.Offered.erase(std::remove(S.Offered.begin(), S.Offered.end(), S.WireNewFrom), S.Offered.end());
            setExpanded(Hn, true);
            S.Renaming = Hn;
            S.RenameBuf = S.RenameFrom = StrOf(D.Node(I), "LABEL");
            S.RenameFocus = true;
            S.WireNewFrom.clear();
            emit documentEdited();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##canvasmenu"))
    {
        if (ImGui::BeginMenu("Add node here")) { addMenu(S.ContextWorld); ImGui::EndMenu(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Fit the package in view", "F")) frameAll();
        if (ImGui::MenuItem("Tidy the layout")) tidyLayout();
        if (ImGui::MenuItem("Select all", "Ctrl+A")) for (const NodeView &V : S.Views) S.Sel.insert(V.Handle);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##boxmenu"))
    {
        const std::string H = S.Sel.size() == 1 ? *S.Sel.begin() : std::string();
        const auto It = S.ViewOf.find(H);
        const bool Chip = It != S.ViewOf.end() && S.Views[(size_t)It->second].Doc < 0;
        if (Chip)
        {
            const auto E = S.ExtCache.find(H);
            if (ImGui::MenuItem("Open its package", nullptr, false, E != S.ExtCache.end() && !E->second.PackageDir.empty()))
                emit openPackageRequested(QString::fromStdString(E->second.PackageDir));
            if (ImGui::MenuItem("Copy CID")) ImGui::SetClipboardText(H.c_str());
            if (std::find(S.Offered.begin(), S.Offered.end(), H) != S.Offered.end() && ImGui::MenuItem("Remove (not wired)")) removeNodes({H});
        }
        else
        {
            if (ImGui::MenuItem("Zoom to it")) select(H, true);
            if (ImGui::MenuItem("Delete", "Del")) removeNodes(S.Sel);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##wiremenu"))
    {
        const int Ci = D.IndexOf(S.SelWireChild);
        const WireView *W = nullptr;
        for (const WireView &X : S.Wires) if (S.Views[(size_t)X.To].Handle == S.SelWireChild && X.L.Slot == S.SelWireSlot) W = &X;
        if (W && Ci >= 0)
        {
            const std::string Parent = S.Views[(size_t)W->From].Handle;
            ImGui::TextDisabled("%s", (S.Views[(size_t)W->To].Title + " -> " + S.Views[(size_t)W->From].Title).c_str());
            ImGui::Separator();
            auto Retype = [&](Document::RefKind K) {
                json N = D.Node(Ci);
                if (PkgGraph::EraseRef(N, S.SelWireSlot)) { D.Replace(Ci, std::move(N)); D.Link(Ci, Parent, K); D.Commit(); emit documentEdited(); }
            };
            if (ImGui::MenuItem("contains it", nullptr, !W->L.Any && !W->L.Not) && (W->L.Any || W->L.Not)) Retype(Document::RefKind::Node);
            if (ImGui::MenuItem("requires one of (ANY)", nullptr, W->L.Any) && !W->L.Any) Retype(Document::RefKind::Any);
            if (ImGui::MenuItem("excludes it (NOT)", nullptr, W->L.Not) && !W->L.Not) Retype(Document::RefKind::Not);
            ImGui::Separator();
            if (ImGui::MenuItem("Go to the container")) select(S.Views[(size_t)W->To].Handle, true);
            if (ImGui::MenuItem("Go to the contained")) select(Parent, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Remove wire", "Del"))
            {
                json N = D.Node(Ci);
                if (PkgGraph::EraseRef(N, S.SelWireSlot)) { D.Replace(Ci, std::move(N)); D.Commit(); S.SelWireSlot = -1; emit documentEdited(); }
            }
        }
        else ImGui::TextDisabled("(this wire belongs to another package)");
        ImGui::EndPopup();
    }

    // ---- keyboard ---------------------------------------------------------------------------------------------
    const bool Typing = Io.WantTextInput || ImGui::IsAnyItemActive();
    if (!Typing && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        const bool Ctrl = Io.KeyCtrl;
        if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { if (Io.KeyShift) redo(); else undo(); }
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redo();
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) emit saveRequested();
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) S.SearchFocus = true;
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) for (const NodeView &V : S.Views) S.Sel.insert(V.Handle);
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_D))
        {
            std::set<std::string> New;
            for (const std::string &H : S.Sel)
            {
                const int I = D.IndexOf(H);
                if (I < 0) continue;
                json N = D.Node(I);
                if (N.is_object()) N["LABEL"] = FreeLabel(D, StrOf(N, "LABEL") + " copy");
                const ImVec2 P = WorldPos(S, S.Views[(size_t)S.ViewOf[H]]);
                const int J = D.Add(std::move(N));
                D.SetPos(D.Handle(J), {P.x + 40.0f, P.y + 40.0f});
                New.insert(D.Handle(J));
            }
            if (!New.empty()) { D.Commit(); S.Sel = New; emit documentEdited(); }
        }
        else if (Ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) setZoom(S.Z * 1.25f);
        else if (Ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))) setZoom(S.Z / 1.25f);
        else if (Ctrl && ImGui::IsKeyPressed(ImGuiKey_0)) setZoom(1.0f);
        else if (ImGui::IsKeyPressed(ImGuiKey_F)) frameSelection();
        else if (ImGui::IsKeyPressed(ImGuiKey_E) && !S.Sel.empty())
        {
            //Open every row of the selected nodes — or, when any is open already, fold them all.
            bool AnyOpen = false;
            for (const std::string &H : S.Sel)
            {
                const auto It = S.Open.lower_bound(H + "|");
                if (It != S.Open.end() && It->rfind(H + "|", 0) == 0) AnyOpen = true;
            }
            for (const std::string &H : S.Sel) setExpanded(H, !AnyOpen);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Home)) frameAll();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { S.Sel.clear(); S.SelWireSlot = -1; }
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))
        {
            if (S.SelWireSlot >= 0)
            {
                const int Ci = D.IndexOf(S.SelWireChild);
                json N = Ci >= 0 ? D.Node(Ci) : json();
                if (Ci >= 0 && PkgGraph::EraseRef(N, S.SelWireSlot)) { D.Replace(Ci, std::move(N)); D.Commit(); S.SelWireSlot = -1; emit documentEdited(); }
            }
            else if (!S.Sel.empty()) removeNodes(S.Sel);
        }
    }
    ImGui::End();
    S.RectsValid = false;                                // positions may have changed below this point

    // ---- after the frame ------------------------------------------------------------------------------------------
    //One undo step per gesture: commit when nothing is being typed into or dragged.
    if (D.Pending() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(0) && S.M == PkgCanvasState::Mode::Idle)
    {
        D.Commit();
        emit documentEdited();
    }
    const std::string One = selectedHandle();
    if (One != S.LastSelEmitted) { S.LastSelEmitted = One; emit selectionChanged(QString::fromStdString(One)); }
    //A layer dropped on a row or a node's foot: moved there (Ctrl: copied), one undo step.
    if (!S.Drop.From.empty())
    {
        const auto Dp = S.Drop;
        S.Drop = {};
        std::string Why;
        if (MoveLayer(Dp.From, Dp.FromLayer, Dp.To, Dp.At, Dp.Copy, &Why)) { D.Commit(); emit documentEdited(); }
        else if (!Why.empty()) emit statusMessage(QString::fromStdString(Why));
    }
    if (!S.RemoveReq.empty())
    {
        const std::string H = S.RemoveReq;
        S.RemoveReq.clear();
        removeNodes({H});
    }
    if (!S.MergeReq.first.empty())
    {
        const auto Mr = S.MergeReq;
        S.MergeReq = {};
        std::string Why;
        const int Into = D.IndexOf(Mr.second);
        if (D.MergeInto(D.IndexOf(Mr.first), Into, &Why))
        {
            D.Commit();
            S.RegBuf.clear();
            S.Sel = {Mr.second};
            setExpanded(Mr.second, true);
            emit documentEdited();
        }
        else emit statusMessage(QString::fromStdString(Why));
    }
    //The frame is closed: now an action may open a dialog (a nested event loop).
    if (!S.Pending.first.empty())
    {
        const auto P = S.Pending;
        S.Pending = {};
        emit nodeAction(QString::fromStdString(P.first), QString::fromStdString(P.second));
    }
}

unsigned int PkgCanvas::WireColour(const std::string &From, int Rank) { return WireHue(From, Rank); }
