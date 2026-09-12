#include "cli/climodes.h"
#include "main.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "launchresolver.h"
#include "launchparams.h"
#include "vfsmount.h"
#include "varsubst.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

// ============================================================================
// --audit-packages — the loud sweep over every launchable in the catalog.
//
// WHY THIS EXISTS. `--validate-nodes` checks the graph STATICALLY and has been clean for a long time, yet real
// packages were quietly broken: Worms 4 Mayhem printed "Could not open file for reading" on EVERY launch for
// months and presented only as "the aspect ratio looks a bit off". Once most launches work, nobody reads the
// logs, so the surviving bugs are selected for being quiet.
//
// So this sweep does two things static validation cannot:
//   1. RESOLVES each launchable in-process (runner pick, CustomVar fixpoint, EXEC/EXEARGS substitution) and
//      captures every WARN/ERR the engine emits while doing it — the diagnostics nobody was reading.
//   2. Applies checks for the SHAPES of known silent failure, i.e. authoring that does nothing at all rather
//      than failing: a ConfigWrite that runs before the file it edits exists, a FILE path that escapes its
//      pass, a %token% that survives substitution into a real argument.
//
// It deliberately does NOT mount anything or build a runtime: it must be safe and fast to run over the whole
// catalog. Exit code is non-zero when anything was found, so it can gate a publish.
// ============================================================================

namespace {

using ManifestModel::HasLiveToken;

struct Finding
{
    std::string Node, Package, Kind, Detail;
    bool        Error = true;      // false ⇒ warning
};

//Static checks for authoring that silently does nothing. Each of these has actually shipped.
void CheckLayers(const Node &N, const std::string &Pkg, std::vector<Finding> &Out)
{
    if (!N.Layers.is_array()) return;
    for (const auto &L : N.Layers)
    {
        if (!L.is_object()) continue;
        const std::string Type = L.value("TYPE", std::string());
        //A malformed WHEN fail-opens (silently always-applies) — a conditional layer/var that never gates. Flag it.
        if (L.contains("WHEN") && L["WHEN"].is_string() && !VarSubst::ConditionParses(std::string(L["WHEN"])))
            Out.push_back({N.NodeId, Pkg, "malformed-when",
                           "WHEN \"" + std::string(L["WHEN"]) + "\" does not parse — the layer would always apply.", true});
        if (Type == "FileEdit")
        {
            const std::string Mode = L.value("MODE", std::string());
            const std::string File = L.value("FILE", std::string());
            const bool Override    = L.value("OVERRIDE", false);
            //ConfigWrite READS the file, rewrites a line and writes it back, so the file must already exist. Content
            //from a zip/delta layer exists only in the mounted RUNTIME union — never in DEFAULTDATA, where the base
            //pass writes. A base-pass ConfigWrite therefore logs one error and does nothing.
            if (Mode == "ConfigWrite" && !Override)
                Out.push_back({N.NodeId, Pkg, "fileedit-configwrite-base-pass",
                               "ConfigWrite on '" + File + "' runs in the BASE pass (before the content is mounted), "
                               "so the file does not exist yet and the edit is silently skipped. Add \"OVERRIDE\": true.", true});
            //FILE is joined onto the pass's base dir. An ABSOLUTE path replaces the base entirely
            //(std::filesystem::operator/), so the edit escapes whichever pass it is in.
            if (File.rfind("%RuntimePath%", 0) == 0 || (!File.empty() && File[0] == '/'))
                Out.push_back({N.NodeId, Pkg, "fileedit-absolute-file",
                               "FILE '" + File + "' is absolute; it is joined onto the pass base, so an absolute path "
                               "escapes it. Use a path relative to the pass base (e.g. %PrefixRoot%/drive_c/%PackageUID%/...).", true});
            if (Mode.empty())
                Out.push_back({N.NodeId, Pkg, "fileedit-no-mode", "FileEdit has no MODE — it is skipped entirely.", true});
        }
        if (Type == "BinaryPatch")
        {
            const std::string Mode = L.value("MODE", std::string());
            const std::string File = L.value("FILE", std::string());
            //An UNGUARDED patch (no EXPECT) writes wherever it lands with no check the bytes are what the author
            //thinks — a wrong/rebuilt binary is silently corrupted. Content-addressing freezes the base, but the
            //guard is the cheap insurance that catches a mispointed FILE or a stale offset. Required for all modes.
            if (!L.contains("EXPECT"))
                Out.push_back({N.NodeId, Pkg, "binarypatch-no-expect",
                               "BinaryPatch on '" + File + "' has no EXPECT guard — it will overwrite whatever is at "
                               "the site with no verification. Add EXPECT (the original bytes).", true});
            //FILE joins onto the runtime mount root; an absolute path escapes it (same trap as FileEdit).
            if (File.rfind("%RuntimePath%", 0) == 0 || (!File.empty() && File[0] == '/'))
                Out.push_back({N.NodeId, Pkg, "binarypatch-absolute-file",
                               "FILE '" + File + "' is absolute; author it relative to the mount root.", true});
            if (Mode != "Replace" && Mode != "Cave" && Mode != "Poke")
                Out.push_back({N.NodeId, Pkg, "binarypatch-bad-mode",
                               "BinaryPatch MODE '" + Mode + "' is not Replace/Cave/Poke — the patch is skipped.", true});
            if (!L.contains("ANCHOR") && !L.contains("OFFSET"))
                Out.push_back({N.NodeId, Pkg, "binarypatch-no-site",
                               "BinaryPatch has neither ANCHOR nor OFFSET — nowhere to patch.", true});
            if (Mode == "Replace" && !L.contains("REPLACE"))
                Out.push_back({N.NodeId, Pkg, "binarypatch-replace-no-bytes",
                               "Replace has no REPLACE bytes.", true});
            if (Mode == "Poke" && !L.contains("VALUE"))
                Out.push_back({N.NodeId, Pkg, "binarypatch-poke-no-value",
                               "Poke has no VALUE.", true});
            if (Mode == "Cave" && !L.contains("PAYLOAD"))
                Out.push_back({N.NodeId, Pkg, "binarypatch-cave-no-payload",
                               "Cave has no PAYLOAD (the cave body).", true});
        }
        //A content layer whose local file is missing is a package that cannot launch (and, with no SOURCE, cannot
        //even be fetched). ValidateNodeGraph covers PATH existence; here we only flag the un-fetchable case.
        if ((Type == "VFSZipLayer" || Type == "VFSDeltaLayer") && !L.contains("SOURCE"))
        {
            const std::string P = L.value("PATH", std::string());
            std::error_code Ec;
            std::filesystem::path Abs = std::filesystem::path(N.BundleDir) / P;
            if (!P.empty() && !std::filesystem::exists(Abs, Ec))
                Out.push_back({N.NodeId, Pkg, "layer-missing-and-unfetchable",
                               "'" + P + "' is missing locally and has no SOURCE, so it can never be fetched.", true});
        }
    }
}

} // namespace

int CliModes::RunAuditPackages(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Scope)
{
    NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);

    //Scope narrows to one package/node so the sweep is iterable (a full pass is minutes). Matched the same way
    //--validate-nodes matches: node id, package UID, or bundle-dir name.
    std::vector<std::string> Launchables;
    for (const auto &[Id, N] : Index.Nodes)
    {
        if (!N.IsLaunchable()) continue;
        if (!Scope.empty())
        {
            const std::string Dir = std::filesystem::path(N.BundleDir).filename().string();
            if (Id != Scope && N.Uid != Scope && Dir != Scope && Dir.find(Scope) == std::string::npos) continue;
        }
        Launchables.push_back(Id);
    }
    std::sort(Launchables.begin(), Launchables.end());
    LogOut("audit", "══════════════════════════════════════════════════════════════════════");
    LogOut("audit", "PACKAGE AUDIT — resolving " + std::to_string(Launchables.size()) + " launchable node(s) of "
                    + std::to_string(Index.Nodes.size()) + " total"
                    + (Scope.empty() ? std::string() : (" [scope: " + Scope + "]")));
    LogOut("audit", "══════════════════════════════════════════════════════════════════════");
    if (Launchables.empty())
    { LogErr("audit", "scope matched no launchable node."); ClearLogCallback(); return 1; }

    std::vector<Finding> Findings;

    //STATIC PASS — over EVERY node, not just launchables. Authoring like a FileEdit lives on whatever node owns
    //the content, which is usually a PARENT (Worms 4's broken edits were on its base node, so a launchable-only
    //scan reported it clean). Checking each node once also avoids re-reporting a shared library node once per
    //dependent — with 903 Minecraft variants that difference is thousands of duplicate lines.
    {
        size_t Checked = 0;
        for (const auto &[Id, Nd] : Index.Nodes)
        {
            if (!Scope.empty())
            {
                const std::string Dir = std::filesystem::path(Nd.BundleDir).filename().string();
                if (Id != Scope && Nd.Uid != Scope && Dir != Scope && Dir.find(Scope) == std::string::npos) continue;
            }
            CheckLayers(Nd, std::filesystem::path(Nd.BundleDir).filename().string(), Findings);
            ++Checked;
        }
        LogOut("audit", "static layer pass: " + std::to_string(Checked) + " node(s) inspected, "
                        + std::to_string(Findings.size()) + " finding(s)");
    }

    //DUPLICATE VARIANT LABELS — two launchables on one tile with the same LABEL render as two identical rows in
    //the launch window's variant combo, indistinguishable to the user. Found live: an obsolete Wipeout XL lobby
    //node (WOLOBBY.EXE, superseded by vglobby) kept its "Multiplayer (TCP/IP)" label next to the real one, so
    //which multiplayer you got depended on which identical entry you happened to click.
    for (const auto &Group : PackageCatalog::PresentableGroups(Index))
    {
        std::map<std::string, std::vector<const Node *>> ByLabel;
        for (const Node *N : Group)
        {
            if (!Scope.empty())
            {
                const std::string Dir = std::filesystem::path(N->BundleDir).filename().string();
                if (N->NodeId != Scope && N->Uid != Scope && Dir != Scope && Dir.find(Scope) == std::string::npos) continue;
            }
            const std::string L = !N->Label.empty() ? N->Label
                                  : (N->Meta.is_object() ? N->Meta.value("TITLE", N->NodeId) : N->NodeId);
            ByLabel[L].push_back(N);
        }
        for (const auto &[Label, Ns] : ByLabel)
        {
            if (Ns.size() < 2) continue;
            std::string Ids;
            for (const Node *N : Ns) { if (!Ids.empty()) Ids += ", "; Ids += N->NodeId; }
            Findings.push_back({Ns.front()->NodeId, std::filesystem::path(Ns.front()->BundleDir).filename().string(),
                                "duplicate-variant-label",
                                "label '" + Label + "' is carried by " + std::to_string(Ns.size())
                                    + " launchables on one tile (" + Ids + ") — they render as identical, "
                                      "indistinguishable rows in the variant picker.", true});
        }
    }

    //Capture what the engine says while resolving, instead of letting it scroll past. This is the whole point:
    //these lines were always being printed and never being read.
    std::vector<std::pair<LogLevel, std::string>> Captured;
    bool Capturing = false;
    //RAII, because the callback closes over `Captured` and this frame, and the window now calls into the plan
    //builders — real code that can throw (a filesystem_error from a bad path, a json type error). An escaping
    //exception used to leave the GLOBAL log callback pointing at a destroyed vector and a destroyed frame, so
    //the next Log() anywhere in the process was a use-after-free.
    struct CaptureScope {
        ~CaptureScope() { ClearLogCallback(); }
    } CaptureGuard;
    SetLogCallback([&](LogLevel Lv, const std::string &Ctx, const std::string &Msg) {
        if (Capturing && (Lv == LogLevel::WARN || Lv == LogLevel::ERR))
            Captured.push_back({Lv, Ctx + ": " + Msg});
    });

    for (const std::string &Id : Launchables)
    {
        const Node *N = Index.Find(Id);
        if (!N) continue;
        const std::string Pkg = std::filesystem::path(N->BundleDir).filename().string();

        ContainerParams CP(N->BundleDir, Id, std::string());
        CP.NodeIdx       = &Index;
        CP.LaunchNodeId  = Id;
        //Pin a plausible screen so DerivePaths does not shell out to xrandr once per node (and so a package that
        //passes %ScreenWidth% to the game resolves to something real).
        CP.ScreenWidth   = "1920";
        CP.ScreenHeight  = "1080";
        //The same engine-injected session facts a real launch gets, so a package's use of them resolves here too.
        CP.SessionVars   = { {"VIDYAGOD_SELF_NAME", "Player"} };

        nlohmann::ordered_json Pool = nlohmann::ordered_json::object();
        Captured.clear();
        Capturing = true;
        const bool Init = LaunchResolver::InitializeFromNode(CP, Pool, GlobalConfigJSON);
        const bool Exec = Init && LaunchResolver::ResolveExecutableDefinition(nlohmann::ordered_json::object(), CP);
        //BUILD THE ACTUAL MOUNT PLANS. Everything the plan builders complain about — a %token% that survived
        //substitution, a delta whose declared byte-base nothing composes — is complained about HERE, by the same
        //code a real launch runs, and lands in Captured like any other engine diagnostic. The alternative, which
        //this replaced, was a second approximate model of the plan living in the audit: it disagreed with the
        //mounter about which layer types can be a base, about the effect of plan ORDER, and about the prefix an
        //inner-runner's targets carry — three ways to report a package broken that isn't, or clean when it is.
        //Safe to call because building a plan has no filesystem side effects; MountVFS materializes the dirs.
        if (Init)
        {
            //The prefix LAYOUT variables are normally derived by probing the mounted runner, which this sweep
            //deliberately never does. Derive them anyway (the probe degrades to its documented last-resort
            //paths when nothing is mounted) — the same pretence as the pinned screen size above, and without it
            //every prefix-assembly layer keeps its %token% and the report is all context artifact.
            LaunchResolver::ProbePrefixLayout(CP);
            //...and the source-existence sweep, minus the layers that cannot exist before a mount. Dropping it
            //from the only OFFLINE check would have lost the most valuable diagnostic here: a typo'd PATH
            //("data/mian.zip") validates clean, audits clean, mounts EMPTY and the game silently misses content.
            const nlohmann::ordered_json Plan = VfsMount::BuildLayerSpec(CP);
            (void)VfsMount::ReportMissingSources(Plan, /*SkipRuntimeSourced=*/true);
            if (VfsMount::RunnerShipsMountableBuild(CP))
                (void)VfsMount::ReportMissingSources(VfsMount::BuildRunnerLayerSpec(CP), true);
        }
        Capturing = false;

        if (!Init) Findings.push_back({Id, Pkg, "resolve-failed", "InitializeFromNode failed — this node cannot launch.", true});
        else if (!Exec) Findings.push_back({Id, Pkg, "exec-resolve-failed", "ResolveExecutableDefinition failed.", true});
        for (const auto &[Lv, Line] : Captured)
            Findings.push_back({Id, Pkg, Lv == LogLevel::ERR ? "engine-error" : "engine-warning", Line, Lv == LogLevel::ERR});

        if (Init)
        {
            //A surviving %token% in something the game actually receives is a typo or a broken reference; it will
            //be passed through verbatim to a command line or a path.
            for (const std::string &A : CP.ExeArgs)
                if (HasLiveToken(A))
                    Findings.push_back({Id, Pkg, "unresolved-token-in-exeargs",
                                        "argument '" + A + "' still contains an unresolved %token%.", true});
            for (const auto &[K, V] : CP.CustomVariables)
                if (HasLiveToken(V))
                    Findings.push_back({Id, Pkg, "unresolved-token-in-var",
                                        K + " = '" + V + "' still contains an unresolved %token%.", false});
            if (HasLiveToken(CP.ExePathRelative.string()))
                Findings.push_back({Id, Pkg, "unresolved-token-in-contentpath",
                                    "CONTENTPATH resolved to '" + CP.ExePathRelative.string() + "'.", true});

        }
    }
    //Group by package so the report reads as "which games are broken", not a flat wall.
    std::map<std::string, std::vector<const Finding*>> ByPkg;
    for (const auto &F : Findings) ByPkg[F.Package].push_back(&F);

    size_t Errors = 0, Warnings = 0;
    for (const auto &F : Findings) (F.Error ? Errors : Warnings)++;

    for (const auto &[Pkg, List] : ByPkg)
    {
        LogOut("audit", "── " + (Pkg.empty() ? std::string("(no package dir)") : Pkg));
        for (const Finding *F : List)
        {
            const std::string Line = "   [" + F->Kind + "] " + F->Node + ": " + F->Detail;
            if (F->Error) LogErr("audit", Line); else LogWarn("audit", Line);
        }
    }

    //Summarise by KIND too: a class of mistake repeated across many packages is one authoring bug, not N bugs.
    std::map<std::string, size_t> ByKind;
    for (const auto &F : Findings) ByKind[F.Kind]++;
    if (!ByKind.empty())
    {
        LogOut("audit", "By kind:");
        for (const auto &[K, C] : ByKind) LogOut("audit", "   " + std::to_string(C) + "  " + K);
    }

    //ByPkg groups FINDINGS, so it is empty on a clean run — the summary then read "across 0 package(s)",
    //i.e. exactly like a sweep that audited nothing. Count the bundles actually swept.
    std::set<std::string> Swept;
    for (const auto &L : Launchables) { const Node *N = Index.Find(L); if (N) Swept.insert(N->BundleDir); }
    LogOut("audit", "Audited " + std::to_string(Launchables.size()) + " launchable(s) across "
                    + std::to_string(Swept.size()) + " package(s): "
                    + std::to_string(Errors) + " error(s), " + std::to_string(Warnings) + " warning(s).");
    return Errors == 0 ? 0 : 1;
}
