// The package canvas, driven headlessly: Dear ImGui needs no GPU to lay out and process input, so the whole
// editing surface — node creation, wiring, payload edits, deletion, position persistence — runs under QTest
// with a synthetic mouse. This is the thing the old tab-and-form editor could never have: its logic lived
// inside QWidget constructors, so there was nothing to call.

#include "pkgcanvas.h"
#include "pkggraph.h"
#include "nodelower.h"
#include "manifestmodel.h"

#include "imgui.h"
#include "imnodes.h"

#include <QtTest>
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
        Canvas = new PkgCanvas(&Doc, [this]{ ++Saves; }, nullptr, &Layout);
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
        //Culling stands down while the minimap is on (it would reduce the overview to the viewport), and a
        //test graph is far below the size that turns the minimap off — so say it explicitly rather than
        //relying on a threshold this test does not control.
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


    // Zoom must scale the node BODY, not just positions and the font: at 0.2x the columns come five times
    // closer, so a box that stayed 330 px wide would overlap its neighbours into one unreadable mass —
    // and zoom-out is the direction the big-bundle case needs.
    void zoomingOutShrinksTheNodeBodyNotJustTheSpacing()
    {
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 100, 100);
        runFrame();
        const ImVec2 Full = ImNodes::GetNodeDimensions(0);

        Canvas->setZoom(0.25f);
        runFrame();
        runFrame();
        const ImVec2 Small = ImNodes::GetNodeDimensions(0);

        QVERIFY2(Small.x < Full.x * 0.6f,
                 qPrintable(QString("node stayed %1 px wide at 0.25x (was %2)").arg(Small.x).arg(Full.x)));
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
        //WITH culling on. Without this the minimap stays enabled at 6 nodes, culling stands down entirely,
        //and the assertion below holds even with OnScreen() returning false for everything — it passed with
        //the whole culling block deleted.
        Canvas->setMiniMap(false);
        for (int I = 0; I < 6; ++I) Canvas->addNode("Content", 60.0f + I * 120.0f, 80.0f);
        runFrame();
        QCOMPARE(Canvas->visibleNodes(), 6);
    }

    void aNodeFarOutsideTheViewportIsCulled()
    {
        //Culling stands down while the minimap is on (it would reduce the overview to the viewport), and a
        //test graph is far below the size that turns the minimap off — so say it explicitly rather than
        //relying on a threshold this test does not control.
        Canvas->setMiniMap(false);
        Canvas->addNode("Content", 40, 40);
        Canvas->addNode("Content", 90000, 90000);   // far off-screen at 1.0x
        runFrame();
        QCOMPARE(Canvas->nodeCount(), 2);
        QCOMPARE(Canvas->visibleNodes(), 1);
    }

    // ...and a culled node must not have its position rewritten. imnodes was never told where it goes, so
    // reading its position back yields the default origin — which is precisely how a layout got zeroed before.
    void aCulledNodeKeepsItsStoredPosition()
    {
        //Culling stands down while the minimap is on (it would reduce the overview to the viewport), and a
        //test graph is far below the size that turns the minimap off — so say it explicitly rather than
        //relying on a threshold this test does not control.
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
    void clickAt(ImVec2 p)
    {
        ImGuiIO &io = ImGui::GetIO();
        runFrame(p);
        io.AddMouseButtonEvent(0, true);  runFrame(p);
        io.AddMouseButtonEvent(0, false); runFrame(p);
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
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        LastMouse = mouse;
        ImGui::NewFrame();
        Canvas->frame();
        ImGui::Render();
    }

    ImVec2 LastMouse = ImVec2(400, 300);

private:
    json Doc;
    json Layout;
    PkgCanvas *Canvas = nullptr;
    int Saves = 0;
};

QTEST_MAIN(PkgCanvasTest)
#include "test_pkgcanvas.moc"
