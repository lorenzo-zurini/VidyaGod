// Tests for PackageCatalog: hydration predicates (NodeHasContent / NodeHydrated — the #2 content-guard), the
// remove-flow DehydrateNode (#8/#9), per-package user settings, and runner predicates. Builds real temp bundles on
// disk so the layer-presence checks have something to resolve. IPFS node is not started, so DehydrateNode's
// unpin/drop-ref calls are graceful no-ops (we assert the filesystem side).

#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>

#include "packagecatalog.h"
#include "pkggraph.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "nodefixture.h"
#include "cli/climodes.h"
#include "runnerinstall.h"
#include "ipfswrapper.h"
#include "downloadqueue.h"   // EnqueueBatch/WaitBatch — the REAL rolling queue under test
#include "apppaths.h"
#include "commonutils.h"

#include <filesystem>
#include <fstream>
#include <set>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

namespace {
void writeJson(const QString & path, const json & j)
{
    std::ofstream f(path.toStdString());
    f << j.dump(2);
}
void writeFile(const QString & path, const std::string & content = "x")
{
    std::ofstream f(path.toStdString());
    f << content;
}
}

class PackageCatalogTest : public QObject
{
    Q_OBJECT
private slots:

    // IPNS subscribe: a FRIEND source ({CID:"/ipns/<name>"}) resolves to ONE index doc holding libraries inline, each
    // with per-package dehydrated CIDs; SyncPackageSources mirrors each package + upserts LIBRARY. The CID-diff must
    // skip an unchanged package on re-sync and RE-FETCH a changed one. Drives the whole flow through injected hooks
    // (no live network). Teeth: (a) resolve→"" ⇒ nothing mirrored (the resolve step is load-bearing); (b) a changed
    // package CID re-fetches while an unchanged one does not (the diff is real).
    void ipns_source_mirrors_and_diffs()
    {
        QTemporaryDir Ud; QVERIFY(Ud.isValid());
        json Cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", Ud.path().toStdString()}}},
                                       {"PackageSources", json::array({
                                           json{{"CID","/ipns/testfriend"},{"NAME","Friend"},{"FRIEND",true}} })}}}};

        // Reset the injected hooks on ANY exit — a QVERIFY/QCOMPARE failure `return`s from the slot, so a manual reset
        // at the end would leak the hooks into later tests. qScopeGuard fires regardless.
        auto HookGuard = qScopeGuard([]{ IpfsWrapper::SetIpnsResolveHook({}); IpfsWrapper::SetFetchOnceHook({}); });

        IpfsWrapper::SetIpnsResolveHook([](const std::string & Name, std::string *) {
            // accept both "testfriend" and "/ipns/testfriend"
            return (Name == "testfriend" || Name == "/ipns/testfriend") ? std::string("TOPCID_v1") : std::string();
        });

        // Which package CID the index advertises this run (flip to simulate an update), + a fetch counter per CID.
        std::string PkgCid = "PKGCID_A";
        std::map<std::string,int> Fetches;
        IpfsWrapper::SetFetchOnceHook([&](const std::string & Cid, const std::string & Dest, bool Dir, std::string *Err) -> int {
            Fetches[Cid]++;
            if (!Dir)   // a single-file index fetch (top index doc)
            {
                json Index = json{{"publisher","testfriend"},{"nick","Friend"},
                    {"libraries", json::array({ json{{"name","Games"},{"packages", json::array({
                        json{{"packageUID","pkg1"},{"cid",PkgCid},{"title","Game One"}} })}} })}};
                writeFile(QString::fromStdString(Dest), Index.dump(2));
                return 0;
            }
            // a package meta-CID: materialize a minimal valid dehydrated bundle (tile + launchable).
            std::filesystem::create_directories(Dest);
            json Nodes = json::array({
                json{{"NODE_ID","pkg1_tile"},{"TYPE","DeclareLibraryItem"},{"UID","pkg1"},{"TITLE","Game One"},{"PARENTS",json::array()}},
                json{{"NODE_ID","pkg1_exec"},{"TYPE","DeclareExec"},{"HOST","linux64"},{"PATH","run.sh"},{"PARENTS",json::array({"pkg1_tile"})}} });
            writeFile(QString::fromStdString(Dest) + "/pkg1.json", Nodes.dump(2));
            (void)Err;
            return 0;
        });

        // First sync → the package is mirrored + indexed.
        const int Indexed = PackageCatalog::SyncPackageSources(Cfg);
        QCOMPARE(Indexed, 1);
        QVERIFY(Cfg.contains("LIBRARY") && Cfg["LIBRARY"].is_array() && !Cfg["LIBRARY"].empty());
        const auto & E = Cfg["LIBRARY"][0];
        QCOMPARE(E.value("PACKAGEUID", std::string()), std::string("pkg1"));
        QCOMPARE(E.value("CIDSOURCE",  std::string()), std::string("PKGCID_A"));
        // The source dir is keyed by the peerID (_friend_<peer>), NOT the attacker-chosen nick — so it can't collide
        // with your own collections. The mirror entry is SOURCE-tagged so the static upsert never captures it.
        QVERIFY2(E.value("PATH", std::string()).find("_friend_") != std::string::npos,
                 "mirrored under the peerID-keyed friend source dir");
        QVERIFY2(!E.value("SOURCE", std::string()).empty(), "mirror entry must be SOURCE-tagged");
        const int FetchesA = Fetches["PKGCID_A"];
        QVERIFY(FetchesA >= 1);

        // Re-sync, SAME package CID → the diff skips the re-fetch (still one launchable indexed).
        QCOMPARE(PackageCatalog::SyncPackageSources(Cfg), 1);
        QCOMPARE(Fetches["PKGCID_A"], FetchesA);   // no additional fetch of the unchanged package

        // Simulate hydrated content (a large non-.json layer file the launcher wrote into the bundle) before the update.
        const QString PkgPath = QString::fromStdString(Cfg["LIBRARY"][0].value("PATH", std::string()));
        writeFile(PkgPath + "/content.bin", "HYDRATED-GAME-BYTES");

        // Now the friend publishes an update: the index advertises a NEW package CID → re-fetch exactly that one...
        PkgCid = "PKGCID_B";
        QCOMPARE(PackageCatalog::SyncPackageSources(Cfg), 1);
        QVERIFY2(Fetches["PKGCID_B"] >= 1, "a changed package CID must be re-fetched (the update path)");
        QCOMPARE(Cfg["LIBRARY"][0].value("CIDSOURCE", std::string()), std::string("PKGCID_B"));
        // ...and the hydrated content survives the swap (a metadata update must NOT force a multi-GB re-download).
        // Teeth: drop the non-.json carry-over in the swap and this file is gone after the update.
        QVERIFY2(std::filesystem::exists((PkgPath + "/content.bin").toStdString()),
                 "hydrated content must be carried across a package update, not discarded");

        // Teeth: a failed resolve mirrors nothing (the resolve step is load-bearing, not incidental).
        json Cfg2 = json{{"Settings", {{"Paths", {{"UserDataRoot", Ud.path().toStdString()}}},
                                        {"PackageSources", json::array({
                                            json{{"CID","/ipns/unknownfriend"},{"FRIEND",true}} })}}}};
        IpfsWrapper::SetIpnsResolveHook([](const std::string &, std::string * Err) { if (Err) *Err = "no record"; return std::string(); });
        QCOMPARE(PackageCatalog::SyncPackageSources(Cfg2), 0);
        QVERIFY(!Cfg2.contains("LIBRARY") || Cfg2["LIBRARY"].empty());
        // hooks reset by HookGuard on scope exit
    }

    // Mint stamps SOURCE.SIZE (payload bytes) beside the CID for a FILE layer, a DIRECTORY layer (recursive sum),
    // and a COVER; and BACKFILLS it on re-mint for a package that already carries a valid CID but no size, via the
    // idempotent branch (CID unchanged — the branch runs precisely because NeedsSeed's VerifyCid passed, so the
    // bytes were not re-added). SIZE is what lets a fetch show a real % and a pre-fetch total instantly. Teeth:
    // drop `Src["SIZE"]=LocalPayloadSize(Local)` at the seed site → the file/dir QCOMPAREs fail; drop the cover
    // stamp → the cover QCOMPARE fails; drop the idempotent-backfill branch → the re-mint QCOMPARE fails.
    void publish_stamps_and_backfills_source_size()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());   // offline (ctest env)

        QTemporaryDir Pkg; QVERIFY(Pkg.isValid());
        // (a) a plain FILE layer,
        writeFile(Pkg.path() + "/game.zip", std::string(4096, 'Z'));
        writeJson(Pkg.path() + "/game.json", NodeFixture::Chain("game", {NodeFixture::Content("zip", "game.zip")}));
        // (b) a DIRECTORY layer (SIZE must be the recursive sum: 1000 + 2000 = 3000),
        QDir().mkpath(Pkg.path() + "/datadir");
        writeFile(Pkg.path() + "/datadir/a.bin", std::string(1000, 'A'));
        writeFile(Pkg.path() + "/datadir/b.bin", std::string(2000, 'B'));
        writeJson(Pkg.path() + "/data.json", NodeFixture::Chain("data", {NodeFixture::Content("dir", "datadir")}));
        // (c) a tile with a COVER.
        writeFile(Pkg.path() + "/cover.png", std::string(777, 'C'));
        writeJson(Pkg.path() + "/tile.json", json::array({ json{
            {"NODE_ID", "tile"}, {"TYPE", "DeclareLibraryItem"}, {"UID", "9001"},
            {"COVER", {{"PATH", "cover.png"}}} } }));

        // The SOURCE of the (only) content node in a fragment file, or the COVER's SOURCE for the tile.
        auto srcIn = [&](const QString & file, bool cover) -> json {
            std::ifstream in((Pkg.path() + "/" + file).toStdString());
            const json Frag = json::parse(in, nullptr, false);
            if (Frag.is_array())
                for (const auto & N : Frag) {
                    if (!N.is_object()) continue;
                    if (cover) { if (N.contains("COVER") && N["COVER"].is_object() && N["COVER"].contains("SOURCE")) return N["COVER"]["SOURCE"]; }
                    else if (N.contains("SOURCE") && N["SOURCE"].is_object()) return N["SOURCE"];
                }
            return json::object();
        };

        // 1) Fresh mint → CID + SIZE stamped on every content path.
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        const json File = srcIn("game.json", false), Dir = srcIn("data.json", false), Cov = srcIn("tile.json", true);
        QVERIFY2(!File.value("CID", std::string()).empty(), "mint must stamp a CID");
        QCOMPARE((qulonglong)File.value("SIZE", (qulonglong)0), (qulonglong)4096);
        QCOMPARE((qulonglong)Dir.value("SIZE",  (qulonglong)0), (qulonglong)3000);   // recursive dir sum
        QCOMPARE((qulonglong)Cov.value("SIZE",  (qulonglong)0), (qulonglong)777);    // cover
        const std::string Cid = File.value("CID", std::string());

        // 2) Simulate a pre-SIZE package: strip the file layer's SIZE, re-mint → backfilled, CID UNCHANGED (the
        //    idempotent branch stamped SIZE without re-adding the bytes).
        {
            std::ifstream in((Pkg.path() + "/game.json").toStdString());
            json Frag = json::parse(in);
            for (auto & N : Frag)
                if (N.is_object() && N.contains("SOURCE") && N["SOURCE"].is_object()) N["SOURCE"].erase("SIZE");
            writeJson(Pkg.path() + "/game.json", Frag);
        }
        QVERIFY2(!srcIn("game.json", false).contains("SIZE"), "precondition: SIZE stripped");
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        const json S2 = srcIn("game.json", false);
        QCOMPARE((qulonglong)S2.value("SIZE", (qulonglong)0), (qulonglong)4096);     // backfilled
        QCOMPARE(S2.value("CID", std::string()), Cid);                               // same bytes → same CID

        IpfsWrapper::StopNode();
    }

    // The share list is driven by the PUBLISH flag, decoupled from node type: a game (launchable exec), a runner
    // (GUEST-bearing exec) AND a no-exec library head all publish when flagged; an unflagged launchable never does.
    // Grouping is by the collection dir. Teeth: the OLD predicate ("DeclareExec && !GUEST") dropped the runner and the
    // library head and published the unflagged game — every QCOMPARE below fails under it.
    void publish_share_list_is_driven_by_the_PUBLISH_flag()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());

        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        QDir().mkpath(R + "/VidyaGod/[2] Unshared");
        QDir().mkpath(R + "/VidyaGodRunners/wine");
        QDir().mkpath(R + "/VidyaGodLibraries/dgvoodoo");

        // A shared game: tile + a launchable exec flagged PUBLISH.
        writeJson(R + "/VidyaGod/[1] Game/tile.json", NodeFixture::Chain("g_tile", {NodeFixture::Tile("1", "Game")}));
        writeJson(R + "/VidyaGod/[1] Game/game.json",
                  NodeFixture::Chain("g_exec", {NodeFixture::Exec("win32", "g.exe")}, {"g_tile"}, {{"PUBLISH", true}}));
        // A runner: a GUEST-bearing exec flagged PUBLISH (the old predicate EXCLUDED this).
        writeJson(R + "/VidyaGodRunners/wine/wine.json",
                  NodeFixture::Chain("r_wine", {NodeFixture::Runner("linux", {"win32"}, "wine")}, {}, {{"PUBLISH", true}}));
        // A library head: NO exec at all, flagged PUBLISH (the old predicate EXCLUDED this — not a DeclareExec).
        writeJson(R + "/VidyaGodLibraries/dgvoodoo/dg.json",
                  NodeFixture::Chain("l_dg", {NodeFixture::Content("zip", "dgvoodoo.zip")}, {}, {{"PUBLISH", true}}));
        // An UNSHARED launchable: a valid game exec but NOT flagged (the old predicate would have published it).
        writeJson(R + "/VidyaGod/[2] Unshared/u.json",
                  NodeFixture::Chain("u_exec", {NodeFixture::Exec("win32", "u.exe")}));

        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        const std::vector<std::string> Published = PackageCatalog::PublishLibrary(cfg, &Err);

        QVERIFY2(cfg.contains("Libraries") && cfg["Libraries"].is_object(), "PublishLibrary writes config[\"Libraries\"]");
        const auto & Libs = cfg["Libraries"];
        QVERIFY2(Libs.contains("VidyaGod"),          "a flagged game publishes under its collection");
        QVERIFY2(Libs.contains("VidyaGodRunners"),   "a flagged GUEST-bearing runner publishes (old predicate dropped it)");
        QVERIFY2(Libs.contains("VidyaGodLibraries"), "a flagged no-exec library head publishes (old predicate dropped it)");
        QCOMPARE((int)Libs["VidyaGod"].size(), 1);          // only the flagged game — not the unshared exec
        QCOMPARE((int)Libs["VidyaGodRunners"].size(), 1);
        QCOMPARE((int)Libs["VidyaGodLibraries"].size(), 1);
        QCOMPARE((int)cfg["PublishedList"].size(), 3);      // exactly the three flagged roots
        QCOMPARE((int)Published.size(), 3);                 // return value == the share list

        // Each entry carries the RECEIVER's routing metadata — enough to compute the final library path pre-fetch.
        const auto & G = Libs["VidyaGod"][0];
        QVERIFY2(!G.value("cid", std::string()).empty(), "entry names its node-block CID");
        QCOMPARE(G.value("node", std::string()),  std::string("g_exec"));
        QCOMPARE(G.value("uid", std::string()),   std::string("1"));       // from the LIBRARYITEM tile
        QCOMPARE(G.value("title", std::string()), std::string("Game"));
        QVERIFY2(!G.value("tilecid", std::string()).empty(), "the tile block rides along");
        QCOMPARE(G.value("tilenode", std::string()), std::string("g_tile"));
        const auto & W = Libs["VidyaGodRunners"][0];
        QCOMPARE(W.value("node", std::string()),  std::string("r_wine"));
        QCOMPARE(W.value("uid", std::string()),   std::string("r_wine"));  // no tile -> stands under its own handle
        QVERIFY2(!W.contains("tilecid"), "a tile-less node ships no tile fields");

        IpfsWrapper::StopNode();
    }

    // Helper: publish two distinct PUBLISH'd games in one collection and return their two share entries
    // ({cid,node,uid,title,tilecid,tilenode} routing objects, as PublishLibrary emits them).
    json publishTwoGames(const QString & root)
    {
        std::string Err;
        QDir().mkpath(root + "/VidyaGod/[1] A");
        QDir().mkpath(root + "/VidyaGod/[2] B");
        writeJson(root + "/VidyaGod/[1] A/tile.json", NodeFixture::Chain("a_tile", {NodeFixture::Tile("1", "A")}));
        // Real games carry content in PARENT nodes; a share ships only exec+tile, so a receiver's copy has an
        // INCOMPLETE closure until install — exactly what the vacuous-hydration guard must classify as NOT hydrated.
        writeJson(root + "/VidyaGod/[1] A/content.json",
                  NodeFixture::Chain("a_content", {NodeFixture::ContentCid("File", "data.bin",
                      "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e")}));
        writeJson(root + "/VidyaGod/[1] A/a.json",
                  NodeFixture::Chain("a_exec", {NodeFixture::Exec("win32", "a.exe")}, {"a_content", "a_tile"}, {{"PUBLISH", true}}));
        writeJson(root + "/VidyaGod/[2] B/tile.json", NodeFixture::Chain("b_tile", {NodeFixture::Tile("2", "B")}));
        writeJson(root + "/VidyaGod/[2] B/b.json",
                  NodeFixture::Chain("b_exec", {NodeFixture::Exec("win32", "b.exe")}, {"b_tile"}, {{"PUBLISH", true}}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", root.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder, &Err);
        return seeder["Libraries"].contains("VidyaGod") ? seeder["Libraries"]["VidyaGod"] : json::array();
    }

    // A received share goes STRAIGHT to the library: PlanReceivedFetches turns the snapshot's routing metadata into
    // rolling-queue targets whose dests ARE the final tree paths ("<Nick> - <Lib>/[uid] Title/<node>.json"); the queue
    // lands each node block VERBATIM at its dest (simulated below), and from there zero friend-specific code runs —
    // the plain catalog scan indexes the raw dag-json (link normalization at scan), CID identity survives, and the
    // vacuous-hydration guard routes the un-installed package to the Catalog tab. Teeth: break the dest layout, the
    // tile ride-along, scan-time NormalizeLinks (CID identity dies), the hydration guard, or planner idempotency and
    // the asserts below fail. Hostile uid/title/node must never escape the library root.
    void received_share_lands_at_final_library_paths()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());   // blocks now in the local blockstore
        QCOMPARE((int)Items.size(), 2);

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", RxData.path().toStdString()}}}}}};

        // Plan: every node + its tile gets a FINAL library dest, computed pre-fetch from the snapshot alone.
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 4);                          // 2 roots + their 2 tiles
        const std::string Lib = RxData.path().toStdString() + "/Alice - Games";
        std::set<std::string> Dests;
        for (const auto & T : Plan) Dests.insert(T.Dest);
        QVERIFY2(Dests.count(Lib + "/[1] A/a_exec.json"), "root dest = NODE_ID-named file in its [uid] Title dir");
        QVERIFY2(Dests.count(Lib + "/[1] A/a_tile.json"), "the tile rides along into the SAME package dir");
        QVERIFY2(Dests.count(Lib + "/[2] B/b_exec.json"), "second game gets its own package dir");
        QVERIFY2(Dests.count(Lib + "/[2] B/b_tile.json"), "…with its tile");

        // Land the blocks exactly as the rolling queue does: the block's RAW dag-json bytes, verbatim, at the dest.
        for (const auto & T : Plan)
        {
            const auto Blk = IpfsWrapper::DagGetManyLocal({T.Cid});
            const auto It = Blk.find(T.Cid);
            QVERIFY2(It != Blk.end(), "published block must be locally readable");
            std::filesystem::create_directories(std::filesystem::path(T.Dest).parent_path());
            std::ofstream Out(T.Dest, std::ios::binary);
            Out << It->second;
        }

        // The ORDINARY catalog scan indexes them — raw dag-json links normalize at scan, so the node freezes back to
        // the SAME CID (identity survives the wire; a re-publish re-mints identically — the multi-seeder design).
        auto Idx = PackageCatalog::BuildCatalogIndex(rx);
        const std::string Cid0 = Items[0].value("cid", std::string());
        auto It = Idx.Nodes.find(Cid0);
        QVERIFY2(It != Idx.Nodes.end(), "a landed raw block freezes back to the SAME CID in the plain tree scan");
        QVERIFY2(!It->second.BundleDir.empty(), "it is an ordinary tree node with a real bundle dir");

        // Vacuous-hydration guard: an un-installed received game (PARENTS not fetched) must read NOT hydrated —
        // that is what routes it to the Catalog tab (downloadable) instead of the Library tab (playable).
        const auto Hyd = PackageCatalog::HydrationMap(Idx);
        const auto Hit = Hyd.find(It->second.Key());
        QVERIFY2(Hit != Hyd.end() && !Hit->second.Hydrated,
                 "a received, un-installed package must not count as hydrated (its closure is not fetched)");

        // A HOSTILE snapshot (traversal in every routed field) must stay inside the library root.
        const json Evil = json::array({json{{"cid", Cid0}, {"node", "../../pwn"}, {"uid", ".."}, {"title", "../.."},
                                            {"tilecid", Items[0].value("tilecid", std::string())}, {"tilenode", "/etc/passwd"}}});
        const auto EvilPlan = PackageCatalog::PlanReceivedFetches(rx, "..", json{{"..", Evil}});
        QVERIFY(!EvilPlan.empty());
        const std::string RootPrefix = RxData.path().toStdString() + "/";
        for (const auto & T : EvilPlan)
        {
            const std::string Canon = std::filesystem::weakly_canonical(T.Dest).string();
            QVERIFY2(Canon.rfind(RootPrefix, 0) == 0, ("hostile dest escaped the library root: " + Canon).c_str());
        }

        // Planner dedupe: the same snapshot plans the same 4 targets again (stable), never duplicates within one plan.
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan2.size(), 4);
        std::set<std::string> D2; for (const auto & T : Plan2) D2.insert(T.Dest);
        QCOMPARE(D2.size(), Plan2.size());

        // RE-PUBLISH (multi-seeder): the received library re-mints to IDENTICAL CIDs and re-shares with the SAME
        // routing metadata — which requires the raw-landed docs to read as ordinary tree nodes (scan-time link
        // normalization: LIBRARYITEM must come back as a plain CID string for the tile uid/title lookup). Teeth for
        // NormalizeLinks-at-scan and for PublishLibrary's LIBRARYITEM-as-CID reverse lookup: drop either and the
        // uid/title/tile asserts fail; a drifting re-mint breaks the cid equality.
        std::string PubErr;
        PackageCatalog::PublishLibrary(rx, &PubErr);
        QVERIFY2(rx.contains("Libraries") && rx["Libraries"].contains("Alice - Games"),
                 "the received library re-publishes under its own dir name");
        const auto & Re = rx["Libraries"]["Alice - Games"];
        QCOMPARE((int)Re.size(), 2);
        const auto & R0 = Re[0];                                   // handle order: a_exec first
        QCOMPARE(R0.value("cid", std::string()),  Cid0);           // identical re-mint — the multi-seeder design
        QCOMPARE(R0.value("node", std::string()), std::string("a_exec"));
        QCOMPARE(R0.value("uid", std::string()),  std::string("1"));
        QCOMPARE(R0.value("title", std::string()), std::string("A"));
        QCOMPARE(R0.value("tilenode", std::string()), std::string("a_tile"));
        QCOMPARE(R0.value("tilecid", std::string()), Items[0].value("tilecid", std::string()));

        IpfsWrapper::StopNode();
    }

    // A RE-PUBLISHED node (same NODE_ID → same dest, NEW cid) must land OVER the occupied dest through the REAL
    // rolling queue — the receiver's whole update path. Two independent refusals used to eat it silently: the C++
    // queue marked a new-CID job Done on PathExists(dest) alone, and Go's fetchToPathOnce no-op'd on "dest present,
    // no .part" without checking the node holds the requested CID. Teeth: revert either gate and the v2-bytes
    // assert fails (the dest keeps v1 forever). Landing v1 through the real queue also keeps this test honest about
    // what the queue actually does with an empty dest (no hand-written simulation).
    void republished_node_lands_over_the_stale_dest()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QCOMPARE((int)Items.size(), 2);
        const std::string CidV1 = Items[0].value("cid", std::string());

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", RxData.path().toStdString()}}}}}};
        // TWO libraries carry the same items (the multi-dest normal: one CID job, several dests) — a re-publish
        // must refresh EVERY dest, not just the fetch's primary.
        auto Land = [&](const json & SnapItems) {
            std::vector<IpfsWrapper::FetchTarget> B;
            for (const auto & T : PackageCatalog::PlanReceivedFetches(
                     rx, "Alice", json{{"Games", SnapItems}, {"Favs", SnapItems}}))
                B.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, /*Optional=*/false, /*Dir=*/false,
                                                      /*Verify=*/true });   // receiver semantics: reusable dests
            const auto H = IpfsWrapper::EnqueueBatch(B);
            std::string WErr;
            QVERIFY2(IpfsWrapper::WaitBatch(H, 30000, &WErr), WErr.c_str());
        };
        auto DestBytes = [&](const std::string & Lib, const std::string & Node) {
            std::ifstream In(RxData.path().toStdString() + "/Alice - " + Lib + "/[1] A/" + Node + ".json", std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        };
        Land(Items);
        QCOMPARE(DestBytes("Games", "a_exec"), IpfsWrapper::DagGetManyLocal({CidV1})[CidV1]);   // v1 landed verbatim
        QCOMPARE(DestBytes("Favs", "a_exec"),  IpfsWrapper::DagGetManyLocal({CidV1})[CidV1]);   // …into BOTH libraries

        // Seeder updates the node (same NODE_ID, new content) and re-publishes → NEW cid, SAME dest.
        {
            std::ifstream In((SeedRoot.path() + "/VidyaGod/[1] A/a.json").toStdString());
            json J; In >> J; In.close();
            auto Bump = [](json & N) { if (N.value("NODE_ID", std::string()) == "a_exec") N["ENV"] = json{{"V2", "yes"}}; };
            if (J.is_array()) { for (auto & N : J) Bump(N); } else Bump(J);   // fixture files may hold a node ARRAY
            std::ofstream Out((SeedRoot.path() + "/VidyaGod/[1] A/a.json").toStdString());
            Out << J.dump(2);
        }
        json seeder2 = json{{"Settings", {{"Paths", {{"LibraryRoot", SeedRoot.path().toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder2, &Err);
        json Items2 = seeder2["Libraries"]["VidyaGod"];
        std::string CidV2;
        for (const auto & E : Items2) if (E.value("node", std::string()) == "a_exec") CidV2 = E.value("cid", std::string());
        QVERIFY2(!CidV2.empty() && CidV2 != CidV1, "the update re-minted to a NEW cid");

        Land(Items2);
        QCOMPARE(DestBytes("Games", "a_exec"), IpfsWrapper::DagGetManyLocal({CidV2})[CidV2]);   // v2 REPLACED the stale file
        QCOMPARE(DestBytes("Favs", "a_exec"),  IpfsWrapper::DagGetManyLocal({CidV2})[CidV2]);   // …at EVERY dest, not just
                                                                                                // the fetch's primary

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // UNTRUSTED-input bounds of the planner, with teeth for each: (1) every dest path SEGMENT is capped under
    // NAME_MAX even when nick/lib/uid/title/node arrive at the wire maximums (else the queue retries an
    // ENAMETOOLONG mkdir forever); (2) an over-long cid is dropped; (3) the per-library item cap holds.
    void planner_bounds_hostile_segments_and_counts()
    {
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", "/tmp/vgplanb"}}}}}};
        const std::string Long(1000, 'x');
        const json Items = json::array({ json{{"cid", "bafyplannerboundscidaaaaaaaaaaaaaaaaaaaaaa"},
                                              {"node", Long}, {"uid", Long}, {"title", Long}} });
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, Long, json{{Long, Items}});
        QCOMPARE((int)Plan.size(), 1);
        for (const auto & T : Plan)
            for (const auto & Part : std::filesystem::path(T.Dest.substr(std::string("/tmp/vgplanb/").size())))
                QVERIFY2(Part.string().size() <= 130, ("segment too long: " + Part.string()).c_str());

        const json BadCid = json::array({ json{{"cid", Long}} });                    // >128 bytes → not a CID → dropped
        QCOMPARE((int)PackageCatalog::PlanReceivedFetches(rx, "A", json{{"L", BadCid}}).size(), 0);

        json Many = json::array();
        for (int i = 0; i < 20001; ++i) Many.push_back(json{{"cid", "bafycap" + std::to_string(i)}});
        QCOMPARE((int)PackageCatalog::PlanReceivedFetches(rx, "A", json{{"L", Many}}).size(), 20000);
    }

    // Publish-side bounds + the tile race: a node NAMING a tile that is not in the tree (a received share whose tile
    // hasn't landed) is HELD OUT of the share list — emitting it would ship uid/title = the bare handle and fork
    // receivers' trees into "[handle] handle/" dirs that never reconcile. And an over-long user TITLE is truncated at
    // emit (UTF-8-safe), or one title would make the receiver's inbound gate reject the WHOLE snapshot — silently,
    // at the far end. Teeth: drop the hold-back → HeldGame appears with uid "h_exec"; drop the cap → title is 500.
    void publish_holds_unresolved_tiles_and_bounds_titles()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Held");
        QDir().mkpath(R + "/VidyaGod/[2] Long");
        writeJson(R + "/VidyaGod/[1] Held/h.json",
                  json{{"NODE_ID", "h_exec"}, {"TYPE", "DeclareExec"}, {"HOST", "win32"}, {"EXECUTABLE", "h.exe"},
                       {"LIBRARYITEM", "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e"}, {"PUBLISH", true}});
        writeJson(R + "/VidyaGod/[2] Long/tile.json", NodeFixture::Chain("l_tile", {NodeFixture::Tile("2", std::string(500, 'T'))}));
        writeJson(R + "/VidyaGod/[2] Long/l.json",
                  NodeFixture::Chain("l_exec", {NodeFixture::Exec("win32", "l.exe")}, {"l_tile"}, {{"PUBLISH", true}}));

        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);
        QVERIFY(cfg["Libraries"].contains("VidyaGod"));
        bool SawHeld = false;
        for (const auto & E : cfg["Libraries"]["VidyaGod"])
        {
            if (E.value("node", std::string()) == "h_exec") SawHeld = true;
            if (E.value("node", std::string()) == "l_exec")
            {
                const std::string T = E.value("title", std::string());
                QVERIFY2(T.size() == 120 && T == std::string(120, 'T'), "over-long title truncated at emit");
            }
        }
        QVERIFY2(!SawHeld, "a node with an unresolved LIBRARYITEM is held out of the share list");
        IpfsWrapper::StopNode();
    }

    // Friend-share sources ("friend:<peer>:<lib>") are disk-only: SyncPackageSources / HasMissingSources must skip them
    // (never IPNS-resolve or fetch). Teeth: without the friend: skip, the source routes to MirrorIpnsSource → the
    // IpnsResolve hook fires — the QVERIFY(!resolved) fails.
    void sync_skips_friend_sources()
    {
        auto resolved = std::make_shared<bool>(false);
        IpfsWrapper::SetIpnsResolveHook([resolved](const std::string &, std::string * e){ *resolved = true; if (e) *e = "x"; return std::string(); });
        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "friend:12D3KooWPeer:Games"}, {"NAME", "Alice \xc2\xb7 Games"}, {"FRIEND", true}, {"IPNS", true}} })}}},
                        {"LIBRARY", json::array()}};
        QCOMPARE(PackageCatalog::SyncPackageSources(cfg), 0);
        QVERIFY2(!*resolved, "a friend: source must NOT be IPNS-resolved / synced");
        QVERIFY2(!PackageCatalog::HasMissingSources(cfg), "a friend: source is disk-only — never 'missing'");
        IpfsWrapper::SetIpnsResolveHook({});
    }

    // DATA-LOSS GUARD 3: removing a friend source with PreserveInstalled must delete a STUB-only package but KEEP a
    // package the user INSTALLED (a dir with hydrated content) — converting it to a local package (SOURCE cleared,
    // files intact). Covers every friend-removal path (per-library withdraw, Receive-off, unfriend) since all call
    // RemovePackageSource(..., PreserveInstalled=true). Teeth: without the branch, remove_all nukes the install.
    void remove_friend_source_preserves_installed_content()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());
        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "friend:peerX:Games"}, {"NAME", "Alice"}, {"FRIEND", true}, {"IPNS", true}} })}}},
                        {"LIBRARY", json::array()}};
        const std::string srcDir = PackageCatalog::PackageSourceDir(cfg, cfg["Settings"]["PackageSources"][0]);
        const QString stubDir = QString::fromStdString(srcDir) + "/[1] Stub";
        const QString instDir = QString::fromStdString(srcDir) + "/[2] Installed";
        QDir().mkpath(stubDir);
        QDir().mkpath(instDir);
        { std::ofstream f((stubDir + "/n.json").toStdString()); f << "{}"; }              // stub-only (metadata)
        { std::ofstream f((instDir + "/n.json").toStdString()); f << "{}"; }              // installed: metadata +
        { std::ofstream f((instDir + "/game.bin").toStdString()); f << "installed"; }     //   hydrated content
        cfg["LIBRARY"].push_back(json{{"PACKAGEUID", "1"}, {"PATH", stubDir.toStdString()}, {"SOURCE", "friend:peerX:Games"}});
        cfg["LIBRARY"].push_back(json{{"PACKAGEUID", "2"}, {"PATH", instDir.toStdString()}, {"SOURCE", "friend:peerX:Games"}});

        PackageCatalog::RemovePackageSource(cfg, 0, /*PreserveInstalled=*/true);

        QVERIFY2(!QDir(stubDir).exists(), "a stub-only friend package is removed");
        QVERIFY2(QFile::exists(instDir + "/game.bin"), "installed content must be preserved, never rm'd");
        bool keptLocal = false;
        for (auto & e : cfg["LIBRARY"])
            if (e.is_object() && e.value("PATH", std::string()) == instDir.toStdString())
            { keptLocal = true; QVERIFY2(!e.contains("SOURCE"), "the kept install becomes a local package (SOURCE cleared)"); }
        QVERIFY2(keptLocal, "the installed package's LIBRARY entry survives");
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 0);   // the source config entry is gone
    }

    // SECURITY GUARD: a received friend STUB (under a "_friend_*" dir), even flagged PUBLISH, must NEVER enter the mint
    // when the user publishes — so it can't be re-shared as our own nor shadow our handles. Teeth: without the
    // GatherWorkingTree SkipReserved exclusion, the stub is minted and appears as an extra "_friend_*" library key.
    void publish_excludes_friend_stubs_from_the_mint()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        // Our own PUBLISH'd game.
        QDir().mkpath(R + "/VidyaGod/[1] Mine");
        writeJson(R + "/VidyaGod/[1] Mine/tile.json", NodeFixture::Chain("mytile", {NodeFixture::Tile("1", "Mine")}));
        writeJson(R + "/VidyaGod/[1] Mine/g.json",
                  NodeFixture::Chain("mygame", {NodeFixture::Exec("win32", "g.exe")}, {"mytile"}, {{"PUBLISH", true}}));
        // A received friend stub, flagged PUBLISH, under a reserved _friend_ dir — must be excluded from our publish.
        QDir().mkpath(R + "/_friend_peerX_deadbeef/[9] Evil");
        writeJson(R + "/_friend_peerX_deadbeef/[9] Evil/x.json",
                  json::array({ json{{"NODE_ID", "friendevil"}, {"TYPE", "DeclareExec"}, {"HOST", "win32"},
                                     {"PATH", "evil.exe"}, {"PUBLISH", true}} }));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);
        QVERIFY(cfg.contains("Libraries") && cfg["Libraries"].is_object());
        QVERIFY2(cfg["Libraries"].contains("VidyaGod"), "our own library publishes");
        QCOMPARE((int)cfg["Libraries"].size(), 1);   // ONLY VidyaGod — the _friend_ stub never entered the mint
        IpfsWrapper::StopNode();
    }

    // Publish is where a REFUSED declared position has its real consequence: PkgGraph::Build drops a position
    // no layout could have produced, StampNodePositions then writes a computed one over it, the node file's
    // bytes change and the package's Meta-CID with them. So the warning has to say which of those two things
    // happened — and decide it from what was actually WRITTEN. Deciding it by matching the source label was
    // wrong in both directions in turn (first "stamped over it" for a rejected local override that rewrites
    // nothing, then "ignored" for a node that was rewritten), and neither version was caught by anything.
    void thePublishWarningSaysWhatWasActuallyWritten()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        auto Write = [&](const char *Name, const nlohmann::ordered_json &J) {
            std::ofstream F((Dir.path() + "/" + Name).toStdString());
            F << J.dump(2);
        };
        // Its OWN POS is impossible: publish computes one and writes it, changing the file.
        Write("bad.json",  nlohmann::ordered_json{{"NODE_ID", "bad"},  {"TYPE", "Group"},
                                                  {"POS", nlohmann::ordered_json::array({5e9, 5e9})}});
        // A good POS with an impossible LOCAL override: the node keeps its POS and nothing is rewritten.
        Write("keep.json", nlohmann::ordered_json{{"NODE_ID", "keep"}, {"TYPE", "Group"},
                                                  {"POS", nlohmann::ordered_json::array({60.0, 60.0})},
                                                  {"PARENTS", nlohmann::ordered_json::array({"bad"})}});
        // NO POS of its own, plus an impossible local override. This is the case where the source label and
        // the outcome DIVERGE: the label says "this machine's saved layout", which sounds like nothing in the
        // package changed — but with no POS to fall back on the layout supplies one, the file GAINS a POS it
        // never had, and the Meta-CID moves. Both earlier versions of this line got this case wrong, and a
        // test built only from the two agreeing cases could not tell the difference.
        Write("fresh.json", nlohmann::ordered_json{{"NODE_ID", "fresh"}, {"TYPE", "Group"},
                                                   {"PARENTS", nlohmann::ordered_json::array({"bad"})}});
        nlohmann::ordered_json Override = nlohmann::ordered_json::object();
        Override["keep"]  = nlohmann::ordered_json::array({std::numeric_limits<double>::quiet_NaN(), 1.0});
        Override["fresh"] = nlohmann::ordered_json::array({1e300, 2.0});

        QStringList Warnings;
        struct Sink { ~Sink() { ClearLogCallback(); } } SinkGuard;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M) {
            if (L == LogLevel::WARN && M.find("no layout could have produced") != std::string::npos)
                Warnings << QString::fromStdString(M);
        });
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Dir.path().toStdString(), &Override, &Err),
                 qPrintable(QString::fromStdString(Err)));

        QString BadLine, KeepLine, FreshLine;
        for (const QString &W : Warnings) {
            if (W.contains("'bad'"))   BadLine = W;
            if (W.contains("'keep'"))  KeepLine = W;
            if (W.contains("'fresh'")) FreshLine = W;
        }
        QVERIFY2(!BadLine.isEmpty() && !KeepLine.isEmpty() && !FreshLine.isEmpty(),
                 qPrintable("all three refusals should be reported; saw:\n  " + Warnings.join("\n  ")));
        // The one that WAS rewritten says so, and names the consequence that matters at publish time.
        QVERIFY2(BadLine.contains("written over it") && BadLine.contains("changing this package's bytes"),
                 qPrintable("the rewritten node's line does not say so: " + BadLine));
        // The one that was not says the opposite.
        QVERIFY2(KeepLine.contains("nothing was rewritten"),
                 qPrintable("the untouched node's line claims a rewrite: " + KeepLine));
        // And the divergent one is worded by what HAPPENED, not by which declaration was bad.
        QVERIFY2(FreshLine.contains("written over it") && FreshLine.contains("changing this package's bytes"),
                 qPrintable("a node that gained a POS is reported as untouched: " + FreshLine));

        // And the files agree with the lines.
        auto Read = [&](const char *Name) {
            nlohmann::ordered_json J;
            std::ifstream F((Dir.path() + "/" + Name).toStdString());
            F >> J;
            return J;
        };
        const nlohmann::ordered_json B = Read("bad.json"), K = Read("keep.json"), Fr = Read("fresh.json");
        QVERIFY2(Fr.contains("POS"), "the node with no POS did not gain one, so this case proves nothing");
        QVERIFY2(B.contains("POS") && std::abs(B["POS"][0].get<double>()) < 1.0e6,
                 "the impossible POS was kept or replaced with another impossible one");
        QCOMPARE(K["POS"][0].get<double>(), 60.0);
    }

    //Two nodes with the SAME NODE_ID. A bundle is a folder of files, so nothing stops it, and a hand-edited
    //or peer-authored one can hold a duplicate — the editor's rename guard only covers renames made THROUGH
    //it. The publish line asks "was THIS node's file rewritten?", and it asked by id: one namesake being
    //rewritten made the line claim the other's bytes had changed too, in the one message an author has
    //telling them whether their declared layout survived publish.
    void thePublishWarningDistinguishesTwoNodesSharingAName()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        auto Write = [&](const char *Name, const nlohmann::ordered_json &J) {
            std::ofstream F((Dir.path() + "/" + Name).toStdString());
            F << J.dump(2);
        };
        //Both called "same". The first carries an impossible POS, so publish computes one and REWRITES it.
        Write("a.json", nlohmann::ordered_json{{"NODE_ID", "same"}, {"TYPE", "Group"},
                                               {"POS", nlohmann::ordered_json::array({5e9, 5e9})}});
        //The second carries a POS the layout would produce anyway, plus an impossible local OVERRIDE. It is
        //refused like the first, but its file is left exactly as it was — 60,60 is where the layout puts the
        //first node of the first layer, so "already correct: do not touch the bytes" fires.
        Write("b.json", nlohmann::ordered_json{{"NODE_ID", "same"}, {"TYPE", "Group"},
                                               {"POS", nlohmann::ordered_json::array({60.0, 60.0})}});
        nlohmann::ordered_json Override = nlohmann::ordered_json::object();
        Override["same"] = nlohmann::ordered_json::array({1e300, 2.0});

        QStringList Warnings;
        struct Sink { ~Sink() { ClearLogCallback(); } } SinkGuard;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M) {
            if (L == LogLevel::WARN && M.find("no layout could have produced") != std::string::npos)
                Warnings << QString::fromStdString(M);
        });
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Dir.path().toStdString(), &Override, &Err),
                 qPrintable(QString::fromStdString(Err)));

        //One line per REFUSAL, and the two nodes are refused for different reasons — so the lines are
        //distinguishable by their source even though the id is identical.
        QString Own, Local;
        for (const QString &W : Warnings) {
            if (W.contains("its own POS")) Own = W;
            if (W.contains("machine"))     Local = W;
        }
        QVERIFY2(!Own.isEmpty() && !Local.isEmpty(),
                 qPrintable("both refusals should be reported; saw:\n  " + Warnings.join("\n  ")));

        auto Read = [&](const char *Name) {
            nlohmann::ordered_json J;
            std::ifstream F((Dir.path() + "/" + Name).toStdString());
            F >> J;
            return J;
        };
        const nlohmann::ordered_json A = Read("a.json"), B = Read("b.json");
        //Establish what actually happened on disk FIRST, so the assertions about the wording below are
        //anchored to the files rather than to each other.
        QVERIFY2(std::abs(A["POS"][0].get<double>()) < 1.0e6, "the impossible POS was not replaced");
        QCOMPARE(B["POS"][0].get<double>(), 60.0);
        QCOMPARE(B["POS"][1].get<double>(), 60.0);

        //THE PROPERTY. Keyed by id, the untouched node's line inherited its namesake's rewrite.
        QVERIFY2(Own.contains("written over it"),
                 qPrintable("the rewritten node's line does not say so: " + Own));
        QVERIFY2(Local.contains("nothing was rewritten"),
                 qPrintable("a node whose file was NOT rewritten is reported as rewritten, because a node "
                            "sharing its NODE_ID was: " + Local));
    }
    //The data root is PROCESS-GLOBAL and sticky, and PackageEditorModel::SaveLayout flushes GlobalConfig.JSON
    //to it — so a suite that does not claim it writes to AppPaths' fallback, which is the developer's REAL
    //~/.VidyaGod/GlobalConfig.JSON. Running a single slot by name replaced a 50 KB config (sources, CIDs,
    //friends, per-package variables) with a two-key stub, and reported PASS. A full-suite run only escaped
    //because an earlier slot happened to leave the root pointing somewhere else — order-dependent luck that
    //any reordering removes. Claim it once, for the whole binary.
    void initTestCase()
    {
        SuiteRoot = new QTemporaryDir();
        QVERIFY(SuiteRoot->isValid());
        AppPaths::SetDataRoot(SuiteRoot->path().toStdString());
    }
    void cleanupTestCase() { delete SuiteRoot; SuiteRoot = nullptr; }

    // #2: a content-less node is vacuously "hydrated" but has no content — NodeHasContent must distinguish it.
    void node_has_content_vs_contentless()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("dir", "data"), NodeFixture::Exec("win32", "g.exe")}));
        writeJson(dir.path() + "/empty.json", NodeFixture::Chain("empty", {NodeFixture::Exec("win32", "g.exe")}));
        QDir(dir.path() + "/data").mkpath(".");                 // make 'game' hydrated

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        QVERIFY(PackageCatalog::NodeHasContent(idx, "game"));
        QVERIFY(!PackageCatalog::NodeHasContent(idx, "empty"));  // content-less → hidden from Library/Installed
        QVERIFY(PackageCatalog::NodeHydrated(idx, "game"));      // data dir present
    }

    // NodeHydrated = "everything FETCHABLE has been fetched", NOT "every backing file exists". A missing content layer
    // is un-hydrated ONLY when it carries a CID (a remote source to fetch from). A missing LOCAL-ONLY layer (no CID) is
    // not un-hydrated — there is nothing to download; a broken package is --validate-nodes' concern, not the tile's.
    void node_hydrated_tracks_fetchable_content()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // Missing file + a CID → FETCHABLE-MISSING → un-hydrated (the download button has work to do).
        writeJson(dir.path() + "/remote.json",
                  NodeFixture::Chain("remote", {NodeFixture::ContentCid("file", "blob.bin", "Qmdeadbeef"), NodeFixture::Exec("win32", "g.exe")}));
        // Missing file + NO CID → local-only, nothing to fetch → hydrated (broken-ness surfaces via validation).
        writeJson(dir.path() + "/localonly.json",
                  NodeFixture::Chain("localonly", {NodeFixture::Content("file", "solo.bin"), NodeFixture::Exec("win32", "g.exe")}));

        NodeIndex idx0; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx0);
        QVERIFY(!PackageCatalog::NodeHydrated(idx0, "remote"));     // CID-backed blob.bin not fetched yet
        QVERIFY(PackageCatalog::NodeHydrated(idx0, "localonly"));   // no CID → nothing fetchable is missing

        writeFile(dir.path() + "/blob.bin");                        // the fetch lands the file locally
        NodeIndex idx1; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx1);
        QVERIFY(PackageCatalog::NodeHydrated(idx1, "remote"));      // now fully fetched
    }

    // #8/#9: DehydrateNode deletes the content-layer files (keeping the manifest) and reports what it removed.
    void dehydrate_node_removes_content_keeps_manifest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("file", "blob.bin"), NodeFixture::Exec("win32", "g.exe")}));
        writeFile(dir.path() + "/blob.bin");

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        QVERIFY(PackageCatalog::NodeHydrated(idx, "game"));

        const int removed = PackageCatalog::DehydrateNode(idx, "game");
        QVERIFY(removed >= 1);
        QVERIFY(!QFile::exists(dir.path() + "/blob.bin"));       // content gone
        QVERIFY(QFile::exists(dir.path() + "/game.json"));       // manifest kept → returns to Catalog
    }

    // Per-package user settings persist into the INSTANCE file (InstanceStore) and read back. The temp UserDataRoot
    // isolates the write from the real ~/.VidyaGod; a write to the active instance auto-creates DefaultInstance.
    void package_user_settings_roundtrip()
    {
        QTemporaryDir ud; QVERIFY(ud.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        PackageCatalog::SetPackageUserSetting(cfg, "pkg1", "PREFERRED_RUNNER", "wine-ge");
        json us = PackageCatalog::GetPackageUserSettings(cfg, "pkg1");
        QCOMPARE(us.value("PREFERRED_RUNNER", std::string()), std::string("wine-ge"));
        QVERIFY(PackageCatalog::GetPackageUserSettings(cfg, "other").value("PREFERRED_RUNNER", std::string()).empty());
    }

    // CompatibleRunners returns runners whose GUEST set covers the launchable's host platform.
    void compatible_runners_by_platform()
    {
        NodeIndex idx;
        Node launch; launch.NodeId = "game"; launch.HasExec = true; launch.HostPlatform = "win32";
        Node wine;  wine.NodeId = "wine"; wine.HasRunner = true; wine.GuestPlatform = {"win32", "win64"};
        wine.HostPlatform = ManifestModel::MachinePlatform();
        wine.Exec = json{{"EXECUTABLE", "%RunnerMount%/proton"}};
        Node snes;  snes.NodeId = "snes9x"; snes.HasRunner = true; snes.GuestPlatform = {"snes"};
        snes.HostPlatform = ManifestModel::MachinePlatform();
        snes.Exec = json{{"EXECUTABLE", "%RunnerMount%/snes9x"}};
        idx.Nodes["game"] = launch; idx.Nodes["wine"] = wine; idx.Nodes["snes9x"] = snes;

        auto runners = PackageCatalog::CompatibleRunners(idx, idx.Nodes["game"]);
        bool hasWine = false, hasSnes = false;
        for (const Node * r : runners) { if (r->NodeId == "wine") hasWine = true; if (r->NodeId == "snes9x") hasSnes = true; }
        QVERIFY(hasWine);
        QVERIFY(!hasSnes);   // snes runner doesn't serve win32
    }

    // A package download pools the game's content layers AND its runner's build layers into ONE fetch batch (so they
    // download concurrently). CollectContentTargets + CollectRunnerNodeTargets append to the same Targets vector.
    void download_pools_content_and_runner_into_one_batch()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        // Dehydrated game: content zip has an IPFS CID but the file is absent → a fetch target.
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"content"}));
        writeJson(dir.path() + "/content.json",
                  NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", "game.zip", "CID_GAME")}));
        // Runner with a dehydrated build (its build is a PARENT content node, per the runner closure).
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectContentTargets(idx, "game", {}, targets, &err));
        QVERIFY(RunnerInstall::CollectRunnerNodeTargets(idx, "wine", targets, &err));

        bool hasGame = false, hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_GAME") hasGame = true; if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasGame);                       // game content target
        QVERIFY(hasWine);                       // runner build target — in the SAME batch
        QVERIFY(targets.size() >= 2);
    }

    // --download-all's pump: CollectCatalogTargets must reach EVERY node's content — launchables' closures, runner
    // builds, AND catalog nodes no launchable or runner references (an orphan library still belongs to a full
    // mirror) — while a node whose layer has no source is COUNTED, not fatal. Teeth (each caught): skip the IsRunner
    // branch → CID_WINE missing; collect only launchables/runners → CID_ORPHAN missing; abort on the first sourceless
    // node → later targets missing; stop counting → the stats assertions fail.
    void download_all_collects_every_catalog_node_and_tolerates_sourceless()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game1.json",
                  NodeFixture::Chain("game1", {NodeFixture::Tile("g1"), NodeFixture::Exec("win32", "g1.exe")}, {"content1", "lib"}));
        writeJson(dir.path() + "/content1.json", NodeFixture::Chain("content1", {NodeFixture::ContentCid("zip", "g1.zip", "CID_G1")}));
        writeJson(dir.path() + "/lib.json",      NodeFixture::Chain("lib",      {NodeFixture::ContentCid("zip", "lib.zip", "CID_LIB")}));
        writeJson(dir.path() + "/game2.json",
                  NodeFixture::Chain("game2", {NodeFixture::Tile("g2"), NodeFixture::Exec("win32", "g2.exe")}, {"content2"}));
        writeJson(dir.path() + "/content2.json", NodeFixture::Chain("content2", {NodeFixture::ContentCid("zip", "g2.zip", "CID_G2")}));
        // A runner + its dehydrated build (reached only via the IsRunner branch).
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")}, {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json", NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));
        // An ORPHAN library node: nothing parents it, no runner uses it — a full mirror must still fetch it.
        writeJson(dir.path() + "/orphanlib.json", NodeFixture::Chain("orphanlib", {NodeFixture::ContentCid("zip", "orphan.zip", "CID_ORPHAN")}));
        // A node whose content is missing WITH NO SOURCE (a runtime-generated path) — must be counted, never fatal.
        writeJson(dir.path() + "/sourceless.json", NodeFixture::Chain("sourceless", {NodeFixture::Content("zip", "nowhere.zip")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        // 9 files, 11 nodes: NodeFixture::Chain writes a two-layer chain (Tile + Exec) as TWO nodes (a private
        // "<id>__l0" link + the tail), and game1/game2 are the only two-layer chains here.
        QCOMPARE((int)idx.Nodes.size(), 11);

        std::vector<IpfsWrapper::FetchTarget> targets;
        CliModes::CatalogTargetStats st; int sourcelessCalls = 0; std::string sourcelessId;
        CliModes::CollectCatalogTargets(idx, targets, &st,
            [&](const std::string &id, const std::string &){ ++sourcelessCalls; sourcelessId = id; });

        std::set<std::string> cids; for (const auto & t : targets) cids.insert(t.Cid);
        const std::set<std::string> want{"CID_G1", "CID_LIB", "CID_G2", "CID_WINE", "CID_ORPHAN"};
        QCOMPARE(cids, want);                   // every catalog node's content, nothing invented
        QCOMPARE(st.Nodes, 11);                 // every indexed node visited, link nodes included
        QCOMPARE(st.Launchables, 2);
        QCOMPARE(st.Runners, 1);
        QCOMPARE(st.SourcelessNodes, 1);        // counted…
        QCOMPARE(sourcelessCalls, 1);           // …and reported once
        QCOMPARE(sourcelessId, std::string("sourceless"));
    }

    // SourceDirSynced is the ONE "this source has been fetched" predicate (sync, missing-check, --download-all).
    // Teeth (each caught): treat ENOENT as an error → the absent assert fails; drop the !Ec on is_empty → the
    // unreadable case reports synced=false with a CLEAN Ec and the loud-skip in SyncPackageSources never fires.
    void sourceDirSyncedDistinguishesAbsentEmptyUnreadableAndSynced()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const std::string Base = dir.path().toStdString();
        std::error_code Ec;
        QVERIFY(!PackageCatalog::SourceDirSynced(Base + "/absent", Ec));
        QVERIFY(!Ec);                                              // not yet synced: a clean "no"
        std::filesystem::create_directory(Base + "/empty");
        QVERIFY(!PackageCatalog::SourceDirSynced(Base + "/empty", Ec));
        QVERIFY(!Ec);                                              // fetched nothing yet: re-fetch, not an error
        std::filesystem::create_directory(Base + "/full");
        { std::ofstream(Base + "/full/x.json") << "{}"; }
        QVERIFY(PackageCatalog::SourceDirSynced(Base + "/full", Ec));
        QVERIFY(!Ec);
#if !defined(Q_OS_WIN)
        if (geteuid() != 0)   // root (and Windows) ignore directory permission bits — the premise doesn't hold there
        {
            std::filesystem::permissions(Base + "/full", std::filesystem::perms::none);
            const bool Synced = PackageCatalog::SourceDirSynced(Base + "/full", Ec);
            std::filesystem::permissions(Base + "/full", std::filesystem::perms::owner_all);   // restore before asserts
            QVERIFY(!Synced);
            QVERIFY(Ec);                                           // unreadable: a LOUD no — the sync must refuse it
        }
#endif
    }

    // Hydration asks the LAYER whether it is runtime-sourced, never the resolved absolute path. Two ways the
    // path-derived version got it wrong, both of which report an un-fetched layer as HYDRATED — so the download
    // button never appears and the game launches against a hole:
    //   * a URL-escaped filename, which the predicate deliberately treats as real content; and
    //   * a library root that itself contains a '%' segment, which poisoned every layer beneath it.
    void hydrationAsksTheLayerNotTheResolvedPath()
    {
        auto missingWithCid = [&](const QString &dir, const char *name) {
            writeJson(dir + "/game.json",
                      NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")},
                                         {"content"}));
            writeJson(dir + "/content.json",
                      NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", name, "CID_GAME")}));
            NodeIndex idx; ManifestModel::ScanBundleNodes(dir.toStdString(), idx);
            return PackageCatalog::NodeHydrated(idx, "game");      // file ABSENT + a CID => NOT hydrated
        };

        QTemporaryDir plain; QVERIFY(plain.isValid());
        QVERIFY(!missingWithCid(plain.path(), "game.zip"));                  // the control

        QTemporaryDir escaped; QVERIFY(escaped.isValid());
        QVERIFY(!missingWithCid(escaped.path(), "100%25%20done.zip"));       // an escaped NAME is content

        // A bundle whose own directory carries a '%' segment: nothing about the LAYER changed.
        QTemporaryDir root; QVERIFY(root.isValid());
        const QString odd = root.path() + "/My %Games%";
        QVERIFY(QDir().mkpath(odd));
        QVERIFY(!missingWithCid(odd, "game.zip"));

        // ...and a genuine runtime mount is still Runtime: no file, no CID, nothing to fetch, still "hydrated".
        QTemporaryDir rt; QVERIFY(rt.isValid());
        writeJson(rt.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"pfx"}));
        writeJson(rt.path() + "/pfx.json",
                  NodeFixture::Chain("pfx", {NodeFixture::Content("dir", "%DefaultPfxDir%")}));
        writeJson(rt.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"pfx"}));
        NodeIndex ridx; ManifestModel::ScanBundleNodes(rt.path().toStdString(), ridx);
        QVERIFY(PackageCatalog::NodeHydrated(ridx, "game"));
    }

    // THE install gate, against the shape the flat schema actually produces. A prefix-generating runner's
    // assembly mounts ("%DefaultPfxDir%") used to sit on the runner node itself, where "skip the root" hid them;
    // they are now ordinary Content nodes INSIDE the closure, and they resolve to <bundle>/%DefaultPfxDir% —
    // a path that never exists on disk. Any site that counts them as build content declares the runner
    // not-installed, which greys out Play on every game that needs it with no diagnostic.
    //
    // This asserts the CALL SITES, not the predicate: the previous test pinned IsRuntimeSourcedLayer and passed
    // while two of the five callers had never been taught to use it.
    void prefix_assembly_content_is_not_the_runners_build()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        // The real build: present on disk.
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::Content("zip", "wine.zip")}, {"wine_defaultpfx"}));
        writeFile(dir.path() + "/wine.zip");
        // The prefix-assembly mount, as its own Content node — nothing on disk, by design.
        writeJson(dir.path() + "/wine_defaultpfx.json",
                  NodeFixture::Chain("wine_defaultpfx", {NodeFixture::Content("dir", "%DefaultPfxDir%")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        QVERIFY(RunnerInstall::RunnerBuildPresent(idx, "wine"));      // the build IS here
        QVERIFY(PackageCatalog::RunnerInstalled(idx, "wine"));        // ...so the runner is installed

        // ...and it is not something to download, either: a fetch target for a %variable% path would have the
        // download manager forever re-offering an already-hydrated runner.
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(RunnerInstall::CollectRunnerNodeTargets(idx, "wine", targets, &err));
        for (const auto & t : targets)
            QVERIFY2(t.LocalPath.find('%') == std::string::npos, t.LocalPath.c_str());

        // The control: remove the REAL build and the runner must go back to not-installed, or this test would
        // pass just as well against a gate that checks nothing at all.
        QVERIFY(QFile::remove(dir.path() + "/wine.zip"));
        NodeIndex idx2; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx2);
        QVERIFY(!RunnerInstall::RunnerBuildPresent(idx2, "wine"));
        QVERIFY(!PackageCatalog::RunnerInstalled(idx2, "wine"));
    }

    // Full-closure hydrate: CollectRunnerChainTargets auto-resolves the game's runner via the PLATFORM GRAPH (no
    // manually-named runner) and pools its build — so a downloaded game is immediately playable. Same graph as above:
    // the game (PLATFORM win32) resolves to the wine runner (GUEST [win32]) whose build (CID_WINE) must be fetched.
    void hydrate_pools_resolved_runner_chain()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe")}, {"content"}));
        writeJson(dir.path() + "/content.json",
                  NodeFixture::Chain("content", {NodeFixture::ContentCid("zip", "game.zip", "CID_GAME")}));
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);

        json cfg = json::object();                                 // no runner pin → default platform-graph resolve
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectRunnerChainTargets(idx, "game", cfg, targets, &err));
        bool hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasWine);                       // runner build auto-pooled purely from the game's resolved chain
    }

    // A locally-added bundle (a LIBRARY entry whose PATH is OUTSIDE any repo dir) must be indexed by the catalog —
    // BuildCatalogIndex scans LocalPackageDirs alongside the repo roots. Regression: external bundles silently never
    // showed because only repo dirs were scanned.
    void local_package_is_indexed()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());                 // an external bundle, not under any repo
        writeJson(dir.path() + "/tile.json", NodeFixture::Chain("tile", {NodeFixture::Tile("777", "My Local Game")}));
        writeJson(dir.path() + "/variant.json",
                  NodeFixture::Chain("variant", {NodeFixture::Exec("win32", "g.exe")}, {"tile"}));

        json cfg = json{{"Settings", {{"Repositories", json::array()}}},
                        {"LIBRARY", json::array({ json{{"PACKAGEUID", "777"}, {"PATH", dir.path().toStdString()}} })}};

        QVERIFY(!PackageCatalog::LocalPackageDirs(cfg).empty());   // the external bundle dir is collected
        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        const Node * v = idx.Find("variant");
        QVERIFY(v != nullptr);                                     // indexed despite living outside any repo
        QVERIFY(v->Presentable());                                 // linked to its tile → shows in the library
        const Node * tile = idx.Find("tile");                      // gigagraph: the grouping key is the TILE's identity
        QVERIFY(tile != nullptr);                                  // (its CID in the catalog), reached via the LIBRARYITEM
        QCOMPARE(v->GameKey(), tile->Key());                       // edge — no longer the tile's NODE_ID label
    }

    // Startup prune drops a LIBRARY entry for a local bundle whose PATH no longer exists (moved/deleted), keeps the
    // present one.
    void prune_moved_local_package()
    {
        QTemporaryDir present; QVERIFY(present.isValid());
        json cfg = json{{"Settings", {{"Repositories", json::array()}}},
                        {"LIBRARY", json::array({
                            json{{"PACKAGEUID", "a"}, {"PATH", present.path().toStdString()}},
                            json{{"PACKAGEUID", "b"}, {"PATH", "/no/such/bundle/dir/xyz"}} })}};

        const int removed = PackageCatalog::PruneMovedLocalPackages(cfg);
        QCOMPARE(removed, 1);
        QCOMPARE((int)cfg["LIBRARY"].size(), 1);
        QCOMPARE(cfg["LIBRARY"][0].value("PACKAGEUID", std::string()), std::string("a"));   // present one kept
    }

    // A CID package source (its already-fetched dir under <DataRoot>/LIBRARY/) is indexed by BuildCatalogIndex as a
    // root, and its packages are treated as MANAGED — not badged/pruned as "local". (Simulates a fetched source; the
    // network fetch itself is covered by the Go node's TestFetchDirToPath.)
    void cid_package_source_is_managed_and_indexed()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        const QString bundle = data.path() + "/LIBRARY/mysource/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/tile.json", NodeFixture::Chain("cidtile", {NodeFixture::Tile("9090", "CID Game")}));
        writeJson(bundle + "/variant.json",
                  NodeFixture::Chain("cidvariant", {NodeFixture::Exec("win32", "g.exe")}, {"cidtile"}));

        json cfg = json{{"Settings", {{"Repositories", json::array()},
                                      {"PackageSources", json::array({ json{{"CID", "QmSourceFolderCID"}, {"NAME", "mysource"}} })}}}};

        const auto srcDirs = PackageCatalog::PackageSourceDirs(cfg);
        QCOMPARE((int)srcDirs.size(), 1);                                   // the existing source dir is a scan root
        QVERIFY(PackageCatalog::IsPackageSourcePath(cfg, bundle.toStdString()));
        QVERIFY(!PackageCatalog::IsLocalPackagePath(cfg, bundle.toStdString()));   // managed, NOT local (no LOCAL badge)

        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        const Node * v = idx.Find("cidvariant");
        QVERIFY(v != nullptr);                                             // indexed via the CID-source root
        QVERIFY(v->Presentable());                                         // grouped under its tile
        const Node * tile = idx.Find("cidtile");                           // grouping key = the tile's identity (its CID)
        QVERIFY(tile != nullptr);
        QCOMPARE(v->GameKey(), tile->Key());
    }

    // A PER-PACKAGE CID: the fetched source dir is ITSELF a bundle (node JSON at its top level, no package subdirs).
    // SyncPackageSources must index it as one package whose PATH is the source dir itself (not a subdir) — this is what
    // makes each package individually addable by its own folder CID.
    void single_package_cid_source_indexed_as_one_package()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        // The already-fetched source dir holds the bundle files directly (no wrapping subdir).
        const QString dir = data.path() + "/LIBRARY/solopkg";
        QDir().mkpath(dir);
        writeJson(dir + "/tile.json", NodeFixture::Chain("solotile", {NodeFixture::Tile("7777", "Solo Game")}));
        writeJson(dir + "/variant.json",
                  NodeFixture::Chain("solovariant", {NodeFixture::Exec("win32", "g.exe")}, {"solotile"}));

        json cfg = json{{"Settings", {{"Repositories", json::array()},
                                      {"PackageSources", json::array({ json{{"CID", "QmSoloPackageCID"}, {"NAME", "solopkg"}} })}}}};

        const int indexed = PackageCatalog::SyncPackageSources(cfg);
        QCOMPARE(indexed, 1);                                              // one launchable package
        QCOMPARE((int)cfg["LIBRARY"].size(), 1);
        QVERIFY(!cfg["LIBRARY"][0].value("PACKAGEUID", std::string()).empty());
        QCOMPARE(cfg["LIBRARY"][0].value("PATH", std::string()), dir.toStdString());   // PATH is the source dir itself
        QCOMPARE(cfg["LIBRARY"][0].value("CIDSOURCE", std::string()), std::string("QmSoloPackageCID"));

        NodeIndex idx = PackageCatalog::BuildCatalogIndex(cfg);
        QVERIFY(idx.Find("solovariant") != nullptr);                       // launchable via the per-package CID root
    }

    // Add/remove a package source: add dedups on CID; remove drops the config entry, deletes the dir, and drops LIBRARY
    // entries under it.
    void add_remove_package_source()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());
        json cfg = json{{"Settings", json::object()}};

        QVERIFY(PackageCatalog::AddPackageSource(cfg, "QmABC", "src1"));
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "QmABC", "again"));   // dedup on CID
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "", "empty"));        // empty CID rejected
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);

        // Simulate a fetched dir + a LIBRARY entry under it, then remove.
        const QString dir = data.path() + "/LIBRARY/src1";
        QDir().mkpath(dir);
        cfg["LIBRARY"] = json::array({ json{{"PACKAGEUID", "z"}, {"PATH", (dir + "/pkg").toStdString()}, {"CIDSOURCE", "QmABC"}} });

        PackageCatalog::RemovePackageSource(cfg, 0);
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 0);         // source gone
        QVERIFY(cfg["LIBRARY"].empty());                                    // its LIBRARY entry gone
        QVERIFY(!QDir(dir).exists());                                       // its dir deleted
    }

    // Catalog sectioning: PackageSourceNameForPath returns the owning source's NAME for a bundle under it, or "" for a
    // path outside every source (which the catalog groups under "Local"). Each source is thus its own named section.
    void package_source_name_for_path_sections_by_source()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());

        const QString aBundle = data.path() + "/LIBRARY/Alpha/game";
        const QString bBundle = data.path() + "/LIBRARY/Beta/game";
        QDir().mkpath(aBundle); QDir().mkpath(bBundle);

        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "QmA"}, {"NAME", "Alpha"}}, json{{"CID", "QmB"}, {"NAME", "Beta"}} })}}}};

        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, aBundle.toStdString()), std::string("Alpha"));
        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, bBundle.toStdString()), std::string("Beta"));
        QCOMPARE(PackageCatalog::PackageSourceNameForPath(cfg, (data.path() + "/Elsewhere/pkg").toStdString()), std::string());
    }

    // Regression: a node-native cover lives on the DeclareLibraryItem LAYER's COVER field (post-Declare* refactor), NOT
    // a top-level META.COVER. SeedTargets must collect it (else covers were silently skipped by --seed/re-publish).
    void seed_targets_finds_declarelibraryitem_cover_and_layers()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        // The content file + cover file must exist on disk (SeedTargets only returns present files).
        { std::ofstream f((bundle + "/game.zip").toStdString()); f << "content"; }
        { std::ofstream f((bundle + "/cover.png").toStdString()); f << "img"; }

        json tile = NodeFixture::Tile("1", "G");
        tile["COVER"] = json{{"PATH", "cover.png"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "QmCoverCID"}}}};
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("g", {NodeFixture::ContentCid("zip", "game.zip", "QmLayerCID"), tile}));

        const auto all = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/false);
        QCOMPARE(all.at((bundle + "/game.zip").toStdString()), std::string("QmLayerCID"));
        QCOMPARE(all.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));   // the bug: cover was missed
        QCOMPARE((int)all.size(), 2);

        const auto covers = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/true);
        QCOMPARE((int)covers.size(), 1);                                                       // covers-only skips layers
        QCOMPARE(covers.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));
    }

    // ---- PublishPackage: the one operation whose mistakes travel ----
    //
    // A publish that skips something still returns true and still mints a CID. Every peer then fetches a package
    // that looks healthy and is not, and the gap surfaces days later as "this game is missing files" on a machine
    // that is not the author's. Both quiet skips below are now reported; these pin that they are.

    void publish_reports_an_unparseable_fragment_instead_of_dropping_it()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/good.json", NodeFixture::Chain("g", {}));   // pure composition: TYPE "Group"
        writeFile(bundle + "/broken.json", "{ \"NODE_ID\": \"b\", oops not json");

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Named = false, Summarised = false;
        for (const auto &E : Errors)
        {
            if (E.find("broken.json") != std::string::npos) Named = true;
            if (E.find("PUBLISHED WITH GAPS") != std::string::npos) Summarised = true;
        }
        QVERIFY2(Named, "the fragment that would not parse must be named — it is absent from the published package");
        QVERIFY2(Summarised, "and the publish must end saying the CID will look healthy while content is missing");
    }

    // A layer with neither a CID nor a local file is published as a reference to bytes that exist nowhere: the
    // download reports nothing to fetch and the game is simply missing files on every machine but this one.
    void publish_reports_a_layer_that_is_neither_addressed_nor_present()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json", NodeFixture::Chain("g", {NodeFixture::Content("zip", "never_existed.zip")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Found = false;
        for (const auto &E : Errors)
            if (E.find("UNFETCHABLE") != std::string::npos && E.find("never_existed.zip") != std::string::npos)
                Found = true;
        QVERIFY2(Found, "an unaddressed, absent layer must be named before it is published");
    }

    // The counterpart that keeps the noise meaningful: a package with nothing wrong must publish silently.
    void a_healthy_publish_reports_no_gaps()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/game";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("g", {NodeFixture::ContentCid("zip", "already.zip", "QmAlreadyAddressed")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        for (const auto &E : Errors)
            QVERIFY2(E.find("PUBLISHED WITH GAPS") == std::string::npos,
                     "an already-addressed layer is the idempotent case and must not be reported as a gap");
    }

    // A RUNTIME-SOURCED layer is not a gap. Its PATH carries a %VAR% that resolves to a live mount at launch
    // (the proton runners' prefix-assembly layers, PATH="%DefaultPfxDir%"), so there is no package file to
    // seed and there never will be. Publishing reported all 15 of them as UNFETCHABLE and closed the runners
    // collection with "PUBLISHED WITH GAPS ... the content will not be there" on every single mint — a false
    // alarm over the one summary written to be un-ignorable, and the exact failure launchsources.cpp records
    // having fixed on the launch side (three phantom errors per wine launch) with this same predicate.
    void publish_does_not_report_a_runtime_sourced_layer_as_a_gap()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/runner";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("r", {NodeFixture::Content("dir", "%DefaultPfxDir%"),
                                           NodeFixture::Content("dir", "%WineSys32Dir%")}));

        std::vector<std::string> Errors; QString Walked;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M);
            if (M.find("Dehydrated") != std::string::npos) Walked = QString::fromStdString(M); });
        const bool Ok = PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        QVERIFY2(Ok, "publishing a runner bundle must SUCCEED - asserting only the absence of error text "
                     "passes just as well when the function bailed out early");
        QVERIFY2(Walked.contains("of 2 layer(s)"),
                 qPrintable("the runtime-sourced layers must still be WALKED — the predicate gates the REPORT, "
                            "not the seed path, or 'log-only' becomes a property of the library's contents "
                            "rather than of the code. Saw: " + Walked));
        for (const auto &E : Errors)
        {
            QVERIFY2(E.find("UNFETCHABLE") == std::string::npos,
                     qPrintable(QString("a prefix-assembly layer was reported as an unfetchable gap: %1")
                                    .arg(QString::fromStdString(E))));
            QVERIFY2(E.find("PUBLISHED WITH GAPS") == std::string::npos,
                     qPrintable(QString("a package whose only absent layers are runtime-sourced was reported "
                                        "as published with gaps: %1").arg(QString::fromStdString(E))));
        }
    }

    // ...and the half that keeps the fix honest. Skipping the %VAR% layers must not blanket-silence the check:
    // a layer with a REAL path, no CID and no file, sitting in the same bundle, is still a reference to bytes
    // that exist nowhere and must still be named. A guard written only to pass is not a guard.
    void publish_still_reports_a_real_missing_layer_beside_a_runtime_sourced_one()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/mixed";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("m", {NodeFixture::Content("dir", "%DefaultPfxDir%"),
                                           NodeFixture::Content("zip", "never_existed.zip")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool NamedReal = false, Summarised = false, NamedVar = false;
        for (const auto &E : Errors)
        {
            if (E.find("UNFETCHABLE") != std::string::npos && E.find("never_existed.zip") != std::string::npos)
                NamedReal = true;
            if (E.find("PUBLISHED WITH GAPS") != std::string::npos) Summarised = true;
            if (E.find("DefaultPfxDir") != std::string::npos) NamedVar = true;
        }
        QVERIFY2(NamedReal, "the genuinely absent layer must STILL be named — the %VAR% skip must not silence it");
        QVERIFY2(Summarised, "and the publish must still end saying the CID will look healthy without the content");
        QVERIFY2(!NamedVar, "the prefix-assembly layer must not be named alongside it");
    }

    // The call site must use THE predicate, not its own idea of one. A hand-rolled `PATH.find('%')` scanner
    // passes every test above — and it is wrong twice over, in the two ways manifestmodel.h warns a second
    // scanner always drifts:
    //   * a '%' is not a variable. PathVariableTokens requires a MATCHED PAIR around an IDENTIFIER, because
    //     URL-escaped filenames are ordinary in scraped content ("100%25%20done.zip" — "25" is not an
    //     identifier). Treating that as a variable silently drops a REAL missing file from the report.
    //   * the field is PATH *overridden by SOURCE.PATH*, so a layer carrying the token only on the override
    //     is classified off the wrong string.
    // Both directions are asserted here, so the only implementation that passes is the shared predicate.
    void publish_classifies_runtime_sourced_by_the_predicate_not_by_a_percent_sign()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/pct";
        QDir().mkpath(bundle);
        json escaped   = NodeFixture::Content("zip", "100%25%20done.zip");   // matched pair, NOT an identifier
        json overriden = NodeFixture::Content("dir", "placeholder");
        overriden["SOURCE"] = json{{"TYPE", "ipfs"}, {"PATH", "%DefaultPfxDir%"}};   // token on the OVERRIDE
        writeJson(bundle + "/node.json", NodeFixture::Chain("p", {escaped, overriden}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool NamedEscaped = false, NamedOverride = false;
        for (const auto &E : Errors)
        {
            if (E.find("100%25%20done.zip") != std::string::npos) NamedEscaped = true;
            if (E.find("DefaultPfxDir") != std::string::npos)     NamedOverride = true;
        }
        QVERIFY2(NamedEscaped, "a URL-escaped filename is NOT a runtime-sourced layer — it is a real missing "
                               "file and must be reported. A scanner that only looks for '%' hides it.");
        QVERIFY2(!NamedOverride, "SOURCE.PATH overrides PATH, so a token on the override makes the layer "
                                 "runtime-sourced. A scanner reading only PATH reports it as a gap.");
    }

    // The predicate gates the REPORT and nothing else. Placed any earlier it also skips the VerifyCid drift
    // repair and the seed itself, which would make "log-only" a property of the library's contents (today no
    // token-bearing layer happens to have bytes) rather than of the code — and every other test here is blind
    // to that, because they all use token-bearing layers with NO file.
    //
    // The discriminator needs no IPFS: a layer whose bytes ARE on disk must REACH the seed path, and in this
    // harness the node is not running, so reaching it fails loudly and names the file. Asserting a FAILURE is
    // the point — the mutant that skips the layer publishes "successfully" with the content silently absent,
    // which is the exact outcome this whole area exists to prevent. `_off` is an identifier, so the name is a
    // genuine %VAR% by PathVariableTokens' rule, not merely a string with percent signs in it.
    void publish_still_seeds_a_token_bearing_layer_whose_bytes_are_present()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/probe";
        QDir().mkpath(bundle);
        writeFile(bundle + "/%Foo%", "real bytes behind a token-shaped name");
        writeJson(bundle + "/node.json", NodeFixture::Chain("t", {NodeFixture::Content("file", "%Foo%")}));

        std::vector<std::string> Msgs;
        SetLogCallback([&](LogLevel, const std::string &, const std::string &M){ Msgs.push_back(M); });
        const bool Ok = PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Reached = false, CalledUnfetchable = false;
        for (const auto &M : Msgs)
        {
            if (M.find("could not seed layer") != std::string::npos && M.find("%Foo%") != std::string::npos)
                Reached = true;
            if (M.find("UNFETCHABLE") != std::string::npos) CalledUnfetchable = true;
        }
        QVERIFY2(Reached, "a runtime-sourced layer whose file EXISTS must still reach the seed path — if the "
                          "predicate gates anything but the report, this layer is skipped and its bytes never "
                          "ship, while publish reports success");
        QVERIFY2(!Ok, "...and that failure must propagate, not be swallowed");
        QVERIFY2(!CalledUnfetchable, "it is present, so it is not an unfetchable gap either");
    }

    // A COVER that is not an object is content that can never be addressed, and it used to be stepped over in
    // total silence: the call site was `is_object()` and nothing else. One package shipped that way — Tonic
    // Trouble's library tile carried the pre-node bare-string form, the PNG sat in the bundle, it was never
    // seeded, and both ManifestTargets and PackageCoverCids require the object form, so no peer could ever
    // receive the tile art. Nothing logged it and --validate-nodes called the library clean.
    void publish_reports_a_cover_that_can_never_be_addressed()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/tile";
        QDir().mkpath(bundle);
        writeFile(bundle + "/cover.png", "PNGBYTES");
        json tile = NodeFixture::Tile("1234", "A Game");
        tile["COVER"] = "cover.png";                       // the pre-node bare-string form, file present
        writeJson(bundle + "/node.json", NodeFixture::Chain("t", {}, {}, tile));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Named = false, Summarised = false;
        for (const auto &E : Errors)
        {
            if (E.find("COVER") != std::string::npos && E.find("not objects") != std::string::npos) Named = true;
            if (E.find("PUBLISHED WITH GAPS") != std::string::npos
                && E.find("unaddressable cover") != std::string::npos) Summarised = true;
        }
        QVERIFY2(Named, "a COVER that is not an object must be named — the art never reaches another machine");
        QVERIFY2(Summarised, "and it must count toward the gaps summary, which is the line that gets read");
    }

    // ---- StampNodePositions: the layout the author sees is what a peer receives -------------------------
    // Positions are the one thing allowed to enter the package's bytes only at publish time. These pin the
    // three properties that makes that safe: it writes POS for everything, it bakes the CURRENT layout (a
    // local drag beats the node's old POS), and it is a no-op on an unchanged bundle — because a stamp that
    // rewrote bytes every run would mint a new CID, and re-download the package for every peer, for nothing.

    void stamp_writes_POS_for_every_node()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        for (int I = 0; I < 5; ++I)
        {
            json N;
            N["NODE_ID"] = "n" + std::to_string(I);
            N["TYPE"]    = "Group";
            if (I > 0) N["PARENTS"] = json::array({"n" + std::to_string(I - 1)});
            writeJson(Dir.filePath(QString("n%1.json").arg(I)), N);
        }
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err), Err.c_str());

        std::set<std::pair<double, double>> Seen;
        for (int I = 0; I < 5; ++I)
        {
            std::ifstream In(Dir.filePath(QString("n%1.json").arg(I)).toStdString());
            json J; In >> J;
            QVERIFY(J.contains("POS"));
            QVERIFY(J["POS"].is_array() && J["POS"].size() == 2);
            Seen.insert({J["POS"][0].get<double>(), J["POS"][1].get<double>()});
        }
        QCOMPARE(Seen.size(), (size_t)5);          // distinct: nothing stacked on the origin
    }

    void stamp_is_a_no_op_the_second_time()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        json B; B["NODE_ID"] = "b"; B["TYPE"] = "Group"; B["PARENTS"] = json::array({"a"});
        writeJson(Dir.filePath("a.json"), A);
        writeJson(Dir.filePath("b.json"), B);

        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        auto Read = [&](const char *F) {
            std::ifstream In(Dir.filePath(F).toStdString());
            return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        };
        const std::string A1 = Read("a.json"), B1 = Read("b.json");
        //Byte equality alone proves nothing here — rewriting the same content produces the same bytes, so it
        //holds even if the skip is gone. The property worth pinning is that an unchanged node is NOT WRITTEN
        //at all, so assert the mtime too, after letting the clock move far enough to tell.
        const QDateTime AT = QFileInfo(Dir.filePath("a.json")).lastModified();
        QTest::qSleep(1100);
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        QCOMPARE(Read("a.json"), A1);              // byte-identical ⇒ same Meta-CID on republish
        QCOMPARE(Read("b.json"), B1);
        QCOMPARE(QFileInfo(Dir.filePath("a.json")).lastModified(), AT);   // and untouched on disk
    }

    void stamp_bakes_a_local_drag_over_the_existing_POS()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group"; A["POS"] = json::array({10.0, 20.0});
        writeJson(Dir.filePath("a.json"), A);

        json Local = json::object();
        Local["a"] = json::array({777.0, 888.0});   // the author dragged it; that is the current layout
        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), &Local, &Err));

        std::ifstream In(Dir.filePath("a.json").toStdString());
        json J; In >> J;
        QCOMPARE(J["POS"][0].get<double>(), 777.0);
        QCOMPARE(J["POS"][1].get<double>(), 888.0);
    }

    void stamp_handles_a_file_holding_an_array_of_nodes()
    {
        //Grouping nodes into one file is presentation the editor preserves, so the stamp has to reach into an
        //array rather than skipping it — a skipped file is a package that publishes half a layout.
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json Arr = json::array();
        for (int I = 0; I < 3; ++I)
        { json N; N["NODE_ID"] = "m" + std::to_string(I); N["TYPE"] = "Group"; Arr.push_back(N); }
        writeJson(Dir.filePath("many.json"), Arr);

        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), nullptr, &Err));
        std::ifstream In(Dir.filePath("many.json").toStdString());
        json J; In >> J;
        QCOMPARE(J.size(), (size_t)3);
        for (const auto &N : J) QVERIFY(N.contains("POS"));
    }

    // The WIRING, not the function. Every other stamp test calls StampNodePositions with a hand-built override,
    // so when the editor's key and publishing's lookup drifted apart — which they did — nothing failed: the
    // lookup simply missed and the author's whole arrangement was replaced by the algorithm's default at the
    // one moment it was supposed to be preserved. This asserts the two agree on the same bundle.
    void theEditorsLayoutKeyIsTheOnePublishingLooksUp()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const QString Bundle = Dir.filePath("[999][v1.0] Keyed");
        QVERIFY(QDir().mkpath(Bundle));
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        writeJson(Bundle + "/a.json", A);

        // What the editor writes, through the editor's own path.
        json Cfg = json::object();
        PackageEditorModel Model(&Cfg, nullptr);
        Model.initPackage(Bundle, nullptr);
        PkgGraph::SetPos(Model.layout(), "a", 4242.0, 2424.0);
        Model.SaveLayout();
        QVERIFY2(Cfg.contains("EDITORLAYOUT"), "the editor wrote no layout at all");

        // What publishing reads, through publishing's own path.
        // Through the SAME function the publisher calls, and — critically — with the path SHAPES the publisher
        // actually produces. An already-absolute, already-normal path makes both sides equal by construction,
        // so the test would pass with EditorLayoutKey implemented as `return BundleDir.string();` and could not
        // see a relative root (--remint-library LIBRARY), a trailing slash, or a "/./" segment.
        for (const std::string &Shape : { Bundle.toStdString(),
                                          Bundle.toStdString() + "/",
                                          Bundle.toStdString() + "/./",
                                          std::filesystem::relative(Bundle.toStdString()).string() })
        {
            if (Shape.empty()) continue;   // relative() yields "" across filesystems; not a case we can force
            QVERIFY2(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Shape)) != nullptr,
                     qPrintable(QString("publishing finds nothing for bundle path shape '%1'")
                                .arg(QString::fromStdString(Shape))));
        }
        const json *Local = PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Bundle.toStdString()));
        QVERIFY2(Local != nullptr,
                 "publishing's own lookup finds nothing for the bundle the editor just wrote");
        QCOMPARE((*Local)["a"][0].get<double>(), 4242.0);

        // ...and end to end: the drag must reach POS.
        std::string Err;
        QVERIFY2(PackageCatalog::StampNodePositions(Bundle.toStdString(), Local, &Err), Err.c_str());
        std::ifstream In((Bundle + "/a.json").toStdString());
        json J; In >> J;
        QCOMPARE(J["POS"][0].get<double>(), 4242.0);
        QCOMPARE(J["POS"][1].get<double>(), 2424.0);
    }

    // The two properties the publish split exists for. Both were unpinned: re-inserting the stamp into
    // PublishPackage, or dropping the has-a-layout gate, left the whole suite green — and four consecutive
    // fix-ups in this changeset each introduced a regression, so the fixes themselves get the tests.

    void publishingDoesNotWritePositionsIntoNodeFiles()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["NODE_ID"] = "a"; A["TYPE"] = "Group";
        writeJson(Dir.filePath("a.json"), A);

        std::string Err;
        // Dehydrated destination empty = publish in place, which is what the IPFS tab and --publish do.
        (void)PackageCatalog::PublishPackage(Dir.path().toStdString(), std::string(), &Err);

        std::ifstream In(Dir.filePath("a.json").toStdString());
        json J; In >> J;
        QVERIFY2(!J.contains("POS"),
                 "PublishPackage stamped a layout — it is reached for bundles fetched from other people, "
                 "whose bytes must keep matching the CID that serves them");
    }

    // Pins the LOOKUP CONTRACT the two authoring gates are built on: nullptr for a bundle nobody arranged,
    // non-null once one exists. It does NOT pin the gates themselves — removing `if (Local)` from
    // packageeditor.cpp or packagecatalog_publish.cpp leaves this green, because reaching either needs a live
    // PackageEditor or a RemintLibrary run (IPFS). The PublishPackage half IS pinned, by
    // publishingDoesNotWritePositionsIntoNodeFiles; the editor-side gate is currently unguarded.
    void theLayoutLookupIsNullUntilSomethingIsArranged()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json Cfg = json::object();
        QCOMPARE(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Dir.path().toStdString())),
                 (const json *)nullptr);

        Cfg["EDITORLAYOUT"] = json::object();
        Cfg["EDITORLAYOUT"][PackageCatalog::EditorLayoutKey(
            std::filesystem::path(Dir.path().toStdString()))] = json{{"a", json::array({1.0, 2.0})}};
        QVERIFY(PackageCatalog::EditorLayoutFor(Cfg, std::filesystem::path(Dir.path().toStdString())) != nullptr);
    }

private:
    QTemporaryDir *SuiteRoot = nullptr;
};

QTEST_MAIN(PackageCatalogTest)
#include "test_packagecatalog.moc"
