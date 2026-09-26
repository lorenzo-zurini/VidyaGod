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
#include "nodegraph.h"      // VerifyNodeBytes — what a receiver checks a landed node file with
#include "cid.h"
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
    NodeFixture::WriteNodes(path.toStdString(), j);
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
            const json Exec = json{{"LABEL","Play"},{"HOST","linux64"},{"EXE","run.sh"},{"TILE",{{"UID","pkg1"},{"TITLE","Game One"}}}};
            const json Node = json{{"CID","pkg1_exec"},{"LABEL","pkg1_exec"},{"VARIANT","Play"},
                                   {"LAYERS", json::array({ json{{"EXEC", json::array({ Exec })}} })}};
            writeFile(QString::fromStdString(Dest) + "/pkg1.json", Node.dump(2));
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

    // Mint stamps SOURCE (the CID) and SIZE (payload bytes) beside a content layer's file and a tile COVER, and
    // BACKFILLS SIZE on re-mint for a package that already carries a valid CID but no size, via the idempotent branch
    // (CID unchanged — the branch runs precisely because NeedsSeed's VerifyCid passed, so the bytes were not re-added).
    // A DIR layer is local content: no SOURCE is stamped (a peer cannot fetch a directory). SIZE is what lets a fetch
    // show a real % and a pre-fetch total instantly. Teeth: drop the SIZE stamp at the seed site → the layer QCOMPARE
    // fails; drop the cover branch → the cover QCOMPARE fails; drop the idempotent backfill → the re-mint QCOMPARE fails.
    void publish_stamps_and_backfills_source_size()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());   // offline (ctest env)

        QTemporaryDir Pkg; QVERIFY(Pkg.isValid());
        // (a) a ZIP layer,
        writeFile(Pkg.path() + "/game.zip", std::string(4096, 'Z'));
        writeJson(Pkg.path() + "/game.json", NodeFixture::Chain("game", {NodeFixture::Content("zip", "game.zip")}));
        // (b) a DIR layer — local only,
        QDir().mkpath(Pkg.path() + "/datadir");
        writeFile(Pkg.path() + "/datadir/a.bin", std::string(1000, 'A'));
        writeJson(Pkg.path() + "/data.json", NodeFixture::Chain("data", {NodeFixture::Content("dir", "datadir")}));
        // (c) a tile with a COVER.
        writeFile(Pkg.path() + "/cover.png", std::string(777, 'C'));
        auto Tile = NodeFixture::Tile("9001", "T");
        Tile["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"] = json{{"FILE", "cover.png"}};
        writeJson(Pkg.path() + "/tile.json", NodeFixture::Chain("tile", {Tile}));

        auto Read = [&](const QString & file) { std::ifstream in((Pkg.path() + "/" + file).toStdString()); return json::parse(in, nullptr, false); };
        auto Layer = [&](const QString & file) { return Read(file)["LAYERS"][0]; };
        auto Cover = [&]() { return Read("tile.json")["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"]; };

        // 1) Fresh mint → CID + SIZE stamped on the file and the cover; the DIR carries neither.
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        const json File = Layer("game.json"), Dir = Layer("data.json"), Cov = Cover();
        QVERIFY2(File.value("SOURCE", std::string()).rfind("baf", 0) == 0, "mint must stamp a CID");
        QCOMPARE((qulonglong)File.value("SIZE", (qulonglong)0), (qulonglong)4096);
        QVERIFY2(!Dir.contains("SOURCE") && !Dir.contains("SIZE"), "a DIR layer is never content-addressed");
        QVERIFY2(Cov.value("SOURCE", std::string()).rfind("baf", 0) == 0, "the cover is content-addressed");
        QCOMPARE((qulonglong)Cov.value("SIZE", (qulonglong)0), (qulonglong)777);
        const std::string Cid = File.value("SOURCE", std::string());
        QCOMPARE(Cid, IpfsWrapper::ComputeCid((Pkg.path() + "/game.zip").toStdString()));   // the file's own CID

        // 2) Simulate a pre-SIZE package: strip the layer's SIZE, re-mint → backfilled, CID UNCHANGED (the idempotent
        //    branch stamped SIZE without re-adding the bytes).
        {
            json N = Read("game.json");
            N["LAYERS"][0].erase("SIZE");
            writeJson(Pkg.path() + "/game.json", N);
        }
        QVERIFY2(!Layer("game.json").contains("SIZE"), "precondition: SIZE stripped");
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        QCOMPARE((qulonglong)Layer("game.json").value("SIZE", (qulonglong)0), (qulonglong)4096);   // backfilled
        QCOMPARE(Layer("game.json").value("SOURCE", std::string()), Cid);                          // same bytes → same CID

        IpfsWrapper::StopNode();
    }

    // Sharing is per PACKAGE: every bundle dir publishes as ONE UnixFS folder — its node files (<cid>.json, the
    // canonical bytes, each hashing to its name) and .package.json naming them — grouped by collection dir. There is
    // no per-node flag — a runner package, a library package and a plain game all share alike. Teeth: gate on any node
    // field and a package drops; leave a node out of the folder and the NODES/file counts fail; mint a library-level
    // block and the PublishedManifest assert fails; publish handle-bearing bytes and VerifyLanded refuses them.
    void publish_shares_every_package_as_one_folder()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());

        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        QDir().mkpath(R + "/VidyaGod/[2] Other");
        QDir().mkpath(R + "/VidyaGodRunners/wine");
        QDir().mkpath(R + "/VidyaGodLibraries/dgvoodoo");
        writeJson(R + "/VidyaGod/[1] Game/game.json",
                  NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"g_tile"}));
        writeJson(R + "/VidyaGodRunners/wine/wine.json",
                  NodeFixture::Chain("r_wine", {NodeFixture::Runner("linux", {"win32"}, "wine")}));
        writeJson(R + "/VidyaGodLibraries/dgvoodoo/dg.json",
                  NodeFixture::Chain("l_dg", {NodeFixture::Content("zip", "dgvoodoo.zip")}));
        // [2] Other carries content that is really seeded here: its PIN folder must link it.
        writeFile(R + "/VidyaGod/[2] Other/u.zip", std::string(3000, 'U'));
        const std::string UCid = IpfsWrapper::AddNoCopy((R + "/VidyaGod/[2] Other/u.zip").toStdString(), &Err);
        QVERIFY2(!UCid.empty(), Err.c_str());
        writeJson(R + "/VidyaGod/[2] Other/u.json",
                  NodeFixture::Chain("u_exec", {NodeFixture::Merge({NodeFixture::ContentCid("zip", "u.zip", UCid), NodeFixture::Exec("win32", "u.exe")})}));
        writeJson(R + "/VidyaGod/[1] Game/tile.json", NodeFixture::Chain("g_tile", {NodeFixture::Tile("1", "Game")}));

        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        std::string Gaps = "unset";
        const std::vector<std::string> Published = PackageCatalog::PublishLibrary(cfg, &Err, &Gaps);
        QVERIFY2(Gaps.empty(), Gaps.c_str());                 // everything held, everything froze: a clean verdict

        QVERIFY2(cfg.contains("Libraries") && cfg["Libraries"].is_object(), "PublishLibrary writes config[\"Libraries\"]");
        const auto & Libs = cfg["Libraries"];
        QCOMPARE((int)Libs["VidyaGod"].size(), 2);          // BOTH packages — nothing is gated
        QCOMPARE((int)Libs["VidyaGodRunners"].size(), 1);
        QCOMPARE((int)Libs["VidyaGodLibraries"].size(), 1);
        QCOMPARE((int)cfg["PublishedList"].size(), 4);      // one folder per package
        QCOMPARE((int)Published.size(), 4);
        QVERIFY2(!cfg.contains("PublishedManifest"), "no library-level block: a library is a name, never a CID");

        // The package folder: every node file of the package, verbatim, plus the manifest naming them.
        const json * G = nullptr;
        for (const auto & E : Libs["VidyaGod"]) if (E.value("pkg", std::string()) == "[1] Game") G = &E;
        QVERIFY2(G, "the entry names the seeder's real package dir");
        QCOMPARE(G->value("nodes", 0), 2);
        const auto Files = folderFiles(G->value("cid", std::string()));
        QCOMPARE((int)Files.size(), 3);                      // two nodes + .package.json
        const json M = json::parse(Files.at(".package.json"), nullptr, false);
        QVERIFY2(M.is_object() && M.contains("NODES") && M["NODES"].is_array(), "the manifest reads back");
        QCOMPARE((int)M["NODES"].size(), 2);                 // tile + exec: every node of the package
        QCOMPARE(M.value("PKG", std::string()), std::string("[1] Game"));
        for (const auto & C : M["NODES"])
        {
            const std::string Name = C.get<std::string>() + ".json";
            QVERIFY2(Files.count(Name), "each named node is a file of the folder");
            json J;
            std::string VErr;
            QVERIFY2(NodeGraph::VerifyNodeBytes(Files.at(Name), C.get<std::string>(), J, &VErr), VErr.c_str());   // canonical, handle-free, its own CID
        }
        // The PIN folder (what a pinning service pins): the package folder, and the package's own content beside it —
        // none for [1] Game (no content), [2] Other's zip for it.
        QTemporaryDir PinDir;
        std::string FErr;
        QVERIFY2(!IpfsWrapper::FetchDirToPath(G->value("pin", std::string()), (PinDir.path() + "/g").toStdString(), &FErr).empty(), FErr.c_str());
        QVERIFY(QFile::exists(PinDir.path() + "/g/package/.package.json"));
        QVERIFY2(!QDir(PinDir.path() + "/g/content").exists(), "no content: no content folder");
        const json * U = nullptr;
        for (const auto & E : Libs["VidyaGod"]) if (E.value("pkg", std::string()) == "[2] Other") U = &E;
        QVERIFY(U && U->value("pin", std::string()) != U->value("cid", std::string()));
        QVERIFY2(!IpfsWrapper::FetchDirToPath(U->value("pin", std::string()), (PinDir.path() + "/u").toStdString(), &FErr).empty(), FErr.c_str());
        QCOMPARE(QFileInfo(PinDir.path() + "/u/content/" + QString::fromStdString(UCid)).size(), (qint64)3000);   // the content, linked
        QVERIFY(QFile::exists(PinDir.path() + "/u/package/.package.json"));

        // What did not publish whole is in the verdict, never only in a scrolled-past warning: a package naming
        // content this machine does not hold, and a node that cannot freeze (a cycle), are both named.
        QDir().mkpath(R + "/VidyaGod/[3] Gap");
        writeJson(R + "/VidyaGod/[3] Gap/gap.json", NodeFixture::Chain("gap_exec", {NodeFixture::Merge({
                      NodeFixture::ContentCid("zip", "g.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku"),
                      NodeFixture::Exec("win32", "g.exe")})}));
        writeJson(R + "/VidyaGod/[3] Gap/loop.json", json{{"CID", "hLoop"}, {"LABEL", "loop"},
                                                          {"LAYERS", json::array({ {{"NODE", "hLoop"}} })}});
        PackageCatalog::PublishLibrary(cfg, &Err, &Gaps);
        QVERIFY2(Gaps.find("[3] Gap (1)") != std::string::npos, Gaps.c_str());
        QVERIFY2(Gaps.find("did not freeze") != std::string::npos && Gaps.find("[3] Gap/loop") != std::string::npos, Gaps.c_str());
        QVERIFY2(Gaps.find("[2] Other") == std::string::npos, "held content is not a gap");
        IpfsWrapper::StopNode();
    }

    // One entry = one package folder, landing in <pkg dir>/.package. Two folders naming the same package dir (only
    // a hostile or a mid-change snapshot does that) both land, the second under a CID-qualified dir; the same CID
    // twice is one target.
    void planner_lands_one_folder_per_package()
    {
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", "/tmp/vg_rx_plan/LIBRARY"}}}}}};
        const json Items = json::array({
            json{{"cid", "bafypkgone"}, {"pkg", "[802] Warcraft III"}},
            json{{"cid", "bafypkgtwo"}, {"pkg", "[802] Warcraft III"}},
            json{{"cid", "bafypkgone"}, {"pkg", "[802] Warcraft III"}} });
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 2);
        QCOMPARE(std::filesystem::path(Plan[0].Dest).filename(), std::filesystem::path(".package"));
        QCOMPARE(std::filesystem::path(Plan[0].Dest).parent_path().filename(), std::filesystem::path("[802] Warcraft III"));
        QVERIFY2(Plan[1].Dest.find("[802] Warcraft III (bafypkgtwo") != std::string::npos, "the colliding package's dir carries its CID");
    }

    // The files of a published folder: name → bytes (fetched like a receiver would, into a throwaway dir).
    std::map<std::string, std::string> folderFiles(const std::string & FolderCid)
    {
        std::map<std::string, std::string> Out;
        QTemporaryDir T;
        std::string Err;
        const std::string Dir = (T.path() + "/f").toStdString();
        if (IpfsWrapper::FetchDirToPath(FolderCid, Dir, &Err).empty()) return Out;
        for (const auto & F : std::filesystem::directory_iterator(Dir))
        {
            std::ifstream In(F.path(), std::ios::binary);
            Out[F.path().filename().string()] = std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
        }
        return Out;
    }

    // Land a friend's share the way the app does: each package folder through the real queue, then complete + check.
    void landShares(const json & Rx, const std::string & Nick, const json & Libs)
    {
        std::vector<IpfsWrapper::FetchTarget> B;
        for (const auto & T : PackageCatalog::PlanReceivedFetches(Rx, Nick, Libs))
            B.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, /*Optional=*/false, /*Dir=*/true, /*Verify=*/true });
        std::string WErr;
        QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(B), 30000, &WErr), WErr.c_str());
        std::string LErr;
        QVERIFY2(PackageCatalog::LandReceivedPackages(Rx, &LErr), LErr.c_str());
    }

    // Helper: publish two games in one collection and return the share entries — one per PACKAGE, as
    // PublishLibrary emits them ({cid: <package folder>, pkg, node, title, nodes}).
    json publishTwoGames(const QString & root)
    {
        std::string Err;
        QDir().mkpath(root + "/VidyaGod/[1] A");
        QDir().mkpath(root + "/VidyaGod/[2] B");
        writeJson(root + "/VidyaGod/[1] A/tile.json", NodeFixture::Chain("a_tile", {NodeFixture::Tile("1", "A")}));
        // Real games carry content in PARENT nodes, TWO levels below the exec: the whole package lands, closure and all.
        writeJson(root + "/VidyaGod/[1] A/base.json",
                  NodeFixture::Chain("a_base", {[]{
                      auto L = NodeFixture::ContentCid("file", "base.bin",
                          "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e");
                      L["LAYERS"][0]["SIZE"] = 4096;   // stamped size — the dialog's instant size derivation reads THIS
                      return L;
                  }()}));
        writeJson(root + "/VidyaGod/[1] A/content.json",
                  NodeFixture::Chain("a_content", {NodeFixture::ContentCid("file", "data.bin",
                      "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e")}, {"a_base"}));
        writeJson(root + "/VidyaGod/[1] A/a.json",
                  NodeFixture::Chain("a_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "a.exe"), NodeFixture::Variant("Play")})}, {"a_content", "a_tile"}));
        // An EXPANSION of A: its own tile (same UID, another title), OVER the main launchable. Nesting is derived
        // from that edge — on the seeder and, once the package lands, on the receiver.
        writeJson(root + "/VidyaGod/[1] A/ax.json",
                  NodeFixture::Chain("a_x", {NodeFixture::Merge({NodeFixture::Exec("win32", "ax.exe"), NodeFixture::Tile("1x", "A: Expansion", nlohmann::ordered_json::object(), "1"), NodeFixture::Variant("Expansion")})},
                                     {"a_exec"}));
        writeJson(root + "/VidyaGod/[2] B/tile.json", NodeFixture::Chain("b_tile", {NodeFixture::Tile("2", "B")}));
        writeJson(root + "/VidyaGod/[2] B/b.json",
                  NodeFixture::Chain("b_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "b.exe"), NodeFixture::Variant("Play")})}, {"b_tile"}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", root.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder, &Err);
        return seeder["Libraries"].contains("VidyaGod") ? seeder["Libraries"]["VidyaGod"] : json::array();
    }

    // The cid of the node labelled `Label` inside a package folder (its node files are named by their CIDs).
    std::string cidOfLabel(const std::string & FolderCid, const std::string & Label)
    {
        for (const auto & [Name, Bytes] : folderFiles(FolderCid))
        {
            const json B = json::parse(Bytes, nullptr, false);
            if (B.is_object() && B.value("LABEL", std::string()) == Label) return Name.substr(0, Name.size() - 5);
        }
        return {};
    }

    // A received share is a set of PACKAGE folders: the planner lands each at "<Nick> - <Lib>/<pkg>/.package" through
    // the rolling queue — every node file verbatim, under its CID — and from there zero friend-specific code runs:
    // the plain catalog scan indexes the landed bytes, CID identity survives, the whole package is Received with its
    // bundle the package dir, its closure is complete, and the card nests exactly as on the seeder. Hostile pkg/node
    // must never escape the CATALOG root.
    void received_share_lands_at_final_library_paths()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());   // blocks now in the local blockstore
        QCOMPARE((int)Items.size(), 2);                         // packages A and B

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);   // sibling of LIBRARY → RxData/CATALOG

        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 2);
        const std::string Lib = Catalog + "/Alice - Games";     // received stubs land in CATALOG, not LIBRARY
        std::set<std::string> Dests;
        for (const auto & T : Plan) Dests.insert(T.Dest);
        QVERIFY2(Dests.count(Lib + "/[1] A/.package"), "a package's folder lands in the seeder's package dir name");
        QVERIFY2(Dests.count(Lib + "/[2] B/.package"), "second package gets its own dir");

        landShares(rx, "Alice", json{{"Games", Items}});   // the folders land through the REAL queue
        QVERIFY2(!PackageCatalog::ReceivedPackagesIncomplete(rx), "every node file the manifests name is on disk");
        const std::string ACid = cidOfLabel(Items[0].value("cid", std::string()), "a_exec");
        {
            std::ifstream In(Lib + "/[1] A/.package/" + ACid + ".json", std::ios::binary);
            const std::string Landed((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            QCOMPARE(Cid::OfBytes(Landed), ACid);                // landed VERBATIM: the file is the block
        }

        auto Idx = PackageCatalog::BuildCatalogIndex(rx);
        const Node * A = Idx.Find("a_exec");
        QVERIFY2(A, "a landed raw block freezes back in the plain tree scan");
        QCOMPARE(A->BundleDir, std::filesystem::path(Lib + "/[1] A"));   // content hydrates beside the folder
        QCOMPARE(A->Key(), ACid);
        QVERIFY2(A->Received, "…marked RECEIVED (it lives under CATALOG): never a graft candidate");
        for (const auto & [K, N] : Idx.Nodes)
            if (N.BundleDir.string().rfind(Catalog, 0) != 0) QVERIFY2(!N.Received, "a LIBRARY node is not Received");
        QVERIFY2(!PackageCatalog::NodeClosureIncomplete(Idx, A->Key()), "the whole package landed: the closure is complete");
        int LandedA = 0;
        for (const auto & [K, N] : Idx.Nodes)
            if (N.Received && N.BundleDir.string().rfind(Lib + "/[1] A", 0) == 0) ++LandedA;
        QCOMPARE(LandedA, 5);                                   // a_exec, a_x, a_tile, a_content, a_base — all Received

        // Vacuous-hydration guard: an un-installed received game (content not fetched) reads NOT hydrated —
        // that is what routes it to the Catalog tab (downloadable) instead of the Library tab (playable).
        const auto Hyd = PackageCatalog::HydrationMap(Idx);
        const auto Hit = Hyd.find(A->Key());
        QVERIFY2(Hit != Hyd.end() && !Hit->second.Hydrated, "a received, un-installed package must not count as hydrated");

        // The shelf derives exactly as on the seeder: the expansion is its own tile, a child of the base game's
        // (PARENTUID) — its own card, right after its family's base game — and each tile lists its own row.
        const Node * X = Idx.Find("a_x");
        QVERIFY(X);
        QCOMPARE(X->Uid, std::string("1x"));
        QCOMPARE(X->PackageUid, std::string("1"));
        QCOMPARE(A->Uid, std::string("1"));
        QCOMPARE(A->Meta.value("TITLE", std::string()), std::string("A"));
        std::vector<std::string> Order;
        std::map<std::string, std::vector<std::string>> RowsOf;
        for (const auto & T : PackageCatalog::ShelfTiles(Idx))
        {
            Order.push_back(T.Uid);
            for (const Node * N : T.Rows) RowsOf[T.Uid].push_back(N->NodeId);
        }
        const auto Base = std::find(Order.begin(), Order.end(), std::string("1"));
        QVERIFY2(Base != Order.end() && Base + 1 != Order.end() && *(Base + 1) == "1x", "the expansion's tile follows its base game's");
        QCOMPARE(RowsOf["1"], std::vector<std::string>{"a_exec"});
        QCOMPARE(RowsOf["1x"], std::vector<std::string>{"a_x"});

        // A HOSTILE snapshot (traversal in every routed field) must stay inside the CATALOG root.
        const json Evil = json::array({json{{"cid", Items[0].value("cid", std::string())}, {"node", "../../pwn"}, {"pkg", "../../.."}}});
        const auto EvilPlan = PackageCatalog::PlanReceivedFetches(rx, "..", json{{"..", Evil}});
        QVERIFY(!EvilPlan.empty());
        const std::string RootPrefix = Catalog + "/";
        for (const auto & T : EvilPlan)
        {
            const std::string Canon = std::filesystem::weakly_canonical(T.Dest).string();
            QVERIFY2(Canon.rfind(RootPrefix, 0) == 0, ("hostile dest escaped the CATALOG root: " + Canon).c_str());
        }

        // Planner dedupe: the same snapshot plans the same targets again (stable), never duplicates within one plan.
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan2.size(), 2);
        std::set<std::string> D2; for (const auto & T : Plan2) D2.insert(T.Dest);
        QCOMPARE(D2.size(), Plan2.size());

        // CATALOG stubs are BROWSE-ONLY: publish scans LIBRARY only, so a re-publish must NOT include a received
        // package — you re-share what you've installed, not everything you've browsed.
        std::string PubErr;
        PackageCatalog::PublishLibrary(rx, &PubErr);
        QVERIFY2(!rx.value("Libraries", json::object()).contains("Alice - Games"),
                 "a CATALOG browse stub is NOT published (LIBRARY-only publish; browse != share)");

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A hostile hydrated block with non-typed fields (PUBLISH:"yes", LIBRARYITEM:5, TITLE:{}) must NOT crash the
    // publish walk. The blocks land in LIBRARY (as a hostile friend's added game would); PublishLibrary + Mint run
    // over them and must complete without throwing (the reads are all is-type guarded). Teeth: unguard any of
    // PUBLISH/LIBRARYITEM/UID/TITLE and this aborts (std::terminate in the real app's detached mint thread).
    void publish_tolerates_hostile_field_types()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Evil");
        // Every identity/flag field carries a hostile non-matching type.
        writeJson(R + "/VidyaGod/[1] Evil/e.json",
                  json{{"TYPE", "DeclareExec"}, {"HOST", "win32"}, {"LABEL", "Evil"},
                       {"PUBLISH", "yes"}, {"LIBRARYITEM", 5}, {"TITLE", json::object()}});
        // …plus a well-formed shared game so the walk actually does work alongside the hostile node.
        QDir().mkpath(R + "/VidyaGod/[2] Good");
        writeJson(R + "/VidyaGod/[2] Good/tile.json", NodeFixture::Chain("gd_tile", {NodeFixture::Tile("2", "Good")}));
        writeJson(R + "/VidyaGod/[2] Good/g.json",
                  NodeFixture::Chain("gd_exec", {NodeFixture::Exec("win32", "g.exe")}, {"gd_tile"}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);   // MUST NOT throw
        QVERIFY2(cfg.contains("Libraries"), "publish completed over a hostile block without crashing");
        // The hostile legacy-typed object is not a node: its package has no nodes and no manifest; only Good publishes.
        int shared = 0;
        for (const auto & E : cfg.value("Libraries", json::object()).value("VidyaGod", json::array()))
            if (E.value("pkg", std::string()) == "[2] Good") shared++;
        QCOMPARE(shared, 1);
        QCOMPARE((int)cfg["Libraries"]["VidyaGod"].size(), 1);
        IpfsWrapper::StopNode();
    }

    // A PUBLISH'd NAMELESS node IS shareable under Model C: identity is the CID, so a label-less node stands under its
    // CID handle rather than a synthetic path key — the old path-leak that forced a mandatory LABEL is gone. The
    // property that must hold is that NO share entry carries a filesystem path (uid/node/title). Teeth: reintroduce a
    // synthetic-path handle and the assert below catches the '/' it leaks.
    void publish_skips_nameless_flagged_node()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Named");
        QDir().mkpath(R + "/VidyaGod/[2] Nameless");
        writeJson(R + "/VidyaGod/[1] Named/tile.json", NodeFixture::Chain("n_tile", {NodeFixture::Tile("1", "Named")}));
        writeJson(R + "/VidyaGod/[1] Named/g.json",
                  NodeFixture::Chain("n_exec", {NodeFixture::Exec("win32", "g.exe")}, {"n_tile"}));
        // A nameless node (no LABEL): a content node in a package of its own.
        writeJson(R + "/VidyaGod/[2] Nameless/c.json",
                  json{{"LAYERS", json::array({ json{{"FORM", "zip"}, {"PATH", "c.zip"},
                           {"SOURCE", {{"TYPE","ipfs"},{"CID","bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e"}}}} })}});
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);
        QVERIFY(cfg.contains("Libraries") && cfg["Libraries"].contains("VidyaGod"));
        // Model C: a nameless PUBLISH'd node is safe to share — its handle is its CID (identity), so it stands under
        // that CID, never a filesystem path. THE property that matters is the absence of a path leak in ANY entry
        // (uid/node/title), not the exclusion of nameless nodes. Teeth: the old synthetic-key leak would put the
        // seeder's bundle PATH in uid/node here.
        bool sawNamed = false, sawNameless = false;
        for (const auto & E : cfg["Libraries"]["VidyaGod"])
        {
            const std::string node = E.value("node", std::string()), pkg = E.value("pkg", std::string());
            const std::string Msg = "no filesystem path in a share entry: node=" + node + " pkg=" + pkg;
            QVERIFY2(node.find('/') == std::string::npos && pkg.find('/') == std::string::npos, Msg.c_str());
            if (pkg == "[1] Named") sawNamed = true;
            if (pkg == "[2] Nameless") { sawNameless = true; QCOMPARE(E.value("nodes", 0), 1); }
        }
        QVERIFY2(sawNamed, "the named game's package publishes");
        QVERIFY2(sawNameless, "the nameless node's package publishes — a manifest links it under its CID");
        IpfsWrapper::StopNode();
    }

    // CATALOG is a scanned root beside LIBRARY: a stub placed in CATALOG shows up in the catalog index, and a
    // LIBRARY package's cross-reference to it resolves through the MERGED index. Teeth: drop the CATALOG
    // GatherWorkingTree in BuildCatalogIndex and the CATALOG node is absent from the scan.
    void catalog_dir_is_a_scanned_root_beside_library()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Root.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(cfg);
        const std::string Library = PackageCatalog::LibraryRootDir(cfg);
        // One node in LIBRARY, one in CATALOG — both must appear in the single merged index.
        QDir().mkpath(QString::fromStdString(Library) + "/VidyaGod/[1] Mine");
        QDir().mkpath(QString::fromStdString(Catalog) + "/Alice - Games/[2] Theirs");
        writeJson(QString::fromStdString(Library) + "/VidyaGod/[1] Mine/m.json",
                  NodeFixture::Chain("mine", {NodeFixture::Exec("win32", "m.exe")}));
        writeJson(QString::fromStdString(Catalog) + "/Alice - Games/[2] Theirs/t.json",
                  NodeFixture::Chain("theirs", {NodeFixture::Exec("win32", "t.exe")}));
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(cfg);
        bool haveMine = false, haveTheirs = false;
        for (const auto & [C, N] : Idx.Nodes) { if (N.NodeId == "mine") haveMine = true; if (N.NodeId == "theirs") haveTheirs = true; }
        QVERIFY2(haveMine, "LIBRARY node is scanned");
        QVERIFY2(haveTheirs, "CATALOG node is scanned into the SAME index (drop the CATALOG root → this fails)");
    }

    // A node that does not freeze (here: a cycle — each needs the other's CID first) is skipped, and so is every node
    // referencing it: its frozen bytes would carry a working-tree handle nothing holds. The rest of the library stands.
    // Teeth: stop FreezeNodeJson reporting refs to unfrozen tree nodes and a, b and user index with raw handles.
    void a_ref_to_a_node_that_did_not_freeze_skips_its_referrer()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Root.path() + "/LIBRARY").toStdString()}}}}}};
        const QString Pkg = QString::fromStdString(PackageCatalog::LibraryRootDir(cfg)) + "/VidyaGod/[1] P";
        QDir().mkpath(Pkg);
        const auto node = [&](const std::string &Handle, const std::string &Label, json Layers) {
            writeJson(Pkg + "/" + QString::fromStdString(Label) + ".json",
                      json{{"CID", Handle}, {"LABEL", Label}, {"LAYERS", Layers}});
        };
        node("hA", "a", json::array({ {{"NODE", "hB"}} }));
        node("hB", "b", json::array({ {{"NODE", "hA"}} }));
        node("hU", "user", json::array({ {{"NODE", "hA"}}, {{"ZIP", "u.zip"}} }));
        node("hG", "good", json::array({ {{"ZIP", "g.zip"}} }));
        const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(cfg);
        std::set<std::string> Labels;
        for (const auto & [C, N] : Idx.Nodes) Labels.insert(N.NodeId);
        QCOMPARE(Labels, (std::set<std::string>{"good"}));
    }

    // Install on a RECEIVED card whose closure reaches outside its package: the folder brings the package's own nodes,
    // but a node it contains that lives in ANOTHER package (a library nobody shared) is missing. CompleteClosure
    // fetches it — a plain FetchTarget through the SAME rolling queue, wave by wave — into the package dir, verified
    // and CID-named; a fresh scan then resolves real content targets and sizes. Teeth: break the wave loop, the queue
    // routing, the verification or the frontier discovery and the asserts below fail.
    void install_completes_a_received_closure_via_the_queue()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QCOMPARE((int)Items.size(), 2);                         // packages A and B
        // The seeder's library package: a node of its own, published with the library but NOT shared below.
        const std::string LibFolder = [&]{
            json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", SeedRoot.path().toStdString()}}}}}};
            QDir().mkpath(SeedRoot.path() + "/VidyaGodLibraries/lib");
            writeJson(SeedRoot.path() + "/VidyaGodLibraries/lib/lib.json",
                      NodeFixture::Chain("a_lib", {NodeFixture::ContentCid("zip", "lib.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku")}));
            std::string E;
            PackageCatalog::PublishLibrary(seeder, &E);
            return seeder["Libraries"]["VidyaGodLibraries"][0].value("cid", std::string());
        }();
        QVERIFY(!LibFolder.empty());
        const std::string LibCid = cidOfLabel(LibFolder, "a_lib");
        QVERIFY(!LibCid.empty());

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        // A received package whose game contains a_lib (by CID) — a package of its own the friend shared, a_lib not.
        {
            const std::string Pkg = Catalog + "/Alice - Games/[3] C/.package";
            std::filesystem::create_directories(Pkg);
            const json Game = json{{"LABEL", "c_exec"}, {"VARIANT", "Play"}, {"LAYERS", json::array({
                json{{"NODE", LibCid}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", "win32"}, {"EXE", "c.exe"}, {"TILE", {{"UID", "3"}, {"TITLE", "C"}}}} })}} })}};
            const std::string Bytes = Cid::Canonical(Game);
            const std::string GCid = Cid::OfBytes(Bytes);
            { std::ofstream O(Pkg + "/" + GCid + ".json", std::ios::binary); O << Bytes; }
            { std::ofstream O(Pkg + "/.package.json"); O << json{{"NODES", json::array({GCid})}, {"PKG", "[3] C"}}.dump(); }
        }
        NodeIndex Before = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY2(PackageCatalog::NodeClosureIncomplete(Before, "c_exec"), "precondition: the library node is missing");
        QVERIFY2(PackageCatalog::CompleteClosure(Before, "c_exec", &Err), Err.c_str());
        const QString PkgDir = QString::fromStdString(Catalog) + "/Alice - Games/[3] C";
        QVERIFY2(QFile::exists(PkgDir + "/" + QString::fromStdString(LibCid) + ".json"), "the library node landed CID-named in the package dir");

        NodeIndex Fresh = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY2(!PackageCatalog::NodeClosureIncomplete(Fresh, "c_exec"), "the closure is complete after the fetch");
        QVERIFY2(!PackageCatalog::NodeContentCids(Fresh, "c_exec").empty(),
                 "content targets (and thus sizes) now resolve — the Download button has something to do");

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // INSTALL = ADOPT: a received package moves out of CATALOG into LIBRARY/<lib>/<pkg> — an ordinary local package
    // from then on (not Received: launchable, grafts offered, published with the library). The planner then never
    // re-lands it as a stub beside itself, and a name collision is refused, never merged.
    void install_adopts_a_received_package_into_the_library()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QCOMPARE((int)Items.size(), 2);
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"FriendLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};   // nick = peer tail "derAlice"
        const std::string Nick = "derAlice";
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        landShares(rx, Nick, json{{"Games", Items}});
        const std::filesystem::path Stub = std::filesystem::path(Catalog) / (Nick + " - Games") / "[1] A";
        QVERIFY(std::filesystem::exists(Stub / ".package" / ".package.json"));

        std::filesystem::path NewDir;
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Stub, &NewDir, &Err), Err.c_str());
        QCOMPARE(NewDir, std::filesystem::path(RxData.path().toStdString()) / "LIBRARY" / "Games" / "[1] A");
        QVERIFY2(!std::filesystem::exists(Stub), "the stub dir is gone from CATALOG");
        QVERIFY2(std::filesystem::exists(NewDir) && !std::filesystem::exists(NewDir / ".package"), "moved, the landed folder dissolved");
        const std::string ACid = cidOfLabel(Items[0].value("cid", std::string()), "a_exec");
        QVERIFY2(std::filesystem::exists(NewDir / (ACid + ".json")), "its node files are the package's own now");
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        const Node * A = Idx.Find("a_exec");
        QVERIFY(A);
        QVERIFY2(!A->Received, "an installed package is ours: never 'Received' again");
        QCOMPARE(A->BundleDir, NewDir);
        QVERIFY2(Idx.Find("b_exec") && Idx.Find("b_exec")->Received, "the un-installed package stays a stub");
        // The planner skips the adopted package (the library holds it) and still plans the other.
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, Nick, json{{"Games", Items}});
        QCOMPARE((int)Plan2.size(), 1);
        QVERIFY(Plan2[0].Dest.find("[2] B") != std::string::npos);
        // A second adopt of a package whose name the library already holds is refused.
        std::filesystem::create_directories(Stub);
        { std::ofstream S(Stub / ".package.json"); S << "{}"; }
        QVERIFY(!PackageCatalog::AdoptReceivedPackage(rx, Stub, nullptr, &Err));
        QVERIFY(Err.find("already exists") != std::string::npos);
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // The library NAME in a friend's snapshot is theirs to choose. Installing a received package must land it inside
    // LIBRARY whatever that name holds — "../..", an absolute path, "..". Teeth: drop the adopt's sanitising and a
    // package lands outside the library.
    void adopting_under_a_hostile_library_name_stays_in_the_library()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        for (const std::string Lib : {std::string("../../escape"), std::string("/tmp/vg_adopt_escape"), std::string("..")})
        {
            QTemporaryDir RxData; QVERIFY(RxData.isValid());
            json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                           {"FriendLibraries", {{"12D3KooWSeederAlice", {{Lib, Items}}}}}};
            landShares(rx, "derAlice", json{{Lib, Items}});
            const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "derAlice", json{{Lib, Items}});
            QVERIFY(!Plan.empty());
            const std::filesystem::path Stub = std::filesystem::path(Plan.front().Dest).parent_path();
            std::filesystem::path NewDir;
            const bool Ok = PackageCatalog::AdoptReceivedPackage(rx, Stub, &NewDir, &Err);
            const std::string Library = std::filesystem::weakly_canonical(RxData.path().toStdString() + "/LIBRARY").string();
            if (Ok)
                QVERIFY2(std::filesystem::weakly_canonical(NewDir).string().rfind(Library + "/", 0) == 0,
                         ("installed outside the library: " + NewDir.string()).c_str());
            QVERIFY2(!std::filesystem::exists("/tmp/vg_adopt_escape"), "a package landed at an absolute library name");
            QVERIFY2(!std::filesystem::exists(RxData.path().toStdString() + "/../escape"), "a package landed above the library");
        }
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A RE-PUBLISHED package (same dir → same dest, a NEW folder) must land OVER the occupied dest through the REAL
    // rolling queue — the receiver's whole update path — at EVERY dest the snapshot routes it to. The folder dir is
    // the fetch's own, replaced whole, so the previous generation's node files go with it. Teeth: let the queue
    // settle a new-CID job on the occupied dest and the v2 asserts fail (the dest keeps v1 forever).
    void republished_package_lands_over_the_stale_dest()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QCOMPARE((int)Items.size(), 2);                         // packages A and B
        const std::string FolderV1 = Items[0].value("cid", std::string());
        const std::string CidV1 = cidOfLabel(FolderV1, "a_exec");
        QVERIFY(!CidV1.empty());

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        // TWO libraries carry the same items (the multi-dest normal: one CID job, several dests).
        auto Land = [&](const json & SnapItems) { landShares(rx, "Alice", json{{"Games", SnapItems}, {"Favs", SnapItems}}); };
        auto Has = [&](const std::string & Lib, const std::string & Cid) {
            return std::filesystem::exists(Catalog + "/Alice - " + Lib + "/[1] A/.package/" + Cid + ".json");
        };
        auto Names = [&](const std::string & Lib, const std::string & Cid) {
            std::ifstream In(Catalog + "/Alice - " + Lib + "/[1] A/.package/.package.json");
            const json M = json::parse(In, nullptr, false);
            if (!M.is_object() || !M.contains("NODES")) return false;
            for (const auto & C : M["NODES"]) if (C == Cid) return true;
            return false;
        };
        Land(Items);
        QVERIFY2(Names("Games", CidV1) && Names("Favs", CidV1), "v1's manifest landed in both libraries");
        QVERIFY2(Has("Games", CidV1) && Has("Favs", CidV1), "v1's node file landed in both");

        // Seeder updates the node (same LABEL, new content) and re-publishes → NEW cid, NEW folder, SAME dest.
        {
            const std::string F = (SeedRoot.path() + "/VidyaGod/[1] A/a.json").toStdString();
            std::ifstream In(F);
            json J; In >> J; In.close();
            J["LAYERS"].push_back(json{{"ENV", {{"V2", "yes"}}}});
            std::ofstream Out(F);
            Out << J.dump(2);
        }
        json seeder2 = json{{"Settings", {{"Paths", {{"LibraryRoot", SeedRoot.path().toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder2, &Err);
        json Items2 = seeder2["Libraries"]["VidyaGod"];
        std::string FolderV2;
        for (const auto & E : Items2) if (E.value("pkg", std::string()) == "[1] A") FolderV2 = E.value("cid", std::string());
        const std::string CidV2 = cidOfLabel(FolderV2, "a_exec");
        QVERIFY2(!FolderV2.empty() && FolderV2 != FolderV1, "the package re-minted to a NEW folder");
        QVERIFY2(!CidV2.empty() && CidV2 != CidV1, "the update re-minted the node to a NEW cid");

        Land(Items2);
        QVERIFY2(Names("Games", CidV2) && Names("Favs", CidV2), "v2's manifest REPLACED the stale one at EVERY dest");
        QVERIFY2(Has("Games", CidV2) && Has("Favs", CidV2), "v2's node file landed in both");
        QVERIFY2(!Has("Games", CidV1) && !Has("Favs", CidV1), "the stale v1 file went with the replaced folder");
        NodeIndex After = PackageCatalog::BuildCatalogIndex(rx);
        QCOMPARE(PackageCatalog::PruneStaleReceived(After, rx), 0);   // nothing stale left in a folder dir

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // UNTRUSTED-input bounds of the planner, with teeth for each: (1) every dest path SEGMENT is capped under
    // NAME_MAX even when nick/lib/uid/title/node arrive at the wire maximums (else the queue retries an
    // ENAMETOOLONG mkdir forever); (2) an over-long cid is dropped; (3) the per-library item cap holds.
    void planner_bounds_hostile_segments_and_counts()
    {
        QTemporaryDir PlanRoot; QVERIFY(PlanRoot.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (PlanRoot.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        const std::string Long(1000, 'x');
        const json Items = json::array({ json{{"cid", "bafyplannerboundscidaaaaaaaaaaaaaaaaaaaaaa"},
                                              {"node", Long}, {"uid", Long}, {"title", Long}} });
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, Long, json{{Long, Items}});
        QCOMPARE((int)Plan.size(), 1);
        for (const auto & T : Plan)
            for (const auto & Part : std::filesystem::path(T.Dest.substr(Catalog.size() + 1)))
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
                  json{{"LABEL", "h_exec"}, {"TYPE", "DeclareExec"}, {"HOST", "win32"}, {"EXECUTABLE", "h.exe"},
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
                  json::array({ json{{"LABEL", "friendevil"}, {"TYPE", "DeclareExec"}, {"HOST", "win32"},
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
        // Model C: the layout override + warnings key on the node's stored CID HANDLE, so each fixture node carries
        // one (= its readable name here); NODE layers reference those handles.
        // Its OWN POS is impossible: publish computes one and writes it, changing the file.
        Write("bad.json",  nlohmann::ordered_json{{"CID", "bad"}, {"LABEL", "bad"},  
                                                  {"POS", nlohmann::ordered_json::array({5e9, 5e9})}, {"LAYERS", nlohmann::ordered_json::array()}});
        // A good POS with an impossible LOCAL override: the node keeps its POS and nothing is rewritten.
        Write("keep.json", nlohmann::ordered_json{{"CID", "keep"}, {"LABEL", "keep"}, 
                                                  {"POS", nlohmann::ordered_json::array({60.0, 60.0})},
                                                  {"LAYERS", nlohmann::ordered_json::array({ nlohmann::ordered_json{{"NODE", "bad"}} })}});
        // NO POS of its own, plus an impossible local override. This is the case where the source label and
        // the outcome DIVERGE: the label says "this machine's saved layout", which sounds like nothing in the
        // package changed — but with no POS to fall back on the layout supplies one, the file GAINS a POS it
        // never had, and the Meta-CID moves. Both earlier versions of this line got this case wrong, and a
        // test built only from the two agreeing cases could not tell the difference.
        Write("fresh.json", nlohmann::ordered_json{{"CID", "fresh"}, {"LABEL", "fresh"}, 
                                                   {"LAYERS", nlohmann::ordered_json::array({ nlohmann::ordered_json{{"NODE", "bad"}} })}});
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
        //Both share the handle "same" (Model C: the CID handle is the identity the override + warnings key on; the
        //point of this test is two nodes with the SAME handle). The first carries an impossible POS, so publish
        //computes one and REWRITES it.
        Write("a.json", nlohmann::ordered_json{{"CID", "same"}, {"LABEL", "same"}, 
                                               {"POS", nlohmann::ordered_json::array({5e9, 5e9})}, {"LAYERS", nlohmann::ordered_json::array()}});
        //The second carries a POS the layout would produce anyway, plus an impossible local OVERRIDE. It is
        //refused like the first, but its file is left exactly as it was — 60,60 is where the layout puts the
        //first node of the first layer, so "already correct: do not touch the bytes" fires.
        Write("b.json", nlohmann::ordered_json{{"CID", "same"}, {"LABEL", "same"}, 
                                               {"POS", nlohmann::ordered_json::array({60.0, 60.0})}, {"LAYERS", nlohmann::ordered_json::array()}});
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

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
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

        NodeIndex idx0; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx0); ManifestModel::DeriveFacts(idx0);
        QVERIFY(!PackageCatalog::NodeHydrated(idx0, "remote"));     // CID-backed blob.bin not fetched yet
        QVERIFY(PackageCatalog::NodeHydrated(idx0, "localonly"));   // no CID → nothing fetchable is missing

        writeFile(dir.path() + "/blob.bin");                        // the fetch lands the file locally
        NodeIndex idx1; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx1); ManifestModel::DeriveFacts(idx1);
        QVERIFY(PackageCatalog::NodeHydrated(idx1, "remote"));      // now fully fetched
    }

    // #8/#9: DehydrateNode deletes the content-layer files (keeping the manifest) and reports what it removed.
    void dehydrate_node_removes_content_keeps_manifest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("file", "blob.bin"), NodeFixture::Exec("win32", "g.exe")}));
        writeFile(dir.path() + "/blob.bin");

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
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

    // The pre-download dialog's runner-offer contract (#2): a RECEIVED runner — closure incomplete, no enumerable
    // build yet — must be OFFERABLE, not filtered as a PATH runner. The dialog keeps a compatible runner when it has
    // content OR its closure is incomplete; only a complete-closure runner with no content is a PATH runner (skipped).
    // Teeth: flip the incomplete-runner arm and a received runner gets wrongly filtered.
    void received_runner_is_offerable_not_path_runner()
    {
        NodeIndex idx;
        Node launch; launch.NodeId = "game"; launch.HasExec = true; launch.HostPlatform = "win32";
        // A RECEIVED runner: compatible, but its build closure hasn't landed (a PARENT it names isn't in the index).
        Node proton; proton.NodeId = "proton"; proton.HasRunner = true; proton.GuestPlatform = {"win32"};
        proton.HostPlatform = ManifestModel::MachinePlatform();
        NodeFixture::Wire(proton, {"proton_build"});   // dangling → closure incomplete (build not fetched yet)
        // A PATH runner: complete closure, no content at all (resolves an executable on the system).
        Node nativeR; nativeR.NodeId = "native"; nativeR.HasRunner = true; nativeR.GuestPlatform = {"win32"};
        nativeR.HostPlatform = ManifestModel::MachinePlatform();
        idx.Nodes["game"] = launch; idx.Nodes["proton"] = proton; idx.Nodes["native"] = nativeR;

        QVERIFY2(PackageCatalog::NodeClosureIncomplete(idx, "proton"), "received runner: closure is incomplete");
        QVERIFY2(!PackageCatalog::NodeClosureIncomplete(idx, "native"), "PATH runner: closure is complete");
        QVERIFY2(PackageCatalog::NodeContentCids(idx, "native").empty(), "PATH runner has no content");
        // The dialog's offer predicate: offer when (has content) OR (closure incomplete).
        auto Offerable = [&](const std::string & rid){
            return !PackageCatalog::NodeContentCids(idx, rid).empty()
                || PackageCatalog::NodeClosureIncomplete(idx, rid);
        };
        QVERIFY2(Offerable("proton"), "a received runner MUST be offered (regression: proton auto-pulled invisibly)");
        QVERIFY2(!Offerable("native"), "a PATH runner is correctly not offered as a download");
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

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);

        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectContentTargets(idx, "game", {}, targets, &err));
        QVERIFY(RunnerInstall::CollectRunnerNodeTargets(idx, "wine", targets, &err));

        bool hasGame = false, hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_GAME") hasGame = true; if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasGame);                       // game content target
        QVERIFY(hasWine);                       // runner build target — in the SAME batch
        QVERIFY(targets.size() >= 2);
    }

    // A download writes where a node's content lives, and a node's content lives in its own package dir. A layer
    // that resolves anywhere else — a peer's absolute or ".." name — is never a fetch target. Teeth: drop the
    // walker's containment check and the escaping target appears.
    void a_download_never_writes_outside_the_package()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Merge({NodeFixture::Tile("g"), NodeFixture::Exec("win32", "g.exe"),
                                                                  NodeFixture::ContentCid("zip", "in.zip", "CID_IN")})}));
        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
        // The vocabulary refuses an escaping name outright (such a node is malformed, and never resolves) — so the
        // walker's own guard is reached only by a node whose lowered layers were built another way: forge one.
        Node &G = idx.Nodes.begin()->second;
        G.Layers.push_back(json{{"TYPE", "VFSZipLayer"}, {"PATH", "/tmp/vg_escape_target.zip"},
                                {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_ESCAPE"}}}});
        G.Layers.push_back(json{{"TYPE", "VFSZipLayer"}, {"PATH", "../outside.zip"},
                                {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_DOTDOT"}}}});
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectContentTargets(idx, "game", {}, targets, &err));
        std::set<std::string> cids;
        for (const auto & t : targets) cids.insert(t.Cid);
        QVERIFY(cids.count("CID_IN"));
        QVERIFY2(!cids.count("CID_ESCAPE") && !cids.count("CID_DOTDOT"), "a download target outside the package");
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
                  NodeFixture::Chain("game1", {NodeFixture::Tile("g1"), NodeFixture::Merge({NodeFixture::Exec("win32", "g1.exe"), NodeFixture::Variant("Play")})}, {"content1", "lib"}));
        writeJson(dir.path() + "/content1.json", NodeFixture::Chain("content1", {NodeFixture::ContentCid("zip", "g1.zip", "CID_G1")}));
        writeJson(dir.path() + "/lib.json",      NodeFixture::Chain("lib",      {NodeFixture::ContentCid("zip", "lib.zip", "CID_LIB")}));
        writeJson(dir.path() + "/game2.json",
                  NodeFixture::Chain("game2", {NodeFixture::Tile("g2"), NodeFixture::Merge({NodeFixture::Exec("win32", "g2.exe"), NodeFixture::Variant("Play")})}, {"content2"}));
        writeJson(dir.path() + "/content2.json", NodeFixture::Chain("content2", {NodeFixture::ContentCid("zip", "g2.zip", "CID_G2")}));
        // A runner + its dehydrated build (reached only via the IsRunner branch).
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")}, {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json", NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));
        // An ORPHAN library node: nothing parents it, no runner uses it — a full mirror must still fetch it.
        writeJson(dir.path() + "/orphanlib.json", NodeFixture::Chain("orphanlib", {NodeFixture::ContentCid("zip", "orphan.zip", "CID_ORPHAN")}));
        // A node whose content is missing WITH NO SOURCE (a runtime-generated path) — must be counted, never fatal.
        writeJson(dir.path() + "/sourceless.json", NodeFixture::Chain("sourceless", {NodeFixture::Content("zip", "nowhere.zip")}));

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
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
            NodeIndex idx; ManifestModel::ScanBundleNodes(dir.toStdString(), idx); ManifestModel::DeriveFacts(idx);
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
        NodeIndex ridx; ManifestModel::ScanBundleNodes(rt.path().toStdString(), ridx); ManifestModel::DeriveFacts(ridx);
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

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);

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
        NodeIndex idx2; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx2); ManifestModel::DeriveFacts(idx2);
        QVERIFY(!RunnerInstall::RunnerBuildPresent(idx2, "wine"));
        QVERIFY(!PackageCatalog::RunnerInstalled(idx2, "wine"));

        // A runner whose build is ON the runner node (folded: ENTRYPOINTS + LAYERS) is a build-shipping runner
        // too: installed iff its zip is here, and its zip is what a fetch collects when it is not.
        QTemporaryDir d3; QVERIFY(d3.isValid());
        writeJson(d3.path() + "/java.json",
                  NodeFixture::Chain("java", {NodeFixture::Merge({NodeFixture::Runner(ManifestModel::MachinePlatform(), {"java_8"}, "%RunnerMount%/__jre/bin/java"),
                                                                  NodeFixture::Content("zip", "jre.zip")})}));
        NodeIndex idx3; ManifestModel::ScanBundleNodes(d3.path().toStdString(), idx3); ManifestModel::DeriveFacts(idx3);
        QVERIFY2(!PackageCatalog::RunnerInstalled(idx3, "java"), "build on the runner node, zip absent ⇒ not installed");
        QVERIFY(!RunnerInstall::RunnerBuildPresent(idx3, "java"));
        writeFile(d3.path() + "/jre.zip");
        NodeIndex idx4; ManifestModel::ScanBundleNodes(d3.path().toStdString(), idx4); ManifestModel::DeriveFacts(idx4);
        QVERIFY2(RunnerInstall::RunnerBuildPresent(idx4, "java"), "…and present once the zip is here");
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

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);

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
        const Node * tile = idx.Find("tile");                      // the grouping key is the UID, inherited through OVER
        QVERIFY(tile != nullptr);
        QCOMPARE(v->GameKey(), std::string("777"));
        QCOMPARE(v->GameKey(), tile->GameKey());
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
        const Node * tile = idx.Find("cidtile");                           // grouping key = the UID, inherited through OVER
        QVERIFY(tile != nullptr);
        QCOMPARE(v->GameKey(), tile->GameKey());
        QVERIFY(!v->Uid.empty());
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
                  NodeFixture::Chain("solovariant", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"solotile"}));

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
        tile["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"] = json{{"FILE", "cover.png"}, {"SOURCE", "QmCoverCID"}};
        writeJson(bundle + "/node.json",
                  NodeFixture::Chain("g", {NodeFixture::ContentCid("zip", "game.zip", "QmLayerCID"), tile}));
        // A landed package folder (<pkg>/.package/): its nodes' content lives beside it, in the package dir.
        QDir().mkpath(bundle + "/.package");
        { std::ofstream f((bundle + "/extra.zip").toStdString()); f << "more"; }
        writeJson(bundle + "/.package/landed.json", NodeFixture::Chain("landed", {NodeFixture::ContentCid("zip", "extra.zip", "QmExtraCID")}));

        const auto all = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/false);
        QCOMPARE(all.at((bundle + "/game.zip").toStdString()), std::string("QmLayerCID"));
        QCOMPARE(all.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));   // the bug: cover was missed
        QCOMPARE(all.at((bundle + "/extra.zip").toStdString()), std::string("QmExtraCID"));   // beside the folder
        QCOMPARE((int)all.size(), 3);

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

    // The call site must use THE predicate, not its own idea of one. A hand-rolled `find('%')` scanner is wrong: a
    // '%' is not a variable. PathVariableTokens requires a MATCHED PAIR around an IDENTIFIER, because URL-escaped
    // filenames are ordinary in scraped content ("100%25%20done.zip" — "25" is not an identifier). Treating that as a
    // variable silently drops a REAL missing file from the report.
    void publish_classifies_runtime_sourced_by_the_predicate_not_by_a_percent_sign()
    {
        QTemporaryDir d; QVERIFY(d.isValid());
        const QString bundle = d.path() + "/pct";
        QDir().mkpath(bundle);
        writeJson(bundle + "/node.json", NodeFixture::Chain("p", {NodeFixture::Content("zip", "100%25%20done.zip")}));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool NamedEscaped = false;
        for (const auto &E : Errors) if (E.find("100%25%20done.zip") != std::string::npos) NamedEscaped = true;
        QVERIFY2(NamedEscaped, "a URL-escaped filename is NOT a runtime-sourced layer — it is a real missing "
                               "file and must be reported. A scanner that only looks for '%' hides it.");
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
        tile["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"] = "cover.png";   // a bare string, not {FILE, SOURCE, SIZE}; file present
        writeJson(bundle + "/node.json", NodeFixture::Chain("t", {}, {}, tile));

        std::vector<std::string> Errors;
        SetLogCallback([&](LogLevel L, const std::string &, const std::string &M){
            if (L == LogLevel::ERR) Errors.push_back(M); });
        (void)PackageCatalog::PublishPackage(bundle.toStdString(), std::string(), nullptr);
        ClearLogCallback();

        bool Named = false, Summarised = false;
        for (const auto &E : Errors)
        {
            if (E.find("COVER") != std::string::npos && E.find("not {FILE, SOURCE, SIZE} objects") != std::string::npos) Named = true;
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
            N["LABEL"] = "n" + std::to_string(I);
            N["LAYERS"] = json::array();

            if (I > 0) N["LAYERS"] = json::array({ json{{"NODE", "n" + std::to_string(I - 1)}} });
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
        json A; A["LABEL"] = "a"; A["LAYERS"] = json::array();
        json B; B["LABEL"] = "b"; B["LAYERS"] = json::array({ json{{"NODE", "a"}} });
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
        json A; A["CID"] = "a"; A["LABEL"] = "a"; A["POS"] = json::array({10.0, 20.0}); A["LAYERS"] = json::array();
        writeJson(Dir.filePath("a.json"), A);

        json Local = json::object();
        Local["a"] = json::array({777.0, 888.0});   // the author dragged it; keyed by the node's CID handle "a"
        std::string Err;
        QVERIFY(PackageCatalog::StampNodePositions(Dir.path().toStdString(), &Local, &Err));

        std::ifstream In(Dir.filePath("a.json").toStdString());
        json J; In >> J;
        QCOMPARE(J["POS"][0].get<double>(), 777.0);
        QCOMPARE(J["POS"][1].get<double>(), 888.0);
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
        json A; A["CID"] = "a"; A["LABEL"] = "a"; A["LAYERS"] = json::array();   // handle "a" is what SetPos + the override key on
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
        json A; A["LABEL"] = "a";
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
