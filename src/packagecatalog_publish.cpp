#include "packagecatalog.h"
#include "packagecatalog_p.h"
#include "apppaths.h"
#include "manifestmodel.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "ipfswrapper.h"
#include "varsubst.h"
#include "launchresolver.h"
#include "runnerinstall.h"
#include "launchparams.h"

#include <QDir>
#include <QFile>
#include <cstdint>
#include <map>
#include <set>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <vector>

using namespace ManifestModel;

// P6 split: the PUBLISH/SEED surface of the catalog service (hydrate->publish, content seeding, meta-CID
// minting, orphan healing) — the query/index/sync/user-settings surface stays in packagecatalog.cpp.

namespace PackageCatalog {


// ----- import / publish -----


bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::PublishPackage", M); return false; };

    std::error_code Ec;
    const std::filesystem::path Pkg(PackageDir);
    if (!std::filesystem::is_directory(Pkg, Ec)) return Fail("not a package directory: " + PackageDir);

    if (!IpfsWrapper::DaemonRunning())
        LogWarn("PackageCatalog::PublishPackage",
                "IPFS node not online yet — CIDs will be computed but content seeds to peers only once it connects.");

    int Seeded = 0, Walked = 0, Covers = 0;

    //Walk every *.json fragment directly (no assemble/decompose round-trip — preserves each subcomponent's exact
    //file placement). Content-address VFS layers AND cover assets in place; re-save only mutated fragments.
    for (const auto &Entry : std::filesystem::directory_iterator(Pkg, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;
        QFile FragFile(QString::fromStdString(Entry.path().string()));
        nlohmann::ordered_json Frag;
        if (JSONOps::LoadJSON(&FragFile, &Frag)) continue;                       // LoadJSON returns true on FAILURE

        bool Mutated = false;

        //Content layers: keep PATH, add SOURCE:{ipfs,CID} (idempotent — skip those already carrying a CID).
        if (Frag.contains("COMPONENTS") && Frag["COMPONENTS"].is_array())
        for (auto &C : Frag["COMPONENTS"])
        {
            if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
            for (auto &S : C["SUBCOMPONENTS"])
            {
                if (!IsVfsLayer(LayerType(S))) continue;
                ++Walked;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Pkg, Local, Cid);

                std::error_code Rc;
                if (!Cid.empty()) continue;                                      // already has an ipfs CID — idempotent
                if (!std::filesystem::exists(Local, Rc)) continue;               // no local content to seed
                std::string Err;
                const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
                if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");

                nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object())
                                                 ? S["SOURCE"] : nlohmann::ordered_json::object();
                Src["TYPE"] = "ipfs";                                            // keep any existing SOURCE.PATH override
                Src["CID"]  = NewCid;
                S["SOURCE"] = std::move(Src);
                Mutated = true;
                ++Seeded;
            }
        }

        //Cover art: content-address the curated GAMES[].METADATA.COVER like a layer — keep the filename in PATH, add
        //SOURCE:{ipfs,CID}. Upgrades the legacy bare-string form; idempotent once a CID is present.
        auto SeedCover = [&](nlohmann::ordered_json &Holder)
        {
            if (!Holder.contains("COVER")) return;
            nlohmann::ordered_json &Cover = Holder["COVER"];
            std::string File;
            if (Cover.is_string()) File = Cover.get<std::string>();
            else if (Cover.is_object())
            {
                if (Cover.contains("SOURCE") && Cover["SOURCE"].is_object()
                    && !Cover["SOURCE"].value("CID", std::string()).empty()) return;   // already addressed
                File = Cover.value("PATH", std::string());
            }
            else return;
            if (File.empty()) return;
            std::error_code Rc;
            const std::filesystem::path Local = Pkg / File;
            if (!std::filesystem::exists(Local, Rc)) return;                      // not a local file (CID-only ref)
            std::string Err;
            const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
            if (NewCid.empty()) { LogWarn("PackageCatalog::PublishPackage", "could not seed cover " + Local.string() + " (" + Err + ")"); return; }
            Cover = nlohmann::ordered_json{ {"PATH", File}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", NewCid}}} };
            Mutated = true;
            ++Covers;
        };
        if (Frag.contains("GAMES") && Frag["GAMES"].is_array())
        for (auto &G : Frag["GAMES"])
        {
            if (!G.is_object()) continue;
            if (G.contains("METADATA") && G["METADATA"].is_object()) SeedCover(G["METADATA"]);
            SeedCover(G);                                                         // legacy game-level COVER
        }

        //Node files (everything-is-a-node): seed VFS layers in LAYERS + the cover on the DeclareLibraryItem layer.
        if (Frag.contains("NODE_ID") && Frag["NODE_ID"].is_string())
        {
            if (Frag.contains("LAYERS") && Frag["LAYERS"].is_array())
            for (auto &S : Frag["LAYERS"])
            {
                if (!IsVfsLayer(LayerType(S))) continue;
                ++Walked;
                std::filesystem::path Local; std::string Cid;
                LayerLocator(S, Pkg, Local, Cid);
                std::error_code Rc;
                if (!Cid.empty()) continue;                                      // already addressed — idempotent
                if (!std::filesystem::exists(Local, Rc)) continue;               // no local content to seed
                std::string Err;
                const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
                if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");
                nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object()) ? S["SOURCE"] : nlohmann::ordered_json::object();
                Src["TYPE"] = "ipfs"; Src["CID"] = NewCid; S["SOURCE"] = std::move(Src);
                Mutated = true; ++Seeded;
            }
            //Cover art: node-native covers live on a Declare* layer's COVER field (DeclareLibraryItem), not a top-level
            //META.COVER (that was the pre-Declare* shape). Seed whichever is present.
            if (Frag.contains("LAYERS") && Frag["LAYERS"].is_array())
                for (auto &L : Frag["LAYERS"]) if (L.is_object() && L.contains("COVER")) SeedCover(L);
            if (Frag.contains("META") && Frag["META"].is_object()) SeedCover(Frag["META"]);
        }

        if (Mutated && !JSONOps::SaveJSON(&Frag, &FragFile))
            return Fail("could not write annotated manifest fragment: " + Entry.path().string());
    }
    LogSucc("PackageCatalog::PublishPackage", "Dehydrated " + PackageDir + " (" + std::to_string(Seeded)
            + " of " + std::to_string(Walked) + " layer(s) + " + std::to_string(Covers) + " cover(s) newly seeded)");

    //Export the dehydrated manifest, if requested — a clean manifest-only copy (no image bytes; covers travel as CIDs).
    if (!DehydratedDestDir.empty())
    {
        std::filesystem::remove_all(DehydratedDestDir, Ec);
        const int Copied = MirrorDehydrated(PackageDir, DehydratedDestDir);
        LogSucc("PackageCatalog::PublishPackage", "Exported dehydrated manifest to " + DehydratedDestDir
                + " (" + std::to_string(Copied) + " JSON fragment(s))");
    }
    return true;
}

std::map<std::string, std::string> SeedTargets(const std::string &Dir, bool CoversOnly)
{
    namespace fs = std::filesystem;
    // Every CID-referenced local file (path → recorded SOURCE CID) across a folder's node JSONs, de-duped by path so a
    // file referenced by several nodes is seeded once. Pure (no IPFS) — the reference source of what SeedDirectory adds.
    std::map<std::string, std::string> ToSeed;
    std::error_code Ec;
    for (fs::recursive_directory_iterator It(Dir, fs::directory_options::skip_permission_denied, Ec), End;
         It != End; It.increment(Ec))
    {
        if (Ec) { Ec.clear(); continue; }
        if (!It->is_regular_file(Ec) || It->path().extension() != ".json") continue;

        nlohmann::ordered_json J;
        { std::ifstream F(It->path()); if (!F) continue; try { F >> J; } catch (...) { continue; } }
        if (!J.is_object()) continue;
        const fs::path Bundle = It->path().parent_path();

        auto Consider = [&](const nlohmann::ordered_json &Obj) {            // an object with PATH + SOURCE{ipfs,CID}
            if (!Obj.is_object()) return;
            const std::string Path = Obj.value("PATH", std::string());
            if (Path.empty() || !Obj.contains("SOURCE") || !Obj["SOURCE"].is_object()) return;
            const auto &S = Obj["SOURCE"];
            if (S.value("TYPE", std::string()) != "ipfs") return;
            const std::string Cid = S.value("CID", std::string());
            if (Cid.empty()) return;
            const fs::path Local = Bundle / Path;
            if (fs::exists(Local, Ec)) ToSeed[Local.string()] = Cid;
        };

        if (!CoversOnly && J.contains("LAYERS") && J["LAYERS"].is_array())
            for (const auto &L : J["LAYERS"]) Consider(L);                        // content layers (skipped in covers-only)
        // Cover art (ALWAYS seeded): node-native covers live on a Declare* layer's COVER field (DeclareLibraryItem);
        // legacy manifests put it at top-level META.COVER. Both are {PATH, SOURCE:{ipfs,CID}} like a layer.
        if (J.contains("LAYERS") && J["LAYERS"].is_array())
            for (const auto &L : J["LAYERS"])
                if (L.is_object() && L.contains("COVER") && L["COVER"].is_object()) Consider(L["COVER"]);
        if (J.contains("META") && J["META"].is_object() && J["META"]["COVER"].is_object())
            Consider(J["META"]["COVER"]);                                        // legacy pre-Declare* format
    }
    return ToSeed;
}

int SeedDirectory(const std::string &Dir,
                  const std::function<void(int, int, const std::string &)> &Progress,
                  int *Mismatched, bool CoversOnly, bool Overwrite)
{
    namespace fs = std::filesystem;
    if (Mismatched) *Mismatched = 0;

    // 1) Collect every CID-referenced content file (path → recorded SOURCE CID) from the bundles' node JSONs.
    const std::map<std::string, std::string> ToSeed = SeedTargets(Dir, CoversOnly);

    // 2) Add each by reference (re-hash → filestore ref + pin + reprovide). Count parity matches vs changed files.
    //    Modes: ADDITIVE (default) skips a CID the node already holds with an intact backing file (no re-hash); it
    //    still re-points ORPHANED references (backing file gone — the seed-source moved) and adds new content.
    //    OVERWRITE re-references every file. Either way, a still-held reference is dropped first (the node's filestore
    //    skips re-adding a block it already has, so the stale reference must go before AddNoCopy can re-point it).
    int Seeded = 0, Skipped = 0, Done = 0;
    const int Total = (int)ToSeed.size();
    for (const auto &[Path, Cid] : ToSeed)
    {
        const bool Has     = IpfsWrapper::HasLocal(Cid);
        const bool Orphan  = Has && IpfsWrapper::CidMissing(Cid);
        if (Has && !Orphan && !Overwrite) { ++Skipped; ++Seeded; }   // already seeded + intact → leave it (counts as seeded)
        else
        {
            if (Orphan)                                              // observability: the self-heal case (backing moved/re-created)
                LogOut("PackageCatalog::SeedDirectory", "re-pointing orphaned reference " + Cid + " -> " + Path);
            if (Has) IpfsWrapper::DropRef(Cid);                       // clear the stale/old reference so the re-add isn't deduped
            std::string Err;
            const std::string Got = IpfsWrapper::AddNoCopy(Path, &Err);
            if (Got == Cid)            ++Seeded;
            else if (Mismatched) { ++*Mismatched;
                LogWarn("PackageCatalog::SeedDirectory", "CID mismatch (file changed since publish?) for " + Path
                        + (Got.empty() ? (" — add failed: " + Err) : (" — got " + Got + ", expected " + Cid)));
            }
        }
        ++Done;
        if (Progress) Progress(Done, Total, fs::path(Path).filename().string());
    }
    LogSucc("PackageCatalog::SeedDirectory",
            "Seeded " + std::to_string(Seeded) + "/" + std::to_string(Total) + " referenced file(s) from " + Dir
            + " (" + std::to_string(Skipped) + " already-seeded skipped, mode=" + (Overwrite ? "overwrite" : "additive") + ")");
    return Seeded;
}

int HealSourceContent(const nlohmann::ordered_json &GlobalConfigJSON)
{
    const std::vector<std::string> Dirs = PackageSourceDirs(GlobalConfigJSON);
    std::set<std::string> Referenced;
    for (const std::string &Dir : Dirs)
    {
        SeedDirectory(Dir);   // additive: re-points orphaned refs to their present path, skips intact CIDs (no re-hash)
        for (const auto &[Path, Cid] : SeedTargets(Dir)) Referenced.insert(Cid);
    }

    // Config-level meta-CIDs (package sources + published package CIDs) are pinned but never appear inside node
    // JSONs — count them as referenced so the prune below can never touch them.
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("PackageSources") && S["PackageSources"].is_array())
            for (const auto &Src : S["PackageSources"])
            {
                const std::string Cid = Src.is_object() ? Src.value("CID", std::string())
                                      : (Src.is_string() ? Src.get<std::string>() : std::string());
                if (!Cid.empty()) Referenced.insert(Cid);
            }
        if (S.contains("PackageCids") && S["PackageCids"].is_object())
            for (const auto &[Key, Val] : S["PackageCids"].items())
                if (Val.is_string() && !Val.get<std::string>().empty()) Referenced.insert(Val.get<std::string>());
    }

    // Prune: a pin that is UNREFERENCED by any source AND UNSERVABLE (a backing file is gone) is the leftover of a
    // superseded publish — a deleted .meta staging mirror, a replaced delta, a re-published package. The re-point pass
    // above can't fix it (nothing records a current path for it), so it would sit as a red "missing files" row and a
    // broken serving promise forever; drop its reference closure + pin. Healthy unreferenced pins (e.g. a manually
    // seeded master folder) are left alone, as is anything still referenced (surfaced, not destroyed — re-seeding the
    // source is the fix there). Skipped entirely when no source yielded targets (a source disk that isn't mounted
    // would make EVERYTHING look unreferenced).
    if (Referenced.empty()) return 0;
    int Pruned = 0;
    for (const IpfsWrapper::PinEntry &P : IpfsWrapper::Pins())
        if (!Referenced.count(P.Cid) && IpfsWrapper::CidMissing(P.Cid) && IpfsWrapper::DropRef(P.Cid))
            ++Pruned;
    if (Pruned)
        LogSucc("PackageCatalog::HealSourceContent",
                "pruned " + std::to_string(Pruned) + " stale pin(s) (unreferenced + backing file gone)");
    return Pruned;
}

int MirrorDehydrated(const std::string &SrcDir, const std::string &DestDir)
{
    std::error_code Ec;
    std::filesystem::create_directories(DestDir, Ec);
    const std::filesystem::path Src(SrcDir), Dest(DestDir);
    int Copied = 0;
    // Recursively copy ONLY *.json, preserving the relative tree — this is what makes a Meta-CID text-only: cover PNGs,
    // content zips and runtime dirs (e.g. DEFPREFIX/) are left behind; covers travel as content CIDs in the JSON.
    for (const auto &Entry : std::filesystem::recursive_directory_iterator(Src, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;   // manifests only
        std::error_code Ce;
        const std::filesystem::path Rel = std::filesystem::relative(Entry.path(), Src, Ce);
        if (Ce || Rel.empty()) continue;
        // Skip runtime/artifact subtrees. Only TOP-LEVEL package node fragments are manifests; DEFPREFIX (the wine
        // prefix) and USERDATA (persisted saves + REGISTRY/REGKEYS hive stores) hold per-machine build/runtime state —
        // some of which happens to be .json (e.g. a generated prefix's winevulkan.json) and would otherwise bloat the
        // Meta-CID and make it non-reproducible across machines.
        bool Runtime = false;
        for (const auto &Part : Rel.parent_path())
        {
            const std::string P = Part.string();
            if (P == "DEFPREFIX" || P == "USERDATA") { Runtime = true; break; }
        }
        if (Runtime) continue;
        const std::filesystem::path Out = Dest / Rel;
        std::filesystem::create_directories(Out.parent_path(), Ce);
        std::filesystem::copy_file(Entry.path(), Out, std::filesystem::copy_options::overwrite_existing, Ce);
        if (Ce) LogWarn("PackageCatalog::MirrorDehydrated", "skip " + Entry.path().string() + " (" + Ce.message() + ")");
        else ++Copied;
    }
    return Copied;
}

// Mint a JSON-only Meta-CID for SrcDir (a single bundle OR a collection of bundle subdirs): (1) ensure every package's
// VFS content + covers are content-addressed (idempotent PublishPackage — writes SOURCE.CID into the node JSON);
// (2) AddNoCopyMeta(SrcDir) → the folder CID, seeded IN PLACE from the *.json manifests (no staging mirror; content +
// DEFPREFIX/USERDATA are excluded by the filestore builder itself). The CID is identical to a JSON-only mirror's.
// Returns "" on failure.
std::string PublishMetaCid(const std::string &SrcDir, std::string *Error)
{
    auto Fail = [&](const std::string &M) -> std::string { if (Error) *Error = M; LogErr("PackageCatalog::PublishMetaCid", M); return {}; };
    std::error_code Ec;
    if (!std::filesystem::is_directory(SrcDir, Ec)) return Fail("not a directory: " + SrcDir);

    // 1. Content-address content + covers (idempotent: already-CID'd layers are skipped).
    auto EnsureSeeded = [&](const std::string &PkgDir) -> bool {
        std::string E;
        if (!PublishPackage(PkgDir, "", &E)) { Fail("seed " + PkgDir + ": " + E); return false; }
        return true;
    };
    if (ScanBundleIdentity(SrcDir).Valid) { if (!EnsureSeeded(SrcDir)) return {}; }               // single package
    else                                                                                          // collection of packages
        for (const auto &Sub : std::filesystem::directory_iterator(SrcDir, Ec))
        {
            if (!Sub.is_directory() || !ScanBundleIdentity(Sub.path().string()).Valid) continue;
            if (!EnsureSeeded(Sub.path().string())) return {};
        }

    // 2. Add-by-reference, text-only, IN PLACE → the Meta folder CID (seeds straight from the package tree's *.json).
    std::string E;
    const std::string Cid = IpfsWrapper::AddNoCopyMeta(SrcDir, &E);
    if (Cid.empty()) return Fail("AddNoCopyMeta(" + SrcDir + "): " + E);
    LogSucc("PackageCatalog::PublishMetaCid", "Meta-CID " + Cid + " (text-only, in place) <- " + SrcDir);
    return Cid;
}

} // namespace PackageCatalog
