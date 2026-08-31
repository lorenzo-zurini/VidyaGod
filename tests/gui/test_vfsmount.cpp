// test_vfsmount.cpp — BuildLayerSpec: the mount plan that IS the game's filesystem.
//
// Layer ORDER is priority. Get it wrong and nothing fails: the mount succeeds, the game runs, and it silently
// reads the wrong copy of a file — a package's own content shadowing the edits meant to fix it, or an ephemeral
// write layer sitting under a durable one so the user's saves vanish at teardown. Nothing in this subsystem
// throws when the plan is wrong, so the plan itself has to be asserted.

#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "vfsmount.h"
#include "launchparams.h"
#include "commonutils.h"

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

private slots:

    // ---- what gets into the plan at all ----

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
        CP.KeepDirs = {"game/save"};

        const json Spec = VfsMount::BuildLayerSpec(CP);
        const int K = IndexOfTarget(Spec, "game/save");
        QVERIFY2(K > IndexOfTarget(Spec, "game"), "a KEEP dir must outrank the content it persists into");
        QVERIFY2(Spec["layers"][K].value("rw", false), "a KEEP dir must be writable or nothing persists");
        const std::string Src = Spec["layers"][K].value("source", std::string());
        QVERIFY2(Src.find("USERDATA") != std::string::npos,
                 "a KEEP dir must be backed by the DURABLE UserDataPath, not by TEMP");
    }

    // DROP carves an ephemeral hole in a keep; if anything outranked it, the write it is meant to discard would
    // land in durable storage instead.
    void drop_paths_are_last_and_ephemeral()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DirLayer("content", "game") });
        CP.DefaultDataPath = std::filesystem::path(Root.path().toStdString()) / "DEFAULTDATA";
        std::filesystem::create_directories(CP.DefaultDataPath);
        CP.KeepDirs   = {"game/save"};
        CP.DropPaths  = {"game/save/cache"};

        const json Spec = VfsMount::BuildLayerSpec(CP);
        const auto T = Targets(Spec);
        QCOMPARE(T.back(), std::string("game/save/cache"));
        const auto &Last = Spec["layers"].back();
        QVERIFY(Last.value("rw", false));
        const std::string Src = Last.value("source", std::string());
        QVERIFY2(Src.find("TEMP") != std::string::npos && Src.find("DROPS") != std::string::npos,
                 "a DROP must be backed by ephemeral TEMP storage — that is the whole point");
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

    void persist_all_makes_the_write_branch_durable_and_drops_the_keep_layers()
    {
        ContainerParams CP = Make();
        CP.PersistAll = true;
        CP.KeepDirs   = {"game/save"};
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY2(Spec.value("writelayer", std::string()).find("USERDATA") != std::string::npos,
                 "a whole-runtime keep writes straight into the durable store");
        QVERIFY2(IndexOfTarget(Spec, "game/save") < 0,
                 "KEEP passthroughs are redundant once the whole branch is durable — emitting them anyway would "
                 "stack a second durable source over the same target");
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

        VfsMount::BuildLayerSpec(CP);
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

    void the_mountpoint_is_the_runtime_path()
    {
        ContainerParams CP = Make();
        QCOMPARE(VfsMount::BuildLayerSpec(CP).value("mountpoint", std::string()), CP.RuntimePath.string());
    }
};

QTEST_MAIN(VfsMountTest)
#include "test_vfsmount.moc"
