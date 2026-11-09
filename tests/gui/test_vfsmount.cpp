// test_vfsmount.cpp — BuildLayerSpec: the mount plan that IS the game's filesystem.
//
// Layer ORDER is priority. Get it wrong and nothing fails: the mount succeeds, the game runs, and it silently
// reads the wrong copy of a file — a package's own content shadowing the edits meant to fix it, or an ephemeral
// write layer sitting under a durable one so the user's saves vanish at teardown. Nothing in this subsystem
// throws when the plan is wrong, so the plan itself has to be asserted.

#include <fstream>
#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "qtestjson.h"
#include "sessions.h"
#ifndef _WIN32
#include <unistd.h>
#endif

// QVERIFY_THROWS_NO_EXCEPTION arrived in Qt 6.3; CI's Ubuntu 22.04 ships 6.2.
#define VERIFY_NO_THROW(Expr)                                                                                  \
    do {                                                                                                       \
        try { Expr; }                                                                                          \
        catch (const std::exception &E) { QFAIL((std::string(#Expr " threw: ") + E.what()).c_str()); }       \
        catch (...) { QFAIL(#Expr " threw"); }                                                                 \
    } while (0)
#include "vfsmount.h"
#include "launchparams.h"
#include "commonutils.h"
#include "launchresolver.h"
#include "manifestmodel.h"

using json = nlohmann::ordered_json;

namespace {

struct Complaints
{
    std::vector<std::string> Lines;
    Complaints()  { SetLogCallback([this](LogLevel L, const std::string &C, const std::string &M){
                        if (L == LogLevel::WARN || L == LogLevel::ERR) Lines.push_back(C + "|" + M); }); }
    ~Complaints() { ClearLogCallback(); }
    bool Mentions(const std::string &Needle) const
    {
        for (const auto &L : Lines) if (L.find(Needle) != std::string::npos) return true;
        return false;
    }
};

// Targets in plan order — the assertion that actually matters.
std::vector<std::string> Targets(const json &Spec)
{
    std::vector<std::string> T;
    for (const auto &L : Spec.value("layers", json::array())) T.push_back(L.value("target", std::string()));
    return T;
}
std::vector<std::string> Types(const json &Spec)
{
    std::vector<std::string> T;
    for (const auto &L : Spec.value("layers", json::array())) T.push_back(L.value("type", std::string()));
    return T;
}
int IndexOfTarget(const json &Spec, const std::string &Target)
{
    const auto T = Targets(Spec);
    for (size_t i = 0; i < T.size(); ++i) if (T[i] == Target) return int(i);
    return -1;
}

} // namespace

class VfsMountTest : public QObject
{
    Q_OBJECT

    QTemporaryDir Root;

    // A ContainerParams with every path rooted in the temp dir, so create_directories side effects stay contained.
    ContainerParams Make()
    {
        const std::filesystem::path R = Root.path().toStdString();
        ContainerParams CP(R / "PKG");
        CP.RuntimePath     = R / "RUNTIME";
        CP.WriteLayerPath  = R / "WRITELAYER";
        CP.TempPath        = R / "TEMP";
        CP.UserDataPath    = R / "USERDATA";
        CP.PackagePath     = R / "PKG";
        CP.SubComponentsArray = json::array();
        std::filesystem::create_directories(CP.PackagePath);
        return CP;
    }

    // A VFSDirLayer whose source really exists, so the missing-source detector stays quiet unless a test wants it.
    json DirLayer(const std::string &Rel, const std::string &Target = {})
    {
        std::filesystem::create_directories(std::filesystem::path(Root.path().toStdString()) / "PKG" / Rel);
        json L{{"TYPE", "VFSDirLayer"}, {"PATH", Rel}};
        if (!Target.empty()) L["TARGET"] = Target;
        return L;
    }

    // A VFSZipLayer whose archive really exists — with Cid, the SOURCE a delta names it by.
    json ZipLayer(const std::string &Rel, const std::string &Target = {}, const std::string &Cid = {})
    {
        const std::filesystem::path P = std::filesystem::path(Root.path().toStdString()) / "PKG" / Rel;
        std::filesystem::create_directories(P.parent_path());
        std::ofstream(P, std::ios::binary) << "PK\x05\x06";
        json L{{"TYPE", "VFSZipLayer"}, {"PATH", Rel}};
        if (!Target.empty()) L["TARGET"] = Target;
        if (!Cid.empty()) L["SOURCE"] = {{"TYPE", "ipfs"}, {"CID", Cid}};
        return L;
    }

    // A VFSDeltaLayer whose .vgdelta really exists, so the missing-source detector stays quiet. `Bases` is its
    // BASE_SOURCES (null: none), `Cid` its own SOURCE.
    json DeltaLayer(const std::string &Rel, const std::string &Target, const json &Bases, const std::string &Cid = {})
    {
        const std::filesystem::path P = std::filesystem::path(Root.path().toStdString()) / "PKG" / Rel;
        std::filesystem::create_directories(P.parent_path());
        std::ofstream(P, std::ios::binary) << "vgdelta";
        json L{{"TYPE", "VFSDeltaLayer"}, {"PATH", Rel}, {"TARGET", Target}};
        if (!Bases.is_null()) L["BASE_SOURCES"] = Bases;
        if (!Cid.empty()) L["SOURCE"] = {{"TYPE", "ipfs"}, {"CID", Cid}};
        return L;
    }

private slots:

    // ---- what gets into the plan at all ----

    // A session folder is live while its launch holds <folder>.lock (beside it: clearing the folder never removes the lock;
    // the kernel drops it when that process dies, however it dies). The startup sweep clears every folder whose lock
    // nobody holds, a lock file left without its folder, and the empty package folder; a live one is left alone, and a
    // claim made while it is held fails. A release removes the lock file. Teeth: SessionOwned always false (the live
    // folder is swept); Sweep ignoring the lock (same); Sweep skipping a released lock (the dead folder stays); the lock
    // inside the folder (the live claim's lock is deleted with a clear, and a second claim succeeds); no package removal.
    void the_startup_sweep_clears_dead_sessions_and_leaves_live_ones()
    {
#ifdef _WIN32
        QSKIP("session ownership locks are Linux's");
#else
        QTemporaryDir Tmp;
        const std::filesystem::path Root = Tmp.path().toStdString();
        const auto Put = [](const std::filesystem::path &F) {
            std::filesystem::create_directories(F.parent_path());
            std::ofstream(F) << "x";
        };
        const std::filesystem::path Live = Root / "1" / "DefaultInstance", Dead = Root / "2" / "DefaultInstance",
                                    Old = Root / "3" / "vgtest", Stray = Root / "4" / "gone";
        int LiveFd = Sessions::ClaimSession(Live);
        QVERIFY(LiveFd >= 0);
        Put(Live / "WRITELAYER" / "save.dat");
        QVERIFY(Sessions::SessionOwned(Live));
        QCOMPARE(Sessions::ClaimSession(Live), -2);                  // a second launch of a running instance
        VfsMount::CleanStaleRuntime(Live);                           // what a refused launch's clear would have done
        QVERIFY(Sessions::SessionOwned(Live));                       // the lock is not in the folder
        QCOMPARE(Sessions::ClaimSession(Live), -2);
        Put(Live / "WRITELAYER" / "save.dat");
        int DeadFd = Sessions::ClaimSession(Dead);
        QVERIFY(DeadFd >= 0);
        Put(Dead / "WRITELAYER" / "save.dat");
        ::close(DeadFd);                                             // its launch died: the kernel let the lock go
        QVERIFY(!Sessions::SessionOwned(Dead));
        Put(Old / "HOME" / ".bashrc");                               // a session of a build before the lock
        int StrayFd = Sessions::ClaimSession(Stray);                 // a launch that died before making its folder
        ::close(StrayFd);

        QCOMPARE(VfsMount::SweepDeadSessions(Root), 2);
        QVERIFY(std::filesystem::exists(Live / "WRITELAYER" / "save.dat"));
        QVERIFY(!std::filesystem::exists(Root / "2"));
        QVERIFY(!std::filesystem::exists(Root / "3"));
        QVERIFY(!std::filesystem::exists(Root / "4"));
        QVERIFY(Sessions::SessionOwned(Live));                        // the sweep did not take the live lock

        Sessions::ReleaseSession(LiveFd, Live);
        QCOMPARE(LiveFd, -1);
        QVERIFY(!std::filesystem::exists(Root / "1" / "DefaultInstance.lock"));
        QVERIFY(!Sessions::SessionOwned(Live));
        QCOMPARE(VfsMount::SweepDeadSessions(Root), 1);
        QVERIFY(!std::filesystem::exists(Root / "1"));
#endif
    }

    void only_vfs_layers_become_mount_layers()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DirLayer("content"),
            json{{"TYPE", "RegEdit"},    {"REGPATH", "HKLM\\X"}, {"KEYVALUES", json::object()}},
            json{{"TYPE", "FileEdit"},   {"MODE", "Overwrite"},  {"FILE", "a.ini"}},
            json{{"TYPE", "CustomVar"},  {"KEY", "k"},           {"DEFAULT", "v"}},
            json{{"TYPE", "DllOverride"},{"DLL", "d3d9"},        {"MODE", "native"}},
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(Spec["layers"].size(), size_t(1));
        QVERIFY2(Types(Spec)[0] == "dir", "the one VFS layer must survive; the edit layers are not mounts");
    }

#ifndef _WIN32
    // The mount reports its files as owned by the user running it, for the game's mount and the runner's alike. It
    // was a fixed 1000: for any other uid the sandbox saw the files as `nobody`'s and the game could not write its
    // saves. Teeth (needs a uid other than 1000, as on CI): put the fixed 1000 back.
    void the_mount_is_owned_by_the_user_running_it()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content") });
        const json Game = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(Game.value("uid", 0u), static_cast<unsigned>(::getuid()));
        QCOMPARE(Game.value("gid", 0u), static_cast<unsigned>(::getgid()));
        CP.RunnerPackagePath = CP.PackagePath;
        CP.RunnerLayers = { DirLayer("runner") };
        const json Runner = VfsMount::BuildRunnerLayerSpec(CP);
        QCOMPARE(Runner.value("uid", 0u), static_cast<unsigned>(::getuid()));
        QCOMPARE(Runner.value("gid", 0u), static_cast<unsigned>(::getgid()));
    }
#endif

    void subcomponent_order_is_preserved_as_layer_priority()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DirLayer("base",  "game"),
            DirLayer("patch", "game/patch"),
            DirLayer("mod",   "game/mod"),
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        const auto T = Targets(Spec);
        QCOMPARE(T.size(), size_t(3));
        QCOMPARE(T[0], std::string("game"));
        QCOMPARE(T[1], std::string("game/patch"));
        QVERIFY2(T[2] == "game/mod", "later subcomponents must stay later in the plan — that IS their priority");
    }

    // ---- the priority ladder: content < DEFAULTDATA < KEEP < DROP ----

    // DEFAULTDATA holds the package's base Reg/File edits. Below it and the package's own content wins, and the
    // edit silently does nothing — exactly the Worms 4 failure mode, one layer down.
    void defaultdata_sits_above_the_content_layers()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        CP.DefaultDataPath = std::filesystem::path(Root.path().toStdString()) / "DEFAULTDATA";
        std::filesystem::create_directories(CP.DefaultDataPath);

        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(Spec["layers"].size(), size_t(2));
        QVERIFY2(IndexOfTarget(Spec, "game") < IndexOfTarget(Spec, ""),
                 "DEFAULTDATA (target '') must come after the content it overrides");
    }

    void a_missing_defaultdata_dir_contributes_no_layer()
    {
        ContainerParams CP = Make();
        CP.DefaultDataPath = std::filesystem::path(Root.path().toStdString()) / "NO_SUCH_DEFAULTDATA";
        QCOMPARE(VfsMount::BuildLayerSpec(CP)["layers"].size(), size_t(0));
    }

    void keep_dirs_are_durable_rw_above_the_content()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        CP.KeepDirs = { {"game/save", "save", true} };

        const json Spec = VfsMount::BuildLayerSpec(CP);
        const int K = IndexOfTarget(Spec, "game/save");
        QVERIFY2(K > IndexOfTarget(Spec, "game"), "a KEEP dir must outrank the content it persists into");
        QVERIFY2(Spec["layers"][K].value("rw", false), "a KEEP dir must be writable or nothing persists");
        const std::string Src = Spec["layers"][K].value("source", std::string());
        QVERIFY2(Src.find("USERDATA") != std::string::npos,
                 "a KEEP dir must be backed by the DURABLE UserDataPath, not by TEMP");
        QVERIFY2(Src.find("save") != std::string::npos,
                 "a KEEP dir's durable source is UserDataPath/<Target>, a NAMED sibling of the instance config");
    }

    // ---- the writable branch ----

    void the_write_layer_is_ephemeral_by_default()
    {
        ContainerParams CP = Make();
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY(!Spec["writelayer"].is_null());
        QVERIFY2(Spec.value("writelayer", std::string()).find("WRITELAYER") != std::string::npos,
                 "pristine-by-default: writes go to the ephemeral branch");
        QCOMPARE(Spec.value("readonly", true), false);
    }

    // A whole-runtime persist (PATH "") is no longer a magic "make the write branch durable" flag — it is a plain
    // KEEP dir whose runtime <Path> is the ROOT (target "") and whose durable source is UserDataPath/<Target>, a
    // NAMED sibling of the instance config. The write layer stays ephemeral (so instance.json never lands in a
    // game-writable mount); the root persist rides above it as an ordinary durable RW passthrough.
    void a_whole_runtime_persist_is_a_root_keep_dir_over_an_ephemeral_write_layer()
    {
        ContainerParams CP = Make();
        CP.KeepDirs = { {"", "AllData", true} };
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY2(Spec.value("writelayer", std::string()).find("WRITELAYER") != std::string::npos,
                 "the write layer stays ephemeral even under a whole-runtime persist");
        const int K = IndexOfTarget(Spec, "");
        QVERIFY2(K >= 0, "a PATH \"\" persist mounts a durable RW dir at the runtime ROOT (target \"\")");
        QVERIFY(Spec["layers"][K].value("rw", false));
        const std::string Src = Spec["layers"][K].value("source", std::string());
        QVERIFY2(Src.find("USERDATA") != std::string::npos && Src.find("AllData") != std::string::npos,
                 "its durable source is UserDataPath/<Target>, a NAMED sibling of the instance config");
    }

    void readonly_vfs_has_no_write_layer()
    {
        ContainerParams CP = Make();
        CP.ReadOnlyVFS = true;
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(Spec.value("readonly", false), true);
        QVERIFY2(Spec["writelayer"].is_null(), "a read-only runtime must not carry a writable branch");
    }

    // ---- target resolution ----

    void an_absent_target_means_the_vfs_root()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content") });
        QCOMPARE(Targets(VfsMount::BuildLayerSpec(CP))[0], std::string(""));
    }

    void targets_are_variable_substituted_and_normalized()
    {
        ContainerParams CP = Make();
        CP.PackageUID  = "UID123";
        CP.ContentRoot = "pfx/drive_c/UID123";
        CP.PrefixRoot  = "pfx";
        CP.SubComponentsArray = json::array({ DirLayer("content", "%PrefixRoot%\\drive_c\\%PackageUID%") });

        const std::string T = Targets(VfsMount::BuildLayerSpec(CP))[0];
        QCOMPARE(T, std::string("pfx/drive_c/UID123"));
    }

    // %PrefixRoot% is EMPTY under wine, so this exact template routinely produces a leading '/' and a '//' — two
    // spellings of one target, which the FS would treat as two different mount points.
    void an_empty_prefix_root_does_not_leave_stray_slashes()
    {
        ContainerParams CP = Make();
        CP.PackageUID = "UID123";
        CP.PrefixRoot = "";                                    // wine
        CP.SubComponentsArray = json::array({ DirLayer("content", "%PrefixRoot%/drive_c/%PackageUID%") });
        QCOMPARE(Targets(VfsMount::BuildLayerSpec(CP))[0], std::string("drive_c/UID123"));
    }

    // The quietest failure this subsystem can produce: the layer mounts at a directory literally named "%Foo%",
    // every file in it is invisible to the game, and the mount reports success.
    void a_surviving_token_in_a_target_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "%NoSuchVariable%/game") });

        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(Targets(Spec)[0], std::string("%NoSuchVariable%/game"));   // unchanged — the plan is honest
        QVERIFY2(C.Mentions("%token%"), "a token that survived substitution must be reported, not mounted quietly");
        QVERIFY2(C.Mentions("NoSuchVariable"), "and it must name the variable that was missing");
    }

    // A layer whose source has been deleted mounts EMPTY and succeeds. Nothing downstream can tell that apart
    // from a package that genuinely ships an empty directory.
    void a_missing_layer_source_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ json{{"TYPE", "VFSDirLayer"}, {"PATH", "gone_forever"}, {"TARGET", "game"}} });

        //The sweep belongs to the MOUNT, not to building the plan — a plan is also built by callers that never
        //mount one (--audit-packages builds 960), where a runtime-sourced layer legitimately does not exist yet.
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Lines.empty(), "building a plan must not judge the filesystem");
        QCOMPARE(VfsMount::ReportMissingSources(Spec), size_t(1));
        QVERIFY2(C.Mentions("do not exist"), "a vanished layer source must be called out before the mount");
    }

    void an_existing_source_produces_no_complaint()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Lines.empty(), "a healthy plan must be quiet, or the noise stops meaning anything");
    }

    // ---- cross-namespace nesting is gated off for every classic chain ----

    void a_classic_chain_adds_no_inner_runner_layers()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        // No RunnerChain at all — BoundaryLinkIndex is 0, so the nesting block must be a complete no-op.
        QCOMPARE(VfsMount::BuildLayerSpec(CP)["layers"].size(), size_t(1));
    }

    // ---- a delta's bases: named by CID in the node and in the FS's spec; the FS finds each file ----

private:   // a helper, not a test: moc never sees its json parameters (no metatype for json on Windows)
    // The CIDs the delta layer at `Target` names, in order.
    std::vector<std::string> BasesOf(const json &Spec, const std::string &Target)
    {
        std::vector<std::string> B;
        const int I = IndexOfTarget(Spec, Target);
        if (I < 0) return B;
        const json &L = Spec["layers"][size_t(I)];
        if (L.contains("bases")) for (const auto &E : L["bases"]) B.push_back(E.get<std::string>());
        return B;
    }
private slots:

    // A delta over [wine ‖ dxvk] reaches the FS naming its bases by CID, in order (the concatenation its COPY offsets
    // were generated against), and each zip and delta carries its own CID, so the FS can find the files. Teeth: drop a
    // CID; reorder the bases.
    void a_delta_reaches_the_spec_naming_its_bases_by_cid()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("wine.zip", "wine", "QmW"),
            ZipLayer("dxvk.zip", "dxvk", "QmD"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "QmD", "QmW" }), "QmP"),
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(BasesOf(Spec, "prefix"), (std::vector<std::string>{ "QmD", "QmW" }));
        QCOMPARE(Spec["layers"][0].value("cid", std::string()), std::string("QmW"));
        QCOMPARE(Spec["layers"][2].value("cid", std::string()), std::string("QmP"));
        QVERIFY2(C.Lines.empty(), "a plan the FS will mount must produce no complaint");
    }

    // A delta that cannot rebuild — a CID it names is on no zip or delta of this plan (a FILE carries no archive), or it
    // names none — is said before the mount, by name, with what it costs: the FS leaves it out. A base placed anywhere
    // in the plan is found (its bytes do not depend on where it is). Teeth: stay quiet; complain about a base above.
    void a_delta_that_cannot_rebuild_is_reported_before_the_mount()
    {
        for (int Case = 0; Case < 3; ++Case)
        {
            Complaints C;
            ContainerParams CP = Make();
            json F{{"TYPE", "VFSFileLayer"}, {"PATH", "f.bin"}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", "QmW"}}}};
            CP.SubComponentsArray = Case == 0 ? json::array({ DeltaLayer("d.vgdelta", "out", json::array({ "QmNone" })), ZipLayer("wine.zip", "wine", "QmW") })
                                  : Case == 1 ? json::array({ F, DeltaLayer("d.vgdelta", "out", json::array({ "QmW" })) })
                                              : json::array({ DeltaLayer("d.vgdelta", "out", json(nullptr)), ZipLayer("wine.zip", "wine", "QmW") });
            VfsMount::BuildLayerSpec(CP);
            QVERIFY2(C.Mentions("d.vgdelta") && C.Mentions("LEAVES IT OUT"), "the delta is named, and what it costs said");
            if (Case < 2) QVERIFY2(C.Mentions(Case == 0 ? "QmNone" : "QmW"), "and the base it could not find");
        }
        Complaints Quiet;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DeltaLayer("d.vgdelta", "out", json::array({ "QmW" })), ZipLayer("wine.zip", "wine", "QmW") });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(Quiet.Lines.empty(), "a base later in the plan is still the file it names");
    }

    // A base no TAKE shows is mounted for its bytes only; only a delta has bases. Teeth: drop the flag; carry bases on a dir.
    void a_bytes_only_base_is_flagged_and_only_a_delta_has_bases()
    {
        ContainerParams CP = Make();
        json Z = ZipLayer("win.zip", "win", "QmW");
        Z["BYTES_ONLY"] = true;
        json D = DirLayer("content", "game");
        D["BASE_SOURCES"] = json::array({ "QmW" });            // author error / leftover key
        CP.SubComponentsArray = json::array({ Z, D, DeltaLayer("mac.vgdelta", "mac", json::array({ "QmW" })) });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY(Spec["layers"][0].value("bytesOnly", false));
        QVERIFY2(!Spec["layers"][1].contains("bases"), "only a delta has a byte-base — the key is meaningless elsewhere");
        QCOMPARE(BasesOf(Spec, "mac"), (std::vector<std::string>{ "QmW" }));
    }

    // The same reader serves the EDITOR, which holds raw layer entries (FORM "delta"), as well as lowered layers.
    void base_sources_are_read_from_an_entry_as_well_as_a_layer()
    {
        const json Entry{{"FORM", "delta"}, {"PATH", "d.vgdelta"}, {"BASE_SOURCES", json::array({"QmW", "QmD"})}};
        QCOMPARE(ManifestModel::LayerBaseSources(Entry), (std::vector<std::string>{"QmW", "QmD"}));
        const json Zip{{"FORM", "zip"}, {"PATH", "a.zip"}, {"BASE_SOURCES", json::array({"QmW"})}};
        QVERIFY(ManifestModel::LayerBaseSources(Zip).empty());
        const json Bad{{"TYPE", "VFSDeltaLayer"}, {"PATH", "d.vgdelta"}, {"BASE_SOURCES", "QmW"}};
        QVERIFY(ManifestModel::LayerBaseSources(Bad).empty());
    }

    // The runner build and the inner-runner chain carry bases the same way and get the same check: where multi-base
    // pays (a prefix delta'd over [wine ‖ dxvk]). Teeth: either builder skipping CheckPlan.
    void the_runner_builds_carry_bases_the_same_way()
    {
        {
            Complaints C;
            ContainerParams CP = Make();
            CP.RunnerPackagePath = CP.PackagePath;
            CP.RunnerLayers = { ZipLayer("wine.zip", "wine", "QmW"), ZipLayer("dxvk.zip", "dxvk", "QmD"),
                                DeltaLayer("pfx.vgdelta", "prefix", json::array({ "QmW", "QmD" })),
                                DeltaLayer("bad.vgdelta", "bad", json::array({ "QmNone" })) };
            const json Spec = VfsMount::BuildRunnerLayerSpec(CP);
            QCOMPARE(BasesOf(Spec, "prefix"), (std::vector<std::string>{ "QmW", "QmD" }));
            QVERIFY(C.Mentions("bad.vgdelta") && C.Mentions("QmNone"));
        }
        ContainerParams CP = Make();
        RunnerLink Inner;
        Inner.NodeId = "emu";
        Inner.PackagePath = CP.PackagePath;
        Inner.Layers = { ZipLayer("ewine.zip", "wine", "QmE"), DeltaLayer("epfx.vgdelta", "prefix", json::array({ "QmE" })) };
        RunnerLink Outer;  Outer.NodeId = "proton";  Outer.PackagePath = CP.PackagePath;
        Outer.ContentRoot = "pfx/drive_c/UID123";
        CP.RunnerChain = { Inner, Outer };
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(LaunchResolver::BoundaryLinkIndex(CP), 1);
        const std::string Base = LaunchResolver::InnerRunnerMountRel("emu");
        QCOMPARE(BasesOf(Spec, Base + "/prefix"), (std::vector<std::string>{ "QmE" }));
    }

    // Building a plan must not touch the filesystem: the audit builds 960 of them, and a plan that creates
    // directories cannot be inspected without side effects — which is why the audit used to re-implement it.
    void building_a_plan_creates_no_directories()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        CP.KeepDirs   = { {"game/save", "save", true} };
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(!std::filesystem::exists(CP.UserDataPath / "save"),
                 "a KEEP dir's durable source must be created by the MOUNT, not by building the plan");
        QVERIFY2(!std::filesystem::exists(CP.WriteLayerPath),
                 "nor the ephemeral write layer");

        // ...and the MOUNT's half of the split: everything the plan named must then actually appear, or the
        // mount fails on real hardware with nothing in the suite able to notice.
        const json Spec = VfsMount::BuildLayerSpec(CP);
        VfsMount::MaterializePlanPaths(Spec);
        QVERIFY(std::filesystem::exists(CP.WriteLayerPath));
        QVERIFY(std::filesystem::exists(CP.UserDataPath / "save"));
    }

    // MaterializePlanPaths sits in SpawnVidyagodfs, which every mount goes through — INCLUDING the read-only
    // ones (the runner build, runner install, a read-only runtime), whose plans set "writelayer": null. A
    // `Spec.value("writelayer", std::string())` there does not return the default: nlohmann THROWS on a
    // present-but-wrong-type key and only defaults when the key is ABSENT. That took down the whole app on the
    // first proton launch — the mount step of every game that has a runner build.
    void materializing_a_read_only_plan_does_not_throw()
    {
        ContainerParams CP = Make();
        CP.ReadOnlyVFS = true;
        //A path of this test's own: the fixture's temp root is shared, and an earlier test legitimately creates
        //the default WRITELAYER — asserting on it here would pass or fail on test ORDER, not on behaviour.
        CP.WriteLayerPath = std::filesystem::path(Root.path().toStdString()) / "RO_WRITELAYER";
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY2(Spec["writelayer"].is_null(), "a read-only plan has a NULL write branch, not a missing one");
        VfsMount::MaterializePlanPaths(Spec);           // must not throw
        QVERIFY(!std::filesystem::exists(CP.WriteLayerPath));

        // ...and the same for a spec that is malformed outright — a plan can be hand-written or come off disk.
        VfsMount::MaterializePlanPaths(json::object());
        VfsMount::MaterializePlanPaths(json{{"writelayer", 7}, {"layers", "not-an-array"}});
        VfsMount::MaterializePlanPaths(json{{"layers", json::array({ json{{"rw", true}, {"type", "dir"}} })}});
        QVERIFY2(VfsMount::ReportMissingSources(json::object()) == 0, "and the sweep is total too");
    }

    // A kept folder (an RW passthrough source) that is a link is served as is by the helper, so every write into the
    // keep, the launcher's own edits included, lands where the link points — and it may be the user's own (saves in a
    // synced folder), so it is never removed: the mount refuses and the link stays. The writelayer is the session's,
    // and the game could swap it for a link between sessions: that one is replaced. Neither link's target is touched.
    // Teeth: create_directories over the keep link (true, mount goes ahead); remove the keep link (the user's link is
    // gone); keep the writelayer link (still a link).
    void materializing_refuses_a_kept_folder_link_and_replaces_a_writelayer_link()
    {
        ContainerParams CP = Make();
        CP.WriteLayerPath = std::filesystem::path(Root.path().toStdString()) / "LINK_WRITELAYER";
        CP.KeepDirs = { {"game/linked_keep", "linked_keep", true} };
        const std::filesystem::path Victim = std::filesystem::path(Root.path().toStdString()) / "victim";
        std::filesystem::create_directories(Victim);
        { std::ofstream(Victim / "bashrc") << "the user's own"; }
        std::filesystem::create_directories(CP.UserDataPath);
        std::filesystem::create_symlink(Victim, CP.UserDataPath / "linked_keep");
        std::filesystem::create_symlink(Victim, CP.WriteLayerPath);
        Complaints C;
        QVERIFY(!VfsMount::PrepareMount(VfsMount::BuildLayerSpec(CP)));
        QVERIFY(C.Mentions("linked_keep is a link"));
        QVERIFY(std::filesystem::is_symlink(CP.UserDataPath / "linked_keep"));
        QVERIFY(std::filesystem::symlink_status(CP.WriteLayerPath).type() == std::filesystem::file_type::directory);
        QVERIFY(std::filesystem::exists(Victim / "bashrc"));
        CP.KeepDirs.clear();
        QVERIFY(VfsMount::PrepareMount(VfsMount::BuildLayerSpec(CP)));
    }

    // The sweep must run AFTER the mount has created what the plan names, or it reports the RW passthrough
    // sources (KEEP dirs) as missing on every first launch — a false alarm on the one diagnostic that exists to
    // catch genuinely absent content, which is how a real one stops being believed.
    void the_missing_source_sweep_does_not_flag_paths_the_mount_creates()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        //Paths unique to this test: the fixture's temp root is shared, so reusing a name another test already
        //materialised would make the "before" assertion pass or fail on test ORDER rather than on behaviour.
        CP.KeepDirs  = { {"game/save_sweep_only", "save_sweep_only", true} };
        const json Spec = VfsMount::BuildLayerSpec(CP);

        // before the mount touches anything its durable source is legitimately absent...
        QCOMPARE(VfsMount::ReportMissingSources(Spec), size_t(1));

        // ...and PrepareMount — what MountVFS actually calls — must leave nothing missing. Asserting the two
        // halves separately would pass with the calls in EITHER order; the order is the whole bug.
        Complaints C;
        VfsMount::PrepareMount(Spec);
        QVERIFY2(!C.Mentions("do not exist"),
                 "the sweep ran before the mount created the paths it names — a false missing-content alarm");
    }

    // SUBMOUNTS relocate a subtree to another runtime path, and EVERY submount-using node in the library
    // carries a %var% in its destination. Package layers are substituted wholesale by the resolver, but the
    // runner-build list is assigned straight off the runner node and reaches the mount builder RAW — it used to
    // substitute TARGET by name and leave PATH and SUBMOUNTS alone, so a library package
    // parented to a runner mounted its files under a directory literally called "%dxwnd_dir%", silently.
    void runner_build_layers_are_substituted_whole_not_field_by_field()
    {
        ContainerParams CP = Make();
        CP.PrefixRoot        = "pfx";
        CP.RunnerPackagePath = CP.PackagePath;
        json L = ZipLayer("codecs.zip", "%PrefixRoot%/lib");
        L["SUBMOUNTS"] = json::array({ "codecs/ir41_32.dll:%PrefixRoot%/drive_c/windows/syswow64/ir41_32.dll" });
        CP.RunnerLayers = { L };

        const json Spec = VfsMount::BuildRunnerLayerSpec(CP);
        QCOMPARE(Spec["layers"].size(), size_t(1));
        QCOMPARE(Spec["layers"][0].value("target", std::string()), std::string("pfx/lib"));
        QVERIFY2(Spec["layers"][0]["submounts"][0].get<std::string>()
                     == "codecs/ir41_32.dll:pfx/drive_c/windows/syswow64/ir41_32.dll",
                 "a SUBMOUNT destination must be %var%-substituted like any other target");
    }

    // ...and if one ever does survive, it must be reported rather than mounted at a literal "%Var%" path.
    void a_token_surviving_in_a_submount_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        json L = ZipLayer("codecs.zip", "lib");
        L["SUBMOUNTS"] = json::array({ "codecs/x.dll:%NoSuchVariable%/drive_c/x.dll" });
        CP.SubComponentsArray = json::array({ L });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("SUBMOUNT"), "a token surviving in a submount must be reported, not mounted quietly");
        QVERIFY2(C.Mentions("NoSuchVariable"), "and must name the variable that was missing");
    }

    void the_mountpoint_is_the_runtime_path()
    {
        ContainerParams CP = Make();
        QCOMPARE(VfsMount::BuildLayerSpec(CP).value("mountpoint", std::string()), CP.RuntimePath.string());
    }

    // A substituted VALUE that would break JSON must still substitute. The old form serialised the layer,
    // spliced values into the TEXT, and re-parsed: one backslash or quote in a path or a package title made
    // the re-parse fail, and the layer was then mounted with its %tokens% intact — silently, which is the
    // exact failure whole-layer substitution exists to prevent.
    void aValueContainingJsonMetacharactersStillSubstitutes()
    {
        ContainerParams CP = Make();
        CP.CustomVariables["awkward"] = R"(C:\Users\Tom "T" O'Neil\dir)";
        CP.RunnerLayers = { json{{"TYPE","VFSZipLayer"},{"PATH","%awkward%/a.zip"},
                                 {"TARGET","%awkward%/t"},
                                 {"SUBMOUNTS", json::array({"x:%awkward%/y"})}} };
        CP.RunnerShipsBuild = true;
        CP.UnifiedRuntime   = false;
        const json Spec = VfsMount::BuildRunnerLayerSpec(CP);
        QVERIFY(Spec.contains("layers"));
        QVERIFY(!Spec["layers"].empty());
        const json &L = Spec["layers"][0];
        const std::string Target = L.value("target", std::string());
        const std::string Source = L.value("source", std::string());
        QVERIFY2(Target.find('%') == std::string::npos,
                 ("target kept a raw token: " + Target).c_str());
        QVERIFY2(Source.find('%') == std::string::npos,
                 ("source kept a raw token: " + Source).c_str());
        // ...and the value survived intact rather than being mangled by an escaping round trip.
        QVERIFY(Target.find(R"(Tom "T" O'Neil)") != std::string::npos);
    }

    // The crash this guards is a key that is PRESENT and NULL, not one that is absent — value() has always
    // handled absent. Every read of a plan has to survive it, so every read is exercised with a null.
    void aPlanWhoseKeysArePresentAndNullDoesNotThrow()
    {
        json Spec;
        Spec["writelayer"] = nullptr;
        Spec["layers"]     = nullptr;
        VERIFY_NO_THROW(VfsMount::MaterializePlanPaths(Spec));
        VERIFY_NO_THROW(VfsMount::ReportMissingSources(Spec));
        VERIFY_NO_THROW(VfsMount::PrepareMount(Spec));

        json WithNullLayer;
        WithNullLayer["writelayer"] = nullptr;
        WithNullLayer["layers"] = json::array({ json{{"type", nullptr}, {"target", nullptr},
                                                     {"source", nullptr}, {"rw", nullptr},
                                                     {"runtimeSourced", nullptr}} });
        VERIFY_NO_THROW(VfsMount::MaterializePlanPaths(WithNullLayer));
        VERIFY_NO_THROW(VfsMount::ReportMissingSources(WithNullLayer));
        VERIFY_NO_THROW(VfsMount::PrepareMount(WithNullLayer));
    }
};

QTEST_MAIN(VfsMountTest)
#include "test_vfsmount.moc"
