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
                for (const std::string &Dep : ManifestModel::ResolveNodeOrder(Index, Sd, {})) Scope.insert(Dep);
            }
            if (Scope.empty()) { LogErr("validate-nodes", "scope '" + S + "' matched no package/node."); return 1; }
        }

        ManifestModel::ValidateNodeGraph(Index, Errors, Warnings, Scope.empty() ? nullptr : &Scope);
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
        const int Fixed = ManifestModel::FixCaseConflicts(Index, Log);
        for (const auto &Line : Log) LogOut("fix-case-conflicts", Line);
        LogSucc("fix-case-conflicts", "Rewrote " + std::to_string(Fixed) + " zip(s). Re-run --validate-nodes to confirm.");
        return 0;
    }

    //HEADLESS: reduce a version chain to one base full zip + per-version .vgdelta layers (byte-verified).
    //Each node after the first has its VFSZipLayer replaced by a VFSDeltaLayer diffed against the version
    //before it (overlay-relative chaining) and is reparented onto it; the now-redundant full zips are deleted.
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
            V v; v.node = n;
            for (int k = 0; k < (int)n->Layers.size(); ++k)
                if (n->Layers[k].value("TYPE", std::string()) == "VFSZipLayer") { v.layerIdx = k; break; }
            if (v.layerIdx < 0) { LogErr("convert-delta", "no VFSZipLayer in " + id); return 1; }
            fs::path local; std::string cid;
            ManifestModel::LayerLocator(n->Layers[v.layerIdx], n->BundleDir, local, cid);
            v.zipPath = local.string();
            v.target  = n->Layers[v.layerIdx].value("TARGET", std::string());
            if (!fs::exists(v.zipPath)) { LogErr("convert-delta", "missing zip for " + id + ": " + v.zipPath); return 1; }
            vs.push_back(std::move(v));
        }

        uint64_t before = 0; for (auto &v : vs) before += fs::file_size(v.zipPath);
        uint64_t after  = fs::file_size(vs[0].zipPath);   // the base full zip stays

        // Pass 1: generate + byte-verify every delta against the ORIGINAL prior zip (all still present), write
        // the .vgdelta and rewrite the node JSON. Deletion of the redundant full zips is deferred to pass 2.
        std::vector<std::string> toDelete;
        for (size_t i = 1; i < vs.size(); ++i)
        {
            auto base = std::make_shared<MapSrc>();
            auto tgt  = std::make_shared<MapSrc>();
            if (!base->Map(vs[i - 1].zipPath) || !tgt->Map(vs[i].zipPath)) { LogErr("convert-delta", "mmap failed"); return 1; }
            LogOut("convert-delta", "diff " + vs[i].node->NodeId + " against " + vs[i - 1].node->NodeId + " ...");
            // Spill scratch next to the archives, NOT in the system temp dir — /tmp is a tmpfs on most desktops,
            // so spilling several GB there would just move the memory pressure instead of relieving it.
            std::vector<uint8_t> delta = vgdelta::GenerateDelta(base->p, base->n, tgt->p, tgt->n,
                                                                vgdelta::DEFAULT_BLOCK,
                                                                vs[i].node->BundleDir.string());

            // byte-verify via the shared vgdelta::VerifyDelta (same routine the tests use — no drift).
            if (std::string Verr; !vgdelta::VerifyDelta(delta, base, *tgt, Verr))
            { LogErr("convert-delta", "VERIFY FAILED (" + Verr + ") — aborting, nothing changed"); return 1; }

            // write the delta blob + rewrite the node JSON (reparent + swap the zip layer for a delta layer).
            std::string blob = "delta_from__" + vs[i - 1].node->NodeId + ".vgdelta";
            { std::ofstream o(vs[i].node->BundleDir / blob, std::ios::binary); o.write((const char *)delta.data(), (std::streamsize)delta.size()); }
            after += delta.size();

            nlohmann::ordered_json J; { std::ifstream in(vs[i].node->File); in >> J; }
            nlohmann::ordered_json parents = J.value("PARENTS", nlohmann::ordered_json::array());
            if (!parents.is_array()) parents = nlohmann::ordered_json::array();
            bool have = false; for (auto &p : parents) if (p == vs[i - 1].node->NodeId) have = true;
            if (!have) parents.push_back(vs[i - 1].node->NodeId);
            J["PARENTS"] = parents;
            nlohmann::ordered_json deltaLayer{ {"TYPE", "VFSDeltaLayer"}, {"PATH", blob}, {"TARGET", vs[i].target} };
            // Cross-target: when the byte-base zip mounts at a DIFFERENT target than this node (e.g. a complete
            // archive at the package root diffed over a base zip at a sub-target), name it so the FS can pair them.
            if (vs[i - 1].target != vs[i].target) deltaLayer["BASE_TARGET"] = vs[i - 1].target;
            J["LAYERS"][vs[i].layerIdx] = deltaLayer;
            { std::ofstream o(vs[i].node->File); o << J.dump(4) << "\n"; }

            toDelete.push_back(vs[i].zipPath);
            LogSucc("convert-delta", "  " + vs[i].node->NodeId + " ✓ verified  (" + std::to_string(delta.size() >> 20) + " MB delta)");
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
    if (LaunchParameters.ListNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        auto Groups = PackageCatalog::PresentableGroups(Index);
        const auto Hyd = PackageCatalog::HydrationMap(Index);   // O(N+E) once, not per-launchable
        LogOut("list-nodes", std::to_string(Groups.size()) + " library tile(s):");
        for (const auto &G : Groups)
        {
            const Node *Rep = G.front();
            std::string Variants;
            for (const Node *N : G)
                Variants += " " + N->NodeId + ((Hyd.count(N->NodeId) && Hyd.at(N->NodeId).Hydrated) ? "[hydrated]" : "[remote]");
            LogOut("list-nodes", "  " + (Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId)
                   + "  (game " + Rep->GameKey() + "):" + Variants);
        }
        return 0;
    }

    //HEADLESS (node graph): launch a launchable node from the global, cross-bundle node index — the engine
    //resolves EVERYTHING natively from the node graph (LaunchResolver::InitializeFromNode). No manifest.
    return -1;   // no mode of this family requested
}
