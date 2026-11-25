// Tests for PackageCatalog: hydration predicates (NodeHasContent / NodeHydrated — the #2 content-guard), the
// remove-flow DehydrateNode (#8/#9), per-package user settings, and runner predicates. Builds real temp bundles on
// disk so the layer-presence checks have something to resolve. IPFS node is not started, so DehydrateNode's
// unpin/drop-ref calls are graceful no-ops (we assert the filesystem side).

#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>

#include "qtestjson.h"
#include "sessions.h"
#include "instancestore.h"
#include "installed.h"
#include "shelf.h"
#include "builtinrunners.h"
#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "pkggraph.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "validate.h"
#include "nodefixture.h"
#include "cli/climodes.h"
#include "ipfswrapper.h"
#include "platform/platform.h"
#include "nodegraph.h"
#include "noderefs.h"      // VerifyNodeBytes — what a receiver checks a landed node file with
#include "cid.h"
#include "contentfile.h"
#include "nodefiles.h"
#include "nodestore.h"
#include "vfsmount.h"
#include "packagesview.h"
#include "runnerspage.h"
#include "appmodel.h"
#include <QPushButton>
#include "downloadqueue.h"   // EnqueueBatch/WaitBatch — the REAL rolling queue under test
#include "apppaths.h"
#include "commonutils.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <atomic>
#include <chrono>
#include <thread>
#ifndef Q_OS_WIN
#include <unistd.h>
#endif
#include <filesystem>
#include <limits>
#include <tuple>
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

    // Mint stamps SOURCE (the CID) and SIZE (payload bytes) beside a content layer's file and a tile COVER, and
    // BACKFILLS SIZE on re-mint for a package that already carries a valid CID but no size, via the idempotent branch
    // (CID unchanged — the branch runs precisely because NeedsSeed's VerifyCid passed, so the bytes were not re-added).
    // A DIR layer is local content: no SOURCE is stamped (a peer cannot fetch a directory). SIZE is what lets a fetch
    // show a real % and a pre-fetch total instantly. Teeth: drop the SIZE stamp at the seed site → the layer QCOMPARE
    // fails; drop the cover branch → the cover QCOMPARE fails; drop the idempotent backfill → the re-mint QCOMPARE fails.
    // A source's folder is the source's when it carries the source's marker (its fetch made it, or the sync marked one
    // fetched before markers) and is not a library this machine publishes. Removing a source deletes only its packages
    // (its rows) from such a folder; keeps what was installed from it by default (rows kept without the source, so
    // nothing dehydrates them); and otherwise leaves the folder. A source is refused when it is no CID, when its name
    // is a folder already in the library, a published library, or another source's folder; a name of only dots is no
    // folder of its own. Teeth: accept a taken name or a shared folder; delete an unmarked or published folder; take a
    // marker naming another CID; delete a package the user made in the folder; never mark an old source (unremovable).
    void a_source_removes_only_a_folder_its_own_fetch_made()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::filesystem::path Lib = D.filePath("LIBRARY").toStdString();
        const std::string Cid = "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi", Cid2 = "bafybeihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku";
        std::filesystem::create_directories(Lib / "VidyaGod" / "[1] Game");
        std::ofstream(Lib / "VidyaGod" / "[1] Game" / "game.json") << "{}";
        json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib.string()}}}}}, {"LIBRARY", json::array()},
                        {"ShareSheet", {{"bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e", "VidyaGod/[1] Game/game.json"}}}};
        std::string Why;
        QVERIFY(!PackageCatalog::AddPackageSource(Cfg, "not-a-cid", "x", &Why) && Why.find("not a CID") != std::string::npos);
        QVERIFY(!PackageCatalog::AddPackageSource(Cfg, Cid, "VidyaGod", &Why) && Why.find("already in the library") != std::string::npos);
        QVERIFY(PackageCatalog::AddPackageSource(Cfg, Cid, "..", &Why));
        QCOMPARE(QString::fromStdString(std::filesystem::path(PackageCatalog::PackageSourceDir(Cfg, Cfg["Settings"]["PackageSources"][0])).filename().string()),
                 QString::fromStdString(Cid.substr(0, 12)));
        QVERIFY(!PackageCatalog::AddPackageSource(Cfg, Cid, "again", &Why) && Why.find("already added") != std::string::npos);
        QVERIFY(!PackageCatalog::AddPackageSource(Cfg, Cid2, Cid.substr(0, 12), &Why) && Why.find("Another source") != std::string::npos);
        //A source whose name came to be a published library's folder (a hand-edited config): the sync never marks it, and
        //removing it leaves the folder.
        Cfg["Settings"]["PackageSources"] = json::array({ json{{"CID", Cid2}, {"NAME", "VidyaGod"}} });
        PackageCatalog::SyncPackageSources(Cfg);
        QVERIFY(!std::filesystem::exists(Lib / "VidyaGod" / ".vgsource"));
        PackageCatalog::RemovePackageSource(Cfg, 0, /*KeepInstalled=*/false);
        QVERIFY(std::filesystem::exists(Lib / "VidyaGod" / "[1] Game" / "game.json"));
        QVERIFY(Cfg["Settings"]["PackageSources"].empty());
        for (const auto &E : Cfg["LIBRARY"]) QVERIFY(E.value("CIDSOURCE", std::string()) != Cid2);
        //A folder no fetch of the source made (no marker): the sync does not take it, and removing the source leaves it.
        {
            const std::filesystem::path S = Lib / "Unmarked";
            std::filesystem::create_directories(S / "[5] Pkg");
            std::ofstream(S / "[5] Pkg" / "p.json") << R"({"LABEL":"P","LAYERS":[{"EXEC":[{"LABEL":"P","HOST":"win32","EXE":"p.exe","TILE":{"UID":"5","TITLE":"P"}}]}]})";
            Cfg["Settings"]["PackageSources"] = json::array({ json{{"CID", Cid}, {"NAME", "Unmarked"}} });
            PackageCatalog::SyncPackageSources(Cfg);
            QVERIFY(!std::filesystem::exists(S / ".vgsource"));
            PackageCatalog::RemovePackageSource(Cfg, 0, /*KeepInstalled=*/false);
            QVERIFY(std::filesystem::exists(S / "[5] Pkg" / "p.json"));
            Cfg["LIBRARY"] = json::array();
        }
        //A source's fetched folder (its marker lists what the fetch brought): removed, its packages go; a package the user
        //made in its folder since stays, re-sync or not.
        const auto Old = [&](const char *Name) {
            const std::filesystem::path S = Lib / Name;
            std::filesystem::create_directories(S / "[2] Stub");
            std::ofstream(S / "[2] Stub" / "stub.json") << R"({"LABEL":"Stub","LAYERS":[{"EXEC":[{"LABEL":"Stub","HOST":"win32","EXE":"s.exe","TILE":{"UID":"2","TITLE":"Stub"}}]}]})";
            std::filesystem::create_directories(S / "[3] Played");
            std::ofstream(S / "[3] Played" / "played.json") << R"({"LABEL":"Played","LAYERS":[{"EXEC":[{"LABEL":"Played","HOST":"win32","EXE":"p.exe","TILE":{"UID":"3","TITLE":"Played"}}]}]})";
            std::ofstream(S / "[3] Played" / "content.zip") << "bytes";
            std::ofstream(S / ".vgsource") << Cid << "\n[2] Stub\n[3] Played\n";   // as its fetch marks it
            Cfg["Settings"]["PackageSources"] = json::array({ json{{"CID", Cid}, {"NAME", Name}} });
            Cfg["LIBRARY"] = json::array();
            PackageCatalog::SyncPackageSources(Cfg);
            return S;
        };
        const std::filesystem::path Gone = Old("Gone");
        QCOMPARE((int)Cfg["LIBRARY"].size(), 2);
        std::filesystem::create_directories(Gone / "[9] Mine");
        std::ofstream(Gone / "[9] Mine" / "mine.json") << R"({"LABEL":"Mine","LAYERS":[{"EXEC":[{"LABEL":"Mine","HOST":"win32","EXE":"m.exe","TILE":{"UID":"9","TITLE":"Mine"}}]}]})";
        PackageCatalog::SyncPackageSources(Cfg);                               // a start: the user's package is not the source's
        QCOMPARE((int)Cfg["LIBRARY"].size(), 2);
        Cfg["LIBRARY"].push_back(json{{"PATH", (Lib / "VidyaGod" / "[1] Game").string()}, {"CIDSOURCE", "another"}});
        //…and a row naming the source for the user's package (an earlier sync's, a hand edit): the marker's list decides.
        Cfg["LIBRARY"].push_back(json{{"PATH", (Gone / "[9] Mine").string()}, {"CIDSOURCE", Cid}});
        PackageCatalog::RemovePackageSource(Cfg, 0, /*KeepInstalled=*/false);
        QVERIFY(std::filesystem::exists(Lib / "VidyaGod" / "[1] Game" / "game.json"));   // another source's row: untouched
        QCOMPARE((int)Cfg["LIBRARY"].size(), 2);
        Cfg["LIBRARY"] = json::array();
        QVERIFY(!std::filesystem::exists(Gone / "[2] Stub") && !std::filesystem::exists(Gone / "[3] Played"));
        QVERIFY(std::filesystem::exists(Gone / "[9] Mine" / "mine.json"));
        QVERIFY(Cfg["LIBRARY"].empty());
        //Kept: the installed package stays (its row without the source: nothing dehydrates it), the stub goes, and the
        //folder is no longer the source's — added again, the source is refused its name rather than fed a half folder.
        const std::filesystem::path Kept = Old("Kept");
        PackageCatalog::RemovePackageSource(Cfg, 0);
        QVERIFY(!std::filesystem::exists(Kept / "[2] Stub") && std::filesystem::exists(Kept / "[3] Played" / "content.zip"));
        QVERIFY(!std::filesystem::exists(Kept / ".vgsource"));
        QCOMPARE((int)Cfg["LIBRARY"].size(), 1);
        QVERIFY(!Cfg["LIBRARY"][0].contains("CIDSOURCE") && Cfg["LIBRARY"][0].value("KEPT_FROM", std::string()) == Cid);
        QVERIFY(PackageCatalog::SourceOfPackage(Cfg, Kept / "[3] Played").empty());
        QVERIFY(!PackageCatalog::AddPackageSource(Cfg, Cid, "Kept", &Why) && Why.find("already in the library") != std::string::npos);
        //A marker naming another source: not this one's.
        const std::filesystem::path Other = Old("Other");
        std::ofstream(Other / ".vgsource", std::ios::trunc) << Cid2 << "\n[2] Stub\n[3] Played\n";
        PackageCatalog::RemovePackageSource(Cfg, 0, /*KeepInstalled=*/false);
        QVERIFY(std::filesystem::exists(Other / "[2] Stub"));
        QVERIFY(std::filesystem::exists(Lib / "VidyaGod" / "[1] Game" / "game.json"));
    }

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

        //Publishing re-mints (recording a SOURCE changes a node's bytes) and names each node file by its label, so a node
        //is found by its LABEL, not by the name it was written under.
        auto FileOf = [&](const QString & label) -> QString {
            for (const QString &F : QDir(Pkg.path()).entryList({"*.json"}, QDir::Files))
            {
                std::ifstream in((Pkg.path() + "/" + F).toStdString());
                const json J = json::parse(in, nullptr, false);
                if (J.is_object() && J.value("LABEL", std::string()) == label.toStdString()) return F;
            }
            return QString();
        };
        auto Read = [&](const QString & label) { std::ifstream in((Pkg.path() + "/" + FileOf(label)).toStdString()); json J = json::parse(in, nullptr, false); return J.is_object() ? J : json::object({{"LAYERS", json::array({json::object()})}}); };
        auto Layer = [&](const QString & label) { return Read(label)["LAYERS"][0]; };
        auto Cover = [&]() { return Read("tile")["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"]; };

        // 1) Fresh mint → CID + SIZE stamped on the file and the cover; the DIR carries neither.
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        const json File = Layer("game"), Dir = Layer("data"), Cov = Cover();
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
            const QString F = FileOf("game");
            json N = Read("game");
            N["LAYERS"][0].erase("SIZE");
            writeJson(Pkg.path() + "/" + F, N);
        }
        QVERIFY2(!Layer("game").contains("SIZE"), "precondition: SIZE stripped");
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.path().toStdString(), "", &Err), Err.c_str());
        QCOMPARE((qulonglong)Layer("game").value("SIZE", (qulonglong)0), (qulonglong)4096);   // backfilled
        QCOMPARE(Layer("game").value("SOURCE", std::string()), Cid);                          // same bytes → same CID
        //And every node file is canonical bytes named by its label: publishing re-minted, it did not edit in place.
        QCOMPARE(QDir(Pkg.path()).entryList({"*.json"}, QDir::Files), (QStringList{ "data.json", "game.json", "tile.json" }));
        for (const QString &F : QDir(Pkg.path()).entryList({"*.json"}, QDir::Files))
        {
            std::ifstream in((Pkg.path() + "/" + F).toStdString(), std::ios::binary);
            const std::string B((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            QCOMPARE(Cid::OfBytes(B), Cid::OfNode(json::parse(B)));
        }

        IpfsWrapper::StopNode();
    }

    // Sharing is per PACKAGE: every node of every bundle dir is an entry of the share sheet — its CID → where it lives,
    // <library>/<package>/<file named by its label> — and every package gets a pin folder of its node files and its
    // content. There is no per-node flag — a runner package, a library package and a plain game all share alike.
    // Teeth: gate on any node field and a package drops; mint a library-level block and the PublishedManifest assert
    // fails; publish handle-bearing bytes and VerifyNodeBytes refuses them.
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
                  NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"g_tile", "l_dg"}));
        writeJson(R + "/VidyaGodRunners/wine/wine.json",
                  NodeFixture::Chain("r_wine", {NodeFixture::Runner("linux", {"win32"}, "wine")}));
        writeFile(R + "/VidyaGodLibraries/dgvoodoo/dgvoodoo.zip", std::string(1000, 'D'));
        const std::string DgCid = IpfsWrapper::AddNoCopy((R + "/VidyaGodLibraries/dgvoodoo/dgvoodoo.zip").toStdString(), &Err);
        QVERIFY2(!DgCid.empty(), Err.c_str());
        writeJson(R + "/VidyaGodLibraries/dgvoodoo/dg.json",
                  NodeFixture::Chain("l_dg", {NodeFixture::ContentCid("zip", "dgvoodoo.zip", DgCid)}));
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

        QVERIFY2(cfg.contains("ShareSheet") && cfg["ShareSheet"].is_object(), "PublishLibrary writes the share sheet");
        QVERIFY2(!cfg.contains("Libraries"), "no package rows: a package is its entries");
        const json & Sheet = cfg["ShareSheet"];
        std::map<std::string, int> PerLib;
        for (const auto & E : PackageCatalog::SheetEntries(Sheet)) ++PerLib[E.Lib];
        QCOMPARE(PerLib["VidyaGod"], 3);                      // BOTH packages' nodes — nothing is gated
        QCOMPARE(PerLib["VidyaGodRunners"], 1);
        QCOMPARE(PerLib["VidyaGodLibraries"], 1);
        QCOMPARE((int)Published.size(), 4);                 // four packages
        QVERIFY2(!cfg.contains("PublishedManifest"), "no library-level block: a library is a name, never a CID");

        // Every entry names its node by where it lives, its file named by its label (chapter 02 §2.5), and the bytes
        // it was published as are that CID. Teeth: name the file by the CID, or record the folder instead of the node.
        const std::string GExec = cidAt(cfg, "VidyaGod/[1] Game/g_exec.json"), GTile = cidAt(cfg, "VidyaGod/[1] Game/g_tile.json");
        QVERIFY2(!GExec.empty() && !GTile.empty(), Sheet.dump().c_str());
        for (const auto & [C, P] : Sheet.items())
        {
            json J;
            std::string VErr;
            QVERIFY2(NodeGraph::VerifyNodeBytes(IpfsWrapper::BlockGet(C, &Err), C, J, &VErr), VErr.c_str());   // canonical, handle-free, its own CID
        }
        // A share is closed over what its nodes contain: a peer given VidyaGod also gets the dgVoodoo package
        // [1] Game contains — as its own package, never copied into the game — and not the runners nothing contains.
        // Teeth: drop SharedWith's walk of the contained nodes, and dgvoodoo is missing.
        const json Shared = PackageCatalog::SharedWith(Sheet, {"VidyaGod"}, containsOf);
        QCOMPARE((int)Shared["VidyaGod"].size(), 3);
        QVERIFY2(Shared.contains("VidyaGodLibraries") && Shared["VidyaGodLibraries"].size() == 1, Shared.dump().c_str());
        QVERIFY2(!Shared.contains("VidyaGodRunners"), "a package nothing shared contains is not shared");
        QCOMPARE(Shared["VidyaGodLibraries"][0].value("node", std::string()), std::string("l_dg.json"));
        // The PIN folder (what a pinning service pins): the package's node files under their names, and the package's
        // own content beside them — none for [1] Game (no content), [2] Other's zip for it.
        QTemporaryDir PinDir;
        std::string FErr;
        const std::string GPin = cfg["Pins"].value("VidyaGod/[1] Game", std::string());
        QVERIFY2(!IpfsWrapper::FetchDirToPath(GPin, (PinDir.path() + "/g").toStdString(), &FErr).empty(), FErr.c_str());
        QCOMPARE(QDir(PinDir.path() + "/g/package").entryList(QDir::Files), (QStringList{ "g_exec.json", "g_tile.json" }));
        QVERIFY2(!QDir(PinDir.path() + "/g/content").exists(), "no content: no content folder");
        const std::string UPin = cfg["Pins"].value("VidyaGod/[2] Other", std::string());
        QVERIFY(!UPin.empty() && UPin != GPin);
        QVERIFY2(!IpfsWrapper::FetchDirToPath(UPin, (PinDir.path() + "/u").toStdString(), &FErr).empty(), FErr.c_str());
        QCOMPARE(QFileInfo(PinDir.path() + "/u/content/" + QString::fromStdString(UCid)).size(), (qint64)3000);   // the content, linked
        QCOMPARE((int)QDir(PinDir.path() + "/u/package").entryList(QDir::Files).size(), 1);   // [2] Other's one node

        // What did not publish whole is in the verdict, never only in a scrolled-past warning: a package naming
        // content this machine does not hold, and a node that cannot freeze (a cycle), are both named.
        QDir().mkpath(R + "/VidyaGod/[3] Gap");
        writeJson(R + "/VidyaGod/[3] Gap/gap.json", NodeFixture::Chain("gap_exec", {NodeFixture::Merge({
                      NodeFixture::ContentCid("zip", "g.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku"),
                      NodeFixture::Content("zip", "nosource.zip"),
                      NodeFixture::Exec("win32", "g.exe")})}));
        writeJson(R + "/VidyaGod/[3] Gap/loop.json", json{{"CID", "hLoop"}, {"LABEL", "loop"},
                                                          {"LAYERS", json::array({ {{"NODE", "hLoop"}} })}});
        const std::vector<std::string> WithGap = PackageCatalog::PublishLibrary(cfg, &Err, &Gaps);
        QVERIFY2(Err.empty(), Err.c_str());
        QCOMPARE((int)WithGap.size(), 5);                   // a gap is named, never a reason to publish nothing
        QVERIFY2(Gaps.find("[3] Gap (1)") != std::string::npos, Gaps.c_str());
        QVERIFY2(Gaps.find("did not freeze") != std::string::npos && Gaps.find("[3] Gap/loop") != std::string::npos, Gaps.c_str());
        //...with why: a node left out says what stopped it. Teeth: drop Mint's Skipped record and the reason is gone.
        QVERIFY2(Gaps.find("[3] Gap/loop: it references 'hLoop'") != std::string::npos, Gaps.c_str());
        QVERIFY2(Gaps.find("[2] Other") == std::string::npos, "held content is not a gap");
        //A file named with no SOURCE is shared as a name only: a peer sees the game and can never fetch it. Teeth:
        //count only the CIDs a package names (it publishes "clean" but for the unheld one).
        const size_t Unsourced = Gaps.find("name no SOURCE");
        QVERIFY2(Unsourced != std::string::npos && Gaps.find("[3] Gap (1)", Unsourced) != std::string::npos, Gaps.c_str());
        // Content whose file was moved or removed after it was seeded still "has" its block (the reference stays), but
        // nothing can serve it: publishing it into a pin folder recorded a package as pinned that no one could fetch.
        // It is a gap. Teeth: gate the pin folder on HasLocal again and [2] Other publishes "clean".
        QFile::remove(R + "/VidyaGod/[2] Other/u.zip");
        QVERIFY2(IpfsWrapper::HasLocal(UCid), "precondition: the orphaned reference still claims the block");
        const std::vector<std::string> Orphaned = PackageCatalog::PublishLibrary(cfg, &Err, &Gaps);
        QVERIFY2(Err.empty(), Err.c_str());
        QCOMPARE((int)Orphaned.size(), 5);
        QVERIFY2(Gaps.find("[2] Other (1)") != std::string::npos, Gaps.c_str());
        // A copy of a node another package holds (here dgVoodoo's node, copied into [1] Game the way older builds
        // did) is named in the verdict and not shared a second time. Teeth: drop the one-home check in PublishLibrary.
        json DgCopy = NodeFixture::Chain("l_dg", {NodeFixture::ContentCid("zip", "dgvoodoo.zip", DgCid)}).back();
        DgCopy.erase("CID");
        writeJson(R + "/VidyaGod/[1] Game/dg_copy.json", DgCopy);
        (void)PackageCatalog::PublishLibrary(cfg, &Err, &Gaps);
        QVERIFY2(Gaps.find("copies of a node another package holds") != std::string::npos, Gaps.c_str());
        IpfsWrapper::StopNode();
    }

    // A received node file already here, holding its CID, is not fetched again when the app starts and asks for it
    // (every received file, on every start: read twice, hashed twice and announced, one by one through the slots), and
    // it lands nothing — the receive that asked shows nothing new. A file there holding another version is fetched.
    // Teeth: drop the AlreadyHere check in RunJob (the fetch runs for the file in place); let it pass on presence
    // alone (the stale file is kept); report a job found already here as landed.
    void a_received_file_already_here_is_not_fetched_again()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::SetFetchOnceHook({}); IpfsWrapper::DebugResetQueue(); IpfsWrapper::StopNode(); });
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const json A = json{{"LABEL", "Game 1.0"}, {"LAYERS", json::array({ json{{"DIR", "a"}} })}};
        const json A2 = json{{"LABEL", "Game 1.0"}, {"LAYERS", json::array({ json{{"DIR", "a2"}} })}};
        const std::string Here = Root.path().toStdString() + "/Game 1.0.json";
        const std::string Stale = Root.path().toStdString() + "/Game 1.0 (old).json";
        std::ofstream(Here, std::ios::binary) << Cid::Canonical(A);
        std::ofstream(Stale, std::ios::binary) << Cid::Canonical(A2);       // the same path, an older version
        QCOMPARE(IpfsWrapper::AddNoCopy(Here, &Err), Cid::OfNode(A));
        QCOMPARE(IpfsWrapper::AddNoCopy(Stale, &Err), Cid::OfNode(A2));
        std::atomic<int> Fetches{0};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *) {
            ++Fetches; std::ofstream(dest, std::ios::binary | std::ios::trunc) << Cid::Canonical(cid == Cid::OfNode(A) ? A : A2); return 0; });
        IpfsWrapper::DebugResetQueue();                                    // the app started again: the queue knows nothing
        const long long Before = IpfsWrapper::DebugAlreadyHere();
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{Cid::OfNode(A), Here, true, false, /*Verify=*/true}}), 10000));
        QCOMPARE(Fetches.load(), 0);
        QCOMPARE(IpfsWrapper::DebugAlreadyHere() - Before, 1LL);
        bool Landed = true;
        QVERIFY(IpfsWrapper::JobSettled(Cid::OfNode(A), &Landed));
        QVERIFY2(!Landed, "a file found already here landed nothing");
        // The stale path holds A2; A is asked for there, on another start: fetched.
        IpfsWrapper::DebugResetQueue();
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{Cid::OfNode(A), Stale, true, false, /*Verify=*/true}}), 10000));
        QCOMPARE(Fetches.load(), 1);
        QVERIFY(IpfsWrapper::JobSettled(Cid::OfNode(A), &Landed));
        QVERIFY2(Landed, "a fetched file landed");
    }

    // A received file's job that lands one copy leaves another copy already holding its node as it is: it was copied
    // over at every run (read, hashed, copied). Teeth: drop the check (the copy is written again: its time changes).
    void a_copy_already_holding_its_node_is_left_as_it_is()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::SetFetchOnceHook({}); IpfsWrapper::DebugResetQueue(); IpfsWrapper::StopNode(); });
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const json A = json{{"LABEL", "Game 1.0"}, {"LAYERS", json::array({ json{{"DIR", "a"}} })}};
        const std::string Missing = Root.path().toStdString() + "/one/Game 1.0.json", Here = Root.path().toStdString() + "/two/Game 1.0.json";
        std::filesystem::create_directories(std::filesystem::path(Here).parent_path());
        std::ofstream(Here, std::ios::binary) << Cid::Canonical(A);
        QCOMPARE(IpfsWrapper::AddNoCopy(Here, &Err), Cid::OfNode(A));
        const auto Old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        std::filesystem::last_write_time(Here, Old);
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &dest, bool, std::string *) {
            std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
            std::ofstream(dest, std::ios::binary) << Cid::Canonical(A); return 0; });
        IpfsWrapper::DebugResetQueue();
        QVERIFY(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({IpfsWrapper::FetchTarget{Cid::OfNode(A), Missing, true, false, true},
                                                                  IpfsWrapper::FetchTarget{Cid::OfNode(A), Here, true, false, true}}), 10000));
        QVERIFY(std::filesystem::exists(Missing));
        QVERIFY2(std::filesystem::last_write_time(Here) == Old, "the copy already holding the node was written again");
    }

    // Seeding a node's content again (Seed & share, every publish of a package) does not read back a file it read
    // before and that has not changed since: a 50 GB game was read whole at every publish, the editor waiting. A file
    // changed since is read, and its drift found. Teeth: drop the record (read every time); key it on the node's path,
    // not the files the CID reads (the file changed in place stays trusted).
    void seeding_again_reads_only_what_changed()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        const std::string Saved = AppPaths::DataRootSet() ? AppPaths::DataRoot().string() : std::string();
        const auto Restore = qScopeGuard([&] { AppPaths::SetDataRoot(Saved); IpfsWrapper::StopNode(); });
        AppPaths::SetDataRoot(data.path().toStdString());
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((data.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        const std::string Pkg = data.path().toStdString() + "/LIBRARY/VidyaGod/[9] S";
        std::filesystem::create_directories(Pkg);
        { std::ofstream(Pkg + "/disc.bin", std::ios::binary) << std::string(400000, 'a'); }
        nlohmann::ordered_json Node = NodeFixture::Chain("s_content", {NodeFixture::Content("file", "disc.bin")}).back();
        PackageCatalog::SeedReport R1, R2, R3;
        bool Changed = false;
        QVERIFY2(PackageCatalog::SeedNodeContent(Node, Pkg, R1, Changed, &Err), Err.c_str());
        QCOMPARE(R1.Seeded, 1);
        QVERIFY2(PackageCatalog::SeedNodeContent(Node, Pkg, R2, Changed, &Err), Err.c_str());
        QCOMPARE(R2.Read, 1);                                               // the first check reads it…
        QVERIFY2(PackageCatalog::SeedNodeContent(Node, Pkg, R3, Changed, &Err), Err.c_str());
        QCOMPARE(R3.Read, 0);                                               // …and the next one does not
        // The same size, other bytes, a later time: read again, found drifted, seeded anew.
        { std::ofstream(Pkg + "/disc.bin", std::ios::binary) << std::string(400000, 'b'); }
        std::filesystem::last_write_time(Pkg + "/disc.bin", std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
        PackageCatalog::SeedReport R4;
        QVERIFY2(PackageCatalog::SeedNodeContent(Node, Pkg, R4, Changed, &Err), Err.c_str());
        QCOMPARE(R4.Read, 1);
        QCOMPARE(R4.Repaired, 1);
    }

    // A publish reads the record of checked files once and writes it once, whatever the package's node count (it was
    // read and written whole per node: thousands of nodes, thousands of rewrites of a file listing every check), and
    // what another process wrote there meanwhile stays — unless its file is gone (the record only grew). Teeth: write
    // it at the end of each node's seed; write this process's view without reading the file first; keep gone files'.
    void a_publish_writes_the_checked_record_once()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        const std::string Saved = AppPaths::DataRootSet() ? AppPaths::DataRoot().string() : std::string();
        const auto Restore = qScopeGuard([&] { AppPaths::SetDataRoot(Saved); IpfsWrapper::StopNode(); });
        AppPaths::SetDataRoot(data.path().toStdString());
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((data.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        const QString Pkg = data.path() + "/LIBRARY/VidyaGod/[9] V";
        QDir().mkpath(Pkg);
        for (const char * N : {"a", "b", "c"})
        {
            writeFile(Pkg + "/" + N + ".bin", std::string(200000, N[0]));
            writeJson(Pkg + "/" + N + ".json", NodeFixture::Chain(std::string("v_") + N, {NodeFixture::Content("file", std::string(N) + ".bin")}).back());
        }
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.toStdString(), "", &Err), Err.c_str());   // seeds: nothing checked yet
        const QString Record = data.path() + "/CACHE/seedverified.json";
        writeFile(data.path() + "/elsewhere.bin", "x");
        json Other = json::parse(R"({"format": "vgseedverified 2", "cids": {"bafygone": {"/no/such/file.bin": "1 2 3 4"}}})");
        Other["cids"]["bafyother"] = {{(data.path() + "/elsewhere.bin").toStdString(), "1 2 3 4"}};
        QDir().mkpath(data.path() + "/CACHE");
        writeJson(Record, Other);                                                    // another process's check
        const long long Io = PackageCatalog::DebugVerifiedFileIo();
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.toStdString(), "", &Err), Err.c_str());   // checks all three
        QVERIFY2(PackageCatalog::DebugVerifiedFileIo() - Io <= 2, std::to_string(PackageCatalog::DebugVerifiedFileIo() - Io).c_str());
        std::ifstream In(Record.toStdString());
        const json J = json::parse(In, nullptr, false);
        QVERIFY2(J.is_object() && J["cids"].size() == 4, J.dump().c_str());          // three checked here, one kept
        QVERIFY(J["cids"].contains("bafyother"));
        QVERIFY(!J["cids"].contains("bafygone"));
        In.close();
        // Another process writing it (its lock held): this publish writes nothing over it, and what it found is written
        // by the next pass. Teeth: write without the lock.
        writeFile(Pkg + "/d.bin", std::string(200000, 'd'));
        writeJson(Pkg + "/d.json", NodeFixture::Chain("v_d", {NodeFixture::Content("file", "d.bin")}).back());
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.toStdString(), "", &Err), Err.c_str());   // seeds d
        {
            const Platform::ScopedFileLock Theirs((Record + ".lock").toStdString());
            QVERIFY(Theirs.held());
            QVERIFY2(PackageCatalog::PublishPackage(Pkg.toStdString(), "", &Err), Err.c_str());   // checks d
            std::ifstream R(Record.toStdString());
            QCOMPARE(json::parse(R, nullptr, false)["cids"].size(), size_t(4));          // left as the other wrote it
        }
        QVERIFY2(PackageCatalog::PublishPackage(Pkg.toStdString(), "", &Err), Err.c_str());
        std::ifstream R2(Record.toStdString());
        QCOMPARE(json::parse(R2, nullptr, false)["cids"].size(), size_t(5));
    }

    // A heal that prunes what nothing references keeps a published package whole: its pin folder holds its node folder
    // and content folder (each pinned too) and its node files (pinned directly), all referenced through the pin folder.
    // Teeth: count only the pin folders (the package's own folders and nodes are unpinned).
    void a_prune_keeps_what_a_published_package_holds()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::StopNode(); });
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString Pkg = Root.path() + "/VidyaGod/[5] P";
        QDir().mkpath(Pkg);
        { std::ofstream(Pkg.toStdString() + "/p.bin", std::ios::binary) << std::string(5000, 'p'); }
        const std::string Bin = IpfsWrapper::AddNoCopy(Pkg.toStdString() + "/p.bin", &Err);
        QVERIFY(!Bin.empty());
        writeJson(Pkg + "/c.json", NodeFixture::Chain("p_content", {NodeFixture::ContentCid("file", "p.bin", Bin)}));
        writeJson(Pkg + "/p.json", NodeFixture::Chain("p_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "p.exe"),
                  NodeFixture::Variant("Play"), NodeFixture::Tile("5", "P")})}, {"p_content"}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString()}}}}}};
        QVERIFY2(!PackageCatalog::PublishLibrary(cfg, &Err).empty(), Err.c_str());
        std::set<std::string> Before;
        for (const auto &P : IpfsWrapper::Pins()) Before.insert(P.Cid);
        cfg["Settings"]["PackageSources"] = json::array({ json{{"CID", "bafysourcefolder"}, {"NAME", "VidyaGod"}} });   // the heal's scope
        QVERIFY(!PackageCatalog::SeedTargets(PackageCatalog::PackageSourceDir(cfg, cfg["Settings"]["PackageSources"][0])).empty());
        PackageCatalog::HealOptions O;
        O.PruneUnreferenced = true;
        const auto R = PackageCatalog::HealSourceContent(cfg, O);
        QCOMPARE(R.PrunedUnreferenced, 0);
        std::set<std::string> After;
        for (const auto &P : IpfsWrapper::Pins()) After.insert(P.Cid);
        QCOMPARE(After, Before);
    }

    // Upgrading a package source keeps the content that only moves held, served from where it is now — a move of its
    // references, not a drop and a re-add of each file (every byte read again: gigabytes for a file that only moved).
    // Teeth: drop and re-add each moved file again (it is read whole); move nothing (the moved files' references name
    // where they were).
    void a_source_upgrade_moves_references_and_reads_nothing()
    {
#ifndef __linux__
        QSKIP("counts reads through /proc/self/io");
#endif
        const auto ReadSoFar = [] {
            std::ifstream In("/proc/self/io");
            for (std::string L; std::getline(In, L); )
                if (L.rfind("rchar: ", 0) == 0) return std::stoll(L.substr(7));
            return -1LL;
        };
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::StopNode(); });
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const std::string Dir = Root.path().toStdString() + "/Src", Staging = Root.path().toStdString() + "/.Src.upgrade";
        std::filesystem::create_directories(Dir + "/[1] A");
        std::filesystem::create_directories(Staging + "/[2] A");
        const auto Big = [](const std::string &P, char Fill) { std::ofstream O(P, std::ios::binary); O << std::string(8 << 20, Fill); };
        Big(Dir + "/[1] A/a.bin", 'a');
        Big(Dir + "/[1] A/old.bin", 'o');
        const std::string A = IpfsWrapper::AddNoCopy(Dir + "/[1] A/a.bin", &Err), O = IpfsWrapper::AddNoCopy(Dir + "/[1] A/old.bin", &Err);
        QVERIFY(!A.empty() && !O.empty());
        writeJson(QString::fromStdString(Staging + "/[2] A/a.json"), NodeFixture::Chain("a_content", {NodeFixture::ContentCid("file", "a.bin", A)}));
        json cfg = json{{"Settings", {{"PackageSources", json::array({ json{{"CID", "bafyoldsource"}, {"NAME", "Src"}} })}}}};
        PackageCatalog::SourceUpgradePlan P;
        P.Name = "Src"; P.OldCid = ""; P.NewCid = "bafynewsource"; P.Dir = Dir; P.StagingDir = Staging;
        P.DeprecatedDir = Root.path().toStdString() + "/.deprecated/Src/old";
        P.ContentMove[A] = { Dir + "/[1] A/a.bin", Dir + "/[2] A/a.bin" };
        P.ContentDeprecate[Dir + "/[1] A/old.bin"] = O;
        P.Valid = true;
        const long long Before = ReadSoFar();
        PackageCatalog::ApplySourceUpgrade(cfg, P, true, &Err);
        const long long Read = ReadSoFar() - Before;
        QVERIFY2(Read < (8 << 20), std::to_string(Read).c_str());           // neither 8 MB file was read again
        QVERIFY2(IpfsWrapper::HasLocal(A) && !IpfsWrapper::CidMissing(A), "the moved file is held where it is now");
        QVERIFY2(IpfsWrapper::HasLocal(O) && !IpfsWrapper::CidMissing(O), "the deprecated file is held where it is now");
        //The folder is the source's at its new CID (else it could never be removed after an upgrade).
        std::ifstream Mark(Dir + "/.vgsource");
        std::string Marked;
        QVERIFY(std::getline(Mark, Marked) && Marked == "bafynewsource");
    }

    // Publishing names a package's frozen node files by their labels (chapter 02 §2.5): a file named by its CID, as a
    // script or an older build wrote it, takes its label's name, and what refers to it in place follows — every
    // package's, in one move made before any folder. Two nodes of one label are both bumped. Teeth: drop the Normalize
    // in Publish (the CID names stay); drop its MoveRefs, or make it after the folders (the node's reference dies with
    // the old name and its folder is refused); move only one package's references (the other's dies); move each
    // package's in a call of its own (a scan of every reference each: a first publish took hours).
    void publish_names_frozen_node_files_by_their_labels()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const std::string Pkg = Root.path().toStdString() + "/VidyaGod/[1] G";
        std::filesystem::create_directories(Pkg);
        const json A = json{{"LABEL", "Game 1.0"}, {"LAYERS", json::array({ json{{"DIR", "a"}} })}};
        const json B = json{{"LABEL", "Mod"}, {"LAYERS", json::array({ json{{"DIR", "b"}} })}};
        const json C = json{{"LABEL", "Mod"}, {"LAYERS", json::array({ json{{"DIR", "c"}} })}};
        for (const json &N : {A, B, C})
            std::ofstream(Pkg + "/" + Cid::OfNode(N) + ".json", std::ios::binary) << Cid::Canonical(N);
        QCOMPARE(IpfsWrapper::AddNoCopy(Pkg + "/" + Cid::OfNode(A) + ".json", &Err), Cid::OfNode(A));   // referenced in place
        const std::string Pkg2 = Root.path().toStdString() + "/VidyaGod/[2] H";                       // another package's,
        std::filesystem::create_directories(Pkg2);                                                    // in the same publish
        const json D = json{{"LABEL", "Other 2.0"}, {"LAYERS", json::array({ json{{"DIR", "d"}} })}};
        std::ofstream(Pkg2 + "/" + Cid::OfNode(D) + ".json", std::ios::binary) << Cid::Canonical(D);
        QCOMPARE(IpfsWrapper::AddNoCopy(Pkg2 + "/" + Cid::OfNode(D) + ".json", &Err), Cid::OfNode(D));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString()}}}}}};
        const long long Scans = IpfsWrapper::DebugMoveRefScans(), Batches = NodeStore::DebugMintBatches();
        QVERIFY2(PackageCatalog::PublishLibrary(cfg, &Err).size() == 2, Err.c_str());
        QCOMPARE(IpfsWrapper::DebugMoveRefScans() - Scans, 1LL);              // one scan of the references, for both
        QVERIFY2(NodeStore::DebugMintBatches() > Batches, "the nodes were stored as one batch, not one by one");
        QCOMPARE(QDir(QString::fromStdString(Pkg2)).entryList({"*.json"}, QDir::Files), QStringList{ "Other 2.0.json" });
        QVERIFY2(!IpfsWrapper::CidMissing(Cid::OfNode(D)), "the other package's node reference followed its file too");
        const QStringList Files = QDir(QString::fromStdString(Pkg)).entryList({"*.json"}, QDir::Files);
        const std::string BT = Cid::OfNode(B).substr(Cid::OfNode(B).size() - 8), CT = Cid::OfNode(C).substr(Cid::OfNode(C).size() - 8);
        QCOMPARE(Files, (QStringList{ "Game 1.0.json", QString::fromStdString("Mod [" + BT + "].json"), QString::fromStdString("Mod [" + CT + "].json") }));
        QVERIFY2(!IpfsWrapper::CidMissing(Cid::OfNode(A)), "the node's reference followed its file");
        QCOMPARE(cidAt(cfg, "VidyaGod/[1] G/Game 1.0.json"), Cid::OfNode(A));
        IpfsWrapper::StopNode();
    }

    // Publishing ONE package (the package editor's Publish, `packages publish`) mints it and what it contains and puts
    // its entries into the share sheet in place of its own: another package's entries are never touched, even stale
    // ones; a contained package's are redone when stale and kept as they were (with their pin) when not; a folder that
    // is no package is refused with the sheet untouched, alone or beside a real one. Teeth: replace the sheet
    // wholesale (the runner's entry goes); select only the named package (the edited dgVoodoo entry stays stale and
    // the game cannot freeze against it); rebuild every contained package (the marker pin goes); skip the stale check
    // (the edited dgVoodoo entry keeps its old CID).
    // What sharing needs published: right after a publish, nothing; an edited package, a new one, each alone (a
    // package of a library not shared is never named); a library the sheet names nothing of — the first, or one added
    // after — is unprepared, never stale; a package gone from disk is named, and leaves the sheet with its pin alone.
    // Your libraries are the folders on disk, published or not. Teeth: an unchanged package named, an edit missed, a
    // new package missed, another library's package named, an unprepared library stale, a removal missed or dropping
    // more than its own entries, the libraries read from the sheet only.
    void stale_shared_packages_are_what_changed_since_publishing()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path();
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        QDir().mkpath(R + "/VidyaGod/[2] Other");
        QDir().mkpath(R + "/VidyaGodLibraries/dgvoodoo");
        writeJson(R + "/VidyaGod/[1] Game/game.json", NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}));
        writeJson(R + "/VidyaGod/[2] Other/u.json", NodeFixture::Chain("u_exec", {NodeFixture::Exec("win32", "u.exe")}));
        writeJson(R + "/VidyaGodLibraries/dgvoodoo/dg.json", NodeFixture::Chain("l_dg", {NodeFixture::Exec("win32", "dg.exe")}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        const std::set<std::string> Games{"VidyaGod"};

        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(cfg);
        QCOMPARE(PackageCatalog::LibrariesOnDisk(cfg, Idx), (std::vector<std::string>{"VidyaGod", "VidyaGodLibraries"}));
        auto St = PackageCatalog::StaleSharedPackages(cfg, Idx, Games);
        QCOMPARE(St.Unprepared, (std::set<std::string>{"VidyaGod"}));   // never published: not stale, unprepared
        QVERIFY(St.Stale.empty());
        QCOMPARE((int)St.UnpreparedPackages.size(), 2);

        std::string Gaps;
        QVERIFY2(!PackageCatalog::PublishLibrary(cfg, &Err, &Gaps).empty(), Err.c_str());
        Idx = PackageCatalog::BuildCatalogIndex(cfg);
        St = PackageCatalog::StaleSharedPackages(cfg, Idx, Games);
        QVERIFY(St.Unprepared.empty() && St.Removed.empty());
        QVERIFY2(St.Stale.empty(), St.Stale.empty() ? "" : St.Stale.front().Dir.string().c_str());
        QCOMPARE((int)St.Signature.size(), 2);

        const auto Edit = [](const QString &File, const char *Note) {
            std::ifstream In(File.toStdString());
            json J = json::parse(In);
            In.close();
            for (auto &N : (J.is_array() ? J : (J = json::array({J})))) N["COMMENT"] = Note;
            NodeFixture::WriteNodes(File.toStdString(), J);
        };
        const auto Names = [](const PackageCatalog::ShareStaleness &S) {
            std::set<std::string> Out;
            for (const auto &D : S.Stale) Out.insert(D.Dir.filename().string());
            return Out; };
        QDir Dir(R + "/VidyaGod/[2] Other");
        const QString OtherFile = Dir.entryList({"*.json"}).value(0);
        QVERIFY(!OtherFile.isEmpty());
        Edit(R + "/VidyaGod/[2] Other/" + OtherFile, "v2");
        QDir Dg(R + "/VidyaGodLibraries/dgvoodoo");
        Edit(R + "/VidyaGodLibraries/dgvoodoo/" + Dg.entryList({"*.json"}).value(0), "v2");   // a library not shared
        QDir().mkpath(R + "/VidyaGod/[3] New");
        writeJson(R + "/VidyaGod/[3] New/n.json", NodeFixture::Chain("n_exec", {NodeFixture::Exec("win32", "n.exe")}));
        Idx = PackageCatalog::BuildCatalogIndex(cfg);
        St = PackageCatalog::StaleSharedPackages(cfg, Idx, Games);
        QCOMPARE(Names(St), (std::set<std::string>{"[2] Other", "[3] New"}));
        QVERIFY(St.Removed.empty());
        QCOMPARE(Names(PackageCatalog::StaleSharedPackages(cfg, Idx, {"VidyaGodLibraries"})), (std::set<std::string>{"dgvoodoo"}));

        //A library added once another was published: unprepared, never stale (only a person prepares it).
        QDir().mkpath(R + "/Mods/[9] Mod");
        writeJson(R + "/Mods/[9] Mod/m.json", NodeFixture::Chain("m_exec", {NodeFixture::Exec("win32", "m.exe")}));
        Idx = PackageCatalog::BuildCatalogIndex(cfg);
        St = PackageCatalog::StaleSharedPackages(cfg, Idx, {"VidyaGod", "Mods"});
        QCOMPARE(St.Unprepared, (std::set<std::string>{"Mods"}));
        QCOMPARE(Names(St), (std::set<std::string>{"[2] Other", "[3] New"}));

        //A package gone from disk: named, and taken out of the sheet (and its pin) with nothing else.
        QVERIFY(QDir(R + "/VidyaGod/[1] Game").removeRecursively());
        Idx = PackageCatalog::BuildCatalogIndex(cfg);
        St = PackageCatalog::StaleSharedPackages(cfg, Idx, Games);
        QCOMPARE(St.Removed, (std::vector<std::string>{"VidyaGod/[1] Game"}));
        const int Before = int(cfg["ShareSheet"].size());
        QCOMPARE(PackageCatalog::DropFromShareSheet(cfg, St.Removed), 1);
        QCOMPARE(int(cfg["ShareSheet"].size()), Before - 1);
        QVERIFY(!cfg["Pins"].contains("VidyaGod/[1] Game") && cfg["Pins"].contains("VidyaGod/[2] Other"));
        QVERIFY(PackageCatalog::StaleSharedPackages(cfg, PackageCatalog::BuildCatalogIndex(cfg), Games).Removed.empty());
        IpfsWrapper::StopNode();
    }

    // A publish never writes under another writer of the library: with its lock held elsewhere (the packaging scripts'
    // flock on <data dir>/library-write.lock) it does nothing and says the library is busy, the sheet untouched; free,
    // it publishes; a lock that cannot be opened is said so, not "busy". Teeth: the lock not taken, taken after the
    // tree was read (a sheet written while busy), an unopenable lock taken for a busy one.
    void a_publish_waits_for_the_librarys_other_writer()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const QString R = Root.path() + "/LIBRARY";
        QDir().mkpath(R + "/VidyaGod/[1] Game");
        writeJson(R + "/VidyaGod/[1] Game/game.json", NodeFixture::Chain("g_exec", {NodeFixture::Exec("win32", "g.exe")}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        QCOMPARE(PackageCatalog::LibraryWriteLock(cfg), std::filesystem::path((Root.path() + "/library-write.lock").toStdString()));
        std::string Gaps;
        {
            const Platform::ScopedFileLock Theirs(PackageCatalog::LibraryWriteLock(cfg));
            QVERIFY(Theirs.held());
            QVERIFY(PackageCatalog::PublishLibrary(cfg, &Err, &Gaps).empty());
            QCOMPARE(Err, std::string(PackageCatalog::LibraryBusy));
            QVERIFY(!cfg.contains("ShareSheet"));
            Err.clear();
            QVERIFY(PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[1] Game").toStdString() }, &Err, &Gaps).empty());
            QCOMPARE(Err, std::string(PackageCatalog::LibraryBusy));
        }
        QVERIFY2(!PackageCatalog::PublishLibrary(cfg, &Err, &Gaps).empty(), Err.c_str());
        QCOMPARE((int)cfg["ShareSheet"].size(), 1);
        //A lock nobody can open is no other writer: said as such, never "busy" (which is waited on for ever).
        json nowhere = json{{"Settings", {{"Paths", {{"LibraryRoot", "/proc/self/vg-nowhere/LIBRARY"}}}}}};
        Err.clear();
        QVERIFY(PackageCatalog::PublishLibrary(nowhere, &Err, &Gaps).empty());
        QVERIFY2(Err.find("cannot be opened") != std::string::npos, Err.c_str());
        IpfsWrapper::StopNode();
    }

    void publishing_one_package_merges_its_row_in_place()
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
                  NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"l_dg"}));
        writeJson(R + "/VidyaGod/[2] Other/u.json", NodeFixture::Chain("u_exec", {NodeFixture::Exec("win32", "u.exe")}));
        writeJson(R + "/VidyaGodRunners/wine/wine.json", NodeFixture::Chain("r_wine", {NodeFixture::Runner("linux", {"win32"}, "wine")}));
        writeJson(R + "/VidyaGodLibraries/dgvoodoo/dg.json", NodeFixture::Chain("l_dg", {NodeFixture::Exec("win32", "dg.exe")}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        std::string Gaps;
        QCOMPARE((int)PackageCatalog::PublishLibrary(cfg, &Err, &Gaps).size(), 4);
        //A package's entries in the sheet ({cid: path}) and its pin.
        const auto RowIn = [&](const char *Lib, const char *Pkg) {
            const std::string Key = std::string(Lib) + "/" + Pkg + "/";
            json E = json::object();
            for (const auto &[C, P] : cfg["ShareSheet"].items()) if (P.get<std::string>().rfind(Key, 0) == 0) E[C] = P;
            return json{{"entries", E}, {"pin", cfg["Pins"].value(std::string(Lib) + "/" + Pkg, std::string())}}; };
        //An edit to a node, as the editor's save leaves it on disk (its handle kept, its bytes changed).
        const auto Edit = [](const QString &File, const char *Note) {
            std::ifstream In(File.toStdString());
            json J = json::parse(In);
            In.close();
            for (auto &N : (J.is_array() ? J : (J = json::array({J})))) N["COMMENT"] = Note;
            NodeFixture::WriteNodes(File.toStdString(), J);
        };

        //1. Only the named package: [2] Other is redone, the edited (stale) runner and the game are left alone.
        const json Wine0 = RowIn("VidyaGodRunners", "wine"), Game0 = RowIn("VidyaGod", "[1] Game"), Other0 = RowIn("VidyaGod", "[2] Other");
        Edit(R + "/VidyaGod/[2] Other/u.json", "v2");
        Edit(R + "/VidyaGodRunners/wine/wine.json", "v2");
        const auto One = PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[2] Other").toStdString() }, &Err, &Gaps);
        QVERIFY2(One.size() == 1, Err.c_str());
        QVERIFY(RowIn("VidyaGod", "[2] Other")["entries"] != Other0["entries"]);
        QCOMPARE(RowIn("VidyaGodRunners", "wine"), Wine0);
        QCOMPARE(RowIn("VidyaGod", "[1] Game"), Game0);
        QCOMPARE((int)cfg["ShareSheet"].size(), 4);         // the old [2] Other entry is replaced, not kept beside it

        //2. A contained package that is stale is redone with the game that contains it.
        const json Dg0 = RowIn("VidyaGodLibraries", "dgvoodoo");
        Edit(R + "/VidyaGodLibraries/dgvoodoo/dg.json", "v2");
        const auto Two = PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[1] Game").toStdString() }, &Err, &Gaps);
        QVERIFY2(Two.size() == 2 && Gaps.empty(), (Err + Gaps).c_str());
        const json Dg1 = RowIn("VidyaGodLibraries", "dgvoodoo");
        QVERIFY(Dg1["entries"] != Dg0["entries"]);
        QVERIFY(RowIn("VidyaGod", "[1] Game")["entries"] != Game0["entries"]);   // it names dgVoodoo's new node
        QCOMPARE(RowIn("VidyaGodRunners", "wine"), Wine0);

        //3. A contained package that is current keeps its entries and pin as they were (no content re-check, no rebuild).
        cfg["Pins"]["VidyaGodLibraries/dgvoodoo"] = "kept";
        QCOMPARE((int)PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[1] Game").toStdString() }, &Err, &Gaps).size(), 2);
        QVERIFY2(RowIn("VidyaGodLibraries", "dgvoodoo")["pin"] == "kept", "a current contained package was rebuilt");

        //4. A folder that is no package under the root is refused, and the sheet is untouched.
        const json Before = cfg["ShareSheet"], PinsBefore = cfg["Pins"];
        QVERIFY(PackageCatalog::PublishPackages(cfg, { Repo.path().toStdString() }, &Err, &Gaps).empty() && !Err.empty());
        Err.clear();
        QVERIFY(PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[9] None").toStdString() }, &Err, &Gaps).empty() && !Err.empty());
        //...also beside a real one: a package named and not there is never skipped silently while the rest publishes.
        Err.clear();
        QVERIFY(PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[2] Other").toStdString(), (R + "/VidyaGod/[9] None").toStdString() },
                                                &Err, &Gaps).empty());
        QVERIFY2(Err.find("[9] None") != std::string::npos, Err.c_str());
        QCOMPARE(cfg["ShareSheet"], Before);
        QCOMPARE(cfg["Pins"], PinsBefore);

        //5. Node files as a package lands them hold no handle: the game names dgVoodoo's node by its CID, and that CID
        //still finds dgVoodoo's package — it is published with the game. Teeth: look references up by handle only.
        for (const char *F : { "/VidyaGod/[1] Game/game.json", "/VidyaGod/[2] Other/u.json", "/VidyaGodRunners/wine/wine.json",
                               "/VidyaGodLibraries/dgvoodoo/dg.json" })
        {
            std::ifstream In((R + F).toStdString());
            json J = json::parse(In);
            In.close();
            for (auto &N : (J.is_array() ? J : (J = json::array({J})))) N.erase("CID");
            NodeFixture::WriteNodes((R + F).toStdString(), J);
        }
        cfg["ShareSheet"] = json::object();                // nothing yet: what this publish writes is all there is
        const auto Five = PackageCatalog::PublishPackages(cfg, { (R + "/VidyaGod/[1] Game").toStdString() }, &Err, &Gaps);
        QVERIFY2(Five.size() == 2, (Err + Gaps).c_str());
        QCOMPARE(Five, (std::vector<std::string>{ "VidyaGod/[1] Game", "VidyaGodLibraries/dgvoodoo" }));
        QCOMPARE((int)RowIn("VidyaGodLibraries", "dgvoodoo")["entries"].size(), 1);
        IpfsWrapper::StopNode();
    }

    // Two of a peer's packages whose names collide ("Foo:1", "foo?1" → "Foo_1", without case) EACH land as
    // "Foo_1 (<hash of its name>)", whatever order they arrive in. Installed is judged by the FINAL name: the one
    // installed is not planned again, the other still is. Teeth: qualify only the second (the names depend on the
    // order); compare with case (one folder on Windows); check the plain name for installed.
    void a_colliding_package_is_judged_installed_by_its_final_name()
    {
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const json Items = json::array({ json{{"cid", "bafkreifirstnode"}, {"pkg", "Foo:1"}, {"node", "a.json"}},
                                         json{{"cid", "bafkreisecondnode"}, {"pkg", "foo?1"}, {"node", "b.json"}} });
        const auto Both = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Both.size(), 2);
        std::map<std::string, std::string> Dir;                              // cid → its folder
        for (const auto & T : Both) Dir[T.Cid] = std::filesystem::path(T.Dest).parent_path().filename().string();
        for (const auto & [C, D] : Dir)
            QVERIFY2(D.size() == std::string("Foo_1 (12345678)").size() && (D.rfind("Foo_1 (", 0) == 0 || D.rfind("foo_1 (", 0) == 0), D.c_str());
        QVERIFY(Dir["bafkreifirstnode"] != Dir["bafkreisecondnode"]);
        const json Reversed = json::array({ Items[1], Items[0] });            // the other order: the same folders
        for (const auto & T : PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Reversed}}))
            QCOMPARE(std::filesystem::path(T.Dest).parent_path().filename().string(), Dir[T.Cid]);
        std::filesystem::create_directories(RxData.path().toStdString() + "/LIBRARY/Games/" + Dir["bafkreisecondnode"]);
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 1);
        QCOMPARE(Plan[0].Cid, std::string("bafkreifirstnode"));
    }

    // A package received again after its installed copy was deleted is a stub again: planning it forgets the install's
    // redirect, or its files went into the deleted package (which then read as installed, and it never came back). An
    // install that finished just after the plan was read keeps its redirect. Teeth: drop the DropRedirectsUnder call in
    // ReceivedFetchTargets and the re-received file is sent to the deleted package; drop its "destination gone" check
    // and the fresh install loses its redirect.
    void planning_a_package_again_ends_its_install_redirect()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::string Src = D.path().toStdString() + "/CATALOG/Alice - Games/[1] A";
        const std::string Dest = D.path().toStdString() + "/LIBRARY/Games/[1] A";
        std::filesystem::create_directories(Dest);
        IpfsWrapper::RedirectDestsUnder(Src, Dest);
        const std::vector<PackageCatalog::ReceivedFetch> Plan{ {"bafkreinode", Src + "/a.json"} };
        const auto Targets = PackageCatalog::ReceivedFetchTargets(Plan);
        QCOMPARE((int)Targets.size(), 1);
        QVERIFY(!Targets[0].Dir);                           // one node file, not a folder
        QCOMPARE(IpfsWrapper::Redirected(Src + "/x.json"), Dest + "/x.json");       // the install is there: it holds
        std::filesystem::remove_all(Dest);                                           // the installed package deleted
        (void)PackageCatalog::ReceivedFetchTargets(Plan);
        QCOMPARE(IpfsWrapper::Redirected(Src + "/x.json"), Src + "/x.json");
        IpfsWrapper::DebugResetQueue();
    }

    // The machine's own LIBRARY is a seed root: a content file its nodes name but the node does not hold is added (the
    // seeder answered "block not found" for four library zips on the replication; only a pinning gateway saved the
    // download), and so is one held only in part (that seeder held each zip's root and none of its leaves; here a
    // reference whose file is gone) — unless its bytes are not the recorded CID, which is reported and never added.
    // Teeth: leave LIBRARY out of SeedRoots and nothing under it is found; skip what HasLocal holds instead of what is
    // held whole and the half-held file stays unservable; add without checking the CID first and the wrong bytes are held.
    void the_library_seeds_what_its_nodes_name_and_it_does_not_hold()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::string R = D.path().toStdString() + "/LIBRARY";
        const std::string Pkg = R + "/VidyaGodLibraries/[5] Codec";
        std::filesystem::create_directories(Pkg);
        const auto write = [](const std::string &P, const std::string &Bytes) { std::ofstream O(P, std::ios::binary); O << Bytes; };
        write(Pkg + "/good.zip", std::string(300000, 'g') + QUuid::createUuid().toString().toStdString());
        write(Pkg + "/bad.zip", "bytes that are not the recorded CID " + QUuid::createUuid().toString().toStdString());
        const std::string HalfBytes = std::string(600000, 'h') + QUuid::createUuid().toString().toStdString();
        write(Pkg + "/half.zip", HalfBytes);
        const std::string Good = IpfsWrapper::ComputeCid(Pkg + "/good.zip", &Err);
        QVERIFY2(!Good.empty(), Err.c_str());
        // half.zip: held only through a copy elsewhere, since deleted — its root is held, its leaves are not servable.
        const std::string Elsewhere = D.path().toStdString() + "/copy.zip";
        write(Elsewhere, HalfBytes);
        const std::string Half = IpfsWrapper::AddNoCopy(Elsewhere, &Err);
        QVERIFY2(!Half.empty(), Err.c_str());
        std::filesystem::remove(Elsewhere);
        QVERIFY2(IpfsWrapper::HasLocal(Half) && !IpfsWrapper::HeldWhole(Half), "precondition: held in part only");
        const std::string Recorded = Cid::OfBytes("what bad.zip was published as " + QUuid::createUuid().toString().toStdString());
        writeJson(QString::fromStdString(Pkg + "/codec.json"), NodeFixture::Chain("codec", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "good.zip", Good), NodeFixture::ContentCid("zip", "bad.zip", Recorded),
            NodeFixture::ContentCid("zip", "half.zip", Half)})}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R}}}}}};
        const auto Roots = PackageCatalog::SeedRoots(cfg);
        QVERIFY2(std::find(Roots.begin(), Roots.end(), R) != Roots.end(), "the LIBRARY is a seed root");
        QVERIFY2(!IpfsWrapper::HasLocal(Good), "precondition: the good file is not held");
        std::vector<PackageCatalog::SeedFailure> Bad;
        int Added = 0;
        for (const std::string &Dir : Roots) Added += PackageCatalog::SeedUnheld(Dir, &Bad);
        QCOMPARE(Added, 2);
        QVERIFY2(IpfsWrapper::HeldWhole(Good), "the file its node names is held and servable");
        QVERIFY2(IpfsWrapper::HeldWhole(Half), "the half-held file is held whole again, from the library's copy");
        QCOMPARE((int)Bad.size(), 1);
        QCOMPARE(Bad[0].RecordedCid, Recorded);
        QVERIFY2(!IpfsWrapper::HasLocal(Bad[0].ActualCid), "bytes that are not the recorded CID are never added");
        QCOMPARE(PackageCatalog::SeedUnheld(R, &Bad), 0);                     // held now: nothing to add again

        // A node naming files outside its package (a peer's node lands verbatim) yields no target: seeding read,
        // pinned and announced whatever it named — /dev/zero, forever. Teeth: drop the PathWithin check in
        // ManifestTargets and these two are targets.
        std::filesystem::create_directories(D.path().toStdString() + "/outside");
        write(D.path().toStdString() + "/outside/escape.zip", "outside the package");
        writeJson(QString::fromStdString(Pkg + "/hostile.json"), NodeFixture::Chain("hostile", {NodeFixture::Merge({
            NodeFixture::ContentCid("file", "/etc/hostname", Good), NodeFixture::ContentCid("zip", "../../../outside/escape.zip", Good)})}));
        for (const auto &[Path, Cid] : PackageCatalog::SeedTargets(R))
            QVERIFY2(NodeGraph::PathWithin(R, Path), ("a seed target outside the library: " + Path).c_str());

        // A package source inside the LIBRARY is walked with it, once (every heal swept each source twice). Teeth:
        // compare roots by string only and the source dir is listed again.
        std::filesystem::create_directories(R + "/VidyaGodLibraries");
        json withSrc = cfg;
        withSrc["Settings"]["PackageSources"] = json::array({ json{{"CID", "bafysourcefolder"}, {"NAME", "VidyaGodLibraries"}} });
        QCOMPARE((int)PackageCatalog::PackageSourceDirs(withSrc).size(), 1);  // precondition: the source is there
        QCOMPARE((int)PackageCatalog::SeedRoots(withSrc).size(), 1);

        // The heal's SeedDirectory never adds wrong bytes either, and names them even with no counter passed. Teeth:
        // add before checking the CID in SeedDirectory's not-held branch, or gate Failures on the counter again.
        const std::string Wrong = Cid::OfBytes("recorded, never these bytes " + QUuid::createUuid().toString().toStdString());
        write(Pkg + "/wrong.zip", "these are not the recorded bytes " + QUuid::createUuid().toString().toStdString());
        writeJson(QString::fromStdString(Pkg + "/wrong.json"), NodeFixture::Chain("wrong", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "wrong.zip", Wrong)})}));
        std::vector<PackageCatalog::SeedFailure> HealBad;
        PackageCatalog::SeedDirectory(R, {}, nullptr, false, false, false, &HealBad);
        const auto W = std::find_if(HealBad.begin(), HealBad.end(), [&](const PackageCatalog::SeedFailure &F) { return F.RecordedCid == Wrong; });
        QVERIFY2(W != HealBad.end(), "the heal names the file whose bytes are not its CID");
        QVERIFY2(!IpfsWrapper::HasLocal(W->ActualCid), "and never adds them");

        // Stopped: nothing is read or hashed. Teeth: drop the DaemonRunning check and the unheld file is hashed and
        // reported as not added.
        write(Pkg + "/later.zip", std::string(1000, 'z') + QUuid::createUuid().toString().toStdString());
        writeJson(QString::fromStdString(Pkg + "/later.json"), NodeFixture::Chain("later", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "later.zip", IpfsWrapper::ComputeCid(Pkg + "/later.zip", &Err))})}));
        IpfsWrapper::StopNode();
        std::vector<PackageCatalog::SeedFailure> Stopped;
        QCOMPARE(PackageCatalog::SeedUnheld(R, &Stopped), 0);
        QVERIFY2(Stopped.empty(), "a stopped node read and reported files");
    }

    // The start's seed pass reads each file once — checked as it is added, no ComputeCid first, no pin walking every
    // leaf back — the smallest first, and a file found not to be its recorded CID is not read again until it changes.
    // Teeth: check with ComputeCid before adding (or pin by walking) and the pass reads the good file more than once;
    // drop the sort and the big file is tried first; drop the remembered mismatches and the second pass reads both
    // wrong files again.
    void a_seed_pass_reads_each_file_once_smallest_first_and_wrong_ones_not_again()
    {
#ifndef __linux__
        QSKIP("counts reads through /proc/self/io");
#endif
        const auto ReadSoFar = [] {
            std::ifstream In("/proc/self/io");
            for (std::string L; std::getline(In, L); )
                if (L.rfind("rchar: ", 0) == 0) return std::stoll(L.substr(7));
            return -1LL;
        };
        QTemporaryDir data; QVERIFY(data.isValid());
        const std::string Saved = AppPaths::DataRootSet() ? AppPaths::DataRoot().string() : std::string();
        const auto Restore = qScopeGuard([&] { AppPaths::SetDataRoot(Saved); IpfsWrapper::StopNode(); });
        AppPaths::SetDataRoot(data.path().toStdString());
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode((data.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        const std::string R = data.path().toStdString() + "/LIBRARY";
        const std::string Pkg = R + "/VidyaGod/[9] Discs";
        std::filesystem::create_directories(Pkg);
        const auto write = [](const std::string &P, std::size_t Size, char Fill) {
            std::ofstream O(P, std::ios::binary);
            O << QUuid::createUuid().toString().toStdString() << std::string(Size, Fill);
        };
        write(Pkg + "/a big disc.iso", 6 << 20, 'b');      // first by name, last by size
        write(Pkg + "/b small.zip", 100000, 's');
        write(Pkg + "/c good.zip", 8 << 20, 'g');
        const std::string Good = IpfsWrapper::ComputeCid(Pkg + "/c good.zip", &Err);
        QVERIFY2(!Good.empty(), Err.c_str());
        const std::string BigWas = Cid::OfBytes("the disc as it was published " + QUuid::createUuid().toString().toStdString());
        const std::string SmallWas = Cid::OfBytes("the zip as it was published " + QUuid::createUuid().toString().toStdString());
        writeJson(QString::fromStdString(Pkg + "/discs.json"), NodeFixture::Chain("discs", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "a big disc.iso", BigWas), NodeFixture::ContentCid("zip", "b small.zip", SmallWas),
            NodeFixture::ContentCid("zip", "c good.zip", Good)})}));
        const long long Bytes = (6 << 20) + 100000 + (8 << 20);

        std::vector<PackageCatalog::SeedFailure> Bad;
        long long Before = ReadSoFar();
        QCOMPARE(PackageCatalog::SeedUnheld(R, &Bad), 1);
        const long long First = ReadSoFar() - Before;
        QVERIFY2(IpfsWrapper::HeldWhole(Good), "the good file is held whole");
        QVERIFY2(First < Bytes * 3 / 2, ("the pass read " + std::to_string(First >> 20) + " MiB of "
                                         + std::to_string(Bytes >> 20) + " MiB of files: a file was read more than once").c_str());
        QCOMPARE((int)Bad.size(), 2);
        QCOMPARE(Bad[0].RecordedCid, SmallWas);                                 // smallest first
        QCOMPARE(Bad[1].RecordedCid, BigWas);
        const std::string BigIs = Bad[1].ActualCid;
        QVERIFY2(!BigIs.empty() && !IpfsWrapper::HasLocal(BigIs), "the wrong bytes are answered, never added");

        Bad.clear();
        Before = ReadSoFar();
        QCOMPARE(PackageCatalog::SeedUnheld(R, &Bad), 0);
        const long long Second = ReadSoFar() - Before;
        QVERIFY2(Second < (1 << 20), ("the next pass read " + std::to_string(Second >> 10) + " KiB: the wrong files were read again").c_str());
        const auto Found = [&](const std::string &Recorded) {
            const auto F = std::find_if(Bad.begin(), Bad.end(), [&](const PackageCatalog::SeedFailure &X) { return X.RecordedCid == Recorded; });
            return F == Bad.end() ? std::string("(not reported)") : F->ActualCid;
        };
        QCOMPARE((int)Bad.size(), 2);                                           // still reported, as found
        QCOMPARE(Found(BigWas), BigIs);

        write(Pkg + "/a big disc.iso", 6 << 20, 'B');                           // changed since: read again
        Bad.clear();
        Before = ReadSoFar();
        PackageCatalog::SeedUnheld(R, &Bad);
        QVERIFY2(ReadSoFar() - Before >= (6 << 20), "a wrong file that changed since is read again");
        QCOMPARE((int)Bad.size(), 2);
        QVERIFY(Found(BigWas) != BigIs && Found(BigWas) != "(not reported)");

        // Held only through a file since deleted, and the library's copy is other bytes: nothing can serve it, so it is
        // no longer pinned and offered (the start's heal dropped it, before this pass replaced it). Teeth: keep the pin
        // after the mismatch and the dead content stays held and announced.
        const std::string Elsewhere = data.path().toStdString() + "/gone.zip", Kept = data.path().toStdString() + "/kept.zip";
        write(Kept, 300000, 'o');
        std::filesystem::copy_file(Kept, Elsewhere);
        const std::string Dead = IpfsWrapper::AddNoCopy(Elsewhere, &Err);
        QVERIFY2(!Dead.empty(), Err.c_str());
        std::filesystem::remove(Elsewhere);
        write(Pkg + "/d other.zip", 300000, 'x');
        writeJson(QString::fromStdString(Pkg + "/dead.json"), NodeFixture::Chain("dead", {NodeFixture::Merge({
            NodeFixture::ContentCid("zip", "d other.zip", Dead)})}));
        QVERIFY2(IpfsWrapper::HasLocal(Dead) && IpfsWrapper::CidMissing(Dead), "precondition: held over a gone file");
        Bad.clear();
        PackageCatalog::SeedUnheld(R, &Bad);
        QVERIFY2(!IpfsWrapper::HasLocal(Dead), "content held only over a gone file, with no copy that is it, stays pinned");
        // And when the library's copy is already known not to be it (not read again): the same. Teeth: drop only after a
        // fresh read and the content held again through a copy since deleted stays pinned.
        std::filesystem::copy_file(Kept, Elsewhere);
        QCOMPARE(IpfsWrapper::AddNoCopy(Elsewhere, &Err), Dead);   // same bytes: the same CID
        std::filesystem::remove(Elsewhere);
        Bad.clear();
        Before = ReadSoFar();
        PackageCatalog::SeedUnheld(R, &Bad);
        QVERIFY2(ReadSoFar() - Before < (1 << 20), "the known-wrong copy was read again");
        QVERIFY2(!IpfsWrapper::HasLocal(Dead), "content held only over a gone file, known to have no copy here, stays pinned");
    }

    // A fresh index is built after the call began (a node written just before is in it), and callers asking at once
    // share builds: 8 at once get at most two. Every download rebuilt the whole index itself — 46 resumed downloads
    // ran up to 138 scans together. Teeth: build per call (no single flight) and 8 callers get 8 indexes; return the
    // last build without checking it began after the call and the node written just before is missing.
    void a_fresh_catalog_index_is_shared_and_never_stale()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const QString R = D.path() + "/LIBRARY";
        for (int i = 0; i < 300; ++i)                                         // enough that a build outlasts the arrivals
        {
            QDir().mkpath(R + QString("/VidyaGod/[%1] P%1").arg(i + 10));
            writeJson(R + QString("/VidyaGod/[%1] P%1/n.json").arg(i + 10),
                      json{{"LABEL", "n" + std::to_string(i)}, {"LAYERS", json::array({ json{{"DIR", "d"}} })}});
        }
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        QVERIFY(PackageCatalog::FreshCatalogIndex(cfg)->Find("n0"));
        QVERIFY(!PackageCatalog::FreshCatalogIndex(cfg)->Find("late"));
        QDir().mkpath(R + "/VidyaGod/[5] Late");
        writeJson(R + "/VidyaGod/[5] Late/l.json", json{{"LABEL", "late"}, {"LAYERS", json::array({ json{{"DIR", "l"}} })}});
        QVERIFY2(PackageCatalog::FreshCatalogIndex(cfg)->Find("late"), "a node written before the call is in the index");
        std::vector<std::thread> T;
        std::vector<const NodeIndex *> Got(8, nullptr);
        std::vector<std::shared_ptr<const NodeIndex>> Keep(8);
        std::atomic<int> Ready{0};
        for (int i = 0; i < 8; ++i)
            T.emplace_back([&, i]{ ++Ready; while (Ready < 8) {} Keep[i] = PackageCatalog::FreshCatalogIndex(cfg); Got[i] = Keep[i].get(); });
        for (auto &t : T) t.join();
        std::set<const NodeIndex *> Distinct(Got.begin(), Got.end());
        QVERIFY2(Distinct.size() <= 2, ("8 callers at once got " + std::to_string(Distinct.size()) + " indexes").c_str());
    }

    // Two peers with one nickname file their libraries under two labels (one would replace the other's node files),
    // and a peer keeps its label through a rename (its folders stay where they are). Teeth: the live nickname as the
    // label (both "Deck"; the renamed one moves).
    void two_peers_named_alike_get_two_labels_and_a_rename_keeps_one()
    {
        json Cfg = json::object();
        const std::string A = "12D3KooWAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAaaa111",
                          B = "12D3KooWBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBbbb222";
        QCOMPARE(QString::fromStdString(PackageCatalog::AssignReceivedLabel(Cfg, A, "Deck")), QStringLiteral("Deck"));
        const std::string LB = PackageCatalog::AssignReceivedLabel(Cfg, B, "deck");   // the same folder, case aside
        QCOMPARE(QString::fromStdString(LB), QStringLiteral("deck [bbb222]"));   // its ID's tail, short
        QCOMPARE(QString::fromStdString(PackageCatalog::AssignReceivedLabel(Cfg, A, "Renamed")), QStringLiteral("Deck"));
        QCOMPARE(QString::fromStdString(PackageCatalog::ReceivedNickLabel(Cfg, A)), QStringLiteral("Deck"));
        QCOMPARE(PackageCatalog::ReceivedNickLabel(Cfg, B), LB);
        //A nick holding the folder's own separator cannot make another peer's folder name. Teeth: kept as it is.
        const std::string C = "12D3KooWCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCccc333";
        QCOMPARE(QString::fromStdString(PackageCatalog::AssignReceivedLabel(Cfg, C, "Deck - Steam")), QStringLiteral("Deck · Steam"));
        const std::string D = "12D3KooWDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDddd444";
        QCOMPARE(QString::fromStdString(PackageCatalog::AssignReceivedLabel(Cfg, D, "Deck -")), QStringLiteral("Deck [ddd444]"));   // "Deck" is A's
    }

    // The planner lands one file per entry of a peer's sheet: CATALOG/<nick> - <lib>/<pkg>/<file>, the same entry twice
    // once, and two CIDs claiming one file name in one package (without case) each at its bumped name. Teeth: key the
    // plan by package (one target per package); let the second CID overwrite the first's file; bump only one.
    void planner_lands_one_file_per_entry()
    {
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", "/tmp/vg_rx_plan/LIBRARY"}}}}}};
        const json Items = json::array({
            json{{"cid", "bafkreione"}, {"pkg", "[802] Warcraft III"}, {"node", "Warcraft III 1.0.json"}},
            json{{"cid", "bafkreitwo12345678"}, {"pkg", "[802] Warcraft III"}, {"node", "warcraft iii 1.0.json"}},
            json{{"cid", "bafkreione"}, {"pkg", "[802] Warcraft III"}, {"node", "Warcraft III 1.0.json"}},
            json{{"cid", "bafkreithree"}, {"pkg", "[802] Warcraft III"}, {"node", "Frozen Throne"}} });
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 3);
        //A node file is one raw block: a folder's or a file DAG's CID named as one is not planned (its root can say any
        //size). Teeth: any alphanumeric CID planned.
        QCOMPARE((int)PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", json::array({
            json{{"cid", "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi"}, {"pkg", "P"}, {"node", "a.json"}},
            json{{"cid", "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG"}, {"pkg", "P"}, {"node", "b.json"}} })}}).size(), 0);
        //...and a package with one such entry is left out whole: planned short it would read complete. Teeth: drop the
        //entry alone (one target planned).
        QCOMPARE((int)PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", json::array({
            json{{"cid", "bafkreigoodnodeaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {"pkg", "Q"}, {"node", "a.json"}},
            json{{"cid", "bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi"}, {"pkg", "Q"}, {"node", "b.json"}} })}}).size(), 0);
        std::map<std::string, std::filesystem::path> At;
        for (const auto & T : Plan) At[T.Cid] = T.Dest;
        QCOMPARE(At["bafkreione"].parent_path().filename(), std::filesystem::path("[802] Warcraft III"));
        QCOMPARE(At["bafkreione"].parent_path().parent_path().filename(), std::filesystem::path("Alice - Games"));
        QCOMPARE(At["bafkreione"].filename(), std::filesystem::path("Warcraft III 1.0 [fkreione].json"));   // both bumped
        QCOMPARE(At["bafkreitwo12345678"].filename(), std::filesystem::path("warcraft iii 1.0 [12345678].json"));
        QCOMPARE(At["bafkreithree"].filename(), std::filesystem::path("Frozen Throne.json"));   // a node file
        //A peer that names a file what a bump makes of another's: one file, one CID — never two fetches racing for
        //it. Teeth: drop the planned-names guard (two targets at one dest).
        const json Hostile = json::array({
            json{{"cid", "bafkreione"}, {"pkg", "P"}, {"node", "x.json"}},
            json{{"cid", "bafkreitwo12345678"}, {"pkg", "P"}, {"node", "X.json"}},
            json{{"cid", "bafkreifour"}, {"pkg", "P"}, {"node", "x [fkreione].json"}} });
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Hostile}});
        std::set<std::string, ContentFile::NameLess> Dests;
        for (const auto & T : Plan2) QVERIFY2(Dests.insert(T.Dest).second, ("two CIDs planned to " + T.Dest).c_str());
        QCOMPARE((int)Plan2.size(), 2);
    }

    // What the Library shows is what is HERE. A peer's node whose file is not here is not installed, whether or not
    // it names a CID (one that names none is one nobody can fetch, never one this machine has); a package of ours
    // missing a file it names no CID for is broken, not un-downloaded (validation's), but with no file of its own here
    // it has nothing to show either (OnDisk) — nor has one with no content at all. Teeth: judge a received node by its
    // CID alone (it reads installed); take a declared layer for one on disk.
    void the_library_shows_only_games_with_files_here()
    {
        QTemporaryDir Tmp;
        QVERIFY(Tmp.isValid());
        const std::filesystem::path D = Tmp.path().toStdString();
        { std::ofstream(D / "here.zip") << "zip"; }
        NodeIndex Idx;
        const auto Add = [&](const std::string &Id, json Layers, bool Received) {
            Node N;
            QVERIFY(ManifestModel::ParseNode(json{{"CID", Id}, {"LABEL", Id}, {"LAYERS", Layers}}, D / (Id + ".json"), D, N));
            N.Received = Received;
            Idx.Nodes[N.Key()] = N;
        };
        Add("peers", json::array({ json{{"ZIP", "gone.zip"}, {"TARGET", "FILES"}} }), true);
        Add("ours_broken", json::array({ json{{"ZIP", "gone.zip"}, {"TARGET", "FILES"}} }), false);
        Add("ours", json::array({ json{{"ZIP", "here.zip"}, {"TARGET", "FILES"}} }), false);
        Add("empty", json::array({ json{{"VARS", json{{"x", json{{"DEFAULT", "1"}}}}}} }), false);
        Add("sourced", json::array({ json{{"FILE", "game.chd"}, {"SOURCE", "QmVKWrSnH7BJDvPWdQh4qXjCCqtGYAvSC5wCWiJXtCrTAz"}, {"TARGET", "FILES"}} }), true);
        Add("over_sourced", json::array({ json{{"NODE", "sourced"}} }), true);
        Add("waiting", json::array({ json{{"NODE", "not_landed_yet"}} }), true);
        Add("over_waiting", json::array({ json{{"NODE", "waiting"}} }), true);
        const auto Hyd = Installed::HydrationMap(Idx);
        //What the Catalog offers, from the same pass: something to fetch beneath it, or a package still to land.
        //Teeth: fold neither from what a node contains; mark a missing contained node complete.
        QVERIFY2(Hyd.at("over_sourced").HasCid && !Hyd.at("peers").HasCid, "a CID beneath it is something to fetch");
        QVERIFY2(Hyd.at("over_waiting").Incomplete && !Hyd.at("over_sourced").Incomplete, "a contained node not here yet");
        QVERIFY2(!Hyd.at("peers").Hydrated, "a peer's game with its file not here is not installed");
        QVERIFY2(Hyd.at("ours_broken").Hydrated && !Hyd.at("ours_broken").OnDisk, "nothing of ours is here to show");
        QVERIFY2(Hyd.at("ours").Hydrated && Hyd.at("ours").OnDisk, "a file of it is here");
        QVERIFY2(!Hyd.at("empty").OnDisk && !Hyd.at("empty").HasContent, "no content at all");
    }

    // A node is at most 256 KiB (one raw block, §2.3). The index refuses a larger one, and the validator names its FILE
    // with the split that brings it under, not only its container's "missing node", and only in that package's scope.
    // Teeth: index a node of exactly 256 KiB too (> vs >=); drop the error, its hint, or its scoping.
    void a_node_over_256_kib_is_named_by_its_file()
    {
        json Big = {{"LABEL", "depot"}, {"LAYERS", json::array()}};
        for (int i = 0; Cid::Canonical(Big).size() <= Cid::MaxNodeBytes; ++i)
            Big["LAYERS"].push_back(json{{"EDIT", json::array({json{{"MODE", "Delete"}}})},
                                         {"TARGET", "FILES/Game.app/Contents/Resources/file" + std::to_string(i)}});
        json Edge = {{"LABEL", "edge"}, {"COMMENT", ""}, {"LAYERS", json::array()}};
        Edge["COMMENT"] = std::string(Cid::MaxNodeBytes - Cid::Canonical(Edge).size(), 'x');
        QCOMPARE(Cid::Canonical(Edge).size(), Cid::MaxNodeBytes);

        std::map<std::string, json> Tree; std::map<std::string, std::filesystem::path> Dirs;
        const auto Put = [&](const std::string &File, const json &N) {
            const std::string Key = "\x01" + File + "#1";
            Tree[Key] = N;
            Dirs[Key] = std::filesystem::path(File).parent_path();
        };
        Put("/lib/P/bigdepot.json", Big);
        Put("/lib/P/top.json", json{{"LABEL", "top"}, {"LAYERS", json::array({json{{"NODE", "bigdepot"}}})}});
        Put("/lib/P/edge.json", Edge);
        Put("/lib/Q/other.json", json{{"LABEL", "other"}, {"LAYERS", json::array()}});
        const NodeIndex Idx = NodeGraph::FreezeToIndex(Tree, Dirs);
        QCOMPARE(Idx.Nodes.size(), size_t(3));
        QVERIFY2(Idx.Find(Cid::OfNode(Edge)), "exactly 256 KiB is one block, and indexed");
        QCOMPARE(Idx.RefusedNodes.size(), size_t(1));
        QCOMPARE(Idx.RefusedNodes[0].File, std::filesystem::path("/lib/P/bigdepot.json"));

        const auto Validate = [&](const std::set<std::string> *Scope) {
            std::vector<std::string> E, W;
            Validate::Nodes(Idx, E, W, Scope);
            return E;
        };
        const auto Has = [](const std::vector<std::string> &E, const std::string &A, const std::string &B) {
            return std::any_of(E.begin(), E.end(), [&](const std::string &S) {
                return S.find(A) != std::string::npos && S.find(B) != std::string::npos; });
        };
        const std::vector<std::string> All = Validate(nullptr);
        QVERIFY2(Has(All, "node file '/lib/P/bigdepot.json' is refused", "over the 256 KiB"), "the file is named");
        QVERIFY2(Has(All, "bigdepot.json", "put a NODE layer naming it in their place"), "with the split");
        QVERIFY2(Has(All, "contains missing node 'bigdepot'", "refused"), "its container points at it");

        const std::string Top = Cid::OfNode(Tree.at("\x01/lib/P/top.json#1"));
        const std::string Other = Cid::OfNode(Tree.at("\x01/lib/Q/other.json#1"));
        const std::set<std::string> InP = {Top}, InQ = {Other};
        QVERIFY2(Has(Validate(&InP), "bigdepot.json", "is refused"), "in its own package's scope");
        QVERIFY2(!Has(Validate(&InQ), "bigdepot.json", "is refused"), "not in another package's");
    }

    // The Windows runner comes with the app: on a machine that hosts it (a Windows PC) the catalog has it without a
    // download, and it is the very node published in VidyaGodRunners — a copy on disk is the same node, kept once.
    // Teeth: add it on any machine (linux64 gets it too); change its bytes (no longer the published CID).
    void the_windows_runner_comes_with_the_app()
    {
        const auto &All = BuiltinRunners::All();
        QCOMPARE(All.size(), size_t(1));
        QCOMPARE(Cid::OfBytes(Cid::Canonical(All[0].Node)), std::string("bafkreic77imjhq76wb4vb3dgl7tmo7gz42q2somalux3tek2bwut5nnjdi"));

        std::map<std::string, json> Tree; std::map<std::string, std::filesystem::path> Dirs;
        BuiltinRunners::AddTo(Tree, Dirs, "/lib", "linux64");
        QVERIFY2(Tree.empty(), "a Linux machine cannot host it, so it gets none");
        BuiltinRunners::AddTo(Tree, Dirs, "/lib", "win64");
        QCOMPARE(Tree.size(), size_t(1));

        // Beside a copy of the same node read from disk (a handle-less file: synthetic key), one node results.
        Tree["\x01/lib/VidyaGodRunners/windows-native/copy.json#1"] = All[0].Node;
        Dirs["\x01/lib/VidyaGodRunners/windows-native/copy.json#1"] = "/lib/VidyaGodRunners/windows-native";
        const NodeIndex Idx = NodeGraph::FreezeToIndex(Tree, Dirs);
        QCOMPARE(Idx.Nodes.size(), size_t(1));
        const Node *R = Idx.Find("bafkreic77imjhq76wb4vb3dgl7tmo7gz42q2somalux3tek2bwut5nnjdi");
        QVERIFY(R);
        QVERIFY(R->IsRunner());
        QCOMPARE(R->HostPlatform, std::string("win64"));
        QCOMPARE(R->GuestPlatform, (std::vector<std::string>{"win32", "win64"}));
    }

private:   // a helper, not a test: moc never sees its json parameters (no metatype for json on Windows)
    // The files of a published folder: name → bytes (fetched like a receiver would, into a throwaway dir).
    // A received package folder as the app lands it: the folder over the listed node files (referenced where they
    // landed, as a fetch leaves them), recorded as peer Alice's share entry {cid, pkg} in library `Lib`.
    // A peer ("Alice") shares the node files Listed (names in Dir) as package Pkg of library Lib.
    static void shareEntries(json & Rx, const std::string & Lib, const std::string & Pkg, const std::string & Dir,
                             const std::vector<std::string> & Listed)
    {
        std::string Err;
        for (const std::string & F : Listed)
        {
            const std::string C = IpfsWrapper::AddNoCopy(Dir + "/" + F, &Err);
            QVERIFY2(!C.empty(), Err.c_str());
            Rx["PeerLibraries"]["Alice"][Lib].push_back(json{{"cid", C}, {"pkg", Pkg}, {"node", F}});   // an unknown peer files under its id
        }
    }
private slots:

    // The share sheet's node at Path ("" if none).
    static std::string cidAt(const json & Cfg, const std::string & Path)
    {
        if (Cfg.contains("ShareSheet"))
            for (const auto & [C, P] : Cfg["ShareSheet"].items()) if (P == Path) return C;
        return {};
    }
    // What a published node contains, read from its block (the sender's closure in SharedWith).
    static std::vector<std::string> containsOf(const std::string & Cid)
    {
        std::string Err;
        const json J = json::parse(IpfsWrapper::BlockGet(Cid, &Err), nullptr, false);
        return J.is_discarded() ? std::vector<std::string>() : NodeRefs::Contained(J);
    }

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

private:   // a helper, not a test: moc never sees its json parameters (no metatype for json on Windows)
    // Land a peer's share the way the app does: each package folder through the real queue, then complete + check.
    // What the seeder published, by CID: one IPFS node plays both machines here, and a receiver's fetch takes a block
    // into its landed file — so before each landing, the seeder's blocks are put back, as a seeder holds them.
    std::map<std::string, std::string> SeederBlocks;
    void landShares(const json & Rx, const std::string & Nick, const json & Libs)
    {
        for (const auto & [C, B] : SeederBlocks) { std::string E; IpfsWrapper::BlockPut(B, &E); }
        std::vector<IpfsWrapper::FetchTarget> B;
        for (const auto & T : PackageCatalog::PlanReceivedFetches(Rx, Nick, Libs))
            B.push_back(IpfsWrapper::FetchTarget{ T.Cid, T.Dest, /*Optional=*/false, /*Dir=*/false, /*Verify=*/true });
        std::string WErr;
        QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(B), 30000, &WErr), WErr.c_str());
        std::string LErr;
        QVERIFY2(PackageCatalog::LandReceivedPackages(Rx, &LErr), LErr.c_str());
    }
private slots:

private:   // a helper, not a test: moc never sees its json parameters (no metatype for json on Windows)
    // Helper: publish two games in one collection and return the library's share entries as a peer receives them —
    // one per NODE ({cid, pkg, node: its file name}), sorted by package and file.
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
        // B's title falls BETWEEN the base game's and its expansion's: a flat title sort would split the family.
        writeJson(root + "/VidyaGod/[2] B/tile.json", NodeFixture::Chain("b_tile", {NodeFixture::Tile("2", "A: Bonus")}));
        writeJson(root + "/VidyaGod/[2] B/b.json",
                  NodeFixture::Chain("b_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "b.exe"), NodeFixture::Variant("Play")})}, {"b_tile"}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", root.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(seeder, &Err);
        for (const auto & [C, P] : seeder.value("ShareSheet", json::object()).items()) SeederBlocks[C] = IpfsWrapper::BlockGet(C, &Err);
        const json Wired = PackageCatalog::Wire(PackageCatalog::SheetEntries(seeder.value("ShareSheet", json::object())));
        return Wired.contains("VidyaGod") ? Wired["VidyaGod"] : json::array();
    }
private slots:

    // The cid of the node labelled `Label` among share entries (its file is named by its label).
    std::string cidOfLabel(const json & Items, const std::string & Label)
    {
        for (const auto & It : Items) if (It.value("node", std::string()) == Label + ".json") return It.value("cid", std::string());
        return {};
    }

    // A received share is a peer's share sheet entries: the planner lands each at "<Nick> - <Lib>/<pkg>/<file>" through
    // the rolling queue — every node file verbatim, under its name — and from there zero peer-specific code runs:
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
        QCOMPARE((int)Items.size(), 7);                         // packages A (5 nodes) and B (2)

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);   // sibling of LIBRARY → RxData/CATALOG

        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan.size(), 7);
        const std::string Lib = Catalog + "/Alice - Games";     // received stubs land in CATALOG, not LIBRARY
        std::set<std::string> Dests;
        for (const auto & T : Plan) Dests.insert(T.Dest);
        QVERIFY2(Dests.count(Lib + "/[1] A/a_exec.json"), "a node lands in the seeder's package dir, under its name");
        QVERIFY2(Dests.count(Lib + "/[2] B/b_exec.json"), "second package gets its own dir");

        landShares(rx, "Alice", json{{"Games", Items}});   // the files land through the REAL queue
        QVERIFY2(!PackageCatalog::ReceivedPackagesIncomplete(rx), "every node file the entries name is on disk");
        const std::string ACid = cidOfLabel(Items, "a_exec");
        {
            std::ifstream In(Lib + "/[1] A/a_exec.json", std::ios::binary);
            const std::string Landed((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            QCOMPARE(Cid::OfBytes(Landed), ACid);                // landed VERBATIM: the file is the block
        }

        auto Idx = PackageCatalog::BuildCatalogIndex(rx);
        const Node * A = Idx.Find("a_exec");
        QVERIFY2(A, "a landed raw block freezes back in the plain tree scan");
        QCOMPARE(A->BundleDir, std::filesystem::path(Lib + "/[1] A"));   // content hydrates beside its node files
        QCOMPARE(A->Key(), ACid);
        QVERIFY2(A->Received, "…marked RECEIVED (it lives under CATALOG): never a graft candidate");
        const auto under = [](const std::filesystem::path &P, const std::string &Dir) {   // separators differ on Windows
            return P.lexically_normal().generic_string().rfind(std::filesystem::path(Dir).lexically_normal().generic_string(), 0) == 0;
        };
        for (const auto & [K, N] : Idx.Nodes)
            if (!under(N.BundleDir, Catalog)) QVERIFY2(!N.Received, "a LIBRARY node is not Received");
        QVERIFY2(!PackageCatalog::NodeClosureIncomplete(Idx, A->Key()), "the whole package landed: the closure is complete");
        int LandedA = 0;
        for (const auto & [K, N] : Idx.Nodes)
            if (N.Received && under(N.BundleDir, Lib + "/[1] A")) ++LandedA;
        QCOMPARE(LandedA, 5);                                   // a_exec, a_x, a_tile, a_content, a_base — all Received

        // Vacuous-hydration guard: an un-installed received game (content not fetched) reads NOT hydrated —
        // that is what routes it to the Catalog tab (downloadable) instead of the Library tab (playable).
        const auto Hyd = Installed::HydrationMap(Idx);
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
        for (const auto & T : Shelf::Tiles(Idx))
        {
            Order.push_back(T.Uid);
            for (const Node * N : T.Rows) RowsOf[T.Uid].push_back(N->NodeId);
        }
        const auto Base = std::find(Order.begin(), Order.end(), std::string("1"));
        QVERIFY2(Base != Order.end() && Base + 1 != Order.end() && *(Base + 1) == "1x", "the expansion's tile follows its base game's");
        // … even though "A: Bonus" sorts between "A" and "A: Expansion". Teeth: sort tiles by title alone and 2 lands
        // between 1 and 1x.
        QCOMPARE(RowsOf["1"], std::vector<std::string>{"a_exec"});
        QCOMPARE(RowsOf["1x"], std::vector<std::string>{"a_x"});

        // A HOSTILE snapshot (traversal in every routed field) must stay inside the CATALOG root.
        const json Evil = json::array({json{{"cid", Items[0].value("cid", std::string())}, {"node", "../../pwn"}, {"pkg", "../../.."}}});
        const auto EvilPlan = PackageCatalog::PlanReceivedFetches(rx, "..", json{{"..", Evil}});
        QVERIFY(!EvilPlan.empty());
        const std::string RootPrefix = std::filesystem::weakly_canonical(Catalog).generic_string() + "/";
        for (const auto & T : EvilPlan)
        {
            const std::string Canon = std::filesystem::weakly_canonical(T.Dest).generic_string();
            QVERIFY2(Canon.rfind(RootPrefix, 0) == 0, ("hostile dest escaped the CATALOG root: " + Canon).c_str());
        }

        // Planner dedupe: the same snapshot plans the same targets again (stable), never duplicates within one plan.
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}});
        QCOMPARE((int)Plan2.size(), 7);
        std::set<std::string> D2; for (const auto & T : Plan2) D2.insert(T.Dest);
        QCOMPARE(D2.size(), Plan2.size());

        // CATALOG stubs are BROWSE-ONLY: publish scans LIBRARY only, so a re-publish must NOT include a received
        // package — you re-share what you've installed, not everything you've browsed.
        std::string PubErr;
        PackageCatalog::PublishLibrary(rx, &PubErr);
        QVERIFY2(rx.value("ShareSheet", json::object()).empty(),
                 "a CATALOG browse stub is NOT published (LIBRARY-only publish; browse != share)");

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A hostile hydrated block with non-typed fields (PUBLISH:"yes", LIBRARYITEM:5, TITLE:{}) must NOT crash the
    // publish walk. The blocks land in LIBRARY (as a hostile peer's added game would); PublishLibrary + Mint run
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
        QVERIFY2(cfg.contains("ShareSheet"), "publish completed over a hostile block without crashing");
        // The hostile legacy-typed object is not a node: its package has no nodes; only Good publishes.
        std::set<std::string> Pkgs;
        for (const auto & E : PackageCatalog::SheetEntries(cfg["ShareSheet"])) Pkgs.insert(E.Lib + "/" + E.Pkg);
        QCOMPARE(Pkgs, (std::set<std::string>{ "VidyaGod/[2] Good" }));
        IpfsWrapper::StopNode();
    }

    // A NAMELESS node IS shareable: identity is the CID, so a label-less node is an entry like any other, its file
    // named "node.json" (chapter 02 §2.5) — never a synthetic path key. The property that must hold is that NO entry
    // carries a filesystem path beyond its three segments. Teeth: name a nameless node's file by its handle (a path)
    // and the three-segment check fails.
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
        QVERIFY(cfg.contains("ShareSheet"));
        QCOMPARE(PackageCatalog::SheetEntries(cfg["ShareSheet"]).size(), cfg["ShareSheet"].size());   // every path: three segments
        bool sawNamed = false, sawNameless = false;
        for (const auto & E : PackageCatalog::SheetEntries(cfg["ShareSheet"]))
        {
            if (E.Pkg == "[1] Named") sawNamed = true;
            if (E.Pkg == "[2] Nameless") { sawNameless = true; QCOMPARE(E.File, std::string("node.json")); }
        }
        QVERIFY2(sawNamed, "the named game's package publishes");
        QVERIFY2(sawNameless, "the nameless node's package publishes, its file named node.json");
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

    // A node has ONE home package. The same node in a second installed package is a copy — the validator names it, and
    // only its home's engine serves "custom" launches; a peer's catalog stub of an installed package is a view of it,
    // never a home: the node reads from the installed package. Teeth: drop the Duplicates record (no error), or the
    // stub-is-a-view swap (the engine reads as a received stub; CATALOG sorts before LIBRARY).
    void a_node_has_one_home_package()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Root.path() + "/LIBRARY").toStdString()}}}}}};
        const std::filesystem::path Lib = PackageCatalog::LibraryRootDir(cfg), Cat = PackageCatalog::CatalogRootDir(cfg);
        const json Engine = json{{"LABEL", "engine"}, {"LAYERS", json::array({ json{{"EXEC", json::array({
            json{{"LABEL", "engine"}, {"HOST", ManifestModel::MachinePlatform()}, {"GUEST", json::array({"custom"})},
                 {"EXE", "%RunnerMount%/engine"}} })}} })}};
        const std::filesystem::path BG = Lib / "VidyaGod/[1] BG", Stub = Cat / "Alice - Games/[1] BG";
        for (const auto &D : { BG, Stub / ".package" })
        {
            std::filesystem::create_directories(D);
            writeJson(QString::fromStdString((D / "engine.json").string()), Engine);
        }
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(cfg);
        const Node *E = nullptr;
        for (const auto &[C, N] : Idx.Nodes) if (N.NodeId == "engine") E = &N;
        QVERIFY(E);
        QVERIFY2(!E->Received && E->BundleDir == BG, "an installed package beats a peer's stub of it");
        QVERIFY2(Idx.Duplicates.empty(), "a stub of an installed package is a view, not a second home");
        QVERIFY(ManifestModel::ServesPlatform(*E, "custom", BG));
        // A second INSTALLED package holding the same node: one home stays, the copy is an error.
        const std::filesystem::path BG2 = Lib / "VidyaGod/[2] BG2";
        std::filesystem::create_directories(BG2);
        writeJson(QString::fromStdString((BG2 / "engine.json").string()), Engine);
        Idx = PackageCatalog::BuildCatalogIndex(cfg);
        E = nullptr;
        for (const auto &[C, N] : Idx.Nodes) if (N.NodeId == "engine") E = &N;
        QVERIFY(E);
        QCOMPARE((int)Idx.Duplicates.size(), 1);
        const std::filesystem::path Other = E->BundleDir == BG ? BG2 : BG;
        QVERIFY2(!ManifestModel::ServesPlatform(*E, "custom", Other), "a copy's package does not get the engine");
        std::vector<std::string> Errs, W;
        Validate::Nodes(Idx, Errs, W);
        bool Named = false;
        for (const auto &Err : Errs) if (Err.find("one home package") != std::string::npos) Named = true;
        QVERIFY2(Named, "the validator names the copy");
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
        //The runners built into the app are in every index (Windows': windows-native); the library's own nodes are
        //what this is about.
        std::set<std::string> BuiltIn;
        for (const auto & [C, N] : PackageCatalog::BuildCatalogIndex(json{{"Settings", {{"Paths", {{"LibraryRoot", (Root.path() + "/EMPTY").toStdString()}}}}}}).Nodes)
            BuiltIn.insert(C);
        std::set<std::string> Labels;
        for (const auto & [C, N] : Idx.Nodes) if (!BuiltIn.count(C)) Labels.insert(N.NodeId);
        QCOMPARE(Labels, (std::set<std::string>{"good"}));
    }

    // A package row shows our package's pin folder (what a pinning service is given), by its library and folder
    // name; a same-named package in another library is a different package, and a peer's package has none.
    // Teeth: key Pins by the folder name alone, and the VidyaGod row reads proton's pin.
    void package_row_shows_its_pin_folder()
    {
        const json Cfg = json{{"Pins", {{"VidyaGod/[10972][v1.0] Silent Hill 2", "bafyOwnSH2"}, {"VidyaGodRunners/proton", "bafyOwnProton"}}}};
        using PackageCatalog::PackagePin;
        QCOMPARE(PackagePin(Cfg, "/r/LIBRARY/VidyaGod/[10972][v1.0] Silent Hill 2"), std::string("bafyOwnSH2"));
        QCOMPARE(PackagePin(Cfg, "/r/LIBRARY/VidyaGodRunners/proton"), std::string("bafyOwnProton"));
        QCOMPARE(PackagePin(Cfg, "/r/LIBRARY/VidyaGod/proton"), std::string());           // not proton's library
        QCOMPARE(PackagePin(Cfg, "/r/CATALOG/Alice - VidyaGod/[2190][v1.0] Baldur_s Gate"), std::string());
        //A package directly under the library root is keyed in "Library", as publishing keys it. Teeth: key it by the
        //root folder's own name.
        QTemporaryDir Root; QVERIFY(Root.isValid());
        std::filesystem::create_directories(Root.path().toStdString() + "/LIBRARY/[3] Loose");
        const json Cfg2 = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString() + "/LIBRARY"}}}}},
                               {"Pins", {{"Library/[3] Loose", "bafyLoose"}}}};
        QCOMPARE(PackagePin(Cfg2, Root.path().toStdString() + "/LIBRARY/[3] Loose"), std::string("bafyLoose"));
    }

    // Pruning an old generation's node file from one package must not un-seed the same node landed in another (a node
    // two packages share): the reference pointed at the pruned copy; dropping it left the surviving copy on disk and
    // silently not held — never served, never re-shared. Teeth: drop the re-seed from Elsewhere in RemoveLandedNode.
    void pruning_a_stale_copy_keeps_the_same_node_held_from_another_package()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Cat = PackageCatalog::CatalogRootDir(rx) + "/Alice - Games";
        const json X = json{{"LABEL", "shared"}, {"LAYERS", json::array({ json{{"DIR", "s"}} })}};
        const json Y = json{{"LABEL", "only_p1"}, {"LAYERS", json::array({ json{{"DIR", "y"}} })}};
        const std::string XB = Cid::Canonical(X), XC = Cid::OfBytes(XB), YB = Cid::Canonical(Y), YC = Cid::OfBytes(YB);
        auto land = [&](const std::string &Pkg, const std::vector<std::pair<std::string, std::string>> &Files,
                        const std::vector<std::string> &Listed) {
            const std::string D = Cat + "/" + Pkg;
            std::filesystem::create_directories(D);
            for (const auto &[F, B] : Files) { std::ofstream O(D + "/" + F, std::ios::binary); O << B; }
            shareEntries(rx, "Games", Pkg, D, Listed);                                   // the entries name only these
            return D;
        };
        const std::string P1 = land("[1] P1", {{"shared.json", XB}, {"only_p1.json", YB}}, {"only_p1.json"});   // X stale here
        land("[2] P2", {{"shared.json", XB}}, {"shared.json"});                          // …listed here
        QCOMPARE(IpfsWrapper::AddNoCopy(P1 + "/shared.json", &Err), XC);                 // referenced at P1's copy
        const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY(PackageCatalog::PruneStaleReceived(Idx, rx) >= 1);
        QVERIFY2(!std::filesystem::exists(P1 + "/shared.json"), "precondition: the stale copy was pruned");
        QVERIFY2(std::filesystem::exists(P1 + "/only_p1.json"), "what the entries name stays");
        QVERIFY2(IpfsWrapper::HasLocal(XC) && !IpfsWrapper::CidMissing(XC), "the node stays held, from P2's copy");
        IpfsWrapper::StopNode();
    }

    // Pruning older generations from several received packages drops their references in one call, not one per file
    // (each its own commit, unpins and compaction request — hundreds after a peer's re-publish). Teeth: remove each
    // stale file on its own (a drop per file); drop per package (a drop per package).
    void pruning_stale_copies_drops_their_references_together()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::StopNode(); });
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Cat = PackageCatalog::CatalogRootDir(rx) + "/Alice - Games";
        std::vector<std::string> Old;
        for (int I = 1; I <= 2; ++I)
        {
            const std::string Pkg = "[" + std::to_string(I) + "] P" + std::to_string(I), D = Cat + "/" + Pkg;
            std::filesystem::create_directories(D);
            const json Cur = json{{"LABEL", "now" + std::to_string(I)}, {"LAYERS", json::array({ json{{"DIR", "n"}} })}};
            for (int J = 0; J < 3; ++J)
            {
                const json Was = json{{"LABEL", "was" + std::to_string(I * 10 + J)}, {"LAYERS", json::array({ json{{"DIR", "w"}} })}};
                const std::string F = D + "/was" + std::to_string(J) + ".json";
                { std::ofstream O(F, std::ios::binary); O << Cid::Canonical(Was); }
                QCOMPARE(IpfsWrapper::AddNoCopy(F, &Err), Cid::OfNode(Was));
                Old.push_back(Cid::OfNode(Was));
            }
            { std::ofstream O(D + "/now.json", std::ios::binary); O << Cid::Canonical(Cur); }
            shareEntries(rx, "Games", Pkg, D, {"now.json"});
        }
        const long long Before = IpfsWrapper::DebugDropCalls();
        QCOMPARE(PackageCatalog::PruneStaleReceived(PackageCatalog::BuildCatalogIndex(rx), rx), 6);
        QCOMPARE(IpfsWrapper::DebugDropCalls() - Before, 1LL);
        for (const std::string &C : Old) QVERIFY2(!IpfsWrapper::HasLocal(C), C.c_str());
    }

    // The same, when the surviving copy is in an INSTALLED package (adopted into LIBRARY): no peer's entry names it,
    // so the prune finds it through the index (whose node, held in CATALOG and LIBRARY, is the installed one's).
    // Teeth: drop the index's file from Elsewhere in PruneStaleReceived.
    void pruning_a_stale_copy_keeps_the_same_node_held_from_an_installed_package()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const json X = json{{"LABEL", "shared"}, {"LAYERS", json::array({ json{{"DIR", "s"}} })}};
        const json Y = json{{"LABEL", "only_p1"}, {"LAYERS", json::array({ json{{"DIR", "y"}} })}};
        const std::string XB = Cid::Canonical(X), XC = Cid::OfBytes(XB), YB = Cid::Canonical(Y), YC = Cid::OfBytes(YB);
        const std::string P1 = PackageCatalog::CatalogRootDir(rx) + "/Alice - Games/[1] P1";
        std::filesystem::create_directories(P1);
        for (const auto &[F, B] : std::vector<std::pair<std::string, std::string>>{{"shared.json", XB}, {"only_p1.json", YB}})
        { std::ofstream O(P1 + "/" + F, std::ios::binary); O << B; }
        shareEntries(rx, "Games", "[1] P1", P1, {"only_p1.json"});                        // X stale: not listed
        const std::string P3 = RxData.path().toStdString() + "/LIBRARY/Games/[3] P3";
        std::filesystem::create_directories(P3);
        const json Z = json{{"LABEL", "only_p3"}, {"LAYERS", json::array({ json{{"DIR", "z"}} })}};             // its own node
        const std::string ZB = Cid::Canonical(Z), ZC = Cid::OfBytes(ZB);
        for (const auto &[F, B] : std::vector<std::pair<std::string, std::string>>{{"shared.json", XB}, {"only_p3.json", ZB}})   // installed
        { std::ofstream O(P3 + "/" + F, std::ios::binary); O << B; }
        QCOMPARE(IpfsWrapper::AddNoCopy(P1 + "/shared.json", &Err), XC);                 // referenced at P1's copy
        const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY(PackageCatalog::PruneStaleReceived(Idx, rx) >= 1);
        QVERIFY2(!std::filesystem::exists(P1 + "/shared.json"), "precondition: the stale copy was pruned");
        QVERIFY2(IpfsWrapper::HasLocal(XC) && !IpfsWrapper::CidMissing(XC), "the node stays held, from the installed copy");
        IpfsWrapper::StopNode();
    }

    // What a download adopts is every received package its closures reach — not just the packages of the nodes it
    // names. A runner's own NODE layers reach other packages (proton contains proton-wine's chain); left in CATALOG,
    // that package is never ours (never re-shared: a peer of ours would get proton without its wine), and on the
    // replication run it was exactly that. A package nothing reaches stays a stub. Teeth: take only the named nodes
    // (not their Closure) in PackagesToAdopt and the contained packages drop out.
    // Installing what a download reaches moves every received package into the library with its references, in ONE
    // scan of the references (each scan reads the whole index); a package that cannot be installed refuses them all
    // before anything moves. Teeth: a scan per package; move the packages before judging them all (the first moves,
    // then the collision refuses); leave a package's references behind.
    void installing_received_packages_moves_them_and_their_references_in_one_scan()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Data; QVERIFY(Data.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Data.path() + "/LIBRARY").toStdString()}}}}},
                        {"PeerLibraries", {{"12D3KooWAlice", {{"Games", json::array()}, {"Runners", json::array()}}},
                                           {"12D3KooWBob", {{"Games", json::array()}}}}}};
        const std::string Cat = PackageCatalog::CatalogRootDir(cfg);
        const auto stub = [&](const std::string &Peer, const std::string &Lib, const std::string &Pkg, std::string *C) {
            const std::filesystem::path D = std::filesystem::path(Cat) / (PackageCatalog::ReceivedNickLabel(cfg, Peer) + " - " + Lib) / Pkg;
            std::filesystem::create_directories(D);
            { std::ofstream(D / "content.bin", std::ios::binary) << std::string(300000, Pkg[1]) << Peer; }
            *C = IpfsWrapper::AddNoCopy((D / "content.bin").string(), &Err);          // fetched: referenced in place
            return D; };
        std::vector<std::filesystem::path> Dirs;
        std::vector<std::string> Cids(3);
        Dirs.push_back(stub("12D3KooWAlice", "Games", "[1] One", &Cids[0]));
        Dirs.push_back(stub("12D3KooWAlice", "Games", "[2] Two", &Cids[1]));
        Dirs.push_back(stub("12D3KooWAlice", "Games", "[3] Three", &Cids[2]));
        for (const auto &C : Cids) QVERIFY2(!C.empty(), Err.c_str());
        // a collision with the library refuses every package, and moves none
        const std::filesystem::path Lib = std::filesystem::path(PackageCatalog::LibraryRootDir(cfg)) / "Games";
        std::filesystem::create_directories(Lib / "[3] Three");
        QVERIFY(!PackageCatalog::AdoptReceivedPackages(cfg, Dirs, nullptr, &Err));
        for (const auto &D : Dirs) QVERIFY2(std::filesystem::is_directory(D), "a package moved although another was refused");
        std::filesystem::remove_all(Lib / "[3] Three");
        // one package from two peers would install as one: refused, nothing moved (never merged, never half-installed)
        std::string BobCid;
        const auto Bobs = stub("12D3KooWBob", "Games", "[1] One", &BobCid);
        QVERIFY(!PackageCatalog::AdoptReceivedPackages(cfg, { Dirs[0], Bobs }, nullptr, &Err));
        QVERIFY2(std::filesystem::is_directory(Dirs[0]) && std::filesystem::is_directory(Bobs), "a package moved although two would install as one");
        QVERIFY(!std::filesystem::exists(Lib / "[1] One"));
        // all at once (one asked for twice): one scan of the references
        const long long Scans = IpfsWrapper::DebugMoveRefScans();
        std::vector<std::filesystem::path> New;
        QVERIFY2(PackageCatalog::AdoptReceivedPackages(cfg, { Dirs[0], Dirs[1], Dirs[2], Dirs[1] }, &New, &Err), Err.c_str());
        QCOMPARE(IpfsWrapper::DebugMoveRefScans() - Scans, 1LL);
        QCOMPARE(New.size(), size_t(3));
        for (size_t I = 0; I < New.size(); ++I)
        {
            QVERIFY(std::filesystem::exists(New[I] / "content.bin"));
            QVERIFY(!std::filesystem::exists(Dirs[I]));
            QVERIFY2(!IpfsWrapper::CidMissing(Cids[I]), "a package's references stayed behind");
        }
        // one that cannot move (its library is read-only) goes back with its references; the others are installed
        std::string RunCid, GameCid;
        const auto Runner = stub("12D3KooWAlice", "Runners", "proton", &RunCid);
        const auto Game = stub("12D3KooWAlice", "Games", "[4] Four", &GameCid);
        const std::filesystem::path RO = std::filesystem::path(PackageCatalog::LibraryRootDir(cfg)) / "Runners";
        std::filesystem::create_directories(RO);
        std::filesystem::permissions(RO, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove);
        const bool Ok = PackageCatalog::AdoptReceivedPackages(cfg, { Runner, Game }, nullptr, &Err);
        std::filesystem::permissions(RO, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        QVERIFY(!Ok);
        QVERIFY2(std::filesystem::exists(Runner / "content.bin"), "the package that could not move is gone from where it was");
        QVERIFY2(!IpfsWrapper::CidMissing(RunCid), "its references did not come back");
        QVERIFY2(std::filesystem::exists(Lib / "[4] Four" / "content.bin") && !IpfsWrapper::CidMissing(GameCid), "the other package was not installed");
        IpfsWrapper::StopNode();
    }

    // Pruning a received library reads only the files no entry names: the named ones (thousands) are kept without a
    // read — reading every one to skip it froze a handheld's GUI for seconds after each landing. Teeth: read every file.
    void pruning_received_packages_reads_only_what_no_entry_names()
    {
        std::ifstream Io("/proc/self/io");
        if (!Io) QSKIP("reads counted from /proc");
        const auto ReadSoFar = [] {
            std::ifstream I("/proc/self/io");
            for (std::string L; std::getline(I, L);) if (L.rfind("rchar: ", 0) == 0) return std::stoll(L.substr(7));
            return 0LL;
        };
        QTemporaryDir Data; QVERIFY(Data.isValid());
        json Items = json::array();
        const std::string Pad(50000, 'x');
        std::vector<json> Nodes;
        for (int I = 0; I < 400; ++I)
        {
            const json N = json{{"LABEL", "n" + std::to_string(I)}, {"NOTE", Pad}, {"LAYERS", json::array({ json{{"DIR", "d"}} })}};
            Items.push_back(json{{"cid", Cid::OfNode(N)}, {"pkg", "Game " + std::to_string(I % 20)}, {"node", "n" + std::to_string(I) + ".json"}});
            Nodes.push_back(N);
        }
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Data.path() + "/LIBRARY").toStdString()}}}}},
                        {"PeerLibraries", {{"12D3KooWAlice", {{"Games", Items}}}}}};
        const auto Plan = PackageCatalog::PlanReceivedFetches(cfg, PackageCatalog::ReceivedNickLabel(cfg, "12D3KooWAlice"), cfg["PeerLibraries"]["12D3KooWAlice"]);
        QCOMPARE(Plan.size(), Nodes.size());
        long long Named = 0;
        for (const auto &T : Plan)
            for (const json &N : Nodes)
                if (Cid::OfNode(N) == T.Cid)
                {
                    std::filesystem::create_directories(std::filesystem::path(T.Dest).parent_path());
                    std::ofstream(T.Dest, std::ios::binary) << Cid::Canonical(N);
                    Named += static_cast<long long>(Cid::Canonical(N).size());
                }
        const json Stale = json{{"LABEL", "old"}, {"LAYERS", json::array({ json{{"DIR", "o"}} })}};   // an older generation's
        const std::filesystem::path StalePath = std::filesystem::path(Plan.front().Dest).parent_path() / "old.json";
        std::ofstream(StalePath, std::ios::binary) << Cid::Canonical(Stale);
        const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(cfg);
        const long long Before = ReadSoFar();
        QCOMPARE(PackageCatalog::PruneStaleReceived(Idx, cfg), 1);
        const long long Read = ReadSoFar() - Before;
        QVERIFY2(!std::filesystem::exists(StalePath), "the stale file stays");
        QVERIFY2(Read < Named / 10, qPrintable(QString("pruning read %1 KB of %2 KB named").arg(Read / 1024).arg(Named / 1024)));
    }

    void download_adopts_every_received_package_its_closures_reach()
    {
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        auto receive = [&](const std::string &Lib, const std::string &Pkg, const std::vector<json> &Nodes) {
            const std::string Dir = Catalog + "/Alice - " + Lib + "/" + Pkg;
            std::filesystem::create_directories(Dir);
            std::vector<std::string> Cids;
            for (const json &N : Nodes)
            {
                const std::string Bytes = Cid::Canonical(N);
                const std::string C = Cid::OfBytes(Bytes);
                { std::ofstream O(Dir + "/" + N.value("LABEL", std::string()) + ".json", std::ios::binary); O << Bytes; }
                Cids.push_back(C);
            }
            return Cids;
        };
        const auto Wine = receive("Runners", "proton-wine", {json{{"LABEL", "wine_11"}, {"LAYERS", json::array({
            NodeFixture::ContentCid("zip", "wine.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku")})}}});
        receive("Runners", "proton", {json{{"LABEL", "geproton_11"}, {"LAYERS", json::array({json{{"NODE", Wine[0]}},
            json{{"EXEC", json::array({json{{"LABEL", "run"}, {"HOST", "linux"}, {"GUEST", "win64"}, {"EXE", "proton"}}})}}})}}});
        const auto Lib = receive("Libraries", "dplay", {json{{"LABEL", "dplay_lib"}, {"LAYERS", json::array({
            NodeFixture::ContentCid("zip", "dplay.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku")})}}});
        receive("Games", "[7] G", {json{{"LABEL", "g_exec"}, {"VARIANT", "Play"}, {"LAYERS", json::array({json{{"NODE", Lib[0]}},
            json{{"EXEC", json::array({json{{"LABEL", "Play"}, {"HOST", "linux"}, {"EXE", "g"}, {"TILE", {{"UID", "7"}, {"TITLE", "G"}}}}})}}})}}});
        receive("Games", "[8] Untouched", {json{{"LABEL", "u_exec"}, {"VARIANT", "Play"}, {"LAYERS", json::array({
            json{{"EXEC", json::array({json{{"LABEL", "Play"}, {"HOST", "linux"}, {"EXE", "u"}, {"TILE", {{"UID", "8"}, {"TITLE", "U"}}}}})}}})}}});

        const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY2(Idx.Find("geproton_11") && Idx.Find("geproton_11")->Received, "precondition: the runner is received");
        const auto Adopt = PackageCatalog::PackagesToAdopt(Idx, {"g_exec"}, {"geproton_11"}, rx);
        auto has = [&](const std::string &Rel) { return Adopt.count(std::filesystem::path(Catalog) / Rel) > 0; };
        QVERIFY2(has("Alice - Runners/proton"), "the ticked runner's own package");
        QVERIFY2(has("Alice - Runners/proton-wine"), "the package the runner CONTAINS (its NODE layer) — adopted too");
        QVERIFY2(has("Alice - Games/[7] G"), "the launchable's own package");
        QVERIFY2(has("Alice - Libraries/dplay"), "the library package the launchable contains");
        QVERIFY2(!has("Alice - Games/[8] Untouched"), "a package no closure reaches stays a stub");
        QCOMPARE((int)Adopt.size(), 4);
    }

    // A landing pass fetches every received package's missing files together: one package's slow file no longer holds
    // up the next package's (each was enqueued and waited for in turn). A file that does not land fails its own package,
    // and only its own. Teeth: enqueue and wait per package (B lands only after A's file); make the files required (A's
    // failure ends the wait and B is never looked at, or B fails with it).
    void received_packages_land_together_and_fail_on_their_own()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::SetFetchOnceHook({}); IpfsWrapper::DebugResetQueue(); IpfsWrapper::StopNode(); });
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"PeerLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};
        const std::string Nick = "derAlice";
        landShares(rx, Nick, json{{"Games", Items}});
        const std::filesystem::path Lib = std::filesystem::path(PackageCatalog::CatalogRootDir(rx)) / (Nick + " - Games");
        const std::filesystem::path A = Lib / "[1] A" / "a_exec.json", B = Lib / "[2] B" / "b_exec.json";
        const auto Read = [](const std::filesystem::path &P) { std::ifstream In(P, std::ios::binary); return std::string(std::istreambuf_iterator<char>(In), {}); };
        const std::string ABytes = Read(A), BBytes = Read(B);
        QVERIFY(!ABytes.empty() && !BBytes.empty());
        std::atomic<bool> BLanded{false}, ASawB{false}, FailA{false};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &, const std::string &Dest, bool, std::string *E) {
            if (Dest == A.string())
            {
                if (FailA) { if (E) *E = "nobody has it"; return 2; }
                for (int I = 0; I < 300 && !BLanded; ++I) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ASawB = BLanded.load();
                std::ofstream(Dest, std::ios::binary) << ABytes;
                return 0;
            }
            if (Dest == B.string()) { std::ofstream(Dest, std::ios::binary) << BBytes; BLanded = true; return 0; }
            return 2; });
        std::filesystem::remove(A);
        std::filesystem::remove(B);
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &Err), Err.c_str());
        QVERIFY2(ASawB, "B's file landed while A's was still on its way");
        // A's file cannot be had: A's package fails, B's lands.
        std::filesystem::remove(A);
        std::filesystem::remove(B);
        FailA = true;
        Err.clear();
        QVERIFY(!PackageCatalog::LandReceivedPackages(rx, &Err));
        QVERIFY2(Err.find("[1] A") != std::string::npos, Err.c_str());
        QVERIFY2(std::filesystem::exists(B), "the other package's file landed");
        QCOMPARE(Read(B), BBytes);
    }

    // A package hydrated from its launchable's CID fetches its content files together: file after file, each waited
    // for in turn, took their fetch times added up. One that cannot be had leaves the package incomplete (resumable),
    // and the others land. Teeth: wait per file (the second lands only after the first); make them required (the
    // failed one ends the wait before the other lands).
    void a_hydrated_package_fetches_its_content_together()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::SetFetchOnceHook({}); IpfsWrapper::DebugResetQueue(); IpfsWrapper::StopNode(); });
        QTemporaryDir Seed; QVERIFY(Seed.isValid());
        const std::string One = Cid::OfBytes("the first content file"), Two = Cid::OfBytes("the second content file");
        const QString Pkg = Seed.path() + "/VidyaGod/[9] H";
        QDir().mkpath(Pkg);
        writeJson(Pkg + "/c1.json", NodeFixture::Chain("h_c1", {NodeFixture::ContentCid("file", "one.bin", One)}));
        writeJson(Pkg + "/c2.json", NodeFixture::Chain("h_c2", {NodeFixture::ContentCid("file", "two.bin", Two)}));
        writeJson(Pkg + "/h.json", NodeFixture::Chain("h_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "h.exe"),
                  NodeFixture::Variant("Play"), NodeFixture::Tile("9", "H")})}, {"h_c1", "h_c2"}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", Seed.path().toStdString()}}}}}};
        QVERIFY2(!PackageCatalog::PublishLibrary(seeder, &Err).empty(), Err.c_str());
        const json Wired = PackageCatalog::Wire(PackageCatalog::SheetEntries(seeder.value("ShareSheet", json::object())));
        const std::string Launch = cidOfLabel(Wired["VidyaGod"], "h_exec");
        QVERIFY(!Launch.empty());
        std::atomic<bool> TwoLanded{false}, OneSawTwo{false}, FailOne{false};
        IpfsWrapper::SetFetchOnceHook([&](const std::string &cid, const std::string &dest, bool, std::string *E) {
            if (cid == One)
            {
                if (FailOne) { if (E) *E = "nobody has it"; return 2; }
                for (int I = 0; I < 300 && !TwoLanded; ++I) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                OneSawTwo = TwoLanded.load();
            }
            std::ofstream(dest) << cid;
            if (cid == Two) TwoLanded = true;
            return 0; });
        QTemporaryDir Dest; QVERIFY(Dest.isValid());
        const std::string Dir = NodeStore::HydratePackage(Dest.path().toStdString(), Launch, &Err);
        QVERIFY2(!Dir.empty(), Err.c_str());
        QVERIFY2(OneSawTwo, "the second file landed while the first was on its way");
        // Again, the first cannot be had: incomplete, the other landed.
        QTemporaryDir Dest2; QVERIFY(Dest2.isValid());
        IpfsWrapper::DebugResetQueue();
        FailOne = true;
        TwoLanded = false;
        QVERIFY(NodeStore::HydratePackage(Dest2.path().toStdString(), Launch, &Err).empty());
        QVERIFY2(TwoLanded, "the other file landed");
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
        QCOMPARE((int)Items.size(), 7);
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"PeerLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};   // nick = peer tail "derAlice"
        const std::string Nick = "derAlice";
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        landShares(rx, Nick, json{{"Games", Items}});
        const std::filesystem::path Stub = std::filesystem::path(Catalog) / (Nick + " - Games") / "[1] A";
        QVERIFY(std::filesystem::is_regular_file(Stub / "a_exec.json"));
        const std::string ACid = cidOfLabel(Items, "a_exec");
        // A node file missing from a received package is fetched ON ITS OWN by LandReceivedPackages — a file
        // referenced in place, so installing has a reference under the stub to carry. (One node plays both machines:
        // the fetch moved the block into the landed file, so the seeder's copy is put back, as the seeder holds it.)
        std::string AExec;
        {
            std::ifstream In(Stub / "a_exec.json", std::ios::binary);
            AExec.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
        }
        std::filesystem::remove(Stub / "a_exec.json");
        QCOMPARE(IpfsWrapper::BlockPut(AExec, &Err), ACid);
        const long long Holds = IpfsWrapper::DebugNetHolds();
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &Err), Err.c_str());
        QVERIFY2(IpfsWrapper::DebugNetHolds() > Holds, "LandReceivedPackages fetched package after package without holding the foreground");
        QVERIFY2(IpfsWrapper::HasLocal(ACid) && !IpfsWrapper::CidMissing(ACid), "precondition: the landed node is held, referenced in place");

        std::filesystem::path NewDir;
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Stub, &NewDir, &Err), Err.c_str());
        QCOMPARE(NewDir, std::filesystem::path(RxData.path().toStdString()) / "LIBRARY" / "Games" / "[1] A");
        QVERIFY2(!std::filesystem::exists(Stub), "the stub dir is gone from CATALOG");
        QVERIFY2(std::filesystem::exists(NewDir / "a_exec.json"), "its node files are the package's own now");
        // The references moved with the files: the installed node still reads (and so can be served / re-shared).
        // Teeth: drop the stub→installed MoveRefs in AdoptReceivedPackage and the node reads as missing here.
        QVERIFY2(!IpfsWrapper::CidMissing(ACid), "the installed node's reference points at its new place");
        // A download still reading the old index asks for the old place: it lands in the installed package, never
        // re-creating the stub. Teeth: drop the RedirectDestsUnder calls in AdoptReceivedPackage.
        IpfsWrapper::EnqueueBatch({{"CID_LATE_AFTER_ADOPT", (Stub / "late.json").string(), /*Optional=*/true}});
        QCOMPARE(std::filesystem::path(IpfsWrapper::QueueDestForCid("CID_LATE_AFTER_ADOPT")), NewDir / "late.json");
        // Another download sharing this package (a runner every game reaches) adopts it again from its older snapshot:
        // already installed — success, the library's copy — not a failure that sinks that whole download.
        std::filesystem::path Again;
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Stub, &Again, &Err), Err.c_str());
        QCOMPARE(Again, NewDir);
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        const Node * A = Idx.Find("a_exec");
        QVERIFY(A);
        QVERIFY2(!A->Received, "an installed package is ours: never 'Received' again");
        QCOMPARE(A->BundleDir, NewDir);
        QVERIFY2(Idx.Find("b_exec") && Idx.Find("b_exec")->Received, "the un-installed package stays a stub");
        // The planner skips the adopted package (the library holds it) and still plans the other.
        const auto Plan2 = PackageCatalog::PlanReceivedFetches(rx, Nick, json{{"Games", Items}});
        QCOMPARE((int)Plan2.size(), 2);
        for (const auto & T : Plan2) QVERIFY2(T.Dest.find("[2] B") != std::string::npos, T.Dest.c_str());
        // A second adopt of a package whose name the library already holds is refused: never merged, never a second
        // copy of its nodes. Teeth: drop the refusal and the stub's nodes land beside the installed ones.
        std::filesystem::create_directories(Stub);
        QVERIFY(!PackageCatalog::AdoptReceivedPackage(rx, Stub, nullptr, &Err));
        QVERIFY(Err.find("already exists") != std::string::npos);
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // Switching an installed package to a peer's version. A library L (node libA, its dx.zip) and a game G that
    // contains libA are installed. The peer's version of L has libB (a NEW dx.zip under the same name) and libC (the
    // OLD bytes, renamed extra.zip). Switching L: libA stays (G still contains it), the two dx.zip versions live at
    // bumped names (the installed bytes move, their reference with them), G stays complete. Then G is switched to the
    // peer's version (it contains libB) and L to one with libB and libC: now libA goes, and so does its file.
    // Teeth: drop the containment walk (libA goes, G breaks); drop the bumped-name rule (both versions read one
    // dx.zip); drop MoveRefs on rename (an orphaned reference); keep Removed nodes/Deletes (they stay); offer the
    // version already installed, or one already in beside kept nodes; drop Elsewhere on a delete (X1 unreferenced);
    // apply the confirmed plan unchecked; ignore the journal; drop the versions section from the page (no button).
    void an_installed_package_switches_to_a_peers_version()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const QString R = SeedRoot.path(), Ld = R + "/Libs/[L] DxWnd", Gd = R + "/Games/[1] G";
        QDir().mkpath(Ld); QDir().mkpath(Gd);
        writeFile(Ld + "/dx.zip", std::string(3000, '1'));
        const std::string X1 = IpfsWrapper::AddNoCopy((Ld + "/dx.zip").toStdString(), &Err);
        QVERIFY2(!X1.empty(), Err.c_str());
        writeJson(Ld + "/a.json", NodeFixture::Chain("libA", {NodeFixture::ContentCid("zip", "dx.zip", X1)}));
        writeJson(Gd + "/tile.json", NodeFixture::Chain("g_tile", {NodeFixture::Tile("1", "G")}));
        writeJson(Gd + "/g.json", NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"libA", "g_tile"}));
        json seeder = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        auto Publish = [&]{ Err.clear(); const bool Ok = !PackageCatalog::PublishLibrary(seeder, &Err).empty(); if (!Ok) qWarning("publish: %s", Err.c_str());
                            return PackageCatalog::Wire(PackageCatalog::SheetEntries(seeder["ShareSheet"])); };
        const json Gen1 = Publish();
        const std::string LibA = cidOfLabel(Gen1["Libs"], "libA");

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        const std::string Peer = "12D3KooWSeederAlice", Nick = "derAlice";
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"PeerLibraries", {{Peer, Gen1}}}};
        landShares(rx, Nick, Gen1);
        const std::filesystem::path Cat = PackageCatalog::CatalogRootDir(rx);
        std::filesystem::path Lib, Game;
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Cat / (Nick + " - Libs") / "[L] DxWnd", &Lib, &Err), Err.c_str());
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Cat / (Nick + " - Games") / "[1] G", &Game, &Err), Err.c_str());
        writeFile(QString::fromStdString((Lib / "dx.zip").string()), std::string(3000, '1'));   // hydrated here
        QCOMPARE(IpfsWrapper::AddNoCopy((Lib / "dx.zip").string(), &Err), X1);
        QVERIFY2(PackageCatalog::AvailableUpdates(rx).empty(), "the peer's versions are the installed ones");

        // The peer's second version of L: dx.zip is new bytes (libB); the old bytes are extra.zip (libC). G contains libB.
        QFile::remove(Ld + "/a.json");
        // (One node plays both machines: the peer rewrites dx.zip in place, so X1 is held from the receiver's copy.)
        QVERIFY(IpfsWrapper::DropRef(X1));
        QCOMPARE(IpfsWrapper::AddNoCopy((Lib / "dx.zip").string(), &Err), X1);
        writeFile(Ld + "/dx.zip", std::string(3000, '2'));
        const std::string X2 = IpfsWrapper::AddNoCopy((Ld + "/dx.zip").toStdString(), &Err);
        writeFile(Ld + "/extra.zip", std::string(3000, '1'));                   // X1's bytes, already held here
        writeJson(Ld + "/b.json", NodeFixture::Chain("libB", {NodeFixture::ContentCid("zip", "dx.zip", X2)}));
        writeJson(Ld + "/c.json", NodeFixture::Chain("libC", {NodeFixture::ContentCid("zip", "extra.zip", X1)}));
        writeJson(Gd + "/g.json", NodeFixture::Chain("g_exec", {NodeFixture::Merge({NodeFixture::Exec("win32", "g.exe"), NodeFixture::Variant("Play")})}, {"libB", "g_tile"}));
        const json Gen2 = Publish();
        QVERIFY2(Gen2["Libs"] != Gen1["Libs"], Gen2.dump().c_str());
        rx["PeerLibraries"] = json{{Peer, Gen2}};
        QCOMPARE((int)PackageCatalog::AvailableUpdates(rx).size(), 2);          // both installed packages have another version
        //Asked again with nothing changed, the installed packages' node files are not read again (every refresh of the
        //packages page read them all). Teeth: drop the memo in PackageNodeCids; keep it whatever the files (below,
        //after a switch, the package would still read as the old version).
        {
            const long long Reads = PackageCatalog::DebugNodeCidReads();
            QCOMPARE((int)PackageCatalog::AvailableUpdates(rx).size(), 2);
            QCOMPARE(PackageCatalog::DebugNodeCidReads() - Reads, 0LL);
        }
        {   // Settings → Installed Packages offers it, by the peer's name
            QDir appDir(RxData.path());
            AppModel model(&rx, &appDir);
            PackagesView view(model);
            view.show();
            QPushButton * Offer = nullptr;
            QTRY_VERIFY_WITH_TIMEOUT([&]{ for (auto * B : view.findChildren<QPushButton *>()) if (B->text() == "Use derAlice's version") Offer = B;
                                          return Offer != nullptr; }(), 20000);
        }
        auto Switch = [&](const std::filesystem::path &Dir, PackageCatalog::StagedUpdate &S) {
            for (const auto & U : PackageCatalog::AvailableUpdates(rx))
                if (U.Dir == Dir) return PackageCatalog::StageUpdate(rx, U, &S, &Err) && PackageCatalog::ApplyStagedUpdate(rx, S, &Err);
            Err = "no other version offered for " + Dir.string();
            return false;
        };
        PackageCatalog::StagedUpdate S1;
        {   // A switch refused part way (a node file cannot move in) keeps the references of what it moved: the next
            // attempt skips a node file already in, so its reference would point into the stage for good. Teeth:
            // refuse there without moving the references first.
            for (const auto & U : PackageCatalog::AvailableUpdates(rx))
                if (U.Dir == Lib) QVERIFY2(PackageCatalog::StageUpdate(rx, U, &S1, &Err), Err.c_str());
            std::vector<std::string> Incoming;
            for (const std::string & C : S1.Listing)
                if (!std::filesystem::exists(Lib / (C + ".json"))) Incoming.push_back(C);
            QVERIFY2(Incoming.size() >= 2, "two node files move in");
            const std::filesystem::path Block = Lib / (Incoming[1] + ".json");
            std::filesystem::create_directories(Block / "x");                          // neither renamed nor copied over
            QVERIFY2(!PackageCatalog::ApplyStagedUpdate(rx, S1, &Err) && Err.find("cannot move") != std::string::npos, Err.c_str());
            QVERIFY2(std::filesystem::exists(Lib / (Incoming[0] + ".json")), "the first node file moved in");
            for (const auto & P : IpfsWrapper::OrphanedRefPaths())
                QVERIFY2(P.find(S1.Stage.string()) == std::string::npos, P.c_str());   // its reference followed it
            std::filesystem::remove_all(Block);
        }
        const long long SwitchScans = IpfsWrapper::DebugMoveRefScans();
        QVERIFY2(PackageCatalog::ApplyStagedUpdate(rx, S1, &Err), Err.c_str());
        //A switch moves references at most twice — the content's (when content is deleted between) and the node
        //files' with their names: a move each was a scan of every reference each. Teeth: move the staged nodes' and
        //their names' references in calls of their own (three).
        QVERIFY2(IpfsWrapper::DebugMoveRefScans() - SwitchScans <= 2, std::to_string(IpfsWrapper::DebugMoveRefScans() - SwitchScans).c_str());
        QVERIFY2(std::find(S1.Kept.begin(), S1.Kept.end(), LibA) != S1.Kept.end(), "libA stays: G still contains it");
        QVERIFY2(std::filesystem::exists(Lib / "libA.json"), "libA's node file stays");
        const std::filesystem::path Old = Lib / ContentFile::Bumped("dx.zip", X1);
        QVERIFY2(std::filesystem::exists(Old) && !std::filesystem::exists(Lib / "dx.zip"), "the installed bytes moved to their bumped name");
        for (const auto & P : IpfsWrapper::OrphanedRefPaths())
            QVERIFY2(P.find(RxData.path().toStdString()) == std::string::npos, P.c_str());       // the reference followed
        {
            const NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
            QVERIFY2(PackageCatalog::MissingFromClosure(Idx, "g_exec").empty(), "G is still complete");
            std::filesystem::path Local; std::string C;
            ManifestModel::LayerLocator(Idx.Find(LibA)->LoweredLayers()[0], Lib, Local, C);
            QCOMPARE(Local, Old);                                                  // libA reads its own bytes
            const std::string LibB = cidOfLabel(Gen2["Libs"], "libB");
            ManifestModel::LayerLocator(Idx.Find(LibB)->LoweredLayers()[0], Lib, Local, C);
            QCOMPARE(Local, Lib / ContentFile::Bumped("dx.zip", X2));             // libB never reads libA's
        }
        {   // L is the peer's version now, plus libA kept for G: offered no more (G still is)
            const auto Offered = PackageCatalog::AvailableUpdates(rx);
            QCOMPARE((int)Offered.size(), 1);
            QCOMPARE(Offered[0].Dir, Game);
        }

        // G switches to the peer's version (it contains libB): libA may go now, so L is offered again.
        PackageCatalog::StagedUpdate S2, S3;
        QVERIFY2(Switch(Game, S2), Err.c_str());
        {
            const auto Offered = PackageCatalog::AvailableUpdates(rx);
            QVERIFY2(Offered.size() == 1 && Offered[0].Dir == Lib, "libA is contained by nothing now");
        }
        // The peer's next L keeps libC (extra.zip: X1's bytes, hydrated here) — libA's copy of X1 goes, and X1 stays
        // referenced at extra.zip.
        writeFile(QString::fromStdString((Lib / "extra.zip").string()), std::string(3000, '1'));
        rx["PeerLibraries"] = json{{Peer, Publish()}};
        // (One node plays both machines: X1 is held at the receiver's old copy alone, as it would be there.)
        QVERIFY(IpfsWrapper::DropRef(X1));
        QCOMPARE(IpfsWrapper::AddNoCopy(Old.string(), &Err), X1);
        PackageCatalog::PackageUpdate U3;
        for (const auto & U : PackageCatalog::AvailableUpdates(rx)) if (U.Dir == Lib) U3 = U;
        QVERIFY(!U3.Version.empty() && !U3.Listing.empty());
        QVERIFY2(PackageCatalog::StageUpdate(rx, U3, &S3, &Err), Err.c_str());
        {   // Never under a running game (held by every launch, on every platform). Teeth: drop the check.
            const Sessions::RuntimeHold Running;
            QVERIFY2(!PackageCatalog::ApplyStagedUpdate(rx, S3, &Err) && Err.find("running") != std::string::npos, Err.c_str());
            QVERIFY2(std::filesystem::exists(Old), "nothing changed");
        }
        {   // Applying plans again: a delete the user was not shown (the library changed meanwhile) is refused.
            PackageCatalog::StagedUpdate Shown = S3;
            Shown.Deletes.clear();
            QVERIFY2(!PackageCatalog::ApplyStagedUpdate(rx, Shown, &Err) && Err.find("changed since") != std::string::npos, Err.c_str());
            QVERIFY2(std::filesystem::exists(Old), "nothing changed");
        }
        {   // A switch that stopped half-way resumes as planned: its journal, in the stage, is staged and applied again.
            writeFile(QString::fromStdString((Lib / "junk.bin").string()), "half-switched");
            const std::string Junk = IpfsWrapper::AddNoCopy((Lib / "junk.bin").string(), &Err);
            json J = {{"listing", S3.Listing}, {"added", S3.Added}, {"removed", S3.Removed}, {"kept", S3.Kept},
                      {"moves", json::array()}, {"deletes", json::array()}, {"bytes", 0}};
            for (const auto & [F, T] : S3.Moves) J["moves"].push_back({F.string(), T.string()});
            for (const auto & D : S3.Deletes)
            {
                json E = json::array();
                for (const auto & P : D.Elsewhere) E.push_back(P.string());
                J["deletes"].push_back({{"file", D.File.string()}, {"cid", D.Cid}, {"elsewhere", E}});
            }
            J["deletes"].push_back({{"file", (Lib / "junk.bin").string()}, {"cid", Junk}, {"elsewhere", json::array()}});
            writeJson(QString::fromStdString((S3.Stage / "switch.json").string()), J);
            PackageCatalog::StagedUpdate Resumed;
            QVERIFY2(PackageCatalog::StageUpdate(rx, U3, &Resumed, &Err), Err.c_str());
            QCOMPARE(Resumed.Deletes.size(), S3.Deletes.size() + 1);
            QVERIFY2(PackageCatalog::ApplyStagedUpdate(rx, Resumed, &Err), Err.c_str());
            QVERIFY2(!std::filesystem::exists(Lib / "junk.bin"), "the journaled plan was applied");
        }
        QVERIFY2(std::find(S3.Removed.begin(), S3.Removed.end(), LibA) != S3.Removed.end(), "libA goes: nothing contains it");
        QVERIFY(!std::filesystem::exists(Lib / "libA.json"));
        QVERIFY2(std::filesystem::exists(Lib / "libB.json") && std::filesystem::exists(Lib / "libC.json"),
                 "the version's node files are named by their labels in the package");
        QVERIFY2(!std::filesystem::exists(Old), "its file goes with it");
        QVERIFY2(IpfsWrapper::HasLocal(X1) && !IpfsWrapper::CidMissing(X1), "X1 is still held: referenced at extra.zip");
        QVERIFY2(PackageCatalog::AvailableUpdates(rx).empty(), "every installed package is the peer's version now");
        // A package holding a never-saved (draft) node file is not switched: the switch could not judge the draft.
        // Teeth: drop the HasDraftNodes refusal.
        writeFile(Ld + "/z.json", NodeFixture::Chain("libZ", {NodeFixture::Content("zip", "z.zip")}).back().dump());
        rx["PeerLibraries"] = json{{Peer, Publish()}};
        writeJson(QString::fromStdString((Lib / "draft.json").string()), json{{"LABEL", "draft"}, {"LAYERS", json::array()}});
        PackageCatalog::StagedUpdate S4;
        QVERIFY2(!Switch(Lib, S4) && Err.find("drafts") != std::string::npos, Err.c_str());
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // Installing moves the references BEFORE the package. An install whose move fails leaves the package in CATALOG,
    // still Received, its files referenced where they are, so the next download installs it. (Moved after the rename,
    // a failure left an installed package, never adopted again, whose files read as missing for good.) Teeth: drop
    // the point-back on failure and the node reads as missing.
    void a_failed_install_is_retried_by_the_next_download()
    {
        namespace fs = std::filesystem;
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"PeerLibraries", {{"12D3KooWSeederAlice", {{"Games", Items}}}}}};
        const std::string Nick = "derAlice";
        landShares(rx, Nick, json{{"Games", Items}});
        const fs::path Stub = fs::path(PackageCatalog::CatalogRootDir(rx)) / (Nick + " - Games") / "[1] A";
        const std::string ACid = cidOfLabel(Items, "a_exec");
        std::string AExec;
        {
            std::ifstream In(Stub / "a_exec.json", std::ios::binary);
            AExec.assign(std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>());
        }
        fs::remove(Stub / "a_exec.json");                                 // fetched on its own: referenced in place
        QCOMPARE(IpfsWrapper::BlockPut(AExec, &Err), ACid);               // (the seeder's copy: one node plays both)
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &Err), Err.c_str());
        QVERIFY2(IpfsWrapper::HasLocal(ACid) && !IpfsWrapper::CidMissing(ACid), "precondition: the landed node is referenced in place");

        const fs::path Lib = fs::path(RxData.path().toStdString()) / "LIBRARY" / "Games";
        fs::create_directories(Lib.parent_path());
        { std::ofstream Block(Lib); Block << "a file where the library folder goes"; }   // the package cannot move in
        //(A FILE, not a read-only folder: on Windows a folder's read-only bit does not stop a move into it.)
        QVERIFY(!PackageCatalog::AdoptReceivedPackage(rx, Stub, nullptr, &Err));
        QVERIFY2(!fs::exists(Lib / "[1] A"), "nothing installed");
        QVERIFY2(fs::exists(Stub / "a_exec.json"), "the package is still in CATALOG");
        QVERIFY2(!IpfsWrapper::CidMissing(ACid), "its files are referenced where they are");
        NodeIndex Idx = PackageCatalog::BuildCatalogIndex(rx);
        QVERIFY2(PackageCatalog::PackagesToAdopt(Idx, {"a_exec"}, {}, rx).count(Stub), "the next download installs it");

        fs::remove(Lib);
        fs::path NewDir;
        QVERIFY2(PackageCatalog::AdoptReceivedPackage(rx, Stub, &NewDir, &Err), Err.c_str());
        QCOMPARE(NewDir, Lib / "[1] A");
        QVERIFY2(!IpfsWrapper::CidMissing(ACid), "installed, the reference points at its new place");
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A node fetch planned before its package was installed is redirected into the installed package — and there it
    // lands a missing file but never replaces the package's own node file: only a version switch changes an installed
    // package. Teeth: keep Verify on a redirected target (the installed node is replaced by the peer's).
    void a_redirected_node_fetch_never_replaces_an_installed_node()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::string Stub = D.path().toStdString() + "/CATALOG/Alice - Games/[1] A", Installed = D.path().toStdString() + "/LIBRARY/Games/[1] A";
        std::filesystem::create_directories(Installed);
        const json Mine = json{{"LABEL", "a"}, {"LAYERS", json::array({ json{{"DIR", "mine"}} })}};
        const json Theirs = json{{"LABEL", "a"}, {"LAYERS", json::array({ json{{"DIR", "theirs"}} })}};
        std::ofstream(Installed + "/a.json", std::ios::binary) << Cid::Canonical(Mine);
        QCOMPARE(IpfsWrapper::BlockPut(Cid::Canonical(Theirs), &Err), Cid::OfNode(Theirs));   // fetchable
        IpfsWrapper::RedirectDestsUnder(Stub, Installed);
        std::string WErr;
        QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({ IpfsWrapper::FetchTarget{ Cid::OfNode(Theirs), Stub + "/a.json",
                     /*Optional=*/false, /*Dir=*/false, /*Verify=*/true } }), 30000, &WErr), WErr.c_str());
        const auto Holds = [&]{ std::ifstream In(Installed + "/a.json", std::ios::binary);
                                return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>()); };
        QCOMPARE(Holds(), Cid::Canonical(Mine));
        //…also when another requester of the same node asks for a refreshed file elsewhere (the job's Verify is theirs):
        //the redirected dest is filled only if missing. Teeth: drop the per-dest Keep in RunJob. (A queue reset drops
        //the redirects too: registered again each time.)
        const auto Reset = [&]{ IpfsWrapper::DebugResetQueue(); IpfsWrapper::RedirectDestsUnder(Stub, Installed); };
        const auto HoldsAt = [&](const std::string &P, const json &N) { std::ifstream In(P, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>()) == Cid::Canonical(N); };
        Reset();
        const json Theirs2 = json{{"LABEL", "a"}, {"LAYERS", json::array({ json{{"DIR", "theirs2"}} })}};
        QCOMPARE(IpfsWrapper::BlockPut(Cid::Canonical(Theirs2), &Err), Cid::OfNode(Theirs2));
        const std::string Elsewhere = D.path().toStdString() + "/CATALOG/Bob - Games/[1] A/a.json";
        QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch({
                     IpfsWrapper::FetchTarget{ Cid::OfNode(Theirs2), Elsewhere, /*Optional=*/false, /*Dir=*/false, /*Verify=*/true },
                     IpfsWrapper::FetchTarget{ Cid::OfNode(Theirs2), Stub + "/a.json", /*Optional=*/false, /*Dir=*/false, /*Verify=*/true } }),
                 30000, &WErr), WErr.c_str());
        QVERIFY(HoldsAt(Elsewhere, Theirs2));
        QCOMPARE(Holds(), Cid::Canonical(Mine));
        //…when the node's fetch is done already (elsewhere) and the redirected request joins it: the installed file
        //is there, and stays. Teeth: keep Verify on a redirected target (the join copies the peer's node over it).
        Reset();
        const auto Fetch = [&](const std::vector<IpfsWrapper::FetchTarget> &T) {
            std::string E; QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(T), 30000, &E), E.c_str()); };
        const auto Node = [&](const char *Dir) { const json N = json{{"LABEL", "a"}, {"LAYERS", json::array({ json{{"DIR", Dir}} })}};
            std::string E; IpfsWrapper::BlockPut(Cid::Canonical(N), &E); return N; };
        const auto Target = [](const json &N, const std::string &Dest) {
            return IpfsWrapper::FetchTarget{ Cid::OfNode(N), Dest, /*Optional=*/false, /*Dir=*/false, /*Verify=*/true }; };
        const json Theirs3 = Node("theirs3");
        const std::string Elsewhere3 = D.path().toStdString() + "/CATALOG/Carol - Games/[1] A/a.json";
        Fetch({ Target(Theirs3, Elsewhere3) });
        Fetch({ Target(Theirs3, Stub + "/a.json") });
        QVERIFY(HoldsAt(Elsewhere3, Theirs3));
        QCOMPARE(Holds(), Cid::Canonical(Mine));
        //…when every dest of the fetch is there already, an older version at the other one: the fetch lands the node
        //there — never a copy of the installed file. Teeth: take the first dest (the installed file) for the fetch.
        Reset();
        const json Theirs4 = Node("theirs4");
        const std::string Elsewhere4 = D.path().toStdString() + "/CATALOG/Dave - Games/[1] A/a.json";
        std::filesystem::create_directories(std::filesystem::path(Elsewhere4).parent_path());
        std::ofstream(Elsewhere4, std::ios::binary) << Cid::Canonical(Theirs2);               // an older version
        Fetch({ Target(Theirs4, Stub + "/a.json"), Target(Theirs4, Elsewhere4) });
        QVERIFY2(HoldsAt(Elsewhere4, Theirs4), "the other dest was copied from the installed file");
        QCOMPARE(Holds(), Cid::Canonical(Mine));
        //…when the installed file is the only dest of a finished job and another request joins it: that request is
        //fetched, never copied from the installed file. Teeth: let FirstExisting pick a kept dest.
        Reset();
        const json Theirs5 = Node("theirs5");
        const std::string Elsewhere5 = D.path().toStdString() + "/CATALOG/Erin - Games/[1] A/a.json";
        Fetch({ Target(Theirs5, Stub + "/a.json") });
        QCOMPARE(IpfsWrapper::DebugJobState(Cid::OfNode(Theirs5)), 2 /* Done: the file is there */);
        Fetch({ Target(Theirs5, Elsewhere5) });
        QVERIFY2(HoldsAt(Elsewhere5, Theirs5), "a request was copied from the installed file");
        QCOMPARE(Holds(), Cid::Canonical(Mine));
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A peer's version is compared with the package installed where that peer's package would have landed: two of
    // the peer's packages whose names clash land (and install) under qualified names, so neither is compared with a
    // package of the plain name. Teeth: look for the installed package under its plain name.
    void a_version_is_compared_with_the_package_where_it_would_have_landed()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::string Lib = D.path().toStdString() + "/LIBRARY";
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", Lib}}}}}};
        const json Items = json::array({ json{{"cid", "bafkreifoonode1"}, {"pkg", "Foo:1"}, {"node", "a.json"}},
                                         json{{"cid", "bafkreifoonode2"}, {"pkg", "foo?1"}, {"node", "b.json"}} });
        rx["PeerLibraries"]["Alice"]["Games"] = Items;
        const auto Folders = PackageCatalog::ReceivedFolderNames(json{{"Games", Items}});
        const std::string Q1 = Folders.at({"Games", "Foo:1"});
        auto install = [&](const std::string &Folder, const std::string &Label) {
            std::filesystem::create_directories(Lib + "/Games/" + Folder);
            const json N = json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"DIR", Label}} })}};
            std::ofstream(Lib + "/Games/" + Folder + "/" + Label + ".json", std::ios::binary) << Cid::Canonical(N);
        };
        install(Q1, "mine");                                                  // Foo:1, installed earlier (another version)
        install("Foo_1", "unrelated");                                        // a package of ours with the plain name
        const auto Offered = PackageCatalog::AvailableUpdates(rx);
        QCOMPARE((int)Offered.size(), 1);
        QCOMPARE(Offered[0].Dir.filename().string(), Q1);
    }

    // The library NAME in a peer's snapshot is theirs to choose. Installing a received package must land it inside
    // LIBRARY whatever that name holds — "../..", an absolute path, "..". Teeth: drop the adopt's sanitising and a
    // package lands outside the library.
    void adopting_under_a_hostile_library_name_stays_in_the_library()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        for (const std::string &Lib : {std::string("../../escape"), std::string("/tmp/vg_adopt_escape"), std::string("..")})
        {
            QTemporaryDir RxData; QVERIFY(RxData.isValid());
            json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                           {"PeerLibraries", {{"12D3KooWSeederAlice", {{Lib, Items}}}}}};
            landShares(rx, "derAlice", json{{Lib, Items}});
            const auto Plan = PackageCatalog::PlanReceivedFetches(rx, "derAlice", json{{Lib, Items}});
            QVERIFY(!Plan.empty());
            const std::filesystem::path Stub = std::filesystem::path(Plan.front().Dest).parent_path();
            std::filesystem::path NewDir;
            const bool Ok = PackageCatalog::AdoptReceivedPackage(rx, Stub, &NewDir, &Err);
            const std::string Library = std::filesystem::weakly_canonical(RxData.path().toStdString() + "/LIBRARY").generic_string();
            if (Ok)
                QVERIFY2(std::filesystem::weakly_canonical(NewDir).generic_string().rfind(Library + "/", 0) == 0,
                         ("installed outside the library: " + NewDir.string()).c_str());
            QVERIFY2(!std::filesystem::exists("/tmp/vg_adopt_escape"), "a package landed at an absolute library name");
            QVERIFY2(!std::filesystem::exists(RxData.path().toStdString() + "/../escape"), "a package landed above the library");
        }
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A RE-PUBLISHED node (same label → same file, a NEW CID) must land OVER the occupied file through the REAL rolling
    // queue — the receiver's whole update path — at EVERY dest the snapshot routes it to. Teeth: let the queue settle
    // a new-CID job on the occupied dest and the v2 asserts fail (the dest keeps v1 forever).
    void republished_package_lands_over_the_stale_dest()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir SeedRoot; QVERIFY(SeedRoot.isValid());
        const json Items = publishTwoGames(SeedRoot.path());
        QCOMPARE((int)Items.size(), 7);                         // packages A (5 nodes) and B (2)
        const std::string CidV1 = cidOfLabel(Items, "a_exec");
        QVERIFY(!CidV1.empty());

        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        // TWO libraries carry the same items (the multi-dest normal: one CID job, several dests).
        auto Land = [&](const json & SnapItems) {   // the snapshot held, then landed — as the app does
            rx["PeerLibraries"]["Alice"] = json{{"Games", SnapItems}, {"Favs", SnapItems}};
            landShares(rx, "Alice", json{{"Games", SnapItems}, {"Favs", SnapItems}});
        };
        auto Holds = [&](const std::string & Lib, const std::string & Cid) {   // a_exec.json there holds that node
            std::ifstream In(Catalog + "/Alice - " + Lib + "/[1] A/a_exec.json", std::ios::binary);
            return In && Cid::OfBytes(std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>())) == Cid;
        };
        Land(Items);
        QVERIFY2(Holds("Games", CidV1) && Holds("Favs", CidV1), "v1's node file landed in both");

        // Seeder updates the node (same LABEL, new content) and re-publishes → NEW cid, SAME file.
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
        for (const auto & [C, P] : seeder2["ShareSheet"].items()) SeederBlocks[C] = IpfsWrapper::BlockGet(C, &Err);
        const json Items2 = PackageCatalog::Wire(PackageCatalog::SheetEntries(seeder2["ShareSheet"]))["VidyaGod"];
        const std::string CidV2 = cidOfLabel(Items2, "a_exec");
        QVERIFY2(!CidV2.empty() && CidV2 != CidV1, "the update re-minted the node to a NEW cid");

        {   // the queue alone (what the app runs when a snapshot arrives) replaces the stale file at every dest
            rx["PeerLibraries"]["Alice"] = json{{"Games", Items2}, {"Favs", Items2}};
            for (const auto & [C, B] : SeederBlocks) { std::string E; IpfsWrapper::BlockPut(B, &E); }
            std::string WErr;
            QVERIFY2(IpfsWrapper::WaitBatch(IpfsWrapper::EnqueueBatch(PackageCatalog::ReceivedFetchTargets(
                         PackageCatalog::PlanReceivedFetches(rx, "Alice", rx["PeerLibraries"]["Alice"]))), 30000, &WErr), WErr.c_str());
            QVERIFY2(Holds("Games", CidV2) && Holds("Favs", CidV2), "the queue took a stale file for the node it asked for");
        }
        Land(Items2);
        QVERIFY2(Holds("Games", CidV2) && Holds("Favs", CidV2), "v2's node file REPLACED the stale one at EVERY dest");
        NodeIndex After = PackageCatalog::BuildCatalogIndex(rx);
        QCOMPARE(PackageCatalog::PruneStaleReceived(After, rx), 0);   // nothing stale left
        // …and the prune does judge these dirs: a node file their entries do not name goes.
        const std::string Stray = Catalog + "/Alice - Games/[1] A/stray.json";
        {   // v1's bytes, under a name no entry has
            std::ofstream O(Stray, std::ios::binary | std::ios::trunc);
            O << SeederBlocks.at(CidV1);
        }
        QVERIFY(PackageCatalog::PruneStaleReceived(PackageCatalog::BuildCatalogIndex(rx), rx) >= 1);
        QVERIFY2(!std::filesystem::exists(Stray), "a node file the current entries do not name survived the prune");

        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // Two peers share the same library and package name — the multi-seeder normal. Each landed dir is judged against
    // ITS peer's entries: Bob's current node is neither pruned for missing from Alice's, nor joined by Alice's.
    // Teeth: judge a dir by library and package name alone (whichever peer's entries come first).
    void two_peers_packages_of_one_name_are_judged_each_by_its_own_folder()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Root = PackageCatalog::CatalogRootDir(rx);
        const json X = json{{"LABEL", "v1"}, {"LAYERS", json::array({ json{{"DIR", "s"}} })}};
        const json Y = json{{"LABEL", "v2"}, {"LAYERS", json::array({ json{{"DIR", "y"}} })}};
        const std::string XB = Cid::Canonical(X), XC = Cid::OfBytes(XB), YB = Cid::Canonical(Y), YC = Cid::OfBytes(YB);
        auto land = [&](const std::string &Peer, const std::string &C, const std::string &B, const std::string &F) {
            const std::string D = Root + "/" + Peer + " - Games/[1] A";
            std::filesystem::create_directories(D);
            { std::ofstream O(D + "/" + F, std::ios::binary); O << B; }
            std::string E;
            if (IpfsWrapper::AddNoCopy(D + "/" + F, &E) != C) return std::string();
            rx["PeerLibraries"][Peer]["Games"].push_back(json{{"cid", C}, {"pkg", "[1] A"}, {"node", F}});
            return D;
        };
        const std::string Alice = land("Alice", XC, XB, "v1.json");
        const std::string Bob = land("Bob", YC, YB, "v2.json");
        QVERIFY(!Alice.empty() && !Bob.empty());
        QVERIFY2(!PackageCatalog::ReceivedPackagesIncomplete(rx), "each dir holds everything its own entries name");
        { std::ofstream O(Bob + "/settings.json"); O << "{}"; }                           // a .json that is no node
        QCOMPARE(PackageCatalog::PruneStaleReceived(PackageCatalog::BuildCatalogIndex(rx), rx), 0);
        QVERIFY2(std::filesystem::exists(Bob + "/settings.json"), "a file that is not a node was pruned as one");
        QVERIFY2(std::filesystem::exists(Bob + "/v2.json"), "Bob's current node was pruned against Alice's entries");
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &Err), Err.c_str());
        QVERIFY2(!std::filesystem::exists(Bob + "/v1.json"), "Alice's node landed in Bob's dir");
        QVERIFY2(!std::filesystem::exists(Alice + "/v2.json"), "Bob's node landed in Alice's dir");
        // …and Bob's dir IS judged, against Bob's entries: a stray node file there is pruned, a listed one missing is noticed.
        const json Z = json{{"LABEL", "stray"}, {"LAYERS", json::array({ json{{"DIR", "z"}} })}};
        const std::string ZB = Cid::Canonical(Z);
        { std::ofstream O(Bob + "/stray.json", std::ios::binary); O << ZB; }
        QVERIFY(PackageCatalog::PruneStaleReceived(PackageCatalog::BuildCatalogIndex(rx), rx) >= 1);
        QVERIFY2(!std::filesystem::exists(Bob + "/stray.json"), "Bob's dir was never judged: a stray node survived");
        std::filesystem::remove(Bob + "/v2.json");
        QVERIFY2(PackageCatalog::ReceivedPackagesIncomplete(rx), "Bob's dir was never judged: a missing listed node went unnoticed");
        // Alice's next generation puts another node under the same name: the file there (the older generation's) is
        // replaced in one pass, never refused and left for the next. Teeth: drop LandReceivedPackages' first pass over
        // present files.
        const json W = json{{"LABEL", "v1"}, {"LAYERS", json::array({ json{{"DIR", "w"}} })}};
        const std::string WB = Cid::Canonical(W), WC = Cid::OfBytes(WB);
        {   // the peers hold their nodes (one node plays both machines): put them as seeders hold them
            std::string E;
            QCOMPARE(IpfsWrapper::BlockPut(WB, &E), WC);
            QCOMPARE(IpfsWrapper::BlockPut(YB, &E), YC);
        }
        rx["PeerLibraries"]["Alice"]["Games"] = json::array({ json{{"cid", WC}, {"pkg", "[1] A"}, {"node", "v1.json"}} });
        std::string LErr;
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &LErr), LErr.c_str());
        {
            std::ifstream In(Alice + "/v1.json", std::ios::binary);
            QCOMPARE(Cid::OfBytes(std::string((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>())), WC);
        }
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // A received package's node list is its entries — also for the second of two packages whose names make one folder
    // name, which lands as "<folder> (<hash of its name>)": a node file missing from THAT package is still noticed
    // (and landed). Teeth: judge a landed dir by its plain folder name only.
    void a_same_named_second_package_is_completed_from_its_own_entries()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        QTemporaryDir Src; QVERIFY(Src.isValid());
        auto node = [&](const std::string & Label) {   // a node a peer holds
            const std::string Bytes = Cid::Canonical(json{{"LABEL", Label}, {"LAYERS", json::array({ json{{"DIR", Label}} })}});
            const std::string F = Src.path().toStdString() + "/" + Label + ".json";
            { std::ofstream O(F, std::ios::binary); O << Bytes; }
            std::string E;
            return IpfsWrapper::AddNoCopy(F, &E);
        };
        const std::string C1 = node("first"), C2 = node("second");
        QVERIFY(!C1.empty() && !C2.empty());
        QTemporaryDir RxData; QVERIFY(RxData.isValid());
        const json Items = json::array({ json{{"cid", C1}, {"pkg", "[1] Same:"}, {"node", "first.json"}},
                                         json{{"cid", C2}, {"pkg", "[1] Same?"}, {"node", "second.json"}} });
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (RxData.path() + "/LIBRARY").toStdString()}}}}},
                       {"PeerLibraries", {{"Alice", {{"Games", Items}}}}}};
        landShares(rx, "Alice", json{{"Games", Items}});
        std::string Second;
        for (const auto & T : PackageCatalog::PlanReceivedFetches(rx, "Alice", json{{"Games", Items}}))
            if (T.Cid == C2) Second = std::filesystem::path(T.Dest).parent_path().string();
        QVERIFY2(std::filesystem::path(Second).filename().string().rfind("[1] Same_ (", 0) == 0, Second.c_str());
        QVERIFY2(std::filesystem::exists(Second + "/second.json"), "precondition: the second package landed qualified");
        QVERIFY(!PackageCatalog::ReceivedPackagesIncomplete(rx));
        std::filesystem::remove(Second + "/second.json");
        QCOMPARE(IpfsWrapper::AddNoCopy(Src.path().toStdString() + "/second.json", &Err), C2);   // the peer still holds it
        QVERIFY2(PackageCatalog::ReceivedPackagesIncomplete(rx), "a node missing from the qualified package went unnoticed");
        QVERIFY2(PackageCatalog::LandReceivedPackages(rx, &Err), Err.c_str());
        QVERIFY2(std::filesystem::exists(Second + "/second.json"), "the missing node was not landed");
        IpfsWrapper::DebugResetQueue();
        IpfsWrapper::StopNode();
    }

    // UNTRUSTED-input bounds of the planner, with teeth for each: (1) every dest path SEGMENT is capped under
    // NAME_MAX even when nick/lib/uid/title/node arrive at the wire maximums (else the queue retries an
    // ENAMETOOLONG mkdir forever); (2) an over-long or non-plain cid is dropped; (3) the entry bound takes whole packages.
    void planner_bounds_hostile_segments_and_counts()
    {
        QTemporaryDir PlanRoot; QVERIFY(PlanRoot.isValid());
        json rx = json{{"Settings", {{"Paths", {{"LibraryRoot", (PlanRoot.path() + "/LIBRARY").toStdString()}}}}}};
        const std::string Catalog = PackageCatalog::CatalogRootDir(rx);
        const std::string Long(1000, 'x');
        const json Items = json::array({ json{{"cid", "bafkreiplannerboundscidaaaaaaaaaaaaaaaaaaa"},
                                              {"node", Long}, {"pkg", Long}} });
        const auto Plan = PackageCatalog::PlanReceivedFetches(rx, Long, json{{Long, Items}});
        QCOMPARE((int)Plan.size(), 1);
        for (const auto & T : Plan)
            for (const auto & Part : std::filesystem::path(T.Dest.substr(Catalog.size() + 1)))
                QVERIFY2(Part.string().size() <= 130, ("segment too long: " + Part.string()).c_str());

        const json BadCid = json::array({ json{{"cid", Long}, {"pkg", "P"}, {"node", "n.json"}},             // >128 bytes → dropped
                                          json{{"cid", "baf/../x"}, {"pkg", "P"}, {"node", "n.json"}} });  // not plain → dropped
        QCOMPARE((int)PackageCatalog::PlanReceivedFetches(rx, "A", json{{"L", BadCid}}).size(), 0);

        // Names Windows cannot hold as given: a trailing '.' or ' ' (dropped there, so the folder the plan names never
        // exists) and device names (no file at all). Planned the same on every platform, so a receiver on either lands
        // the same name. Teeth: drop NodeFiles::SafeName's trailing-dot/space or device-name rule.
        for (const auto & [Lib, Pkg] : std::vector<std::pair<std::string, std::string>>{
                 {"..", "[1] Game."}, {"Games ", "[2] B "}, {"L", "CON"}, {"L", "nul.txt"}, {"L", "Com1 .x"}})
        {
            const json One = json::array({ json{{"cid", "bafkreiplannerwindowsnamesaaaaaaaaaaaaaaaa"}, {"pkg", Pkg}, {"node", Pkg}} });
            const auto P = PackageCatalog::PlanReceivedFetches(rx, "derAlice", json{{Lib, One}});
            QCOMPARE((int)P.size(), 1);
            for (const auto & Part : std::filesystem::path(P[0].Dest.substr(Catalog.size() + 1)))
            {
                const std::string N = Part.string();
                QVERIFY2(N.empty() || (N.back() != '.' && N.back() != ' '), ("ends in '.' or ' ': " + N).c_str());
                std::string Stem = N.substr(0, N.find('.'));
                while (!Stem.empty() && Stem.back() == ' ') Stem.pop_back();
                for (char &C : Stem) C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
                QVERIFY2(Stem != "CON" && Stem != "NUL" && Stem != "COM1", ("a device name: " + N).c_str());
            }
        }

        // The bound counts node entries and takes a package whole or not at all: P1 (60 000 nodes) fits, P2 (50 000)
        // would pass 100 000 and is left out entirely — never cut short, which would read complete.
        json Many = json::array();
        for (int i = 0; i < 60000; ++i) Many.push_back(json{{"cid", "bafkreicap" + std::to_string(i)}, {"pkg", "P1"}, {"node", "n" + std::to_string(i)}});
        for (int i = 0; i < 50000; ++i) Many.push_back(json{{"cid", "bafkreicapb" + std::to_string(i)}, {"pkg", "P2"}, {"node", "n" + std::to_string(i)}});
        const auto Capped = PackageCatalog::PlanReceivedFetches(rx, "A", json{{"L", Many}});
        QCOMPARE((int)Capped.size(), 60000);
        for (const auto & T : Capped) QVERIFY(T.Dest.find("/P1/") != std::string::npos);
    }

    // Publish-side bounds: a node's file name is its label made a safe name of at most 120 bytes, so an over-long label
    // never makes an entry a receiver's file system cannot hold (or its inbound gate refuses); and an object that is no
    // node (a legacy-typed block with no LAYERS) is no entry. Teeth: drop SafeName's cap and the name is 505 bytes;
    // index a non-node and h_exec appears.
    void publish_bounds_file_names_and_skips_non_nodes()
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
        writeJson(R + "/VidyaGod/[2] Long/l.json",
                  json{{"LABEL", std::string(500, 'T')}, {"LAYERS", json::array({ json{{"DIR", "d"}} })}});

        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);
        const auto Entries = PackageCatalog::SheetEntries(cfg["ShareSheet"]);
        QCOMPARE((int)Entries.size(), 1);
        QCOMPARE(Entries[0].Pkg, std::string("[2] Long"));
        QCOMPARE(Entries[0].File, std::string(120, 'T') + ".json");
        IpfsWrapper::StopNode();
    }

    // DATA-LOSS GUARD 3: removing a source keeping what was installed (`sources remove`, the default) deletes a STUB-only
    // package but KEEPS a package the user INSTALLED (a dir with hydrated content), files intact, its LIBRARY entry
    // kept. Teeth: without the branch, remove_all nukes the install.
    void remove_source_keep_installed_keeps_what_was_installed()
    {
        QTemporaryDir data; QVERIFY(data.isValid());
        AppPaths::SetDataRoot(data.path().toStdString());
        json cfg = json{{"Settings", {{"PackageSources", json::array({
                            json{{"CID", "QmGames"}, {"NAME", "Games"}} })}}},
                        {"LIBRARY", json::array()}};
        const std::string srcDir = PackageCatalog::PackageSourceDir(cfg, cfg["Settings"]["PackageSources"][0]);
        const QString stubDir = QString::fromStdString(srcDir) + "/[1] Stub";
        const QString instDir = QString::fromStdString(srcDir) + "/[2] Installed";
        QDir().mkpath(stubDir);
        QDir().mkpath(instDir);
        { std::ofstream f((stubDir + "/n.json").toStdString()); f << "{}"; }              // stub-only (metadata)
        { std::ofstream f((instDir + "/n.json").toStdString()); f << "{}"; }              // installed: metadata +
        { std::ofstream f((instDir + "/game.bin").toStdString()); f << "installed"; }     //   hydrated content
        { std::ofstream f(srcDir + "/.vgsource"); f << "QmGames\n[1] Stub\n[2] Installed\n"; }   // its own fetch made it
        cfg["LIBRARY"].push_back(json{{"PACKAGEUID", "1"}, {"PATH", stubDir.toStdString()}, {"CIDSOURCE", "QmGames"}});
        cfg["LIBRARY"].push_back(json{{"PACKAGEUID", "2"}, {"PATH", instDir.toStdString()}, {"CIDSOURCE", "QmGames"}});

        PackageCatalog::RemovePackageSource(cfg, 0, /*KeepInstalled=*/true);

        QVERIFY2(!QDir(stubDir).exists(), "a stub-only package is removed");
        QVERIFY2(QFile::exists(instDir + "/game.bin"), "installed content must be preserved, never rm'd");
        bool keptLocal = false;
        for (auto & e : cfg["LIBRARY"])
            if (e.is_object() && e.value("PATH", std::string()) == instDir.toStdString())
            keptLocal = true;
        QVERIFY2(keptLocal, "the installed package's LIBRARY entry survives");
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 0);   // the source config entry is gone
    }

    // SECURITY GUARD: a received peer STUB (under a "_peer_*" dir), even flagged PUBLISH, must NEVER enter the mint
    // when the user publishes — so it can't be re-shared as our own nor shadow our handles. Teeth: without the
    // GatherWorkingTree SkipReserved exclusion, the stub is minted and appears as an extra "_peer_*" library key.
    void publish_excludes_peer_stubs_from_the_mint()
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
        // A received peer stub, flagged PUBLISH, under a reserved _peer_ dir — must be excluded from our publish.
        QDir().mkpath(R + "/_peer_peerX_deadbeef/[9] Evil");
        writeJson(R + "/_peer_peerX_deadbeef/[9] Evil/x.json",
                  json::array({ json{{"LABEL", "peerevil"}, {"TYPE", "DeclareExec"}, {"HOST", "win32"},
                                     {"PATH", "evil.exe"}, {"PUBLISH", true}} }));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        PackageCatalog::PublishLibrary(cfg, &Err);
        std::set<std::string> Libs;
        for (const auto & E : PackageCatalog::SheetEntries(cfg["ShareSheet"])) Libs.insert(E.Lib);
        QCOMPARE(Libs, (std::set<std::string>{ "VidyaGod" }));   // ONLY VidyaGod — the _peer_ stub never entered the mint
        IpfsWrapper::StopNode();
    }

    //The data root is PROCESS-GLOBAL and sticky, and PackageEditorModel::SaveLayout flushes GlobalConfig.JSON
    //to it — so a suite that does not claim it writes to AppPaths' fallback, which is the developer's REAL
    //~/.VidyaGod/GlobalConfig.JSON. Running a single slot by name replaced a 50 KB config (sources, CIDs,
    //peers, per-package variables) with a two-key stub, and reported PASS. A full-suite run only escaped
    //because an earlier slot happened to leave the root pointing somewhere else — order-dependent luck that
    //any reordering removes. Claim it once, for the whole binary.
    void initTestCase()
    {
        SuiteRoot = new QTemporaryDir();
        QVERIFY(SuiteRoot->isValid());
        AppPaths::SetDataRoot(SuiteRoot->path().toStdString());
        //Launch sessions live under the temp dir (LaunchResolver::SessionRoot): this machine's own running games must
        //not read as "a game is running" to the switch tests.
        qputenv("TMPDIR", SuiteRoot->path().toUtf8());
    }
    void cleanupTestCase() { delete SuiteRoot; SuiteRoot = nullptr; }
    //After EVERY slot, passed or not: a failing check returns before the slot's own StopNode, and the node it left
    //running (its repo open, its queue full) became the next slot's — on Windows one failure cascaded into a crash two
    //slots later, and the temp dirs it held open could not be removed.
    void cleanup() { IpfsWrapper::DebugResetQueue(); IpfsWrapper::StopNode(); }

    // #2: a content-less node is vacuously "hydrated" but has no content — NodeHasContent must distinguish it.
    void node_has_content_vs_contentless()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Content("dir", "data"), NodeFixture::Exec("win32", "g.exe")}));
        writeJson(dir.path() + "/empty.json", NodeFixture::Chain("empty", {NodeFixture::Exec("win32", "g.exe")}));
        QDir(dir.path() + "/data").mkpath(".");                 // make 'game' hydrated

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
        QVERIFY(Installed::NodeHasContent(idx, "game"));
        QVERIFY(!Installed::NodeHasContent(idx, "empty"));  // content-less → hidden from Library/Installed
        QVERIFY(Installed::NodeHydrated(idx, "game"));      // data dir present
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
        QVERIFY(!Installed::NodeHydrated(idx0, "remote"));     // CID-backed blob.bin not fetched yet
        QVERIFY(Installed::NodeHydrated(idx0, "localonly"));   // no CID → nothing fetchable is missing

        writeFile(dir.path() + "/blob.bin");                        // the fetch lands the file locally
        NodeIndex idx1; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx1); ManifestModel::DeriveFacts(idx1);
        QVERIFY(Installed::NodeHydrated(idx1, "remote"));      // now fully fetched
    }

    // #8/#9: DehydrateNode deletes the game's own downloaded content (keeping the manifest) and reports what it removed;
    // a library in another package's folder (other games' too) and a file with no CID (nothing to download it again
    // from) stay. Teeth: delete every content file of the closure (the shared library and the unsourced file go).
    void dehydrate_node_removes_content_keeps_manifest()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QTemporaryDir other; QVERIFY(other.isValid());
        writeJson(other.path() + "/lib.json",
                  NodeFixture::Chain("sharedlib", {NodeFixture::ContentCid("file", "shared.dll", "bafkreib52upmn2n6u65qll6mmj2dft4ddgnvrkcvyhiczcbjlrv2lu766e")}));
        writeFile(other.path() + "/shared.dll");
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {json{{"NODE", "sharedlib"}},
                                              NodeFixture::ContentCid("file", "blob.bin", "bafkreigh2akiscaildcqabsyg3dfr6chu3fgpregiymsck7e7aqa4s52zy"),
                                              NodeFixture::Content("file", "mine.bin"), NodeFixture::Exec("win32", "g.exe")}));
        writeFile(dir.path() + "/blob.bin");
        writeFile(dir.path() + "/mine.bin");

        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx);
        ManifestModel::ScanBundleNodes(other.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
        QVERIFY(Installed::NodeHydrated(idx, "game"));

        const int removed = PackageCatalog::DehydrateNode(idx, "game");
        QCOMPARE(removed, 1);
        QVERIFY(!QFile::exists(dir.path() + "/blob.bin"));       // content gone
        QVERIFY(QFile::exists(dir.path() + "/game.json"));       // manifest kept → returns to Catalog
        QVERIFY(QFile::exists(other.path() + "/shared.dll"));    // another package's library: other games' too
        QVERIFY(QFile::exists(dir.path() + "/mine.bin"));        // no CID: nothing to download it again from
    }

    // An export folder is replaced whole, so one that is not an export's is refused before anything is minted: the
    // package itself, a folder inside it, one holding it, a non-empty folder no export made. An empty one and an earlier
    // export's are fine. Teeth: accept the package's parent (the library); accept a folder of the user's.
    void an_export_never_replaces_a_folder_that_is_not_an_exports()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::string Root = D.path().toStdString(), Pkg = Root + "/LIBRARY/VidyaGod/[1] Game";
        std::filesystem::create_directories(Pkg);
        std::ofstream(Pkg + "/game.json") << "{}";
        std::filesystem::create_directories(Root + "/mine");
        std::ofstream(Root + "/mine/notes.txt") << "mine";
        std::filesystem::create_directories(Root + "/old_export");
        std::ofstream(Root + "/old_export/.vgexport") << "an export";
        QVERIFY(!PackageCatalog::ExportDestRefused(Pkg, Pkg).empty());
        QVERIFY(!PackageCatalog::ExportDestRefused(Pkg, Pkg + "/out").empty());
        QVERIFY(!PackageCatalog::ExportDestRefused(Pkg, Root + "/LIBRARY").empty());
        std::ofstream(Root + "/LIBRARY/VidyaGod/.vgexport") << "a mistaken export's marker";
        QVERIFY(!PackageCatalog::ExportDestRefused(Pkg, Root + "/LIBRARY/VidyaGod").empty());   // still holds the package
        QVERIFY(!PackageCatalog::ExportDestRefused(Pkg, Root + "/mine").empty());
        QVERIFY(PackageCatalog::ExportDestRefused(Pkg, Root + "/new_export").empty());
        QVERIFY(PackageCatalog::ExportDestRefused(Pkg, Root + "/old_export").empty());
        std::string Err;
        QVERIFY(!PackageCatalog::PublishPackage(Pkg, Root + "/LIBRARY", &Err));
        QVERIFY(std::filesystem::exists(Pkg + "/game.json") && std::filesystem::exists(Root + "/mine/notes.txt"));
        QVERIFY(PackageCatalog::MirrorDehydrated(Pkg, Pkg + "/inside") == 0 && !std::filesystem::exists(Pkg + "/inside/game.json"));
    }

    // Per-package user settings persist into the INSTANCE file (InstanceStore) and read back. The temp UserDataRoot
    // isolates the write from the real ~/.VidyaGod; a write to the active instance auto-creates DefaultInstance.
    void package_user_settings_roundtrip()
    {
        QTemporaryDir ud; QVERIFY(ud.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"UserDataRoot", ud.path().toStdString()}}}}}};
        InstanceStore::SetSetting(cfg, "pkg1", "PREFERRED_RUNNER", "wine-ge");
        json us = InstanceStore::ReadConfig(cfg, "pkg1");
        QCOMPARE(us.value("PREFERRED_RUNNER", std::string()), std::string("wine-ge"));
        QVERIFY(InstanceStore::ReadConfig(cfg, "other").value("PREFERRED_RUNNER", std::string()).empty());
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

        auto runners = Installed::CompatibleRunners(idx, idx.Nodes["game"]);
        bool hasWine = false, hasSnes = false;
        for (const Node * r : runners) { if (r->NodeId == "wine") hasWine = true; if (r->NodeId == "snes9x") hasSnes = true; }
        QVERIFY(hasWine);
        QVERIFY(!hasSnes);   // snes runner doesn't serve win32
    }

    // A "custom" engine is offered only to its own package: the chain-step choices and the runner list both ask where
    // the launch lives. Teeth: drop LaunchBundleDir from either call's ServesPlatform and the other package sees it.
    void a_custom_engine_is_offered_only_to_its_own_package()
    {
        NodeIndex idx;
        Node engine; engine.NodeId = "engine"; engine.HasRunner = true; engine.GuestPlatform = {"custom"};
        engine.HostPlatform = ManifestModel::MachinePlatform(); engine.BundleDir = "/pkg/a";
        engine.Exec = json{{"EXECUTABLE", "%RunnerMount%/engine"}};
        idx.Nodes["engine"] = engine;
        Node mine; mine.NodeId = "mine"; mine.HasExec = true; mine.HostPlatform = "custom"; mine.BundleDir = "/pkg/a";
        Node theirs = mine; theirs.NodeId = "theirs"; theirs.BundleDir = "/pkg/b";
        QCOMPARE((int)Installed::CandidateRunners(idx, "custom", "/pkg/a").size(), 1);
        QCOMPARE((int)Installed::CandidateRunners(idx, "custom", "/pkg/b").size(), 0);
        QCOMPARE((int)Installed::CompatibleRunners(idx, mine).size(), 1);
        QCOMPARE((int)Installed::CompatibleRunners(idx, theirs).size(), 0);
    }

    // The runners a game package carries, found in one pass, are the ones IsEmbeddedRunner names runner by runner (the
    // Runners page asked each of ~200 runners, every node's folder compared each time). Teeth: count a runner beside
    // anything (the standalone one is embedded); compare against runners' folders (none is).
    void embedded_runners_in_one_pass_agree_runner_by_runner()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString Game = dir.path() + "/Game", Wine = dir.path() + "/Wine";
        QDir().mkpath(Game); QDir().mkpath(Wine);
        writeJson(Game + "/game.json", NodeFixture::Chain("game", {NodeFixture::Merge({NodeFixture::Variant("Original"),
                  NodeFixture::Tile("g"), NodeFixture::Exec("custom", "g.exe")})}));
        writeJson(Game + "/engine.json", NodeFixture::Chain("engine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"custom"}, "e")}));
        writeJson(Wine + "/wine.json", NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")}));
        writeJson(Wine + "/notes.json", NodeFixture::Chain("notes", {NodeFixture::Content("zip", "n.zip")}));
        NodeIndex idx;
        ManifestModel::ScanBundleNodes(Game.toStdString(), idx);
        ManifestModel::ScanBundleNodes(Wine.toStdString(), idx);
        ManifestModel::DeriveFacts(idx);
        const std::set<std::string> E = Installed::EmbeddedRunners(idx);
        int Runners = 0;
        for (const auto &[Id, N] : idx.Nodes)
        {
            if (!N.IsRunner()) continue;
            ++Runners;
            QCOMPARE(E.count(N.Key()) == 1, Installed::IsEmbeddedRunner(idx, N.Key()));
        }
        QCOMPARE(Runners, 2);
        QCOMPARE(E.size(), size_t(1));
    }

    // The Runners page waits to be shown: hidden, a catalog change (every download's end, every peer library) does not
    // rebuild it; shown, it is built once. Rebuilds asked while its games count runs count once more after it, not
    // once each. Teeth: drop the isVisible gate (rebuilt while hidden); drop the showEvent rebuild (never built); drop
    // the Counting gate (a count per rebuild).
    void the_runners_page_waits_to_be_shown_and_counts_once_at_a_time()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QDir().mkpath(dir.path() + "/Runners/wine");
        writeJson(dir.path() + "/Runners/wine/wine.json", NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", dir.path().toStdString()}}}}}};
        QDir appDir(dir.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        RunnersPage Page(model);
        QCOMPARE(Page.rebuilds(), 0);
        for (int I = 0; I < 3; ++I) emit model.catalogChanged();
        QCOMPARE(Page.rebuilds(), 0);
        Page.show();
        QTRY_COMPARE_WITH_TIMEOUT(Page.rebuilds(), 1, 2000);
        const int Counts = Page.counts();
        for (int I = 0; I < 5; ++I) emit model.catalogChanged();
        QCOMPARE(Page.rebuilds(), 6);
        QTest::qWait(500);
        QVERIFY2(Page.counts() - Counts <= 2, std::to_string(Page.counts() - Counts).c_str());
    }

    // Settings → Installed Packages waits to be shown, and looks for other versions one scan at a time: every catalog
    // change copied the config and read the installed packages' node files off-thread, a scan per change, beside each
    // other, page shown or not. Teeth: drop the isVisible gate (scans while hidden); drop the Updating gate (a scan
    // per change).
    void the_packages_page_waits_to_be_shown_and_scans_once_at_a_time()
    {
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto Stop = qScopeGuard([]{ IpfsWrapper::StopNode(); });
        QTemporaryDir dir; QVERIFY(dir.isValid());
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", dir.path().toStdString()}}}}}};
        QDir appDir(dir.path());
        AppModel model(&cfg, &appDir);
        PackagesView View(model);
        for (int I = 0; I < 3; ++I) emit model.catalogChanged();
        QCOMPARE(View.updateScans(), 0);
        View.show();
        QTRY_COMPARE_WITH_TIMEOUT(View.updateScans(), 1, 2000);
        for (int I = 0; I < 5; ++I) emit model.catalogChanged();
        QTest::qWait(500);
        QVERIFY2(View.updateScans() <= 3, std::to_string(View.updateScans()).c_str());
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
        QVERIFY(PackageCatalog::CollectRunnerNodeTargets(idx, "wine", targets, &err));

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
        G.ContentLayers.push_back(json{{"TYPE", "VFSZipLayer"}, {"PATH", "/tmp/vg_escape_target.zip"},
                                {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "CID_ESCAPE"}}}});
        G.ContentLayers.push_back(json{{"TYPE", "VFSZipLayer"}, {"PATH", "../outside.zip"},
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
            return Installed::NodeHydrated(idx, "game");      // file ABSENT + a CID => NOT hydrated
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
        QVERIFY(Installed::NodeHydrated(ridx, "game"));
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

        QVERIFY(Installed::RunnerBuildPresent(idx, "wine"));      // the build IS here
        QVERIFY(Installed::RunnerInstalled(idx, "wine"));        // ...so the runner is installed

        // ...and it is not something to download, either: a fetch target for a %variable% path would have the
        // download manager forever re-offering an already-hydrated runner.
        std::vector<IpfsWrapper::FetchTarget> targets; std::string err;
        QVERIFY(PackageCatalog::CollectRunnerNodeTargets(idx, "wine", targets, &err));
        for (const auto & t : targets)
            QVERIFY2(t.LocalPath.find('%') == std::string::npos, t.LocalPath.c_str());

        // The control: remove the REAL build and the runner must go back to not-installed, or this test would
        // pass just as well against a gate that checks nothing at all.
        QVERIFY(QFile::remove(dir.path() + "/wine.zip"));
        NodeIndex idx2; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx2); ManifestModel::DeriveFacts(idx2);
        QVERIFY(!Installed::RunnerBuildPresent(idx2, "wine"));
        QVERIFY(!Installed::RunnerInstalled(idx2, "wine"));

        // A runner whose build is ON the runner node (folded: ENTRYPOINTS + LAYERS) is a build-shipping runner
        // too: installed iff its zip is here, and its zip is what a fetch collects when it is not.
        QTemporaryDir d3; QVERIFY(d3.isValid());
        writeJson(d3.path() + "/java.json",
                  NodeFixture::Chain("java", {NodeFixture::Merge({NodeFixture::Runner(ManifestModel::MachinePlatform(), {"java_8"}, "%RunnerMount%/__jre/bin/java"),
                                                                  NodeFixture::Content("zip", "jre.zip")})}));
        NodeIndex idx3; ManifestModel::ScanBundleNodes(d3.path().toStdString(), idx3); ManifestModel::DeriveFacts(idx3);
        QVERIFY2(!Installed::RunnerInstalled(idx3, "java"), "build on the runner node, zip absent ⇒ not installed");
        QVERIFY(!Installed::RunnerBuildPresent(idx3, "java"));
        writeFile(d3.path() + "/jre.zip");
        NodeIndex idx4; ManifestModel::ScanBundleNodes(d3.path().toStdString(), idx4); ManifestModel::DeriveFacts(idx4);
        QVERIFY2(Installed::RunnerBuildPresent(idx4, "java"), "…and present once the zip is here");
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
        std::vector<IpfsWrapper::FetchTarget> targets;
        PackageCatalog::CollectRunnerChainTargets(idx, "game", cfg, targets);
        bool hasWine = false;
        for (const auto & t : targets) { if (t.Cid == "CID_WINE") hasWine = true; }
        QVERIFY(hasWine);                       // runner build auto-pooled purely from the game's resolved chain
    }

    // A download pools every entry's runner, not the default's alone: a game with a native Play and a Windows entry
    // needs the Windows one's runner too, or that entry cannot launch once downloaded — but it gates (and adopts) by
    // the launch's own chain, so an alternative entry's unshared runner never blocks the game. Teeth: resolve the
    // default entry only for the fetch; every entry for the gate.
    void runner_chain_is_every_entrys()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        writeJson(dir.path() + "/game.json",
                  NodeFixture::Chain("game", {NodeFixture::Tile("g"), NodeFixture::Exec(ManifestModel::MachinePlatform(), "g"),
                                              NodeFixture::Exec("win32", "g.exe", "Windows")}));
        writeJson(dir.path() + "/wine.json",
                  NodeFixture::Chain("wine", {NodeFixture::Runner(ManifestModel::MachinePlatform(), {"win32"}, "x")},
                                     {"winebuild"}));
        writeJson(dir.path() + "/winebuild.json",
                  NodeFixture::Chain("winebuild", {NodeFixture::ContentCid("zip", "wine.zip", "CID_WINE")}));
        NodeIndex idx; ManifestModel::ScanBundleNodes(dir.path().toStdString(), idx); ManifestModel::DeriveFacts(idx);
        const auto Every = PackageCatalog::RunnerChainIds(idx, "game", json::object(), true);
        QVERIFY(std::find(Every.begin(), Every.end(), "wine") != Every.end());
        //What the download gates on is the launch's own chain: Play runs natively, so nothing blocks on wine.
        const auto Launch = PackageCatalog::RunnerChainIds(idx, "game", json::object());
        QVERIFY(std::find(Launch.begin(), Launch.end(), "wine") == Launch.end());
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

    // The editor's "New package": a game's folder is "[uid][v1.0] Title" in a library that exists, named so every
    // system can hold it, and never a second folder for a game the library already has (in ANY library: the UID is the
    // game's identity). Teeth: drop the duplicate check, the digits check, the library check, or the sanitizer.
    void a_new_package_gets_one_folder_named_by_its_game()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString()}}}}}};
        QDir().mkpath(Root.path() + "/VidyaGod");
        QDir().mkpath(Root.path() + "/Mods/[123][v1.0] Taken");
        writeFile(Root.path() + "/notes.txt");                  // a file beside the libraries is not one
        QCOMPARE(PackageCatalog::LibraryNames(Cfg), (std::vector<std::string>{"Mods", "VidyaGod"}));

        std::string Err;
        const auto Dir = PackageCatalog::CreatePackageFolder(Cfg, "VidyaGod", "9940", "Black & White 2: Gods?", Err);
        QVERIFY2(!Dir.empty(), Err.c_str());
        QCOMPARE(Dir.filename().string(), std::string("[9940][v1.0] Black & White 2 - Gods_"));
        QVERIFY(std::filesystem::is_directory(Dir));
        QCOMPARE(Dir.parent_path().filename().string(), std::string("VidyaGod"));

        //A UID that prefixes another's is a different game; the same UID in another library is not.
        QVERIFY2(!PackageCatalog::CreatePackageFolder(Cfg, "VidyaGod", "12", "Twelve", Err).empty(), Err.c_str());
        Err.clear();
        QVERIFY(PackageCatalog::CreatePackageFolder(Cfg, "VidyaGod", "123", "Again", Err).empty());
        QVERIFY2(Err.find("Mods/[123]") != std::string::npos, Err.c_str());
        QVERIFY(!QDir(Root.path() + "/VidyaGod/[123][v1.0] Again").exists());

        for (const auto & [Lib, Uid, Title] : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"NoSuchLibrary", "7", "Game"}, {"..", "7", "Game"}, {"VidyaGod", "", "Game"}, {"VidyaGod", "7a", "Game"},
                 {"VidyaGod", "../7", "Game"}, {"VidyaGod", "7", " .. "}, {"VidyaGod", "7", ""}})
        {
            Err.clear();
            QVERIFY2(PackageCatalog::CreatePackageFolder(Cfg, Lib, Uid, Title, Err).empty(), (Lib + "|" + Uid + "|" + Title).c_str());
            QVERIFY(!Err.empty());
        }
        QCOMPARE(QDir(Root.path() + "/VidyaGod").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);   // nothing else made
        QVERIFY(!QDir(Root.path() + "/NoSuchLibrary").exists());
    }

    // A library package (files games graft: a runtime's DLLs) is named by its name, with no game ID, in a library that
    // exists, and never over a folder that library already has - each refusal saying why. Teeth: tag it like a game;
    // drop the exists check, the library check, or the empty-name check.
    void a_new_library_package_gets_a_folder_named_by_its_name()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString()}}}}}};
        QDir().mkpath(Root.path() + "/VidyaGodLibraries/corefonts");
        std::string Err;
        const auto Dir = PackageCatalog::CreateLibraryFolder(Cfg, "VidyaGodLibraries", "mfc42: VC6", Err);
        QVERIFY2(!Dir.empty(), Err.c_str());
        QCOMPARE(Dir.filename().string(), std::string("mfc42 - VC6"));
        QCOMPARE(Dir.parent_path().filename().string(), std::string("VidyaGodLibraries"));
        QVERIFY(std::filesystem::is_directory(Dir));
        //Each refusal says why (the folder could not be made either way; the author needs the reason).
        for (const auto & [Lib, Name, Why] : std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"VidyaGodLibraries", "corefonts", "already has"}, {"NoSuchLibrary", "x", "no library named"},
                 {"VidyaGodLibraries", "  ", "needs a name"}, {"VidyaGodLibraries", "..", "needs a name"}})
        {
            Err.clear();
            QVERIFY2(PackageCatalog::CreateLibraryFolder(Cfg, Lib, Name, Err).empty(), (Lib + "|" + Name).c_str());
            QVERIFY2(Err.find(Why) != std::string::npos, Err.c_str());
        }
        QCOMPARE(QDir(Root.path() + "/VidyaGodLibraries").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);
        QVERIFY(!QDir(Root.path() + "/NoSuchLibrary").exists());
    }

    // The editor's Open lists the packages themselves: every library's package folders by title (the [tags] off),
    // and the local packages outside LIBRARY, in title order whatever the case; a stray file is not a package.
    // Teeth: keep the tags; leave out the local packages; sort case-sensitively.
    void the_editor_opens_packages_by_title()
    {
        QTemporaryDir Root, Outside; QVERIFY(Root.isValid() && Outside.isValid());
        const json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root.path().toStdString()}}}, {"Repositories", json::array()}}},
                              {"LIBRARY", json::array({ json{{"PACKAGEUID", "5"}, {"PATH", Outside.path().toStdString()}} })}};
        QDir().mkpath(Root.path() + "/VidyaGod/[749][v1.0] age of Empires II");
        QDir().mkpath(Root.path() + "/VidyaGod/[9940][v1.0] Black & White 2");
        //A title the folder could not hold, from the tile the folder is named for (not another tile of the package).
        const QString Bg = Root.path() + "/VidyaGod/[2190][v1.0] Baldur_s Gate";
        QDir().mkpath(Bg);
        writeFile(Bg + "/a.json", json{{"LAYERS", {{{"EXEC", {{{"LABEL", "Play"}, {"TILE", {{"UID", "99"}, {"TITLE", "Tales of the Sword Coast"}}}}}}}}}}.dump());
        writeFile(Bg + "/b.json", json{{"LAYERS", {{{"EXEC", {{{"LABEL", "Play"}, {"TILE", {{"UID", "2190"}, {"TITLE", "Baldur's Gate"}}}}}}}}}}.dump());
        QDir().mkpath(Root.path() + "/Runners/[1][v2] Wine");
        writeFile(Root.path() + "/VidyaGod/readme.txt");
        const auto P = PackageCatalog::AuthoredPackages(Cfg);
        std::vector<std::string> Titles, Libs;
        for (const auto &A : P) { Titles.push_back(A.Title); Libs.push_back(A.Library); }
        const std::string Local = PackageCatalog::FolderTitle(Outside.path().toStdString());
        std::vector<std::string> Want{"age of Empires II", "Baldur's Gate", "Black & White 2", "Wine", Local};
        auto Lower = [](std::string T) { for (char &C : T) C = (char)std::tolower((unsigned char)C); return T; };
        std::sort(Want.begin(), Want.end(), [&](const std::string &A, const std::string &B) { return Lower(A) < Lower(B); });
        QCOMPARE(Titles, Want);
        for (const auto &A : P)
            if (A.Title == "Wine") QCOMPARE(A.Library, std::string("Runners"));
            else if (A.Title == Local) { QCOMPARE(A.Library, std::string()); QCOMPARE(A.Dir, std::filesystem::path(Outside.path().toStdString())); }
        QCOMPARE(PackageCatalog::FolderTitle("/x/[1][v1] [beta] Odd"), std::string("Odd"));
        QCOMPARE(PackageCatalog::FolderTitle("/x/[1]"), std::string("[1]"));        // nothing but tags: the name as is
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

        QVERIFY(PackageCatalog::AddPackageSource(cfg, "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG", "src1"));
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG", "again"));   // dedup on CID
        QVERIFY(!PackageCatalog::AddPackageSource(cfg, "", "empty"));        // empty CID rejected
        QCOMPARE((int)cfg["Settings"]["PackageSources"].size(), 1);

        // Simulate a fetched dir + a LIBRARY entry under it, then remove.
        const QString dir = data.path() + "/LIBRARY/src1";
        QDir().mkpath(dir);
        { std::ofstream f((dir + "/.vgsource").toStdString()); f << "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG\npkg\n"; }       // its own fetch made it
        cfg["LIBRARY"] = json::array({ json{{"PACKAGEUID", "z"}, {"PATH", (dir + "/pkg").toStdString()}, {"CIDSOURCE", "QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG"}} });

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
        // A second node file of the package: its content lives beside it, in the package dir.
        { std::ofstream f((bundle + "/extra.zip").toStdString()); f << "more"; }
        writeJson(bundle + "/landed.json", NodeFixture::Chain("landed", {NodeFixture::ContentCid("zip", "extra.zip", "QmExtraCID")}));

        const auto all = PackageCatalog::SeedTargets(d.path().toStdString(), /*CoversOnly=*/false);
        QCOMPARE(all.at((bundle + "/game.zip").toStdString()), std::string("QmLayerCID"));
        QCOMPARE(all.at((bundle + "/cover.png").toStdString()), std::string("QmCoverCID"));   // the bug: cover was missed
        QCOMPARE(all.at((bundle + "/extra.zip").toStdString()), std::string("QmExtraCID"));   // beside its node file
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
            if (M.find("Seeded") != std::string::npos) Walked = QString::fromStdString(M); });
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

    // Positions are this machine's view of a package (GlobalConfig), never its content: publishing writes none into
    // the node files it re-mints.
    void publishingDoesNotWritePositionsIntoNodeFiles()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        json A; A["LABEL"] = "a"; A["LAYERS"] = json::array();
        writeJson(Dir.filePath("a.json"), A);
        std::string Err;
        (void)PackageCatalog::PublishPackage(Dir.path().toStdString(), std::string(), &Err);
        int Nodes = 0;
        for (const QString &F : QDir(Dir.path()).entryList({"*.json"}, QDir::Files))
        {
            std::ifstream In(Dir.filePath(F).toStdString());
            json J; In >> J;
            ++Nodes;
            QVERIFY2(!J.contains("POS"), "PublishPackage stamped a layout into a node");
        }
        QCOMPARE(Nodes, 1);
    }

    // Settings → Installed Packages lists, from the index, the packages installed here: a LIBRARY package whose content
    // is on disk — with its UID, title, versions and size — never one whose content is still to download, never a
    // peer's CATALOG stub. A package is named by its identity UID or by the UID its folder was made under.
    // Teeth: drop the hydration check and "[2] Remote" (and the stub, never "on disk" here) is listed; drop the
    // folder-name match and UID 20 names nothing; drop the size walk and Bytes is 0.
    void installed_packages_come_from_the_index()
    {
        QTemporaryDir Data; QVERIFY(Data.isValid());
        const json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Data.path() + "/LIBRARY").toStdString()}}}}}};
        const auto Package = [&](const QString &Dir, const std::string &Id, const std::string &Uid, bool Here, bool Cid) {
            QDir().mkpath(Dir);
            writeJson(Dir + "/" + QString::fromStdString(Id) + ".json",
                      NodeFixture::Chain(Id, {NodeFixture::Tile(Uid, Id + " Game"), NodeFixture::Exec("win32", "g.exe"),
                                              Cid ? NodeFixture::ContentCid("zip", "game.zip", "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku")
                                                  : NodeFixture::Content("zip", "game.zip")},
                                         {}, {{"VARIANT", "Edition 1"}}));
            if (Here) { std::ofstream Z((Dir + "/game.zip").toStdString(), std::ios::binary); Z << std::string(4096, 'z'); }
        };
        Package(Data.path() + "/LIBRARY/VidyaGod/[1] Here", "here", "1", true, false);
        Package(Data.path() + "/LIBRARY/VidyaGod/[2] Remote", "remote", "2", false, true);   // to download: not installed
        Package(Data.path() + "/LIBRARY/VidyaGod/[20] Renamed", "renamed", "21", true, false); // its tile moved to UID 21
        Package(Data.path() + "/CATALOG/Peer - VidyaGod/[3] Stub", "stub", "3", true, false);

        const NodeIndex Idx = PackageCatalog::ScanCatalogIndex(Cfg);
        const auto P = PackageCatalog::InstalledPackages(Cfg, Idx);
        std::vector<std::string> Uids;
        for (const auto &X : P) Uids.push_back(X.Uid);
        QCOMPARE(Uids, (std::vector<std::string>{"1", "21"}));
        QCOMPARE(P[0].Title, std::string("here Game"));
        QCOMPARE(P[0].Library, std::string("VidyaGod"));
        QVERIFY(!P[0].Local);
        QCOMPARE(P[0].Variants, (std::vector<std::string>{"Edition 1"}));
        QVERIFY2(P[0].Bytes >= 4096, "its size counts its content");
        QCOMPARE(PackageCatalog::PackageDirsWithUid(Idx, "20").size(), size_t(1));   // the folder's UID
        QCOMPARE(PackageCatalog::PackageDirsWithUid(Idx, "21").size(), size_t(1));   // its identity
        QVERIFY(PackageCatalog::PackageDirsWithUid(Idx, "4").empty());
    }

    // THE INDEX CACHE: a build whose node files are unchanged loads the cached index — identical to a scan, field by
    // field, and saying again what the scan logged — and any edit to a node file (or a corrupt cache) builds afresh.
    // Teeth: drop the files' sizes and times from the cache's inputs and the edited LABEL is never seen; drop the log
    // replay and the malformed node's warning goes quiet on a hit; drop a field from NodeFromJson and the compare fails.
    void the_index_cache_is_exact_and_notices_every_edit()
    {
        QTemporaryDir Data; QVERIFY(Data.isValid());
        const std::string Saved = AppPaths::DataRootSet() ? AppPaths::DataRoot().string() : std::string();   // "" = unset
        const auto Restore = qScopeGuard([&] { AppPaths::SetDataRoot(Saved); });
        AppPaths::SetDataRoot(Data.path().toStdString());
        const json Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", (Data.path() + "/LIBRARY").toStdString()}}}}}};
        const QString Pkg = Data.path() + "/LIBRARY/VidyaGod/[1] Game";
        QDir().mkpath(Pkg);
        writeJson(Pkg + "/tile.json", NodeFixture::Chain("tile", {NodeFixture::Tile("1", "Game")}));
        writeJson(Pkg + "/game.json", NodeFixture::Chain("game", {NodeFixture::Exec("win32", "g.exe")}, {"tile"},
                                                         {{"VARIANT", "Edition 1"}, {"RECOMMENDED", json::array({"1"})}}));
        writeJson(Pkg + "/bad.json", json{{"LABEL", "bad"}, {"LAYERS", json::array({ json{{"ZIP", "a.zip"}, {"FILE", "b"}} })}});
        //Node files written just now are too fresh to cache (a rewrite within the same clock tick could look the same):
        //a build over them scans and keeps nothing. Teeth: drop the freshness guard and a cache appears here.
        QVERIFY(!PackageCatalog::BuildCatalogIndex(Cfg).Nodes.empty());
        QVERIFY2(!PackageCatalog::IndexCacheStatus(Cfg).Exists, "a build over node files written this second is not kept");
        //Age them past it, as any library is by the time it is opened again.
        const auto Age = [&] {
            for (const auto &E : std::filesystem::directory_iterator(Pkg.toStdString()))
                std::filesystem::last_write_time(E.path(), std::filesystem::file_time_type::clock::now() - std::chrono::minutes(1));
        };
        Age();

        const auto Same = [](const NodeIndex &A, const NodeIndex &B) {
            if (A.Nodes.size() != B.Nodes.size()) return false;
            for (const auto &[K, N] : A.Nodes)
            {
                const Node *M = B.Find(K);
                if (!M || ManifestModel::NodeToJson(N) != ManifestModel::NodeToJson(*M) || N.LoweredLayers() != M->LoweredLayers()) return false;
            }
            return ManifestModel::IndexTablesToJson(A) == ManifestModel::IndexTablesToJson(B);
        };
        const auto WarnsMalformed = [](const Diagnostics::Report &R) {
            for (const auto &E : R.Entries) if (E.Context == "ManifestModel::ParseNode") return true;
            return false;
        };

        Diagnostics::Begin();
        const NodeIndex Fresh = PackageCatalog::BuildCatalogIndex(Cfg);
        QVERIFY2(WarnsMalformed(Diagnostics::Take()), "the scan warns about the malformed node");
        QVERIFY(PackageCatalog::IndexCacheStatus(Cfg).Exists);
        QVERIFY(PackageCatalog::IndexCacheStatus(Cfg).Current);

        Diagnostics::Begin();
        const NodeIndex Cached = PackageCatalog::BuildCatalogIndex(Cfg);
        QVERIFY2(WarnsMalformed(Diagnostics::Take()), "a cache hit says again what the scan said");
        QVERIFY2(Same(Fresh, Cached), "the cached index is the scanned one, field by field");
        QVERIFY(Same(PackageCatalog::ScanCatalogIndex(Cfg), Cached));

        // An edit that keeps the size and puts the modification time back (cp -a, rsync, unzip, a clock too coarse to
        // tell): still seen — the change time cannot be set back. Teeth: stamp files by size and mtime only and the
        // cache serves the old LABEL.
        {
            const std::filesystem::path Game = (Pkg + "/game.json").toStdString();
            const auto Then = std::filesystem::last_write_time(Game);
            const auto Size = std::filesystem::file_size(Game);
            json J; { std::ifstream In(Game); In >> J; }
            QCOMPARE(J["LABEL"].get<std::string>(), std::string("game"));
            J["LABEL"] = "gamf";                                   // same length: the file keeps its size
            writeJson(Pkg + "/game.json", J);
            QCOMPARE(std::filesystem::file_size(Game), Size);
            std::filesystem::last_write_time(Game, Then);
            QVERIFY2(!PackageCatalog::IndexCacheStatus(Cfg).Current, "a same-size edit with its time put back is still an edit");
            bool Seen = false;
            for (const auto &[K, N] : PackageCatalog::BuildCatalogIndex(Cfg).Nodes) Seen = Seen || N.NodeId == "gamf";
            QVERIFY(Seen);
        }

        // An edit: another LABEL (and another size, so the check cannot hinge on the clock's resolution alone).
        writeJson(Pkg + "/game.json", NodeFixture::Chain("game renamed", {NodeFixture::Exec("win32", "g.exe")}, {"tile"},
                                                         {{"VARIANT", "Edition 1"}, {"RECOMMENDED", json::array({"1"})}}));
        Age();
        QVERIFY(!PackageCatalog::IndexCacheStatus(Cfg).Current);
        const NodeIndex Edited = PackageCatalog::BuildCatalogIndex(Cfg);
        bool Renamed = false;
        for (const auto &[K, N] : Edited.Nodes) Renamed = Renamed || N.NodeId == "game renamed";
        QVERIFY2(Renamed, "an edited node file is read again, never served from the cache");

        // A corrupt cache is built over, never trusted.
        { std::ofstream F((Data.path() + "/CACHE/index.cbor").toStdString(), std::ios::trunc); F << "not an index"; }
        QVERIFY(Same(PackageCatalog::BuildCatalogIndex(Cfg), Edited));
        QVERIFY(PackageCatalog::IndexCacheStatus(Cfg).Current);
    }

    // THE LAST INDEX: the app's window starts from the index the last run kept, whatever changed since, and from what of
    // it was on disk then — no walk of the library on the GUI thread before it shows — and a check off that thread then
    // swaps in what changed. A command's model reads the library as it is. Teeth: LastCatalogIndex asking for current
    // inputs serves the renamed node at once; refreshFromDisk not swapping never shows it; the hydration not loaded
    // from its cache shows no sentinel; FromLastIndex ignored makes the command's model stale.
    void the_app_starts_from_the_last_index_and_checks_the_disk_after()
    {
        QTemporaryDir Data; QVERIFY(Data.isValid());
        const std::string Saved = AppPaths::DataRootSet() ? AppPaths::DataRoot().string() : std::string();   // "" = unset
        const auto Restore = qScopeGuard([&] { AppPaths::SetDataRoot(Saved); });
        AppPaths::SetDataRoot(Data.path().toStdString());
        json Config = json{{"Settings", {{"Paths", {{"LibraryRoot", (Data.path() + "/LIBRARY").toStdString()}}}}}};
        QDir Dir(Data.path());
        const QString Pkg = Data.path() + "/LIBRARY/VidyaGod/[1] Game";
        QDir().mkpath(Pkg);
        const auto Write = [&](const std::string &Label) {
            writeJson(Pkg + "/tile.json", NodeFixture::Chain("tile", {NodeFixture::Tile("1", "Game")}));
            writeJson(Pkg + "/game.json", NodeFixture::Chain(Label, {NodeFixture::Exec("win32", "g.exe")}, {"tile"},
                                                             {{"VARIANT", "Edition 1"}, {"RECOMMENDED", json::array({"1"})}}));
            for (const auto &E : std::filesystem::directory_iterator(Pkg.toStdString()))   // past the cache's freshness guard
                std::filesystem::last_write_time(E.path(), std::filesystem::file_time_type::clock::now() - std::chrono::minutes(1));
        };
        const auto Has = [](const AppModel &M, const std::string &Label) {
            for (const auto &[K, N] : M.catalogIndex().Nodes) if (N.NodeId == Label) return true;
            return false;
        };
        Write("game");
        QVERIFY(!PackageCatalog::BuildCatalogIndex(Config).Nodes.empty());   // the index a run keeps
        QVERIFY(PackageCatalog::IndexCacheStatus(Config).Current);
        QVERIFY(!PackageCatalog::LastHydration());

        Write("game renamed");
        //What was on disk, as the last run kept it — with a node no index has, so where the start's hydration came from shows.
        PackageCatalog::SaveLastHydration({{"sentinel", Installed::NodeHydration{}}});
        AppModel App(&Config, &Dir, nullptr, true);
        QVERIFY2(Has(App, "game") && !Has(App, "game renamed"), "the start shows the last index, not a walk of the library");
        QVERIFY2(App.hydration().count("sentinel"), "and what was on disk then, not a stat of every content file");
        QSignalSpy Checked(&App, &AppModel::diskChecked);
        QVERIFY(Checked.wait(30000));
        QCOMPARE(Checked.first().at(0).toBool(), true);
        QVERIFY2(Has(App, "game renamed") && !Has(App, "game"), "the check swaps in the library as it is");
        QVERIFY(!App.hydration().count("sentinel"));
        QVERIFY(App.hydration().count(App.catalogIndex().Nodes.begin()->first));
        const auto Kept = PackageCatalog::LastHydration();
        QVERIFY2(Kept && Kept->size() == App.hydration().size() && !Kept->count("sentinel"), "the check keeps what it found for the next start");

        //A check with nothing changed changes nothing.
        Checked.clear();
        App.refreshFromDisk();
        QVERIFY(Checked.wait(30000));
        QCOMPARE(Checked.first().at(0).toBool(), false);

        //Checks asked together (a save at every edit of a package being authored) read the library once at a time: one
        //asked meanwhile reads it once more after, and the last one alone says it is done — with what changed in
        //between. Teeth: drop the Refreshing gate (five reads, five answers); drop the read again (the change made
        //during the first read is never seen).
        Checked.clear();
        const int Reads = App.refreshes();
        App.refreshFromDisk();
        Write("game edited");
        for (int I = 0; I < 4; ++I) App.refreshFromDisk();
        QVERIFY(Checked.wait(30000));
        QTest::qWait(300);
        QCOMPARE(Checked.count(), 1);
        QCOMPARE(App.refreshes() - Reads, 2);
        QVERIFY2(Has(App, "game edited"), "the read after shows what changed during the first");

        Write("game again");
        AppModel Command(&Config, &Dir);
        QVERIFY2(Has(Command, "game again"), "a command's model reads the library as it is now");
    }

    // A check that throws (a path the filesystem refuses: a 300-character file name) ends the validation with that
    // error, as one pass over the nodes always did — never a terminate from a worker thread taking the app down. Two
    // packages, so the packages run on two threads. Teeth: drop the catch in Parallel::Run and the test binary aborts.
    void a_throwing_check_is_an_error_not_a_crash()
    {
        QTemporaryDir Lib; QVERIFY(Lib.isValid());
        const QString A = Lib.path() + "/VidyaGod/[1] One", B = Lib.path() + "/VidyaGod/[2] Two";
        QDir().mkpath(A); QDir().mkpath(B);
        writeJson(A + "/one.json", NodeFixture::Chain("one", {NodeFixture::Tile("1", "One"), NodeFixture::Exec("win32", "a.exe")}, {}, {{"VARIANT", "v"}}));
        writeJson(B + "/two.json", NodeFixture::Chain("two", {NodeFixture::Tile("2", "Two"), NodeFixture::Exec("win32", "b.exe"),
                                                              NodeFixture::Content("zip", std::string(300, 'z') + ".zip")}, {}, {{"VARIANT", "v"}}));
        NodeIndex Idx;
        ManifestModel::ScanBundleNodes(A.toStdString(), Idx);
        ManifestModel::ScanBundleNodes(B.toStdString(), Idx);
        ManifestModel::DeriveFacts(Idx);
        std::vector<std::string> Errors, Warnings;
        bool Threw = false;
        try { Validate::Nodes(Idx, Errors, Warnings); }
        catch (const std::exception &) { Threw = true; }
        QVERIFY2(Threw, "the filesystem's refusal reaches the caller as an error");
    }

private slots:
    // What uninstalling would do is said before anything is touched: a package of LIBRARY that nothing could download again
    // is the only copy; one a source of the user's has comes back from there; one added from outside is forgotten. A peer
    // listing it — even every node of this very version — is said, but stays the only copy: a listing names node files,
    // never whether the peer holds the content (two machines each deleting theirs "from the other" lost both). Another
    // installed game whose content lives in its folder is named, and a package the user shares says so. Teeth: a peer's
    // listing taken for a copy (Download); a listing of another version named; the source's name not looked up; a package
    // needing nothing named; the share sheet not read; an outside package not forgotten.
    void uninstall_says_whether_a_package_can_be_downloaded_again()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const QString R = D.path() + "/LIBRARY";
        const QString Pkg = R + "/Mine/[1] Game", Other = R + "/Mine/[2] Other";
        QDir().mkpath(Pkg); QDir().mkpath(Other);
        { std::ofstream f((Pkg + "/game.zip").toStdString()); f << "content"; }
        NodeFixture::WriteNodes((Pkg + "/game.json").toStdString(), NodeFixture::Chain("game", {NodeFixture::Tile("1", "Game"),
            NodeFixture::Exec("win32", "g.exe"), NodeFixture::ContentCid("zip", "game.zip", "bafkreigamezip")}, {}, {{"VARIANT", "Edition 1"}}));
        NodeFixture::WriteNodes((Other + "/other.json").toStdString(), NodeFixture::Chain("other", {NodeFixture::Tile("2", "Other"),
            NodeFixture::Exec("win32", "o.exe")}, {}, {{"VARIANT", "Edition 1"}}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        using K = PackageCatalog::UninstallVerdict::Kind;
        auto Judge = [&](const QString &Dir) { return PackageCatalog::JudgeUninstall(cfg, *PackageCatalog::FreshCatalogIndex(cfg), Dir.toStdString()); };
        PackageCatalog::UninstallVerdict V = Judge(Pkg);
        QVERIFY(V.K == K::OnlyCopy);
        QVERIFY(V.NeededBy.empty() && !V.Shared && V.From.empty());

        // a peer sharing another version (one node of it): still the only copy; every node: downloadable from them
        const std::string Peer = "12D3KooWPeerPeerPeerPeer";
        cfg["PeerLabels"] = json{{Peer, "Deck"}};
        std::vector<std::string> Cids;
        for (const auto &[C, F] : NodeFiles::In(Pkg.toStdString())) Cids.push_back(C);
        QVERIFY(Cids.size() >= 2);
        auto Listing = [&](size_t N) {
            json L = json::array();
            for (size_t I = 0; I < N; ++I) L.push_back(json{{"cid", Cids[I]}, {"pkg", "[1] Game"}, {"node", Cids[I] + ".json"}});
            return json{{"Mine", L}};
        };
        cfg["PeerLibraries"] = json{{Peer, Listing(Cids.size() - 1)}};
        V = Judge(Pkg);
        QVERIFY(V.K == K::OnlyCopy);
        QVERIFY(V.ListedBy.empty());
        cfg["PeerLibraries"] = json{{Peer, Listing(Cids.size())}};
        V = Judge(Pkg);
        QVERIFY(V.K == K::OnlyCopy);
        QVERIFY(V.From.empty());
        QCOMPARE(QString::fromStdString(V.ListedBy), QString("Deck"));
        cfg.erase("PeerLibraries");

        // a source of the user's: named by its name
        cfg["LIBRARY"] = json::array({ json{{"PATH", Pkg.toStdString()}, {"CIDSOURCE", "bafysource"}} });
        cfg["Settings"]["PackageSources"] = json::array({ json{{"CID", "bafysource"}, {"NAME", "Shop"}} });
        V = Judge(Pkg);
        QVERIFY(V.K == K::Download);
        QCOMPARE(QString::fromStdString(V.From), QString("the source 'Shop'"));
        cfg.erase("LIBRARY");
        cfg["Settings"].erase("PackageSources");

        // another game containing it: its content lives here, so it is named; and a package the user shares says so
        NodeFixture::WriteNodes((Other + "/other.json").toStdString(), NodeFixture::Chain("other", {NodeFixture::Tile("2", "Other"),
            NodeFixture::Exec("win32", "o.exe")}, {"game"}, {{"VARIANT", "Edition 1"}}));
        cfg["ShareSheet"] = json{{Cids.front(), "Mine/[1] Game/" + Cids.front() + ".json"}};
        V = Judge(Pkg);
        QVERIFY(V.K == K::OnlyCopy);
        QCOMPARE((int)V.NeededBy.size(), 1);
        QCOMPARE(QString::fromStdString(V.NeededBy.front()), QString("Other"));
        QVERIFY(V.Shared);
        QVERIFY(Judge(Other).NeededBy.empty());            // nothing of Game's lives in Other's folder

        // a library's folder (not a package): nothing to do, whatever is asked — never the only copy to trash
        QVERIFY(Judge(R + "/Mine").K == K::Nothing);
        // added from outside LIBRARY: forgotten; a folder nothing names: nothing to do
        QTemporaryDir Ext; QVERIFY(Ext.isValid());
        QVERIFY(Judge(Ext.path()).K == K::Nothing);
        cfg["LIBRARY"] = json::array({ json{{"PATH", Ext.path().toStdString()}, {"PACKAGEUID", "ext"}} });
        QVERIFY(Judge(Ext.path()).K == K::Forget);
    }

    // Whatever reaches into a package's folder is named before it goes: a package containing a node of it that has no
    // content (a library's tile, a runner), and a mod — a graft row, in a package with no launchable of its own — over
    // the game in it. Teeth: the content walk alone (the content-less node unseen); grafts or graft-only packages skipped.
    void uninstall_names_every_package_reaching_into_the_folder()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const QString R = D.path() + "/LIBRARY";
        const QString Game = R + "/Mine/[1] Game", Mod = R + "/Mine/[3] Mod", Lib = R + "/Mine/[4] Lib", User = R + "/Mine/[5] User";
        for (const QString &P : { Game, Mod, Lib, User }) QDir().mkpath(P);
        { std::ofstream f((Game + "/game.zip").toStdString()); f << "content"; }
        { std::ofstream f((Mod + "/mod.zip").toStdString()); f << "mod"; }
        { std::ofstream f((User + "/user.zip").toStdString()); f << "user"; }
        using namespace NodeFixture;
        WriteNodes((Game + "/game.json").toStdString(), Chain("game", {Tile("1", "Game"), Exec("win32", "g.exe"),
            Content("zip", "game.zip")}, {}, {{"VARIANT", "Edition 1"}}));
        WriteNodes((Mod + "/mod.json").toStdString(), Chain("mod", {Merge({ Layers({ json{{"ANY", json::array({"game"})}} }),
            Tile("3", "Mod"), Content("zip", "mod.zip") })}, {}, {{"VARIANT", "Mod"}}));
        WriteNodes((Lib + "/lib.json").toStdString(), Chain("lib", {Tile("4", "Lib"), Exec("win32", "l.exe")}, {}, {{"VARIANT", "Lib"}}));
        WriteNodes((User + "/user.json").toStdString(), Chain("user", {Tile("5", "User"), Exec("win32", "u.exe"),
            Content("zip", "user.zip")}, {"lib"}, {{"VARIANT", "Edition 1"}}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        const auto Idx = PackageCatalog::FreshCatalogIndex(cfg);
        QVERIFY(Idx->Find("mod") && Idx->Find("mod")->IsGraft);
        auto Needing = [&](const QString &Dir) {
            QStringList L;
            for (const std::string &N : PackageCatalog::JudgeUninstall(cfg, *Idx, Dir.toStdString()).NeededBy) L << QString::fromStdString(N);
            return L;
        };
        QCOMPARE(Needing(Lib), QStringList{"User"});
        QCOMPARE(Needing(Game), QStringList{"Mod"});
        QVERIFY(Needing(User).isEmpty());
        QVERIFY(Needing(Mod).isEmpty());
    }

    // The only copy, trashed, lets go of its own CIDs only: a file of the same bytes another package keeps in its own folder
    // is that CID too, and dropping it stopped that package serving (no longer held whole). And a link in the library
    // naming a package trashes the package, not the link — the CIDs let go are the folder's. Teeth: every CID of the
    // package dropped; the link moved to the trash (the package stays, its CIDs dropped under it).
    void trashing_a_package_keeps_what_another_package_holds_too()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const QString R = D.path() + "/LIBRARY", Game = R + "/Mine/[1] Game", Copy = R + "/Mine/[2] Copy";
        QDir().mkpath(Game); QDir().mkpath(Copy);
        const std::string Bytes(300000, 'z');
        for (const QString &P : { Game, Copy }) { std::ofstream f((P + "/same.bin").toStdString(), std::ios::binary); f << Bytes; }
        std::string Err;
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto StopNode = qScopeGuard([] { IpfsWrapper::StopNode(); });
        const std::string Cid = IpfsWrapper::AddNoCopy((Game + "/same.bin").toStdString(), &Err);
        QVERIFY2(!Cid.empty(), Err.c_str());
        QCOMPARE(IpfsWrapper::AddNoCopy((Copy + "/same.bin").toStdString(), &Err), Cid);   // the reference: the last file added
        std::vector<std::string> From;
        QVERIFY(IpfsWrapper::HeldWholeFiles(Cid, &From));
        QVERIFY2(!From.empty() && QString::fromStdString(From.front()).startsWith(Copy), From.empty() ? "" : From.front().c_str());
        using namespace NodeFixture;
        WriteNodes((Game + "/game.json").toStdString(), Chain("game", {Tile("1", "Game"), Exec("win32", "g.exe"),
            ContentCid("file", "same.bin", Cid)}, {}, {{"VARIANT", "Edition 1"}}));
        WriteNodes((Copy + "/copy.json").toStdString(), Chain("copy", {Tile("2", "Copy"), Exec("win32", "c.exe"),
            ContentCid("file", "same.bin", Cid)}, {}, {{"VARIANT", "Edition 1"}}));
        QVERIFY(QFile::link(Game, R + "/Mine/[9] Alias"));
        //One package, one lock, however it is named: a trash through the link and a version switch by the folder exclude
        //each other.
        QCOMPARE(&PackageCatalog::PackageLock((R + "/Mine/[9] Alias").toStdString()), &PackageCatalog::PackageLock(Game.toStdString()));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        const auto Idx = PackageCatalog::FreshCatalogIndex(cfg);
        QVERIFY(IpfsWrapper::HeldWhole(Cid));
        const QByteArray WasData = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", (D.path() + "/xdgdata").toUtf8());
        const auto Restore = qScopeGuard([&] { if (WasData.isEmpty()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", WasData); });
        QVERIFY2(PackageCatalog::TrashPackage(*Idx, (R + "/Mine/[9] Alias").toStdString(), &Err), Err.c_str());
        QVERIFY2(!QFileInfo::exists(Game), "the package the link names is what went to the trash");
        QVERIFY(QFileInfo::exists(Copy + "/same.bin"));
        QVERIFY2(IpfsWrapper::HeldWhole(Cid), "Copy still serves its file");
    }

    // A seed says each file it looks at, with its size, before reading it (the editor's "file n of m · bytes"), and a no
    // stops it there: the node is left as it was, the caller told "stopped". Teeth: the step not asked; a stop that
    // keeps going; a stopped node left half-recorded.
    void a_seed_says_each_file_and_stops_when_told()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const std::filesystem::path Pkg = D.path().toStdString();
        { std::ofstream f(Pkg / "a.bin"); f << std::string(1000, 'a'); }
        { std::ofstream f(Pkg / "b.bin"); f << std::string(500, 'b'); }
        json N = json{{"LABEL", "n"}, {"LAYERS", json::array({ json{{"FILE", "a.bin"}}, json{{"FILE", "b.bin"}}, json{{"DIR", "d"}} })}};
        const PackageCatalog::SeedWorkload W = PackageCatalog::SeedWork(N, Pkg);
        QCOMPARE(W.Files, 2);                       // the DIR is local: not looked at
        QCOMPARE(W.Bytes, 1500ull);
        const json Was = N;
        std::vector<std::pair<std::string, unsigned long long>> Told;
        PackageCatalog::SeedReport R;
        bool Changed = true;
        std::string Err;
        //The first file is seeded (its SOURCE written into the node), the stop comes at the second: the node goes back to
        //as it was, never half recorded.
        QTemporaryDir Repo; QVERIFY(Repo.isValid());
        QVERIFY2(IpfsWrapper::StartNode((Repo.path() + "/ipfs").toStdString(), &Err), Err.c_str());
        auto StopNode = qScopeGuard([] { IpfsWrapper::StopNode(); });
        QVERIFY(!PackageCatalog::SeedNodeContent(N, Pkg, R, Changed, &Err, [&](const std::string &F, unsigned long long B) {
            Told.push_back({ F, B });
            return Told.size() < 2;
        }));
        QCOMPARE(QString::fromStdString(Err), QString("stopped"));
        QVERIFY(!Changed);
        QCOMPARE((int)Told.size(), 2);
        QCOMPARE(QString::fromStdString(Told[0].first), QString("a.bin"));
        QCOMPARE(Told[0].second, 1000ull);
        QCOMPARE(Told[1].second, 500ull);
        QCOMPARE(R.Seeded, 1);                       // a.bin was seeded before the stop
        QCOMPARE(N, Was);
    }

    // Every installed package can be uninstalled from Settings → Installed Packages — not only the ones with a config
    // entry — and the question says what happens: what comes back from where, or that the only copy goes to the trash
    // (asked with Cancel the default). Teeth: the old `Local || HasEntry` gate (no button on a package of LIBRARY); the
    // only copy's question defaulting to yes; a refusal offered as a question; a package gone since (Nothing) asked as a
    // blank box whose yes uninstalls; a peer's listing not said.
    void every_installed_package_offers_uninstall_with_what_it_does()
    {
        QTemporaryDir D; QVERIFY(D.isValid());
        const QString R = D.path() + "/LIBRARY", Pkg = R + "/Mine/[1] Game";
        QDir().mkpath(Pkg);
        { std::ofstream f((Pkg + "/game.zip").toStdString()); f << "content"; }
        NodeFixture::WriteNodes((Pkg + "/game.json").toStdString(), NodeFixture::Chain("game", {NodeFixture::Tile("1", "Game"),
            NodeFixture::Exec("win32", "g.exe"), NodeFixture::Content("zip", "game.zip")}, {}, {{"VARIANT", "Edition 1"}}));
        json cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", R.toStdString()}}}}}};
        QDir appDir(D.path());
        AppModel model(&cfg, &appDir);
        model.rebuildCatalog();
        PackagesView View(model);
        View.show();
        QTRY_VERIFY_WITH_TIMEOUT([&]{ for (auto *B : View.findChildren<QPushButton *>()) if (B->accessibleName() == "Uninstall Game") return true;
                                      return false; }(), 20000);

        using K = PackageCatalog::UninstallVerdict::Kind;
        PackageCatalog::UninstallVerdict V;
        V.K = K::Download; V.From = "the source 'Shop'";
        auto A = PackagesView::uninstallAsk(V, "Game", 3ull << 30);
        QVERIFY(!A.Refused && !A.DefaultNo);
        QVERIFY2(A.Text.contains("download again from the source 'Shop'") && A.Text.contains("3.0 GB") && A.Text.contains("saves"), qPrintable(A.Text));
        V.NeededBy = { "Other" };
        QVERIFY(PackagesView::uninstallAsk(V, "Game", 1).Text.contains("Other uses its content too"));
        V.K = K::OnlyCopy; V.NeededBy.clear(); V.Shared = true;
        A = PackagesView::uninstallAsk(V, "Game", 1);
        QVERIFY(!A.Refused && A.DefaultNo);
        QCOMPARE(A.Confirm, QString("Move to trash"));
        QVERIFY2(A.Text.contains("only copy") && A.Text.contains("cannot be downloaded again") && A.Text.contains("trash")
                 && A.Text.contains("peers will no longer get it from you"), qPrintable(A.Text));
        V.ListedBy = "Deck";                         // a peer listing it: said, and still the only copy
        A = PackagesView::uninstallAsk(V, "Game", 1);
        QVERIFY(!A.Refused && A.DefaultNo);
        QVERIFY2(A.Text.contains("Deck lists it") && A.Text.contains("cannot be known"), qPrintable(A.Text));
        V.ListedBy.clear();
        V.NeededBy = { "Other" };
        A = PackagesView::uninstallAsk(V, "Game", 1);
        QVERIFY(A.Refused);
        QVERIFY2(A.Text.contains("Other uses its content"), qPrintable(A.Text));
        V.K = K::Forget;
        QCOMPARE(PackagesView::uninstallAsk(V, "Game", 1).Confirm, QString("Forget"));
        V = {};                                      // gone since the list was made: refused, never a blank yes
        A = PackagesView::uninstallAsk(V, "Game", 1);
        QVERIFY(A.Refused);
        QVERIFY2(A.Text.contains("no longer there"), qPrintable(A.Text));
    }

private:
    QTemporaryDir *SuiteRoot = nullptr;

};

QTEST_MAIN(PackageCatalogTest)
#include "test_packagecatalog.moc"
