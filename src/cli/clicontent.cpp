#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "nodegraph.h"        // gigagraph mint / frozen-DAG read-back (--mint)
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"

#include <QDir>
#include <QFile>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
#include "runnerinstall.h"

void CliModes::CollectCatalogTargets(const NodeIndex &Idx,
                                     std::vector<IpfsWrapper::FetchTarget> &Out, CatalogTargetStats *Stats,
                                     const std::function<void(const std::string &, const std::string &)> &OnSourceless)
{
    CatalogTargetStats St;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        ++St.Nodes;
        std::string Err;
        if (!PackageCatalog::CollectContentTargets(Idx, Id, {}, Out, &Err))
        {
            ++St.SourcelessNodes;                       // counted, reported, never fatal (see climodes.h)
            if (OnSourceless) OnSourceless(Id, Err);
        }
        if (N.IsVariant()) ++St.Launchables;   // (its runner chain's builds are catalog nodes → the IsRunner branch)
        if (N.IsRunner()) { ++St.Runners; (void)RunnerInstall::CollectRunnerNodeTargets(Idx, Id, Out, &Err); }
    }
    if (Stats) *Stats = St;
}

int CliModes::RunContentModes(LaunchParameters &LaunchParameters, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    (void)GlobalConfigJSON; (void)AppDataDir;
    // ── --download-all : the real download path (DownloadManager::beginDownload), applied to the WHOLE catalog.
    // Syncs every package source, then pools EVERY node's content targets + every launchable's resolved runner chain
    // + every runner node's build into ONE concurrent batch through the real DownloadQueue. Full mirror / pre-seed. ──
    if (LaunchParameters.DownloadAll)
    {
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 20; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogOut("download-all", std::string("online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()) + " — syncing sources");
        // Sync sources; the many-small-block meta trees can need several passes on a slow (Pinata-only) network —
        // each pass PERSISTS blocks and resumes, so retry until every configured source has indexed (bounded).
        // "Every source synced" = the sync's OWN predicate (PackageCatalog::SourceDirSynced: exists, readable,
        // non-empty) on the catalog's OWN path rule (PackageSourceDir: honours Settings.Paths.LibraryRoot and the
        // sanitized / CID-derived source name) — not a hand-built AppData/LIBRARY/<NAME> that an empty NAME or a
        // custom root would make trivially true, or never true.
        const auto Sources = GlobalConfigJSON["Settings"].value("PackageSources", nlohmann::ordered_json::array());
        size_t WantSources = 0;
        for (const auto &Src : Sources) if (!PackageCatalog::PackageSourceCID(Src).empty()) ++WantSources;
        if (WantSources == 0) { LogErr("download-all", "no CID-bearing package source configured — nothing to mirror"); return 1; }
        auto SyncedSources = [&]{
            std::error_code Ec; size_t Have = 0;
            for (const auto &Src : Sources)
            {
                if (PackageCatalog::PackageSourceCID(Src).empty()) continue;
                const std::string Dir = PackageCatalog::PackageSourceDir(GlobalConfigJSON, Src);
                if (PackageCatalog::SourceDirSynced(Dir, Ec)) ++Have;
                else if (Ec) LogWarn("download-all", "source dir " + Dir + ": " + Ec.message());
            }
            return Have;
        };
        NodeIndex Index; size_t HaveSources = 0;
        for (int pass = 1; pass <= 12; ++pass)
        {
            std::string SErr;
            PackageCatalog::SyncPackageSources(GlobalConfigJSON, &SErr);
            Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
            HaveSources = SyncedSources();
            LogOut("download-all", "sync pass " + std::to_string(pass) + ": " + std::to_string(HaveSources) + "/"
                   + std::to_string(WantSources) + " source(s), " + std::to_string(Index.Nodes.size()) + " node(s)"
                   + (SErr.empty() ? "" : "  (last: " + SErr + ")"));
            if (HaveSources == WantSources) break;   // sync completeness is the only gate (a runner-only catalog has 0 launchables)
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (HaveSources < WantSources)
        {
            LogErr("download-all", "only " + std::to_string(HaveSources) + "/" + std::to_string(WantSources)
                   + " source(s) synced — refusing to call a partial catalog a mirror");
            return 1;
        }
        // Collect targets for EVERYTHING (CollectCatalogTargets — the testable half of this mode: no fetches).
        std::vector<IpfsWrapper::FetchTarget> Targets; std::string Err;
        CliModes::CatalogTargetStats St;
        int reported = 0;
        CliModes::CollectCatalogTargets(Index, Targets, &St,
            [&reported](const std::string &Id, const std::string &E){ if (++reported <= 20) LogWarn("download-all", "content collect '" + Id + "': " + E); });
        LogOut("download-all", "collected " + std::to_string(Targets.size()) + " fetch target(s) (pre-dedup) from "
               + std::to_string(St.Nodes) + " node(s) [" + std::to_string(St.Launchables) + " launchable, "
               + std::to_string(St.Runners) + " runner]" + (St.SourcelessNodes ? ("; " + std::to_string(St.SourcelessNodes) + " node(s) had a missing/sourceless layer") : std::string()));
        if (Targets.empty()) { LogErr("download-all", "no fetch targets — sources did not sync?"); return 1; }
        const auto T0 = std::chrono::steady_clock::now();
        const bool Ok = IpfsWrapper::FetchTargetsConcurrent(Targets, &Err);
        const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        // Tally what actually landed on disk.
        std::error_code Ec; std::uintmax_t Bytes = 0; int Present = 0;
        for (const auto &T : Targets)
            if (std::filesystem::is_regular_file(T.LocalPath, Ec))
            {
                const std::uintmax_t Sz = std::filesystem::file_size(T.LocalPath, Ec);
                if (Ec) continue;                       // unreadable: not "present" (file_size returns uintmax_t(-1) on error)
                ++Present; Bytes += Sz;
            }
        LogOut("download-all", "materialized " + std::to_string(Present) + "/" + std::to_string(Targets.size())
               + " target(s), " + std::to_string(Bytes) + " bytes in " + std::to_string(Secs) + "s");
        if (!Ok) { LogErr("download-all", "batch reported a failure: " + Err); return 1; }
        // A mirror is EVERY target on disk. The batch can report success while a dest is missing (an Optional
        // target's fetch gave up) — the tally is the verdict, not the batch flag.
        if (Present != (int)Targets.size())
        {
            LogErr("download-all", "NOT a full mirror: " + std::to_string((int)Targets.size() - Present)
                   + " of " + std::to_string(Targets.size()) + " target(s) missing on disk");
            return 1;
        }
        LogSucc("download-all", "full mirror complete: " + std::to_string(Present) + " file(s), "
                + std::to_string(Bytes) + " bytes");
        return 0;
    }
    if (!LaunchParameters.FetchCid.empty())
    {
        LogOut("main.cpp", "Fetch test: " + LaunchParameters.FetchCid + " -> " + LaunchParameters.FetchDest);
        // Warm up first: wait for a real peer set (or 30 s) so the timed fetch reflects a RUNNING node (the GUI),
        // not a cold start with 0 peers + an empty DHT routing table.
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 30; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        // Optional direct peering: dial a known seed so the transfer is peer-to-peer direct (no DHT/relay lottery).
        if (!LaunchParameters.ConnectAddr.empty())
        {
            const bool Ok = IpfsWrapper::Connect(LaunchParameters.ConnectAddr);
            LogOut("main.cpp", std::string("direct connect to ") + LaunchParameters.ConnectAddr + (Ok ? " : ok" : " : FAILED"));
        }
        LogOut("main.cpp", std::string("warmed: online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()));
        const auto T0 = std::chrono::steady_clock::now();
        std::string Err;
        // QUEUE TEST (VG_QUEUE_TEST=1): enqueue the SAME CID at TWO dests through the CID-addressed DownloadQueue and
        // assert it fetched ONCE (dedup) and cross-dest materialized (dest2 hard-linked to dest — same inode). With
        // VG_FETCH_DEBUG=1 the log shows a single "fetchToPath ENTER" for the CID. Exercises the whole queue headlessly.
        if (std::getenv("VG_QUEUE_TEST"))
        {
            const std::string Dest2 = LaunchParameters.FetchDest + ".2";
            std::error_code Rc; std::filesystem::remove(Dest2, Rc);
            const bool Ok = IpfsWrapper::FetchTargetsConcurrent(
                { { LaunchParameters.FetchCid, LaunchParameters.FetchDest, false },
                  { LaunchParameters.FetchCid, Dest2,                      false } }, &Err);
            if (!Ok) { LogErr("main.cpp", "Queue test fetch failed: " + Err); return 1; }
            std::error_code E1, E2, E3;
            const bool Both  = std::filesystem::exists(LaunchParameters.FetchDest, E1) && std::filesystem::exists(Dest2, E2);
            const bool Same  = Both && std::filesystem::equivalent(LaunchParameters.FetchDest, Dest2, E3);
            const auto Sz1 = std::filesystem::file_size(LaunchParameters.FetchDest, E1);
            const auto Sz2 = std::filesystem::file_size(Dest2, E2);
            if (Both && Same && !E1 && !E2 && Sz1 == Sz2)
                LogSucc("main.cpp", "Queue test PASS: single fetch, cross-dest hard-link (" + std::to_string(Sz1) + " bytes, same inode)");
            else
            { LogErr("main.cpp", "Queue test FAIL: both=" + std::to_string(Both) + " same-inode=" + std::to_string(Same)
                     + " sz1=" + std::to_string(E1?0:Sz1) + " sz2=" + std::to_string(E2?0:Sz2)); return 1; }
            return 0;
        }
        if (LaunchParameters.FetchDirMode)
        {
            // Recursively materialize a FOLDER CID (the add-by-CID path) — verifies the node-file blocks are served.
            const std::string Got = IpfsWrapper::FetchDirToPath(LaunchParameters.FetchCid, LaunchParameters.FetchDest, &Err);
            const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
            if (Got.empty()) { LogErr("main.cpp", "Fetch (dir) failed: " + Err); return 1; }
            std::error_code Ec; std::uintmax_t Bytes = 0; int Files = 0;
            if (std::filesystem::is_directory(LaunchParameters.FetchDest, Ec))
                for (const auto &E : std::filesystem::recursive_directory_iterator(LaunchParameters.FetchDest, Ec))
                    if (E.is_regular_file(Ec)) { ++Files; Bytes += E.file_size(Ec); }
            LogSucc("main.cpp", "Fetched folder: " + std::to_string(Files) + " file(s), " + std::to_string(Bytes)
                    + " bytes in " + std::to_string(Secs) + "s");
            return 0;
        }
        // CONCURRENT-FETCH probe (VG_FETCH_MULTI="cid2,cid3,…"): fetch FetchCid + the listed CIDs together through the
        // real DownloadQueue (FetchTargetsConcurrent), to reproduce/diagnose concurrent downloads stalling. With
        // VG_FETCH_DEBUG the per-file bitswap sessions + stalls are visible.
        if (const char *ML = std::getenv("VG_FETCH_MULTI"))
        {
            std::vector<IpfsWrapper::FetchTarget> Targets{ { LaunchParameters.FetchCid, LaunchParameters.FetchDest + ".0", false } };
            std::string List = ML;
            int i = 1;
            while (!List.empty())
            {
                const size_t c = List.find(',');
                const std::string Cid = List.substr(0, c);
                if (!Cid.empty())
                    Targets.push_back({ Cid, LaunchParameters.FetchDest + "." + std::to_string(i++), false });
                if (c == std::string::npos) break;
                List = List.substr(c + 1);
            }
            LogOut("main.cpp", "concurrent probe: fetching " + std::to_string(Targets.size()) + " CIDs together");
            const bool Ok = IpfsWrapper::FetchTargetsConcurrent(Targets, &Err);
            const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
            LogOut("main.cpp", std::string("concurrent probe ") + (Ok ? "OK" : ("FAILED: " + Err)) + " in " + std::to_string(Secs) + "s");
            return Ok ? 0 : 1;
        }
        const std::string Got = IpfsWrapper::FetchToPath(LaunchParameters.FetchCid, LaunchParameters.FetchDest, &Err);
        const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        if (Got.empty()) { LogErr("main.cpp", "Fetch failed: " + Err); return 1; }
        std::error_code Ec; const auto Sz = std::filesystem::file_size(LaunchParameters.FetchDest, Ec);
        LogSucc("main.cpp", "Fetched " + std::to_string(Ec ? 0 : Sz) + " bytes in "
                + std::to_string(Secs) + "s  (" + std::to_string((Ec ? 0.0 : double(Sz)) / 1048576.0 / Secs) + " MB/s)");
        return 0;
    }

    //HEADLESS: seed a folder of published packages into the IPFS node (re-establish seeding from a master), then exit.
    if (!LaunchParameters.SeedDir.empty())
    {
        LogOut("main.cpp", std::string(LaunchParameters.SeedCoversOnly ? "Seeding COVERS ONLY from: " : "Seeding referenced content from: ")
                           + LaunchParameters.SeedDir);
        int Mismatched = 0;
        const int Seeded = PackageCatalog::SeedDirectory(LaunchParameters.SeedDir,
            [](int done, int total, const std::string &name){
                if (done == total || done % 10 == 0) LogOut("seed", std::to_string(done) + "/" + std::to_string(total) + "  " + name);
            }, &Mismatched, LaunchParameters.SeedCoversOnly, LaunchParameters.SeedOverwrite);
        LogSucc("main.cpp", "Seeded " + std::to_string(Seeded) + " file(s)"
                + (Mismatched ? ("; " + std::to_string(Mismatched) + " changed/un-seedable") : std::string()));
        return 0;
    }

    //HEADLESS: heal all configured package sources (re-point orphaned refs + prune stale pins), then exit. Same pass
    //the GUI runs on startup when it detects orphans — this is the manual/scriptable entry point.
    //HEADLESS: what a file's CID WOULD be. No node, nothing seeded — the direct answer to "do these bytes still
    //match the CID we published?", which is the CONTENT DRIFT class that made four packages unfetchable. Prints
    //"<cid>\t<path>"; exits non-zero if any file could not be hashed.
    if (!LaunchParameters.CidPaths.empty())
    {
        int Bad = 0;
        for (const std::string &P : LaunchParameters.CidPaths)
        {
            std::string Err;
            const std::string Cid = IpfsWrapper::ComputeCid(P, &Err);
            if (Cid.empty()) { LogErr("main.cpp", "could not hash " + P + ": " + Err); ++Bad; continue; }
            std::cout << Cid << "\t" << P << "\n";
        }
        return Bad ? 1 : 0;
    }

    //HEADLESS: can we actually SERVE this CID? Reads the whole DAG back through the same blockstore path bitswap
    //uses, so it catches a reference whose backing file exists but whose bytes changed — the failure that makes a
    //requesting peer hang rather than fail over. Exits non-zero when it is unservable.
    if (!LaunchParameters.VerifyCidArg.empty())
    {
        const std::string Err = IpfsWrapper::VerifyCid(LaunchParameters.VerifyCidArg);
        if (Err.empty()) { LogSucc("main.cpp", "servable: " + LaunchParameters.VerifyCidArg); return 0; }
        LogErr("main.cpp", "UNSERVABLE " + LaunchParameters.VerifyCidArg + " — " + Err);
        return 2;
    }

    //HEADLESS: merge-upgrade a package source to a new collection CID. Editing the CID in Settings does nothing on
    //its own (SyncPackageSources only fetches when the source dir is MISSING), and remove+re-add destroys every
    //hydrated content file, so this is the only correct way to move an existing install to a new mint.
    if (!LaunchParameters.UpgradeSourceName.empty())
    {
        std::string Err;
        const PackageCatalog::SourceUpgradePlan Plan = PackageCatalog::PlanSourceUpgrade(
            GlobalConfigJSON, LaunchParameters.UpgradeSourceName, LaunchParameters.UpgradeSourceCid, &Err);
        if (!Plan.Valid) { LogErr("main.cpp", "upgrade-source failed: " + Err); return 1; }

        auto Gb = [](long long B) { char T[32]; std::snprintf(T, sizeof T, "%.2f GB", (double)B / 1e9); return std::string(T); };
        LogOut("main.cpp", "Upgrade '" + Plan.Name + "': " + (Plan.OldCid.empty() ? "(none)" : Plan.OldCid) + " → " + Plan.NewCid);
        LogOut("main.cpp", "  packages ......... " + std::to_string(Plan.OldPackages) + " → " + std::to_string(Plan.NewPackages)
                           + " (" + std::to_string(Plan.SharedPackages) + " shared)");
        LogOut("main.cpp", "  manifests ........ +" + std::to_string(Plan.JsonAdded.size())
                           + " ~" + std::to_string(Plan.JsonChanged.size()) + " -" + std::to_string(Plan.JsonRemoved.size()));
        LogOut("main.cpp", "  content kept ..... " + std::to_string(Plan.ContentKeep.size()) + " file(s), " + Gb(Plan.KeptBytes)
                           + " (NOT re-downloaded)");
        LogOut("main.cpp", "  content moved .... " + std::to_string(Plan.ContentMove.size()) + " file(s)");
        LogOut("main.cpp", "  content new ...... " + std::to_string(Plan.ContentNew.size()) + " CID(s) to hydrate on demand");
        LogOut("main.cpp", "  deprecated ....... " + std::to_string(Plan.ContentDeprecate.size()) + " file(s), " + Gb(Plan.DeprecatedBytes)
                           + " → " + Plan.DeprecatedDir);
        if (Plan.SharedPackages == 0 && Plan.OldPackages > 0)
            LogWarn("main.cpp", "the new tree shares NO packages with the current source — this looks like a different "
                                "collection, not an upgrade (use --force to apply anyway)");
        if (LaunchParameters.UpgradeSourceDryRun) { LogSucc("main.cpp", "dry run — nothing changed."); return 0; }

        if (!PackageCatalog::ApplySourceUpgrade(GlobalConfigJSON, Plan, LaunchParameters.ForceOp, &Err))
        { LogErr("main.cpp", "upgrade-source failed: " + Err); return 1; }
        PackageCatalog::SyncPackageSources(GlobalConfigJSON);
        QFile CfgFile(AppDataDir.filePath("GlobalConfig.JSON"));
        if (!JSONOps::SaveJSON(&GlobalConfigJSON, &CfgFile))
        { LogErr("main.cpp", "upgrade applied but saving GlobalConfig.JSON failed"); return 1; }
        LogSucc("main.cpp", "Upgraded '" + Plan.Name + "' to " + Plan.NewCid);
        return 0;
    }

    if (LaunchParameters.NetTest)
    {
        // Same sweep as Settings -> Network, headless. One row per check; exit 1 on any FAIL so scripts and the
        // provision harness can gate on "the network is not the problem".
        bool Offline = false;
        const std::vector<IpfsWrapper::NetCheck> Checks = IpfsWrapper::NetworkTest(&Offline);
        if (Offline) { LogErr("net-test", "node is offline — nothing to probe"); return 1; }
        int Fails = 0;
        for (const auto &C : Checks)
        {
            const std::string Line = "[" + C.Status + "]  " + C.Name + " — " + C.Detail;
            if (C.Status == "fail") { ++Fails; LogErr("net-test", Line); }
            else if (C.Status == "warn") LogWarn("net-test", Line);
            else LogSucc("net-test", Line);
        }
        LogOut("net-test", Fails ? std::to_string(Fails) + " check(s) FAILED — the rows above say what to unblock."
                                 : "all checks passed — the network is not your problem.");
        return Fails ? 1 : 0;
    }

    if (LaunchParameters.HealPins)
    {
        // The explicit ops command is the COMPREHENSIVE one — it is meant to be the only thing anyone has to run,
        // so it verifies as well as repairs: re-point orphans, READ BACK every referenced CID (catching stale refs
        // whose file exists but whose bytes changed — invisible to a stat-only check and the reason peers hang),
        // repair those via drop-ref + re-add, and drop pins nothing references. The 90s background pass in AppModel
        // deliberately stays on the cheap stat-only path; this one reads the whole library.
        LogOut("main.cpp", "Healing package sources (deep: verifying every referenced CID by reading it back)…");
        const PackageCatalog::HealReport R =
            PackageCatalog::HealSourceContent(GlobalConfigJSON, PackageCatalog::HealOptions{ true, true });

        LogOut("main.cpp", "  verified servable ......... " + std::to_string(R.Verified));
        LogOut("main.cpp", "  files re-seeded (cheap pass) " + std::to_string(R.Repointed));
        LogOut("main.cpp", "  stale refs repaired ....... " + std::to_string(R.StaleRepaired));
        LogOut("main.cpp", "  stale pins pruned ......... " + std::to_string(R.PrunedUnservable));
        LogOut("main.cpp", "  unreferenced pins dropped . " + std::to_string(R.PrunedUnreferenced));

        // LOUD: these two are content problems no automatic repair may paper over — each one means some peer cannot
        // fetch something this node claims to publish. Non-zero exit so a script/CI cannot mistake it for success.
        for (const std::string &D : R.Drift)
            LogErr("main.cpp", "CONTENT DRIFT — published CID does not match the file: " + D);
        for (const std::string &U : R.Unrepaired)
            LogErr("main.cpp", "UNREPAIRABLE — content is gone or unreadable: " + U);
        if (!R.Drift.empty())
            LogErr("main.cpp", std::to_string(R.Drift.size()) + " node file(s) publish a CID nobody can fetch. "
                   "Repoint each SOURCE.CID to the actual content shown above and re-seed that package; the catalog "
                   "re-derives its CIDs on load, and re-publishing (Sharing tab) re-seeds the node blocks.");
        if (R.Ok()) { LogSucc("main.cpp", "Heal done — every referenced CID is servable."); return 0; }
        LogErr("main.cpp", "Heal finished with " + std::to_string(R.Drift.size() + R.Unrepaired.size())
               + " UNRESOLVED problem(s) — see above.");
        return 2;
    }

    //HEADLESS: import a runner NODE (fetch its IPFS build + generate its DEFPREFIX artifact) then exit. Composed from
    //the SAME primitives the GUI download pump uses — collect targets → fetch → generate DEFPREFIX — no bespoke path.
    if (!LaunchParameters.ImportRunnerId.empty())
    {
        //--import-runner <RUNNER_NODE_ID> (node runners have no variants — a stray ":suffix" is ignored).
        std::string Id = LaunchParameters.ImportRunnerId;
        if (auto Colon = Id.find(':'); Colon != std::string::npos) Id = Id.substr(0, Colon);
        LogOut("main.cpp", "Importing runner node: " + Id);
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::string Err;
        std::vector<IpfsWrapper::FetchTarget> Targets;
        bool Ok = RunnerInstall::CollectRunnerNodeTargets(Index, Id, Targets, &Err)
               && IpfsWrapper::FetchTargetsConcurrent(Targets, &Err);
        LogOut("main.cpp", Ok ? "Runner imported." : ("Runner import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: hydrate a library game in place (fetch its node closure's IPFS content) then exit. Resolve the
    //launchable node whose UID (or NODE_ID) matches the requested id, then hydrate its content closure.
    if (!LaunchParameters.ImportPackageUid.empty())
    {
        const std::string Uid = LaunchParameters.ImportPackageUid;
        LogOut("main.cpp", "Importing package (node closure) for: " + Uid);
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::string LaunchId;
        for (const auto &[NId, N] : Index.Nodes)
            if (N.IsVariant() && (N.Uid == Uid || N.NodeId == Uid)) { LaunchId = NId; break; }
        if (LaunchId.empty()) { LogErr("main.cpp", "No launchable node found for '" + Uid + "'."); return 1; }
        std::string Err;
        const bool Ok = PackageCatalog::HydrateNode(Index, LaunchId, {}, &Err, &GlobalConfigJSON);   // pool the runner chain → playable
        LogOut("main.cpp", Ok ? "Package imported." : ("Package import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: publish (dehydrate) a local package — seed its layer content over IPFS, record the CIDs into the
    //manifest fragments in place, and optionally export a manifest-only copy — then exit.
    if (!LaunchParameters.PublishPackageDir.empty())
    {
        const std::string Dir  = LaunchParameters.PublishPackageDir;
        const std::string Dest = LaunchParameters.PublishToDir;
        LogOut("main.cpp", "Publishing package: " + Dir + (Dest.empty() ? "" : (" -> " + Dest)));
        std::string Err;
        const bool Ok = PackageCatalog::PublishPackage(Dir, Dest, &Err);
        LogOut("main.cpp", Ok ? "Package published." : ("Package publish failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: recursively add a folder (of dehydrated packages) to IPFS BY REFERENCE and print its folder CID — the
    //value to hand out for the Library's "Add by CID". The folder's files are referenced in place, so keep the folder
    //on disk and run VidyaGod online to seed it. Content still seeds per-layer from wherever it lives.
    if (!LaunchParameters.PublishCidDir.empty())
    {
        std::string Err;
        const std::string Cid = IpfsWrapper::AddNoCopy(LaunchParameters.PublishCidDir, &Err);
        if (Cid.empty()) { LogErr("main.cpp", "publish-cid failed: " + Err); return 1; }
        // AddNoCopy eagerly announces the CID to the DHT (async), but this is a short-lived process — wait until the
        // provider record is actually out there (poll findprovs) so the CID is discoverable after we exit, instead of
        // killing the provide goroutine mid-flight. The long-running seeder (same peerID) then serves it.
        LogOut("main.cpp", "announcing " + Cid + " to the DHT…");
        for (int i = 0; i < 40 && IpfsWrapper::ProviderCount(Cid) < 1; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogSucc("main.cpp", "CID: " + Cid + " (providers: " + std::to_string(IpfsWrapper::ProviderCount(Cid)) + ")");
        std::cout << Cid << "\n";   // machine-readable on stdout
        return 0;
    }

    //HEADLESS: mint a JSON-only Meta-CID for a bundle or a collection of bundles — content-address content+covers,
    //then seed the *.json manifests IN PLACE (no staging dir), print the CID.
    if (!LaunchParameters.PublishMetaSrc.empty())
    {
        std::string Err;
        const std::string Cid = PackageCatalog::PublishMetaCid(LaunchParameters.PublishMetaSrc, &Err);
        if (Cid.empty()) { LogErr("main.cpp", "publish-meta failed: " + Err); return 1; }
        LogSucc("main.cpp", "Meta-CID: " + Cid);
        std::cout << Cid << "\n";   // machine-readable on stdout
        return 0;
    }

    //HEADLESS: MINT a working-tree bundle into the gigagraph — freeze its nodes to dag-json blocks (identity = CID),
    //print the launchable root CIDs (the library-list entries), then verify by re-reading the frozen DAG back. The
    //end-to-end proof of Mint + FreezeNodeJson + BuildFrozenIndex + DeriveIdentity on real data.
    if (!LaunchParameters.MintDir.empty())
    {
        const std::string Dir = LaunchParameters.MintDir;
        LogOut("main.cpp", "Minting working tree: " + Dir);
        std::error_code Ec;
        if (!fs::is_directory(Dir, Ec)) { LogErr("main.cpp", "not a directory: " + Dir); return 1; }

        // Gather the working tree RECURSIVELY (whole-library): cross-package edges (e.g. a game's content PARENTing a
        // shared node in ANOTHER bundle) only resolve when the whole library is gathered, since handles are globally
        // unique. This is the migration's model.
        std::map<std::string, nlohmann::ordered_json> Tree;
        std::map<std::string, fs::path> Dirs;   // NODE_ID handle -> the on-disk bundle it lives in (for BundleDir)
        NodeGraph::GatherWorkingTree(Dir, Tree, Dirs);
        if (Tree.empty()) { LogErr("main.cpp", "no nodes found in " + Dir); return 1; }
        LogOut("main.cpp", "gathered " + std::to_string(Tree.size()) + " node(s)");

        // Freeze deps-first.
        NodeGraph::MintResult MR;
        std::string Err;
        if (!NodeGraph::Mint(Tree, MR, &Err)) { LogErr("main.cpp", "mint failed: " + Err); return 1; }
        LogSucc("main.cpp", "minted " + std::to_string(MR.HandleToCid.size()) + " node(s), "
                + std::to_string(MR.Launchables.size()) + " launchable(s)");
        for (const auto &Cid : MR.Launchables) std::cout << Cid << "\n";   // machine-readable: the list entries

        // Verify: re-read the frozen DAG from the launchable roots. No unresolved blocks == a coherent, self-contained
        // closure. (ValidateNodeGraph errors about a missing runner are EXPECTED here — runners are minted separately.)
        std::vector<std::string> Missing;
        NodeIndex Frozen = NodeGraph::BuildFrozenIndex(MR.Launchables, &Missing);
        std::vector<std::string> ValErrs, ValWarns;
        ManifestModel::ValidateNodeGraph(Frozen, ValErrs, ValWarns);
        LogOut("main.cpp", "frozen read-back: " + std::to_string(Frozen.Nodes.size()) + " node(s), "
               + std::to_string(Missing.size()) + " unresolved block(s), "
               + std::to_string(ValErrs.size()) + " validate error(s) (runner-absent expected)");
        for (const auto &M : Missing) LogErr("main.cpp", "  unresolved: " + M);

        // Launch-readiness: derive the CID-keyed catalog straight from the on-disk working tree (FreezeToIndex, no
        // block writes) with BundleDir wired, and resolve a launchable's closure. This also proves the two freeze
        // paths agree — Mint's DagPut CIDs are keys in the DagCid-built catalog (same canonical encoding → same CID).
        std::string FErr;
        NodeIndex Cat = NodeGraph::FreezeToIndex(Tree, Dirs, &FErr);
        if (Cat.Nodes.empty()) { LogErr("main.cpp", "FreezeToIndex failed: " + FErr); return 1; }
        int WithDir = 0, DirPresent = 0;
        for (const auto &[C, N] : Cat.Nodes)
        {
            (void)C;
            if (N.BundleDir.empty()) continue;
            ++WithDir;
            std::error_code De;
            if (std::filesystem::is_directory(N.BundleDir, De)) ++DirPresent;
        }
        LogOut("main.cpp", "FreezeToIndex: " + std::to_string(Cat.Nodes.size()) + " CID-keyed node(s), "
               + std::to_string(WithDir) + " with BundleDir (" + std::to_string(DirPresent) + " present on disk)");
        if (!MR.Launchables.empty())
        {
            const std::string &L = MR.Launchables.front();
            const bool InCat = Cat.Find(L) != nullptr;   // Mint (DagPut) CID must be a key in the DagCid catalog
            std::vector<std::string> RMissing;
            const auto Order = ManifestModel::ResolveNodeOrder(Cat, L, {}, &RMissing);
            LogOut("main.cpp", "resolve " + L.substr(0, 20) + "…: in-catalog=" + std::string(InCat ? "yes" : "NO")
                   + ", closure=" + std::to_string(Order.size()) + " node(s), "
                   + std::to_string(RMissing.size()) + " missing edge(s)");
            if (!InCat) { LogErr("main.cpp", "CID MISMATCH: Mint and FreezeToIndex disagree — canonical encoding drift"); return 1; }
        }
        return Missing.empty() ? 0 : 1;
    }

    //HEADLESS: HYDRATE a launchable's closure from IPFS into the pretty on-disk checkout — the sharing CONSUMER: a
    //friend's / pasted launchable CID becomes a playable, locally-hydrated game. Prints the checkout dir.
    if (!LaunchParameters.HydrateCid.empty())
    {
        //DHT/bitswap need a bootstrapped routing table for a not-yet-local closure; a one-shot must wait for peers.
        for (int i = 0; i < 40 && IpfsWrapper::PeerCount() < 3; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::string Dest = LaunchParameters.HydrateDest.empty()
                               ? PackageCatalog::LibraryRootDir(GlobalConfigJSON) : LaunchParameters.HydrateDest;
        LogOut("main.cpp", "Hydrating " + LaunchParameters.HydrateCid + " -> " + Dest);
        std::string Err;
        const std::string Dir = NodeGraph::HydratePackage(Dest, LaunchParameters.HydrateCid, &Err);
        if (Dir.empty()) { LogErr("main.cpp", "hydrate failed: " + Err); return 1; }
        LogSucc("main.cpp", "Hydrated to " + Dir);
        std::cout << Dir << "\n";
        return 0;
    }

    //HEADLESS: validate the node graph (dangling/cyclic PARENTS, layer PATHs, runner resolution, ...). Bare form
    //scans the WHOLE catalog; `--validate-nodes <pkg>` scopes to one package (UID / bundle dir / node id) + its
    //PARENTS closure — a fast pre-publish check that never pays the cross-package content scan (e.g. a huge
    //delta-chained package you aren't touching). Cross-package content is still consulted for the in-scope nodes.
    return -1;   // no mode of this family requested
}
