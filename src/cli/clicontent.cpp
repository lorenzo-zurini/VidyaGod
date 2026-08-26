#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
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

int CliModes::RunContentModes(LaunchParameters &LaunchParameters, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    (void)GlobalConfigJSON; (void)AppDataDir;
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
    if (LaunchParameters.HealPins)
    {
        LogOut("main.cpp", "Healing package sources: re-pointing orphaned references, pruning stale pins…");
        const int Pruned = PackageCatalog::HealSourceContent(GlobalConfigJSON);
        LogSucc("main.cpp", "Heal done; " + std::to_string(Pruned) + " stale pin(s) pruned.");
        return 0;
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
            if (N.IsLaunchable() && (N.Uid == Uid || N.NodeId == Uid)) { LaunchId = NId; break; }
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

    //HEADLESS: re-mint ALL CIDs of a whole library (a dir of source-collection subdirs) across the 3-level schema in
    //one node session — content CIDs (per file), package meta-CIDs (per package, into Settings.PackageCids), collection
    //meta-CIDs (per source, into Settings.PackageSources) — persist the config, then print the list.
    if (!LaunchParameters.RemintLibraryDir.empty())
    {
        LogOut("main.cpp", "Re-minting all CIDs (3-level schema) for library: " + LaunchParameters.RemintLibraryDir);
        std::vector<PackageCatalog::RemintEntry> Rows;
        std::string Err;
        if (!PackageCatalog::RemintLibrary(LaunchParameters.RemintLibraryDir, GlobalConfigJSON, Rows, &Err))
        { LogErr("main.cpp", "remint-library failed: " + Err); return 1; }
        QFile CfgFile(AppDataDir.filePath("GlobalConfig.JSON"));
        if (!JSONOps::SaveJSON(&GlobalConfigJSON, &CfgFile))
            LogWarn("main.cpp", "remint done but saving GlobalConfig.JSON failed — CIDs printed below are still valid");
        // Machine-readable list on stdout: "<level>\t<name>\t<cid>".
        std::cout << "\n===== RE-MINTED CIDs (level\tname\tcid) =====\n";
        int Pkgs = 0, Cols = 0;
        for (const auto &R : Rows)
        {
            std::cout << R.Level << "\t" << R.Name << "\t" << R.Cid << "\n";
            (R.Level == "collection") ? ++Cols : ++Pkgs;
        }
        LogSucc("main.cpp", "Re-minted " + std::to_string(Pkgs) + " package + " + std::to_string(Cols)
                + " collection meta-CID(s); content CIDs written into the node JSONs (SOURCE.CID).");
        return 0;
    }

    //HEADLESS: validate the node graph (dangling/cyclic PARENTS, layer PATHs, runner resolution, ...). Bare form
    //scans the WHOLE catalog; `--validate-nodes <pkg>` scopes to one package (UID / bundle dir / node id) + its
    //PARENTS closure — a fast pre-publish check that never pays the cross-package content scan (e.g. a huge
    //delta-chained package you aren't touching). Cross-package content is still consulted for the in-scope nodes.
    return -1;   // no mode of this family requested
}
