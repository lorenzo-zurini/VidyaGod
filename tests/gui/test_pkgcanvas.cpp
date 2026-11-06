// The package canvas, driven headlessly (see canvasharness.h): what a person does with a mouse and a keyboard, and
// what the package document looks like afterwards. Every test names what it pins and how it was made to fail.

#include "qtestjson.h"
#include "canvasharness.h"
#include "pkggraph.h"
#include "cid.h"
#include "imguiprobe.h"

#include <QtTest>
#include <set>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>

using json = nlohmann::ordered_json;

namespace {

json Zip(const std::string &Label, const std::string &File) { return json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"ZIP", File}, {"TARGET", "FILES/%GameDir%"}} })}}; }
json Over(const std::string &Label, std::vector<std::string> Refs)
{
    json L = json::array();
    for (const auto &R : Refs) L.push_back(json{{"NODE", R}});
    L.push_back(json{{"ENV", {{"A", "1"}}}});
    return json{{"LABEL", Label}, {"LAYERS", L}};
}

bool Overlap(const ImVec4 &A, const ImVec4 &B) { return A.x < B.z && B.x < A.z && A.y < B.w && B.y < A.w; }

} // namespace

class PkgCanvasTest : public QObject
{
    Q_OBJECT

    CanvasHarness H;
    //The probe on, writing where this run can read it back: what tools/edrive.py reads, and how these tests find a
    //control by what it says.
    QTemporaryDir ProbeDir;

    //A small published package: two contents, a game containing both, a mod containing the game and a node of
    //another package (a chip). Handles are CIDs, as a published package's are.
    std::string A, B, G, M;
    const std::string Ext = "bafkreiexternalnodexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    void package()
    {
        const json Ja = Zip("Base", "a.zip"), Jb = Zip("Music", "b.zip");
        A = Cid::OfNode(Ja); B = Cid::OfNode(Jb);
        const json Jg = Over("Game", {A, B});
        G = Cid::OfNode(Jg);
        const json Jm = Over("Mod", {G, Ext});
        M = Cid::OfNode(Jm);
        H.Doc.Reset({{A, Ja}, {B, Jb}, {G, Jg}, {M, Jm}});
        H.Canvas->setExternalLookup([this](const std::string &C) {
            return C == Ext ? PkgCanvas::External{"Core Fonts", "Fonts Library", "/lib/fonts", ""} : PkgCanvas::External{};
        });
        H.frames(4);                                      // layout, measure, re-layout, settle
    }

private slots:
    void initTestCase()
    {
        QVERIFY(ProbeDir.isValid());
        qputenv("VG_UI_PROBE", (ProbeDir.path() + "/probe.json").toUtf8());
        CanvasHarness::InitContext();
    }
    void cleanupTestCase() { CanvasHarness::DestroyContext(); }
    void init() { H.Doc.Reset({}); H.open(); }
    void cleanup() { H.close(); }

    // A published package opens wired: every reference is a wire (the old editor keyed nodes by a field gen-6 nodes
    // lack, so none resolved), another package's node is a chip carrying its package and name, and nothing overlaps.
    // Teeth: drop the external lookup (the chip reads a CID); draw wires only between drawn nodes.
    void aPackageOpensWiredLabelledAndLaidOut()
    {
        package();
        QCOMPARE(H.Canvas->visibleWires(), 4);            // Game<-Base, Game<-Music, Mod<-Game, Mod<-chip
        QCOMPARE(H.Canvas->externalCount(), 1);
        QCOMPARE(H.Canvas->externalLabel(Ext), std::string("Fonts Library: Core Fonts"));
        std::vector<ImVec4> R;
        for (const std::string &X : {A, B, G, M, Ext}) { R.push_back(H.rect(X)); QVERIFY2(R.back().z > R.back().x, X.c_str()); }
        for (size_t I = 0; I < R.size(); ++I)
            for (size_t J = I + 1; J < R.size(); ++J) QVERIFY2(!Overlap(R[I], R[J]), "two nodes overlap in the default layout");
        //Contained nodes sit left of what contains them.
        QVERIFY(R[0].z < R[2].x && R[2].z < R[3].x);
    }

    void clickingANodeSelectsItAndTheBackgroundClears()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(G, true);
        H.frames(3);
        H.Canvas->select("");
        H.frames(2);
        H.click(H.title(G));
        QCOMPARE(H.Canvas->selectedHandle(), G);
        H.click(H.empty());
        QVERIFY(H.Canvas->selection().empty());
    }

    void clickingABoxSelectsItWhenZoomedOut()
    {
        package();
        H.Canvas->setZoom(0.25f);
        H.frames(3);
        const ImVec4 R = H.rect(G);
        QVERIFY(R.z > R.x);
        H.click(ImVec2((R.x + R.z) * 0.5f, (R.y + R.w) * 0.5f));
        QCOMPARE(H.Canvas->selectedHandle(), G);
    }

    // Dragging a title moves the node (and every selected node) by the drag, as ONE undo step, and only positions
    // change: the node's bytes (its CID) do not. Teeth: SetPos per frame of the drag (many steps), or no commit.
    void draggingATitleMovesTheNodeAsOneUndoStep()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(G, true);
        H.frames(3);
        float X0, Y0, W, Hh;
        QVERIFY(H.Canvas->worldRect(G, X0, Y0, W, Hh));
        const json Before = H.Doc.Node(H.Doc.IndexOf(G));
        const ImVec2 T = H.title(G);
        H.drag(T, ImVec2(T.x + 120, T.y + 60));
        float X1, Y1;
        QVERIFY(H.Canvas->worldRect(G, X1, Y1, W, Hh));
        QVERIFY2(std::abs((X1 - X0) - 120.0f) < 2.0f && std::abs((Y1 - Y0) - 60.0f) < 2.0f, "the node did not follow the drag");
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(G)), Before);
        QVERIFY(!H.Doc.Dirty());                          // a position is not content: nothing to save
        H.key(ImGuiKey_Z, true);
        float X2, Y2;
        QVERIFY(H.Canvas->worldRect(G, X2, Y2, W, Hh));
        QVERIFY2(std::abs(X2 - X0) < 0.5f && std::abs(Y2 - Y0) < 0.5f, "one undo did not put it back");
    }

    // Dragging from a node's right-hand port onto another node makes the other CONTAIN it (a NODE layer, before its
    // own layers); with Shift held it REQUIRES it instead (ANY). Dropped on empty canvas: nothing changes. Teeth: swap the
    // direction of the out-port drop.
    void draggingFromAPortWiresNodes()
    {
        package();
        H.Canvas->frameAll();
        H.frames(3);
        const int Before = (int)H.Doc.Node(H.Doc.IndexOf(M))["LAYERS"].size();
        H.drag(H.port(B, true), H.title(M));             // Music -> Mod: Mod contains Music
        bool Contains = false;
        PkgDoc::ForEachRef(H.Doc.Node(H.Doc.IndexOf(M)), [&](const std::string &R) { if (R == B) Contains = true; });
        QVERIFY2(Contains, "the drop did not wire the node in");
        QCOMPARE((int)H.Doc.Node(H.Doc.IndexOf(M))["LAYERS"].size(), Before + 1);
        QVERIFY(H.Doc.Node(H.Doc.IndexOf(M))["LAYERS"][0].contains("NODE"));
        //Onto nothing: no change - a menu offers a new node there instead, and Escape closes it.
        const json Now = H.Doc.Node(H.Doc.IndexOf(M));
        const int Count = H.Doc.Count();
        H.drag(H.port(A, true), H.empty());
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(M)), Now);
        H.key(ImGuiKey_Escape);
        H.frames(2);
        QCOMPARE(H.Doc.Count(), Count);
        //Shift: requires one of (ANY).
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
        H.drag(H.port(A, true), H.title(M));
        ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
        H.frame();
        qInfo("M after shift-drop: %s", H.Doc.Node(H.Doc.IndexOf(M)).dump().c_str());
        bool Any = false;
        for (const json &L : H.Doc.Node(H.Doc.IndexOf(M))["LAYERS"])
            if (L.contains("ANY")) for (const json &X : L["ANY"]) if (X == A) Any = true;
        QVERIFY2(Any, "Shift-drop did not make a requirement");
    }

    // A wire is selected by clicking it and removed with Delete (its reference leaves the container; undo restores).
    // Teeth: hit-test wires only at their ends.
    void aWireIsSelectedByClickingAndDeletedWithDel()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(M, true);
        H.frames(3);
        H.Canvas->select("");
        H.frames(2);
        //The midpoint of the Mod <- Game wire, where no node is (it plugs into Mod's row 0, the NODE layer naming Game).
        const ImVec2 P0 = H.port(G, true), P1 = H.row(M, 0);
        const ImVec2 Mid((P0.x + P1.x) * 0.5f, (P0.y + P1.y) * 0.5f);
        H.click(Mid);
        H.key(ImGuiKey_Delete);
        bool Still = false;
        PkgDoc::ForEachRef(H.Doc.Node(H.Doc.IndexOf(M)), [&](const std::string &R) { if (R == G) Still = true; });
        QVERIFY2(!Still, "the clicked wire was not deleted");
        H.key(ImGuiKey_Z, true);
        Still = false;
        PkgDoc::ForEachRef(H.Doc.Node(H.Doc.IndexOf(M)), [&](const std::string &R) { if (R == G) Still = true; });
        QVERIFY(Still);
    }

    // Delete removes the selected nodes and every wire to them; Ctrl+Z brings them back, wires and all.
    void deleteRemovesNodesAndUndoBringsThemBack()
    {
        package();
        H.Canvas->select(G, true);
        H.frames(2);
        H.frame(H.empty());
        H.key(ImGuiKey_Delete);
        QCOMPARE(H.Doc.IndexOf(G), -1);
        bool Dangling = false;
        PkgDoc::ForEachRef(H.Doc.Node(H.Doc.IndexOf(M)), [&](const std::string &R) { if (R == G) Dangling = true; });
        QVERIFY(!Dangling);
        H.key(ImGuiKey_Z, true);
        QVERIFY(H.Doc.IndexOf(G) >= 0);
        bool Back = false;
        PkgDoc::ForEachRef(H.Doc.Node(H.Doc.IndexOf(M)), [&](const std::string &R) { if (R == G) Back = true; });
        QVERIFY(Back);
    }

    // Double-click a title, type, Enter: the node's LABEL, as one undo step. Teeth: commit per keystroke.
    void doubleClickingATitleRenamesTheNode()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(A, true);
        H.frames(3);
        H.doubleClick(H.title(A));
        H.frames(2);
        H.key(ImGuiKey_A, true);                          // select what is there
        H.type("Renamed");
        H.key(ImGuiKey_Enter);
        H.frames(2);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(A))["LABEL"].get<std::string>(), std::string("Renamed"));
        H.key(ImGuiKey_Z, true);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(A))["LABEL"].get<std::string>(), std::string("Base"));
    }

    // A name changed elsewhere while its rename box is open (the JSON pane sets LABEL) ends the rename: the box neither
    // keeps showing the old name nor writes it back once it is let go. Teeth: keep the rename open (the old name comes
    // back when the box loses focus).
    void aRenameEndsWhenTheNameChangesElsewhere()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(A, true);
        H.frames(3);
        H.doubleClick(H.title(A));
        H.frames(2);
        {
            json N = H.Doc.Node(H.Doc.IndexOf(A));
            N["LABEL"] = "Set in the JSON pane";
            H.Doc.Replace(H.Doc.IndexOf(A), std::move(N));
            H.Doc.Commit();
        }
        H.frames(2);
        H.click(H.title(B));                              // the rename box, were it still open, is let go here
        H.frames(3);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(A))["LABEL"].get<std::string>(), std::string("Set in the JSON pane"));
    }

    // "Contain another node" made while a node is selected contains that node: a filled reference, never an empty one
    // to wire up later; nothing selected, it waits empty as before. Teeth: ignore the reference (NODE stays "").
    void aNewReferenceNodeRefersToTheSelectedNode()
    {
        package();
        const int I = H.Canvas->addNode("NODE", 0, 0, A);
        QCOMPARE(H.Doc.Node(I)["LAYERS"][0]["NODE"].get<std::string>(), A);
        const int J = H.Canvas->addNode("ANY", 0, 0, A);
        QCOMPARE(H.Doc.Node(J)["LAYERS"][0]["ANY"], json::array({A}));
        const int K = H.Canvas->addNode("NODE", 0, 0);
        QCOMPARE(H.Doc.Node(K)["LAYERS"][0]["NODE"].get<std::string>(), std::string());
    }

    // The canvas pans in world space and never scrolls its window: scrolled (ImGui scrolls a window to a focused item
    // below its edge), the toolbar slid out of view for good. Teeth: drop the scroll pin.
    void theCanvasWindowNeverScrolls()
    {
        package();
        H.Canvas->setZoom(1.0f);
        const ImVec4 C = H.canvasRect();
        //A low in the view; a node added just above it lands clear of it - below A, past the canvas's bottom edge - and
        //its rename box takes the keyboard there: what made ImGui scroll the canvas window down to it.
        float x, y, w, hh;
        H.Canvas->worldRect(A, x, y, w, hh);
        H.Canvas->setCamera(x - 40.0f, y - (C.w - C.y) + 160.0f);
        H.frames(3);
        const ImVec2 E(H.title(A).x, H.rect(A).y - 30.0f);
        const int Before = H.Doc.Count();
        for (int Y = 0; Y < 400 && H.Doc.Count() == Before; Y += 6)
        {
            if (ImGui::GetCurrentContext()->OpenPopupStack.Size == 0) { H.click(E, 1); H.frames(2); }
            H.frame(ImVec2(E.x + 20, E.y + 10));
            H.frames(3);
            H.click(ImVec2(E.x + 220, E.y + 10 + (float)Y));
        }
        QCOMPARE(H.Doc.Count(), Before + 1);
        H.frames(4);
        ImGuiWindow *Win = ImGui::FindWindowByName("##pkgcanvas");
        QVERIFY(Win);
        QCOMPARE(Win->Scroll.y, 0.0f);
        //The new node is panned into view, though its free spot was past the canvas's bottom edge. Teeth: leave it
        //off-screen.
        const std::string New = H.Doc.Handle(H.Doc.Count() - 1);
        H.frames(2);
        const ImVec4 R = H.rect(New), Cv = H.canvasRect();
        QVERIFY2(R.y >= Cv.y && R.y + 20.0f <= Cv.w, qPrintable(QString("node top %1, canvas %2..%3").arg(R.y).arg(Cv.y).arg(Cv.w)));
    }

    // The wheel zooms about the cursor: the world point under it stays under it. Teeth: zoom about the view centre.
    void theWheelZoomsAboutTheCursor()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.frames(2);
        const ImVec2 P = H.title(G);
        float Cx0, Cy0;
        H.Canvas->cameraPos(Cx0, Cy0);
        const ImVec4 C = H.canvasRect();
        const ImVec2 W0(Cx0 + (P.x - C.x) / 1.0f, Cy0 + (P.y - C.y) / 1.0f);
        H.wheel(P, 3.0f);
        const float Z = H.Canvas->zoom();
        QVERIFY(Z > 1.3f);
        float Cx1, Cy1;
        H.Canvas->cameraPos(Cx1, Cy1);
        const ImVec2 W1(Cx1 + (P.x - C.x) / Z, Cy1 + (P.y - C.y) / Z);
        QVERIFY2(std::abs(W1.x - W0.x) < 1.0f && std::abs(W1.y - W0.y) < 1.0f, "the point under the cursor moved");
    }

    // A middle-drag pans (the whole view follows the cursor); so does a right-drag on the background.
    void draggingTheBackgroundPans()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.frames(2);
        float X0, Y0, X1, Y1;
        H.Canvas->cameraPos(X0, Y0);
        const ImVec2 E = H.empty();
        H.drag(E, ImVec2(E.x + 100, E.y - 50), 8, 2);
        H.Canvas->cameraPos(X1, Y1);
        QVERIFY(std::abs((X0 - X1) - 100.0f) < 1.0f && std::abs((Y0 - Y1) + 50.0f) < 1.0f);
        H.drag(E, ImVec2(E.x + 40, E.y), 8, 1);
        float X2, Y2;
        H.Canvas->cameraPos(X2, Y2);
        QVERIFY(std::abs((X1 - X2) - 40.0f) < 1.0f);
        QVERIFY(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);   // a drag is not a right-click
    }

    // A wire whose two ends are off screen but which crosses it is drawn; one entirely off screen is not.
    // Teeth: cull wires with their endpoints.
    void wiresAreCulledByTheirOwnExtent()
    {
        const json Ja = Zip("Left", "l.zip");
        const std::string La = Cid::OfNode(Ja);
        const json Jb = Over("Right", {La});
        const std::string Rb = Cid::OfNode(Jb);
        H.Doc.Reset({{La, Ja}, {Rb, Jb}});
        H.Doc.SetPositions({{La, {-20000, 300}}, {Rb, {20000, 300}}});
        H.frames(2);
        H.Canvas->setZoom(1.0f);
        H.Canvas->setCamera(0, 0);
        H.frames(2);
        QCOMPARE(H.Canvas->visibleNodes(), 0);
        QCOMPARE(H.Canvas->visibleWires(), 1);
        H.Canvas->setCamera(0, 50000);
        H.frames(2);
        QCOMPARE(H.Canvas->visibleWires(), 0);
    }

    // A node opened to be taller than the screen stays drawn while any of it is on screen (culling uses its drawn
    // size, not the folded estimate). Teeth: cull by the estimate.
    void aTallOpenNodeStaysDrawnWhileAnyOfItShows()
    {
        //ONE layer of a hundred variables: folded it is a few rows, opened a hundred.
        json Env = json::object();
        for (int K = 0; K < 100; ++K) Env["K" + std::to_string(K)] = "v";
        json Big = json{{"LABEL", "Big"}, {"LAYERS", json::array({ json{{"ENV", Env}} })}};
        const std::string Hb = Cid::OfNode(Big);
        H.Doc.Reset({{Hb, Big}});
        H.Doc.SetPositions({{Hb, {0, 0}}});
        H.frames(2);
        H.Canvas->setZoom(1.0f);
        H.Canvas->setCamera(-50, -50);
        H.frames(2);
        H.Canvas->setExpanded(Hb, true);
        H.frames(4);
        const ImVec4 R = H.rect(Hb);
        QVERIFY2(R.w - R.y > 1400.0f, qPrintable(QString("the opened node is only %1px tall").arg(R.w - R.y)));
        H.Canvas->setCamera(-50, 1000);
        H.frames(2);
        QCOMPARE(H.Canvas->visibleNodes(), 1);
    }

    // Drawing never edits: frames with rows opened, zooming and hovering leave the document exactly as it was
    // (a node whose bytes change on view would change its CID for every peer). Teeth: materialise a field on draw.
    void renderingNeverChangesTheDocument()
    {
        package();
        const uint64_t Rev = H.Doc.Revision();
        for (const std::string &X : {A, B, G, M}) H.Canvas->setExpanded(X, true);
        H.Canvas->setZoom(1.0f);
        for (float Z : {1.0f, 0.3f, 1.7f}) { H.Canvas->setZoom(Z); H.frames(3); H.frame(H.title(G)); }
        QCOMPARE(H.Doc.Revision(), Rev);
        QVERIFY(!H.Doc.Dirty());
    }

    // Malformed nodes (the editor is the tool you open to repair them) draw as what they are and survive every row
    // opened. Teeth: draw fields of a non-object entry (a write would throw out of the frame).
    void malformedNodesDrawAndSurviveOpening()
    {
        const json Bad1 = json{{"LABEL", "Two types"}, {"LAYERS", json::array({ json{{"ZIP", "a.zip"}, {"DIR", "d"}} })}};
        const json Bad2 = json{{"LABEL", "Layers not a list"}, {"LAYERS", "nope"}};
        const json Bad3 = json{{"LABEL", "Bad fields"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({ 5, json{{"LABEL", "Play"}, {"ARGS", "not a list"}} })}},
            json{{"EDIT", "not a list"}, {"TARGET", "FILES/x"}},
            json{{"REG", json::array({1})}},
            json{{"VARS", {{"k", 7}}}},
            json{{"KEEP", "x"}} })}};
        H.Doc.Reset({{"h1", Bad1}, {"h2", Bad2}, {"h3", Bad3}, {"h4", json::array()}});
        for (const char *X : {"h1", "h2", "h3", "h4"}) H.Canvas->setExpanded(X, true);
        H.Canvas->setZoom(1.0f);
        H.frames(4);
        H.Canvas->frameAll();
        H.frames(4);
        QCOMPARE(H.Doc.Count(), 4);
        QVERIFY(!H.Doc.Dirty());
    }

    // Find: type part of a name, Enter — the node is selected and brought into view.
    void findSelectsAndFramesANode()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->setCamera(50000, 50000);
        H.frames(2);
        H.key(ImGuiKey_F, true);                          // Ctrl+F: the find box
        H.frames(2);
        H.type("mod");
        H.key(ImGuiKey_Enter);
        H.frames(2);
        QCOMPARE(H.Canvas->selectedHandle(), M);
        QVERIFY(H.rect(M).z > H.rect(M).x);               // drawn: it is on screen now
    }

    // After a save renames nodes, the canvas follows them: the selection, the open rows and the positions stay with
    // their nodes under the new names. Teeth: skip applyRenames (the selection is lost).
    void afterASaveTheCanvasFollowsRenamedNodes()
    {
        const QString Dir = QDir::tempPath() + "/vg_canvas_rename";
        QDir(Dir).removeRecursively();
        QDir().mkpath(Dir);
        package();
        H.Canvas->select(A);
        H.Canvas->setExpanded(A, true);
        H.frames(2);
        json N = H.Doc.Node(H.Doc.IndexOf(A));
        N["LABEL"] = "Base v2";
        H.Doc.Replace(H.Doc.IndexOf(A), N);
        H.Doc.Commit();
        H.frames(2);
        const PkgDoc::SaveReport R = H.Doc.Save(Dir.toStdString(), "", "");
        QVERIFY(R.Ok);
        H.Canvas->applyRenames(H.Doc.TakeRenames());
        H.frames(2);
        const std::string NewA = R.Renamed.at(A);
        QCOMPARE(H.Canvas->selectedHandle(), NewA);
        QVERIFY(H.rect(NewA).z > H.rect(NewA).x);
        QVERIFY(H.rect(NewA).w - H.rect(NewA).y > 80.0f);   // still opened
    }

    // Where two nodes overlap, the one drawn on top is the one that takes the click (imgui hit-tests windows in the
    // order they were created; the canvas re-orders them to the order they are drawn). Teeth: skip the re-order.
    void theNodeDrawnOnTopTakesTheClick()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Doc.SetPositions({{A, {0, 0}}, {B, {40, 12}}});   // B overlaps A, created later
        H.Canvas->setCamera(-40, -40);
        H.frames(3);
        const ImVec4 Ra = H.rect(A);
        H.click(ImVec2(Ra.x + 15, Ra.y + 14));            // A's uncovered edge: brings A to the front
        QCOMPARE(H.Canvas->selectedHandle(), A);
        H.frames(2);
        const ImVec4 Rb = H.rect(B);
        H.frame(ImVec2(Rb.x + 30, Rb.y + 14));
        H.click(ImVec2(Rb.x + 30, Rb.y + 14));            // inside both: A is on top now
        QCOMPARE(H.Canvas->selectedHandle(), A);
    }

    // The node menu's Delete removes the node and the canvas draws on: the node it was drawing is gone mid-frame
    // (the last one: no index left to land on). Teeth: remove it from inside the menu, while it is drawn.
    void theNodeMenuDeletesTheNodeItIsOn()
    {
        package();
        H.Canvas->frameAll();
        H.settle();
        const int Before = H.Doc.Count();
        const std::string Last = H.Doc.Handle(Before - 1);
        H.click(H.title(Last), 1);
        H.settle();
        const ImVec2 Del = H.item("Delete");
        QVERIFY(Del.x >= 0);
        H.click(Del);
        H.settle();
        QCOMPARE(H.Doc.Count(), Before - 1);
        QCOMPARE(H.Doc.IndexOf(Last), -1);
    }

    // Escape cancels a wire being dragged: nothing is wired.
    void escapeCancelsAWireDrag()
    {
        package();
        H.Canvas->frameAll();
        H.frames(2);
        const json Before = H.Doc.Node(H.Doc.IndexOf(M));
        const ImVec2 P = H.port(B, true), T = H.title(M);
        H.frame(P);
        H.press();
        for (int I = 1; I <= 6; ++I) H.frame(ImVec2(P.x + (T.x - P.x) * I / 6.0f, P.y + (T.y - P.y) * I / 6.0f));
        H.key(ImGuiKey_Escape);
        H.release();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(M)), Before);
    }

    // Right-click on the background, "Add node here", pick a type: a node where the menu was opened.
    void theCanvasMenuAddsANodeWhereItWasOpened()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.frames(2);
        const ImVec2 E(H.empty().x + 200, H.empty().y - 200);
        H.click(E, 1);
        H.frames(2);
        QVERIFY(ImGui::GetCurrentContext()->OpenPopupStack.Size > 0);
        const int Before = H.Doc.Count();
        //Walk the menu down from where it opened until a click adds a node (menu layout is imgui's business).
        for (int Y = 0; Y < 400 && H.Doc.Count() == Before; Y += 6)
        {
            H.frame(ImVec2(E.x + 20, E.y + 10));
            if (ImGui::GetCurrentContext()->OpenPopupStack.Size == 0) { H.click(E, 1); H.frames(2); }
            H.frame(ImVec2(E.x + 20, E.y + 10));          // hover "Add node here" opens its submenu
            H.frames(3);
            H.click(ImVec2(E.x + 220, E.y + 10 + (float)Y));
        }
        QCOMPARE(H.Doc.Count(), Before + 1);
        //...ready to fill in: its name is being edited, so typing names it.
        H.frames(2);
        H.key(ImGuiKey_A, true);
        H.type("Setup files");
        H.key(ImGuiKey_Enter);
        H.frames(2);
        QCOMPARE(H.Doc.Node(Before).value("LABEL", std::string()), std::string("Setup files"));
    }

    // A node added where another one is lands clear of it (straight below), never on top of it. Teeth: place it
    // exactly where asked.
    void aNewNodeNeverLandsOnAnother()
    {
        package();
        float X = 0, Y = 0, W = 0, Hh = 0;
        QVERIFY(H.Canvas->worldRect(G, X, Y, W, Hh));
        const int I = H.Canvas->addNode("ZIP", X + 40, Y + 10);
        H.frames(4);
        const std::string New = H.Doc.Handle(I);
        float Nx = 0, Ny = 0, Nw = 0, Nh = 0;
        QVERIFY(H.Canvas->worldRect(New, Nx, Ny, Nw, Nh));
        QCOMPARE(Nx, X + 40);                             // the column asked for
        for (const std::string &O : {A, B, G, M})
        {
            float Ox = 0, Oy = 0, Ow = 0, Oh = 0;
            QVERIFY(H.Canvas->worldRect(O, Ox, Oy, Ow, Oh));
            const bool Clear = Nx >= Ox + Ow || Ox >= Nx + Nw || Ny >= Oy + Oh || Oy >= Ny + Nh;
            QVERIFY2(Clear, ("the new node covers " + O).c_str());
        }
    }

    // A chip's "open its package" (double-click) asks the host to open the package that owns the node.
    void doubleClickingAChipOpensItsPackage()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(Ext, true);
        H.frames(3);
        QString Opened;
        QObject::connect(H.Canvas, &PkgCanvas::openPackageRequested, [&](const QString &D) { Opened = D; });
        const ImVec4 R = H.rect(Ext);
        H.doubleClick(ImVec2((R.x + R.z) * 0.5f, (R.y + R.w) * 0.5f));
        QCOMPARE(Opened, QString("/lib/fonts"));
    }

    // Tidy forgets this machine's positions: nodes go back to the layout (one undo step brings the positions back).
    void tidyReturnsNodesToTheLayout()
    {
        package();
        float X0, Y0, W, Hh;
        QVERIFY(H.Canvas->worldRect(G, X0, Y0, W, Hh));
        H.Doc.SetPos(G, {X0 + 900, Y0 + 900});
        H.Doc.Commit();
        H.frames(2);
        H.Canvas->tidyLayout();
        H.frames(2);
        float X1, Y1;
        QVERIFY(H.Canvas->worldRect(G, X1, Y1, W, Hh));
        QVERIFY(std::abs(X1 - X0) < 1.0f && std::abs(Y1 - Y0) < 1.0f);
        QVERIFY(H.Doc.Positions().empty());
    }

    // Wiring (or any edit that changes the graph) never moves a node the author has already seen: only new nodes are
    // laid out. Teeth: re-run the whole layout on every structural change (every node shifts when a wire is added).
    void editingTheGraphDoesNotRearrangeIt()
    {
        package();
        H.Canvas->frameAll();
        H.frames(3);
        std::map<std::string, ImVec2> Before;
        for (const std::string &X : {A, B, G, M, Ext}) { float x, y, w, h; H.Canvas->worldRect(X, x, y, w, h); Before[X] = ImVec2(x, y); }
        H.drag(H.port(B, true), H.title(M));              // a new wire
        H.Canvas->addNode("ZIP", 0, 0);                    // and a new node
        H.frames(4);
        for (const std::string &X : {A, B, G, M, Ext})
        {
            float x, y, w, h;
            H.Canvas->worldRect(X, x, y, w, h);
            QVERIFY2(std::abs(x - Before[X].x) < 0.5f && std::abs(y - Before[X].y) < 0.5f, "a node moved when the graph was edited");
        }
    }

    // Find ranks the exact name first, then names starting with the text, then names containing it: "UserPatch 1.5"
    // must find "UserPatch 1.5", not "UserPatch 1.5 - Base" listed before it. Teeth: keep document order.
    void findPrefersTheExactName()
    {
        const json Base = Zip("Tool 1.5 - Base", "b.zip"), Exact = Zip("Tool 1.5", "t.zip");
        H.Doc.Reset({{"hb", Base}, {"ht", Exact}});
        H.frames(3);
        H.key(ImGuiKey_F, true);
        H.frames(2);
        H.type("tool 1.5");
        H.key(ImGuiKey_Enter);
        H.frames(2);
        QCOMPARE(H.Canvas->selectedHandle(), std::string("ht"));
    }

    // A node scrolled partly above the canvas is cut off at the canvas's edge: it neither draws over the toolbar nor
    // takes its clicks. Teeth: drop the clip around the nodes (the toolbar's button under the node stops working).
    void aNodeNeverCoversTheToolbar()
    {
        package();
        H.Canvas->setZoom(1.0f);
        const ImVec4 C = H.canvasRect();
        //Put Game's title just above the canvas's top edge, under the toolbar.
        float x, y, w, hh;
        H.Canvas->worldRect(G, x, y, w, hh);
        H.Canvas->setCamera(x - 20.0f, y + 30.0f);
        H.frames(3);
        ImGuiWindow *Win = nullptr;
        for (ImGuiWindow *W : ImGui::GetCurrentContext()->Windows)
            if (W->Active && std::string(W->Name).find("##node" + G) != std::string::npos) Win = W;
        QVERIFY2(Win, "the node was not drawn");
        QVERIFY2(Win->OuterRectClipped.Min.y >= C.y - 0.5f, "a node reaches over the toolbar");
    }

    // Typing into a field is ONE undo step, however many keystrokes: the document commits when the field is let go.
    // The field is found by effect (click down the opened node until typing lands in the ZIP name), not by pixel.
    // Teeth: commit every frame (every character becomes a step).
    void typingInAFieldIsOneUndoStep()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(A, true);
        H.Canvas->setExpanded(A, true);
        H.frames(4);
        const ImVec4 R = H.rect(A);
        bool Found = false;
        for (float Y = R.y + 40.0f; Y < R.w - 5.0f && !Found; Y += 6.0f)
        {
            H.Canvas->setExpanded(A, true);                // a probe that lands on a fold header toggles it: undo that
            H.frames(2);
            H.click(ImVec2(R.z - 60.0f, Y));
            if (!ImGui::GetIO().WantTextInput) continue;
            H.key(ImGuiKey_End);
            H.type("xyz");
            Found = H.Doc.Node(H.Doc.IndexOf(A))["LAYERS"][0]["ZIP"].get<std::string>() == "a.zipxyz";
            if (!Found) { H.key(ImGuiKey_Escape); H.click(H.empty()); H.frames(2); while (H.Doc.CanUndo()) H.key(ImGuiKey_Z, true); }
        }
        QVERIFY2(Found, "no field of the node took the typing into its ZIP name");
        H.click(H.empty());                               // let the field go
        H.frames(2);
        H.key(ImGuiKey_Z, true);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(A))["LAYERS"][0]["ZIP"].get<std::string>(), std::string("a.zip"));
    }

    // With no package open the canvas is where one starts: "New package..." and "Open package..." ask the editor for
    // them. With a package open (even an empty one) it says how to add a node instead. Teeth: drop either emit; show
    // the buttons whatever is open.
    void anEmptyEditorOffersToStartOrOpenAPackage()
    {
        H.Canvas->setPackageOpen(false);
        H.frames(3);
        QSignalSpy New(H.Canvas, &PkgCanvas::newPackageRequested), Open(H.Canvas, &PkgCanvas::openPackagePickerRequested);
        QVERIFY(H.shows("New package..."));
        H.click(H.item("New package..."));
        QCOMPARE(New.count(), 1);
        QCOMPARE(Open.count(), 0);
        H.click(H.item("Open package..."));
        QCOMPARE(Open.count(), 1);
        QCOMPARE(New.count(), 1);
        H.Canvas->setPackageOpen(true);
        H.frames(2);
        QVERIFY(!H.shows("New package...") && !H.shows("Open package..."));
    }

    //A cover named by typing is a new image: the old one's SOURCE and SIZE go (publish stamps the new one's), as when
    //it is browsed. Kept, friends fetched the old image by its CID under the new name.
    void typingACoverNameDropsTheOldImagesCid()
    {
        const json Game = json{{"LABEL", "Game"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "%GameDir%/g.exe"},
                {"TILE", {{"UID", "1"}, {"TITLE", "G"}, {"COVER", {{"FILE", "a.png"}, {"SOURCE", "QmOld"}, {"SIZE", 5}}}}}} })}} })}};
        const std::string G = Cid::OfNode(Game);
        H.Doc.Reset({{G, Game}});
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        H.Canvas->setZoom(1.0f);
        H.frames(4);
        H.Canvas->setExpanded(G, true);
        H.settle();
        const auto Cover = [&] { return H.Doc.Node(H.Doc.IndexOf(G))["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"]; };
        bool Typing = false;
        for (int N = 0; N < 4 && !Typing; ++N)
        {
            const ImVec2 At = H.item("Cover", N);
            if (At.x < 0) break;
            H.click(At);
            Typing = ImGui::GetIO().WantTextInput;
        }
        QVERIFY2(Typing, "the Cover box took no click");
        H.key(ImGuiKey_End);
        H.type("x");
        QCOMPARE(Cover(), json({{"FILE", "a.pngx"}}));
    }

    //A kept registry key names the bitness of the program that reads it (a 32-bit program's HKLM\Software is under
    //Wow6432Node): its row offers 32-bit and 64-bit, written as ARCH; a kept folder's row does not. A delta offers its
    //bases.
    void aKeptRegistryKeyOffersItsBitnessAndADeltaItsBases()
    {
        const json Game = json{{"LABEL", "Game"}, {"LAYERS", json::array({
            json{{"KEEP", {{"FILES/%GameDir%/Saves/", true}, {"REG/HKLM/Software/Acme/Game", true}}}},
            json{{"DELTA", "free.vgdelta"}, {"TARGET", "FILES/%GameDir%"}} })}};
        const std::string G = Cid::OfNode(Game);
        H.Doc.Reset({{G, Game}});
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        H.Canvas->setZoom(1.0f);
        H.frames(4);
        H.Canvas->setExpanded(G, true);
        H.settle();
        QVERIFY(H.shows("32-bit") && H.shows("64-bit"));
        QVERIFY(!H.shows("32-bit", 1));                    // the folder's row has none
        QVERIFY(H.shows("Bases"));
        const auto Keep = [&] { return H.Doc.Node(H.Doc.IndexOf(G))["LAYERS"][0]["KEEP"]["REG/HKLM/Software/Acme/Game"]; };
        H.click(H.item("32-bit"));
        H.frames(2);
        QCOMPARE(Keep(), json({{"ARCH", {"32"}}}));
        H.settle();
        H.click(H.item("64-bit"));
        H.frames(2);
        QCOMPARE(Keep(), json({{"ARCH", {"32", "64"}}}));
        H.settle();
        H.click(H.item("32-bit"));
        H.frames(2);
        QCOMPARE(Keep(), json({{"ARCH", {"64"}}}));
        H.settle();
        H.click(H.item("64-bit"));
        H.frames(2);
        QCOMPARE(Keep(), json(true));                      // no view left: plain true again
    }

    // A game's EXEC entry shows what a game needs: Host has the usual platforms beside it, Exe a "pick" that asks for
    // the programs of THIS entry (layer and index), and a runner's own fields stay out of the way unless the entry is a
    // runner's (it has a GUEST) or already sets one. Teeth: show runner fields on every entry; request the pick with no
    // entry; ignore the platform chosen; leave out the runners' platforms, or list a described one twice.
    void anExecEntryOffersPlatformsAndAProgramPick()
    {
        const json Game = json{{"LABEL", "Game"}, {"LAYERS", json::array({ json{{"ENV", {{"A", "1"}}}},
            json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "%GameDir%/"}},
                                        json{{"LABEL", "Tool"}, {"HOST", "win32"}, {"CONTENT_ROOT", "x"}} })}} })}};
        const json Runner = json{{"LABEL", "Runner"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({ json{{"LABEL", "Run"}, {"HOST", "linux64"}, {"GUEST", json::array({"win32"})}} })}} })}};
        const std::string G = Cid::OfNode(Game), R = Cid::OfNode(Runner);
        H.Doc.Reset({{G, Game}, {R, Runner}});
        H.Display = ImVec2(1400, 2400);                    // both entries on screen: a clipped field draws no label
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        H.Canvas->setZoom(1.0f);
        H.frames(4);
        H.Canvas->setExpanded(G, true);
        H.settle();
        QVERIFY(H.shows("Exe") && H.shows("Host"));
        QVERIFY(!H.shows("Generate prefix") && !H.shows("Unified runtime"));   // a game entry: no runner fields...
        QVERIFY(H.shows("Content root"));                                  // ...unless it already sets one
        QVERIFY(!H.shows("Content root", 1));                              // (the Tool's, not the Play's)

        QSignalSpy Act(H.Canvas, &PkgCanvas::nodeAction);
        H.click(H.item("pick"));
        H.frames(2);
        QCOMPARE(Act.count(), 1);
        QCOMPARE(Act.at(0).at(0).toString().toStdString(), G);
        QCOMPARE(Act.at(0).at(1).toString(), QString("pick_exe:L1/0"));

        H.settle();
        H.click(H.item("hostchoices"));
        H.frames(2);
        QVERIFY(H.shows("win64 - Windows, 64-bit"));
        H.click(H.item("win64 - Windows, 64-bit"));
        H.frames(2);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(G))["LAYERS"][1]["EXEC"][0]["HOST"], json("win64"));
        //...and what this machine's runners play, each once.
        int Asked = 0;
        H.Canvas->setPlatforms([&Asked] { ++Asked; return std::vector<std::string>{"win32", "snes", "n64"}; });
        H.click(H.item("hostchoices"));
        H.frames(3);
        QVERIFY(H.shows("snes") && H.shows("n64") && !H.shows("win32"));
        H.click(H.item("snes"));
        H.frames(2);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(G))["LAYERS"][1]["EXEC"][0]["HOST"], json("snes"));
        QCOMPARE(Asked, 1);                                // asked once, not every frame

        H.Canvas->setExpanded(G, false);
        H.Canvas->setExpanded(R, true);
        H.settle();
        QVERIFY(H.shows("Generate prefix") && H.shows("Content root"));   // a runner's entry has them all
    }

    // The guide on the canvas walks a new game's package to playable, one step at a time and from what is in it:
    // files, then what runs (Host AND a program, not the "%GameDir%/" placeholder), then a tile, a version (only a
    // VARIANT is on the shelf), then its cover - every tile's, an expansion's on the same node too.
    // A runner's package and one with files but no game entry are never nagged. Teeth: count a placeholder Exe as
    // set; skip the Host; skip the version; nag a library; nag a runner; one cover for all tiles; nag a UID-only face.
    void theGuideNamesTheOneStepAPackageStillNeeds()
    {
        auto Step = [](std::vector<json> Ns) {
            std::vector<const json *> P;
            for (const json &N : Ns) P.push_back(&N);
            return PkgGraph::NextStep(P);
        };
        auto Entry = [](json E) { return json{{"LAYERS", json::array({ json{{"EXEC", json::array({E})}} })}}; };
        const json Tile = json{{"UID", "9940"}, {"TITLE", "B&W2"}};
        const json Files = Zip("data", "game.zip");
        QVERIFY(Step({Entry(json{{"LABEL", "Play"}, {"TILE", Tile}})}).find("files") != std::string::npos);
        QVERIFY(Step({Entry(json{{"TILE", Tile}}), Over("refs", {"bafkreiother"})}).find("files") == std::string::npos);
        QVERIFY(Step({Files, Entry(json{{"HOST", "win32"}, {"EXE", "%GameDir%/"}, {"TILE", Tile}})}).find("Exe") != std::string::npos);
        QVERIFY(Step({Files, Entry(json{{"EXE", "%GameDir%/g.exe"}, {"TILE", Tile}})}).find("Host") != std::string::npos);
        QVERIFY(Step({Files, Entry(json{{"HOST", "win32"}, {"EXE", "%GameDir%/g.exe"}})}).find("Tile") != std::string::npos);
        json Game = Entry(json{{"HOST", "win32"}, {"EXE", "%GameDir%/g.exe"}, {"TILE", Tile}});
        QVERIFY(Step({Files, Game}).find("Variant") != std::string::npos);   // only a version is on the shelf
        Game["VARIANT"] = "Original";
        QVERIFY(Step({Files, Game}).find("cover") != std::string::npos);
        Game["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"] = json{{"FILE", "cover.png"}};
        QCOMPARE(Step({Files, Game}), std::string());
        //An expansion's tile on the same node still needs its own cover; a variant naming only a covered tile's UID does not.
        json Two = Game;
        Two["LAYERS"][0]["EXEC"].push_back(json{{"LABEL", "BotG"}, {"HOST", "win32"}, {"EXE", "%GameDir%/b.exe"},
                                                {"TILE", {{"UID", "35496"}, {"PARENTUID", "9940"}, {"TITLE", "Battle of the Gods"}}}});
        QVERIFY(Step({Files, Two}).find("\"Battle of the Gods\" a cover") != std::string::npos);
        QCOMPARE(Step({Files, Game, Entry(json{{"HOST", "win32"}, {"EXE", "%GameDir%/g.exe"}, {"TILE", {{"UID", "9940"}}}})}), std::string());
        QCOMPARE(Step({Files}), std::string());                                                   // a library
        QCOMPARE(Step({Entry(json{{"HOST", "linux64"}, {"GUEST", json::array({"win32"})}})}), std::string());   // a runner
        QVERIFY(Step({json("not a node"), json{{"LAYERS", 5}}}).find("files") != std::string::npos);   // malformed: no throw
    }

    // "+ layer" offers the types grouped, and a pick lands that layer on the node, opened. Teeth: offer nothing; add
    // the type the menu row does not say.
    void plusLayerAddsTheTypePicked()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.Canvas->select(A, true);
        H.Canvas->setExpanded(A, true);
        H.settle();
        const int Before = (int)H.Doc.Node(H.Doc.IndexOf(A))["LAYERS"].size();
        H.click(H.item("+ layer", 0, A));
        H.frames(2);
        QVERIFY(H.shows("EXEC") && H.shows("KEEP") && !H.shows("empty"));
        H.click(H.item("REG"));
        H.frames(2);
        const json L = H.Doc.Node(H.Doc.IndexOf(A))["LAYERS"];
        QCOMPARE((int)L.size(), Before + 1);
        QCOMPARE(PkgGraph::LayerType(L.back()), std::string("REG"));
    }

    // A wire plugs into the ROW holding its reference, not the node's title: Mod's two references (rows 0 and 1) end
    // at two heights, each inside Mod and below its title. Teeth: plug every wire into the title port.
    void wiresPlugIntoTheRowThatHoldsThem()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.settle();
        const ImVec4 R = H.rect(M);
        const ImVec2 R0 = H.row(M, 0), R1 = H.row(M, 1), T = H.port(M, false);
        QVERIFY2(R0.y > T.y + 5.0f, "the first reference's wire ends at the title");
        QVERIFY(R1.y > R0.y + 5.0f);
        QVERIFY(R1.y < R.w);
        QCOMPARE(R0.x, R.x);
    }

    // A wire dropped INTO a node goes between the rows the cursor is between: the reference becomes that layer. A
    // drop that would make a node contain itself is refused. Teeth: append whatever the cursor says; drop the cycle
    // check (Y would contain X, which contains Y).
    void aDroppedWireGoesWhereItWasDropped()
    {
        const json X = json{{"LABEL", "X"}, {"LAYERS", json::array({ json{{"ENV", {{"P", "1"}}}}, json{{"ENV", {{"Q", "2"}}}} })}};
        const json Y = Zip("Y", "y.zip");
        const std::string Hx = Cid::OfNode(X), Hy = Cid::OfNode(Y);
        H.Doc.Reset({{Hx, X}, {Hy, Y}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        const ImVec2 Row1 = H.row(Hx, 1);
        H.drag(H.port(Hy, true), ImVec2(H.rect(Hx).x + 60.0f, Row1.y - 3.0f));   // the upper half of row 1
        H.settle();
        const json L = H.Doc.Node(H.Doc.IndexOf(Hx))["LAYERS"];
        QCOMPARE((int)L.size(), 3);
        QCOMPARE(L[1]["NODE"], json(Hy));

        //X contains Y now: Y may not contain X (the wire is refused, and Y is dimmed while it is dragged).
        const std::string Hy2 = H.Doc.Handle(H.Doc.IndexOf(Hy)), Hx2 = H.Doc.Handle(H.Doc.IndexOf(Hx));
        const json Before = H.Doc.Node(H.Doc.IndexOf(Hy2));
        H.drag(H.port(Hx2, true), H.title(Hy2));
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hy2)), Before);
        //...while a node that may take it does (the drag itself works).
        const json Z = json{{"LABEL", "Z"}, {"LAYERS", json::array()}};
        const int Zi = H.Doc.Add(Z);
        H.settle();
        H.drag(H.port(Hx2, true), H.title(H.Doc.Handle(Zi)));
        H.settle();
        QCOMPARE((int)H.Doc.Node(Zi)["LAYERS"].size(), 1);
    }

    // A wire let go on the empty canvas offers a new node there, already wired: picked from the out port, the new
    // node contains the one the wire came from. Teeth: drop the popup (the wire is just lost).
    void aWireLetGoOnTheCanvasMakesAWiredNode()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.settle();
        const int Before = H.Doc.Count();
        //Open canvas well inside the view: a wire dragged to the edge pans the view (nodes slide under the cursor).
        const ImVec2 Open(700, 800);
        for (const std::string &N : {A, B, G, M})
        { const ImVec4 R = H.rect(N); QVERIFY(!(Open.x >= R.x && Open.x <= R.z && Open.y >= R.y && Open.y <= R.w)); }
        H.drag(H.port(A, true), Open);
        H.settle();
        QVERIFY2(H.shows("ZIP"), "no node menu where the wire was let go");
        H.click(H.item("EXEC"));
        H.settle();
        QCOMPARE(H.Doc.Count(), Before + 1);
        const json N = H.Doc.Node(H.Doc.Count() - 1);
        bool Contains = false;
        for (const json &L : N["LAYERS"]) if (L.contains("NODE") && L["NODE"] == A) Contains = true;
        QVERIFY2(Contains, "the new node does not contain the node the wire came from");
    }

    // A layer is dragged by its row: onto another node's row it moves there (before or after that row by which half
    // the cursor is in), onto a row of its own node it reorders, and with Ctrl it is copied. Teeth: ignore the drop
    // position; always copy; never copy.
    void layersAreDraggedBetweenAndWithinNodes()
    {
        const json P = json{{"LABEL", "P"}, {"LAYERS", json::array({ json{{"ENV", {{"a", "1"}}}}, json{{"ENV", {{"b", "1"}}}} })}};
        const json Q = json{{"LABEL", "Q"}, {"LAYERS", json::array({ json{{"ENV", {{"z", "1"}}}} })}};
        const std::string Hp = Cid::OfNode(P), Hq = Cid::OfNode(Q);
        H.Doc.Reset({{Hp, P}, {Hq, Q}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        auto Row = [&](const std::string &N, int L, float Dy) { const ImVec2 R = H.row(N, L); return ImVec2(R.x + 60.0f, R.y + Dy); };
        auto Envs = [&](const std::string &N) {
            std::string Out;
            for (const json &L : H.Doc.Node(H.Doc.IndexOf(N))["LAYERS"]) Out += L["ENV"].begin().key();
            return Out;
        };
        H.drag(Row(Hp, 0, 0), Row(Hq, 0, 4.0f), 12);      // a -> after z
        H.settle();
        QCOMPARE(Envs(Hp), std::string("b"));
        QCOMPARE(Envs(Hq), std::string("za"));
        H.drag(Row(Hq, 1, 0), Row(Hq, 0, -4.0f), 12);     // within Q: a before z
        H.settle();
        QCOMPARE(Envs(Hq), std::string("az"));
        {                                                 // Ctrl held while it is dropped: b copied after z
            const ImVec2 From = Row(Hp, 0, 0), To = Row(Hq, 1, 4.0f);
            H.frame(From);
            H.press();
            for (int I = 1; I <= 12; ++I)
            {
                if (I == 6) ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
                H.frame(ImVec2(From.x + (To.x - From.x) * (float)I / 12.0f, From.y + (To.y - From.y) * (float)I / 12.0f));
            }
            H.release();
            ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
            H.settle();
        }
        QCOMPARE(Envs(Hp), std::string("b"));
        QCOMPARE(Envs(Hq), std::string("azb"));
        H.key(ImGuiKey_Z, true);                          // one gesture, one undo step
        QCOMPARE(Envs(Hq), std::string("az"));
    }

    // A node's message about a step the author is about to take is said calmly with that step beside it ("waiting for
    // its files" + bring in files...), and a message with a fix gets a button that does it. Teeth: show the raw
    // warning; drop the button's action.
    void messagesOnANodeComeWithTheirFix()
    {
        const json N = json{{"LABEL", "N"}, {"VARIANT", "Original"}, {"LAYERS", json::array({ json{{"ZIP", ""}, {"TARGET", "FILES/%GameDir%"}} })}};
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setIssues({{Hn, {"layer 1 — ZIP needs a file name", "VARIANT 'Original' has no effective entry to run"}}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY(H.shows("bring in files...") && H.shows("+ EXEC layer"));
        QSignalSpy Act(H.Canvas, &PkgCanvas::nodeAction);
        H.click(H.item("bring in files..."));
        H.settle();
        QCOMPARE(Act.count(), 1);
        QCOMPARE(Act.at(0).at(1).toString(), QString("bring_in"));
        H.click(H.item("+ EXEC layer"));
        H.settle();
        bool Exec = false;
        for (const json &L : H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"]) if (L.contains("EXEC")) Exec = true;
        QVERIFY(Exec);
        QVERIFY(PkgGraph::FixFor("layer 1 — ZIP needs a file name").Waiting);
        QVERIFY(!PkgGraph::FixFor("references undefined variable").Waiting);
    }

    // A DLL (or ENV) entry just added has no name yet: the name typed into it is kept, on Enter as on Tab or a click
    // away. Teeth: rename only from a buffer that is rebuilt every frame (the typed name is lost).
    void aNewMapEntryKeepsTheNameTypedIntoIt()
    {
        H.Display = ImVec2(1400, 2400);                    // the order list on screen: a clipped item draws no label
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"DLL":{"":"n,b"}}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY2(H.shows("DLL  "), "no DLL row");
        H.click(H.item("DLL  "));
        H.settle();
        QVERIFY(H.shows("k"));
        H.click(H.item("k"));
        H.type("amd_ags_x64");
        H.key(ImGuiKey_Enter);
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["DLL"], json::parse(R"({"amd_ags_x64":"n,b"})"));
        //"disabled" is the empty order (spec 07 §7.2), not a letter of its own.
        QVERIFY2(H.shows("v"), "no order list");
        H.click(H.item("v"));
        H.settle();
        QVERIFY2(H.shows("disabled"), "the order list did not open");
        H.click(H.item("disabled"));
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["DLL"], json::parse(R"({"amd_ags_x64":""})"));
        //An unnamed entry is removed by its x like any other.
        { json M = H.Doc.Node(H.Doc.IndexOf(Hn)); M["LAYERS"][0]["DLL"] = json::parse(R"({"":"n,b"})"); H.Doc.Replace(H.Doc.IndexOf(Hn), M); }
        H.settle();
        H.click(H.item("x"));
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["DLL"], json::object());
    }

    // An edit op shows the fields its mode reads, and every field it carries: key = value shows Key and Value, not the
    // byte fields; a byte replace shows Offset, Expect, Replace, not Key; an old SECTION an op still carries stays in
    // view. Teeth: show every field; hide what the op carries.
    void anEditOpShowsWhatItsModeReads()
    {
        using PkgGraph::FieldApplies;
        const auto &Ops = PkgGraph::FieldsFor("EDIT")[1].Sub;
        auto Shown = [&](const json &Op) { std::set<std::string> K; for (const auto &F : Ops) if (FieldApplies(F, Op)) K.insert(F.Key); return K; };
        const auto C = Shown(json{{"MODE", "ConfigWrite"}});
        QVERIFY(C.count("KEY") && C.count("VALUE") && C.count("COMMENT") && !C.count("OFFSET") && !C.count("REPLACE") && !C.count("SECTION"));
        const auto R = Shown(json{{"MODE", "Replace"}});
        QVERIFY(R.count("OFFSET") && R.count("EXPECT") && R.count("REPLACE") && !R.count("KEY") && !R.count("VALUE"));
        QVERIFY(Shown(json{{"MODE", "ConfigWrite"}, {"SECTION", "Video"}}).count("SECTION"));
        //And on the canvas: an Overwrite op draws Value, not Offset.
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"EDIT":[{"MODE":"Overwrite","VALUE":"x","LABEL":"the op"}],"TARGET":"FILES/a.ini"}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("EDIT  a.ini (1 ops)"));
        H.settle();
        QVERIFY2(H.shows("the op"), "no op row");
        H.click(H.item("the op"));
        H.settle();
        QVERIFY(H.shows("Value") && !H.shows("Offset") && !H.shows("Payload"));
    }

    // Saving renames an edited node (its CID changes); the rows the author had open stay open under its new name.
    // Teeth: forget the open rows on rename.
    void openRowsSurviveTheSaveThatRenamesTheirNode()
    {
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"DLL":{"a":"n,b"}},{"ENV":{"X":"1"}}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY2(H.shows("DLL  a"), "no DLL row");
        H.click(H.item("DLL  a"));
        H.settle();
        QVERIFY(H.shows("k"));
        json M = H.Doc.Node(0); M["LAYERS"][1]["ENV"]["X"] = "2";
        H.Doc.Replace(0, M); H.Doc.Commit();
        const auto R = H.Doc.Save(Dir.path().toStdString(), std::vector<std::filesystem::path>{}, std::string());
        QVERIFY2(R.Ok, R.Error.c_str());
        const auto Ren = H.Doc.TakeRenames();
        QVERIFY(!Ren.empty());
        H.Canvas->applyRenames(Ren);
        H.settle();
        QVERIFY2(H.shows("k"), "the DLL row closed on save");
    }

    // An entry just added (a keep, a DLL) takes the keyboard: what the author types next is its name, replacing the
    // starter text - no hunting for the new empty row. Teeth: leave the focus where it was (the typing goes nowhere).
    void aNewEntryIsTypedIntoAtOnce()
    {
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"KEEP":{"FILES/%GameDir%/Save/":true}},{"DLL":{"a":"n,b"}}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("KEEP  Save/"));
        H.settle();
        H.click(H.item("+ keep"));
        H.settle();
        H.type("FILES/%Documents%/Game/");
        H.key(ImGuiKey_Enter);
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["KEEP"],
                 json::parse(R"({"FILES/%GameDir%/Save/":true,"FILES/%Documents%/Game/":true})"));
        QVERIFY2(H.shows("DLL  a"), "no DLL row");
        H.click(H.item("DLL  a"));
        H.settle();
        QVERIFY2(H.shows("+ add"), "the DLL row did not open");
        H.click(H.item("+ add"));
        H.settle();
        H.type("d3d8");
        H.key(ImGuiKey_Enter);
        H.settle();
        QVERIFY2(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][1]["DLL"].contains("d3d8"), H.Doc.Node(H.Doc.IndexOf(Hn)).dump().c_str());
    }

    // Wires are told apart by colour: the same wire always gets the same one (named by its node and its place among
    // that node's wires, not by CIDs a save changes); wires leaving one node always differ clearly in hue; none is the
    // red that means exclusion. Teeth: one colour for all; hue from the target only; allow red.
    void eachWireHasItsOwnColour()
    {
        QCOMPARE(PkgCanvas::WireColour("Deus Ex", 1), PkgCanvas::WireColour("Deus Ex", 1));
        auto HueOf = [](unsigned int C) {
            float H, Sa, V;
            ImGui::ColorConvertRGBtoHSV((C & 0xFF) / 255.0f, ((C >> 8) & 0xFF) / 255.0f, ((C >> 16) & 0xFF) / 255.0f, H, Sa, V);
            return H;
        };
        for (const char *From : {"Deus Ex", "Game", "S.T.A.L.K.E.R."})
            for (int A = 0; A < 6; ++A)
            {
                const float Ha = HueOf(PkgCanvas::WireColour(From, A));
                QVERIFY2(Ha > 0.06f && Ha < 0.94f, "a wire took the exclusion red");
                for (int B = A + 1; B < 6; ++B)
                {
                    const float D = std::fabs(Ha - HueOf(PkgCanvas::WireColour(From, B)));
                    QVERIFY2(std::min(D, 1.0f - D) > 0.04f, "two wires of one node share a hue");
                }
            }
        QVERIFY(std::fabs(HueOf(PkgCanvas::WireColour("Deus Ex", 0)) - HueOf(PkgCanvas::WireColour("Deus Ex", 1))) > 0.15f);
    }

    // A list field (an entry's Args) is one entry per line: Enter starts the next one, so a second argument can be
    // typed - not end the edit after the first. Teeth: a one-line input while the list holds one entry.
    void aListFieldTakesASecondEntryOnEnter()
    {
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"EXEC":[{"LABEL":"Play","HOST":"win32","EXE":"%GameDir%/g.exe"}]}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("EXEC  Run: Play"));
        H.settle();
        H.click(H.item("Play"));
        H.settle();
        QVERIFY2(H.shows("Args"), "no Args field");
        H.click(H.item("Args"));
        H.settle();
        H.type("-skipdialog");
        H.key(ImGuiKey_Enter);
        H.type("-localdata");
        H.frames(3);
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["EXEC"][0]["ARGS"], json::parse(R"(["-skipdialog","-localdata"])"));
    }

    // A field emptied is gone: no "COMMENT": "" left in the node; a layer's type key emptied stays (a ZIP waiting for
    // its file). Teeth: keep the empty string; erase the type key too.
    void anEmptiedFieldLeavesNoEmptyKey()
    {
        H.Display = ImVec2(1400, 2400);
        auto Restore = qScopeGuard([this] { H.Display = ImVec2(1400, 900); });
        const json N = json::parse(R"({"LABEL":"N","LAYERS":[{"ZIP":"a.zip","TARGET":"FILES/g","COMMENT":"note"}]})");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("ZIP  a.zip -> g"));
        H.settle();
        H.click(H.item("Comment"));
        H.key(ImGuiKey_A, true);
        H.key(ImGuiKey_Backspace);
        H.settle();
        QVERIFY2(!H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0].contains("COMMENT"), H.Doc.Node(H.Doc.IndexOf(Hn)).dump().c_str());
        H.click(H.item("Zip"));
        H.key(ImGuiKey_A, true);
        H.key(ImGuiKey_Backspace);
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["ZIP"], json(""));
    }

    // A node that has its entry, only unfinished (no Host; the starter Exe, a folder), is offered that entry to finish:
    // the button opens it, Host and Exe in view - no second EXEC layer folding over it. Teeth: always offer "+ EXEC
    // layer"; open only the layer.
    void anUnfinishedEntryIsOpenedToFinish()
    {
        const json N = json::parse(R"({"LABEL":"N","VARIANT":"Original","LAYERS":[{"ZIP":"g.zip","TARGET":"FILES/%GameDir%"},
            {"EXEC":[{"LABEL":"Play","EXE":"%GameDir%/g.exe"}]}]})");
        const std::string Hn = Cid::OfNode(N);
        const json M = json::parse(R"({"LABEL":"M","VARIANT":"Original","LAYERS":[{"EXEC":[{"LABEL":"Play","HOST":"win32","EXE":"%GameDir%/"}]}]})");
        QCOMPARE(PkgGraph::FixFor("VARIANT 'Original' has no effective entry to run", &N).Action, std::string("open_entry:L1/EXEC/0"));
        QCOMPARE(PkgGraph::FixFor("entry 'Play' runs a folder (EXE '%GameDir%/')", &M).Action, std::string("open_entry:L0/EXEC/0"));
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setIssues({{Hn, {"VARIANT 'Original' has no effective entry to run"}}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY(!H.shows("+ EXEC layer"));
        QVERIFY(!H.shows("Host"));
        H.click(H.item("choose its Host"));
        H.settle();
        QVERIFY(H.shows("Host") && H.shows("Exe"));
        QCOMPARE((int)H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"].size(), 2);
    }

    // Another package loaded under the canvas (New package over an open one): until the canvas rebuilds, its views
    // still name the old package's nodes, and a validation result arriving then must not measure them in the new,
    // smaller document. Teeth: drop the revision guard (reads past the new document's nodes; ASan/crash, or throws).
    void issuesArrivingBetweenPackagesMeasureNothingStale()
    {
        package();
        H.settle();
        H.Doc.Reset({});
        try { H.Canvas->setIssues({}); H.Canvas->setIssues({{"anything", {"x"}}}); }
        catch (const std::exception &E) { QFAIL(E.what()); }
        H.settle();
        QCOMPARE(H.Doc.Count(), 0);
    }

    // Its last entry removed, an EXEC layer is empty and the node malformed: the message offers the empty layer gone,
    // and the button takes exactly that layer. Teeth: no fix; remove the wrong layer (the files).
    void anEmptiedLayerIsOfferedGone()
    {
        const json N = json::parse(R"({"LABEL":"V","VARIANT":"Fix","LAYERS":[{"ZIP":"f.zip","TARGET":"FILES/%GameDir%"},{"EXEC":[]}]})");
        const std::string Hn = Cid::OfNode(N);
        QCOMPARE(PkgGraph::FixFor("layer 1 \xe2\x80\x94 EXEC must be a non-empty list of entries", &N).Action, std::string("remove_layer:1"));
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setIssues({{Hn, {"layer 1 \xe2\x80\x94 EXEC must be a non-empty list of entries"}}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("remove the empty layer"));
        H.settle();
        const json After = H.Doc.Node(H.Doc.IndexOf(Hn));
        QCOMPARE((int)After["LAYERS"].size(), 1);
        QCOMPARE(After["LAYERS"][0]["ZIP"], json("f.zip"));
    }

    // Two kept folders stored under one name (a game's and its expansion's "Saved Games/"): the message offers to name
    // the one the launch would drop (named first) after its top folder, and the button gives exactly that address a
    // NAME, keeping its other options. Teeth: no fix; name the other address; drop CLOUD.
    void twoKeepsOfOneNameAreNamedApart()
    {
        const json N = json::parse(R"({"LABEL":"V","VARIANT":"Keep","LAYERS":[{"ZIP":"f.zip","TARGET":"FILES/%GameDir%"},
            {"KEEP":{"FILES/%GameDir%/Base/Data/Saved Games/":true,"FILES/%GameDir%/Expansion/Data/Saved Games/":{"CLOUD":false}}}]})");
        const std::string Hn = Cid::OfNode(N);
        const std::string M = "KEEP '%GameDir%/Expansion/Data/Saved Games/' is stored as 'Saved Games' like '%GameDir%/Base/Data/Saved Games/', "
                              "which takes the name first - it would not be kept; give it its own NAME";
        QCOMPARE(PkgGraph::FixFor(M, &N).Action, std::string("keep_name:FILES/%GameDir%/Expansion/Data/Saved Games/"));
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setIssues({{Hn, {M}}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        H.click(H.item("name it apart"));
        H.settle();
        const json K = H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][1]["KEEP"];
        QCOMPARE(K["FILES/%GameDir%/Expansion/Data/Saved Games/"], json::parse(R"({"CLOUD":false,"NAME":"Expansion Saved Games"})"));
        QCOMPARE(K["FILES/%GameDir%/Base/Data/Saved Games/"], json(true));
        //An address a node it contains keeps (the base game's): a KEEP layer of its own over it, naming it apart.
        json V = json::parse(R"({"LABEL":"Mod","VARIANT":"Mod","LAYERS":[{"NODE":"bafkreibase"}]})");
        QVERIFY(PkgGraph::NameKeepApart(V, "FILES/%GameDir%/Revision/Save/"));
        QCOMPARE(V["LAYERS"][1]["KEEP"], json::parse(R"({"FILES/%GameDir%/Revision/Save/":{"NAME":"Revision Save"}})"));
    }

    // Files from the file manager land on the node drawn topmost under the cursor, never a chip, nothing on the empty
    // canvas. Teeth: take the first node in the list; count chips.
    void filesLandOnTheNodeUnderTheCursor()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.settle();
        const ImVec2 T = H.title(G);
        QCOMPARE(H.Canvas->nodeAt(T.x, T.y), G);
        QCOMPARE(H.Canvas->nodeAt(700, 800), std::string());
        float X0, Y0, X1, Y1;
        if (H.Canvas->nodeRect(Ext, X0, Y0, X1, Y1)) QCOMPARE(H.Canvas->nodeAt((X0 + X1) / 2, (Y0 + Y1) / 2), std::string());
    }

    // A new package starts from what the game is: one version with its files waiting and its tile on the Play entry,
    // DOS and a console's platform known up front (Windows and Linux left for the program's header); a library waits
    // for its files and nothing else. The recipes for a game the package has: another version contains it; a mod
    // rides on it. Teeth: preset win32 for Windows; drop
    // the waiting ZIP layer; give a library a TARGET or a VARIANT; make the mod contain the game instead of riding on it.
    void recipesStartAPackageAndItsVersions()
    {
        using PkgGraph::Recipe;
        const json W = PkgGraph::RecipeNode(Recipe::Windows, "Demo", "7", "");
        QCOMPARE(W["VARIANT"], json("Original"));
        QCOMPARE(PkgGraph::ContentType(W), std::string("ZIP"));
        QCOMPARE(PkgGraph::ContentName(W), std::string());
        const json *Play = nullptr;
        for (const json &L : W["LAYERS"]) if (L.contains("EXEC")) Play = &L["EXEC"][0];
        QVERIFY(Play && !Play->contains("HOST"));
        QCOMPARE((*Play)["TILE"]["UID"], json("7"));
        QCOMPARE(PkgGraph::RecipeNode(Recipe::Dos, "D", "1", "")["LAYERS"][1]["EXEC"][0]["HOST"], json("dos"));
        QCOMPARE(PkgGraph::RecipeNode(Recipe::Console, "S", "2", "snes")["LAYERS"][1]["EXEC"][0]["HOST"], json("snes"));
        std::vector<const json *> Ns{&W};
        QVERIFY(PkgGraph::NextStep(Ns).find("on the node") != std::string::npos);   // its waiting layer: the button there
        const json V = PkgGraph::VersionNode("base", "Demo");
        QCOMPARE(V["LAYERS"][0]["NODE"], json("base"));
        QVERIFY(!V["VARIANT"].get<std::string>().empty());
        const json M = PkgGraph::ModNode("base", "Demo");
        QCOMPARE(M["LAYERS"][0]["ANY"], json::array({"base"}));
        for (const json &L : M["LAYERS"]) QVERIFY(!L.contains("NODE"));
        //A library waits for its files with no place of its own (the game grafting it says where) and is nothing
        //on the shelf: no version, no entry.
        const json Lib = PkgGraph::RecipeNode(Recipe::Library, "mfc42", "", "");
        QCOMPARE(Lib["LABEL"], json("mfc42"));
        QCOMPARE(PkgGraph::ContentType(Lib), std::string("ZIP"));
        QCOMPARE(PkgGraph::ContentName(Lib), std::string());
        QVERIFY(!Lib.contains("VARIANT"));
        for (const json &L : Lib["LAYERS"]) { QVERIFY(!L.contains("EXEC")); QVERIFY(!L.contains("TARGET")); }
    }

    // "+ Node" offers recipes for the package's game: "another version" makes a node containing it (the selected
    // node, else the first version), placed beside it where it hides no node, and selected. Teeth: offer nothing;
    // contain the wrong node; drop it where the view's middle is; leave the game selected.
    void theNodeMenuMakesAnotherVersion()
    {
        const json Game = PkgGraph::RecipeNode(PkgGraph::Recipe::Windows, "Demo", "7", "");
        const std::string Hg = Cid::OfNode(Game);
        const json Other = json{{"LABEL", "Other"}, {"LAYERS", json::array({ json{{"ENV", {{"A", "1"}}}} })}};
        H.Doc.Reset({{Cid::OfNode(Other), Other}, {Hg, Game}});
        //The other node sits right where a version would go beside the game: the new one must find room elsewhere.
        H.Doc.SetPositions({{Hg, {0, 0}}, {Cid::OfNode(Other), {360 + 60, 0}}});
        H.Canvas->documentReset();
        H.Canvas->setZoom(1.0f);
        H.Canvas->select(Hg, false);
        H.settle();
        H.click(H.item("+ Node"));
        H.settle();
        QVERIFY(H.shows("another version"));
        H.click(H.item("another version"));
        H.settle();
        QCOMPARE(H.Doc.Count(), 3);
        const json N = H.Doc.Node(2);
        QCOMPARE(N["LAYERS"][0]["NODE"], json(Hg));
        //It lands beside its game, on top of no node.
        const std::string Hn = H.Doc.Handle(2);
        H.settle();
        const ImVec4 Rn = H.rect(Hn), Rg = H.rect(Hg), Ro = H.rect(Cid::OfNode(Other));
        auto Overlap = [](const ImVec4 &A, const ImVec4 &B) { return A.x < B.z && B.x < A.z && A.y < B.w && B.y < A.w; };
        QVERIFY2(!Overlap(Rn, Rg) && !Overlap(Rn, Ro), "the new version hides another node");
        QVERIFY2(Rn.x > Rg.x, "the new version is not beside its game");
        QCOMPARE(H.Canvas->selectedHandle(), Hn);              // the node being worked on: the JSON pane follows it
    }

    // A package with no version yet - only the game's files and its tile at the base (an install captured) - offers
    // "another version" over that base, with nothing selected. Teeth: look for versions only; take the other node.
    void theFirstVersionBuildsOnTheTileAtTheBase()
    {
        const json Base = json{{"LABEL", "Demo files"}, {"LAYERS", json::array({ json{{"ZIP", "demo.zip"}, {"TARGET", "FILES/%GameDir%"}},
                                 json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"TILE", {{"UID", "7"}, {"TITLE", "Demo"}}}} })}} })}};
        const json Other = json{{"LABEL", "Other"}, {"LAYERS", json::array({ json{{"ENV", {{"A", "1"}}}} })}};
        const std::string Hb = Cid::OfNode(Base);
        H.Doc.Reset({{Cid::OfNode(Other), Other}, {Hb, Base}});
        H.Canvas->documentReset();
        H.Canvas->setZoom(1.0f);
        H.Canvas->frameAll();
        H.settle();
        QVERIFY(H.Canvas->selectedHandle().empty());
        H.click(H.item("+ Node"));
        H.settle();
        H.click(H.item("another version"));
        H.settle();
        QCOMPARE(H.Doc.Count(), 3);
        QCOMPARE(H.Doc.Node(2)["LAYERS"][0]["NODE"], json(Hb));
    }

    // A layer's TARGET has a list of places in words beside it (the game's folder, Documents, AppData...); picking one
    // writes its anchor. Teeth: drop the list; write the place's words.
    void aTargetIsPickedFromPlaces()
    {
        const json N = Zip("Saves", "s.zip");
        const std::string Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.Canvas->setExpanded(Hn, true);
        H.settle();
        H.click(H.item("places"));
        H.settle();
        QVERIFY(H.shows("Documents"));
        H.click(H.item("Documents"));
        H.settle();
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(Hn))["LAYERS"][0]["TARGET"], json("FILES/%Documents%"));
    }

    // Quick add: "/" over the canvas opens a search; typed words narrow it (a type by its plain words too) and Enter
    // adds the first match where the cursor was. Teeth: no shortcut; match the keyword only ("registry" finds nothing).
    void quickAddFindsWhatIsTyped()
    {
        package();
        H.Canvas->setZoom(1.0f);
        H.settle();
        const int Before = H.Doc.Count();
        H.frame(ImVec2(700, 800));
        H.key(ImGuiKey_Slash);
        H.settle();
        QVERIFY2(H.shows("quick"), "no quick-add box");
        H.type("registry");
        H.key(ImGuiKey_Enter);
        H.settle();
        QCOMPARE(H.Doc.Count(), Before + 1);
        QCOMPARE(PkgGraph::KindOf(H.Doc.Node(H.Doc.Count() - 1)), std::string("REG"));
    }

    // A node in words: what it runs and on what, where its files come from, what it contains, rides on or never meets,
    // what it changes and keeps; a version says so first. Teeth: drop the platform words; name references by CID.
    void aNodeIsDescribedInWords()
    {
        const json N = json{{"VARIANT", "Original"}, {"LAYERS", json::array({
            json{{"ZIP", "demo.zip"}}, json{{"NODE", "base"}}, json{{"ANY", json::array({"game"})}},
            json{{"EDIT", json::array({ json::object(), json::object() })}}, json{{"KEEP", {{"FILES/%Documents%/Demo/", true}, {"FILES/%GameDir%/cfg.ini", false}}}},
            json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "%GameDir%/bin/demo.exe"}} })}} })}};
        const std::string D = PkgGraph::Describe(N, [](const std::string &R) { return R == "base" ? std::string("Core Files") : std::string("The Game"); });
        for (const char *Want : {"Version \"Original\"", "runs demo.exe on Windows 32-bit", "files from demo.zip", "contains \"Core Files\"",
                                 "rides on \"The Game\"", "changes 2 settings", "keeps Documents/Demo"})
            QVERIFY2(D.find(Want) != std::string::npos, (D + "  <- missing: " + Want).c_str());
        QVERIFY2(D.find("cfg.ini") == std::string::npos, (D + "  <- a key taken back (KEEP false) is not kept").c_str());
        QCOMPARE(PkgGraph::Describe(json{{"LAYERS", json::array()}}), std::string("Nothing yet"));
        const json R = json{{"LAYERS", json::array({ json{{"EXEC", json::array({ json{{"HOST", "linux64"}, {"GUEST", json::array({"snes"})}} })}} })}};
        QCOMPARE(PkgGraph::Describe(R), std::string("Plays Super Nintendo games on Linux"));
    }

    // The fold flattened in the order it is applied: a contained node's layers where it is contained, one deeper; a
    // node of another package is one step, not entered; a cycle is walked once. Teeth: list contained layers after the
    // container's own; enter cycles forever.
    void theFoldIsListedInTheOrderItApplies()
    {
        std::map<std::string, json> Ns{
            {"top", json{{"LAYERS", json::array({ json{{"ENV", {{"a", "1"}}}}, json{{"NODE", "mid"}}, json{{"ZIP", "top.zip"}}, json{{"NODE", "far"}} })}}},
            {"mid", json{{"LAYERS", json::array({ json{{"ZIP", "mid.zip"}}, json{{"NODE", "top"}} })}}}};
        auto Find = [&](const std::string &H) -> const json * { const auto It = Ns.find(H); return It == Ns.end() ? nullptr : &It->second; };
        const auto St = PkgGraph::FoldOrder("top", Find);
        QCOMPARE((int)St.size(), 4);                        // mid's NODE top is the cycle: top is on the path, not re-entered
        QCOMPARE(St[0].Type, std::string("ENV"));
        QCOMPARE(St[1].Title, std::string("mid.zip"));
        QCOMPARE(St[1].Depth, 1);
        QCOMPARE(St[2].Title, std::string("top.zip"));
        QVERIFY(St[3].Outside && St[3].Title == "far");
    }

    // The fold shown is the fold a launch makes (Fold::Resolve): a layer whose WHEN is false at the options' defaults
    // is not in it, a node reached twice from different lists is folded once (held where it first occurs), and a
    // contained node's layers say which TAKE they are seen through. Teeth: walk the layers as written (the off layer
    // listed, lib.zip twice); drop the take.
    void theFoldShownIsTheFoldALaunchMakes()
    {
        std::map<std::string, json> Ns{
            {"top", json{{"LAYERS", json::array({ json{{"VARS", {{"hd", {{"DEFAULT", "0"}}}}}},
                                                   json{{"ZIP", "hd.zip"}, {"WHEN", "%hd%==1"}},
                                                   json{{"ZIP", "sd.zip"}, {"WHEN", "%hd%==0"}},
                                                   json{{"NODE", "lib"}},
                                                   json{{"NODE", "mid"}},
                                                   json{{"NODE", "maps"}, {"TAKE", json::array({"FILES/%GameDir%/maps"})}} })}}},
            {"mid", json{{"LAYERS", json::array({ json{{"NODE", "lib"}}, json{{"ZIP", "mid.zip"}} })}}},
            {"maps", json{{"LAYERS", json::array({ json{{"ZIP", "maps.zip"}, {"TARGET", "FILES/%GameDir%"}} })}}},
            {"lib", json{{"LAYERS", json::array({ json{{"ZIP", "lib.zip"}} })}}}};
        auto Find = [&](const std::string &H) -> const json * { const auto It = Ns.find(H); return It == Ns.end() ? nullptr : &It->second; };
        const auto St = PkgGraph::FoldOrder("top", Find);
        std::vector<std::string> Titles;
        for (const auto &S : St) Titles.push_back(S.Title);
        QCOMPARE((int)std::count(Titles.begin(), Titles.end(), std::string("hd.zip")), 0);
        QCOMPARE((int)std::count(Titles.begin(), Titles.end(), std::string("sd.zip")), 1);
        QCOMPARE((int)std::count(Titles.begin(), Titles.end(), std::string("lib.zip")), 1);
        const auto Maps = std::find_if(St.begin(), St.end(), [](const PkgGraph::FoldStep &S) { return S.Title == "maps.zip"; });
        QVERIFY(Maps != St.end());
        QCOMPARE(Maps->Depth, 1);
        QCOMPARE(Maps->Take, std::string("FILES/%GameDir%/maps"));
    }

    // The shelf card a version shows: the tile its fold presents (its own, else a contained node's), and every version
    // of the package with the recommended one marked. Teeth: look only at the node's own layers; mark none recommended.
    void aVersionsShelfCardComesFromItsFold()
    {
        std::map<std::string, json> Ns{
            {"tile", json{{"LAYERS", json::array({ json{{"EXEC", json::array({ json{{"TILE", {{"UID", "9"}, {"TITLE", "Nine"}, {"COVER", {{"FILE", "c.png"}}}}}} })}} })}}},
            {"orig", json{{"VARIANT", "Original"}, {"LAYERS", json::array({ json{{"NODE", "tile"}} })}}},
            {"port", json{{"VARIANT", "Source port"}, {"RECOMMENDED", json::array({"9"})}, {"LAYERS", json::array({ json{{"NODE", "orig"}} })}}}};
        auto Find = [&](const std::string &H) -> const json * { const auto It = Ns.find(H); return It == Ns.end() ? nullptr : &It->second; };
        const auto C = PkgGraph::CardFor("port", {"tile", "orig", "port"}, Find);
        QCOMPARE(C.Title, std::string("Nine"));
        QCOMPARE(C.Cover, std::string("c.png"));
        QCOMPARE((int)C.Versions.size(), 2);
        bool PortRec = false, OrigRec = true;
        for (const auto &[N, R] : C.Versions) { if (N == "Source port") PortRec = R; if (N == "Original") OrigRec = R; }
        QVERIFY(PortRec && !OrigRec);
    }

    // The card's tile is the folded one, as the shelf takes it: a version that renames the tile it contains shows
    // the new title on the contained UID. Teeth: take the first layer naming a tile, unmerged (no UID, or the old
    // title).
    void aVersionsCardShowsItsFoldedTile()
    {
        const json Play = json{{"LABEL", "Play"}, {"TILE", {{"UID", "9"}, {"TITLE", "Nine"}}}};
        std::map<std::string, json> Ns{
            {"orig", json{{"VARIANT", "Original"}, {"LAYERS", json::array({ json{{"EXEC", json::array({ Play })}} })}}},
            {"hd", json{{"VARIANT", "HD"}, {"LAYERS", json::array({ json{{"NODE", "orig"}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"TILE", {{"TITLE", "Nine HD"}}}} })}} })}}}};
        auto Find = [&](const std::string &H) -> const json * { const auto It = Ns.find(H); return It == Ns.end() ? nullptr : &It->second; };
        const auto C = PkgGraph::CardFor("hd", {"orig", "hd"}, Find);
        QCOMPARE(C.Uid, std::string("9"));
        QCOMPARE(C.Title, std::string("Nine HD"));
    }

    // A version (or an entry) has Test Play on its title bar: it asks for the test launch, and does not start a drag
    // of the node. A node that cannot run has none. Teeth: no button; the title's drag takes the click.
    void aVersionHasTestPlayOnItsTitle()
    {
        const json V = PkgGraph::RecipeNode(PkgGraph::Recipe::Dos, "Demo", "1", "");
        const json N = Zip("Data", "d.zip");
        const std::string Hv = Cid::OfNode(V), Hn = Cid::OfNode(N);
        H.Doc.Reset({{Hv, V}, {Hn, N}});
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY(H.shows("testplay"));
        QVERIFY(!H.shows("testplay", 1));                  // the data node has none
        const ImVec4 Before = H.rect(Hv);
        QSignalSpy Act(H.Canvas, &PkgCanvas::nodeAction);
        H.click(H.item("testplay"));
        H.settle();
        QCOMPARE(Act.count(), 1);
        QCOMPARE(Act.at(0).at(1).toString(), QString("test_launch"));
        QCOMPARE(H.rect(Hv).x, Before.x);
    }

    // A chip of another package's game shows that game's cover, when its node presents one and the image loads.
    // Teeth: never draw it; draw it for a chip without a cover.
    void aChipShowsItsGamesCover()
    {
        package();
        H.Canvas->setExternalLookup([this](const std::string &C) {     // after package(): it installs its own
            return C == Ext ? PkgCanvas::External{"Core Fonts", "Fonts Library", "/lib/fonts", "/lib/fonts/cover.png"} : PkgCanvas::External{};
        });
        int Asked = 0;
        H.Canvas->setImageLookup([&Asked](const std::string &P, unsigned long long &Id, float &W, float &Hh) {
            ++Asked;
            if (P != "/lib/fonts/cover.png") return false;
            Id = 1; W = 200; Hh = 300;
            return true;
        });
        H.Canvas->setZoom(1.0f);
        H.settle();
        QVERIFY(Asked > 0);
        QVERIFY(H.Canvas->coverShown(Ext));
        H.Canvas->setImageLookup([](const std::string &, unsigned long long &, float &, float &) { return false; });
        H.frames(2);
        QVERIFY(!H.Canvas->coverShown(Ext));
    }

    // The first-time tour: cards one at a time, Next through to "Got it" (finished), or Skip (finished as well).
    // Teeth: never finish; finish on the first Next.
    void theTourWalksThroughItsCards()
    {
        package();
        QSignalSpy Done(H.Canvas, &PkgCanvas::tourFinished);
        H.Canvas->startTour();
        H.settle();
        int Nexts = 0;
        while (H.shows("Next") && Nexts < 10) { H.click(H.item("Next")); H.settle(); ++Nexts; QCOMPARE(Done.count(), 0); }
        QCOMPARE(Nexts, 4);
        QVERIFY(H.shows("Got it"));
        H.click(H.item("Got it"));
        H.settle();
        QCOMPARE(Done.count(), 1);
        QVERIFY(!H.Canvas->touring());
        H.Canvas->startTour();
        H.settle();
        H.click(H.item("Skip the tour"));
        H.settle();
        QCOMPARE(Done.count(), 2);
        QVERIFY(!H.shows("Next"));
    }

    // The probe's file is what tools/edrive.py drives the editor by: every item of the frame with its label as drawn
    // (quotes and all - an entry's fold is titled by its LABEL), its rectangle and the canvas's place on screen. Teeth: skip the escaping (the file stops
    // parsing); drop the origin.
    void theProbeWritesEachItemByItsLabel()
    {
        const json N = json{{"LABEL", "n"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({ json{{"LABEL", "Say \"hi\"\\now"}, {"HOST", "win32"}} })}} })}};
        H.Doc.Reset({{"n1", N}});
        H.Canvas->setZoom(1.0f);
        H.Canvas->setExpanded("n1", true);
        H.frames(4);
        QVERIFY(ImGuiProbe::Enabled());
        ImGuiProbe::WriteFrame(40, 70, 1400, 900, 1.0);
        QFile F(ProbeDir.path() + "/probe.json");
        QVERIFY(F.open(QIODevice::ReadOnly));
        const json P = json::parse(F.readAll().toStdString(), nullptr, false);
        QVERIFY(P.is_object());
        QCOMPARE(P["origin"], json::array({40, 70}));
        QVERIFY(!P["items"].empty());
        bool Seen = false;
        for (const json &I : P["items"])
        {
            QVERIFY(I["rect"].size() == 4 && I["rect"][2].get<double>() > I["rect"][0].get<double>());
            if (I["label"].get<std::string>().find("Say \"hi\"\\now") != std::string::npos) Seen = true;
        }
        QVERIFY2(Seen, "the entry's fold is not among the items");
        QVERIFY(!QFile::exists(ProbeDir.path() + "/probe.json.tmp"));
    }
};

QTEST_MAIN(PkgCanvasTest)
#include "test_pkgcanvas.moc"
