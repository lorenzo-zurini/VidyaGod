#include "pkggraph.h"   // AddNodeRef
#include "noderefs.h"
#include "cid.h"      // a rewritten node's new name, for the version that contains it
#include "installed.h"
#include "shelf.h"
#include "validate.h"
#include "fold.h"   // Fold::TypeOf — a layer's one type key
#include "cli/climodes.h"
#include "main.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "instancestore.h"   // --list-nodes names each game's instances
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "jsonoperations.h"

#include <QDate>
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
#include "vgdelta.h"
#include "zipscan.h"

int CliModes::RunMaintenanceModes(LaunchParameters &LaunchParameters, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir)
{
    (void)GlobalConfigJSON; (void)AppDataDir;
    if (LaunchParameters.AuditPackages)
        return CliModes::RunAuditPackages(GlobalConfigJSON, LaunchParameters.ValidateScope);
    if (LaunchParameters.ValidateNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);   // repos + locally-added packages
        std::vector<std::string> Errors, Warnings;

        std::set<std::string> Scope;
        if (!LaunchParameters.ValidateScope.empty())
        {
            const std::string &S = LaunchParameters.ValidateScope;
            std::error_code Ec;
            const std::string SAbs = std::filesystem::exists(S, Ec)
                ? std::filesystem::weakly_canonical(S, Ec).string() : std::string();
            //Seed nodes belonging to the requested package: matched by node id, package UID, bundle-dir path
            //(exact/prefix), or bundle-dir name. Each seed then pulls in its whole PARENTS closure (as the editor does).
            std::vector<std::string> Seeds;
            for (const auto &[Id, N] : Index.Nodes)
            {
                bool Match = (Id == S) || (!N.Uid.empty() && N.Uid == S)
                          || (std::filesystem::path(N.BundleDir).filename().string() == S);
                if (!Match && !SAbs.empty() && !N.BundleDir.empty())
                {
                    const std::string B = std::filesystem::weakly_canonical(N.BundleDir, Ec).string();
                    Match = (B == SAbs) || (B.rfind(SAbs + "/", 0) == 0);
                }
                if (Match) Seeds.push_back(Id);
            }
            for (const std::string &Sd : Seeds)
            {
                Scope.insert(Sd);
                for (const std::string &Dep : ManifestModel::Closure(Index, Sd)) Scope.insert(Dep);
            }
            if (Scope.empty()) { LogErr("validate-nodes", "scope '" + S + "' matched no package/node."); return 1; }
        }

        Validate::Nodes(Index, Errors, Warnings, Scope.empty() ? nullptr : &Scope);
        for (const auto &W : Warnings) LogWarn("validate-nodes", W);
        for (const auto &E : Errors)   LogErr ("validate-nodes", E);
        const std::string Extent = Scope.empty() ? (std::to_string(Index.Nodes.size()) + " node(s)")
                                                  : (std::to_string(Scope.size()) + " node(s) in scope '" + LaunchParameters.ValidateScope + "'");
        LogOut("validate-nodes", "Validated " + Extent + ": "
               + std::to_string(Errors.size()) + " error(s), " + std::to_string(Warnings.size()) + " warning(s).");
        return Errors.empty() ? 0 : 1;
    }

    //HEADLESS: resolve cross-layer case conflicts by canonicalizing the higher-priority layers' zip entries to
    //the base layer's case (unpack→rename→repackage STORE), so patches/add-ons override cleanly, then exit.
    if (LaunchParameters.FixCaseConflicts)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        std::vector<std::string> Log;
        const int Fixed = Validate::FixCaseConflicts(Index, Log);
        for (const auto &Line : Log) LogOut("fix-case-conflicts", Line);
        LogSucc("fix-case-conflicts", "Rewrote " + std::to_string(Fixed) + " zip(s). Re-run --validate-nodes to confirm.");
        return 0;
    }

    //HEADLESS: reduce a version chain to one base full zip + per-version .vgdelta layers (byte-verified).
    //Each node after the first has its ZIP layer replaced by a DELTA diffed against the version before it and
    //contains that version (a NODE layer); the now-redundant full zips are listed for deletion.
    if (!LaunchParameters.ConvertDeltaChain.empty())
    {
        namespace fs = std::filesystem;
        const auto &Chain = LaunchParameters.ConvertDeltaChain;
        if (Chain.size() < 2) { LogErr("convert-delta", "need >=2 nodes: <baseNode> <newer> [<newer>...]"); return 1; }
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);

        // Archives are MAPPED, not read into RAM (see MmapByteSource in the FS repo — it replaced the local
        // MapSrc): a chain step holds base + target open at once, and eager reads OOM-killed a 32 GB desktop.
        using MapSrc = MmapByteSource;

        struct V { const Node *node = nullptr; int layerIdx = -1; std::string zipPath, target; };
        std::vector<V> vs;
        for (const auto &id : Chain)
        {
            const Node *n = Index.Find(id);
            if (!n) { LogErr("convert-delta", "node not found: " + id); return 1; }
            //It rewrites node files: a node without one of its own (a package's working tree names its nodes by handle,
            //in one document) is the editor's to convert ("-> delta").
            if (n->File.empty() || !fs::exists(n->File))
            { LogErr("convert-delta", "node " + id + " has no node file of its own here — convert it in the package editor (-> delta)"); return 1; }
            V v; v.node = n;
            for (int k = 0; k < (int)n->ContentLayers.size(); ++k)
                if (n->ContentLayers[k].value("TYPE", std::string()) == "VFSZipLayer") { v.layerIdx = k; break; }
            if (v.layerIdx < 0) { LogErr("convert-delta", "no VFSZipLayer in " + id); return 1; }
            fs::path local; std::string cid;
            ManifestModel::LayerLocator(n->ContentLayers[v.layerIdx], n->BundleDir, local, cid);
            v.zipPath = local.string();
            v.target  = n->ContentLayers[v.layerIdx].value("TARGET", std::string());
            if (!fs::exists(v.zipPath)) { LogErr("convert-delta", "missing zip for " + id + ": " + v.zipPath); return 1; }
            vs.push_back(std::move(v));
        }

        uint64_t before = 0; for (auto &v : vs) before += fs::file_size(v.zipPath);
        uint64_t after  = fs::file_size(vs[0].zipPath);   // the base full zip stays

        for (size_t i = 1; i < vs.size(); ++i)
            if (vs[i].target != vs[i - 1].target)
            { LogErr("convert-delta", vs[i].node->NodeId + " would contain " + vs[i - 1].node->NodeId + ", which mounts at '"
                     + vs[i - 1].target + "', not '" + vs[i].target + "': its files would show beside the newer version's — nothing changed"); return 1; }
        //Each delta names its base by CID: the first version's zip (its SOURCE, or its CID computed and stamped on it),
        //then each version's own delta — the layer that rebuilds the bytes the next one is diffed over.
        nlohmann::ordered_json J0; { std::ifstream in(vs[0].node->File); in >> J0; }
        int Z0 = -1;
        for (int k = 0; k < (int)J0["LAYERS"].size() && Z0 < 0; ++k) if (Fold::TypeOf(J0["LAYERS"][(size_t)k]) == "ZIP") Z0 = k;
        if (Z0 < 0) { LogErr("convert-delta", "node " + vs[0].node->NodeId + " has no ZIP layer in " + vs[0].node->File.string()); return 1; }
        const bool Stamp = !(J0["LAYERS"][(size_t)Z0].contains("SOURCE") && J0["LAYERS"][(size_t)Z0]["SOURCE"].is_string());
        //Rewriting a node renames it: one outside the chain that contains a version rewritten here would be left naming
        //a node that is gone. Refused before anything is written (the editor re-mints a package whole: convert there).
        {
            std::set<std::string> Rewritten;
            if (Stamp) Rewritten.insert(vs[0].node->Key());
            for (size_t i = 1; i < vs.size(); ++i) Rewritten.insert(vs[i].node->Key());
            std::set<std::string> Chain;
            for (const auto &v : vs) Chain.insert(v.node->Key());
            for (const auto &[K, N] : Index.Nodes)
                if (!Chain.count(K))
                    for (const std::string &R : N.Refs)
                        if (Rewritten.count(R))
                        { LogErr("convert-delta", "node " + (N.NodeId.empty() ? K : N.NodeId) + " contains " + R + ", which this would rewrite "
                                 "and so rename — convert the chain in the package editor (-> delta) — nothing changed"); return 1; }
        }
        // Pass 1: generate + byte-verify every delta against the ORIGINAL prior zip (all still present) and name it.
        // Nothing is written to a node until every one has succeeded; a failure removes the deltas made so far.
        std::string Cerr;
        const std::string FirstCid = Stamp ? IpfsWrapper::ComputeCid(vs[0].zipPath, &Cerr)
                                           : J0["LAYERS"][(size_t)Z0]["SOURCE"].get<std::string>();
        if (FirstCid.empty()) { LogErr("convert-delta", "could not compute the CID of " + vs[0].zipPath + " (" + Cerr + ") — nothing changed"); return 1; }
        struct Made { std::filesystem::path Out; std::string Blob, Cid; };
        std::vector<Made> made;
        const auto Undo = [&] { std::error_code ec; for (const Made &m : made) std::filesystem::remove(m.Out, ec); };
        for (size_t i = 1; i < vs.size(); ++i)
        {
            auto base = std::make_shared<MapSrc>();
            auto tgt  = std::make_shared<MapSrc>();
            if (!base->Map(vs[i - 1].zipPath) || !tgt->Map(vs[i].zipPath)) { Undo(); LogErr("convert-delta", "mmap failed — nothing changed"); return 1; }
            LogOut("convert-delta", "diff " + vs[i].node->NodeId + " against " + vs[i - 1].node->NodeId + " ...");
            // Spill scratch next to the archives, NOT in the system temp dir — /tmp is a tmpfs on most desktops,
            // so spilling several GB there would just move the memory pressure instead of relieving it.
            // Streamed into a ".making" file beside the archive (never resident: a big jump is gigabytes; not ".part",
            // a fetch's crash marker), verified from there, and only then named.
            const std::string blob = "delta_from__" + vs[i - 1].node->NodeId + ".vgdelta";
            const std::filesystem::path out = vs[i].node->BundleDir / blob, part = vs[i].node->BundleDir / (blob + ".making");
            std::error_code rc;
            if (std::string Gerr; !vgdelta::GenerateDeltaToFile(base->p, base->n, tgt->p, tgt->n, part.string(), Gerr,
                                                               vgdelta::DEFAULT_BLOCK, vs[i].node->BundleDir.string()))
            { Undo(); LogErr("convert-delta", "delta generation failed (" + Gerr + ") — aborting, nothing changed"); return 1; }

            // byte-verify via the shared vgdelta::VerifyDelta (same routine the tests use — no drift).
            {
                auto written = std::make_shared<MmapByteSource>();
                std::string Verr = "could not read the delta back";
                if (!written->Map(part.string()) || !vgdelta::VerifyDelta(std::shared_ptr<ByteSource>(written), base, *tgt, Verr))
                { written.reset(); std::filesystem::remove(part, rc); Undo(); LogErr("convert-delta", "VERIFY FAILED (" + Verr + ") — aborting, nothing changed"); return 1; }
            }
            std::filesystem::rename(part, out, rc);
            if (rc) { std::filesystem::remove(part, rc); Undo(); LogErr("convert-delta", "could not name " + out.string() + " — nothing changed"); return 1; }
            const std::string blobCid = IpfsWrapper::ComputeCid(out.string(), &Cerr);
            made.push_back({ out, blob, blobCid });
            if (blobCid.empty()) { Undo(); LogErr("convert-delta", "could not compute the CID of " + out.string() + " (" + Cerr + ") — nothing changed"); return 1; }
            after += std::filesystem::file_size(out, rc);
            LogSucc("convert-delta", "  " + vs[i].node->NodeId + " ✓ verified  (" + std::to_string(std::filesystem::file_size(out, rc) >> 20) + " MB delta)");
        }

        // Pass 2: the nodes. The first version carries the CID it is named by; each newer one contains the version before
        // it (as it is now: a rewrite renames a node) and swaps its zip for the delta naming that version's layer.
        std::string prevCid = FirstCid, prevKey = vs[0].node->Key();
        if (Stamp)
        {
            J0["LAYERS"][(size_t)Z0]["SOURCE"] = FirstCid;
            std::ofstream o(vs[0].node->File); o << J0.dump(4) << "\n";
            prevKey = Cid::OfNode(J0);
        }
        std::vector<std::string> toDelete;
        for (size_t i = 1; i < vs.size(); ++i)
        {
            nlohmann::ordered_json J; { std::ifstream in(vs[i].node->File); in >> J; }
            int Raw = -1;
            if (ManifestModel::IsNodeObject(J))
                for (int k = 0; k < (int)J["LAYERS"].size() && Raw < 0; ++k) if (Fold::TypeOf(J["LAYERS"][(size_t)k]) == "ZIP") Raw = k;
            if (Raw < 0) { LogErr("convert-delta", "node " + vs[i].node->NodeId + " has no ZIP layer in " + vs[i].node->File.string()); return 1; }
            NodeRefs::Add(J, prevKey);
            Raw = -1;
            for (int k = 0; k < (int)J["LAYERS"].size() && Raw < 0; ++k)        // the insert may have shifted it
                if (Fold::TypeOf(J["LAYERS"][(size_t)k]) == "ZIP") Raw = k;
            nlohmann::ordered_json &L0 = J["LAYERS"][(size_t)Raw];
            nlohmann::ordered_json D = { {"DELTA", made[i - 1].Blob}, {"SOURCE", made[i - 1].Cid},
                                         {"BASE_SOURCES", nlohmann::ordered_json::array({ prevCid })} };
            for (const char *K : { "TARGET", "WHEN", "COMMENT" }) if (L0.contains(K)) D[K] = L0[K];
            L0 = std::move(D);
            { std::ofstream o(vs[i].node->File); o << J.dump(4) << "\n"; }
            prevCid = made[i - 1].Cid;
            prevKey = Cid::OfNode(J);
            toDelete.push_back(vs[i].zipPath);
        }

        // The old full zips are now redundant (byte-verified recoverable from base + deltas), but deletion is
        // left to the caller so the conversion stays reversible until they choose to reclaim the space.
        uint64_t reclaim = 0;
        for (const auto &z : toDelete) { std::error_code ec; reclaim += fs::file_size(z, ec); }
        LogSucc("convert-delta", "chain reduced (verified): base+deltas = " + std::to_string(after >> 20)
                + " MB vs " + std::to_string(before >> 20) + " MB of full zips ("
                + std::to_string(before ? 100 - after * 100 / before : 0) + "% smaller). Reclaim "
                + std::to_string(reclaim >> 20) + " MB by deleting the now-unreferenced full zips:");
        for (const auto &z : toDelete) LogOut("convert-delta", "  rm  " + z);
        return 0;
    }

    //HEADLESS: print the presentable library tiles (grouped launchable nodes) + hydration status.
    //The golden marker is the user's note on a game, kept in GlobalConfig only (PackageCatalog::SetGolden); a UID no card
    //of the library carries is refused, so a typo does not mark nothing. A running GUI keeps its own copy of the config:
    //what it saves next wins.
    if (!LaunchParameters.GoldenUid.empty())
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        bool Known = false;
        for (const auto &T : Shelf::Tiles(Index))
            for (const Node *N : T.Rows) Known = Known || N->GameKey() == LaunchParameters.GoldenUid;
        if (!Known)
        {
            LogErr("golden", "No game in the library has the package UID " + LaunchParameters.GoldenUid + ".");
            return 1;
        }
        PackageCatalog::SetGolden(GlobalConfigJSON, LaunchParameters.GoldenUid, LaunchParameters.GoldenOn,
                                  QDate::currentDate().toString(Qt::ISODate).toStdString());
        if (!JSONOps::SaveConfig(GlobalConfigJSON, AppDataDir.absolutePath().toStdString()))
        {
            LogErr("golden", "GlobalConfig.JSON could not be written.");
            return 1;
        }
        LogSucc("golden", LaunchParameters.GoldenUid + (LaunchParameters.GoldenOn ? " marked golden." : " unmarked."));
        return 0;
    }

    if (LaunchParameters.ListNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        const auto Tiles = Shelf::Tiles(Index);
        const auto Hyd = Installed::HydrationMap(Index);   // O(N+E) once, not per-launchable
        LogOut("list-nodes", std::to_string(Tiles.size()) + " library tile(s):");
        for (const auto &T : Tiles)
        {
            std::string Variants;
            for (const Node *N : T.Rows)
                Variants += " " + (N->Variant.empty() ? N->NodeId : N->Variant)
                          + ((Hyd.count(N->Key()) && Hyd.at(N->Key()).Hydrated) ? "[hydrated]" : "[remote]");
            const std::string Parent = T.Tile.value("PARENTUID", std::string());
            //The game's instances (settings + saves), last played first — the names --instance takes.
            std::string Instances;
            if (!T.Rows.empty())
                for (const std::string &I : InstanceStore::List(GlobalConfigJSON, T.Rows.front()->GameKey()))
                    Instances += (Instances.empty() ? "; instances: " : ", ") + I;
            const std::string Golden = T.Rows.empty() ? std::string()
                                     : PackageCatalog::GoldenSince(GlobalConfigJSON, T.Rows.front()->GameKey());
            LogOut("list-nodes", "  " + T.Tile.value("TITLE", T.Uid) + "  (tile " + T.Uid
                   + (Parent.empty() ? std::string() : ", in " + Parent) + "):" + Variants + Instances
                   + (Golden.empty() ? std::string() : "; golden since " + Golden));
        }
        return 0;
    }

    //HEADLESS (node graph): launch a launchable node from the global, cross-bundle node index — the engine
    //resolves EVERYTHING natively from the node graph (LaunchResolver::InitializeFromNode). No manifest.
    return -1;   // no mode of this family requested
}
