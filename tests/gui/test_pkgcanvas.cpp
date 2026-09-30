// The package canvas, driven headlessly (see canvasharness.h): what a person does with a mouse and a keyboard, and
// what the package document looks like afterwards. Every test names what it pins and how it was made to fail.

#include "canvasharness.h"
#include "pkggraph.h"
#include "cid.h"

#include <QtTest>
#include <QDir>

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
            return C == Ext ? PkgCanvas::External{"Core Fonts", "Fonts Library", "/lib/fonts"} : PkgCanvas::External{};
        });
        H.frames(4);                                      // layout, measure, re-layout, settle
    }

private slots:
    void initTestCase() { CanvasHarness::InitContext(); }
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
    // own layers); with Shift held it REQUIRES it instead (ANY). Dropped on empty canvas: nothing. Teeth: swap the
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
        //Onto nothing: no change.
        const json Now = H.Doc.Node(H.Doc.IndexOf(M));
        H.drag(H.port(A, true), H.empty());
        QCOMPARE(H.Doc.Node(H.Doc.IndexOf(M)), Now);
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
        //The midpoint of the Mod <- Game wire, where no node is.
        const ImVec2 P0 = H.port(G, true), P1 = H.port(M, false);
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
};

QTEST_MAIN(PkgCanvasTest)
#include "test_pkgcanvas.moc"
