#include "packagecatalog.h"
#include "pkglayout.h"
#include "pkggraph.h"
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

//A node file holds ONE node object or an ARRAY of them (grouping nodes into files is pure presentation), and a
//node IS its layer — the payload is hoisted onto the node, so there is no LAYERS array to walk. These two
//helpers are the whole difference for the seed/publish readers, which parse node JSON directly rather than
//through ParseNode (they must MUTATE it in place to record the minted SOURCE.CID).
static std::vector<nlohmann::ordered_json *> NodeDocsOf(nlohmann::ordered_json &J)
{
    std::vector<nlohmann::ordered_json *> Out;
    if (J.is_array()) { for (auto &N : J) if (N.is_object() && N.contains("NODE_ID")) Out.push_back(&N); }
    else if (J.is_object() && J.contains("NODE_ID")) Out.push_back(&J);
    return Out;
}
//Content nodes are the ones carrying seedable bytes (PATH + SOURCE); FORM says how they are interpreted.
static bool IsContentNode(const nlohmann::ordered_json &N)
{
    return N.is_object() && N.value("TYPE", std::string()) == "Content";
}


// ----- import / publish -----



//---------------------------------------------------------------------------------------------------------
//StampNodePositions — write the canvas layout into the nodes, so a published package opens laid out on a
//machine that has never seen it. Called on the way to a CID, because that is the only moment a layout is
//allowed to change the package's bytes: authoring drags live in GlobalConfig precisely so they do NOT.
//
//What gets stamped is the layout AS IT STANDS: `LocalOverride` (this machine's drags) wins over the node's
//current POS, which wins over the computed default — the same precedence PkgGraph::Build applies on screen, so
//publishing bakes exactly the picture the author is looking at. The algorithm only ever supplies a position for
//a node nobody has positioned.
//
//Idempotent where nothing moved: an unchanged bundle with no new drags produces identical bytes and mints the
//same CID, because a node whose POS already equals its resolved position is not rewritten at all.
//---------------------------------------------------------------------------------------------------------
std::string EditorLayoutKey(const std::filesystem::path &BundleDir)
{
    //ABSOLUTE, then normalised, then stripped of a trailing separator. Every step earns its place:
    //  - absolute, because the editor always writes an absolute key while the remint path takes its root from
    //    argv verbatim — `cd ~/.VidyaGod && VidyaGod --remint-library LIBRARY` produced a relative key that
    //    matched nothing, and a missed lookup is indistinguishable from "never arranged";
    //  - lexically_normal, to fold "…/./X" and "…/a/../X";
    //  - the trailing-separator strip, because lexically_normal does NOT remove one ("/a/b/" stays "/a/b/").
    std::error_code Ec;
    std::filesystem::path P = BundleDir.is_absolute() ? BundleDir : std::filesystem::absolute(BundleDir, Ec);
    if (Ec) P = BundleDir;                       // unreadable cwd: a stable key still beats no key
    std::string S = P.lexically_normal().string();
    while (S.size() > 1 && (S.back() == '/' || S.back() == '\\')) S.pop_back();
    return S;
}

const nlohmann::ordered_json *EditorLayoutFor(const nlohmann::ordered_json &GlobalConfigJSON,
                                              const std::filesystem::path &BundleDir)
{
    const auto SecIt = GlobalConfigJSON.find("EDITORLAYOUT");
    if (SecIt == GlobalConfigJSON.end() || !SecIt->is_object()) return nullptr;
    const auto BIt = SecIt->find(EditorLayoutKey(BundleDir));
    return (BIt != SecIt->end() && BIt->is_object()) ? &*BIt : nullptr;
}

bool StampNodePositions(const std::string &PackageDir, const nlohmann::ordered_json *LocalOverride,
                        std::string *Error)
{
    namespace fs = std::filesystem;
    std::error_code Ec;
    if (!fs::is_directory(PackageDir, Ec)) { if (Error) *Error = "not a directory: " + PackageDir; return false; }

    //One file holds one node or an array of them. Both shapes are collected into a single ordered document,
    //sorted by relative path so the seed order the layout starts from is a property of the bundle, not of the
    //order the filesystem happened to hand back.
    struct Slot { fs::path File; bool Array; size_t Index; };
    //TOP LEVEL ONLY, matching PackageEditorModel::LoadNodes (QDir::entryList, non-recursive). Walking
    //subdirectories would lay out a graph the author has never seen: a node-shaped .json below the bundle root
    //would join the layout, shift every other node's computed coordinates and receive a POS of its own, which
    //contradicts the one thing this function promises — that publishing bakes the picture on screen.
    std::vector<fs::path> Files;
    for (auto It = fs::directory_iterator(PackageDir, Ec); !Ec && It != fs::directory_iterator(); ++It)
        if (It->is_regular_file(Ec) && It->path().extension() == ".json") Files.push_back(It->path());
    std::sort(Files.begin(), Files.end());

    std::map<fs::path, nlohmann::ordered_json> Loaded;
    std::vector<Slot> Slots;
    nlohmann::ordered_json Nodes = nlohmann::ordered_json::array();
    for (const fs::path &F : Files)
    {
        std::ifstream In(F);
        nlohmann::ordered_json J;
        //A file we cannot parse is left alone — but SAID, because dropping it silently removes it from the
        //layout graph and moves every other node, which looks like the algorithm changed.
        try { In >> J; }
        catch (const std::exception &E)
        {
            LogWarn("PackageCatalog::StampNodePositions",
                    "skipping unparseable " + F.filename().string() + " (" + E.what() + ") — its nodes are not "
                    "laid out, and every other node's position is computed without them.");
            continue;
        }
        if (J.is_object() && J.contains("NODE_ID"))
        { Slots.push_back({F, false, 0}); Nodes.push_back(J); Loaded[F] = std::move(J); }
        else if (J.is_array())
        {
            for (size_t I = 0; I < J.size(); ++I)
                if (J[I].is_object() && J[I].contains("NODE_ID"))
                { Slots.push_back({F, true, I}); Nodes.push_back(J[I]); }
            Loaded[F] = std::move(J);
        }
    }
    if (Nodes.empty()) return true;   //nothing to lay out is not a failure

    const PkgGraph::Graph G = PkgGraph::Build(Nodes, LocalOverride);
    if (G.Nodes.size() != Slots.size()) { if (Error) *Error = "node/slot mismatch while stamping positions"; return false; }

    //A declared position Build refused. Publish is the one place where that matters MOST: the computed
    //position is about to be written over the author's declaration, the file's bytes change and the package's
    //Meta-CID with them — and the only line this function prints otherwise is "stamped POS into N file(s)",
    //which says nothing about why one of them moved. Build records rather than logs (it runs per keystroke in
    //the editor); this is the other caller, and it has to say so.
    //Reported AFTER the stamp loop, and worded from what the loop actually WROTE — not from the source label.
    //Deciding it by substring was wrong in both directions in turn: first "stamping over it" for a rejected
    //local override that changes nothing, then "ignored" for a node with NO own POS and a bad override, where
    //the layout supplies a position, the file gains one it never had, and its Meta-CID changes under a line
    //saying the node keeps what the package declares. The only fact that settles it is whether this node's
    //file was marked dirty, which is known one loop down.
    std::set<fs::path> Dirty;
    //Keyed by INDEX, not by id: a bundle can hold two nodes with the same NODE_ID (or none at all), and every
    //such node shared one key here — so the line below could say "written over it" about a node whose file was
    //untouched, purely because a namesake's was.
    std::set<size_t> Stamped;   // node SLOTS whose file this loop actually rewrote
    for (size_t I = 0; I < Slots.size(); ++I)
    {
        nlohmann::ordered_json Pos = nlohmann::ordered_json::array({ G.Nodes[I].X, G.Nodes[I].Y });
        nlohmann::ordered_json &Target = Slots[I].Array ? Loaded[Slots[I].File][Slots[I].Index]
                                                        : Loaded[Slots[I].File];
        if (Target.contains("POS") && Target["POS"] == Pos) continue;   //already correct: do not touch the bytes
        Target["POS"] = std::move(Pos);
        Dirty.insert(Slots[I].File);
        Stamped.insert(I);
    }

    for (const PkgGraph::RejectedPosition &R : G.RejectedPositions)
        Log(LogLevel::WARN, "PackageCatalog::StampNodePositions",
            "node '" + PkgGraph::SafeId(R.NodeId) + "': " + R.Source + " gives " + R.Value
                + ", which no layout could have produced - "
                + (R.Index >= 0 && Stamped.count((size_t)R.Index)
                       ? "a computed position was written over it, changing this package's bytes"
                       : "that declaration is ignored; nothing was rewritten"));
    //Write to a sibling temp and rename. This rewrites EVERY node file of EVERY package in the library
    //(RemintLibrary calls it per package), and a node .json is the author's only copy: truncating in place
    //means a crash, a kill or ENOSPC part way through leaves a half-written file where their package was.
    //rename() within the same directory is atomic, so a node file is either the old one or the new one.
    for (const fs::path &F : Dirty)
    {
        const fs::path Tmp = F.string() + ".vgtmp";
        {
            std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
            if (!Out) { if (Error) *Error = "could not write " + Tmp.string(); return false; }
            //dump(4), NO trailing newline — byte-identical to how SaveNodes and the CID stamper write a node
            //file. A different format here reflows the file on stamp and back again on the next editor save:
            //a byte change with zero semantic change, a new Meta-CID, and every peer re-downloading a package
            //that did not change.
            Out << Loaded[F].dump(4);
            Out.flush();
            //A stream that filled the disk fails HERE, not at open — checked, or the rename below publishes a
            //truncated file over a good one.
            if (!Out.good())
            { std::error_code Rm; fs::remove(Tmp, Rm);
              if (Error) *Error = "write failed (disk full?) for " + F.string(); return false; }
        }
        std::error_code Rn;
        fs::rename(Tmp, F, Rn);
        if (Rn)
        { std::error_code Rm; fs::remove(Tmp, Rm);
          if (Error) *Error = "could not replace " + F.string() + ": " + Rn.message(); return false; }
    }
    if (!Dirty.empty())
        LogOut("PackageCatalog::StampNodePositions",
               "stamped POS into " + std::to_string(Dirty.size()) + " file(s) of " + PackageDir);
    return true;
}

// Payload byte size of a layer's local content: a plain file's size, or the recursive sum of a directory's
// regular files. This matches the UnixFS logical size the fetch progress path counts (fetch.go rdr.Size()), so a
// stamped SOURCE.SIZE is directly usable as the download total for progress %/ETA and pre-fetch estimation.
// Instant (local stat/walk, content is on disk at mint); 0 on any error (treated as "unknown" downstream).
static uint64_t LocalPayloadSize(const std::filesystem::path &P)
{
    std::error_code Ec;
    if (std::filesystem::is_directory(P, Ec))
    {
        uint64_t Sum = 0;
        for (auto It = std::filesystem::recursive_directory_iterator(P, Ec);
             !Ec && It != std::filesystem::recursive_directory_iterator(); It.increment(Ec))
        {
            std::error_code Fc;
            if (It->is_regular_file(Fc)) { const auto S = std::filesystem::file_size(It->path(), Fc); if (!Fc) Sum += S; }
        }
        return Sum;
    }
    const auto S = std::filesystem::file_size(P, Ec);
    return Ec ? 0 : static_cast<uint64_t>(S);
}

bool PublishPackage(const std::string &PackageDir, const std::string &DehydratedDestDir, std::string *Error)
{

    auto Fail = [&](const std::string &M) -> bool { if (Error) *Error = M; LogErr("PackageCatalog::PublishPackage", M); return false; };

    std::error_code Ec;
    const std::filesystem::path Pkg(PackageDir);
    if (!std::filesystem::is_directory(Pkg, Ec)) return Fail("not a package directory: " + PackageDir);

    if (!IpfsWrapper::DaemonRunning())
        LogWarn("PackageCatalog::PublishPackage",
                "IPFS node not online yet — CIDs will be computed but content seeds to peers only once it connects.");

    int Seeded = 0, Walked = 0, Covers = 0, Repaired = 0, SizesStamped = 0;
    //Publishing is the one operation whose mistakes travel: a fragment skipped here is missing from the CID every
    //peer then fetches, and it is missing in a way nothing downstream can distinguish from "the author never wrote
    //that node". So both quiet skips below are counted and reported.
    int Unparseable = 0, BadCovers = 0;
    std::vector<std::string> Unfetchable;

    //A recorded CID is taken on FAITH by the mint (skipped as idempotent), the deliverability check (stat-only), and
    //every peer that fetches it — nothing re-hashes the bytes until bitswap serves them. So a backing file rebuilt in
    //place (same or larger size — a smaller one os.Stat catches) rides through every re-mint as a stale, un-servable
    //reference: "published clean", green in the UI, then a peer's download hangs on "data in file did not match".
    //NeedsSeed closes that: for a layer that already carries a CID, VgVerifyCid reads its whole DAG back through the
    //filestore (the one path that actually compares bytes to hash) and, on any mismatch, re-seeds from the real bytes
    //so the published CID is ALWAYS servable. Cost: reading the content at mint — deliberate, mint is rare.
    auto NeedsSeed = [&](const std::string &Cid, const std::filesystem::path &Local) -> bool {
        if (Cid.empty()) return true;                                   // never seeded → seed it
        std::error_code Rc;
        if (!std::filesystem::exists(Local, Rc)) return false;          // CID-only ref, no local bytes to verify — leave as-is
        const std::string Verr = IpfsWrapper::VerifyCid(Cid);
        if (Verr.empty()) return false;                                 // verified: the CID still serves its bytes → keep
        LogWarn("PackageCatalog::PublishPackage", "content drift: '" + Local.filename().string() + "' recorded CID "
                + Cid + " no longer serves its bytes (" + Verr + ") — re-seeding from the file");
        ++Repaired;
        return true;                                                    // stale → fall through and re-seed
    };

    //Walk every *.json fragment directly (no assemble/decompose round-trip — preserves each subcomponent's exact
    //file placement). Content-address VFS layers AND cover assets in place; re-save only mutated fragments.
    for (const auto &Entry : std::filesystem::directory_iterator(Pkg, Ec))
    {
        if (!Entry.is_regular_file() || Entry.path().extension() != ".json") continue;
        QFile FragFile(QString::fromStdString(Entry.path().string()));
        nlohmann::ordered_json Frag;
        if (JSONOps::LoadJSON(&FragFile, &Frag))                                 // LoadJSON returns true on FAILURE
        {
            //A fragment that will not parse was skipped in silence, so a package with one corrupt node published
            //cleanly, minus that node — and the resulting CID looked healthy to everyone who fetched it.
            LogErr("PackageCatalog::PublishPackage", "SKIPPING unparseable fragment " + Entry.path().filename().string()
                       + " — its nodes and layers will be ABSENT from the published package.");
            ++Unparseable;
            continue;
        }

        bool Mutated = false;

        //Cover art: content-address a DeclareLibraryItem's COVER like a layer — keep the filename in PATH, add
        //SOURCE:{ipfs,CID}. Idempotent once a CID is present. OBJECT FORM ONLY: the bare-string COVER was
        //reachable only through the deleted GAMES pass, so the branch that handled it went with it rather than
        //staying as an unreachable kindness — the caller below now REFUSES a non-object instead.
        auto SeedCover = [&](nlohmann::ordered_json &Holder)
        {
            if (!Holder.contains("COVER")) return;
            nlohmann::ordered_json &Cover = Holder["COVER"];
            if (!Cover.is_object()) return;                                       // refused and reported at the call site
            //CONST-SAFE: a bare Cover["SOURCE"] would INSERT a null "SOURCE" member on a cover that has none, and the
            //SIZE backfill below now saves fragments that used to be left untouched — writing that null out, which the
            //node validator then rejects (whole tile vanishes). Read through contains(), never operator[].
            const std::string CoverCid = (Cover.contains("SOURCE") && Cover["SOURCE"].is_object())
                                       ? Cover["SOURCE"].value("CID", std::string()) : std::string();
            const std::string File = Cover.value("PATH", std::string());
            if (!File.empty() && !NeedsSeed(CoverCid, Pkg / File))                // has a CID that still verifies its bytes — keep
            {
                //Idempotent CID — but backfill SOURCE.SIZE if absent (no re-seed of bytes needed), so an existing
                //library gains sizes on the next --remint-library without re-adding every layer.
                const std::filesystem::path CLocal = Pkg / File;
                std::error_code Sc;
                if (Cover.contains("SOURCE") && Cover["SOURCE"].is_object() && !Cover["SOURCE"].contains("SIZE")
                    && std::filesystem::exists(CLocal, Sc))
                { Cover["SOURCE"]["SIZE"] = LocalPayloadSize(CLocal); Mutated = true; ++SizesStamped; }
                return;
            }
            if (File.empty()) return;
            std::error_code Rc;
            const std::filesystem::path Local = Pkg / File;
            if (!std::filesystem::exists(Local, Rc)) return;                      // not a local file (CID-only ref)
            std::string Err;
            const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
            if (NewCid.empty()) { LogWarn("PackageCatalog::PublishPackage", "could not seed cover " + Local.string() + " (" + Err + ")"); return; }
            Cover = nlohmann::ordered_json{ {"PATH", File}, {"SOURCE", {{"TYPE", "ipfs"}, {"CID", NewCid}, {"SIZE", LocalPayloadSize(Local)}}} };
            Mutated = true;
            ++Covers;
        };
        //Node files (everything-is-a-node): seed each Content node's bytes + the cover on a DeclareLibraryItem node.
        for (nlohmann::ordered_json *Np : NodeDocsOf(Frag))
        {
            nlohmann::ordered_json &S = *Np;
            //Cover art lives on the DeclareLibraryItem node's COVER field ({PATH, SOURCE:{ipfs,CID}}, like content).
            //A COVER of any OTHER shape is content that will never be addressed: this was `is_object()` and
            //nothing else, so a bare-string COVER — the pre-node form — was stepped over in total silence.
            //One shipped that way (Tonic Trouble's library tile): the PNG sat in the bundle, was never seeded,
            //and ManifestTargets and PackageCoverCids both require the object form, so no peer could ever
            //receive the tile art and nothing anywhere said so. Counted as a gap, because that is what it is.
            if (S.contains("COVER") && !S["COVER"].is_null())
            {
                if (S["COVER"].is_object()) SeedCover(S);
                else ++BadCovers;
            }
            if (!IsContentNode(S)) continue;
            ++Walked;
            std::filesystem::path Local; std::string Cid;
            LayerLocator(S, Pkg, Local, Cid);
            std::error_code Rc;
            if (!NeedsSeed(Cid, Local))                                          // has a CID that still verifies — idempotent
            {
                //Backfill SOURCE.SIZE if absent (no re-seed): existing packages gain the download-size hint on the
                //next --remint-library without re-adding bytes. Skips CID-only refs (no local file to measure).
                std::error_code Sc;
                if (S["SOURCE"].is_object() && !S["SOURCE"].contains("SIZE") && std::filesystem::exists(Local, Sc))
                { S["SOURCE"]["SIZE"] = LocalPayloadSize(Local); Mutated = true; ++SizesStamped; }
                continue;
            }
            if (!std::filesystem::exists(Local, Rc))                             // no local content to seed
            {
                //A RUNTIME-SOURCED layer is absent because it is SUPPOSED to be: its PATH carries a %VAR%
                //that resolves to a live mount at launch (a proton prefix-assembly layer, "%DefaultPfxDir%"),
                //so there is no package file to ship and there never will be. It is not a gap, and saying it
                //is put 30 UNFETCHABLE lines and a "PUBLISHED WITH GAPS ... the content will not be there"
                //over the runners collection on every mint — the un-ignorable summary, crying wolf.
                //launchsources.cpp records fixing the identical false alarm on the launch side with this same
                //predicate, having printed three phantom errors per wine launch before it.
                //
                //The predicate gates ONLY THIS REPORT, deliberately. Placing it earlier also skipped the
                //VerifyCid drift repair above and the seeding below, which made "log-only" a property of the
                //library's current contents (no token-bearing layer happens to have bytes) rather than of the
                //code. Here it cannot suppress a seed: anything with content or a CID has already been
                //handled before this line is reached.
                //LIMITATION, named because it is invisible otherwise: a CustomVar-templated PATH whose tokens
                //all resolve to a real file (ResolvePathState treats that as real content) is absent HERE and
                //is now silently not reported. No such node exists in the library; if one is ever authored,
                //this report has to substitute defaults before deciding.
                if (!ManifestModel::IsRuntimeSourcedLayer(S)) Unfetchable.push_back(Local.string());
                continue;
            }
            std::string Err;
            const std::string NewCid = IpfsWrapper::AddNoCopy(Local.string(), &Err);
            if (NewCid.empty()) return Fail("could not seed layer " + Local.string() + " (" + Err + ")");
            nlohmann::ordered_json Src = (S.contains("SOURCE") && S["SOURCE"].is_object()) ? S["SOURCE"] : nlohmann::ordered_json::object();
            Src["TYPE"] = "ipfs"; Src["CID"] = NewCid; Src["SIZE"] = LocalPayloadSize(Local); S["SOURCE"] = std::move(Src);
            Mutated = true; ++Seeded;
        }

        if (Mutated && !JSONOps::SaveJSON(&Frag, &FragFile))
            return Fail("could not write annotated manifest fragment: " + Entry.path().string());
    }
    LogSucc("PackageCatalog::PublishPackage", "Dehydrated " + PackageDir + " (" + std::to_string(Seeded)
            + " of " + std::to_string(Walked) + " layer(s) + " + std::to_string(Covers) + " cover(s) newly seeded"
            + (Repaired ? ", " + std::to_string(Repaired) + " re-seeded after DRIFT" : "")
            + (SizesStamped ? ", " + std::to_string(SizesStamped) + " SIZE backfilled" : "") + ")");

    //A layer with neither a CID nor local content is published as a reference to bytes that exist NOWHERE: the
    //package resolves, the download reports nothing to fetch, and the game is missing files on the first machine
    //that is not this one. Name every one — this is the last moment before it becomes someone else's problem.
    for (const std::string &P : Unfetchable)
        LogErr("PackageCatalog::PublishPackage", "layer '" + P + "' has no CID and no local file — it will be "
                                                 "published as an UNFETCHABLE reference.");
    if (BadCovers)
        LogErr("PackageCatalog::PublishPackage", std::to_string(BadCovers) + " COVER field(s) in " + PackageDir
                   + " are not objects ({PATH, SOURCE}) — cover art in any other shape is never content-addressed, "
                     "so the tile ships with no image on every machine but this one.");
    if (Unparseable || !Unfetchable.empty() || BadCovers)
        LogErr("PackageCatalog::PublishPackage", "PUBLISHED WITH GAPS: " + std::to_string(Unparseable)
                   + " unparseable fragment(s), " + std::to_string(Unfetchable.size()) + " unfetchable layer(s), "
                   + std::to_string(BadCovers) + " unaddressable cover(s) in "
                   + PackageDir + ". The CID will look healthy and the content will not be there.");

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
    return ManifestTargets(Dir, CoversOnly, true);
}

std::map<std::string, std::string> ManifestTargets(const std::string &Dir, bool CoversOnly, bool ExistingOnly)
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
        if (!J.is_object() && !J.is_array()) continue;      // one node, or an array of them
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
            // ExistingOnly is what SEEDING wants (you cannot reference bytes you do not have). An UPGRADE diff wants
            // the recorded targets regardless: the staged new tree is manifests-only, so every content file is absent.
            // generic_string() (forward slashes) NOT string() (native): on Windows string() yields backslash keys,
            // so the same package produced a different seed map than on Linux — breaking cross-platform path lookups
            // (and de-dup) while the tests looked keys up with '/'. Windows file APIs accept '/', so this stays valid.
            if (!ExistingOnly || fs::exists(Local, Ec)) ToSeed[Local.generic_string()] = Cid;
        };

        // A node IS its layer, so the node object itself carries PATH + SOURCE when it is Content. Cover art is
        // ALWAYS seeded and lives on the DeclareLibraryItem node's COVER field — {PATH, SOURCE:{ipfs,CID}}, the
        // same shape as content.
        for (nlohmann::ordered_json *Np : NodeDocsOf(J))
        {
            if (!CoversOnly && IsContentNode(*Np)) Consider(*Np);                 // content (skipped in covers-only)
            if (Np->contains("COVER") && (*Np)["COVER"].is_object()) Consider((*Np)["COVER"]);
        }
    }
    return ToSeed;
}

int SeedDirectory(const std::string &Dir,
                  const std::function<void(int, int, const std::string &)> &Progress,
                  int *Mismatched, bool CoversOnly, bool Overwrite, bool Verify,
                  std::vector<SeedFailure> *Failures)
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

        // SELF-CHECK BEFORE SKIPPING. HasLocal + CidMissing only answer "do we hold it?" and "is the backing file
        // GONE?" — neither reads a byte, so a file whose CONTENTS changed answered has=true, orphan=false and was
        // skipped AND counted as seeded. That is exactly how "Seeded 5/5" was reported over unservable content,
        // and how 933 bad references accumulated unnoticed until a full sweep went looking.
        //
        // The free half of the check: compare the file on disk against the payload size the CID represents (root
        // block only). A rebuilt zip or re-authored delta almost never lands on the identical byte count. It is a
        // filter, not a proof — a same-size edit still needs Verify's byte read — but it costs one stat.
        bool Stale = false;
        if (Has && !Orphan)
        {
            std::error_code Sc;
            const auto OnDisk   = (long long)fs::file_size(Path, Sc);
            const long long Was = IpfsWrapper::CidFileSizeLocal(Cid);
            // ONLY a file that SHRANK is broken. A file that GREW still has every published byte where the
            // references expect it, so the recorded CID stays fully deliverable (verified: appending 4 KiB leaves
            // it servable). Treating "grew" as stale would drop a WORKING reference and re-add under a different
            // CID — actively breaking delivery of the CID peers are asking for, in the name of repairing it.
            if (!Sc && Was >= 0 && OnDisk < Was)
            {
                Stale = true;
                LogWarn("PackageCatalog::SeedDirectory", "backing file truncated (" + std::to_string(Was)
                        + " → " + std::to_string(OnDisk) + " bytes): " + Path);
            }
            else if (!Sc && Was >= 0 && OnDisk > Was)
                LogOut("PackageCatalog::SeedDirectory", "file has grown since publish (" + std::to_string(Was)
                       + " → " + std::to_string(OnDisk) + " bytes) — published bytes still intact: " + Path);
            else if (Verify && !IpfsWrapper::VerifyCid(Cid).empty())
            {
                Stale = true;   // bytes disagree at the same size — only a real read finds this
                LogWarn("PackageCatalog::SeedDirectory", "unservable reference (bytes changed): " + Path);
            }
        }

        if (Has && !Orphan && !Stale && !Overwrite) { ++Skipped; ++Seeded; }   // genuinely intact → leave it
        else
        {
            if (Orphan)                                              // observability: the self-heal case (backing moved/re-created)
                LogOut("PackageCatalog::SeedDirectory", "re-pointing orphaned reference " + Cid + " -> " + Path);
            if (Has) IpfsWrapper::DropRef(Cid);                       // clear the stale/old reference so the re-add isn't deduped
            std::string Err;
            const std::string Got = IpfsWrapper::AddNoCopy(Path, &Err);
            // A re-add that yields the SAME CID is a genuine self-heal: the reference was stale, the bytes are the
            // ones we published, and we can serve it again. A DIFFERENT CID is NOT a heal — the file no longer is
            // what we published, and silently accepting it would republish under a new CID while quietly orphaning
            // the one peers are asking for. That is an error for a human, never an automatic fix.
            if (Got == Cid)            ++Seeded;
            else if (Mismatched) { ++*Mismatched;
                if (Failures) Failures->push_back({ Path, Cid, Got });
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

HealReport HealSourceContent(const nlohmann::ordered_json &GlobalConfigJSON, const HealOptions &Options)
{
    HealReport R;
    const std::vector<std::string> Dirs = PackageSourceDirs(GlobalConfigJSON);
    std::set<std::string> Referenced;
    std::map<std::string, std::string> AllTargets;   // path → recorded CID, across every source
    for (const std::string &Dir : Dirs)
    {
        R.Repointed += SeedDirectory(Dir);   // additive: re-points orphaned refs, skips intact CIDs (no re-hash)
        for (const auto &[Path, Cid] : SeedTargets(Dir)) { Referenced.insert(Cid); AllTargets[Path] = Cid; }
    }

    // DEEP: the cheap pass above only stats backing paths, so it cannot see a reference whose file still exists but
    // whose BYTES no longer match — the failure that makes a requesting peer hang forever (we advertise a block we
    // cannot deliver). Read every referenced CID back through the same blockstore path bitswap serves from.
    if (Options.Deep)
    {
        // Scan the WHOLE filestore index, not just the CIDs the manifests record: a stale entry can survive under a
        // CID nothing references any more (a superseded delta, a re-published package), and we keep advertising it.
        // Starting from the recorded CIDs would never reach it, yet it is exactly what hangs a requesting peer.
        LogOut("PackageCatalog::HealSourceContent", "deep verify: reading every filestore reference back…");
        const std::vector<IpfsWrapper::UnservableRef> Bad = IpfsWrapper::UnservableRefs();
        R.Verified = (int)AllTargets.size();

        // Group by backing file: one file's many bad leaves need exactly one drop+re-add, not one per leaf.
        std::map<std::string, std::vector<std::string>> ByPath;   // path → offending CIDs
        for (const IpfsWrapper::UnservableRef &U : Bad)
        {
            LogWarn("PackageCatalog::HealSourceContent",
                    "unservable ref " + U.Cid + " (status " + std::to_string(U.Status) + ") backed by "
                    + U.Path + (U.Err.empty() ? "" : " — " + U.Err));
            ByPath[U.Path].push_back(U.Cid);
        }

        for (const auto &[Path, Cids] : ByPath)
        {
            std::error_code Ec;
            if (Path.empty() || !std::filesystem::exists(Path, Ec))
            {
                // Backing gone and nothing on disk to re-point at: dropping the closure is the whole repair — it
                // stops us advertising a block we can never deliver.
                for (const std::string &C : Cids) IpfsWrapper::DropRef(C);
                ++R.PrunedUnservable;
                continue;
            }
            // The file IS there but its bytes no longer match. A plain re-add dedup-skips (the CID is already
            // indexed and boxo will not re-reference it), so the closure MUST be dropped first — this is precisely
            // the step that makes "--seed --overwrite" appear to succeed while changing nothing.
            for (const std::string &C : Cids) IpfsWrapper::DropRef(C);
            const auto It = AllTargets.find(Path);
            std::string AddErr;
            const std::string Got = IpfsWrapper::AddNoCopy(Path, &AddErr);
            if (Got.empty())                { R.Unrepaired.push_back(Path + ": re-add failed: " + AddErr); continue; }
            if (It == AllTargets.end())     { ++R.StaleRepaired; continue; }   // not manifest-referenced: dropping was the fix
            if (Got != It->second)
            {
                // The file's real content hashes to something else, so the node JSON has been publishing a CID
                // nobody can ever fetch. Rewriting it here would silently change what this node publishes and
                // invalidate the collection CID, so surface it for a human instead.
                R.Drift.push_back(Path + ": recorded " + It->second + " but content is " + Got);
                continue;
            }
            const std::string Again = IpfsWrapper::VerifyCid(It->second);
            if (Again.empty()) ++R.StaleRepaired;
            else               R.Unrepaired.push_back(Path + " (" + It->second + "): still unservable: " + Again);
        }

        // Recorded CIDs the index has no entry for at all (never seeded under that CID) never surface above, since
        // there is nothing to verify — catch them here so a cold peer's "download hangs forever" becomes visible.
        for (const auto &[Path, Cid] : AllTargets)
            if (!IpfsWrapper::HasLocal(Cid))
                R.Drift.push_back(Path + ": recorded " + Cid + " is not held locally at all (never seeded)");
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
    // Guard on the SCANNED TARGETS, not on Referenced: main.cpp force-seeds the built-in library/runner source CIDs
    // into the config on every launch, so Referenced is never empty even when zero source directories exist on disk.
    // Using it here would let the prune run with no knowledge of what is referenced and delete live content — which
    // is exactly what happened the first time this was tested.
    if (AllTargets.empty()) { LogWarn("PackageCatalog::HealSourceContent",
                                      "no source directory yielded any referenced content — skipping prune"); return R; }
    for (const IpfsWrapper::PinEntry &P : IpfsWrapper::Pins())
    {
        if (Referenced.count(P.Cid)) continue;
        if (IpfsWrapper::CidMissing(P.Cid))
        {
            if (IpfsWrapper::DropRef(P.Cid)) ++R.PrunedUnservable;
        }
        else if (Options.PruneUnreferenced)
        {
            // Healthy, but nothing points at it: a superseded collection/package meta-CID (the IPFS tab's
            // "unknown" rows). Named in the log because this is the one prune that can throw away a hand-seeded
            // folder — which is why it is opt-in rather than part of the background pass.
            LogWarn("PackageCatalog::HealSourceContent", "dropping unreferenced pin " + P.Cid);
            if (IpfsWrapper::Unpin(P.Cid)) ++R.PrunedUnreferenced;
        }
    }
    if (R.PrunedUnservable)
        LogSucc("PackageCatalog::HealSourceContent",
                "pruned " + std::to_string(R.PrunedUnservable) + " stale pin(s) (unreferenced + backing file gone)");
    if (R.PrunedUnreferenced)
        LogSucc("PackageCatalog::HealSourceContent",
                "dropped " + std::to_string(R.PrunedUnreferenced) + " unreferenced pin(s)");
    return R;
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

bool RemintLibrary(const std::string &LibraryRoot, nlohmann::ordered_json &Config,
                   std::vector<RemintEntry> &Out, std::string *Error)
{
    namespace fs = std::filesystem;
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; LogErr("PackageCatalog::RemintLibrary", M); return false; };
    std::error_code Ec;
    if (!fs::is_directory(LibraryRoot, Ec)) return Fail("not a directory: " + LibraryRoot);

    auto &Settings = Config["Settings"];
    if (!Settings.is_object()) Settings = nlohmann::ordered_json::object();

    // Each immediate subdir of LibraryRoot that contains valid package bundles is a SOURCE COLLECTION.
    std::vector<fs::path> Sources;
    for (const auto &S : fs::directory_iterator(LibraryRoot, Ec))
        if (S.is_directory()) Sources.push_back(S.path());
    std::sort(Sources.begin(), Sources.end());

    for (const auto &SrcDir : Sources)
    {
        const std::string SrcName = SrcDir.filename().string();
        std::vector<fs::path> Pkgs;
        for (const auto &P : fs::directory_iterator(SrcDir, Ec))
            if (P.is_directory() && ScanBundleIdentity(P.path().string()).Valid) Pkgs.push_back(P.path());
        if (Pkgs.empty()) { LogOut("PackageCatalog::RemintLibrary", "skip " + SrcName + " (no package bundles)"); continue; }
        std::sort(Pkgs.begin(), Pkgs.end());

        LogOut("PackageCatalog::RemintLibrary", "source '" + SrcName + "': " + std::to_string(Pkgs.size()) + " package(s)");
        // Level 2 — each package's meta-CID (its EnsureSeeded also mints Level-1 content CIDs into the node JSONs).
        for (const auto &Pkg : Pkgs)
        {
            std::string E;
            //Publish the layout as it stands NOW: this machine's dragged positions (EDITORLAYOUT, keyed by
            //bundle directory) are the author's arrangement and become the package's defaults. The algorithm
            //only fills in nodes nobody has ever positioned.
            //Stamp only what this machine AUTHORS. A remint root can contain bundles fetched from someone
            //else's CID source, and writing POS into those changes bytes that a CID out there still claims to
            //serve — the "recorded CID no longer serves its bytes" condition, caused by us. A bundle with a
            //stored layout is one the author has opened and arranged; one without is left exactly as fetched.
            const nlohmann::ordered_json *Local = EditorLayoutFor(Config, Pkg);
            if (Local)
            {
                if (!StampNodePositions(Pkg.string(), Local, &E)) return Fail("package " + Pkg.string() + ": " + E);
            }
            else if (VerboseLogging())
                LogOut("PackageCatalog::RemintLibrary",
                       "no stored layout for " + Pkg.filename().string() + " — leaving its node files untouched.");
            const std::string PkgCid = PublishMetaCid(Pkg.string(), &E);
            if (PkgCid.empty()) return Fail("package " + Pkg.string() + ": " + E);
            Settings["PackageCids"][Pkg.filename().string()] = PkgCid;
            Out.push_back({ "package", SrcName + "/" + Pkg.filename().string(), PkgCid });
        }
        // Level 3 — the collection meta-CID; update the matching source entry so the catalog uses the fresh CID.
        std::string E;
        const std::string ColCid = PublishMetaCid(SrcDir.string(), &E);
        if (ColCid.empty()) return Fail("collection " + SrcDir.string() + ": " + E);
        if (Settings.contains("PackageSources") && Settings["PackageSources"].is_array())
            for (auto &Src : Settings["PackageSources"])
                if (Src.is_object() && Src.value("NAME", std::string()) == SrcName) Src["CID"] = ColCid;
        Out.push_back({ "collection", SrcName, ColCid });
    }
    return true;
}

} // namespace PackageCatalog
