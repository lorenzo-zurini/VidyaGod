// test_vfsmount.cpp — BuildLayerSpec: the mount plan that IS the game's filesystem.
//
// Layer ORDER is priority. Get it wrong and nothing fails: the mount succeeds, the game runs, and it silently
// reads the wrong copy of a file — a package's own content shadowing the edits meant to fix it, or an ephemeral
// write layer sitting under a durable one so the user's saves vanish at teardown. Nothing in this subsystem
// throws when the plan is wrong, so the plan itself has to be asserted.

#include <fstream>
#include <QtTest/QtTest>
#include <QTemporaryDir>

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

    // A VFSZipLayer whose archive really exists. Bases must be ZIP (or delta) layers: only those become a
    // composed BYTE view the FS can reconstruct against (overlay.cpp keeps `baseByTarget` inside that branch),
    // so a fixture that based a delta on a DIR layer would pin a plan the FS refuses to mount.
    json ZipLayer(const std::string &Rel, const std::string &Target = {})
    {
        const std::filesystem::path P = std::filesystem::path(Root.path().toStdString()) / "PKG" / Rel;
        std::filesystem::create_directories(P.parent_path());
        std::ofstream(P, std::ios::binary) << "PK\x05\x06";
        json L{{"TYPE", "VFSZipLayer"}, {"PATH", Rel}};
        if (!Target.empty()) L["TARGET"] = Target;
        return L;
    }

    // A VFSDeltaLayer whose .vgdelta really exists, so the missing-source detector stays quiet. `Bases` is
    // always a LIST — that is the only shape the key has, at the node and at the layer.
    json DeltaLayer(const std::string &Rel, const std::string &Target, const json &Bases)
    {
        const std::filesystem::path P = std::filesystem::path(Root.path().toStdString()) / "PKG" / Rel;
        std::filesystem::create_directories(P.parent_path());
        std::ofstream(P, std::ios::binary) << "vgdelta";
        json L{{"TYPE", "VFSDeltaLayer"}, {"PATH", Rel}, {"TARGET", Target}};
        if (!Bases.is_null()) L["BASE_TARGETS"] = Bases;
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

    // ---- multi-base deltas: the node's bases must survive the trip into the mount plan ----

    // Bases in plan order for the delta layer at `Target`.
    std::vector<std::string> BasesOf(const json &Spec, const std::string &Target)
    {
        std::vector<std::string> B;
        const int I = IndexOfTarget(Spec, Target);
        if (I < 0) return B;
        const json &L = Spec["layers"][size_t(I)];
        if (L.contains("baseTargets")) for (const auto &E : L["baseTargets"]) B.push_back(E.get<std::string>());
        return B;
    }

    // The format has allowed an ARRAY of bases since the delta layer existed, and the FS stitches them with a
    // ConcatByteSource — but the mount builder only ever read the singular key, so a node declaring three bases
    // produced a plan with NONE and the delta reconstructed against the wrong bytes (or refused to open).
    void a_multi_base_delta_reaches_the_plan_as_an_ordered_list()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("wine.zip", "wine"),
            ZipLayer("dxvk.zip", "dxvk"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "wine", "dxvk" })),
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        const int I = IndexOfTarget(Spec, "prefix");
        QVERIFY(I >= 0);
        QCOMPARE(Spec["layers"][size_t(I)].value("type", std::string()), std::string("delta"));
        const auto B = BasesOf(Spec, "prefix");
        QCOMPARE(B.size(), size_t(2));
        QCOMPARE(B[0], std::string("wine"));
        QVERIFY2(B[1] == "dxvk",
                 "ORDER is load-bearing: it is the concatenation the delta's COPY offsets were generated against");
    }

    // ONE key carries the bases however many there are, and a one-entry list IS the ordinary cross-target
    // delta. A singular key could not express a base declared AT THE ROOT — "" would be indistinguishable from
    // "no base declared", and the FS would fall back to the delta's own target, reconstruct against the wrong
    // bytes and skip the layer.
    void one_base_uses_the_same_key_as_many()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("base.zip", "game"),
            DeltaLayer("one.vgdelta", "game2", json::array({ "game" })),
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(BasesOf(Spec, "game2"), std::vector<std::string>{ "game" });
        QVERIFY2(!Spec["layers"][size_t(IndexOfTarget(Spec, "game2"))].contains("baseTarget"),
                 "there is no singular key — one shape for one base and for many");
    }

    void a_base_declared_at_the_root_survives_as_a_declaration()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("root.zip"),                                            // mounts at the VFS root
            DeltaLayer("sub.vgdelta", "game", json::array({ "" })),          // ...and is this delta's byte-base
        });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY2(BasesOf(Spec, "game") == std::vector<std::string>{ "" },
                 "a base at the root is a REAL declaration; dropping it makes the FS use the delta's own target");
    }

    // Bases are TARGETs, so they carry the same %variables% as TARGET and must be resolved the same way —
    // an unsubstituted base names a mount target that does not exist and the delta silently finds no base.
    void base_targets_are_variable_substituted_like_targets()
    {
        ContainerParams CP = Make();
        CP.PackageUID = "UID123";
        CP.PrefixRoot = "pfx";
        CP.SubComponentsArray = json::array({
            DeltaLayer("d.vgdelta", "out", json::array({ "%PrefixRoot%/drive_c/%PackageUID%", "%PrefixRoot%\\shared" })),
        });
        const auto B = BasesOf(VfsMount::BuildLayerSpec(CP), "out");
        QCOMPARE(B.size(), size_t(2));
        QCOMPARE(B[0], std::string("pfx/drive_c/UID123"));
        QVERIFY2(B[1] == "pfx/shared",
                 "backslashes must be normalised in a base exactly as in a target, or the strings never match");
    }

    // %PrefixRoot% is EMPTY for a wine-at-root runner, so a perfectly ordinary base resolves to "". Dropping it
    // as "empty" would turn a two-base delta into a one-base one: a base of the wrong SIZE, a reconstruction
    // that fails its size check, and a layer the FS skips — with the audit still clean.
    void a_base_that_substitutes_to_the_root_is_not_dropped()
    {
        ContainerParams CP = Make();
        CP.PrefixRoot = "";
        CP.SubComponentsArray = json::array({
            ZipLayer("wine.zip"),
            ZipLayer("dxvk.zip", "dxvk"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "%PrefixRoot%", "dxvk" })),
        });
        const auto B = BasesOf(VfsMount::BuildLayerSpec(CP), "prefix");
        QCOMPARE(B.size(), size_t(2));
        QCOMPARE(B[0], std::string(""));
        QCOMPARE(B[1], std::string("dxvk"));
    }

    // A token surviving in a BASE is the same silent disaster as one surviving in a TARGET, and the log line
    // must name the layer — "Layer ? '?'" is unfindable among 992 delta nodes.
    void a_token_surviving_in_a_base_is_reported_with_the_layer()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DeltaLayer("d.vgdelta", "out", json::array({ "ok", "%NoSuchVariable%/base" })),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("%token%"), "a token surviving in a BASE must be reported, not mounted quietly");
        QVERIFY2(C.Mentions("NoSuchVariable"), "and must name the variable that was missing");
        QVERIFY2(C.Mentions("d.vgdelta"), "and the LAYER, or the offender cannot be found in the library");
    }

    // The key is a LIST and nothing else. A bare string is refused at the front end (NodeLower), so one
    // reaching the mounter means a hand-written layer — and inventing a base from it would be the mounter
    // holding a second, private opinion about the shape, which is how the two spellings diverged before.
    void a_base_list_that_is_not_a_list_contributes_nothing()
    {
        ContainerParams CP = Make();
        json D = DeltaLayer("d.vgdelta", "out", json(nullptr));
        D["BASE_TARGETS"] = "c";                                // a string where the list belongs
        CP.SubComponentsArray = json::array({ D });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY(BasesOf(Spec, "out").empty());
        QVERIFY(!Spec["layers"][0].contains("baseTargets"));
    }

    // An empty list declares nothing, which is NOT the same as declaring a base at the root.
    void an_empty_base_list_declares_no_base()
    {
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({ DeltaLayer("d.vgdelta", "out", json::array()) });
        const json Spec = VfsMount::BuildLayerSpec(CP);
        QVERIFY(!Spec["layers"][0].contains("baseTargets"));
        QVERIFY(!Spec["layers"][0].contains("baseTarget"));
    }

    // The same reader serves the EDITOR, which holds NODES (TYPE "Content", FORM "delta"), not lowered layers.
    // It read only the lowered spelling, so every node-level caller silently got "no bases" — including the
    // guard meant to stop `undelta` from reconstructing a multi-base delta against a single archive.
    void base_targets_are_read_from_a_node_as_well_as_a_layer()
    {
        const json Node{{"LABEL","d"}, {"TYPE","Content"}, {"FORM","delta"}, {"PATH","d.vgdelta"},
                        {"BASE_TARGETS", json::array({"wine", "dxvk"})}};
        QCOMPARE(ManifestModel::LayerBaseTargets(Node), (std::vector<std::string>{"wine", "dxvk"}));

        // ...and a non-delta node still has none, whatever key it carries.
        json Zip{{"LABEL","z"}, {"TYPE","Content"}, {"FORM","zip"}, {"PATH","a.zip"},
                 {"BASE_TARGETS", json::array({"wine"})}};
        QVERIFY(ManifestModel::LayerBaseTargets(Zip).empty());
    }

    // A non-delta layer has no byte-base at all; emitting one would make the FS try to reconstruct a plain dir.
    void a_non_delta_layer_never_carries_a_base()
    {
        ContainerParams CP = Make();
        json D = DirLayer("content", "game");
        D["BASE_TARGETS"] = json::array({ "wine" });            // author error / leftover key
        CP.SubComponentsArray = json::array({ D });
        const json Spec = VfsMount::BuildLayerSpec(CP);     // by value — a reference into the temporary dangles
        const json &L = Spec["layers"][0];
        QVERIFY(!L.contains("baseTarget"));
        QVERIFY2(!L.contains("baseTargets"), "only a delta has a byte-base — the key is meaningless elsewhere");
    }

    // ---- the OTHER two mount builders read the same bases the same way ----

    // The runner build is where multi-base actually pays (a prefix delta'd over [wine ‖ dxvk]), and it was the
    // one builder that hand-rolled its own base reading — with no surviving-%token% diagnostic at all.
    void the_runner_build_resolves_bases_the_same_way()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.RunnerPackagePath = CP.PackagePath;
        CP.RunnerLayers = json::array({
            ZipLayer("wine.zip", "wine"),
            ZipLayer("dxvk.zip", "dxvk"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "wine", "dxvk", "%NoSuchVariable%" })),
        });
        const json Spec = VfsMount::BuildRunnerLayerSpec(CP);
        const auto B = BasesOf(Spec, "prefix");
        QCOMPARE(B.size(), size_t(3));
        QCOMPARE(B[0], std::string("wine"));
        QCOMPARE(B[1], std::string("dxvk"));
        QVERIFY2(C.Mentions("%token%"),
                 "the runner build must emit the same token diagnostic as the content mount, or a wrong runner "
                 "plan is built in total silence");
    }

    // The inner-runner nesting mounts under its own base prefix; a delta's bases must carry that SAME prefix or
    // they name targets that do not exist in the plan.
    void inner_runner_bases_carry_the_nesting_prefix()
    {
        ContainerParams CP = Make();
        RunnerLink Inner;
        Inner.NodeId      = "emu";
        Inner.PackagePath = CP.PackagePath;
        Inner.Layers      = json::array({
            ZipLayer("ewine.zip", "wine"),
            DeltaLayer("epfx.vgdelta", "prefix", json::array({ "wine" })),
        });
        // The boundary link is the LAST non-native one; a native namespace is an empty ContentRoot with no
        // prefix generation, so giving the outer link a ContentRoot makes BoundaryLinkIndex() == 1 and the
        // inner-runner nesting block run for `emu`.
        RunnerLink Outer;  Outer.NodeId = "proton";  Outer.PackagePath = CP.PackagePath;
        Outer.ContentRoot = "pfx/drive_c/UID123";
        CP.RunnerChain = { Inner, Outer };

        const json Spec = VfsMount::BuildLayerSpec(CP);
        QCOMPARE(LaunchResolver::BoundaryLinkIndex(CP), 1);
        const std::string Base = LaunchResolver::InnerRunnerMountRel("emu");
        const auto B = BasesOf(Spec, Base + "/prefix");
        QCOMPARE(B.size(), size_t(1));
        QVERIFY2(B[0] == Base + "/wine",
                 "an inner-runner delta's base must be prefixed exactly like its target, or it names nothing");
    }


    // ---- the plan-level base check: the one diagnostic for a delta whose base mounts nothing ----
    //
    // These live here, not in the audit, because the check now runs inside BuildLayerSpec: a real launch says it
    // too, and --audit-packages gets it by capturing the same line. That is deliberate — the audit used to carry
    // a second, approximate model of the plan and disagreed with the mounter about layer types, order, and the
    // prefix an inner-runner's targets carry.

    void a_base_that_nothing_composes_bytes_at_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("wine.zip", "wine"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "wine", "nowhere" })),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("nowhere"), "a base naming a target nothing composes must be reported by name");
        QVERIFY2(C.Mentions("SKIP"), "and must say what it costs: the layer is skipped, the content is missing");
        QVERIFY2(!C.Mentions("'wine'"), "the base that IS composed must not be reported");
    }

    // A dir layer mounts perfectly well and is NOT a byte view — overlay.cpp registers `baseByTarget` only for
    // zip/delta. A check that accepted dir targets would pass exactly the plans the FS refuses.
    void a_dir_layer_is_not_a_byte_base()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DirLayer("loose", "probe"),
            DeltaLayer("d.vgdelta", "out", json::array({ "probe" })),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("probe"), "a dir layer mounts there but composes no bytes — it cannot be a base");
    }

    // ORDER is the whole point: the FS registers each target's composed view as it walks the plan, so a base
    // supplied by a LATER layer is a lookup miss at the moment the delta is built.
    void a_base_supplied_later_in_the_plan_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DeltaLayer("d.vgdelta", "out", json::array({ "wine" })),
            ZipLayer("wine.zip", "wine"),                       // arrives AFTER the delta that needs it
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("wine"), "a base mounted later than the delta is not yet composed when it is needed");
    }

    // ...and the healthy plan must stay quiet, or the noise stops meaning anything.
    void a_well_formed_multi_base_plan_is_quiet()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("wine.zip", "wine"),
            ZipLayer("dxvk.zip", "dxvk"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "wine", "dxvk" })),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Lines.empty(), "a plan the FS will mount must produce no complaint");
    }

    // An UNDECLARED base is the composed view at the delta's own target — the ordinary chain, and what every
    // delta in the library actually is. A chain with its base below it is healthy and must stay quiet.
    void an_undeclared_base_over_a_real_chain_is_quiet()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("base.zip", "game"),
            DeltaLayer("d.vgdelta", "game", json(nullptr)),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY(C.Lines.empty());
    }

    // ...but a delta that is the LOWEST layer at its target has nothing to reconstruct against, and the FS drops
    // it exactly as it drops one with a bad declared base. Checking only the DECLARED case covered ~none of the
    // library: all 992 delta nodes use the implicit form.
    void a_delta_with_nothing_below_it_is_reported()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("elsewhere.zip", "other"),
            DeltaLayer("d.vgdelta", "game", json(nullptr)),     // first layer at "game"
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Mentions("no layer below it"), "an implicit base with nothing under it must be reported");
        QVERIFY2(C.Mentions("game"), "and must name the target it needed");
    }

    // A delta the FS will SKIP composes no byte view either, so a LATER delta based on that target must not be
    // waved through — one broken layer must not silently cost a second.
    void a_skipped_delta_does_not_become_a_byte_view()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            DeltaLayer("broken.vgdelta", "A", json(nullptr)),   // nothing below it -> the FS drops it
            ZipLayer("b.zip", "B"),
            DeltaLayer("dep.vgdelta", "B", json::array({ "A" })),
        });
        VfsMount::BuildLayerSpec(CP);
        int Mentions = 0;
        for (const auto &L : C.Lines) if (L.find("dep.vgdelta") != std::string::npos) ++Mentions;
        QVERIFY2(Mentions > 0, "a base pointing at a layer the FS will skip must be reported too");
    }

    // A target carrying a delta CHAIN composes a byte view, so a base naming it is legitimate however many
    // layers are stacked there — including an ordinary patch zip above the chain. The check asks whether SOME
    // composed view exists by then, not which one; it deliberately does not guess whether a mid-chain delta's
    // base meant the partial or the finished tree (a wrong guess fails the delta's own size check, loudly).
    void a_target_carrying_a_chain_is_a_valid_base()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.SubComponentsArray = json::array({
            ZipLayer("base.zip",   "game"),
            DeltaLayer("v2.vgdelta", "game", json(nullptr)),    // the chain's final delta
            ZipLayer("patch.zip",  "game"),                     // an overlay above it, not a byte input
            DeltaLayer("x.vgdelta", "other", json::array({ "game" })),
        });
        VfsMount::BuildLayerSpec(CP);
        QVERIFY2(C.Lines.empty(), "the chain at 'game' composes a byte view; the overlay above it is irrelevant");
    }

    // The runner build gets the same check — it is where multi-base actually pays.
    void the_runner_build_reports_a_base_that_mounts_nothing()
    {
        Complaints C;
        ContainerParams CP = Make();
        CP.RunnerPackagePath = CP.PackagePath;
        CP.RunnerLayers = json::array({
            ZipLayer("wine.zip", "wine"),
            DeltaLayer("pfx.vgdelta", "prefix", json::array({ "wine", "dxvk" })),
        });
        VfsMount::BuildRunnerLayerSpec(CP);
        QVERIFY2(C.Mentions("dxvk"), "the runner build must run the same check as the content mount");
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
    // substitute TARGET and BASE_TARGETS by name and leave PATH and SUBMOUNTS alone, so a library package
    // parented to a runner mounted its files under a directory literally called "%dxwnd_dir%", silently.
    void runner_build_layers_are_substituted_whole_not_field_by_field()
    {
        ContainerParams CP = Make();
        CP.PrefixRoot        = "pfx";
        CP.RunnerPackagePath = CP.PackagePath;
        json L = ZipLayer("codecs.zip", "%PrefixRoot%/lib");
        L["SUBMOUNTS"] = json::array({ "codecs/ir41_32.dll:%PrefixRoot%/drive_c/windows/syswow64/ir41_32.dll" });
        CP.RunnerLayers = json::array({ L });

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
        CP.RunnerLayers = json::array({ json{{"TYPE","VFSZipLayer"},{"PATH","%awkward%/a.zip"},
                                             {"TARGET","%awkward%/t"},
                                             {"SUBMOUNTS", json::array({"x:%awkward%/y"})}} });
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
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::MaterializePlanPaths(Spec));
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::ReportMissingSources(Spec));
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::PrepareMount(Spec));

        json WithNullLayer;
        WithNullLayer["writelayer"] = nullptr;
        WithNullLayer["layers"] = json::array({ json{{"type", nullptr}, {"target", nullptr},
                                                     {"source", nullptr}, {"rw", nullptr},
                                                     {"runtimeSourced", nullptr}} });
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::MaterializePlanPaths(WithNullLayer));
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::ReportMissingSources(WithNullLayer));
        QVERIFY_THROWS_NO_EXCEPTION(VfsMount::PrepareMount(WithNullLayer));
    }
};

QTEST_MAIN(VfsMountTest)
#include "test_vfsmount.moc"
