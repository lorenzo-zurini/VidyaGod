// `VidyaGod ipfs …`, `queue …`, `transfers …`, `downloads …` — the embedded IPFS node (status, fetch, add, blocks,
// pins, the health of what it serves, identity, the LAN overlay), the ONE network queue every fetch takes a slot of,
// the IPFS tab's per-CID state, and the Catalog's downloads.
#include "command.h"
#include "cmdutil.h"
#include "downloadmanager.h"
#include "downloadqueue.h"
#include "ipfsmodel.h"
#include "ipfswrapper.h"
#include "mainwindow.h"

#include <QDir>
#include <QStringList>
#include <fstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;

void Check(bool Ok, const std::string &Err)
{
    if (!Ok) throw std::runtime_error(Err.empty() ? "failed" : Err);
}

std::string Must(const std::string &R, const std::string &Err)
{
    if (R.empty()) throw std::runtime_error(Err.empty() ? "failed" : Err);
    return R;
}

json Checks(const std::vector<IpfsWrapper::NetCheck> &V)
{
    json Out = json::array();
    for (const auto &C : V) Out.push_back({{"check", C.Name}, {"status", C.Status}, {"detail", C.Detail}});
    return Out;
}

std::vector<IpfsWrapper::FetchTarget> TargetsArg(const json &A)
{
    const json &T = A["targets"];
    if (!T.is_array()) throw Command::BadArgs("--targets is a JSON array of {cid, to, optional?, dir?, verify?}");
    std::vector<IpfsWrapper::FetchTarget> Out;
    for (const auto &X : T)
    {
        IpfsWrapper::FetchTarget F;
        F.Cid = X.value("cid", std::string());
        F.LocalPath = X.value("to", std::string());
        F.Optional = X.value("optional", false);
        F.Dir = X.value("dir", false);
        F.Verify = X.value("verify", false);
        if (F.Cid.empty() || F.LocalPath.empty()) throw Command::BadArgs("each target needs a cid and a to");
        Out.push_back(F);
    }
    return Out;
}

MainWindow &Win(Command::Context &C)
{
    auto *W = qobject_cast<MainWindow *>(C.Window);
    if (!W) throw std::runtime_error("no main window");
    return *W;
}

const char *PhaseName(IpfsModel::CidState::Phase P)
{
    switch (P)
    {
    case IpfsModel::CidState::Pending: return "pending";
    case IpfsModel::CidState::Queued: return "queued";
    case IpfsModel::CidState::Downloading: return "downloading";
    case IpfsModel::CidState::Pinning: return "pinning";
    case IpfsModel::CidState::Stalled: return "stalled";
    case IpfsModel::CidState::Errored: return "errored";
    case IpfsModel::CidState::Seeded: return "seeded";
    }
    return "?";
}

json Row(const QString &Cid, const IpfsModel::CidState &S)
{
    return {{"cid", Cid.toStdString()}, {"label", S.label.toStdString()}, {"package", S.package.toStdString()},
            {"category", S.category.toStdString()}, {"library", S.source.toStdString()}, {"phase", PhaseName(S.phase)}, {"pct", S.pct},
            {"size", S.size}, {"speed", S.speedBps}, {"providers", S.providers}, {"missing", S.missing}, {"uploading", S.uploading},
            {"announced", S.announced}, {"package_folder", S.packageLevel}, {"error", S.error.toStdString()}, {"activity", S.activity.toStdString()}};
}

QStringList Cids(const json &A, const char *Key)
{
    QStringList Out;
    for (const auto &S : List(A, Key)) Out << QString::fromStdString(S);
    return Out;
}

json Keys(const QStringList &L)
{
    json Out = json::array();
    for (const QString &S : L) Out.push_back(S.toStdString());
    return Out;
}

Command::Arg CidArg() { return Command::Pos("cid", "the CID"); }

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("ipfs", "the embedded IPFS node: status, fetch, add, blocks, pins, serving health, identity");
    Command::RegisterGroup("queue", "the ONE network queue: batches, jobs, where fetches land, cancel and prioritise");
    Command::RegisterGroup("transfers", "the IPFS tab: every CID the app tracks, its phase, progress, health");
    Command::RegisterGroup("downloads", "the Catalog's downloads: start, follow, cancel, resume");

    // ---------------- ipfs: the node ----------------
    Command::Register({
        "ipfs status", "the node: up, online, its identity, peers, bandwidth, store size",
        "Whether the node's store is open and the network up, its peer ID and listen addresses, connected peers, "
        "bandwidth now, the store's size and what it holds (filestore references, blocks), what peers are pulling "
        "(--uploads-window ms), and the download slot limit.",
        { Opt("uploads-window", "count a CID as uploading if served within this many ms (default 5000)", Type::Int) }, { "ipfs status" },
        { "IpfsWrapper::Available", "IpfsWrapper::DaemonRunning", "IpfsWrapper::PeerID", "IpfsWrapper::ListenAddrs", "IpfsWrapper::PeerCount",
          "IpfsWrapper::Bandwidth", "IpfsWrapper::RepoSizeHuman", "IpfsWrapper::ActiveUploads", "IpfsWrapper::MaxConcurrentDownloads",
          "IpfsWrapper::StoreCounts" },
        Needs::Config, true,
        [](Command::Context &, const json &A) {
            const auto B = IpfsWrapper::Bandwidth();
            return json{{"available", IpfsWrapper::Available()}, {"online", IpfsWrapper::DaemonRunning()}, {"peer", IpfsWrapper::PeerID()},
                        {"listen", IpfsWrapper::ListenAddrs()}, {"peers", IpfsWrapper::PeerCount()}, {"down_bps", B.DownBps}, {"up_bps", B.UpBps},
                        {"store", IpfsWrapper::RepoSizeHuman()}, {"uploading", IpfsWrapper::ActiveUploads(static_cast<int>(Command::Int(A, "uploads-window", 5000)))},
                        {"download_slots", IpfsWrapper::MaxConcurrentDownloads()}, {"store_counts", IpfsWrapper::StoreCounts()}};
        }, {} });

    Command::Register({
        "ipfs start", "start the node on the data dir's store (if it is not up)",
        "Open the store and join the network. A command that needs the node starts it by itself; this is for a node "
        "stopped with `ipfs stop`.", {}, { "ipfs start" }, { "IpfsWrapper::StartNode" }, Needs::Config, false,
        [](Command::Context &C, const json &) {
            std::string Err;
            Check(IpfsWrapper::StartNode(C.AppDataDir->filePath("IPFS").toStdString(), &Err), Err);
            return json{{"available", IpfsWrapper::Available()}};
        }, {} });

    Command::Register({
        "ipfs stop", "stop the node (no fetching, seeding or friends until it starts again)",
        "Close the node: every transfer stops, friends see you offline.", { Flag("yes", "yes, stop it") }, { "ipfs stop --yes" },
        { "IpfsWrapper::StopNode" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this takes the app offline: add --yes");
            IpfsWrapper::StopNode();
            return json{{"available", IpfsWrapper::Available()}};
        }, {} });

    Command::Register({
        "ipfs slots", "how many downloads run at once (the network queue's slots), or set it",
        "The limit on fetches running at once — and on everything else that opens connections, at lower priority.",
        { Opt("set", "the new limit (1–32)", Type::Int) }, { "ipfs slots", "ipfs slots --set 6" },
        { "IpfsWrapper::SetMaxConcurrentDownloads", "IpfsWrapper::MaxConcurrentDownloads" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (A.contains("set")) IpfsWrapper::SetMaxConcurrentDownloads(static_cast<int>(Command::Int(A, "set")));
            return json{{"slots", IpfsWrapper::MaxConcurrentDownloads()}};
        }, {} });

    Command::Register({
        "ipfs fetch", "fetch a CID to a path (a file or, with --dir, a folder)",
        "Through the ONE network queue, blocking: the file (or folder) lands at <dest>. --once is a single attempt "
        "outside the queue (what each queue job does), --verify checks bytes already at <dest> against the CID.",
        { CidArg(), Pos("dest", "where it lands"), Flag("dir", "a folder CID"), Flag("once", "one attempt, outside the queue"),
          Flag("verify", "(--once) re-check bytes already at dest"), Opt("timeout", "give up after this many ms (file, queued)", Type::Int) },
        { "ipfs fetch Qm… ~/Storage/tmp/x.zip", "ipfs fetch bafybei… ~/Storage/tmp/pkg --dir" },
        { "IpfsWrapper::FetchToPath", "IpfsWrapper::FetchDirToPath", "IpfsWrapper::FetchOnce", "IpfsWrapper::FetchOnceHookActive" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string Cid = Command::Str(A, "cid"), Dest = Command::Str(A, "dest");
            const bool Dir = Command::Bool(A, "dir", false);
            std::string Err;
            if (Command::Bool(A, "once", false))
            {
                const int Rc = IpfsWrapper::FetchOnce(Cid, Dest, Dir, &Err, Command::Bool(A, "verify", false));
                if (Rc != 0) throw std::runtime_error("fetch failed (" + std::to_string(Rc) + "): " + Err);
                return json{{"landed", Dest}, {"hooked", IpfsWrapper::FetchOnceHookActive()}};
            }
            const std::string P = Dir ? IpfsWrapper::FetchDirToPath(Cid, Dest, &Err)
                                      : IpfsWrapper::FetchToPath(Cid, Dest, &Err, static_cast<int>(Command::Int(A, "timeout", 0)));
            return json{{"landed", Must(P, Err)}};
        }, {} });

    Command::Register({
        "ipfs fetch-many", "fetch several targets together through the queue (blocking)",
        "A batch of {cid, to, optional?, dir?, verify?} targets, run concurrently in the queue's slots; fails when a "
        "required one fails.", { WithSample(Opt("targets", "the targets (JSON array)", Type::Json, true), "[{\"cid\":\"Qm…\",\"to\":\"/tmp/a\"}]") },
        { "ipfs fetch-many --targets '[{\"cid\":\"Qm…\",\"to\":\"/tmp/a\"}]'" }, { "IpfsWrapper::FetchTargetsConcurrent" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            Check(IpfsWrapper::FetchTargetsConcurrent(TargetsArg(A), &Err), Err);
            return json{{"fetched", A["targets"].size()}};
        }, {} });

    Command::Register({
        "ipfs add", "add a file to the node in place (no copy), or just compute its CID",
        "Serve a file from where it is (filestore: no copy into the store), returning its CID; --meta adds a node "
        "file the way package folders are (raw leaves); --only-hash computes the CID without adding anything.",
        { Pos("path", "the file"), Flag("meta", "a node / metadata file"), Flag("only-hash", "compute the CID only") },
        { "ipfs add ~/x/game.zip", "ipfs add ~/x/game.zip --only-hash" },
        { "IpfsWrapper::AddNoCopy", "IpfsWrapper::AddNoCopyMeta", "IpfsWrapper::ComputeCid" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string P = Command::Str(A, "path");
            std::string Err;
            if (Command::Bool(A, "only-hash", false)) return json{{"cid", Must(IpfsWrapper::ComputeCid(P, &Err), Err)}, {"added", false}};
            const std::string Cid = Command::Bool(A, "meta", false) ? IpfsWrapper::AddNoCopyMeta(P, &Err) : IpfsWrapper::AddNoCopy(P, &Err);
            return json{{"cid", Must(Cid, Err)}, {"added", true}};
        }, {} });

    Command::Register({
        "ipfs block", "read a raw block, or store one",
        "A block's bytes by CID (printed as text, or --out to a file), or with --put the bytes of a file stored as one "
        "block (its CID printed).", { Pos("cid-or-file", "the CID to read, or (with --put) the file to store"), Flag("put", "store the file"),
                                       Opt("out", "write the block to this file") },
        { "ipfs block bafkrei…", "ipfs block node.json --put" }, { "IpfsWrapper::BlockGet", "IpfsWrapper::BlockPut" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string X = Command::Str(A, "cid-or-file");
            std::string Err;
            if (Command::Bool(A, "put", false))
            {
                std::ifstream In(X, std::ios::binary);
                if (!In) throw Command::BadArgs("cannot read '" + X + "'");
                const std::string B((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
                return json{{"cid", Must(IpfsWrapper::BlockPut(B, &Err), Err)}, {"bytes", B.size()}};
            }
            const std::string B = IpfsWrapper::BlockGet(X, &Err);
            if (B.empty() && !Err.empty()) throw std::runtime_error(Err);
            if (A.contains("out")) { std::ofstream(Command::Str(A, "out"), std::ios::binary) << B; return json{{"bytes", B.size()}, {"out", Command::Str(A, "out")}}; }
            return json{{"bytes", B.size()}, {"data", B}};
        }, {} });

    Command::Register({
        "ipfs ls", "a folder CID's entries", "Name → CID for each entry of a UnixFS folder.", { CidArg() }, { "ipfs ls bafybei…" },
        { "IpfsWrapper::DirEntries" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::map<std::string, std::string> M;
            std::string Err;
            Check(IpfsWrapper::DirEntries(Command::Str(A, "cid"), M, &Err), Err);
            return json(M);
        }, {} });

    Command::Register({
        "ipfs mkdir", "build a folder CID from name → CID entries",
        "Make a UnixFS folder of existing CIDs (local: no network). Lists the entries not held whole here.",
        { WithSample(Opt("entries", "{name: cid} (JSON)", Type::Json, true), "{\"a.json\":\"bafkrei…\"}") }, { "ipfs mkdir --entries '{\"a.json\":\"bafkrei…\"}'" },
        { "IpfsWrapper::MakeDir" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::map<std::string, std::string> M;
            for (const auto &[K, V] : A["entries"].items()) M[K] = V.get<std::string>();
            std::string Err;
            std::vector<std::string> NotWhole;
            return json{{"cid", Must(IpfsWrapper::MakeDir(M, &Err, &NotWhole), Err)}, {"not_whole", NotWhole}};
        }, {} });

    Command::Register({
        "ipfs cid", "everything the node knows of one CID",
        "Held here (any of it, whole), missing its backing file, its size (local; --network asks peers), the backing "
        "file's size, whether it serves (and --verify: every block read back the way bitswap serves it), its DHT "
        "announcement and how many providers have it.",
        { CidArg(), Flag("network", "also ask the network for its size (up to ~35 s)"), Flag("verify", "read every block back"),
          Flag("providers", "count providers (a DHT query)") },
        { "ipfs cid Qm…", "ipfs cid Qm… --verify --providers" },
        { "IpfsWrapper::HasLocal", "IpfsWrapper::HeldWhole", "IpfsWrapper::CidMissing", "IpfsWrapper::CidSizeLocal", "IpfsWrapper::CidSize",
          "IpfsWrapper::CidFileSizeLocal", "IpfsWrapper::CidServeStatus", "IpfsWrapper::VerifyCid", "IpfsWrapper::SeedAnnounced",
          "IpfsWrapper::ProviderCount" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string C = Command::Str(A, "cid");
            json Out = {{"cid", C}, {"held", IpfsWrapper::HasLocal(C)}, {"whole", IpfsWrapper::HeldWhole(C)}, {"backing_gone", IpfsWrapper::CidMissing(C)},
                        {"size", IpfsWrapper::CidSizeLocal(C)}, {"file_size", IpfsWrapper::CidFileSizeLocal(C)},
                        {"serves", IpfsWrapper::CidServeStatus(C)}, {"announced", IpfsWrapper::SeedAnnounced(C)}};
            if (Command::Bool(A, "network", false)) Out["network_size"] = IpfsWrapper::CidSize(C);
            if (Command::Bool(A, "verify", false)) Out["verified"] = IpfsWrapper::VerifyCid(C);
            if (Command::Bool(A, "providers", false)) Out["providers"] = IpfsWrapper::ProviderCount(C);
            return Out;
        }, {} });

    Command::Register({
        "ipfs pins", "every pinned CID (what this node keeps and serves)", "The pin set the IPFS tab reconciles against.", {},
        { "ipfs pins", "ipfs pins --json" }, { "IpfsWrapper::Pins" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (const auto &P : IpfsWrapper::Pins()) Out.push_back(P.Cid);
            return Out;
        }, {} });

    Command::Register({
        "ipfs forget", "stop keeping a CID: unpin it, drop its file reference, or purge its cached blocks",
        "--unpin stops serving it (the file on disk stays); --drop-ref drops its filestore reference (re-add to serve "
        "again); --cached purges the partial blocks a cancelled download left. One of them is required.",
        { CidArg(), Flag("unpin", "unpin it"), Flag("drop-ref", "drop its filestore reference"), Flag("cached", "purge its cached blocks") },
        { "ipfs forget Qm… --unpin", "ipfs forget Qm… --cached" }, { "IpfsWrapper::Unpin", "IpfsWrapper::DropRef", "IpfsWrapper::DropCached" },
        Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string C = Command::Str(A, "cid");
            json Out = json::object();
            if (Command::Bool(A, "unpin", false)) Out["unpinned"] = IpfsWrapper::Unpin(C);
            if (Command::Bool(A, "drop-ref", false)) Out["ref_dropped"] = IpfsWrapper::DropRef(C);
            if (Command::Bool(A, "cached", false)) Out["purged"] = IpfsWrapper::DropCached(C);
            if (Out.empty()) throw Command::BadArgs("give --unpin, --drop-ref or --cached");
            return Out;
        }, {} });

    Command::Register({
        "ipfs move-refs", "re-point filestore references from one path to another (a moved file)",
        "After a content file moved on disk, the store's references follow it (no re-hash). Pairs of from → to.",
        { WithSample(Opt("moves", "[[from, to], …] (JSON)", Type::Json, true), "[[\"/a/x.zip\",\"/b/x.zip\"]]") }, { "ipfs move-refs --moves '[[\"/a/x.zip\",\"/b/x.zip\"]]'" },
        { "IpfsWrapper::MoveRefs" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::vector<std::pair<std::string, std::string>> M;
            for (const auto &P : A["moves"]) M.emplace_back(P.at(0).get<std::string>(), P.at(1).get<std::string>());
            std::string Err;
            const long long N = IpfsWrapper::MoveRefs(M, &Err);
            if (N < 0) throw std::runtime_error(Err);
            return json{{"moved", N}};
        }, {} });

    Command::Register({
        "ipfs health", "what the node cannot serve, and how the network looks",
        "References whose file is gone or changed, references pointing at paths nothing names, recent serve failures, "
        "the services' health; with --network, the reachability tests (NAT, relays, DHT) the Network tab runs.",
        { Flag("network", "run the network reachability tests") }, { "ipfs health", "ipfs health --network" },
        { "IpfsWrapper::UnservableRefs", "IpfsWrapper::OrphanedRefPaths", "IpfsWrapper::ServeFailures", "IpfsWrapper::ServiceHealth",
          "IpfsWrapper::NetworkTest" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            json Un = json::array(), Fails = json::array();
            for (const auto &U : IpfsWrapper::UnservableRefs()) Un.push_back({{"cid", U.Cid}, {"path", U.Path}, {"status", U.Status}, {"error", U.Err}});
            for (const auto &F : IpfsWrapper::ServeFailures()) Fails.push_back({{"cid", F.Cid}, {"error", F.Err}, {"when", F.When}});
            json Out = {{"unservable", Un}, {"orphaned_paths", IpfsWrapper::OrphanedRefPaths()}, {"serve_failures", Fails},
                        {"services", Checks(IpfsWrapper::ServiceHealth())}};
            if (Command::Bool(A, "network", false))
            {
                bool Offline = false;
                Out["network"] = Checks(IpfsWrapper::NetworkTest(&Offline));
                Out["node_offline"] = Offline;
            }
            return Out;
        }, {} });

    Command::Register({
        "ipfs connect", "dial a peer by multiaddress", "Connect the node to a peer directly (/ip4/…/p2p/<id>).",
        { Pos("addr", "the multiaddress") }, { "ipfs connect /ip4/10.10.0.5/udp/4001/quic-v1/p2p/12D3Koo…" }, { "IpfsWrapper::Connect" }, Needs::Config, true,
        [](Command::Context &, const json &A) { return json{{"connected", IpfsWrapper::Connect(Command::Str(A, "addr"))}}; }, {} });

    Command::Register({
        "ipfs resolve", "resolve an IPNS name to the CID it points at",
        "DHT first, then the gateway fallback; the record's signature is checked against the name.", { Pos("name", "the /ipns/ name or peer ID") },
        { "ipfs resolve 12D3KooW…" }, { "IpfsWrapper::IpnsResolve" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            return json{{"cid", Must(IpfsWrapper::IpnsResolve(Command::Str(A, "name"), &Err), Err)}};
        }, {} });

    Command::Register({
        "ipfs identity", "back up the node's identity key, or restore one",
        "The Ed25519 key that IS your friend code. --export writes it to a file (keep it safe: losing it loses your "
        "address for good); --import installs one (the node uses it from its next start) — --yes required.",
        { Opt("export", "write the key here"), Opt("import", "install the key from here"), Flag("yes", "yes, replace my identity") },
        { "ipfs identity --export ~/vidyagod-identity.key" }, { "IpfsWrapper::ExportIdentity", "IpfsWrapper::ImportIdentity" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            if (A.contains("export")) { Check(IpfsWrapper::ExportIdentity(Command::Str(A, "export"), &Err), Err); return json{{"exported", Command::Str(A, "export")}}; }
            if (A.contains("import"))
            {
                if (!Command::Bool(A, "yes", false)) throw Command::BadArgs("this replaces your identity (your friend code): add --yes");
                Check(IpfsWrapper::ImportIdentity(Command::Str(A, "import"), &Err), Err);
                return json{{"imported", Command::Str(A, "import")}, {"restart_needed", true}};
            }
            throw Command::BadArgs("give --export or --import");
        }, {} });

    Command::Register({
        "ipfs seed-levels", "which collections and packages the node announces first",
        "Set the providing priority: these collection CIDs, then these package folder CIDs, announced ahead of leaves.",
        { Opt("collections", "collection CIDs (a,b)"), Opt("packages", "package folder CIDs (a,b)") }, { "ipfs seed-levels --collections bafybei…" },
        { "IpfsWrapper::SetSeedLevels" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            IpfsWrapper::SetSeedLevels(List(A, "collections"), List(A, "packages"));
            return json{{"collections", List(A, "collections")}, {"packages", List(A, "packages")}};
        }, {} });

    Command::Register({
        "ipfs share", "publish (or withdraw) a library share to one friend at the node",
        "What the share sheet sends: library <lib> with its package items, to one peer — the node-level call under "
        "`friends share`. --unshare withdraws it.",
        { Pos("peer", "the friend's peer ID"), Pos("library", "the library name"), WithSample(Opt("items", "the share items (JSON)", Type::Json), "[]"),
          Flag("unshare", "withdraw the share") },
        { "ipfs share 12D3KooW… VidyaGod --items '[]'", "ipfs share 12D3KooW… VidyaGod --unshare" },
        { "IpfsWrapper::ShareLibrary", "IpfsWrapper::UnshareLibrary" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            const std::string P = Command::Str(A, "peer"), L = Command::Str(A, "library");
            if (Command::Bool(A, "unshare", false)) { Check(IpfsWrapper::UnshareLibrary(P, L, &Err), Err); return json{{"unshared", L}}; }
            const nlohmann::json Items = nlohmann::json::parse(A.value("items", json::array()).dump());
            Check(IpfsWrapper::ShareLibrary(P, L, Items, &Err), Err);
            return json{{"shared", L}};
        }, {} });

    Command::Register({
        "ipfs accept", "accept a friend request at the node",
        "The node-level accept (`friends accept` does it with the app's bookkeeping).", { Pos("peer", "the peer ID") },
        { "ipfs accept 12D3KooW…" }, { "IpfsWrapper::FriendAccept" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            Check(IpfsWrapper::FriendAccept(Command::Str(A, "peer"), &Err), Err);
            return json{{"accepted", Command::Str(A, "peer")}};
        }, {} });

    Command::Register({
        "ipfs overlay", "the friend LAN overlay: its state, start it, serve a socket, stop it",
        "The virtual LAN friends' games talk over (10.66.x.y): active or not, the launch variables a game gets, and "
        "which friends are kept off it (--exclude) or denied presence (--deny). --start brings it up, --serve "
        "attaches a TUN socket, --stop takes it down.",
        { Flag("start", "start it"), Opt("serve", "serve this TUN socket path"), Flag("bridge", "(--serve) bridge mode"),
          Flag("host-relay", "(--serve) relay for the host"), Flag("stop", "stop it"), Opt("exclude", "peers off the LAN (a,b)"),
          Opt("deny", "peers denied presence (a,b)") },
        { "ipfs overlay", "ipfs overlay --start", "ipfs overlay --exclude 12D3KooW…" },
        { "IpfsWrapper::OverlayActive", "IpfsWrapper::OverlayStart", "IpfsWrapper::OverlayServe", "IpfsWrapper::OverlayStop",
          "IpfsWrapper::LanLaunchVars", "IpfsWrapper::SetLanExcluded", "IpfsWrapper::SetPresenceDeny" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            json Out = json::object();
            if (Command::Bool(A, "start", false)) Out["started"] = Must(IpfsWrapper::OverlayStart(&Err), Err);
            if (A.contains("serve"))
                Check(IpfsWrapper::OverlayServe(Command::Str(A, "serve"), Command::Bool(A, "bridge", false), Command::Bool(A, "host-relay", false), &Err), Err);
            if (A.contains("exclude")) IpfsWrapper::SetLanExcluded(List(A, "exclude"));
            if (A.contains("deny")) Out["deny_set"] = IpfsWrapper::SetPresenceDeny(List(A, "deny"));
            if (Command::Bool(A, "stop", false)) IpfsWrapper::OverlayStop();
            Out["active"] = IpfsWrapper::OverlayActive();
            Out["launch_vars"] = IpfsWrapper::LanLaunchVars();
            return Out;
        }, {} });

    // ---------------- queue ----------------
    Command::Register({
        "queue job", "one CID's job in the network queue: its state, where it lands, its scheduling",
        "The queue's view of a CID: state (none, queued, active, done, failed), unfinished, its destination, backing "
        "off a retry, being pre-empted to free a slot, prioritised.", { CidArg() }, { "queue job Qm…" },
        { "IpfsWrapper::DebugJobState", "IpfsWrapper::JobUnfinished", "IpfsWrapper::QueueDestForCid", "IpfsWrapper::DebugJobBackingOff",
          "IpfsWrapper::DebugIsPreempting", "IpfsWrapper::DebugJobPrioritized" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string C = Command::Str(A, "cid");
            static const char *States[] = { "queued", "active", "done", "failed" };
            const int S = IpfsWrapper::DebugJobState(C);
            return json{{"cid", C}, {"state", S < 0 || S > 3 ? "none" : States[S]}, {"unfinished", IpfsWrapper::JobUnfinished(C)},
                        {"dest", IpfsWrapper::QueueDestForCid(C)}, {"backing_off", IpfsWrapper::DebugJobBackingOff(C)},
                        {"preempting", IpfsWrapper::DebugIsPreempting(C)}, {"prioritized", IpfsWrapper::DebugJobPrioritized(C)}};
        }, {} });

    Command::Register({
        "queue net", "the network queue's foreground state",
        "Whether no fetch holds or waits for a slot and nobody holds the foreground (the node's own background work "
        "runs then), and how many foreground holds were taken since start.", {}, { "queue net" },
        { "IpfsWrapper::DebugNetForegroundIdle", "IpfsWrapper::DebugNetHolds" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            return json{{"foreground_idle", IpfsWrapper::DebugNetForegroundIdle()}, {"foreground_holds", IpfsWrapper::DebugNetHolds()}};
        }, {} });

    Command::Register({
        "queue enqueue", "enqueue a batch of targets, and (with --until-done) wait for it",
        "Add {cid, to, optional?, dir?, verify?} targets to the queue as one batch — what a download does — and with "
        "--until-done block until every required one is done (--timeout ms).",
        { WithSample(Opt("targets", "the targets (JSON array)", Type::Json, true), "[{\"cid\":\"Qm…\",\"to\":\"/tmp/a\"}]"), Flag("until-done", "wait for the batch"), Opt("timeout", "(--until-done) ms", Type::Int) },
        { "queue enqueue --targets '[{\"cid\":\"Qm…\",\"to\":\"/tmp/a\"}]' --until-done" },
        { "IpfsWrapper::EnqueueBatch", "IpfsWrapper::WaitBatch" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const auto H = IpfsWrapper::EnqueueBatch(TargetsArg(A));
            json Items = json::array();
            for (const auto &[Cid, Opt] : H.Items) Items.push_back({{"cid", Cid}, {"optional", Opt}});
            json Out = {{"batch", Items}};
            if (Command::Bool(A, "until-done", false))
            {
                std::string Err;
                const bool Ok = A.contains("timeout") ? IpfsWrapper::WaitBatch(H, static_cast<int>(Command::Int(A, "timeout")), &Err)
                                                      : IpfsWrapper::WaitBatch(H, &Err);
                Out["done"] = Ok;
                if (!Ok) Out["error"] = Err;
            }
            return Out;
        }, {} });

    Command::Register({
        "queue cancel", "cancel a CID's download (or only flag it, or clear the flag)",
        "Drop a queued job or abort an active one. --flag only marks it cancelled for the fetch to notice; --clear "
        "removes the mark (a later fetch of it may run).", { CidArg(), Flag("flag", "only mark it cancelled"), Flag("clear", "clear the mark") },
        { "queue cancel Qm…", "queue cancel Qm… --clear" },
        { "IpfsWrapper::CancelDownload", "IpfsWrapper::RequestCancel", "IpfsWrapper::ClearCancel" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string C = Command::Str(A, "cid");
            if (Command::Bool(A, "clear", false)) IpfsWrapper::ClearCancel(C);
            else if (Command::Bool(A, "flag", false)) IpfsWrapper::RequestCancel(C);
            else IpfsWrapper::CancelDownload(C);
            return json{{"cid", C}, {"state", IpfsWrapper::DebugJobState(C)}};
        }, {} });

    Command::Register({
        "queue prioritize", "move a queued CID to the front", "The next free slot takes it.", { CidArg() }, { "queue prioritize Qm…" },
        { "IpfsWrapper::PrioritizeDownload" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            IpfsWrapper::PrioritizeDownload(Command::Str(A, "cid"));
            return json{{"prioritized", IpfsWrapper::DebugJobPrioritized(Command::Str(A, "cid"))}};
        }, {} });

    Command::Register({
        "queue expect", "tell the queue how big a CID is (for progress)", "Its size in bytes, as the node's SIZE says.",
        { CidArg(), Pos("bytes", "the size", Type::Int) }, { "queue expect Qm… 1048576" }, { "IpfsWrapper::SetExpectedSize" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            IpfsWrapper::SetExpectedSize(Command::Str(A, "cid"), Command::Int(A, "bytes"));
            return json{{"cid", Command::Str(A, "cid")}, {"bytes", Command::Int(A, "bytes")}};
        }, {} });

    Command::Register({
        "queue landing", "fetches landing under a folder, and moving them",
        "Whether any queued or active fetch lands under <dir>; where a path is now after a folder moved (--path); "
        "--move-to re-points every destination under <dir> to the new folder (a package was moved); --forget drops "
        "those destinations; --drop-redirects forgets the moves recorded under <dir>.",
        { Pos("dir", "the folder"), Opt("path", "a path: where it is now"), Opt("move-to", "re-point destinations under dir here"),
          Flag("forget", "forget destinations under dir"), Flag("drop-redirects", "forget the moves under dir") },
        { "queue landing ~/.VidyaGod/LIBRARY/VidyaGod/x", "queue landing ~/a --move-to ~/b" },
        { "IpfsWrapper::LandingUnder", "IpfsWrapper::Redirected", "IpfsWrapper::RedirectDestsUnder", "IpfsWrapper::ForgetDestsUnder",
          "IpfsWrapper::DropRedirectsUnder" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string D = Command::Str(A, "dir");
            if (A.contains("move-to")) IpfsWrapper::RedirectDestsUnder(D, Command::Str(A, "move-to"));
            if (Command::Bool(A, "forget", false)) IpfsWrapper::ForgetDestsUnder(D);
            if (Command::Bool(A, "drop-redirects", false)) IpfsWrapper::DropRedirectsUnder(D);
            json Out = {{"landing", IpfsWrapper::LandingUnder(D)}};
            if (A.contains("path")) Out["now_at"] = IpfsWrapper::Redirected(Command::Str(A, "path"));
            return Out;
        }, {} });

    Command::Register({
        "queue gate", "close the queue (every wait returns, every fetch fails) or open it again",
        "What shutdown does first: --close makes every queued job and wait fail at once; --open reverses it. --reset "
        "drops every job and its scheduling state (isolation between test runs).",
        { Flag("close", "close it"), Flag("open", "open it"), Flag("reset", "drop every job") }, { "queue gate --close", "queue gate --open" },
        { "IpfsWrapper::CloseQueue", "IpfsWrapper::OpenQueue", "IpfsWrapper::DebugResetQueue" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (Command::Bool(A, "close", false)) IpfsWrapper::CloseQueue();
            if (Command::Bool(A, "open", false)) IpfsWrapper::OpenQueue();
            if (Command::Bool(A, "reset", false)) IpfsWrapper::DebugResetQueue();
            return json{{"ok", true}};
        }, {} });

    // ---------------- transfers (the IPFS tab's model) ----------------
    Command::Register({
        "transfers list", "every CID the IPFS tab tracks, with its phase and progress, and the node strip",
        "The IPFS tab's rows exactly: label, package, category, library, phase (pending, queued, downloading, pinning, "
        "stalled, errored, seeded), percent, size, speed, providers, a missing backing file, uploading, announced — "
        "and the status strip. Filter by --phase or --search.",
        { Opt("phase", "only this phase"), Opt("search", "label or package contains this") },
        { "transfers list", "transfers list --phase downloading", "transfers list --search 'Age of' --json" },
        { "IpfsModel::cids", "IpfsModel::nodeStatus", "MainWindow::ipfsModel" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            IpfsModel &M = *Win(C).ipfsModel();
            const QString Ph = QString::fromStdString(Command::Str(A, "phase", "")), Q = QString::fromStdString(Command::Str(A, "search", ""));
            json Rows = json::array();
            for (auto It = M.cids().constBegin(); It != M.cids().constEnd(); ++It)
            {
                if (!Ph.isEmpty() && PhaseName(It->phase) != Ph) continue;
                if (!Q.isEmpty() && !It->label.contains(Q, Qt::CaseInsensitive) && !It->package.contains(Q, Qt::CaseInsensitive)) continue;
                Rows.push_back(Row(It.key(), It.value()));
            }
            const auto &S = M.nodeStatus();
            return json{{"node", {{"available", S.available}, {"online", S.daemon}, {"peers", S.peers}, {"store", S.repo.toStdString()},
                                  {"down_bps", S.downBps}, {"up_bps", S.upBps}, {"disk_free", S.diskFree}, {"pins", S.pinCount}, {"total", S.totalSize}}},
                        {"cids", Rows}};
        }, {} });

    Command::Register({
        "transfers show", "one CID's row in the IPFS tab", "Its full state, its size and percent as the download manager reads them.",
        { CidArg() }, { "transfers show Qm…" },
        { "IpfsModel::has", "IpfsModel::state", "IpfsModel::stateRef", "IpfsModel::pct", "IpfsModel::size" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            IpfsModel &M = *Win(C).ipfsModel();
            const QString Q = QString::fromStdString(Command::Str(A, "cid"));
            if (!M.has(Q) || !M.stateRef(Q)) throw Command::BadArgs("the IPFS tab tracks no '" + Q.toStdString() + "'");
            json R = Row(Q, M.state(Q));
            R["pct_read"] = M.pct(Q);
            R["size_read"] = M.size(Q);
            return R;
        }, {} });

    Command::Register({
        "transfers package", "a package's folder in the IPFS tab, and its folder CID",
        "Where the tab's package row points, and the folder CID others add to receive exactly that package.",
        { Pos("package", "the package name as the tab shows it") }, { "transfers package 'Age of Empires II'" },
        { "IpfsModel::packageDir", "IpfsModel::packageCid" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            IpfsModel &M = *Win(C).ipfsModel();
            const QString P = QString::fromStdString(Command::Str(A, "package"));
            return json{{"dir", M.packageDir(P).toStdString()}, {"cid", M.packageCid(P).toStdString()}};
        }, {} });

    Command::Register({
        "transfers act", "what the IPFS tab's buttons do: cancel, prioritise, recheck, unpin, seed, add a source",
        "The tab's actions on CIDs (comma-separated): --cancel aborts a download, --prioritize jumps the queue, "
        "--recheck re-counts providers and stats backing files, --force-recheck reads every block back, --unpin stops "
        "seeding; --seed adds a folder's content; --add-source CID,NAME adds a package source; --refresh re-gathers "
        "now; --active on|off starts/stops the periodic refresh (as showing the tab does).",
        { Opt("cancel", "CIDs"), Opt("prioritize", "CIDs"), Opt("recheck", "CIDs"), Opt("force-recheck", "CIDs"), Opt("unpin", "CIDs"),
          Opt("seed", "a folder"), Opt("add-source", "CID,NAME"), Flag("refresh", "re-gather now"), Opt("active", "on or off", Type::String, false, { "on", "off" }) },
        { "transfers act --cancel Qm…", "transfers act --force-recheck Qm…,Qm…", "transfers act --refresh" },
        { "IpfsModel::cancel", "IpfsModel::prioritize", "IpfsModel::recheckHealth", "IpfsModel::forceRecheck", "IpfsModel::unpinMany",
          "IpfsModel::seedFolder", "IpfsModel::addSource", "IpfsModel::refreshNow", "IpfsModel::setActive" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            IpfsModel &M = *Win(C).ipfsModel();
            for (const QString &X : Cids(A, "cancel")) M.cancel(X);
            for (const QString &X : Cids(A, "prioritize")) M.prioritize(X);
            if (A.contains("recheck")) M.recheckHealth(Cids(A, "recheck"));
            if (A.contains("force-recheck")) M.forceRecheck(Cids(A, "force-recheck"));
            if (A.contains("unpin")) M.unpinMany(Cids(A, "unpin"));
            if (A.contains("seed")) M.seedFolder(QString::fromStdString(Command::Str(A, "seed")));
            if (A.contains("add-source"))
            {
                const auto P = List(A, "add-source");
                if (P.size() != 2) throw Command::BadArgs("--add-source takes CID,NAME");
                M.addSource(QString::fromStdString(P[0]), QString::fromStdString(P[1]));
            }
            if (A.contains("active")) M.setActive(Command::Str(A, "active") == "on");
            if (Command::Bool(A, "refresh", false)) M.refreshNow();
            return json{{"ok", true}, {"follow", "VidyaGod events follow --object IpfsModel"}};
        }, {} });

    // ---------------- downloads ----------------
    Command::Register({
        "downloads list", "the downloads in flight: each one's CIDs and what cancelling would drop",
        "Every Catalog download running (by its card key): its content CIDs, the ones it alone needs (what Cancel "
        "aborts), what a cancel would purge, and the download workers not yet joined.", {}, { "downloads list" },
        { "MainWindow::downloads", "DownloadManager::activeKeys", "DownloadManager::cidsOf", "DownloadManager::cancellableCids", "DownloadManager::purgeOnCancel",
          "DownloadManager::workerCount" }, Needs::Ui, false,
        [](Command::Context &C, const json &) {
            DownloadManager &D = *Win(C).downloads();
            json Rows = json::array();
            for (const QString &K : D.activeKeys())
                Rows.push_back({{"key", K.toStdString()}, {"cids", Keys(D.cidsOf(K))}, {"own", Keys(D.cancellableCids(K))}, {"purge", Keys(D.purgeOnCancel(K))}});
            return json{{"downloads", Rows}, {"workers", D.workerCount()}};
        }, {} });

    Command::Register({
        "downloads start", "download games (and runners) the way the Catalog's Download does",
        "Start the download of these game nodes (and --runners), under a card key (default: the first game's family "
        "UID): the batch goes into the queue, the cards show progress, and the run is remembered for resume. "
        "--toggle k=1 picks optional pieces. With --card, it is the Catalog card's own Download (its dialog included). "
        "Follow it with `events wait --signal downloadFinished`.",
        { Opt("games", "game nodes (a,b)"), Opt("runners", "runner nodes (a,b)"), Opt("key", "the card key"), Opt("toggle", "options: k=1,k2=0"),
          Opt("card", "a Catalog card (its title or key): press its Download") },
        { "downloads start --games bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka", "downloads start --card 749" },
        { "DownloadManager::beginDownload", "DownloadManager::startDownload", "IpfsModel::markQueued", "IpfsModel::clearQueued",
          "MainWindow::catalogCard", "CatalogTab::view", "LibraryView::cardAt" , "CatalogTab::markDownloading", "CatalogTab::setDownloadProgress", "CatalogTab::clearDownloading", "IpfsFetchReady" }, Needs::Ui, true,
        [](Command::Context &C, const json &A) {
            MainWindow &W = Win(C);
            if (A.contains("card"))
            {
                LibraryGameCard *Card = W.catalogCard(QString::fromStdString(Command::Str(A, "card")));
                if (!Card) throw Command::BadArgs("no single Catalog card '" + Command::Str(A, "card") + "'"
                                                  + W.cardMiss(true, QString::fromStdString(Command::Str(A, "card"))).toStdString());
                W.downloads()->startDownload(Card);
                return json{{"started", Command::Str(A, "card")}};
            }
            const auto I = Index(C);
            std::vector<std::string> G, R;
            for (const auto &X : List(A, "games")) G.push_back(NodeOf(*I, X)->Key());
            for (const auto &X : List(A, "runners")) R.push_back(NodeOf(*I, X)->Key());
            if (G.empty() && R.empty()) throw Command::BadArgs("give --games, --runners or --card");
            std::map<std::string, bool> T;
            for (const auto &KV : List(A, "toggle"))
            {
                const auto Eq = KV.find('=');
                T[KV.substr(0, Eq)] = Eq == std::string::npos || KV.substr(Eq + 1) != "0";
            }
            const std::string Key = Command::Str(A, "key", G.empty() ? R.front() : I->Find(G.front())->GameKey());
            W.downloads()->beginDownload(QString::fromStdString(Key), G, R, T);
            return json{{"started", Key}, {"follow", "VidyaGod events wait --signal downloadFinished"}};
        }, {} });

    Command::Register({
        "downloads cancel", "cancel a download (what it alone needs is aborted; shared pieces keep going)",
        "Cancel by card key. With --card, it is the Catalog card's own Cancel (its confirmation included).",
        { Opt("key", "the download's card key"), Opt("card", "a Catalog card: press its Cancel") },
        { "downloads cancel --key 749", "downloads cancel --card 749" },
        { "DownloadManager::cancelKey", "DownloadManager::requestCancel" }, Needs::Ui, false,
        [](Command::Context &C, const json &A) {
            MainWindow &W = Win(C);
            if (A.contains("card"))
            {
                LibraryGameCard *Card = W.catalogCard(QString::fromStdString(Command::Str(A, "card")));
                if (!Card) throw Command::BadArgs("no single Catalog card '" + Command::Str(A, "card") + "'"
                                                  + W.cardMiss(true, QString::fromStdString(Command::Str(A, "card"))).toStdString());
                W.downloads()->requestCancel(Card);
                return json{{"cancelling", Command::Str(A, "card")}};
            }
            W.downloads()->cancelKey(QString::fromStdString(Command::Str(A, "key")));
            return json{{"cancelled", Command::Str(A, "key")}};
        }, {} });

    Command::Register({
        "downloads resume", "resume the downloads a previous run left unfinished",
        "What start-up does once online: every remembered download that did not finish starts again.", {}, { "downloads resume" },
        { "DownloadManager::resumeAll" }, Needs::Ui, true,
        [](Command::Context &C, const json &) {
            Win(C).downloads()->resumeAll();
            return json{{"resumed", Keys(Win(C).downloads()->activeKeys())}};
        }, {} });
}
