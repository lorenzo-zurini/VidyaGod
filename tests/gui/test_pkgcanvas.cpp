// The package canvas, driven headlessly: Dear ImGui needs no GPU to lay out and process input, so the whole
// editing surface — node creation, wiring, payload edits, deletion, position persistence — runs under QTest
// with a synthetic mouse. This is the thing the old tab-and-form editor could never have: its logic lived
// inside QWidget constructors, so there was nothing to call.

#include "pkgcanvas.h"
#include "pkggraph.h"
#include "commonutils.h"
#include "pkglayout.h"
#include "nodelower.h"
#include "manifestmodel.h"

#include "imgui.h"
#include "imgui_internal.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imnodes.h"
#include "imnodes_internal.h"   // the colour-modifier stack, which imnodes never clears per frame

#include <QtTest>

#include <array>
#include <cstring>
#include <functional>
#include <cmath>
#include <algorithm>
#include <vector>
#include <QDirIterator>

#include <fstream>

using json = nlohmann::ordered_json;

class PkgCanvasTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char *px = nullptr; int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);          // the GL backend normally builds the atlas
        QVERIFY(io.Fonts->IsBuilt());
    }
    void cleanupTestCase() { ImGui::DestroyContext(); }

    void init()
    {
        Doc = json{{"NODES", json::array()}};
        Layout = json::object();
        Saves = 0;
        Canvas = new PkgCanvas(&Doc, [this]{ ++Saves; ++FullSaves; }, nullptr, &Layout,
                               [this]{ ++Saves; ++LayoutSaves; });
        Canvas->initContexts();
    }
    void cleanup()
    {
        Canvas->shutdownContexts();
        delete Canvas; Canvas = nullptr;
    }

    // Adding nodes and wiring them writes PARENTS into the very document the model persists — there is no
    // second representation, so "the graph is the package" is a testable claim, not a slogan.
    void wiringWritesParentsIntoTheDocument()
    {
        const int content = Canvas->addNode("Content", 40, 40);
        const int exec    = Canvas->addNode("DeclareExec", 400, 40);
        runFrame();

        QCOMPARE(Canvas->nodeCount(), 2);
        QVERIFY(Canvas->connect(content, exec));
        QCOMPARE(Doc["NODES"][exec]["PARENTS"].size(), size_t(1));
        QCOMPARE(Doc["NODES"][exec]["PARENTS"][0].get<std::string>(),
                 Doc["NODES"][content]["NODE_ID"].get<std::string>());

        QVERIFY(!Canvas->connect(content, exec));            // idempotent — no duplicate edge
        QVERIFY(Canvas->disconnect(content, exec));
        QCOMPARE(Doc["NODES"][exec]["PARENTS"].size(), size_t(0));
    }

    // A fresh node is valid by construction. The old editor created content layers with NO TARGET at all,
    // which is exactly how Tonic Trouble died as Proton "create process: 2".
    void newContentNodeIsAnchored()
    {
        const int i = Canvas->addNode("Content");
        QCOMPARE(Doc["NODES"][i]["FORM"].get<std::string>(), std::string("zip"));
        QCOMPARE(Doc["NODES"][i]["TARGET"].get<std::string>(),
                 std::string("%PrefixRoot%/drive_c/%PackageUID%"));
        QVERIFY(!Doc["NODES"][i]["NODE_ID"].get<std::string>().empty());
    }

    // Ids are unique by construction (the old editor named every new node "new_node"), a rename actually
    // RE-POINTS every reference, and a rename onto a name in use is REFUSED — otherwise both nodes map to
    // <id>.json, SaveNodes writes them as one array and the loader keeps first-seen, silently dropping a node.
    void idsAreUniqueAndRenamesRepointChildren()
    {
        const int a = Canvas->addNode("Content");
        const int b = Canvas->addNode("Content");
        const std::string aId = Doc["NODES"][a]["NODE_ID"].get<std::string>();
        const std::string bId = Doc["NODES"][b]["NODE_ID"].get<std::string>();
        QVERIFY(aId != bId);

        QVERIFY(Canvas->connect(a, b));                                  // b depends on a
        QCOMPARE(Doc["NODES"][b]["PARENTS"][0].get<std::string>(), aId);

        QVERIFY(Canvas->renameNode(a, "renamed_base"));
        QCOMPARE(Doc["NODES"][a]["NODE_ID"].get<std::string>(), std::string("renamed_base"));
        QCOMPARE(Doc["NODES"][b]["PARENTS"][0].get<std::string>(), std::string("renamed_base"));  // re-pointed

        QVERIFY(!Canvas->renameNode(b, "renamed_base"));                 // name taken -> refused
        QCOMPARE(Doc["NODES"][b]["NODE_ID"].get<std::string>(), bId);    // and b is untouched
        QVERIFY(!Canvas->renameNode(a, ""));                             // empty -> refused
    }

    // A stored position must actually be restored: imnodes has no persisted state of its own, so a node whose
    // position was never pushed in renders at the origin — and the next mouse-release would write [0,0] over
    // every node in the bundle. This is the test the old one couldn't be: it renders real frames.
    void storedPositionsSurviveRendering()
    {
        Doc["NODES"] = json::array({
            json{{"NODE_ID","a"},{"TYPE","Content"},{"FORM","zip"},{"PATH","a.zip"}},
            json{{"NODE_ID","b"},{"TYPE","Content"},{"FORM","zip"},{"PATH","b.zip"}},
        });
        Layout = json{{"a", json::array({250.0, 140.0})}, {"b", json::array({900.0, 430.0})}};
        Canvas->invalidateGraph();
        runFrame();
        runFrame();                                        // a second frame is where a bad read-back lands
        QCOMPARE(Layout["a"][0].get<double>(), 250.0);
        QCOMPARE(Layout["a"][1].get<double>(), 140.0);
        QCOMPARE(Layout["b"][0].get<double>(), 900.0);
        QCOMPARE(Layout["b"][1].get<double>(), 430.0);
    }

    // Deleting a node takes its inbound references with it, so the graph never carries a dangling parent.
    void deletingANodeDropsReferencesToIt()
    {
        const int a = Canvas->addNode("Content");
        const int b = Canvas->addNode("DeclareExec");
        QVERIFY(Canvas->connect(a, b));
        QVERIFY(Canvas->removeNode(a));
        QCOMPARE(Canvas->nodeCount(), 1);
        QCOMPARE(Doc["NODES"][0]["PARENTS"].size(), size_t(0));   // b's edge went with it
    }

    // Out-of-bundle parents (MediaStack_MS, asiloader…) are reference chips, not boxes we own.
    void externalParentsBecomeChips()
    {
        const int b = Canvas->addNode("DeclareExec");
        Doc["NODES"][b]["PARENTS"] = json::array({"MediaStack_MS"});
        const PkgGraph::Graph g = Canvas->graph();
        QCOMPARE(g.Externals.size(), size_t(1));
        QCOMPARE(g.Externals[0], std::string("MediaStack_MS"));
        QCOMPARE(g.Links.size(), size_t(1));
        QCOMPARE(g.Links[0].ParentIndex, -1);
    }

    // Layout round-trips through the SIDECAR, and — the point of the sidecar — never touches the package. A
    // Meta-CID is minted add-by-reference IN PLACE over the node files, so a position stored in one would make
    // dragging a box change the package's bytes, and therefore its CID, for every peer.
    void positionsPersistIntoTheLayoutSidecarNotThePackage()
    {
        const int i = Canvas->addNode("Content", 123.0f, 456.0f);
        const std::string id = Doc["NODES"][i]["NODE_ID"].get<std::string>();
        QVERIFY(!Doc["NODES"][i].contains("POS"));               // NOT in the package
        QVERIFY(Layout.contains(id));                            // in the sidecar
        QCOMPARE(Layout[id][0].get<double>(), 123.0);
        const PkgGraph::Graph g = Canvas->graph();
        QVERIFY(g.Nodes[i].HasPos);
        QCOMPARE(g.Nodes[i].X, 123.0f);
        QCOMPARE(g.Nodes[i].Y, 456.0f);

        // Dragging a node must not dirty the package at all: a full render pass leaves the node bytes identical.
        const std::string before = Doc.dump();
        runFrame(); runFrame();
        QCOMPARE(Doc.dump(), before);

        // And deleting the node reclaims its layout entry rather than leaving it to be inherited by a
        // later node that happens to reuse the id.
        QVERIFY(Canvas->removeNode(i));
        QVERIFY(!Layout.contains(id));
    }

    // Un-positioned bundles (every hand-authored package in the library) open laid out by dependency depth
    // rather than stacked at the origin.
    void unpositionedNodesAreAutoLaidOut()
    {
        Doc["NODES"] = json::array({
            json{{"NODE_ID","base"},{"TYPE","Content"},{"FORM","zip"},{"PATH","a.zip"}},
            json{{"NODE_ID","mid"}, {"TYPE","Content"},{"FORM","zip"},{"PATH","b.zip"},{"PARENTS",json::array({"base"})}},
            json{{"NODE_ID","tip"}, {"TYPE","DeclareExec"},{"HOST","win32"},{"PARENTS",json::array({"mid"})}},
        });
        const PkgGraph::Graph g = Canvas->graph();
        QVERIFY(g.Nodes[0].X < g.Nodes[1].X);                 // depth increases left → right
        QVERIFY(g.Nodes[1].X < g.Nodes[2].X);
        for (const auto &n : g.Nodes) QVERIFY(!n.HasPos);     // nothing was written to disk by laying out
    }

    // A key's DEFAULT value is stored under the EMPTY NAME. "Create this key, no values" is a different
    // thing, and inferring it from the empty name destroyed every default value on any edit — 48 of them live
    // in LAVFilters carrying the whole DirectShow COM registration, so touching one row there silently broke
    // every codec. The distinction is a FLAG, and these are the assertions that make reinstating the bug fail.
    void registryDefaultValuesSurviveAndKeyOnlyRowsStayKeyOnly()
    {
        json entry = json{{"ARCHITECTURE", json::array({"64"})},
                          {"COMMENT", "why this exists"},
                          {"HKLM", {{"Classes", {{"CLSID", {{"{abc}", {{"", "LAV Splitter"},
                                                                      {"Merit", "dword:00600000"}}}}}}}}},
                          {"HKCU", {{"Software", {{"EmptyKey", json::object()}}}}}};
        const std::string before = entry.dump();

        const auto rows = PkgGraph::RegRowsOf(entry);
        int defaults = 0, keyOnly = 0;
        for (const auto &r : rows)
        {
            if (r.KeyOnly) ++keyOnly;
            else if (r.Name.empty()) ++defaults;
        }
        QCOMPARE(defaults, 1);                       // the CLSID's default value
        QCOMPARE(keyOnly, 1);                        // ...and the genuinely empty key, told apart

        json rebuilt = entry;
        PkgGraph::RegRowsInto(rebuilt, rows);
        QVERIFY2(rebuilt["HKLM"]["Classes"]["CLSID"]["{abc}"].contains(""),
                 "the key's DEFAULT value was destroyed");
        QCOMPARE(rebuilt["HKLM"]["Classes"]["CLSID"]["{abc}"][""].get<std::string>(),
                 std::string("LAV Splitter"));
        QVERIFY(rebuilt["HKCU"]["Software"]["EmptyKey"].is_object());
        QVERIFY(rebuilt["HKCU"]["Software"]["EmptyKey"].empty());        // still key-only
        // Non-hive fields survive (COMMENT is legal here and NodeLower goes out of its way to permit it)...
        QCOMPARE(rebuilt.value("COMMENT", std::string()), std::string("why this exists"));
        // ...and the whole entry is byte-identical: a round trip must not churn the package's CID.
        QCOMPARE(rebuilt.dump(), before);
    }

    // The same, over the REAL library: the shape that actually ships is the one that must round-trip.
    void everyRegEditInTheLibraryRoundTripsByteIdentically()
    {
        const QString Root = QDir::homePath() + "/.VidyaGod/LIBRARY";
        if (!QDir(Root).exists()) QSKIP("no local library to check");
        int checked = 0, bad = 0;
        QDirIterator It(Root, {"*.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (It.hasNext())
        {
            std::ifstream In(It.next().toStdString());
            json D; if (!In) continue;
            try { In >> D; } catch (...) { continue; }
            for (auto &N : (D.is_array() ? D : json::array({D})))
            {
                if (!N.is_object() || N.value("TYPE", std::string()) != "RegEdit") continue;
                if (!N.contains("EDITS") || !N["EDITS"].is_array()) continue;
                for (auto &E : N["EDITS"])
                {
                    json After = E;
                    PkgGraph::RegRowsInto(After, PkgGraph::RegRowsOf(E));
                    ++checked;
                    if (After.dump() != E.dump()) { ++bad;
                        qWarning("DIFF in %s", N.value("NODE_ID", std::string()).c_str()); }
                }
            }
        }
        QVERIFY2(checked > 0, "found no RegEdit entries to check");
        QCOMPARE(bad, 0);
    }

    // The registry is a TREE on disk and flat key paths to a human; the editor round-trips between them.
    void registryRowsRoundTripThroughTheTree()
    {
        json entry = json::object({{"ARCHITECTURE", json::array({"32"})}});
        PkgGraph::RegRowsInto(entry, {{"HKLM\\Software\\Ubi Soft\\TONICT", "Version", "1.00"},
                                      {"HKLM\\Software\\Ubi Soft\\TONICT", "Lang",    "en"}});
        QVERIFY(entry.contains("HKLM"));
        QCOMPARE(entry["HKLM"]["Software"]["Ubi Soft"]["TONICT"]["Version"].get<std::string>(), std::string("1.00"));
        QCOMPARE(entry["ARCHITECTURE"].size(), size_t(1));    // non-hive fields survive the rebuild

        const auto rows = PkgGraph::RegRowsOf(entry);
        QCOMPARE(rows.size(), size_t(2));
        QCOMPARE(rows[0].Path, std::string("HKLM\\Software\\Ubi Soft\\TONICT"));
    }

    // Actions are CONTEXTUAL, not a fixed per-type list: the thing you can do depends on what the node is and
    // where it sits. A zip can become a dir; only a delta can be flattened; "make delta" needs a base to diff
    // against, which means a Content PARENT.
    void actionsAreContextual()
    {
        const int zip = Canvas->addNode("Content");
        auto names = [&](int i, const std::vector<std::string> &hints = {}) {
            std::vector<std::string> out;
            for (const auto &a : PkgGraph::ActionsFor(Doc["NODES"][i], Canvas->graph(), i, hints))
                out.push_back(a.Id);
            return out;
        };
        auto has = [](const std::vector<std::string> &v, const char *x) {
            return std::find(v.begin(), v.end(), std::string(x)) != v.end();
        };

        QVERIFY(has(names(zip), "to_dir"));            // it is a zip
        QVERIFY(!has(names(zip), "flatten"));          // ...so there is nothing to flatten
        QVERIFY(!has(names(zip), "to_delta"));         // ...and no parent to diff against yet
        QVERIFY(!has(names(zip), "restore"));          // not known to be DEFLATE
        QVERIFY(has(names(zip, {"deflate"}), "restore"));   // the host probed the zip: now it is offered

        const int child = Canvas->addNode("Content");
        QVERIFY(Canvas->connect(zip, child));
        QVERIFY(has(names(child), "to_delta"));        // its parent is Content — a base exists

        // A delta offers exactly one reverse conversion — undelta, the inverse of "-> delta". It is not a zip,
        // so the zip actions do not apply until it has been undelta'd.
        Doc["NODES"][child]["FORM"] = "delta";
        QVERIFY(has(names(child), "undelta"));
        QVERIFY(!has(names(child), "to_dir"));         // a delta is not a zip
        QVERIFY(!has(names(child), "to_delta"));       // it already is one
        QVERIFY(!has(names(child), "restore"));

        // Capture is available at ANY point along the chain — that is the whole premise.
        for (const std::string &t : PkgGraph::AllTypes())
        {
            const int i = Canvas->addNode(t);
            QVERIFY2(has(names(i), "capture_setup"), t.c_str());
        }

        // A runner (has GUEST) is not a launchable, so it offers no test launch.
        const int exec = Canvas->addNode("DeclareExec");
        QVERIFY(has(names(exec), "test_launch"));
        Doc["NODES"][exec]["GUEST"] = json::array({"win32"});
        QVERIFY(!has(names(exec), "test_launch"));
    }

    // A node running a heavy action is locked and shows progress; ending it always unlocks. A node left locked
    // forever is the failure mode, so the begin/end pairing is what this pins.
    void busyStateLocksAndAlwaysClears()
    {
        const int i = Canvas->addNode("Content");
        const std::string id = Doc["NODES"][i]["NODE_ID"].get<std::string>();
        QVERIFY(!Canvas->isBusy(id));

        Canvas->beginAction(id, "packing zip…", true);
        QVERIFY(Canvas->isBusy(id));
        Canvas->setProgress(id, 0.5f, "20 / 40");
        runFrame();                                    // draws the bar + cancel instead of the buttons
        QVERIFY(Canvas->isBusy(id));

        Canvas->endAction(id);
        QVERIFY(!Canvas->isBusy(id));
        runFrame();

        // Busy state is canvas-local: it must never leak into the document that gets written to disk.
        QVERIFY(!Doc["NODES"][i].contains("BUSY"));
        QVERIFY(Doc["NODES"][i].dump().find("packing") == std::string::npos);
    }

    // Every wire must be drawn, including on a node with SEVERAL parents — which is every real launchable.
    // syncLinks maps a rendered link back to a PARENTS index; an attempt that counted occurrences per child
    // silently dropped every wire after the first, so the canvas showed real dependencies as unwired. Drawing
    // is what exercises it, so this renders frames and then checks the graph the renderer was handed.
    void multiParentNodesKeepEveryWire()
    {
        const int a = Canvas->addNode("Content");
        const int b = Canvas->addNode("Content");
        const int c = Canvas->addNode("RegEdit");
        const int exec = Canvas->addNode("DeclareExec");
        QVERIFY(Canvas->connect(a, exec));
        QVERIFY(Canvas->connect(b, exec));
        QVERIFY(Canvas->connect(c, exec));
        QVERIFY(Canvas->connectExternal("MediaStack_MS", exec));
        runFrame();
        runFrame();

        const PkgGraph::Graph g = Canvas->graph();
        int into = 0;
        for (const auto &l : g.Links) if (l.ChildIndex == exec) ++into;
        QCOMPARE(into, 4);                                   // three in-bundle + one external chip
        QCOMPARE(Doc["NODES"][exec]["PARENTS"].size(), size_t(4));
        // The externals list is what the chips are drawn from; a missing entry means a wire with no source.
        QCOMPARE(g.Externals.size(), size_t(1));
    }

    // A rename must carry EVERY reference to the node, not just the edges. The ones that are not PARENTS fail
    // quietly: EXCLUDE stops excluding (both variants become selectable, caught later only as a warning), a
    // RUNNER pin falls back to the default runner with nothing catching it, and the canvas position is lost —
    // which matters because renameNode runs per KEYSTROKE, so the box would jump on the first character typed.
    void renamingANodeCarriesEveryReferenceToIt()
    {
        const int a = Canvas->addNode("Content", 300.0f, 400.0f);
        const int b = Canvas->addNode("DeclareExec");
        const std::string old = Doc["NODES"][a]["NODE_ID"].get<std::string>();
        QVERIFY(Canvas->connect(a, b));
        Doc["NODES"][b]["EXCLUDE"] = json::array({old});
        Doc["NODES"][b]["RUNNER"]  = old;
        QVERIFY(Layout.contains(old));

        QVERIFY(Canvas->renameNode(a, "renamed"));

        QCOMPARE(Doc["NODES"][b]["PARENTS"][0].get<std::string>(), std::string("renamed"));
        QCOMPARE(Doc["NODES"][b]["EXCLUDE"][0].get<std::string>(), std::string("renamed"));
        QCOMPARE(Doc["NODES"][b]["RUNNER"].get<std::string>(), std::string("renamed"));
        QVERIFY(!Layout.contains(old));                       // no dead key left behind
        QVERIFY(Layout.contains("renamed"));
        QCOMPARE(Layout["renamed"][0].get<double>(), 300.0);  // ...and the position came with it
    }

    // The link slot must address the REAL PARENTS index, including past entries the graph skips (a null or a
    // non-string left by a hand-edit). Detaching a wire erases PARENTS[slot], so a slot that drifted by one
    // would erase a DIFFERENT parent than the one the user pulled off.
    void linkSlotsAddressTheRealParentsIndex()
    {
        const int p1 = Canvas->addNode("Content");
        const int p2 = Canvas->addNode("Content");
        const int c  = Canvas->addNode("DeclareExec");
        const std::string a = Doc["NODES"][p1]["NODE_ID"].get<std::string>();
        const std::string b = Doc["NODES"][p2]["NODE_ID"].get<std::string>();
        // A junk entry BETWEEN two real ones: a running counter over G.Links would put b at slot 1, not 2.
        Doc["NODES"][c]["PARENTS"] = json::array({a, nullptr, b});
        Canvas->invalidateGraph();

        const PkgGraph::Graph g = Canvas->graph();
        QCOMPARE(g.Links.size(), size_t(2));                 // the null is skipped as an edge...
        for (const auto &L : g.Links)
        {
            QVERIFY(L.Slot >= 0 && L.Slot < (int)Doc["NODES"][c]["PARENTS"].size());
            const json &Ent = Doc["NODES"][c]["PARENTS"][L.Slot];   // ...but the slot still points AT it
            QVERIFY(Ent.is_string());
            const std::string want = (L.ParentIndex >= 0) ? g.Nodes[L.ParentIndex].Id : L.ExternalId;
            QCOMPARE(Ent.get<std::string>(), want);
        }
    }

    // Every TYPE renders through the same declared field table — a full frame over one of each must not throw
    // or crash, which is the cheap proof that no type is missing its editor.
    void everyTypeDrawsAFrame()
    {
        for (const std::string &t : PkgGraph::AllTypes()) Canvas->addNode(t);
        QCOMPARE(Canvas->nodeCount(), (int)PkgGraph::AllTypes().size());
        runFrame();
        runFrame();
        QVERIFY(Saves >= 1);                                   // edits were persisted through the save hook
    }

    // Every field the engine reads must be reachable from the editor. The MODE combo offered Cave and Poke
    // while the table had no PAYLOAD and no VALUE, so choosing either produced a node validation then rejected
    // with no way to fix it here — and the CAVE patches already in the library rendered as patches with no
    // body. TOGGLE/WHEN/EXCLUDE are node-level, so they are the envelope's job, not each type's.
    void everyEngineReadableFieldIsAuthorable()
    {
        auto keys = [](const std::string &type) {
            std::vector<std::string> out;
            for (const PkgGraph::Field &f : PkgGraph::FieldsFor(type))
            {
                out.push_back(f.Key);
                for (const PkgGraph::Field &s : f.Sub) out.push_back(s.Key);
            }
            return out;
        };
        auto has = [](const std::vector<std::string> &v, const char *k) {
            return std::find(v.begin(), v.end(), std::string(k)) != v.end();
        };

        const auto bp = keys("BinaryPatch");
        for (const char *k : {"FILE","MODE","OFFSET","ANCHOR","EXPECT","REPLACE","VALUE","PAYLOAD","CAVE","APPLY","WHEN"})
            QVERIFY2(has(bp, k), k);
        // ...and every MODE the combo offers has the field it needs, or the combo is lying.
        for (const PkgGraph::Field &f : PkgGraph::FieldsFor("BinaryPatch"))
            for (const PkgGraph::Field &s : f.Sub)
                if (std::string(s.Key) == "MODE")
                    for (const auto &o : s.Options)
                    {
                        const std::string m = o.first;
                        if (m == "Cave")    QVERIFY(has(bp, "PAYLOAD"));
                        if (m == "Poke")    QVERIFY(has(bp, "VALUE"));
                        if (m == "Replace") QVERIFY(has(bp, "REPLACE"));
                    }

        const auto ex = keys("DeclareExec");
        for (const char *k : {"HOST","GUEST","PATH","ARGS","ENV","ENV_REMOVE","CONTENT_ROOT",
                              "PREFIX_GENERATE","UNIFIED_RUNTIME","RUNNER"})
            QVERIFY2(has(ex, k), k);

        QVERIFY(has(keys("FileEdit"), "WHEN"));

        // The envelope: settable on ANY type, so it must not live in a per-type table.
        for (const std::string &t : PkgGraph::AllTypes())
            for (const char *k : {"TOGGLE", "EXCLUDE"})
                QVERIFY2(!has(keys(t), k), (t + "/" + k).c_str());
    }

    // TOGGLE has THREE states and the editor must render three. ABSENT = not user-toggleable; "on" =
    // toggleable, starts on; "off" = toggleable, starts off. An editor that draws absent and "on" as the same
    // thing — and writes "on" by ERASING the key — silently converts a user-switchable module into a
    // permanently-on one. Eight shipping nodes are exactly that shape (the Wipeout and NFSU2 toggles).
    //
    // LIMIT, stated because a test that looks like more than it is is worse than none: this pins the three
    // SEMANTIC states (what ParseNode makes of each) and that rendering preserves whichever one is set. It
    // does NOT drive the combo's popup — selecting an item needs a two-level click the input harness here
    // cannot target — so the mapping from menu entry to written value is covered by review, not by this.
    void toggleHasThreeDistinctStatesInTheEditor()
    {
        const int n = Canvas->addNode("Content");
        json &N = Doc["NODES"][n];
        QVERIFY(!N.contains("TOGGLE"));                      // a fresh node is not a toggle

        // Each state survives a render unchanged — in particular "on" is NOT erased back to absent.
        for (const char *V : {"on", "off"})
        {
            N["TOGGLE"] = V;
            N["WHEN"] = "%MODE% == host";
            N["EXCLUDE"] = json::array({"other"});
            Canvas->invalidateGraph();
            runFrame(); runFrame();
            QVERIFY2(N.contains("TOGGLE"), V);
            QCOMPARE(N["TOGGLE"].get<std::string>(), std::string(V));
            QCOMPARE(N["WHEN"].get<std::string>(), std::string("%MODE% == host"));
            QCOMPARE(N["EXCLUDE"].size(), size_t(1));
        }

        // ...and all three are what ParseNode distinguishes, so the editor's three map onto real semantics.
        auto parsed = [](const char *toggle) {
            json J{{"NODE_ID","x"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"}};
            if (toggle) J["TOGGLE"] = toggle;
            Node P; ManifestModel::ParseNode(J, "f.json", "/b", P);
            return std::make_pair(P.Optional, P.Default);
        };
        QCOMPARE(parsed(nullptr), std::make_pair(false, true));    // absent: not a toggle
        QCOMPARE(parsed("on"),    std::make_pair(true,  true));    // a toggle that starts on
        QCOMPARE(parsed("off"),   std::make_pair(true,  false));   // a toggle that starts off
    }

    // Clearing a list ERASES the key rather than writing []. They are not the same thing: an empty
    // BASE_TARGETS is a delta with no base and is refused outright, so clearing the box produced a node the
    // format rejects and the editor could not repair — the refusal says "omit it" and there was no way to.
    void clearingAListErasesTheKeyRatherThanWritingAnEmptyArray()
    {
        // First, why it matters: [] and absent are NOT the same node. An empty BASE_TARGETS is a delta with
        // no base and is refused outright, and the refusal says "omit it" — which the editor must be able to.
        {
            //FORM must be "delta": BASE_TARGETS on any other form is refused for a DIFFERENT reason ("it means
            //nothing on FORM zip"), which would make this assertion pass without testing the empty-array rule.
            json N{{"NODE_ID","c"}, {"TYPE","Content"}, {"FORM","delta"}, {"PATH","a.vgdelta"},
                   {"BASE_TARGETS", json::array()}};
            std::string Err;
            NodeLower::Lower(N, "c", Err);
            QVERIFY2(!Err.empty(), "an empty BASE_TARGETS must be refused");
            N.erase("BASE_TARGETS");
            NodeLower::Lower(N, "c", Err);
            QVERIFY(Err.empty());
        }

        // Now drive the REAL widget: find the list field by effect, type into it, then clear it.
        const int n = Canvas->addNode("Content");
        const std::string id = Doc["NODES"][n]["NODE_ID"].get<std::string>();
        //FORM must be "delta": the base-targets box is offered only there, because the key means nothing on any
        //other form and NodeLower refuses it outright.
        Doc["NODES"][n]["FORM"] = "delta";
        Doc["NODES"][n]["PATH"] = "d.vgdelta";
        auto reset = [&] {
            Doc["NODES"][n].erase("BASE_TARGETS");
            Layout[id] = json::array({30.0, 30.0});
            Canvas->invalidateGraph();
            runFrame(); runFrame();
        };

        ImVec2 box(-1, -1);
        for (float y = 30.0f; y < 700.0f && box.x < 0; y += 4.0f)
            for (float x = 40.0f; x < 420.0f; x += 10.0f)
            {
                reset();
                clickAt(ImVec2(x, y));
                if (!ImGui::IsAnyItemActive()) continue;
                type("Q");
                clickAt(ImVec2(1100, 850));
                runFrame();
                const json &N = Doc["NODES"][n];
                if (N.contains("BASE_TARGETS") && N["BASE_TARGETS"].is_array()
                    && N["BASE_TARGETS"].size() == 1
                    && N["BASE_TARGETS"][0].get<std::string>() == "Q") { box = ImVec2(x, y); break; }
            }
        QVERIFY2(box.x >= 0, "could not find the base-target list field");

        // Typed a character, then removed it — the key must be GONE, not [].
        reset();
        clickAt(box);
        type("Q");
        runFrame();
        QVERIFY(Doc["NODES"][n].contains("BASE_TARGETS"));
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
        runFrame(LastMouse);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
        runFrame(LastMouse);
        clickAt(ImVec2(1100, 850));
        runFrame();
        QVERIFY2(!Doc["NODES"][n].contains("BASE_TARGETS"),
                 "an emptied list wrote [] - which the format refuses and the editor cannot undo");
    }

    // "" is a REAL base target — the mount root — and it is the shape the whole one-key design exists to make
    // expressible. The generic list writer trimmed blank lines as typing noise, so this entry could not be
    // typed AND, far worse, vanished from any node that already had one the moment the box was touched:
    // ["", "dxvk"] silently became ["dxvk"], turning a two-base delta into a one-base one — a base of the
    // wrong SIZE, a reconstruction that fails its size check, and a layer the mount skips without a word.
    void anEmptyBaseTargetEntryIsDataAndSurvivesEditing()
    {
        const int n = Canvas->addNode("Content");
        const std::string id = Doc["NODES"][n]["NODE_ID"].get<std::string>();
        Doc["NODES"][n]["FORM"] = "delta";
        Doc["NODES"][n]["PATH"] = "d.vgdelta";
        Layout[id] = json::array({30.0, 30.0});

        auto seed = [&](const json &V) {
            if (V.is_null()) Doc["NODES"][n].erase("BASE_TARGETS"); else Doc["NODES"][n]["BASE_TARGETS"] = V;
            Canvas->invalidateGraph();
            runFrame(); runFrame();
        };
        auto key = [&](ImGuiKey K, int Times = 1) {
            for (int i = 0; i < Times; ++i) {
                ImGui::GetIO().AddKeyEvent(K, true);  runFrame(LastMouse);
                ImGui::GetIO().AddKeyEvent(K, false); runFrame(LastMouse);
            }
        };

        // Find the base-targets box BY EFFECT — an edit that lands in BASE_TARGETS and nowhere else. Sweeping
        // for "some active widget" is not enough: any other focused field leaves BASE_TARGETS untouched, which
        // reads as success and makes the whole test vacuous.
        ImVec2 box(-1, -1);
        for (float y = 30.0f; y < 700.0f && box.x < 0; y += 4.0f)
            for (float x = 40.0f; x < 420.0f; x += 10.0f)
            {
                seed(json(nullptr));
                clickAt(ImVec2(x, y));
                if (!ImGui::IsAnyItemActive()) continue;
                type("Q");
                clickAt(ImVec2(1100, 850));
                runFrame();
                const json &N = Doc["NODES"][n];
                if (N.contains("BASE_TARGETS") && N["BASE_TARGETS"].is_array() && N["BASE_TARGETS"].size() == 1
                    && N["BASE_TARGETS"][0].get<std::string>() == "Q") { box = ImVec2(x, y); break; }
            }
        QVERIFY2(box.x >= 0, "could not find the base-targets list field");

        // Merely PAINTING a node that already has a root base must not rewrite it.
        seed(json::array({ "", "dxvk" }));
        QCOMPARE(Doc["NODES"][n]["BASE_TARGETS"].size(), size_t(2));
        QCOMPARE(Doc["NODES"][n]["BASE_TARGETS"][0].get<std::string>(), std::string(""));

        // ...and neither must EDITING it. Append at the very end (down past the last line, then End) so the
        // blank first line is untouched by the edit itself — the only thing that can remove it is the writer.
        clickAt(box);
        QVERIFY2(ImGui::IsAnyItemActive(), "the box must activate on click");
        key(ImGuiKey_DownArrow, 4);
        key(ImGuiKey_End);
        type("Z");
        clickAt(ImVec2(1100, 850));
        runFrame();

        const json &B = Doc["NODES"][n]["BASE_TARGETS"];
        QVERIFY2(B.is_array() && B.size() == 2, "the empty (root) entry was silently dropped by the list writer");
        QCOMPARE(B[0].get<std::string>(), std::string(""));
        QCOMPARE(B[1].get<std::string>(), std::string("dxvkZ"));

        // ...and the TRAILING position too, which is the shape a prefix delta over the runner-mount root has
        // (["dxvk", ""]). It renders as "dxvk\n", whose final blank line is an ENTRY, not a separator.
        seed(json::array({ "dxvk", "" }));
        clickAt(box);
        QVERIFY(ImGui::IsAnyItemActive());
        key(ImGuiKey_UpArrow, 4);
        key(ImGuiKey_Home);
        type("Z");
        clickAt(ImVec2(1100, 850));
        runFrame();
        const json &B2 = Doc["NODES"][n]["BASE_TARGETS"];
        QVERIFY2(B2.is_array() && B2.size() == 2, "a TRAILING empty entry is an entry, not a separator");
        QCOMPARE(B2[0].get<std::string>(), std::string("Zdxvk"));
        QCOMPARE(B2[1].get<std::string>(), std::string(""));

        // ...and the root base must be TYPEABLE, not merely preserved: the field's hint says "a blank line is
        // the mount root", which a single-line input cannot accept at all — Enter commits instead of inserting.
        seed(json(nullptr));
        clickAt(box);
        QVERIFY(ImGui::IsAnyItemActive());
        type("wine");
        key(ImGuiKey_Enter);
        clickAt(ImVec2(1100, 850));
        runFrame();
        const json &B3 = Doc["NODES"][n]["BASE_TARGETS"];
        QVERIFY2(B3.is_array() && B3.size() == 2,
                 "a blank line must be typeable, or the field cannot express the root base at all");
        QCOMPARE(B3[0].get<std::string>(), std::string("wine"));
        QCOMPARE(B3[1].get<std::string>(), std::string(""));
    }

    // THE editor must be able to DISPLAY a node the format refuses — it is the tool you open to fix one, and
    // NodeLower has diagnostics that say so ("`\"WHEN\": true` is a plausible slip, the field reads like a
    // boolean"). Every reader here is therefore total: nlohmann's value() throws on a type mismatch, and a
    // throw out of frame() escapes with ImGui scopes still open, so the NEXT paint segfaults on the half-open
    // state. Not a crash in a corner — a crash in the repair tool, on the node you opened it to repair.
    void malformedNodesRenderInsteadOfThrowing()
    {
        Doc["NODES"] = json::array({
            json{{"NODE_ID","a"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"}, {"WHEN", true}},
            json{{"NODE_ID","b"}, {"TYPE","RegEdit"}, {"EDITS", json::array({
                json{{"OVERRIDE","true"}, {"ARCHITECTURE","32"}, {"HKCU", {{"S", {{"v","1"}}}}}}})}},
            json{{"NODE_ID","c"}, {"TYPE","DeclareLibraryItem"}, {"UID","1"}, {"COVER","cover.png"}},
            json{{"NODE_ID","d"}, {"TYPE", 5}},
            json{{"NODE_ID","e"}, {"TYPE","Content"}, {"FORM", 7}, {"PATH", 9}},
            json{{"NODE_ID","f"}, {"TYPE","DeclareExec"}, {"HOST","win32"}, {"RECOMMENDED","yes"}},
        });
        Canvas->invalidateGraph();
        runFrame();
        runFrame();                                   // the frame AFTER is where a half-open state lands
        QCOMPARE(Canvas->nodeCount(), 6);

        // ...and a string-form COVER is a legal shape the widget must EDIT, not just survive: it used to read
        // as empty (so the box looked unset and inviting) and the first keystroke wrote ["PATH"] into a string.
        const PkgGraph::Graph g = Canvas->graph();
        QCOMPARE(g.Nodes.size(), size_t(6));
        QVERIFY(Doc["NODES"][2]["COVER"].is_string());
    }

    // COVER is DUAL-FORM — a bare filename or a {PATH, SOURCE} object — and the widget must read AND write
    // both. It read only the object form, so a string cover rendered as an empty box (inviting a keystroke)
    // and that keystroke wrote ["PATH"] into a string, which throws. Editing must also PRESERVE the form:
    // promoting a string to an object changes the package's bytes for a cosmetic edit.
    void coverEditingHandlesBothFormsAndPreservesWhichever()
    {
        const int n = Canvas->addNode("DeclareLibraryItem");
        const std::string id = Doc["NODES"][n]["NODE_ID"].get<std::string>();
        Doc["NODES"][n]["COVER"] = "cover.png";
        Layout[id] = json::array({30.0, 30.0});
        Canvas->invalidateGraph();
        runFrame(); runFrame();

        // The string form must be VISIBLE, not read as empty: find the box by effect and check what it holds.
        ImVec2 box(-1, -1);
        for (float y = 30.0f; y < 700.0f && box.x < 0; y += 4.0f)
            for (float x = 40.0f; x < 420.0f; x += 10.0f)
            {
                Doc["NODES"][n]["COVER"] = "cover.png";
                Canvas->invalidateGraph(); runFrame(); runFrame();
                clickAt(ImVec2(x, y));
                if (!ImGui::IsAnyItemActive()) continue;
                type("Z");
                clickAt(ImVec2(1100, 850));
                runFrame();
                const json &C = Doc["NODES"][n]["COVER"];
                if (C.is_string() && C.get<std::string>().find("cover.png") != std::string::npos
                    && C.get<std::string>() != "cover.png") { box = ImVec2(x, y); break; }
            }
        QVERIFY2(box.x >= 0, "the string-form cover was not editable (it read as empty)");
        QVERIFY2(Doc["NODES"][n]["COVER"].is_string(), "editing promoted a string COVER to an object");

        // ...and the object form stays an object.
        Doc["NODES"][n]["COVER"] = json{{"PATH", "cover.png"}, {"SOURCE", json{{"CID", "Qm1"}}}};
        Canvas->invalidateGraph(); runFrame(); runFrame();
        clickAt(box); type("Z"); clickAt(ImVec2(1100, 850)); runFrame();
        QVERIFY(Doc["NODES"][n]["COVER"].is_object());
        QVERIFY(Doc["NODES"][n]["COVER"].contains("SOURCE"));      // the CID was not dropped
    }

    // A stale selection must not grow the document. nlohmann's non-const operator[](size_type) FILLS the
    // array with nulls up to the index, so a Delete keypress against an index left over from a previous
    // document did not merely read garbage — it appended nulls that SaveNodes would write to disk.
    void aStaleSelectionCannotGrowTheDocument()
    {
        Canvas->addNode("Content");
        Canvas->addNode("Content");
        Canvas->selectNode(1);
        Doc["NODES"] = json::array({ json{{"NODE_ID","only"}, {"TYPE","Group"}} });   // document swapped
        Canvas->invalidateGraph();

        // PRESS Delete with the stale index still set. Without this the test was vacuous: it never exercised
        // the read it exists to guard, and its own selectNode(7) reset Selected to -1, erasing the setup.
        auto pressDelete = [&] {
            ImGuiIO &io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiKey_Delete, true);
            runFrame();
            io.AddKeyEvent(ImGuiKey_Delete, false);
            runFrame();
        };
        pressDelete();
        QCOMPARE((int)Doc["NODES"].size(), 1);
        for (const auto &N : Doc["NODES"]) QVERIFY2(!N.is_null(), "the document grew nulls");

        Canvas->selectNode(7);                        // out of range: refused outright
        pressDelete();
        QCOMPARE((int)Doc["NODES"].size(), 1);
        for (const auto &N : Doc["NODES"]) QVERIFY(!N.is_null());
    }

    // Merely LOOKING at a package must not change it. The field renderers used to materialise their container
    // ("if (!Node.contains(F.Key)) Node[F.Key] = json::object();") before drawing it, so opening a bundle
    // stamped empty ENV/META/COVER/EDITS onto every node — which changes the file's bytes, and therefore its
    // CID, for every peer, from nothing but a render pass.
    void renderingDoesNotMutateTheDocument()
    {
        for (const std::string &t : PkgGraph::AllTypes()) Canvas->addNode(t);
        const std::string Before = Doc.dump();                 // layout lives in the sidecar, not here
        runFrame();
        runFrame();
        QCOMPARE(Doc.dump(), Before);
    }

    // The two halves of the same registry clash: a value and a subkey cannot share a name. Rows are flattened
    // values-first, so the value is written before the subkey path is walked — and the walk used to overwrite
    // it with an object, dropping it with no diagnostic. Neither side may clobber the other.
    void registryValueAndSubkeyCollisionsKeepWhatIsThere()
    {
        json a = json::object();
        PkgGraph::RegRowsInto(a, {{"HKLM\\Soft", "Thing", "i-am-a-value"},
                                  {"HKLM\\Soft\\Thing", "Inner", "i-am-a-subkey"}});
        QVERIFY(a["HKLM"]["Soft"]["Thing"].is_string());                   // first writer kept
        QCOMPARE(a["HKLM"]["Soft"]["Thing"].get<std::string>(), std::string("i-am-a-value"));

        json b = json::object();                                           // and the mirror order
        PkgGraph::RegRowsInto(b, {{"HKLM\\Soft\\Thing", "Inner", "i-am-a-subkey"},
                                  {"HKLM\\Soft", "Thing", "i-am-a-value"}});
        QVERIFY(b["HKLM"]["Soft"]["Thing"].is_object());
        QCOMPARE(b["HKLM"]["Soft"]["Thing"]["Inner"].get<std::string>(), std::string("i-am-a-subkey"));
    }

    // Typing in a registry row used to commit on EVERY keystroke. The rows are a FLATTENING of the hive tree,
    // and flattening emits a key's VALUES before its SUBKEYS - so the moment a half-typed path turns a row into
    // a sibling value of an earlier subkey row, the two SWAP. The cursor stays in row slot 1, which now holds a
    // different row, and every character after that lands in the wrong place. This drives real clicks and real
    // keystrokes through ImGui to prove the row you are typing in is the row that changes.
    void registryRowsDoNotMoveUnderTheCursor()
    {
        const int n = Canvas->addNode("RegEdit");
        // Flattens to exactly two rows, subkey first: [ HKLM\Soft\Sub -> X , HKLM\So -> Y ].
        auto reset = [&] {
            Doc["NODES"][n]["EDITS"] = json::array({ json{{"ARCHITECTURE", json::array({"32"})},
                {"HKLM", json{{"Soft", json{{"Sub", json{{"X", "1"}}}}},
                              {"So",   json{{"Y", "2"}}}}}} });
            Layout[Doc["NODES"][n]["NODE_ID"].get<std::string>()] = json::array({30.0, 30.0});
            Canvas->invalidateGraph();
            runFrame(); runFrame();
        };
        auto pathOf = [&](const char *name) -> std::string {
            for (const auto &r : PkgGraph::RegRowsOf(Doc["NODES"][n]["EDITS"][0])) if (r.Name == name) return r.Path;
            return "<missing>";
        };
        reset();
        QCOMPARE(pathOf("X"), std::string("HKLM\\Soft\\Sub"));
        QCOMPARE(pathOf("Y"), std::string("HKLM\\So"));

        // Locate the SECOND row's PATH box by EFFECT rather than by hardcoded pixels: click, append, commit,
        // and see whether Y's key path grew a 'Q'. Layout changes cannot silently retarget this test.
        ImVec2 box(-1, -1);
        for (float y = 30.0f; y < 700.0f && box.x < 0; y += 4.0f)
            for (float x = 40.0f; x < 420.0f; x += 10.0f)
            {
                reset();
                clickAt(ImVec2(x, y));
                if (!ImGui::IsAnyItemActive()) continue;
                type("Q");
                clickAt(ImVec2(1100, 850));                        // finish the edit
                runFrame();
                if (pathOf("Y") == "HKLM\\SoQ") { box = ImVec2(x, y); break; }
            }
        QVERIFY2(box.x >= 0, "could not find the second registry row's PATH box");

        reset();
        clickAt(box);
        QVERIFY(ImGui::IsAnyItemActive());
        type("ftZ");   // "HKLM\So" -> "...Soft" (the swap point) -> "...SoftZ", one character per frame
        clickAt(ImVec2(1100, 850));
        runFrame();

        // Every character went into the row the cursor was in. With the per-keystroke rebuild, the 'Z' landed
        // on X instead: X became "HKLM\SoftZ" and Y stopped at "HKLM\Soft".
        QCOMPARE(pathOf("Y"), std::string("HKLM\\SoftZ"));
        QCOMPARE(pathOf("X"), std::string("HKLM\\Soft\\Sub"));
    }

    // ---- zoom + viewport culling -----------------------------------------------------------------------
    // Zoom is a VIEW property built on top of imnodes (which has none of its own): positions are pushed to
    // imnodes pre-scaled and divided back out on read. The bug that buys is obvious and expensive — a drag
    // read back at 0.5x without dividing would halve the whole layout and then PERSIST it.


    // THE round trip: a node drawn, then culled, then brought back. imnodes DESTROYS any node not submitted
    // during a frame (ObjectPoolUpdate) and re-creates it at Origin(0,0) when it next appears — so anything
    // that decides "already seeded, no need to re-place it" hands the read-back a (0,0) that is then written
    // to the layout and, at publish, stamped into POS. Panning away and back must not move a single node.
    void aNodeThatScrollsOutAndBackKeepsItsPosition()
    {
        //Culling is unconditional now; the minimap is off here only so the overview cannot be what a
        //measurement picks up.
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 300, 200);
        Canvas->addNode("Content", 90000, 90000);   // far away: something to scroll to
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 1);

        // Scroll the first node off-screen by panning to the far node, then come back.
        ImNodes::EditorContextResetPanning(ImVec2(-89000, -89000));
        runFrame();
        runFrame();
        ImNodes::EditorContextResetPanning(ImVec2(0, 0));
        runFrame();
        runFrame();

        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[0].X, 300.0f);
        QCOMPARE(G.Nodes[0].Y, 200.0f);
    }


    // A drag must reach the CACHED graph, not just the stored layout — and the observable consequence is
    // CULLING, which tests the cached coordinate. With a stale cache a node dragged far off-screen stays
    // "visible" forever (and, symmetrically, one dragged into view never appears). Asserting through
    // Canvas->graph() cannot see this: that rebuilds from the layout, which SetPos has already updated.
    void aDraggedPositionReachesTheCacheThatCullingReads()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        Canvas->addNode("Content", 300, 100);
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 2);

        ImNodes::SetNodeGridSpacePos(0, ImVec2(90000, 90000));   // as a drag would leave it
        runFrame();                                              // read-back sees it and updates the cache
        runFrame();                                              // ...so this frame culls on the NEW position
        QCOMPARE(Canvas->visibleNodes(), 1);
    }

    // A SELECTED node is never culled: imnodes keeps a freed node's index in its own selection set with no
    // liveness check, so a culled-then-reused slot makes TranslateSelectedNodes drag a node the user never
    // selected — and persist it.
    void aSelectedNodeIsNeverCulled()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 40, 40);
        Canvas->addNode("Content", 600, 300);
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 2);

        // Select it while it is still on screen — imnodes cannot select a node it has already destroyed —
        // and only then move it far away.
        ImNodes::ClearNodeSelection();
        ImNodes::SelectNode(1);
        runFrame();
        ImNodes::SetNodeGridSpacePos(1, ImVec2(90000, 90000));
        runFrame();
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 2);   // off-screen, but selected, so still submitted
    }




    // The save SPLIT. A pure drag must reach the layout-only hook (positions live in no node file, so the full
    // save rewrites the whole bundle for nothing), and a real document edit must still reach the full one.
    // Misclassifying the second way is silent loss of a node edit, so both directions are pinned.
    void aPureDragSavesOnlyTheLayout()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        runFrame();
        FullSaves = 0; LayoutSaves = 0;

        ImNodes::SetNodeGridSpacePos(0, ImVec2(1500, 900));
        runFrame();                       // read-back notices the move
        releaseMouse();                   // the save fires on mouse-up
        QCOMPARE(LayoutSaves, 1);
        QCOMPARE(FullSaves, 0);           // no node file was touched
    }

    void aDocumentEditStillSavesEverything()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        runFrame();
        FullSaves = 0; LayoutSaves = 0;

        Canvas->renameNode(0, "renamed_node");   // a real document change
        runFrame();
        releaseMouse();
        QCOMPARE(FullSaves, 1);
        QCOMPARE(LayoutSaves, 0);
    }


    // ...and when both a document edit and a drag are outstanding, the full save must win — choosing the
    // layout-only hook there would drop the node edit while the canvas still showed it.
    //
    // HONEST NOTE ON ITS STRENGTH: this one pins INTENT, not a reachable bug. Mutating `PosOnly` to ignore
    // DocChanged does not change the outcome, because a document edit commits on the FOLLOWING frame through
    // the `!IsAnyItemActive()` arm rather than waiting for a release — so the two flags never actually meet at
    // a commit. The `!DocChanged` term is defensive against that arm changing, and this test is what would
    // notice if it did.
    void aDocumentEditCombinedWithADragStillSavesTheDocument()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        runFrame();
        FullSaves = 0; LayoutSaves = 0;

        // Drag FIRST and let a frame observe it, so PosDirty is genuinely latched...
        ImNodes::SetNodeGridSpacePos(0, ImVec2(1500, 900));
        runFrame();
        // ...then make a document edit before the button comes up, so both flags are live at the commit.
        // (Editing first would not do, for the reason in the note above rather than the seeding one this
        // comment used to give: a document edit commits on the FOLLOWING frame, so the frame the rename needs
        // clears DocChanged before the drag is released, and the release then takes the layout-only path.
        // Measured: LayoutSaves 1, not 0. renameNode does not drop the seed either — it MOVES the entry to
        // the new id, which is the whole point of the Move() block in it.)
        Canvas->renameNode(0, "edited_and_moved");
        releaseMouse();

        QCOMPARE(FullSaves, 1);       // the node edit reached disk
        QCOMPARE(LayoutSaves, 0);     // ...and was not swallowed by the positions-only path
    }




    // Zooming must not move a single stored coordinate — and the values that threaten that are the ones a
    // WHEEL produces (1.1^n), not the clean 0.5/2.0 an earlier test used, which are exactly representable in
    // binary and so round-trip for free. Positions are pushed to imnodes multiplied by zoom and read back
    // divided by it; if that round trip drifts, the drift is written to the layout and, at publish, stamped
    // into the package's POS — a CID change caused by looking at the graph.
    void wheelSizedZoomStepsDoNotDriftAnyPosition()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 137, 449);          // deliberately not round numbers
        Canvas->addNode("DeclareExec", 911, 1303);
        runFrame();
        const std::string Before = Layout.dump();
        const int SavesBefore = Saves;

        // Ten notches up, then ten back down — the exact sequence the wheel handler produces.
        float z = Canvas->zoom();
        for (int i = 0; i < 10; ++i) { z *= 1.1f; Canvas->setZoom(z); runFrame(); runFrame(); }
        for (int i = 0; i < 10; ++i) { z /= 1.1f; Canvas->setZoom(z); runFrame(); runFrame(); }
        Canvas->setZoom(1.0f);
        runFrame(); runFrame();

        QCOMPARE(Layout.dump(), Before);               // not one coordinate written
        QCOMPARE(Saves, SavesBefore);                  // and nothing marked dirty, so nothing to persist
        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[0].X, 137.0f);
        QCOMPARE(G.Nodes[0].Y, 449.0f);
        QCOMPARE(G.Nodes[1].X, 911.0f);
        QCOMPARE(G.Nodes[1].Y, 1303.0f);
    }


    // THE ACTUAL GESTURE, and worth keeping even though the test above drives the same numbers through
    // setZoom(). The comment here used to say setZoom() clears the seed map and the wheel path does not; it
    // no longer does (see setZoom — clearing it stomped in-flight drags and bought nothing once zoom became a
    // view transform, because imnodes holds WORLD coordinates and a scale change does not invalidate them).
    // What this still covers that setZoom does not is the handler itself: the wheel assigns through the
    // easing fields, so a regression that reintroduces a scale into the pushed position, or that recentres
    // against the wrong "from" zoom, reaches the layout here and not there.
    void wheelingOverTheCanvasDoesNotMoveAnyPosition()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 137, 449);
        Canvas->addNode("DeclareExec", 911, 1303);
        runFrame();
        const std::string Before = Layout.dump();
        const int SavesBefore = Saves;

        auto wheel = [&](float notches) {
            ImGuiIO &io = ImGui::GetIO();
            io.AddMouseWheelEvent(0.0f, notches);
            runFrame();      // the handler runs after EndNodeEditor
            runFrame();      // the next frame re-seeds and reads back
        };
        for (int i = 0; i < 6; ++i) wheel(+1.0f);
        for (int i = 0; i < 6; ++i) wheel(-1.0f);

        QCOMPARE(Layout.dump(), Before);
        QCOMPARE(Saves, SavesBefore);
        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[0].X, 137.0f);
        QCOMPARE(G.Nodes[0].Y, 449.0f);
        QCOMPARE(G.Nodes[1].X, 911.0f);
        QCOMPARE(G.Nodes[1].Y, 1303.0f);
    }


    // Zoom is a VIEW TRANSFORM over the emitted geometry: imnodes is handed world coordinates and an unscaled
    // style, and the vertices it produces are scaled about the canvas origin afterwards. So the thing to
    // assert is the GEOMETRY, not imnodes' own reported sizes — those stay unscaled on purpose, which is
    // exactly why the document can no longer be touched by looking at it.
    // SetVarVisible IS the visibility contract (UI-facet presence). A hidden var carries no UI and resolves from
    // DEFAULT; making it visible adds a minimal UI (control+label) without clobbering an existing facet. Teeth:
    // make "hidden" keep UI, or "visible" overwrite an existing UI — each flips an assertion here.
    void setVarVisibleTogglesTheUiFacet()
    {
        nlohmann::ordered_json V{{"TYPE", "CustomVar"}, {"KEY", "tt_width"}, {"DEFAULT", "%ScreenWidth%"}};
        QVERIFY(!V.contains("UI"));
        PkgGraph::SetVarVisible(V, true);
        QVERIFY(V.contains("UI") && V["UI"].is_object());
        QCOMPARE(V["UI"].value("LABEL", std::string()), std::string("tt_width"));   // defaults label to the key
        QCOMPARE(V["UI"].value("CONTROL", std::string()), std::string("text"));
        V["UI"]["LABEL"] = "Width"; V["UI"]["CONTROL"] = "int";                      // author customised it
        PkgGraph::SetVarVisible(V, true);                                           // idempotent: must NOT clobber
        QCOMPARE(V["UI"].value("LABEL", std::string()), std::string("Width"));
        QCOMPARE(V["UI"].value("CONTROL", std::string()), std::string("int"));
        PkgGraph::SetVarVisible(V, false);                                          // hide → UI gone, DEFAULT kept
        QVERIFY(!V.contains("UI"));
        QCOMPARE(V.value("DEFAULT", std::string()), std::string("%ScreenWidth%"));
    }

    void zoomScalesTheEmittedGeometry()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        Canvas->addNode("DeclareExec", 700, 400);

        // The SURFACE's own bounds, not the whole draw data: the canvas window's background spans the display
        // at every zoom, so a bbox over everything is pinned to the window width and cannot shrink.
        auto surfaceWidth = [&]() {
            runFrame();
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            Canvas->surfaceBounds(x0, y0, x1, y1);
            return x1 - x0;
        };

        const float At1 = surfaceWidth();
        Canvas->setZoom(2.0f);
        const float At2 = surfaceWidth();
        Canvas->setZoom(0.5f);
        const float AtHalf = surfaceWidth();

        QVERIFY2(At2 > At1 * 1.3f,
                 qPrintable(QString("geometry did not grow with zoom: %1 -> %2").arg(At1).arg(At2)));
        QVERIFY2(AtHalf < At1,
                 qPrintable(QString("geometry did not shrink when zoomed out: %1 -> %2").arg(At1).arg(AtHalf)));
        Canvas->setZoom(1.0f);
    }


    // Input must be inverse-transformed on the way INTO the editor. The view scales the geometry, so without
    // this a click at 2x lands where the node would have been drawn at 1x — you click a node and select its
    // neighbour, or nothing. Silent, and the single biggest risk of doing zoom as a surface transform.
    void theCursorIsInverseTransformedForHitTesting()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        runFrame(ImVec2(900, 600));
        float ex = 0, ey = 0;
        Canvas->editorMouse(ex, ey);
        // At 1:1 the editor sees the cursor exactly where it is.
        QCOMPARE(ex, 900.0f);
        QCOMPARE(ey, 600.0f);

        Canvas->setZoom(2.0f);
        runFrame(ImVec2(900, 600));
        Canvas->editorMouse(ex, ey);
        // Zoomed in, the same screen pixel is a point HALF as far from the canvas origin in world space, so
        // the editor must be handed a cursor pulled back toward that origin — never the raw screen position.
        QVERIFY2(ex < 900.0f && ey < 600.0f,
                 qPrintable(QString("cursor not inverse-transformed at 2x: (%1,%2)").arg(ex).arg(ey)));

        Canvas->setZoom(0.5f);
        runFrame(ImVec2(900, 600));
        Canvas->editorMouse(ex, ey);
        QVERIFY2(ex > 900.0f && ey > 600.0f,
                 qPrintable(QString("cursor not inverse-transformed at 0.5x: (%1,%2)").arg(ex).arg(ey)));
        Canvas->setZoom(1.0f);
    }


    // THE OUTLIER SWEEP. With zoom as a pure view transform, EVERYTHING imnodes reports must be
    // zoom-invariant: it is handed world coordinates, a constant style and constant widths, so a node's grid
    // position and its dimensions cannot depend on the zoom. Anything that does move is an element being
    // scaled somewhere it should not be — which is precisely how node widths ended up scaling twice and
    // growing with the SQUARE of the zoom.
    //
    // So: measure every node at every zoom, and name whatever drifts.
    void noElementGeometryDependsOnTheZoom()
    {
        Canvas->setMiniMap(false);
        const char *kinds[] = {"Content", "DeclareExec", "RegEdit", "FileEdit", "CustomVar", "DeclarePersist"};
        for (int i = 0; i < 6; ++i) Canvas->addNode(kinds[i], 500.0f + i * 60.0f, 300.0f + i * 40.0f);
        runFrame();

        struct Geo { ImVec2 pos, dim; };
        auto sample = [&]() {
            runFrame(); runFrame();
            //Every node must have been SUBMITTED, or what follows measures the title-bar-only box imnodes
            //re-creates for a culled one — a uniform, implausible number that reads as "everything resized".
            //setZoom recentres the view now, so a test that pins geometry across zooms has to keep its nodes
            //on screen rather than assume the pan never moves.
            [&]{ QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(),
                          qPrintable(QString("%1 of %2 nodes submitted - a culled node cannot be measured")
                                         .arg(Canvas->visibleNodes()).arg(Canvas->nodeCount()))); }();
            std::vector<Geo> g;
            for (int i = 0; i < Canvas->nodeCount(); ++i)
                g.push_back({ImNodes::GetNodeGridSpacePos(i), ImNodes::GetNodeDimensions(i)});
            return g;
        };
        const std::vector<Geo> Ref = sample();

        QStringList outliers;
        for (float z : {0.4f, 0.75f, 1.5f, 2.0f, 3.0f}) {
            Canvas->setZoom(z);
            const std::vector<Geo> Now = sample();
            QCOMPARE(Now.size(), Ref.size());
            for (size_t i = 0; i < Now.size(); ++i) {
                const float dx = std::abs(Now[i].pos.x - Ref[i].pos.x), dy = std::abs(Now[i].pos.y - Ref[i].pos.y);
                const float dw = std::abs(Now[i].dim.x - Ref[i].dim.x), dh = std::abs(Now[i].dim.y - Ref[i].dim.y);
                //1.5px, not 0.5: imgui rounds some item heights against the window's own scroll origin, which
                //moves when setZoom recentres, so an identical node measures 0.75px different. What this is
                //for is a node whose SIZE follows the zoom, which is a factor, not a rounding — the mutation
                //that put kNodeWidth * Zoom back reports 346 against 340.
                if (dx > 1.5f || dy > 1.5f)
                    outliers << QString("node %1 (%2) MOVED at zoom %3: d=(%4,%5)")
                                   .arg(i).arg(kinds[i % 6]).arg(z).arg(dx).arg(dy);
                if (dw > 1.5f || dh > 1.5f)
                    outliers << QString("node %1 (%2) RESIZED at zoom %3: %4x%5 -> %6x%7")
                                   .arg(i).arg(kinds[i % 6]).arg(z)
                                   .arg(Ref[i].dim.x).arg(Ref[i].dim.y).arg(Now[i].dim.x).arg(Now[i].dim.y);
            }
        }
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // The user's three zoom complaints, each turned into a measurement of the screen furniture that must NOT
    // move: "the minimap position goes crazy with the zoom", "the window where you can pan also scales with
    // the zoom leading to non working controls if you zoom out", and the node elongation (above).
    //
    // Everything here is screen space. Zoom is a transform over the CONTENT, so a rectangle that describes the
    // VIEWPORT — where the canvas is on screen, where its overview sits, how far its clip reaches — must be
    // byte-identical at 0.2x and at 3x. Any difference IS the bug.
    void noScreenFurnitureMovesWithTheZoom()
    {
        Canvas->setMiniMap(true);
        for (int i = 0; i < 6; ++i) Canvas->addNode("Content", 100.0f + i * 400.0f, 80.0f + i * 250.0f);
        runFrame(); runFrame();

        auto rects = [&]() {
            runFrame(); runFrame();
            ImVec4 Mm, Clip, View;
            Canvas->miniMapRect(Mm.x, Mm.y, Mm.z, Mm.w);
            Canvas->surfaceClip(Clip.x, Clip.y, Clip.z, Clip.w);
            Canvas->canvasViewport(View.x, View.y, View.z, View.w);
            return std::array<ImVec4, 3>{Mm, Clip, View};
        };
        const std::array<ImVec4, 3> Ref = rects();
        const char *Names[3] = {"minimap", "surface clip", "viewport"};
        // The minimap must actually be there, or this test passes by measuring nothing.
        QVERIFY2(Ref[0].z - Ref[0].x > 50.0f, "the minimap was not drawn - nothing to measure");
        // And the clip the canvas draws through must BE the viewport at 1:1, so a later zoom has something
        // meaningful to differ from.
        QVERIFY2(std::abs(Ref[1].z - Ref[2].z) < 1.5f && std::abs(Ref[1].x - Ref[2].x) < 1.5f,
                 qPrintable(QString("at 1:1 the canvas clip is not the viewport: [%1,%2] vs [%3,%4]")
                                .arg(Ref[1].x).arg(Ref[1].z).arg(Ref[2].x).arg(Ref[2].z)));

        QStringList outliers;
        for (float z : {0.2f, 0.4f, 0.75f, 1.5f, 2.0f, 3.0f}) {
            Canvas->setZoom(z);
            const std::array<ImVec4, 3> Now = rects();
            for (int r = 0; r < 3; ++r) {
                const float d = std::max(std::max(std::abs(Now[r].x - Ref[r].x), std::abs(Now[r].y - Ref[r].y)),
                                         std::max(std::abs(Now[r].z - Ref[r].z), std::abs(Now[r].w - Ref[r].w)));
                if (d > 1.5f)
                    outliers << QString("%1 MOVED at zoom %2 by %3px: [%4,%5,%6,%7] -> [%8,%9,%10,%11]")
                                   .arg(Names[r]).arg(z).arg(d)
                                   .arg(Ref[r].x).arg(Ref[r].y).arg(Ref[r].z).arg(Ref[r].w)
                                   .arg(Now[r].x).arg(Now[r].y).arg(Now[r].z).arg(Now[r].w);
            }
        }
        Canvas->setZoom(1.0f);
        Canvas->setMiniMap(false);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // Found on the LIVE canvas at 47%, and invisible to every geometry assertion before it: nodes to the right
    // of the viewport rendered as empty boxes with a title bar and nothing inside. imgui culls a widget against
    // the clip rect at SUBMISSION time, and submission happens in unscaled coordinates — so zoomed out, the
    // fields of a node whose unscaled position is off-screen are thrown away, and the transform then scales the
    // hollow box into view. The box is still exactly the right size, which is why sizes and bounds all agreed.
    //
    // What it costs is CONTENT, so content is what this counts: zooming out brings more of the graph on screen,
    // so it must emit MORE vertices, never fewer.
    void zoomingOutStillDrawsWhatIsInsideTheNodes()
    {
        Canvas->setMiniMap(false);
        // Spread well past the viewport (1400x900) horizontally and vertically, so at 1:1 most of it is off
        // screen and at 0.3 all of it is on.
        for (int i = 0; i < 12; ++i) Canvas->addNode("Content", 60.0f + i * 520.0f, 40.0f + i * 240.0f);
        auto verticesAt = [&](float z) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            return Canvas->surfaceVertices();
        };
        const int At1 = verticesAt(1.0f);
        QVERIFY2(At1 > 0, "nothing was drawn at 1:1");

        QStringList outliers;
        for (float z : {0.75f, 0.5f, 0.3f}) {
            const int Now = verticesAt(z);
            if (Now < At1)
                outliers << QString("zoom %1 emitted FEWER vertices than 1:1 (%2 < %3) - content was culled "
                                    "before the transform could bring it on screen").arg(z).arg(Now).arg(At1);
        }
        // And the far-out view must show substantially more than the 1:1 one, or "more of the graph is
        // visible" is not actually true of what got drawn.
        const int AtFar = verticesAt(0.3f);
        if (AtFar < At1 * 3 / 2)
            outliers << QString("zoom 0.3 drew only %1 vertices against %2 at 1:1 - the extra nodes it "
                                "brought on screen are empty").arg(AtFar).arg(At1);
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // The minimap is drawn into a child of its OWN, created after the editor's, for one reason: draw order.
    // A draw list appended to the parent window renders UNDERNEATH imnodes' scrolling region, so the overview
    // would be painted and then covered by the canvas background — invisible, with nothing in the code to say
    // why. Assert the ordering imgui actually produced rather than trusting that reasoning.
    void theMinimapRendersAboveTheCanvas()
    {
        Canvas->setMiniMap(true);
        for (int i = 0; i < 6; ++i) Canvas->addNode("Content", 100.0f + i * 300.0f, 80.0f + i * 200.0f);
        runFrame(); runFrame();

        const ImGuiContext &C = *ImGui::GetCurrentContext();
        const ImDrawList *MiniDL = nullptr, *EditorDL = nullptr;
        for (int w = 0; w < C.Windows.Size; ++w) {
            const ImGuiWindow *W = C.Windows[w];
            //ACTIVE only. The imgui context outlives a single test, so windows from earlier canvases are
            //still in the list under the same names — and a stale one's draw list is in no frame at all.
            if (!W->Name || !W->Active) continue;
            if (std::strstr(W->Name, "##minimap")) MiniDL = W->DrawList;
            else if (std::strstr(W->Name, "scrolling_region")) EditorDL = W->DrawList;
        }
        QVERIFY2(MiniDL, "no ##minimap window was created");
        QVERIFY2(EditorDL, "no imnodes scrolling_region window - the canvas did not draw");
        QVERIFY2(MiniDL != EditorDL, "the minimap shares the canvas draw list - it would be scaled by the view "
                                     "transform and covered by the canvas background");

        int MiniAt = -1, EditorAt = -1;
        const ImDrawData *D = ImGui::GetDrawData();
        QVERIFY2(D, "no draw data");
        for (int i = 0; i < D->CmdListsCount; ++i) {
            if (D->CmdLists[i] == MiniDL)   MiniAt = i;
            if (D->CmdLists[i] == EditorDL) EditorAt = i;
        }
        QVERIFY2(MiniAt >= 0 && EditorAt >= 0,
                 qPrintable(QString("a draw list never reached the frame: minimap %1, canvas %2")
                                .arg(MiniAt).arg(EditorAt)));
        QVERIFY2(MiniAt > EditorAt,
                 qPrintable(QString("the minimap renders UNDER the canvas: list %1 vs %2").arg(MiniAt).arg(EditorAt)));
        Canvas->setMiniMap(false);
    }

    // The minimap draws from OUR node array, not from what imnodes was handed — that is the whole reason it can
    // coexist with viewport culling, and the headline claim of replacing the built-in one.
    //
    // Asserting that its RECTANGLE exists proves none of that: the rectangle is computed from the canvas
    // viewport before anything is drawn, so it is a healthy non-empty box whether the overview painted 107
    // nodes, one, or none. Demonstrated: with the node-rectangle loop cut to zero iterations the whole suite
    // stayed green. So count what the overview actually PAINTED, and pin it to the graph rather than to the
    // viewport: a graph ten times larger draws a busier minimap even when culling submits fewer nodes than the
    // small one did.
    void theMinimapDrawsTheWholeGraphWhileCullingHidesMostOfIt()
    {
        Canvas->setMiniMap(true);
        for (int i = 0; i < 6; ++i) Canvas->addNode("Content", 100.0f + i * 120.0f, 80.0f + i * 90.0f);
        runFrame(); runFrame();
        const int Small = miniMapVertices();
        const int SmallDrawn = Canvas->visibleNodes();
        QVERIFY2(Small > 0, "the minimap painted nothing at all");

        // Ten times the nodes, spread far enough that culling submits FEWER than the small graph did.
        for (int i = 0; i < 60; ++i) Canvas->addNode("Content", 4000.0f + i * 2600.0f, 3000.0f + i * 1700.0f);
        runFrame(); runFrame();
        const int Big = miniMapVertices();
        QVERIFY2(Canvas->visibleNodes() <= SmallDrawn,
                 qPrintable(QString("culling submitted %1 of %2 - not fewer than the 6-node graph's %3, so "
                                    "this proves nothing about drawing what was NOT submitted")
                                .arg(Canvas->visibleNodes()).arg(Canvas->nodeCount()).arg(SmallDrawn)));
        QVERIFY2(Big > Small * 3,
                 qPrintable(QString("66 nodes drew %1 minimap vertices against %2 for 6 - the overview is not "
                                    "drawing the nodes culling hid").arg(Big).arg(Small)));
        Canvas->setMiniMap(false);
    }

    // Click-to-centre. The entire interactive half of the new overview had no coverage, so the mapping from a
    // minimap point back to a world point - and the pan that puts it in the middle of the viewport - could be
    // any function at all and every test would still pass.
    void clickingTheMinimapCentresTheViewThere()
    {
        Canvas->setMiniMap(true);
        // Two nodes far apart, so the overview spans a wide world and a click at one end is unambiguous.
        Canvas->addNode("Content", 0, 0);
        Canvas->addNode("Content", 6000, 3600);
        runFrame(); runFrame();
        float mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
        Canvas->miniMapRect(mx0, my0, mx1, my1);
        QVERIFY2(mx1 - mx0 > 50.0f, "no minimap to click");

        auto screenOf = [&](int n) { return ImNodes::GetNodeScreenSpacePos(n); };
        float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
        Canvas->canvasViewport(vx0, vy0, vx1, vy1);
        const ImVec2 Centre((vx0 + vx1) * 0.5f, (vy0 + vy1) * 0.5f);

        // Click near the FAR (bottom-right) corner of the overview: the far node must end up near the middle
        // of the viewport.
        clickAt(ImVec2(mx1 - 12.0f, my1 - 12.0f));
        runFrame(); runFrame();
        const ImVec2 Far = screenOf(1);
        const float D = std::hypot(Far.x - Centre.x, Far.y - Centre.y);
        QVERIFY2(D < 420.0f,
                 qPrintable(QString("clicking the far corner of the overview left the far node %1px from the "
                                    "viewport centre (node at %2,%3, centre %4,%5)")
                                .arg(D).arg(Far.x).arg(Far.y).arg(Centre.x).arg(Centre.y)));

        // And the near corner brings the other node back, so it is a MAPPING and not a constant.
        clickAt(ImVec2(mx0 + 12.0f, my0 + 12.0f));
        runFrame(); runFrame();
        const ImVec2 Near = screenOf(0);
        const float D2 = std::hypot(Near.x - Centre.x, Near.y - Centre.y);
        QVERIFY2(D2 < 420.0f,
                 qPrintable(QString("clicking the near corner left the near node %1px from centre").arg(D2)));
        Canvas->setMiniMap(false);
    }

    // THE WORST BUG THIS CANVAS HAS HAD, and it was introduced by the minimap's own input guard. The guard
    // handed imnodes ImVec2(-FLT_MAX,-FLT_MAX) whenever the cursor was over the overview — but
    // TranslateSelectedNodes computes a dragged node's origin ABSOLUTELY from that position and runs whether or
    // not the cursor is over the minimap. So dragging a node toward the bottom-right corner, where the minimap
    // lives, wrote -3.4e38 into the node, into the saved layout and into GlobalConfig: a node that can never be
    // drawn (it fails every viewport test), never selected, never dragged back, and that survives reload
    // because a layout entry wins over the package's own POS.
    //
    // The drag must therefore SURVIVE crossing the minimap with a sane position, and nothing absurd may ever
    // reach the layout by any route.
    void draggingANodeAcrossTheMinimapDoesNotDestroyIt()
    {
        Canvas->setMiniMap(true);
        Canvas->addNode("Content", 200, 200);
        runFrame(); runFrame();
        float mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
        Canvas->miniMapRect(mx0, my0, mx1, my1);
        QVERIFY2(mx1 - mx0 > 50.0f, "no minimap - this test would cross nothing");

        // Grab the node's title bar and drag into the middle of the minimap.
        const ImVec2 Grab(ImNodes::GetNodeScreenSpacePos(0).x + 40.0f,
                          ImNodes::GetNodeScreenSpacePos(0).y + 8.0f);
        dragFromTo(Grab, ImVec2((mx0 + mx1) * 0.5f, (my0 + my1) * 0.5f));

        // What IMNODES holds, checked first and deliberately. The read-back validates itself before writing to
        // the layout, so a poisoned position never reaches G.Nodes or the layout at all — which means those
        // two, on their own, stay green with the original -FLT_MAX guard still in place. The defence in depth
        // hides the defect from any test that only looks downstream of it. This is the position the bug
        // actually corrupts.
        const ImVec2 Held = ImNodes::GetNodeGridSpacePos(0);
        QVERIFY2(std::isfinite(Held.x) && std::isfinite(Held.y)
                     && std::abs(Held.x) < 1.0e6f && std::abs(Held.y) < 1.0e6f,
                 qPrintable(QString("the drag destroyed the position imnodes holds: (%1,%2)")
                                .arg(double(Held.x)).arg(double(Held.y))));

        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes.size(), size_t(1));
        QVERIFY2(std::isfinite(G.Nodes[0].X) && std::isfinite(G.Nodes[0].Y)
                     && std::abs(G.Nodes[0].X) < 1.0e6f && std::abs(G.Nodes[0].Y) < 1.0e6f,
                 qPrintable(QString("the drag destroyed the node's position: (%1,%2)")
                                .arg(double(G.Nodes[0].X)).arg(double(G.Nodes[0].Y))));
        const QString Dump = QString::fromStdString(Layout.dump());
        QVERIFY2(!Dump.contains("e+3") && !Dump.contains("inf") && !Dump.contains("nan"),
                 qPrintable("an absurd coordinate reached the saved layout: " + Dump));
        // WHERE it landed, not merely that it is finite. "Somewhere sane" is far too weak a claim: with the
        // suppression applied mid-drag the node is parked at the off-canvas sentinel — about (-10040,-10008),
        // comfortably inside every |v| < 1e6 bound above and comfortably "moved more than 20px" — and the
        // layout is saved with it. A drag has a destination, so assert the destination: the node's grab point
        // follows the cursor, so its origin must end within a node's width of where the cursor was released,
        // in the same world units imnodes holds.
        float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
        Canvas->canvasViewport(vx0, vy0, vx1, vy1);
        const ImVec2 Pan = ImNodes::EditorContextGetPanning();
        const ImVec2 Release((mx0 + mx1) * 0.5f, (my0 + my1) * 0.5f);
        const ImVec2 WantWorld(Release.x - vx0 - Pan.x, Release.y - vy0 - Pan.y);   // zoom is 1 here
        const float Off = std::hypot(G.Nodes[0].X - WantWorld.x, G.Nodes[0].Y - WantWorld.y);
        QVERIFY2(Off < 360.0f,
                 qPrintable(QString("the drag released at world (%1,%2) and the node ended at (%3,%4) - %5 "
                                    "units away, so it did not follow the cursor")
                                .arg(WantWorld.x).arg(WantWorld.y)
                                .arg(double(G.Nodes[0].X)).arg(double(G.Nodes[0].Y)).arg(Off)));
        // And it must have actually MOVED, or the guard is just refusing to drag at all.
        QVERIFY2(std::abs(G.Nodes[0].X - 200.0f) > 20.0f || std::abs(G.Nodes[0].Y - 200.0f) > 20.0f,
                 "the node did not move - the drag never happened, so this proves nothing");
        Canvas->setMiniMap(false);
    }

    // Clicking the minimap must not also reach the canvas behind it. The suppression is what makes that true,
    // and the test above constrains how much suppression is allowed - so pin the other half too, or "fix" the
    // crash by never suppressing at all and both tests still pass.
    void clickingTheMinimapDoesNotTouchTheCanvasBehindIt()
    {
        Canvas->setMiniMap(true);
        // A node placed so it sits UNDER the minimap corner at 1:1.
        Canvas->addNode("Content", 1050, 700);
        runFrame(); runFrame();
        float mx0 = 0, my0 = 0, mx1 = 0, my1 = 0;
        Canvas->miniMapRect(mx0, my0, mx1, my1);
        const PkgGraph::Graph Before = Canvas->graph();

        clickAt(ImVec2((mx0 + mx1) * 0.5f, (my0 + my1) * 0.5f));
        runFrame();
        QCOMPARE(ImNodes::NumSelectedNodes(), 0);      // nothing behind the overview was selected
        const PkgGraph::Graph After = Canvas->graph();
        QCOMPARE(After.Nodes[0].X, Before.Nodes[0].X);
        QCOMPARE(After.Nodes[0].Y, Before.Nodes[0].Y);
        Canvas->setMiniMap(false);
    }

    // imnodes gates every interaction on "is the mouse in the canvas", testing the position we hand it against
    // the canvas rectangle it measured in SCREEN space. Those stop being the same space the moment the zoom is
    // not 1, and the consequence was that only the top-left Z-by-Z fraction of the canvas responded to
    // anything: at 0.5x a node plainly visible in the lower right could not be clicked.
    void everyVisibleNodeIsClickableAtEveryZoom()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 1500, 1000);        // off-screen at 1:1, well on-screen zoomed out
        QStringList outliers;
        for (float z : {1.0f, 0.75f, 0.5f, 0.3f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            // Where the node actually IS on screen: world -> editor -> transform.
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            float ox = 0, oy = 0, dummy = 0;
            Canvas->canvasViewport(ox, oy, dummy, dummy);
            const ImVec2 Hit(ox + (P.x + 40.0f - ox) * z, oy + (P.y + 8.0f - oy) * z);
            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            if (Hit.x < vx0 || Hit.x > vx1 || Hit.y < vy0 || Hit.y > vy1) continue;   // genuinely off-screen
            clickAt(Hit);
            runFrame();
            if (ImNodes::NumSelectedNodes() == 0)
                outliers << QString("zoom %1: the node is drawn at (%2,%3), inside the viewport, and a click "
                                    "there selected nothing").arg(z).arg(Hit.x).arg(Hit.y);
            ImNodes::ClearNodeSelection();
        }
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // Panning moves the view by `MouseDelta`, which imgui measured in SCREEN pixels, into a pan that is in
    // WORLD units - so without dividing it by the zoom the graph crawled at half speed at 0.5x and bolted at
    // 3x, and nothing you grabbed stayed under the pointer. Assert the thing the user feels: the graph moves
    // the same number of SCREEN pixels as the cursor, whatever the zoom.
    void panningMovesTheGraphWithTheCursorAtEveryZoom()
    {
        Canvas->setMiniMap(false);
        //Placed so the drag below starts on EMPTY canvas: a middle-drag begun ON a node is a different
        //gesture, imnodes never reaches BeginCanvasInteraction, and the pan silently measures zero.
        Canvas->addNode("Content", 300, 300);
        QStringList outliers;
        for (float z : {1.0f, 0.5f, 2.0f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            //setZoom recentres, so confirm the node is still drawn before measuring where it is.
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node was culled - cannot measure a pan");
            float ox = 0, oy = 0, d = 0;
            Canvas->canvasViewport(ox, oy, d, d);
            auto screenOf = [&]() {
                const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
                return ImVec2(ox + (P.x - ox) * z, oy + (P.y - oy) * z);
            };
            const ImVec2 Before = screenOf();
            // Middle-drag is the pan gesture; drive it the way imnodes reads it — and start it on EMPTY
            // canvas. A middle-drag begun ON a node is a different gesture: imnodes never reaches
            // BeginCanvasInteraction and the pan silently measures zero, which reads exactly like a broken
            // pan. Derived from where the node actually IS, because setZoom recentres and a fixed point
            // that was empty at one zoom sits on the node at the next.
            ImGuiIO &io = ImGui::GetIO();
            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            const ImVec2 From(vx0 + 60.0f, vy1 - 60.0f);
            const ImVec2 NodeAt = Before;
            const ImVec2 NodeSz = ImNodes::GetNodeDimensions(0);
            QVERIFY2(!(From.x >= NodeAt.x && From.x <= NodeAt.x + NodeSz.x * z
                       && From.y >= NodeAt.y && From.y <= NodeAt.y + NodeSz.y * z),
                     qPrintable(QString("zoom %1: the drag would start on the node, not on empty canvas")
                                    .arg(z)));
            runFrame(From);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
            runFrame(From);
            for (int i = 1; i <= 4; ++i) runFrame(ImVec2(From.x + i * 25.0f, From.y));
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
            runFrame(ImVec2(From.x + 100.0f, From.y));
            const ImVec2 After = screenOf();
            const float Moved = After.x - Before.x;
            if (std::abs(Moved - 100.0f) > 12.0f)
                outliers << QString("zoom %1: a 100px pan moved the graph %2px on screen").arg(z).arg(Moved);
        }
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // A widget that opens a CHILD WINDOW gets its own draw list, which the transform over the editor's list
    // never reaches. InputTextMultiline does — every StringList field with more than one line — so the node
    // body scaled and moved while the text inside it stayed at its 1:1 position, painted over whatever node
    // had moved there. Every geometry assertion passed it, because the box was still the right size.
    void everyNestedFieldScalesWithItsNode()
    {
        Canvas->setMiniMap(false);
        // SUBMOUNTS is a StringList; several lines make it a multiline box, which is what opens the child.
        const int N = Canvas->addNode("Content", 500, 300);
        Doc["NODES"][N]["SUBMOUNTS"] = json::array({"a/b:c/d", "e/f:g/h", "i/j:k/l"});
        Canvas->invalidateGraph();
        runFrame(); runFrame();

        auto nestedBounds = [&]() {
            const ImGuiContext &C = *ImGui::GetCurrentContext();
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            int found = 0;
            for (int w = 0; w < C.Windows.Size; ++w) {
                const ImGuiWindow *W = C.Windows[w];
                const char *SR = W->Name ? std::strstr(W->Name, "scrolling_region") : nullptr;
                if (!W->Active || !SR || !std::strchr(SR, '/')) continue;   // nested INSIDE the editor child
                const ImDrawList *D = W->DrawList;
                if (D->VtxBuffer.Size == 0) continue;
                ++found;
                for (int v = 0; v < D->VtxBuffer.Size; ++v) {
                    x0 = std::min(x0, D->VtxBuffer[v].pos.x); x1 = std::max(x1, D->VtxBuffer[v].pos.x);
                    y0 = std::min(y0, D->VtxBuffer[v].pos.y); y1 = std::max(y1, D->VtxBuffer[v].pos.y);
                }
            }
            return std::array<float, 5>{(float)found, x0, y0, x1, y1};
        };

        const std::array<float, 5> At1 = nestedBounds();
        QVERIFY2(At1[0] > 0.0f, "no nested child window was drawn - this test is measuring nothing");
        Canvas->setZoom(2.0f);
        runFrame(); runFrame();
        //setZoom recentres the view, so the node has to still be submitted for its field to exist at all.
        QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(),
                 "the node was culled at 2x - nothing to measure");
        const std::array<float, 5> At2 = nestedBounds();
        QVERIFY2(At2[0] > 0.0f, "the nested child vanished when zoomed");
        // Its own extent must have grown with the zoom, like everything else the canvas draws.
        const float W1 = At1[3] - At1[1], W2 = At2[3] - At2[1];
        QVERIFY2(W2 > W1 * 1.4f,
                 qPrintable(QString("a nested field did not scale: %1px wide at 1:1, %2px at 2x - it is being "
                                    "drawn at its unscaled position on top of the canvas").arg(W1).arg(W2)));
        Canvas->setZoom(1.0f);
    }

    // imnodes draws its grid inside BeginNodeEditor, before the first vertex the transform can reach, so it
    // kept a fixed screen pitch and translated 1:1 while the content translated Z:1 - the graph slid across
    // its own grid on every pan, and the grid gave no scale cue at all. Ours is drawn as content.
    void theGridScalesWithTheView()
    {
        Canvas->setMiniMap(false);
        // No nodes on purpose: with an empty graph the only thin, tall quads in the editor's draw list are
        // grid lines, so the measurement cannot pick up a node border or a glyph and call it the grid.
        auto pitchAt = [&](float z) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            const ImDrawList *D = nullptr;
            const ImGuiContext &C = *ImGui::GetCurrentContext();
            for (int w = 0; w < C.Windows.Size; ++w)
                if (C.Windows[w]->Name && C.Windows[w]->Active
                    && std::strstr(C.Windows[w]->Name, "scrolling_region")
                    && !std::strstr(C.Windows[w]->Name, "scrolling_region/"))
                    D = C.Windows[w]->DrawList;
            if (!D) return 0.0f;
            // Grid lines are the thin 1px verticals; collect distinct x of 4-vertex quads that are tall.
            std::vector<float> xs;
            for (int v = 0; v + 3 < D->VtxBuffer.Size; v += 4) {
                const float ax = D->VtxBuffer[v].pos.x, bx = D->VtxBuffer[v + 2].pos.x;
                const float ay = D->VtxBuffer[v].pos.y, by = D->VtxBuffer[v + 2].pos.y;
                if (std::abs(bx - ax) < 4.0f && (by - ay) > 200.0f) xs.push_back((ax + bx) * 0.5f);
            }
            std::sort(xs.begin(), xs.end());
            float best = 0.0f;
            for (size_t i = 1; i < xs.size(); ++i) {
                const float d = xs[i] - xs[i - 1];
                if (d > 1.0f && (best == 0.0f || d < best)) best = d;
            }
            return best;
        };
        const float P1 = pitchAt(1.0f);
        QVERIFY2(P1 > 1.0f, "no grid lines found at 1:1 - the grid is not being drawn");
        const float P2 = pitchAt(2.0f);
        QVERIFY2(P2 > P1 * 1.5f,
                 qPrintable(QString("the grid pitch did not scale: %1px at 1:1, %2px at 2x - the grid is "
                                    "pinned to the screen while the graph moves over it").arg(P1).arg(P2)));
        Canvas->setZoom(1.0f);
    }

    // NodeDims is the one piece of per-node canvas state the delete/rename paths forgot. renameNode runs on
    // EVERY KEYSTROKE of an id edit, so a missed move leaks one entry per character typed and lets a node
    // later named back into an old id inherit a stale box size in the overview; removeNode's own comment says
    // exactly why every other map is maintained there.
    void theMeasuredSizeCacheFollowsRenamesAndDeletes()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        Canvas->addNode("DeclareExec", 500, 100);
        Canvas->addNode("DeclarePersist", 900, 100);
        runFrame(); runFrame();
        QCOMPARE(Canvas->cachedNodeSizes(), 3);

        // A typed id arrives one character at a time, which is one rename per character.
        const std::string Start = Doc["NODES"][0]["NODE_ID"].get<std::string>();
        std::string Cur = Start;
        for (const char *C = "abcdef"; *C; ++C) {
            const std::string Next = Cur + *C;
            Canvas->renameNode(0, Next);
            Cur = Next;
            runFrame(); runFrame();
        }
        QVERIFY2(Canvas->cachedNodeSizes() == 3,
                 qPrintable(QString("typing a 6-character id left %1 cached sizes for 3 nodes - the rename "
                                    "path is leaking one per keystroke").arg(Canvas->cachedNodeSizes())));

        Canvas->removeNode(2);
        runFrame(); runFrame();
        QVERIFY2(Canvas->cachedNodeSizes() == 2,
                 qPrintable(QString("a deleted node left its measured size behind (%1 for 2 nodes)")
                                .arg(Canvas->cachedNodeSizes())));
    }

    // The zoom ease re-solves the pan from an anchor captured at the wheel event, every frame until it
    // arrives. Applied unconditionally that DISCARDED whatever the user panned in the meantime: a middle-drag
    // begun one frame after a notch ended up exactly where the ease wanted it, drag thrown away.
    void panningDuringAZoomEaseIsNotThrownAway()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 300, 300);
        runFrame(); runFrame();
        const ImVec2 Pan0 = ImNodes::EditorContextGetPanning();

        ImGuiIO &io = ImGui::GetIO();
        io.AddMouseWheelEvent(0.0f, +1.0f);
        runFrame(ImVec2(700, 450));                    // the notch: the ease is now in flight
        QVERIFY2(Canvas->zoom() < 1.1f - 0.001f, "the zoom did not ease, so there is no ease to pan during");

        const ImVec2 From(700, 450);
        runFrame(From);
        io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
        runFrame(From);
        for (int i = 1; i <= 4; ++i) runFrame(ImVec2(From.x + i * 30.0f, From.y));
        io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
        runFrame(ImVec2(From.x + 120.0f, From.y));
        const ImVec2 Pan1 = ImNodes::EditorContextGetPanning();

        QVERIFY2(Pan1.x - Pan0.x > 40.0f,
                 qPrintable(QString("a 120px pan during the zoom ease moved the view %1 units - the ease "
                                    "overwrote it").arg(Pan1.x - Pan0.x)));
        Canvas->setZoom(1.0f);
    }

    // The layout steps vertically by each node's ESTIMATED height, and it has to be an estimate: the layout is
    // pure and is stamped into POS at publish time, headless, where nothing has ever been rendered. An estimate
    // that drifts from the renderer is a layout that overlaps again, silently, and no layout-level test can see
    // it — so this is the one place the two are tied together.
    //
    // Short is the dangerous direction (two nodes drawn on top of each other); generous merely wastes space.
    void theEstimatedNodeHeightMatchesTheDrawnOne()
    {
        Canvas->setMiniMap(false);
        //Issues and hints both add rows to a node — a warning line each, an action button each — so this
        //states outright that there are none rather than relying on it. They cannot in fact arrive from an
        //earlier test (the fixture builds a fresh PkgCanvas per test, and both live on it), which is worth
        //saying because an earlier version of this comment blamed them for a cross-test effect that was really
        //ImGui's: tree open/closed state lives in the CONTEXT, which does outlive the canvas. Nothing to
        //restore, therefore — a fresh canvas has neither.
        Canvas->setIssues({});
        QStringList outliers;

        // ONE node at a time, at the top-left of the viewport. A node placed off-screen is culled, and
        // GetNodeDimensions then reports the title-bar-only size of the box imnodes re-created — 35.5px for
        // everything, which compares as "wildly over-estimated" for every type at once. That is a measurement
        // artifact, not a finding, and it is exactly the shape of one: a uniform, implausible number.
        //The estimate reserves the "node options" tree as though it were OPEN, so what must be compared against
        //is the OPEN height — and that state is NOT ours to set. It lives in ImGui's per-window storage under
        //an id derived from the node INDEX (imnodes pushes it), the context outlives the per-test canvas, and
        //SetNextItemOpen(..., Once) does not override a stored value. An earlier attempt to force it with a
        //fresh NODE_ID was simply inert for that reason, and the SHORT branch — the one this test exists for —
        //stayed armed or disarmed depending on which tests ran first, silently, by 57px on every node.
        //
        //So measure BOTH states and compare against the taller. No guessing, no ordering.
        auto measure = [&](const char *Type, const json &Payload) {
            const int N = Canvas->addNode(Type, 100.0f, 100.0f);
            for (auto It = Payload.begin(); It != Payload.end(); ++It) Doc["NODES"][N][It.key()] = It.value();
            //Hints are host facts ("this zip is deflate") that add ACTION BUTTONS, and an id reused from an
            //earlier test in this suite arrives carrying them — worth 50-odd pixels of extra button rows on
            //every measurement. Cleared for the node under test so what is measured is the payload.
            Canvas->setNodeHints(Doc["NODES"][N].value("NODE_ID", std::string()), {});
            Canvas->invalidateGraph();
            runFrame(); runFrame();
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(),
                     qPrintable(QString("%1 of %2 nodes submitted - a culled node cannot be measured")
                                    .arg(Canvas->visibleNodes()).arg(Canvas->nodeCount())));
            float Drawn = ImNodes::GetNodeDimensions(N).y;
            //Toggle the options tree and keep the taller reading. The toggle is a click on the tree's own row,
            //found by effect: the one click down the node's left edge that changes its height.
            {
                float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
                Canvas->canvasViewport(vx0, vy0, vx1, vy1);
                const ImVec2 P = ImNodes::GetNodeScreenSpacePos(N);
                bool Toggled = false;
                for (float dy = 40.0f; dy < 140.0f && !Toggled; dy += 4.0f) {
                    const ImVec2 At(P.x + 24.0f, P.y + dy);
                    if (At.x < vx0 || At.x > vx1 || At.y < vy0 || At.y > vy1) continue;
                    clickAt(At);
                    runFrame(); runFrame();
                    const float H2 = ImNodes::GetNodeDimensions(N).y;
                    if (std::abs(H2 - Drawn) > 25.0f) { Drawn = std::max(Drawn, H2); Toggled = true; }
                }
                if (!Toggled)
                    outliers << QString("%1: the node options tree could not be toggled, so only one of its "
                                        "two heights was measured").arg(Type);
            }
            const float Wide  = ImNodes::GetNodeDimensions(N).x;
            const float Est   = Canvas->graph().Nodes[(size_t)N].Height;
            const QString Who = QString("%1%2").arg(Type).arg(Payload.empty() ? "" : " (loaded)");
            //The layout's per-column overlap check rests on a node being narrower than ColumnStep, and nothing
            //estimates width — so the assumption is measured here, where a real node is actually on screen,
            //rather than asserted between two compile-time constants.
            if (Wide >= 430.0f)
                outliers << QString("%1 is %2px wide, at or past the 430px column step - nodes in adjacent "
                                    "columns can now collide").arg(Who).arg(Wide);
            if (Est < Drawn)
                outliers << QString("%1 is drawn %2px but estimated only %3px - SHORT, so the layout will "
                                    "overlap it").arg(Who).arg(Drawn).arg(Est);
            //The slack allowed is deliberately wide, because the estimate deliberately reserves three rows for
            //a "node options" tree that may or may not be open — ImGui keeps that state in its own per-window
            //storage, so the SAME node measures 57px taller or shorter here depending only on which tests ran
            //before this one. A tight bound would make this test pass or fail on test ORDER, which is worse
            //than useless. What it still catches is the failure that matters: an estimate that is SHORT, and
            //one that is wrong by a whole payload (the ignored-cap mutation reports 8680 against 2967).
            //An ABSOLUTE band, not a proportional one, and that distinction is the whole point: a proportional
            //bound grows with the node, so a per-row or per-entry error hides inside it. What the slack
            //legitimately covers is FIXED — two reserved warning lines, plus a little for rows the arms round
            //up. It does NOT need to cover the "node options" tree any more: an earlier version budgeted three
            //rows for it because the tree's state was whatever previous tests had left, and the measurement
            //above now takes both states, so that ambiguity is gone and so is the 57px it bought.
            //
            //Measured across every shape here (2026-09-14, all 20 of them): 31px on a Group or a RegEdit,
            //48px on most, 65px worst case on a DeclareExec. 90 leaves a row's rounding over that. It was 120
            //against a claimed 94px worst case, which no shape in the suite has produced since the estimate
            //was recalibrated — an unearned 55px of headroom. Note the ASYMMETRY even so: the SHORT arm is
            //tight to the pixel (kRowPx 19 -> 18 fails), while over-estimating has ~59px of headroom over the
            //31px baseline, so a single-pixel over-charge on a per-entry constant still needs ~59 entries
            //before it is caught. Doublings and the real per-entry mistakes are caught; a 1px over-charge on
            //kSepPx or kBtnPx is not, and that is a deliberate trade against false positives on shapes the
            //suite does not build.
            else if (Est - Drawn > 90.0f)
                outliers << QString("%1 is drawn %2px but estimated %3px - %4px of slack, past the fixed "
                                    "reservations, so the layout will leave a hole")
                                .arg(Who).arg(Drawn).arg(Est).arg(Est - Drawn);
            Canvas->removeNode(N);
            Canvas->invalidateGraph();
            runFrame();
        };

        for (const std::string &T : PkgGraph::AllTypes()) measure(T.c_str(), json::object());

        // The payloads that actually make a node tall.
        json Keys = json::object();
        for (int r = 0; r < 24; ++r) Keys["Software"]["App"]["v" + std::to_string(r)] = "data";
        json E = json::object(); E["ARCHITECTURE"] = json::array({"64"}); E["HKLM"] = Keys;
        measure("RegEdit", json{{"EDITS", json::array({E})}});

        // SEVERAL ENTRIES, which is the shape with a per-entry cost and the one every case here had missed:
        // one entry hides an error worth a whole row each time, and `capture_reg` emits an entry per captured
        // hive and architecture. Measured with that error present: +17px per entry, 1091px on a 59-entry node.
        {
            json Entries = json::array();
            for (int e = 0; e < 10; ++e) {
                json K = json::object();
                for (int r = 0; r < 4; ++r) K["Software"]["G" + std::to_string(e)]["v" + std::to_string(r)] = "d";
                json En = json::object(); En["ARCHITECTURE"] = json::array({"64"}); En["HKLM"] = K;
                Entries.push_back(En);
            }
            measure("RegEdit", json{{"EDITS", Entries}});
        }

        // FORTY-FIVE entries. The per-entry arms are the ones that hide errors — one entry conceals a whole
        // row's worth either way — and the bound only turns per-entry drift into a failure once enough entries
        // have accumulated it. `capture_reg` emits an entry per captured hive and architecture.
        {
            json Es = json::array();
            for (int e = 0; e < 45; ++e) {
                json K = json::object();
                for (int r = 0; r < 4; ++r) K["Software"]["G" + std::to_string(e)]["v" + std::to_string(r)] = "d";
                json En = json::object(); En["ARCHITECTURE"] = json::array({"64"}); En["HKLM"] = K;
                Es.push_back(En);
            }
            measure("RegEdit", json{{"EDITS", Es}});
        }

        // MALFORMED entries, which drawRegEdits short-circuits to a separator and one disabled line. The
        // editor exists to open broken packages, so this is a shape it must measure correctly — charging a
        // full entry for it was 19.6px each, enough to trip the slack bound on a node made of them.
        measure("RegEdit", json{{"EDITS", json::array({"nope", 7, json::object({{"HKLM", json::object()}}),
                                                       "also nope", 9, "and again"})}});

        // And a REALLY tall one. The slack the estimate reserves is fixed, so a per-ROW calibration error only
        // becomes visible once enough rows have accumulated it: shaving 1.5px off the constant every row is
        // multiplied by is invisible at 24 rows and 180px short at 120. The codec libraries ship 59-row nodes,
        // so this is the direction the real library grows in.
        json ManyKeys = json::object();
        for (int r = 0; r < 120; ++r) ManyKeys["Software"]["App"]["v" + std::to_string(r)] = "data";
        json E2 = json::object(); E2["ARCHITECTURE"] = json::array({"64"}); E2["HKLM"] = ManyKeys;
        measure("RegEdit", json{{"EDITS", json::array({E2})}});

        json Patches = json::array();
        for (int r = 0; r < 5; ++r)
            Patches.push_back(json{{"MODE", "Replace"}, {"OFFSET", "0x1000"}, {"EXPECT", "90"}, {"REPLACE", "cc"}});
        measure("BinaryPatch", json{{"EDITS", Patches}});

        // PAST the cap. drawField reads a batched payload capped at 12 entries and prints "... and N more"
        // instead, so the estimate must stop growing at exactly the same point: count past the cap and the
        // layout reserves a screenful nothing occupies; ignore the cap and it under-reserves, which overlaps.
        json Many = json::array();
        for (int r = 0; r < 30; ++r)
            Many.push_back(json{{"MODE", "Replace"}, {"OFFSET", "0x1000"}, {"EXPECT", "90"}, {"REPLACE", "cc"}});
        measure("BinaryPatch", json{{"EDITS", Many}});

        // A list longer than the multiline box's own 6-line cap, for the same reason.
        json Lines = json::array();
        for (int r = 0; r < 20; ++r) Lines.push_back("a" + std::to_string(r) + ":b");
        measure("Content", json{{"SUBMOUNTS", Lines}});

        // TOGGLE/WHEN open the "node options" tree, which is three more rows.
        measure("CustomVar", json{{"TOGGLE", "on"}, {"WHEN", "%x%==1"}});
        // A multi-line StringList becomes a multiline box rather than a single-line input.
        measure("Content", json{{"SUBMOUNTS", json::array({"a:b", "c:d", "e:f", "g:h"})}});

        // MALFORMED StringLists, which drawField short-circuits to one disabled line. This is the shape that
        // was drawing a node 477px WIDE — past the 430px column step, i.e. across its right-hand neighbour —
        // because the arm printed the whole value with dump() on a single unwrapped line. A 2000-character
        // value measured 14407px. Both checks in `measure` bear on it: the width one is what catches the
        // overflow, and the height one pins the estimate's new kTextPx arm for this shape.
        measure("DeclareExec", json{{"ARGS", json("not a list")}});
        measure("DeclareExec", json{{"ARGS", json::object({{"looks", "structured"}})}});
        measure("DeclareExec", json{{"ARGS", json(std::string(2000, 'w'))}});
        measure("DeclareExec", json{{"ARGS", json::array({5})}});
        // SEVERAL GOOD ENTRIES AND ONE BAD ONE. This is the shape that catches the renderer and the estimator
        // drifting apart: [5] has no strings to measure either way, so it cannot. With the element scan in
        // drawField only, this measured 437px drawn against 600px estimated — a 163px hole reserved in the
        // stamped POS for a delta's BASE_TARGETS with one number in it.
        measure("DeclareExec", json{{"ARGS", json::array({"a", "b", "c", "d", "e", "f", 5})}});
        measure("Content", json{{"BASE_TARGETS", json::array({"", "x", 5})}, {"FORM", "delta"}});
        // A container holding a real payload, with one entry of the wrong type so it actually REACHES the
        // refusal arm: a list of 5000 strings is well-formed and takes the ordinary path. The describer must
        // report the value's SIZE and never serialise it — this one dumps to over a megabyte, which on a
        // per-frame path is the editor freezing on the package you opened to repair it.
        {
            json Huge = json::array();
            for (int r = 0; r < 5000; ++r) Huge.push_back(std::string(200, 'q'));
            Huge.push_back(7);
            measure("DeclareExec", json{{"ARGS", Huge}});
            json HugeStr = std::string(4 * 1024 * 1024, 'w');
            measure("DeclareExec", json{{"ARGS", HugeStr}});
            //A huge CONTAINER, which is the arm the list above does not reach: with a bad entry the canvas
            //describes THAT ENTRY, so only a value that is not a list at all sends the container itself to
            //the describer. This one dumps to ~1.5 MB.
            json HugeObj = json::object();
            for (int r = 0; r < 5000; ++r) HugeObj[std::to_string(r)] = std::string(200, 'q');
            measure("DeclareExec", json{{"ARGS", HugeObj}});
        }

        // The malformed-NODES placeholder: a title bar and one disabled line, and the only node whose height
        // PkgGraph::Build sets without going through the payload arms. Measured here because the layout test
        // that covers it can only compare it against other estimates, never against what is drawn.
        {
            const int N = (int)Doc["NODES"].size();
            Doc["NODES"].push_back("this entry is not an object");
            Canvas->invalidateGraph();
            runFrame(); runFrame();
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the placeholder was culled");
            const float Drawn = ImNodes::GetNodeDimensions(N).y;
            const float Est   = Canvas->graph().Nodes[(size_t)N].Height;
            if (Est < Drawn)
                outliers << QString("the malformed placeholder is drawn %1px but estimated only %2px - SHORT, "
                                    "so the layout will overlap it").arg(Drawn).arg(Est);
            else if (Est - Drawn > 20.0f)
                outliers << QString("the malformed placeholder is drawn %1px but estimated %2px - %3px of "
                                    "slack. It has no payload and no warnings, so the two should agree "
                                    "closely").arg(Drawn).arg(Est).arg(Est - Drawn);
            if (ImNodes::GetNodeDimensions(N).x >= 430.0f)
                outliers << QString("the malformed placeholder is %1px wide, past the column step")
                                .arg(ImNodes::GetNodeDimensions(N).x);
            Doc["NODES"].erase(Doc["NODES"].size() - 1);
            Canvas->invalidateGraph();
            runFrame();
        }

        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // Drawing a widget in the right place is half of it. imgui resolves g.HoveredWindow in NewFrame, from the
    // REAL cursor against last frame's window rectangles — before frame() runs, so the MousePos hijack cannot
    // reach it — and ItemHoverable then rejects any item whose window is not the hovered one. A multiline field
    // lives in a child window of its own, so scaling only its VERTICES draws it where it cannot be clicked.
    void aNestedFieldIsClickableWhereItIsDrawn()
    {
        Canvas->setMiniMap(false);
        const int N = Canvas->addNode("Content", 120, 120);
        Doc["NODES"][N]["SUBMOUNTS"] = json::array({"a/b:c/d", "e/f:g/h", "i/j:k/l"});
        Canvas->invalidateGraph();
        //Start from a known scale: the canvas is shared with every earlier test in this suite and a leftover
        //zoom decides whether the node is on screen at all.
        Canvas->setZoom(1.0f);
        runFrame(); runFrame();

        // Where the field's child window is DRAWN, at each zoom: its own draw-list bounds.
        auto drawnBox = [&]() {
            const ImGuiContext &C = *ImGui::GetCurrentContext();
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int w = 0; w < C.Windows.Size; ++w) {
                const ImGuiWindow *W = C.Windows[w];
                const char *SR = W->Name ? std::strstr(W->Name, "scrolling_region") : nullptr;
                if (!W->Active || !SR || !std::strchr(SR, '/')) continue;
                const ImDrawList *D = W->DrawList;
                for (int v = 0; v < D->VtxBuffer.Size; ++v) {
                    x0 = std::min(x0, D->VtxBuffer[v].pos.x); x1 = std::max(x1, D->VtxBuffer[v].pos.x);
                    y0 = std::min(y0, D->VtxBuffer[v].pos.y); y1 = std::max(y1, D->VtxBuffer[v].pos.y);
                }
            }
            return ImVec4(x0, y0, x1, y1);
        };

        QStringList outliers;
        for (float z : {1.0f, 2.0f, 0.5f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            //Put the node in the middle of the viewport at THIS zoom, so its field is on screen whatever the
            //recentre did. An off-screen child window is skipped by imgui entirely and draws no vertices,
            //which looks identical to "the transform lost it".
            {
                float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
                Canvas->canvasViewport(cx0, cy0, cx1, cy1);
                const ImVec2 P = ImNodes::GetNodeGridSpacePos(0);
                const ImVec2 Sz = ImNodes::GetNodeDimensions(0);
                ImNodes::EditorContextResetPanning(
                    ImVec2(((cx1 - cx0) * 0.5f) / z - P.x - Sz.x * 0.5f,
                           ((cy1 - cy0) * 0.5f) / z - P.y - Sz.y * 0.5f));
                runFrame(); runFrame();
            }
            const ImVec4 B = drawnBox();
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(),
                     qPrintable(QString("zoom %1: the node was culled, so its field does not exist").arg(z)));
            QVERIFY2(B.z > B.x, qPrintable(QString("zoom %1: the nested field drew nothing").arg(z)));
            const ImVec2 Hit((B.x + B.z) * 0.5f, (B.y + B.w) * 0.5f);
            // Click in the middle of where it is drawn; the field must take keyboard focus.
            ImGui::ClearActiveID();
            clickAt(Hit);
            runFrame();
            if (ImGui::GetActiveID() == 0)
                outliers << QString("zoom %1: the field is drawn at [%2,%3 .. %4,%5] and a click at its centre "
                                    "(%6,%7) focused nothing")
                                .arg(z).arg(B.x).arg(B.y).arg(B.z).arg(B.w).arg(Hit.x).arg(Hit.y);
            ImGui::ClearActiveID();
        }
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // setZoom is the toolbar and "reset". Zooming about the canvas ORIGIN means resetting from 3x throws the
    // view onto a different part of the graph than the one you were looking at — so it holds the middle of the
    // viewport still. The first attempt used the new scale on both sides of the solve, which cancels to
    // Pan = Pan: it compiled, ran every frame, and did precisely nothing.
    void setZoomHoldsTheMiddleOfTheViewStill()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 1400, 900);
        runFrame(); runFrame();
        float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
        Canvas->canvasViewport(vx0, vy0, vx1, vy1);
        const ImVec2 Centre((vx0 + vx1) * 0.5f, (vy0 + vy1) * 0.5f);

        auto onScreen = [&]() {
            const float Z = Canvas->zoom();
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            return ImVec2(vx0 + (P.x - vx0) * Z, vy0 + (P.y - vy0) * Z);
        };
        // Put the node under the middle of the viewport at 1:1.
        {
            const ImVec2 Pan = ImNodes::EditorContextGetPanning();
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            ImNodes::EditorContextResetPanning(ImVec2(Pan.x + (Centre.x - P.x), Pan.y + (Centre.y - P.y)));
            runFrame(); runFrame();
        }
        const ImVec2 Before = onScreen();
        QVERIFY2(std::hypot(Before.x - Centre.x, Before.y - Centre.y) < 30.0f,
                 qPrintable(QString("setup failed: the node is at (%1,%2), not the centre (%3,%4)")
                                .arg(Before.x).arg(Before.y).arg(Centre.x).arg(Centre.y)));

        QStringList outliers;
        for (float z : {2.0f, 0.5f, 1.0f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            const ImVec2 Now = onScreen();
            const float D = std::hypot(Now.x - Centre.x, Now.y - Centre.y);
            if (D > 40.0f)
                outliers << QString("zoom %1 moved the centre of the view %2px away (node at %3,%4, centre "
                                    "%5,%6)").arg(z).arg(D).arg(Now.x).arg(Now.y).arg(Centre.x).arg(Centre.y);
        }
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // Culling tested the node's ORIGIN against the viewport. A node is as tall as its payload makes it, so one
    // whose top has scrolled past the edge while the rest of it still fills the screen was dropped: the canvas
    // went blank with a node covering the whole viewport, unclickable and un-editable there. The height was
    // already being computed for the layout; this is the other place that needs it.
    void aTallNodeIsDrawnWhileAnyOfItIsOnScreen()
    {
        Canvas->setMiniMap(false);
        const int N = Canvas->addNode("BinaryPatch", 0, 0);
        json Patches = json::array();
        for (int r = 0; r < 12; ++r)
            Patches.push_back(json{{"MODE", "Replace"}, {"OFFSET", "0x1000"}, {"EXPECT", "90"}, {"REPLACE", "cc"}});
        Doc["NODES"][N]["EDITS"] = Patches;
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        const float H = Canvas->graph().Nodes[(size_t)N].Height;
        QVERIFY2(H > 1500.0f,
                 qPrintable(QString("the test node is only %1px tall - the -1400 case below needs it to reach "
                                    "the viewport top from there, so a shorter node would make this test "
                                    "demand that a genuinely off-screen node be drawn").arg(H)));

        QStringList outliers;
        // Scroll the node's TOP well above the viewport while its body still covers the screen. Every value is
        // past MarginY (500 * max(1, zoom)), or the origin test alone would already pass it and the case would
        // be incapable of failing: -400 was exactly that, and sat here looking like coverage.
        for (float pan : {-700.0f, -1000.0f, -1400.0f}) {
            ImNodes::EditorContextResetPanning(ImVec2(0.0f, pan));
            runFrame(); runFrame();
            if (Canvas->visibleNodes() != 1)
                outliers << QString("pan %1: the node spans %2..%3 and the viewport is 0..900, but it was "
                                    "culled").arg(pan).arg(pan).arg(pan + H);
        }
        ImNodes::EditorContextResetPanning(ImVec2(0, 0));
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // Two setZoom calls before a frame. The pending recentre has to keep the scale it STARTED from — taking
    // the second call's "from" pairs it with a pan that still belongs to the first call's scale.
    void twoZoomChangesInOneFrameStillHoldTheCentre()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 400, 300);
        Canvas->setZoom(1.0f);
        runFrame(); runFrame();
        float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
        Canvas->canvasViewport(vx0, vy0, vx1, vy1);
        const ImVec2 Centre((vx0 + vx1) * 0.5f, (vy0 + vy1) * 0.5f);
        auto onScreen = [&]() {
            const float Z = Canvas->zoom();
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            const ImVec2 D = ImNodes::GetNodeDimensions(0);
            return ImVec2(vx0 + (P.x + D.x * 0.5f - vx0) * Z, vy0 + (P.y + D.y * 0.5f - vy0) * Z);
        };
        // Park the node's middle on the viewport centre.
        {
            const ImVec2 Pan = ImNodes::EditorContextGetPanning();
            const ImVec2 At = onScreen();
            ImNodes::EditorContextResetPanning(ImVec2(Pan.x + (Centre.x - At.x), Pan.y + (Centre.y - At.y)));
            runFrame(); runFrame();
        }
        QVERIFY2(std::hypot(onScreen().x - Centre.x, onScreen().y - Centre.y) < 20.0f, "setup failed");

        // BOTH calls land before the next frame — the toolbar spinner and a double-click on "reset" do this.
        Canvas->setZoom(2.0f);
        Canvas->setZoom(0.5f);
        runFrame(); runFrame();
        const ImVec2 Now = onScreen();
        const float D = std::hypot(Now.x - Centre.x, Now.y - Centre.y);
        QVERIFY2(D < 40.0f,
                 qPrintable(QString("two zoom changes in one frame moved the centre %1px (node at %2,%3, "
                                    "centre %4,%5)").arg(D).arg(Now.x).arg(Now.y).arg(Centre.x).arg(Centre.y)));
        Canvas->setZoom(1.0f);
    }

    // A StringList's height comes from the LINES the field renders, and the join that builds that text does not
    // escape a newline inside an entry — so one array element carrying embedded newlines is a multiline box
    // that an entry count calls a single-line input. Any package this canvas did not author can contain one.
    void aListEntryWithNewlinesIsMeasuredByItsLines()
    {
        Canvas->setMiniMap(false);
        Canvas->setIssues({});
        auto measure = [&](const char *Type, const json &Payload) {
            const int N = Canvas->addNode(Type, 100.0f, 100.0f);
            for (auto It = Payload.begin(); It != Payload.end(); ++It) Doc["NODES"][N][It.key()] = It.value();
            Canvas->setNodeHints(Doc["NODES"][N].value("NODE_ID", std::string()), {});
            Canvas->invalidateGraph();
            runFrame(); runFrame();
            const float Drawn = ImNodes::GetNodeDimensions(N).y;
            const float Est   = Canvas->graph().Nodes[(size_t)N].Height;
            Canvas->removeNode(N);
            Canvas->invalidateGraph();
            runFrame();
            return std::pair<float, float>(Drawn, Est);
        };
        const json Multi = json::array({std::string("a\nb\nc\nd\ne\nf\ng")});
        QStringList outliers;
        const std::vector<std::pair<const char *, json>> Cases = {
            {"DeclareExec", json{{"GUEST", Multi}, {"ARGS", Multi}, {"ENV_REMOVE", Multi}}},
            {"Content",     json{{"SUBMOUNTS", Multi}, {"BASE_TARGETS", Multi}}},
        };
        for (const auto &C : Cases) {
            const auto R = measure(C.first, C.second);
            if (R.second < R.first)
                outliers << QString("%1 with newline-bearing list entries is drawn %2px but estimated %3px - "
                                    "SHORT by %4").arg(C.first).arg(R.first).arg(R.second).arg(R.first - R.second);
        }
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // A combo's popup is a window of its own, positioned by imgui from the widget's rect against the SCREEN
    // viewport — and inside the editor that rect is in world space. The previous review could not drive one
    // open headlessly and said so, which left the claim that in-node combos work at every zoom resting on
    // nothing. This drives one: it walks down the node's field column until a popup actually appears, so it
    // fails loudly if it never manages to open one rather than passing by measuring nothing.
    void anInNodeComboOpensWhereItWasClicked()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 200, 200);          // FORM is an enum, so the node has a combo
        Canvas->setZoom(1.0f);
        runFrame(); runFrame();

        // The popup window imgui makes for a combo.
        auto comboWindow = [&]() -> const ImGuiWindow * {
            const ImGuiContext &C = *ImGui::GetCurrentContext();
            for (int w = 0; w < C.Windows.Size; ++w)
                if (C.Windows[w]->Name && C.Windows[w]->Active && std::strstr(C.Windows[w]->Name, "##Combo"))
                    return C.Windows[w];
            return nullptr;
        };

        QStringList outliers;
        //3x included, and at each zoom the node is driven LOW in the viewport. Both matter: a popup opens
        //BELOW its widget, so only down there does the transform push it past the display edge, and only at a
        //high zoom is the displacement large. Measured on the version that translated popups back on screen:
        //at 3x this is where the dropdown ended up responding 130px away from the sliver it drew in, and a
        //test that stayed at the default position and 2x could not see it.
        for (float z : {1.0f, 0.5f, 2.0f, 3.0f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            {
                float ax = 0, ay = 0, bx = 0, by = 0;
                Canvas->canvasViewport(ax, ay, bx, by);
                const ImVec2 Gp = ImNodes::GetNodeGridSpacePos(0);
                const ImVec2 Dm = ImNodes::GetNodeDimensions(0);
                ImNodes::EditorContextResetPanning(ImVec2(((bx - ax) * 0.35f) / z - Gp.x,
                                                          ((by - ay) * 0.80f) / z - Gp.y - Dm.y * 0.5f));
                runFrame(); runFrame();
            }
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(),
                     qPrintable(QString("zoom %1: the node was culled").arg(z)));
            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            const ImVec2 D = ImNodes::GetNodeDimensions(0);

            // Walk down the node's value column looking for the combo.
            ImVec2 Hit(0, 0);
            const ImGuiWindow *Pop = nullptr;
            for (float f = 0.15f; f < 0.95f && !Pop; f += 0.02f) {
                const ImVec2 Try(vx0 + (P.x + D.x * 0.75f - vx0) * z, vy0 + (P.y + D.y * f - vy0) * z);
                if (Try.x < vx0 || Try.x > vx1 || Try.y < vy0 || Try.y > vy1) continue;
                clickAt(Try);
                runFrame();
                if ((Pop = comboWindow()) != nullptr) Hit = Try;
                else { ImGui::ClearActiveID(); }
            }
            if (!Pop) { outliers << QString("zoom %1: no combo could be opened anywhere down the node").arg(z); continue; }

            // The popup must be AT the thing that was clicked — imgui puts it directly under the widget, so
            // the click point has to be within a widget's height of its top edge and inside it horizontally.
            //The invariant that matters is not where the popup's rectangle is, it is that the popup RESPONDS
            //where it is DRAWN. An earlier version asserted "is it on the display" and drove a fix that
            //translated the pixels and the window rectangle back on screen while leaving the item rects
            //behind — measurably making the dropdown respond in a 13px band 130px away from the sliver it
            //drew in, and the assertion could not see it. So probe it: hover where an entry is painted and
            //require that imgui reports an item under the cursor.
            const ImVec2 Min = Pop->OuterRectClipped.Min, Max = Pop->OuterRectClipped.Max;
            const float DX = std::max(0.0f, std::max(Min.x - Hit.x, Hit.x - Max.x));
            //Either EDGE may be the one against the widget: imgui flips a popup above the cursor when there is
            //no room below, which is exactly what it does for a combo near the bottom of the screen — and
            //driving the node low, which this test now does on purpose, makes that the common case.
            const float DEdge = std::min(std::abs(Min.y - Hit.y), std::abs(Max.y - Hit.y));
            if (DX > 60.0f || DEdge > 60.0f)
                outliers << QString("zoom %1: clicked at (%2,%3) and the popup opened at [%4,%5 .. %6,%7] - "
                                    "%8px off horizontally, nearest edge %9px from the click")
                                .arg(z).arg(Hit.x).arg(Hit.y).arg(Min.x).arg(Min.y).arg(Max.x).arg(Max.y)
                                .arg(DX).arg(DEdge);

            bool Responded = false;
            QString Probed;
            for (float fy = 0.05f; fy < 0.95f && !Responded; fy += 0.05f) {
                const ImVec2 At((Min.x + Max.x) * 0.5f, Min.y + (Max.y - Min.y) * fy);
                if (At.x < vx0 || At.x > vx1 || At.y < vy0 || At.y > vy1) continue;
                runFrame(At); runFrame(At);
                if (Probed.isEmpty()) Probed = QString("%1,%2").arg(At.x).arg(At.y);
                if (ImGui::GetCurrentContext()->HoveredId != 0) Responded = true;
            }
            if (!Responded)
                outliers << QString("zoom %1: the popup is drawn at [%2,%3 .. %4,%5] and nothing in it responds "
                                    "to the cursor anywhere down that band (first probe %6) - it is painted "
                                    "where it cannot be used").arg(z).arg(Min.x).arg(Min.y).arg(Max.x)
                                .arg(Max.y).arg(Probed);
            closeAnyPopup();
        }
        closeAnyPopup();
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // A tooltip raised from inside the editor is placed by imgui from io.MousePos, which in there is the WORLD
    // cursor. The fix hands the real one back for the duration of the call rather than pinning the window with
    // SetNextWindowPos — because pinning makes imgui skip both the flip-to-the-other-side placement and the
    // clamp, so a tip raised near an edge runs off the screen with nothing to pull it back. Both halves are
    // asserted here: it lands near the real cursor, and it stays on screen at the corner.
    void anEditorTooltipLandsAtTheCursorAndStaysOnScreen()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 200, 200);          // Content has action buttons, which carry tooltips
        Canvas->setZoom(1.0f);
        //An ACTIVE item swallows hovering everywhere else, and an earlier test in this suite leaves one behind
        //(the nested-field click test focuses a text box). Without this the search below finds no tooltip
        //anywhere on the node and reports it as a defect, in the full suite only.
        ImGui::ClearActiveID();
        //And the VIEW: panning is canvas state that earlier tests leave wherever they finished, so the node can
        //start off-screen — at which point the search below skips every point as out of viewport and reports
        //"no tooltip anywhere", which is indistinguishable from the defect it is looking for.
        ImNodes::EditorContextResetPanning(ImVec2(0, 0));
        runFrame(); runFrame();
        QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node is not on screen to be hovered");

        auto tipWindow = [&]() -> const ImGuiWindow * {
            const ImGuiContext &C = *ImGui::GetCurrentContext();
            for (int w = 0; w < C.Windows.Size; ++w)
                if (C.Windows[w]->Name && C.Windows[w]->Active && std::strstr(C.Windows[w]->Name, "##Tooltip"))
                    return C.Windows[w];
            return nullptr;
        };
        // Hover along the node's action row until a tooltip actually appears; fail loudly if none ever does.
        auto raiseTip = [&](float z) -> ImVec2 {
            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(0);
            const ImVec2 D = ImNodes::GetNodeDimensions(0);
            for (float fy = 0.05f; fy < 1.0f; fy += 0.02f)
                for (float fx = 0.03f; fx < 0.95f; fx += 0.05f) {
                    const ImVec2 At(vx0 + (P.x + D.x * fx - vx0) * z, vy0 + (P.y + D.y * fy - vy0) * z);
                    if (At.x < vx0 || At.x > vx1 || At.y < vy0 || At.y > vy1) continue;
                    runFrame(At); runFrame(At);
                    if (tipWindow()) return At;
                }
            return ImVec2(-1, -1);
        };

        // TWO checks, at two different setups, because each hides the other's defect.
        //
        // Proximity is checked at 2x with the node CENTRED: there the world cursor is much nearer the canvas
        // origin than the real one, so a tooltip placed from the wrong one lands hundreds of pixels away and
        // imgui has no reason to clamp it back. At 1:1 the two cursors are identical and nothing shows.
        //
        // Staying on screen is checked at 0.5x with the node driven into the BOTTOM-RIGHT corner: only there
        // does imgui have to flip the tooltip to the other side of the cursor, which it will only do when the
        // position is its own to choose. And it must be read from Pos+Size, NOT OuterRectClipped — the latter
        // is clipped to the display by definition, so it reports a tooltip hanging off the edge as ending
        // neatly at it, which is exactly how an earlier version of this test passed the mutation.
        auto panNodeTo = [&](float Z, float FracX, float FracY) {
            float ax = 0, ay = 0, bx = 0, by = 0;
            Canvas->canvasViewport(ax, ay, bx, by);
            const ImVec2 P = ImNodes::GetNodeGridSpacePos(0);
            const ImVec2 D = ImNodes::GetNodeDimensions(0);
            ImNodes::EditorContextResetPanning(ImVec2(((bx - ax) * FracX) / Z - P.x - D.x * FracX,
                                                      ((by - ay) * FracY) / Z - P.y - D.y * FracY));
            runFrame(); runFrame();
        };

        Canvas->setZoom(2.0f);
        runFrame(); runFrame();
        panNodeTo(2.0f, 0.5f, 0.5f);
        QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node left the screen at 2x");
        const ImVec2 At2 = raiseTip(2.0f);
        QVERIFY2(At2.x >= 0.0f, "no action-button tooltip could be raised at 2x");
        {
            const ImGuiWindow *W = tipWindow();
            const ImRect R(W->Pos, ImVec2(W->Pos.x + W->Size.x, W->Pos.y + W->Size.y));
            const float Dist = std::max(std::max(R.Min.x - At2.x, At2.x - R.Max.x),
                                        std::max(R.Min.y - At2.y, At2.y - R.Max.y));
            QVERIFY2(Dist < 90.0f,
                     qPrintable(QString("at 2x the tooltip was raised at (%1,%2) and drawn at [%3,%4 .. "
                                        "%5,%6] - %7px away, so it is following the world cursor")
                                    .arg(At2.x).arg(At2.y).arg(R.Min.x).arg(R.Min.y)
                                    .arg(R.Max.x).arg(R.Max.y).arg(Dist)));
        }
        closeAnyPopup();

        Canvas->setZoom(0.5f);
        runFrame(); runFrame();
        panNodeTo(0.5f, 1.0f, 1.0f);
        QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node left the screen at 0.5x");
        const ImVec2 At3 = raiseTip(0.5f);
        QVERIFY2(At3.x >= 0.0f, "no action-button tooltip could be raised in the corner");
        {
            const ImGuiWindow *W = tipWindow();
            const ImVec2 Disp = ImGui::GetIO().DisplaySize;
            const ImVec2 Max(W->Pos.x + W->Size.x, W->Pos.y + W->Size.y);
            QVERIFY2(W->Pos.x >= -1.0f && W->Pos.y >= -1.0f && Max.x <= Disp.x + 1.0f && Max.y <= Disp.y + 1.0f,
                     qPrintable(QString("the tooltip hangs off the screen: [%1,%2 .. %3,%4] against a %5x%6 "
                                        "display, cursor at (%7,%8)").arg(W->Pos.x).arg(W->Pos.y)
                                    .arg(Max.x).arg(Max.y).arg(Disp.x).arg(Disp.y).arg(At3.x).arg(At3.y)));
        }
        Canvas->setZoom(1.0f);
    }

    // The overview falls back to a node's ESTIMATED height for one it has never measured — which after a
    // document swap is every node, and for a culled one is forever. With a nominal box instead, a graph of
    // wildly different nodes draws as a grid of identical stubs.
    void theOverviewShowsRealSizesForNodesItHasNeverMeasured()
    {
        Canvas->setMiniMap(true);
        // TWIN nodes with identical payloads, therefore identical estimated heights — one left where culling
        // submits it (so the overview uses its MEASURED size) and one far away (so it uses the fallback).
        //
        // That pairing is the only thing that can see this. The overview fits itself to the graph's bounds,
        // and those bounds are computed from the same sizes it draws — so scaling every fallback by a constant
        // moves the bounds and the boxes together and is invisible to any self-relative check. Measured: a
        // fallback of Height * 0.4 left an earlier version of this test, which compared the boxes only to each
        // other, completely green. A measured node and an unmeasured TWIN must get the same box.
        auto reg = [&](float x, float y, int rows) {
            const int N = Canvas->addNode("RegEdit", x, y);
            json K = json::object();
            for (int r = 0; r < rows; ++r) K["Software"]["v" + std::to_string(r)] = "d";
            json E = json::object(); E["HKLM"] = K;
            Doc["NODES"][N]["EDITS"] = json::array({E});
            return N;
        };
        //Close enough that the overview's boxes are TENS of pixels. A box is floored at 1px so a huge graph
        //does not round every node away (pkgcanvas.cpp), and at an 80,000-unit spread the twins measured 1.9px
        //— at which point every undersizing collapses onto that floor and the comparison below can only ever
        //catch OVER-sizing. Spread the graph far enough to cull, no further.
        const int Near = reg(200.0f, 200.0f, 30);        // on screen
        const int Far  = reg(2500.0f, 1800.0f, 30);      // past the cull margin, never submitted
        reg(4000.0f, 2800.0f, 4);                        // a couple more, to give the overview a real spread
        reg(5200.0f, 3600.0f, 60);
        Canvas->invalidateGraph();                       // drop every measured size, as a document swap does
        runFrame(); runFrame();

        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[(size_t)Near].Height, G.Nodes[(size_t)Far].Height);
        QVERIFY2(Canvas->visibleNodes() >= 1 && Canvas->visibleNodes() < Canvas->nodeCount(),
                 qPrintable(QString("%1 of %2 submitted - this needs both a measured node and an unmeasured "
                                    "one").arg(Canvas->visibleNodes()).arg(Canvas->nodeCount())));

        float nx = 0, ny = 0, nw = 0, nh = 0, fx = 0, fy = 0, fw = 0, fh = 0;
        Canvas->miniMapNodeBox(Near, nx, ny, nw, nh);
        Canvas->miniMapNodeBox(Far,  fx, fy, fw, fh);
        //Both must be clear of the 1px floor, or the comparison below is between two clamped numbers and
        //means nothing. This is the assertion that makes the layout above load-bearing.
        QVERIFY2(nh > 4.0f && fh > 1.0f,
                 qPrintable(QString("the overview's boxes are at or near the 1px floor (%1px measured, %2px "
                                    "never-measured) - nothing can be compared at that size").arg(nh).arg(fh)));
        //Scaled by what the overview ACTUALLY used for the near twin — its measured height — not by the
        //estimate. The two differ by exactly the slack the height arms are tuned against, so comparing the
        //boxes directly wires a test about the minimap to the height constants: it showed a 10.7% gap at the
        //correct implementation, which both hid a fallback wrong by up to 13% and turned red on a 1px change
        //to kRowPx, blaming the minimap for it.
        const float NearDrawn = ImNodes::GetNodeDimensions(Near).y;
        QVERIFY2(NearDrawn > 1.0f, "the near twin was not submitted, so its measured height is unavailable");
        const float Expect = nh * (G.Nodes[(size_t)Far].Height / NearDrawn);
        QVERIFY2(std::abs(fh - Expect) <= std::max(1.0f, Expect * 0.06f),
                 qPrintable(QString("the never-measured twin got a %1px box where %2px was due (the measured "
                                    "twin is %3px for a drawn height of %4, and both nodes estimate %5) - the "
                                    "overview is not drawing an unmeasured node at the size the layout gave it")
                                .arg(fh).arg(Expect).arg(nh).arg(NearDrawn).arg(G.Nodes[(size_t)Far].Height)));
        Canvas->setMiniMap(false);
    }

    // A corrupt declared position is refused, and the refusal has to be REPORTED — publish otherwise stamps a
    // computed position over it and changes the package's CID with "stamped POS into N file(s)" as the only
    // trace. But the graph is rebuilt on every cache invalidation, which is every keystroke, so reporting it
    // from there emitted one warning per character typed anywhere in the package: measured at 772 bytes a line
    // (std::to_string(1e300) is 308 digits) and one Diagnostics warning each, per node, per keystroke.
    void aRefusedPositionIsReportedOnceNotPerKeystroke()
    {
        Canvas->setMiniMap(false);
        const int N = Canvas->addNode("Content", 100, 100);
        Doc["NODES"][N]["POS"] = json::array({5.0e9, 5.0e9});
        Canvas->addNode("Content", 500, 100);          // something else to type into
        Canvas->invalidateGraph();

        int Warnings = 0;
        QStringList Lines;
        //RAII. A QCOMPARE/QVERIFY2 expands to `return`, which would skip a ClearLogCallback() at the end and
        //leave this process-global slot holding a lambda that captures two of this method's LOCALS — every
        //later test's warnings then writing into a dead stack frame. The failure path of a test is exactly
        //when that happens, which is exactly when it must not.
        struct Sink { ~Sink() { ClearLogCallback(); } } SinkGuard;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M) {
            if (L == LogLevel::WARN && M.find("no layout could have produced") != std::string::npos)
            { ++Warnings; Lines << QString::fromStdString(M); }
        });
        runFrame(); runFrame();
        QCOMPARE(Warnings, 1);
        // Twenty graph rebuilds. NOTE this is invalidateGraph, the document-REPLACEMENT path — not what a
        // keystroke does (that is MarkDirty, driven below). Both are covered because both rebuild the graph
        // and both therefore re-run the rejection report.
        for (int i = 0; i < 20; ++i) { Canvas->invalidateGraph(); runFrame(); }
        // And twenty keystrokes into the ID BOX of the corrupt node itself, which is the path that actually
        // happens: renameNode runs per character, ends in MarkDirty, and the graph is rebuilt with a new id —
        // so a warning keyed by the old one fires again under the new one, naming ids that never existed.
        std::string Id = Doc["NODES"][N].value("NODE_ID", std::string());
        for (int i = 0; i < 20; ++i) { Id += 'x'; Canvas->renameNode(N, Id); runFrame(); }
        QVERIFY2(Warnings == 1,
                 qPrintable(QString("twenty rebuilds and a twenty-character rename produced %1 warnings, not 1:\n  %2")
                                .arg(Warnings).arg(Lines.join("\n  "))));
        // And the one line has to be usable: it names the node, says which declaration, and does not carry
        // hundreds of digits of whatever the package happened to contain.
        QVERIFY2(Lines[0].contains("its own POS"), qPrintable("the warning does not say WHICH declaration: " + Lines[0]));
        //The bound is what the CODE guarantees, not what this test's short generated id happens to produce:
        //85 chars of fixed wording + 27 for the longest source name + 99 for SafeId's cap and ellipsis + the
        //value, which for the out-of-range arm is two %g numbers. A malformed-shape record is longer again.
        QVERIFY2(Lines[0].size() < 280, qPrintable(QString("the warning is %1 bytes: %2").arg(Lines[0].size()).arg(Lines[0])));
        // The node is laid out rather than left at the impossible coordinate.
        const PkgGraph::Graph G = Canvas->graph();
        QVERIFY2(std::abs(G.Nodes[(size_t)N].X) < 1.0e6f && std::abs(G.Nodes[(size_t)N].Y) < 1.0e6f,
                 qPrintable(QString("the node kept its impossible position (%1,%2)")
                                .arg(double(G.Nodes[(size_t)N].X)).arg(double(G.Nodes[(size_t)N].Y))));
    }

    // A batched entry that is not an OBJECT is malformed content, and this editor is what you open malformed
    // content WITH. Drawing its fields anyway meant the first write — Node[key] = value on a JSON string —
    // threw type_error.305 out of paintGL, which has no catch: one click on a dropdown terminated the app.
    void aMalformedBatchedEntryIsNotDrawnAsFields()
    {
        Canvas->setMiniMap(false);
        const int N = Canvas->addNode("BinaryPatch", 200, 200);
        Doc["NODES"][N]["EDITS"] = json::array({"oops", 42, json::object({{"MODE", "Replace"}})});
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node was culled");
        QVERIFY2(Doc["NODES"][N]["EDITS"][0].is_string(), "the string entry was rewritten");
        // The height estimate agrees with the short form those entries actually draw as.
        const float Est = Canvas->graph().Nodes[(size_t)N].Height;
        const float Drawn = ImNodes::GetNodeDimensions(N).y;
        QVERIFY2(Est >= Drawn && Est - Drawn < 120.0f,
                 qPrintable(QString("a node of malformed entries is drawn %1px and estimated %2px")
                                .arg(Drawn).arg(Est)));
    }

    // The crash itself, DRIVEN rather than hoped for. nlohmann's operator[](string) throws type_error.305 on a
    // value that is not an object, and this editor's purpose is opening packages that are wrong — so every
    // WRITE path that goes through a key has to refuse rather than throw, because the throw leaves frame() and
    // paintGL, which has no catch, and terminates the application.
    //
    // A previous version of this test clicked blindly down the node and asserted "we are still here", which is
    // liveness against an event it never triggered: deleting both guards left it passing. These call the write
    // paths directly, which is the only way to be sure the throw is what is being prevented.
    void everyFieldWriteRefusesAMalformedValueInsteadOfThrowing()
    {
        Canvas->setMiniMap(false);
        QStringList survived;
        auto attempt = [&](const char *What, const std::function<void()> &Write) {
            try { Write(); }
            catch (const std::exception &E)
            { survived << QString("%1: threw %2").arg(What).arg(E.what()); }
        };

        // These call PkgGraph::WriteSubKey — the SAME function the KeyValue and Cover writers in drawField
        // call, not a copy of the pattern and not a hook on the canvas. Reaching a write through synthetic
        // clicks lets a test pass by never getting there (an earlier version did exactly that), and a
        // parallel copy lets the call sites drift; calling the one exported writer does neither.
        // KeyValue: the "+ add" button and a rename both write Node[key][sub].
        for (const char *Bad : {"\"oops\"", "5", "[]", "true", "null"}) {
            const int N = Canvas->addNode("DllOverride", 200, 200);
            Doc["NODES"][N]["OVERRIDES"] = json::parse(Bad);
            Canvas->invalidateGraph(); runFrame();
            attempt(QString("DllOverride OVERRIDES=%1").arg(Bad).toUtf8().constData(),
                    [&] { if (PkgGraph::WriteSubKey(Doc["NODES"][N], "OVERRIDES", "k", "v")) Canvas->invalidateGraph(); });
            Canvas->removeNode(N); Canvas->invalidateGraph(); runFrame();
        }
        // Cover: one KEYSTROKE reached Node[key]["PATH"].
        for (const char *Bad : {"5", "[]", "true"}) {
            const int N = Canvas->addNode("DeclareLibraryItem", 200, 200);
            Doc["NODES"][N]["COVER"] = json::parse(Bad);
            Canvas->invalidateGraph(); runFrame();
            attempt(QString("DeclareLibraryItem COVER=%1").arg(Bad).toUtf8().constData(),
                    [&] { if (PkgGraph::WriteSubKey(Doc["NODES"][N], "COVER", "PATH", "x.png")) Canvas->invalidateGraph(); });
            Canvas->removeNode(N); Canvas->invalidateGraph(); runFrame();
        }
        // A NODES entry that is not an object at all: renameNode and the envelope both write through it.
        {
            Doc["NODES"].push_back("not a node");
            Canvas->invalidateGraph(); runFrame(); runFrame();
            const int Last = (int)Doc["NODES"].size() - 1;
            attempt("a NODES entry that is a string", [&] { Canvas->renameNode(Last, "newid"); });
            attempt("drawing a NODES entry that is a string", [&] { runFrame(); runFrame(); });
            Doc["NODES"].erase(Doc["NODES"].size() - 1);
            Canvas->invalidateGraph(); runFrame();
        }
        QVERIFY2(survived.isEmpty(),
                 qPrintable("a malformed package terminated the editor instead of displaying:\n  "
                            + survived.join("\n  ")));
    }

    // Refusing to THROW is half of it; the other half is refusing to destroy. The same sweep that taught the
    // Cover and KeyValue writers to refuse a malformed value left the two list "+ add" buttons replacing it
    // with an empty array on one click — MarkDirty'd and saved, in the editor you opened to repair the
    // package. Nothing about the value being wrong makes it disposable.
    void addingToAMalformedListRefusesInsteadOfDestroyingIt()
    {
        Canvas->setMiniMap(false);
        QStringList lost;
        auto sweep = [&](const char *Type, const char *Key, const json &Bad) {
            const int N = Canvas->addNode(Type, 300, 200);
            Doc["NODES"][N][Key] = Bad;
            Canvas->invalidateGraph();
            runFrame(); runFrame();
            QVERIFY2(Canvas->visibleNodes() == Canvas->nodeCount(), "the node was culled");
            // Click every row of the node: one of them is the "+ add"/"+ group" button.
            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(N);
            const ImVec2 D = ImNodes::GetNodeDimensions(N);
            for (float fy = 0.05f; fy < 0.99f; fy += 0.02f)
                for (float fx : {0.12f, 0.4f, 0.8f}) {
                    const ImVec2 At(P.x + D.x * fx, P.y + D.y * fy);
                    if (At.x < vx0 || At.x > vx1 || At.y < vy0 || At.y > vy1) continue;
                    ImGui::ClearActiveID();
                    clickAt(At);
                    runFrame();
                    //...AND TYPE. A button destroys on the click; a TEXT FIELD destroys on the first
                    //character, and a click alone only focuses it. The StringList case below is exactly that
                    //shape — a non-array ARGS rendered as an empty box, and one keystroke turned it into
                    //["x"] — so a click-only sweep reported it clean. Harmless where nothing is focused.
                    type("x");
                }
            closeAnyPopup();
            if (Doc["NODES"][N][Key] != Bad)
                lost << QString("%1 %2: %3 became %4").arg(Type).arg(Key)
                            .arg(QString::fromStdString(Bad.dump()))
                            .arg(QString::fromStdString(Doc["NODES"][N][Key].dump()));
            Canvas->removeNode(N);
            Canvas->invalidateGraph();
            runFrame();
        };
        sweep("BinaryPatch", "EDITS", json::object({{"HKLM", json::object({{"Software", "x"}})}}));
        sweep("BinaryPatch", "EDITS", json("a string"));
        sweep("RegEdit",     "EDITS", json::object({{"HKLM", json::object({{"Software", "x"}})}}));
        //A StringList whose value is not a list. ListToText yields "" for it, so this used to draw an empty
        //box that looked unset, and the keystroke above replaced the value. It is now printed and read-only.
        sweep("DeclareExec", "ARGS",  json("not a list"));
        sweep("DeclareExec", "ARGS",  json::object({{"looks", "structured"}}));
        //...and a list of the RIGHT shape holding one entry of the wrong one, which is the likelier case and
        //the one the container check alone misses. ListToText DROPS a non-string entry, so the box showed
        //"a" for ["a",5] and nothing at all for [5]; the keystroke then wrote back what was left and the
        //dropped entry was gone.
        sweep("DeclareExec", "ARGS",  json::array({5}));
        sweep("DeclareExec", "ARGS",  json::array({"a", 5}));
        sweep("DeclareExec", "ARGS",  json::array({"a", json::array({"nested"})}));
        sweep("DeclareExec", "ARGS",  json::array({"a", nullptr}));
        sweep("Content",     "SUBMOUNTS", json::array({json::object({{"k", "v"}})}));
        QVERIFY2(lost.isEmpty(),
                 qPrintable("a click destroyed a malformed value instead of refusing:\n  " + lost.join("\n  ")));
    }

    // The popup extent repoint stands down when the region is too short to hold a dropdown, because below
    // that imgui pins every popup to the extent's corner. Both sides of that comparison are in WORLD units —
    // an earlier version demanded 300 in both axes and described it as the inverse, which switched the whole
    // fix off at 3x on an ordinary canvas: the top of the zoom range, where the misplacement is largest.
    void thePopupExtentFollowsTheEditorAtEveryZoom()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 300, 300);
        QStringList outliers;
        //Several display sizes, including ones small enough that the region alone could not hold a dropdown —
        //that is the case the code has to handle, and an earlier version of this test skipped it with a
        //`continue` and then asked the CODE which branch it had taken, which made the assertion a tautology:
        //deleting the whole guard stayed green. The expectation here is derived from the display and the zoom
        //only, so the code is never consulted about what it should have done.
        for (const ImVec2 Disp : {ImVec2(1400, 900), ImVec2(700, 420), ImVec2(600, 1100)})
        for (float z : {0.5f, 1.0f, 2.0f, 3.0f}) {
            DisplayOverride = Disp;
            Canvas->setZoom(z);
            runFrame(); runFrame();
            float ax = 0, ay = 0, bx = 0, by = 0;
            Canvas->canvasViewport(ax, ay, bx, by);
            const ImVec2 World((bx - ax) / z, (by - ay) / z);
            //The rectangle a popup is placed against must be the editor's own region, ENLARGED where the
            //region alone is too small to hold a dropdown — imgui pins a popup that does not fit to the
            //rectangle's corner, which would put every dropdown in one place regardless of its widget.
            const float WantW = std::max(World.x, 228.0f + 80.0f), WantH = std::max(World.y, 200.0f);
            float ex = 0, ey = 0, ew = 0, eh = 0;
            Canvas->popupExtent(ex, ey, ew, eh);
            if (std::abs(ew - WantW) > 2.0f || std::abs(eh - WantH) > 2.0f)
                outliers << QString("display %1x%2 zoom %3: the region is %4x%5 world units so the extent "
                                    "should be %6x%7, and it is %8x%9")
                                .arg(Disp.x).arg(Disp.y).arg(z).arg(World.x).arg(World.y)
                                .arg(WantW).arg(WantH).arg(ew).arg(eh);
            //And it stays centred on the region, so enlarging it does not also move where popups land.
            const float WantX = ax - (WantW - World.x) * 0.5f, WantY = ay - (WantH - World.y) * 0.5f;
            if (std::abs(ex - WantX) > 2.0f || std::abs(ey - WantY) > 2.0f)
                outliers << QString("display %1x%2 zoom %3: the extent starts at (%4,%5), not (%6,%7)")
                                .arg(Disp.x).arg(Disp.y).arg(z).arg(ex).arg(ey).arg(WantX).arg(WantY);
        }
        DisplayOverride = ImVec2(0, 0);
        Canvas->setZoom(1.0f);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // removeNode's half of the warned-about maintenance. Only the rename path had a test; reverting the
    // delete path to the pre-fix line left the whole suite green.
    void deletingANodeForgetsThatItWasWarnedAbout()
    {
        Canvas->setMiniMap(false);
        const int N = Canvas->addNode("Content", 100, 100);
        const std::string Id = Doc["NODES"][N].value("NODE_ID", std::string());
        Doc["NODES"][N]["POS"] = json::array({5.0e9, 5.0e9});
        Canvas->invalidateGraph();

        int Warnings = 0;
        struct Sink { ~Sink() { ClearLogCallback(); } } SinkGuard;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M) {
            if (L == LogLevel::WARN && M.find("no layout could have produced") != std::string::npos) ++Warnings;
        });
        runFrame(); runFrame();
        QCOMPARE(Warnings, 1);

        // Delete it, then create a DIFFERENT node that happens to reuse the id with the same bad position —
        // which is what opening a second package does, since ids are auto-generated from the type name.
        Canvas->removeNode(N);
        const int M2 = Canvas->addNode("Content", 400, 100);
        Doc["NODES"][M2]["NODE_ID"] = Id;
        Doc["NODES"][M2]["POS"] = json::array({5.0e9, 5.0e9});
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        QVERIFY2(Warnings == 2,
                 qPrintable(QString("the second node's warning was swallowed by the deleted node's entry "
                                    "(%1 warnings, expected 2)").arg(Warnings)));
    }

    // The OTHER producer of a warned-about key. flushPositions refuses an impossible position coming back from
    // imnodes and must, like the Build-rejection path, say it once per node and not suppress a later node's.
    // Both producers write into one set, and an earlier round had them writing two different key shapes so the
    // maintenance covered one and silently ignored the other — reintroducing that is invisible to every other
    // test in this suite, because nothing else drives this path at all.
    void anImpossiblePositionFromTheEditorIsReportedOncePerNode()
    {
        Canvas->setMiniMap(false);
        const int A = Canvas->addNode("Content", 100, 100);
        runFrame(); runFrame();

        int Warnings = 0;
        struct Sink { ~Sink() { ClearLogCallback(); } } SinkGuard;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M) {
            if (L == LogLevel::WARN && M.find("refused an impossible position") != std::string::npos) ++Warnings;
        });

        // Reach past the canvas and put an impossible position into imnodes itself — which is exactly what the
        // guarded minimap drag used to do, and what any future defect in this area would do again.
        for (int i = 0; i < 6; ++i) {
            ImNodes::SetNodeGridSpacePos(A, ImVec2(5.0e9f, 5.0e9f));
            runFrame();
        }
        QVERIFY2(Warnings >= 1, "the impossible position was accepted silently");
        QVERIFY2(Warnings <= 2,
                 qPrintable(QString("six frames produced %1 warnings - it is firing per frame").arg(Warnings)));
        // The node keeps a sane position, and nothing absurd reached the layout.
        const PkgGraph::Graph G = Canvas->graph();
        QVERIFY2(std::abs(G.Nodes[(size_t)A].X) < 1.0e6f, "the impossible position reached the graph");
        QVERIFY2(!QString::fromStdString(Layout.dump()).contains("e+09"), "it reached the saved layout");

        // Now a DIFFERENT node reusing the same id must still be able to warn — the failure mode when the two
        // producers disagree about the key shape is that this one is swallowed.
        const std::string Id = Doc["NODES"][A].value("NODE_ID", std::string());
        Canvas->removeNode(A);
        const int B = Canvas->addNode("Content", 400, 100);
        Doc["NODES"][B]["NODE_ID"] = Id;
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        const int Before = Warnings;
        for (int i = 0; i < 3; ++i) { ImNodes::SetNodeGridSpacePos(B, ImVec2(7.0e9f, 7.0e9f)); runFrame(); }
        QVERIFY2(Warnings > Before,
                 "a second node's impossible position was swallowed by the first node's entry");
    }

    // The malformed-node placeholder returns early, and the two things an early return in drawNode can get
    // wrong are exactly the two it did get wrong: it sat between the colour PUSHES and their POPS, and it
    // skipped the position seeding. Neither is visible in anything the canvas draws, so neither was caught.
    void theMalformedNodePlaceholderBehavesLikeANode()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        Doc["NODES"].push_back("not a node");
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        const int Bad = (int)Doc["NODES"].size() - 1;

        // 1. The colour-style stack must not grow. imnodes never clears it per frame, so three unpopped
        // pushes per frame is unbounded growth for the life of the process — 180/second on an open canvas —
        // and it leaves the style permanently holding the placeholder's colours.
        const int Before = GImNodes->ColorModifierStack.Size;
        for (int i = 0; i < 10; ++i) runFrame();
        const int After = GImNodes->ColorModifierStack.Size;
        QVERIFY2(After == Before,
                 qPrintable(QString("ten frames grew imnodes' colour stack from %1 to %2 - the placeholder is "
                                    "pushing styles it never pops").arg(Before).arg(After)));

        // 2. It must sit where the layout put it. Culling submits it by its LAID-OUT position, so a
        // placeholder drawn at the grid origin instead is a box that vanishes when you pan to where it says
        // it is, and stacks with every other malformed node on top of whatever lives at (0,0).
        const PkgGraph::Graph G = Canvas->graph();
        QVERIFY2(Bad < (int)G.Nodes.size(), "the placeholder is not in the graph");
        const ImVec2 At = ImNodes::GetNodeGridSpacePos(Bad);
        QVERIFY2(std::abs(At.x - G.Nodes[(size_t)Bad].X) < 1.0f
                     && std::abs(At.y - G.Nodes[(size_t)Bad].Y) < 1.0f,
                 qPrintable(QString("the placeholder is drawn at (%1,%2) but laid out at (%3,%4)")
                                .arg(At.x).arg(At.y).arg(G.Nodes[(size_t)Bad].X).arg(G.Nodes[(size_t)Bad].Y)));
        Doc["NODES"].erase(Doc["NODES"].size() - 1);
        Canvas->invalidateGraph();
        runFrame();
    }

    // The canvas borrows imgui's main viewport for the duration of the editor, so that in-node popups place
    // themselves in the space their widgets are laid out in. Every field of it has to come back: Pos and Size
    // feed the backend's projection matrix, and WorkPos/WorkSize are what ImGui::Begin clamps a window
    // against — leaving those two in world space while Pos/Size were screen meant the minimap child and the
    // delete-confirmation modal were placed against two different coordinate systems at once.
    void theEditorHandsTheWholeViewportBack()
    {
        Canvas->setMiniMap(true);
        Canvas->addNode("Content", 300, 300);
        QStringList outliers;
        //Checked at the moment post-editor windows are submitted, NOT after frame() returns: imgui's NewFrame
        //rebuilds the main viewport every frame, so a viewport left in world space is invisible from outside
        //while being exactly what the overview and the delete modal get placed against inside.
        for (float z : {1.0f, 0.5f, 2.0f, 3.0f}) {
            Canvas->setZoom(z);
            runFrame(); runFrame();
            const ImVec2 Disp = ImGui::GetIO().DisplaySize;
            float x = 0, y = 0, w = 0, h = 0, wx = 0, wy = 0, ww = 0, wh = 0;
            Canvas->postEditorViewport(x, y, w, h, wx, wy, ww, wh);
            if (std::abs(w - Disp.x) > 1.0f || std::abs(h - Disp.y) > 1.0f
                || std::abs(x) > 1.0f || std::abs(y) > 1.0f)
                outliers << QString("zoom %1: after the editor the viewport is (%2,%3 %4x%5), not the "
                                    "%6x%7 display").arg(z).arg(x).arg(y).arg(w).arg(h).arg(Disp.x).arg(Disp.y);
            if (std::abs(ww - Disp.x) > 1.0f || std::abs(wh - Disp.y) > 1.0f
                || std::abs(wx) > 1.0f || std::abs(wy) > 1.0f)
                outliers << QString("zoom %1: after the editor the WORK rect is (%2,%3 %4x%5), not the "
                                    "%6x%7 display - ImGui::Begin clamps the overview and the delete modal "
                                    "against this while popups read the main rect")
                                .arg(z).arg(wx).arg(wy).arg(ww).arg(wh).arg(Disp.x).arg(Disp.y);
        }
        Canvas->setZoom(1.0f);
        Canvas->setMiniMap(false);
        QVERIFY2(outliers.isEmpty(), qPrintable("\n  " + outliers.join("\n  ")));
    }

    // A RELOAD that changes a node's declared POS must reach the canvas. invalidateGraph is the one path a
    // document replacement takes — the JSON view's Save, and PackageEditor's LoadNodes — and it did not clear
    // the seed map, so imnodes kept the position from BEFORE the reload. Two frames later flushPositions read
    // that stale origin, saw it differ from the freshly-built graph, and wrote it to the layout sidecar as
    // though the author had dragged the node there: the edit silently reverted AND the old coordinate
    // persisted into GlobalConfig, where it beats the package's own POS for good.
    void aReloadedPositionReachesTheCanvasInsteadOfBeingOverwritten()
    {
        Canvas->setMiniMap(false);
        Doc["NODES"] = json::array({json{{"NODE_ID", "a"}, {"TYPE", "Group"},
                                         {"POS", json::array({100.0, 100.0})}}});
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        QCOMPARE(Canvas->graph().Nodes[0].X, 100.0f);

        // The document is replaced under the canvas, exactly as a JSON-view save does.
        Doc["NODES"][0]["POS"] = json::array({500.0, 400.0});
        Canvas->invalidateGraph();
        runFrame(); runFrame();
        releaseMouse();

        QCOMPARE(Canvas->graph().Nodes[0].X, 500.0f);
        QCOMPARE(Canvas->graph().Nodes[0].Y, 400.0f);
        const ImVec2 Held = ImNodes::GetNodeGridSpacePos(0);
        QVERIFY2(std::abs(Held.x - 500.0f) < 1.0f && std::abs(Held.y - 400.0f) < 1.0f,
                 qPrintable(QString("imnodes still holds the pre-reload position (%1,%2)")
                                .arg(double(Held.x)).arg(double(Held.y))));
        // And nothing was persisted: a reload is not a drag.
        QVERIFY2(Layout.empty() || !Layout.contains("a"),
                 qPrintable("the reload was written to the layout sidecar as if it were a drag: "
                            + QString::fromStdString(Layout.dump())));
    }

    // The crash class, driven through the UI rather than through the helper. everyFieldWriteRefuses... calls
    // PkgGraph::WriteSubKey directly, which pins the helper — but the guard the user meets is the CALL, and a
    // call site can be swapped back to a bare Node[key][sub] with the helper still perfect and every suite
    // still green. That is the predicate-pinned-instead-of-call-sites trap. So: a malformed value, a real
    // click, a real keystroke, and the only thing asserted is that the editor is still alive and the value is
    // untouched — which is exactly what the user gets.
    void aMalformedFieldSurvivesRealClicksAndKeystrokes()
    {
        Canvas->setMiniMap(false);
        QStringList died;

        //Returns whether the sweep CHANGED the value. Run on a well-formed node that is the positive control:
        //if a sweep cannot reach the widget that writes, then "the malformed value was not rewritten" is not
        //evidence of anything, and the mutation that swaps the call site back sails through. Both the KeyValue
        //"+ add" button and the Cover box have to be demonstrably reachable before the refusal means anything.
        auto drive = [&](const char *Type, const char *Key, const json &Bad, bool Typing) {
            const int N = Canvas->addNode(Type, 220, 180);
            Doc["NODES"][N][Key] = Bad;
            Canvas->invalidateGraph();
            ImGui::ClearActiveID();
            ImNodes::EditorContextResetPanning(ImVec2(0, 0));
            runFrame(); runFrame();
            if (Canvas->visibleNodes() != Canvas->nodeCount()) { died << "node culled"; return false; }

            float vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
            Canvas->canvasViewport(vx0, vy0, vx1, vy1);
            const ImVec2 P = ImNodes::GetNodeScreenSpacePos(N);
            const ImVec2 D = ImNodes::GetNodeDimensions(N);
            const QString Who = QString("%1 %2=%3").arg(Type).arg(Key)
                                    .arg(QString::fromStdString(Bad.dump()));
            try {
                // Every row of the node, both columns: one of them is the widget that writes through Key.
                //Absolute x offsets, not fractions: a SmallButton sits at the node's content edge and is ~40px
                //wide, so a fraction of the 346px body steps straight over it. The wider offsets are for the
                //value column, where the text boxes are.
                for (float dy = 4.0f; dy < D.y - 2.0f; dy += 3.0f)
                    for (float dx : {14.0f, 26.0f, 38.0f, 52.0f, 0.45f * D.x, 0.8f * D.x}) {
                        const ImVec2 At(P.x + dx, P.y + dy);
                        if (At.x < vx0 || At.x > vx1 || At.y < vy0 || At.y > vy1) continue;
                        //An item left ACTIVE by an earlier click in this sweep swallows the next press:
                        //ButtonBehavior only takes it when ActiveId is free, and a text box focused two rows
                        //up is still holding it. Without this the sweep never presses a single button, and
                        //"the malformed value was not rewritten" becomes true for the wrong reason.
                        ImGui::ClearActiveID();
                        clickAt(At);
                        runFrame();
                        // A click focused something: type into it. The Cover write happens on the KEYSTROKE,
                        // with no click needed beyond focusing the box, so a click-only sweep never reaches it.
                        if (Typing && ImGui::GetActiveID() != 0) { type("z"); runFrame(); }
                    }
            } catch (const std::exception &E) {
                died << QString("%1: threw %2").arg(Who).arg(E.what());
            }
            closeAnyPopup();
            ImGui::ClearActiveID();
            const bool Changed = !Doc["NODES"][N].contains(Key) || Doc["NODES"][N][Key] != Bad;
            Canvas->removeNode(N);
            Canvas->invalidateGraph();
            runFrame();
            return Changed;
        };

        // The positive controls FIRST: a well-formed value of each shape must be changed by the sweep, which
        // is what proves the sweep reaches the widget that writes through the key.
        if (!drive("DeclareLibraryItem", "COVER", json("cover.png"), true))
            died << "the sweep never reached the Cover box - every Cover result below is vacuous";
        if (!drive("DllOverride", "OVERRIDES", json::object(), false))
            died << "the sweep never reached the KeyValue add button - every OVERRIDES result below is vacuous";

        // Cover: Node[COVER]["PATH"] on every keystroke — no click beyond focusing the box.
        for (const char *Bad : {"5", "[]", "true"})
            if (drive("DeclareLibraryItem", "COVER", json::parse(Bad), true))
                died << QString("DeclareLibraryItem COVER=%1: the malformed value was rewritten").arg(Bad);
        // KeyValue: Node[OVERRIDES][""] on the "+ add" button.
        for (const char *Bad : {"\"oops\"", "5", "[]", "true"})
            if (drive("DllOverride", "OVERRIDES", json::parse(Bad), false))
                died << QString("DllOverride OVERRIDES=%1: the malformed value was rewritten").arg(Bad);

        QVERIFY2(died.isEmpty(),
                 qPrintable("a malformed package did not survive being used:\n  " + died.join("\n  ")));
    }

    void zoomIsClampedAndDefaultsToUnity()
    {
        QCOMPARE(Canvas->zoom(), 1.0f);
        Canvas->setZoom(1000.0f);
        QVERIFY(Canvas->zoom() <= 3.0f);
        Canvas->setZoom(0.0001f);
        QVERIFY(Canvas->zoom() >= 0.2f);
        Canvas->setZoom(1.0f);
        QCOMPARE(Canvas->zoom(), 1.0f);
    }

    void zoomingDoesNotMoveTheStoredPositions()
    {
        Canvas->addNode("Content", 400, 300);
        Canvas->addNode("DeclareExec", 900, 300);
        runFrame();
        const std::string Before = Layout.dump();

        for (float Z : {0.5f, 2.0f, 0.25f, 1.0f})
        {
            Canvas->setZoom(Z);
            runFrame();
            runFrame();   // a second frame: the read-back runs against what the first pushed
        }
        QCOMPARE(Layout.dump(), Before);          // nothing was dragged, only looked at
        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[0].X, 400.0f);
        QCOMPARE(G.Nodes[0].Y, 300.0f);
        QCOMPARE(G.Nodes[1].X, 900.0f);
    }

    // Culling is what makes the 2775-node bundle usable (3753 ms -> 15 ms a frame). It must never drop a node
    // that IS on screen, and a small graph must not be culled at all.
    void everySmallGraphNodeIsStillDrawn()
    {
        //WITH culling on — which it always is now. The minimap is off so nothing it draws can be mistaken
        //for a node the canvas submitted.
        //the whole culling block deleted.
        Canvas->setMiniMap(false);
        for (int I = 0; I < 6; ++I) Canvas->addNode("Content", 60.0f + I * 120.0f, 80.0f);
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 6);
    }

    void aNodeFarOutsideTheViewportIsCulled()
    {
        //Culling is unconditional now; the minimap is off here only so the overview cannot be what a
        //measurement picks up.
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 40, 40);
        Canvas->addNode("Content", 90000, 90000);   // far off-screen at 1.0x
        runFrame();
        QCOMPARE(Canvas->nodeCount(), 2);
        QCOMPARE(Canvas->visibleNodes(), 1);
    }

    // A wire whose ENDPOINTS are both culled but which CROSSES the viewport must still be drawn — you should see
    // the link even when neither node it joins is on screen. Teeth: remove the crossing-link proxy pass → the link
    // is culled with its endpoints and visibleLinks drops to 0.
    void aLinkCrossingTheViewportIsDrawnThoughBothNodesAreCulled()
    {
        Canvas->setMiniMap(false);
        const int p = Canvas->addNode("Content", -90000, -90000);   // far top-left, off-screen
        const int c = Canvas->addNode("DeclareExec", 90000, 90000); // far bottom-right, off-screen
        Doc["NODES"][c]["PARENTS"] = json::array({ Doc["NODES"][p]["NODE_ID"].get<std::string>() });
        Canvas->invalidateGraph();
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 0);   // both nodes are culled...
        QCOMPARE(Canvas->visibleLinks(), 1);   // ...but the wire between them crosses the view, so it is drawn
    }

    // The complement — the geometric cull still WORKS: two culled nodes on the same side, whose wire never enters
    // the viewport, cost nothing to draw. Teeth: make the segment test always-true → this link is drawn (1).
    void aLinkThatMissesTheViewportIsNotDrawn()
    {
        Canvas->setMiniMap(false);
        const int p = Canvas->addNode("Content", 90000, 90000);
        const int c = Canvas->addNode("DeclareExec", 95000, 95000);   // both far bottom-right; wire stays off-screen
        Doc["NODES"][c]["PARENTS"] = json::array({ Doc["NODES"][p]["NODE_ID"].get<std::string>() });
        Canvas->invalidateGraph();
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 0);
        QCOMPARE(Canvas->visibleLinks(), 0);   // wire misses the viewport → correctly culled
    }

    // ...and a culled node must not have its position rewritten. imnodes was never told where it goes, so
    // reading its position back yields the default origin — which is precisely how a layout got zeroed before.
    void aCulledNodeKeepsItsStoredPosition()
    {
        //Culling is unconditional now; the minimap is off here only so the overview cannot be what a
        //measurement picks up.
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 90000, 90000);
        Canvas->addNode("Content", 40, 40);
        runFrame();
        runFrame();
        const PkgGraph::Graph G = Canvas->graph();
        QCOMPARE(G.Nodes[0].X, 90000.0f);
        QCOMPARE(G.Nodes[0].Y, 90000.0f);
    }


private:
    // Press at `from`, move through `steps` intermediate positions to `to`, release. A real drag: imnodes
    // starts an interaction on the press and updates it from the ABSOLUTE cursor on every frame after, so a
    // drag that teleports in one frame exercises none of what a mouse actually does.
    void dragFromTo(ImVec2 from, ImVec2 to, int steps = 6, bool release = true)
    {
        ImGuiIO &io = ImGui::GetIO();
        runFrame(from);
        io.AddMouseButtonEvent(0, true);
        runFrame(from);
        for (int i = 1; i <= steps; ++i)
            runFrame(ImVec2(from.x + (to.x - from.x) * i / steps, from.y + (to.y - from.y) * i / steps));
        if (release) { io.AddMouseButtonEvent(0, false); runFrame(to); }
    }

    // The minimap's own draw list. It is a child window of its own, so what it painted is separable from the
    // canvas — which is the only way to assert that it painted ANYTHING. Returns nullptr when it was not drawn.
    const ImDrawList *miniMapDrawList()
    {
        const ImGuiContext &C = *ImGui::GetCurrentContext();
        for (int w = 0; w < C.Windows.Size; ++w) {
            const ImGuiWindow *W = C.Windows[w];
            if (W->Name && W->Active && std::strstr(W->Name, "##minimap")) return W->DrawList;
        }
        return nullptr;
    }
    int miniMapVertices() { const ImDrawList *D = miniMapDrawList(); return D ? D->VtxBuffer.Size : 0; }

    void clickAt(ImVec2 p)
    {
        ImGuiIO &io = ImGui::GetIO();
        runFrame(p);
        io.AddMouseButtonEvent(0, true);  runFrame(p);
        io.AddMouseButtonEvent(0, false); runFrame(p);
    }
    // Dismiss any open popup (a combo dropdown). Escape is what a person presses, and imgui's own popup
    // handling is what closes it; ClearActiveID does not. An open popup captures hovering for the whole
    // context, which outlives the per-test canvas.
    void closeAnyPopup()
    {
        //A click OUTSIDE it, which is what dismisses a combo for a person and what imgui itself listens for.
        //Escape does not work here: the offscreen harness feeds no keyboard focus to the popup.
        for (int i = 0; i < 6 && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0; ++i)
            clickAt(ImVec2(60.0f, 860.0f));
        runFrame();
        QVERIFY2(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0,
                 "a popup refused to close - it would block hovering for every test after this one");
    }

    // One character per frame: a keystroke is only meaningful once the widget has processed the one before it.
    void type(const char *s)
    {
        ImGuiIO &io = ImGui::GetIO();
        for (; *s; ++s) { io.AddInputCharacter((unsigned int)(unsigned char)*s); runFrame(LastMouse); }
    }

    void runFrame(ImVec2 mouse = ImVec2(400, 300))
    {
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = (DisplayOverride.x > 0.0f) ? DisplayOverride : ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        LastMouse = mouse;
        ImGui::NewFrame();
        Canvas->frame();
        ImGui::Render();
    }

    //A frame with the left button going UP: the canvas only commits a save on release.
    void releaseMouse()
    {
        ImGuiIO &io = ImGui::GetIO();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        runFrame(LastMouse);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        runFrame(LastMouse);
    }

    ImVec2 LastMouse = ImVec2(400, 300);
    //Lets one test drive a display size other than the harness default, to reach a branch that only exists on
    //a small canvas. Reset to (0,0) — meaning "use the default" — by whoever sets it.
    ImVec2 DisplayOverride = ImVec2(0, 0);

private:
    json Doc;
    json Layout;
    PkgCanvas *Canvas = nullptr;
    int Saves = 0;
    int FullSaves = 0;
    int LayoutSaves = 0;
};

QTEST_MAIN(PkgCanvasTest)
#include "test_pkgcanvas.moc"
