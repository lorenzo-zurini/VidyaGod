// `VidyaGod peers …` — the Network tab from the command line: your peer code and nickname, the address book,
// requests, and per peer what you share, what you receive, presence and the virtual LAN. Every command calls the
// AppModel / IpfsWrapper function the tab calls.
#include "command.h"
#include "appmodel.h"
#include "peersharing.h"
#include "ipfswrapper.h"

#include <algorithm>

namespace
{

using json = nlohmann::ordered_json;

// A peer named on the command line: a full peer id, a unique start of one, or a nickname (any case).
std::string Peer(const std::string &Q)
{
    const auto All = IpfsWrapper::PeerList();
    for (const auto &C : All) if (C.PeerID == Q) return Q;
    std::vector<const IpfsWrapper::Contact *> Hits;
    for (const auto &C : All)
        if (C.PeerID.rfind(Q, 0) == 0 || QString::fromStdString(C.Nick).compare(QString::fromStdString(Q), Qt::CaseInsensitive) == 0)
            Hits.push_back(&C);
    if (Hits.size() == 1) return Hits.front()->PeerID;
    if (Hits.empty())
    {
        if (Q.rfind("12D3KooW", 0) == 0 && Q.size() > 40) return Q;   // a full code not (yet) in the book
        throw Command::BadArgs("no peer '" + Q + "' (VidyaGod peers list shows them)");
    }
    std::string Names;
    for (const auto *C : Hits) Names += (Names.empty() ? "" : ", ") + (C->Nick.empty() ? C->PeerID : C->Nick + " (" + C->PeerID.substr(0, 16) + "…)");
    throw Command::BadArgs("'" + Q + "' matches " + std::to_string(Hits.size()) + " peers: " + Names);
}

json ContactJson(const IpfsWrapper::Contact &C, AppModel *M)
{
    json J = {{"peer", C.Nick.empty() ? C.PeerID.substr(0, 16) + "…" : C.Nick}, {"state", C.State}, {"online", C.Online},
              {"peer", C.PeerID}};
    if (M)
    {
        const QString P = QString::fromStdString(C.PeerID);
        json Shared = json::array();
        for (const QString &L : M->peers().myLibraryNames()) if (M->peers().isSharingWithPeer(P, L)) Shared.push_back(L.toStdString());
        J["sharing"] = Shared;
        J["receiving"] = M->peers().isReceivingFrom(P);
        if (M->peers().isReceivingFrom(P))
        {
            const auto R = M->peers().receivedFrom(P);
            J["received"] = {{"packages", R.Packages}, {"here", R.Here}};
        }
        J["presence"] = M->peers().isPresenceSharedWith(P);
        J["vlan"] = M->peers().isInVlan(P);
    }
    if (C.LastSeen > 0) J["last_seen_ms"] = C.LastSeen;
    return J;
}


// The permissions pairing gives (the Peers page's ticks, the table's columns): --share <libraries>, --receive,
// --vlan, --presence. Given none of them, the "New peers" row, as the page's ticks start.
std::vector<Command::Arg> PermissionArgs()
{
    return { Command::Opt("share", "the libraries they get, comma-separated (VidyaGod,VidyaGodRunners,…)"),
             Command::Flag("receive", "you receive their libraries"), Command::Flag("vlan", "they are in your virtual LAN"),
             Command::Flag("presence", "they see when you are online") };
}

PeerSharing::Permissions PermissionsFrom(Command::Context &C, const json &A)
{
    if (!A.contains("share") && !A.contains("receive") && !A.contains("vlan") && !A.contains("presence"))
        return C.Model->peers().defaultPermissions();
    PeerSharing::Permissions P;
    P.Presence = Command::Bool(A, "presence", false);
    P.Receive = Command::Bool(A, "receive", false);
    P.Vlan = Command::Bool(A, "vlan", false);
    const QStringList Mine = C.Model->peers().myLibraryNames();
    for (const QString &L : QString::fromStdString(Command::Str(A, "share", "")).split(',', Qt::SkipEmptyParts))
    {
        if (!Mine.contains(L.trimmed())) throw Command::BadArgs("you have no library '" + L.trimmed().toStdString() + "' (" + Mine.join(", ").toStdString() + ")");
        P.Libraries << L.trimmed();
    }
    return P;
}

json PermissionsJson(const PeerSharing::Permissions &P)
{
    json Libs = json::array();
    for (const QString &L : P.Libraries) Libs.push_back(L.toStdString());
    return {{"share", Libs}, {"receive", P.Receive}, {"vlan", P.Vlan}, {"presence", P.Presence}};
}

std::string Fail(const std::string &What, const std::string &Err)
{
    return What + (Err.empty() ? "" : ": " + Err);
}

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    Command::RegisterGroup("peers", "your peer code, peers, requests, and what you share and receive");

    Command::Register({
        "peers code", "your peer code and nickname",
        "The code a peer pastes to add you (your node's peer id), and the nickname they see.", {},
        { "peers code" }, { "IpfsWrapper::PeerCode", "IpfsWrapper::GetProfile" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            const std::string Code = IpfsWrapper::PeerCode();
            if (Code.empty()) throw std::runtime_error("the node is not up — no peer code yet");
            return json{{"code", Code}, {"nickname", IpfsWrapper::GetProfile().Nick}};
        }, {} });

    Command::Register({
        "peers nick", "set the nickname peers see",
        "What the Network tab's Nickname field + Save does.", { Command::Pos("name", "the nickname") },
        { "peers nick LGZ-MacBook" }, { "PeerSharing::setNickname", "IpfsWrapper::SetProfile" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            if (!C.Model->peers().setNickname(QString::fromStdString(Command::Str(A, "name")), &Err))
                throw std::runtime_error(Fail("could not set the nickname", Err));
            return json{{"nickname", IpfsWrapper::GetProfile().Nick}};
        }, {} });

    Command::Register({
        "peers list", "every peer and request, with what you share and receive",
        "The Peers page's table: each peer's state (accepted, incoming, pending, blocked), whether it is online, the "
        "libraries you share with it, whether you receive its libraries (and how many of its packages are here), and "
        "whether it sees your presence and is in your virtual LAN.",
        { Command::Opt("state", "only contacts in this state", Type::String, false, {"accepted", "incoming", "pending", "blocked"}) },
        { "peers list", "peers list --state incoming", "peers list --json" },
        { "IpfsWrapper::PeerList", "PeerSharing::myLibraryNames", "PeerSharing::isSharingWithPeer", "PeerSharing::isReceivingFrom",
          "PeerSharing::isPresenceSharedWith", "PeerSharing::isInVlan", "PeerSharing::receivedFrom", "PeerSharing::hasChosen",
          "NetworkTab::rebuildRows", "NetworkTab::rebuildTable" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string Only = Command::Str(A, "state", "");
            auto All = IpfsWrapper::PeerList();
            std::sort(All.begin(), All.end(), [](const auto &X, const auto &Y) { return X.Nick < Y.Nick; });
            json Out = json::array();
            for (const auto &K : All) if (Only.empty() || K.State == Only) Out.push_back(ContactJson(K, C.Model));
            return Out;
        }, {} });

    {
    std::vector<Command::Arg> Args{ Command::Pos("code", "their peer code (12D3KooW…)") };
    for (auto &X : PermissionArgs()) Args.push_back(X);
    Command::Register({
        "peers add", "send a pairing request by code",
        "The Peers page's \"Their code\": ask someone by their code, with the permissions you give them — the table's "
        "columns: the libraries they get (--share), whether you receive theirs, have them in your virtual LAN, show them "
        "your presence. Without any, the \"New peers\" row.",
        Args,
        { "peers add 12D3KooWQs9BMz6DtSWeFM6x98vjspCTVFSwTKwGmGDwjjgjXtuA", "peers add 12D3KooWQs9BMz6DtSWeFM6x98vjspCTVFSwTKwGmGDwjjgjXtuA --share VidyaGod --receive --presence" },
        { "PeerSharing::sendPeerRequest", "IpfsWrapper::PeerAdd", "NetworkTab::addPeerClicked", "PeerSharing::defaultPermissions" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            const std::string Code = Command::Str(A, "code");
            const PeerSharing::Permissions P = PermissionsFrom(C, A);
            if (!C.Model->peers().pairWith(QString::fromStdString(Code), P, &Err)) throw std::runtime_error(Fail("could not send the request", Err));
            json Out = {{"requested", Code}};
            Out.update(PermissionsJson(P));
            return Out;
        }, {} });
    }

    {
    std::vector<Command::Arg> Args{ Command::Pos("peer", "their nickname, code, or the start of it") };
    for (auto &X : PermissionArgs()) Args.push_back(X);
    Command::Register({
        "peers accept", "accept a pairing request",
        "The Peers page's \"Waiting on you\": accept a request, with the permissions you give them — the table's columns: "
        "the libraries they get (--share), whether you receive theirs, have them in your virtual LAN, show them your "
        "presence. Without any, the \"New peers\" row.",
        Args,
        { "peers accept LGZ-MacBook", "peers accept LGZ-PC --share VidyaGod,VidyaGodRunners --receive --vlan --presence" },
        { "PeerSharing::acceptPairing", "PeerSharing::applyPairing", "NetworkTab::acceptClicked", "NetworkTab::permissionTicks",
          "NetworkTab::ticks" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "peer"));
            if (!C.Model->peers().acceptPairing(QString::fromStdString(P), PermissionsFrom(C, A)))
                throw std::runtime_error("could not accept: networking is off or still starting");
            for (const auto &K : IpfsWrapper::PeerList()) if (K.PeerID == P) return ContactJson(K, C.Model);
            return json{{"peer", P}};
        }, {} });
    }

    Command::Register({
        "peers nearby", "the VidyaGod devices on your network, by name",
        "\"On your network\" in the Network tab: every VidyaGod found on the local network (mDNS), by the nickname it gives, "
        "and what it is to you (\"\" = nothing yet, pending = you asked it, incoming = it asked you, accepted = a peer). "
        "A device shows a few seconds after it is found; two may give the same name, so each shows its code.",
        {}, { "peers nearby" }, { "IpfsWrapper::NearbyPeers" }, Needs::Model, true,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (const auto &N : IpfsWrapper::NearbyPeers())
                Out.push_back({{"name", N.Nick}, {"state", N.State}, {"peer", N.Peer}});
            return Out;
        }, {} });

    {
    std::vector<Command::Arg> Args{ Command::Pos("who", "its name as `peers nearby` shows it, or the start of its code") };
    for (auto &X : PermissionArgs()) Args.push_back(X);
    Command::Register({
        "peers pair", "pair with a VidyaGod device on your network",
        "The Peers page's \"Add someone\": a request to a device on your network, named as `peers nearby` shows it (or "
        "by the start of its code), with the permissions you give it — the table's columns. Without any, the \"New "
        "peers\" row. Accept it on the other device.",
        Args,
        { "peers pair LGZ-SteamDeck", "peers pair LGZ-SteamDeck --share VidyaGod,VidyaGodRunners --receive --vlan --presence" },
        { "PeerSharing::pairWith", "PeerSharing::applyPairing", "NetworkTab::pairClicked" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string Q = Command::Str(A, "who");
            std::vector<IpfsWrapper::NearbyPeer> Hits;
            for (const auto &N : IpfsWrapper::NearbyPeers())
                if (N.Peer == Q || N.Peer.rfind(Q, 0) == 0
                    || QString::fromStdString(N.Nick).compare(QString::fromStdString(Q), Qt::CaseInsensitive) == 0)
                    Hits.push_back(N);
            if (Hits.empty()) throw Command::BadArgs("no VidyaGod device '" + Q + "' on your network (VidyaGod peers nearby shows them)");
            if (Hits.size() > 1) throw Command::BadArgs("'" + Q + "' matches " + std::to_string(Hits.size()) + " devices: name it by the start of its code");
            std::string Err;
            const PeerSharing::Permissions P = PermissionsFrom(C, A);
            if (!C.Model->peers().pairWith(QString::fromStdString(Hits[0].Peer), P, &Err))
                throw std::runtime_error(Fail("could not ask it to pair", Err));
            json Out = {{"asked", Hits[0].Nick}, {"peer", Hits[0].Peer}};
            Out.update(PermissionsJson(P));
            return Out;
        }, {} });
    }

    Command::Register({
        "peers sharing", "whether what you share is up to date, and what it is waiting on",
        "The Peers page's top line: what you share is up to date, being updated, waiting on networking, or never prepared "
        "(the library's first publish, which names every node file by its label — --prepare starts it). --check looks "
        "again now and starts the background publish of what changed, as a change to the library does by itself.",
        { Command::Flag("check", "look again now"), Command::Flag("prepare", "the first publish of the whole library") },
        { "peers sharing", "peers sharing --check", "peers sharing --prepare" },
        { "PeerSharing::shareState", "PeerSharing::shareStateText", "PeerSharing::checkShares", "PeerSharing::checkSharesSoon",
          "PackageCatalog::StaleSharedPackages", "PackageCatalog::LibrariesOnDisk", "PackageCatalog::DropFromShareSheet",
          "PeerSharing::prepareSharing", "PeerSharing::pump", "AppModel::publishPackages", "AppModel::catalogGeneration" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            if (Command::Bool(A, "prepare", false)) C.Model->peers().prepareSharing();
            else if (Command::Bool(A, "check", false)) C.Model->peers().checkShares();
            static const char *Names[] = { "nothing shared", "not prepared", "offline", "up to date", "updating", "failed" };
            return json{{"state", Names[static_cast<int>(C.Model->peers().shareState())]},
                        {"says", C.Model->peers().shareStateText().toStdString()}, {"publishing", C.Model->publishing()}};
        }, {} });

    for (const auto &[Verb, Summary, Fn] : std::vector<std::tuple<const char *, const char *, void (PeerSharing::*)(const QString &)>>{
             { "decline", "decline a peer request", &PeerSharing::declinePeer },
             { "block", "block a peer (drops all their traffic)", &PeerSharing::blockPeer },
             { "remove", "remove a peer (both directions of sharing end)", &PeerSharing::removePeer } })
    {
        const std::string V = Verb;
        Command::Register({
            "peers " + V, Summary,
            std::string(Summary) + ". You stop sharing with each other, and their shared games leave your catalog; games you "
            "installed from them are kept. The Network tab's ⋮ menu.",
            { Command::Pos("peer", "their nickname, peer code, or the start of it") },
            { "peers " + V + " LGZ-MacBook" },
            { std::string("PeerSharing::") + (V == "decline" ? "declinePeer" : V == "block" ? "blockPeer" : "removePeer"),
              "PeerSharing::forgetPeer", std::string("IpfsWrapper::Peer") + (V == "decline" ? "Decline" : V == "block" ? "Block" : "Remove") },
            Needs::Model, true,
            [Fn = Fn, V](Command::Context &C, const json &A) {
                const std::string P = Peer(Command::Str(A, "peer"));
                (C.Model->peers().*Fn)(QString::fromStdString(P));
                return json{{V == "decline" ? "declined" : V == "block" ? "blocked" : "removed", P}};
            }, {} });
    }

    Command::Register({
        "peers share", "share one of your libraries with a peer (or stop: --off)",
        "Tick (or untick) a peer's \"Share: <library>\" box: they get the library's packages — and the library packages "
        "those need — as a snapshot they can receive.",
        { Command::Pos("peer", "their nickname, peer code, or the start of it"), Command::Pos("library", "one of your libraries (VidyaGod, VidyaGodLibraries, VidyaGodRunners, …)"),
          Command::Flag("off", "stop sharing it instead") },
        { "peers share LGZ-MacBook VidyaGod", "peers share LGZ-MacBook VidyaGodRunners --off" },
        { "PeerSharing::shareLibraryWithPeer", "PeerSharing::stopSharingWithPeer", "PeerSharing::pushSharesTo", "PeerSharing::sharedWith" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const QString P = QString::fromStdString(Peer(Command::Str(A, "peer")));
            const QString L = QString::fromStdString(Command::Str(A, "library"));
            if (!C.Model->peers().myLibraryNames().contains(L))
                throw Command::BadArgs("you have no library '" + L.toStdString() + "' (" + C.Model->peers().myLibraryNames().join(", ").toStdString() + ")");
            if (Command::Bool(A, "off", false)) C.Model->peers().stopSharingWithPeer(P, L); else C.Model->peers().shareLibraryWithPeer(P, L);
            json Shared = C.Model->peers().sharedWith(P.toStdString());
            json Out = json::object();
            for (const auto &[Lib, Items] : Shared.items()) Out[Lib] = Items.size();
            return json{{"peer", P.toStdString()}, {"library", L.toStdString()}, {"shared", C.Model->peers().isSharingWithPeer(P, L)},
                        {"packages_shared", Out}};
        }, {} });

    for (const auto &[Verb, What, Set, Get, SetName, GetName] :
         std::vector<std::tuple<const char *, const char *, void (PeerSharing::*)(const QString &, bool), bool (PeerSharing::*)(const QString &) const, const char *, const char *>>{
             { "receive", "receive a peer's shared libraries (or stop: --off)", &PeerSharing::setReceivingFrom, &PeerSharing::isReceivingFrom, "setReceivingFrom", "isReceivingFrom" },
             { "presence", "show a peer you are online (or hide it: --off)", &PeerSharing::setPresenceSharedWith, &PeerSharing::isPresenceSharedWith, "setPresenceSharedWith", "isPresenceSharedWith" },
             { "vlan", "put a peer in your virtual LAN (or take them out: --off)", &PeerSharing::setInVlan, &PeerSharing::isInVlan, "setInVlan", "isInVlan" } })
    {
        const std::string V = Verb;
        Command::Register({
            "peers " + V, What,
            std::string("The peer's \"") + (V == "receive" ? "Receive" : V == "presence" ? "Presence" : "vLAN") + "\" box: "
            + What + (V == "receive" ? ". Nothing they send is kept before this is on; turning it on asks them for "
                                       "what they share, turning it off drops what was received (installed games stay)." : "."),
            { Command::Pos("peer", "their nickname, peer code, or the start of it"), Command::Flag("off", "turn it off instead") },
            { "peers " + V + " LGZ-MacBook", "peers " + V + " LGZ-MacBook --off" },
            { std::string("PeerSharing::") + SetName, std::string("PeerSharing::") + GetName }, Needs::Model, true,
            [Set = Set, Get = Get, V](Command::Context &C, const json &A) {
                const QString P = QString::fromStdString(Peer(Command::Str(A, "peer")));
                (C.Model->peers().*Set)(P, !Command::Bool(A, "off", false));
                return json{{"peer", P.toStdString()}, {V, (C.Model->peers().*Get)(P)}};
            }, {} });
    }

    Command::Register({
        "peers auto-accept", "accept every peer request by itself (server mode), or not",
        "The Peers page's Advanced \"Accept every request by itself (server mode)\": with it on, a request is accepted the "
        "moment it arrives, with the defaults (`peers defaults`).", { Command::Pos("state", "on or off", Type::String, {"on", "off"}) },
        { "peers auto-accept on", "peers auto-accept off" },
        { "PeerSharing::setAutoAcceptEnabled", "PeerSharing::autoAcceptEnabled", "PeerSharing::acceptPeer",
          "PeerSharing::applyNewPeerDefaults" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            C.Model->peers().setAutoAcceptEnabled(Command::Str(A, "state") == "on");
            return json{{"auto_accept", C.Model->peers().autoAcceptEnabled()}};
        }, {} });

    Command::Register({
        "peers defaults", "what a newly accepted peer gets (or change one)",
        "The table's \"New peers\" row: what pairing's ticks start as, and what a peer accepted by itself (server mode) "
        "is given — the libraries it gets, whether you receive from it, show it your presence, put it in your vLAN. "
        "With <setting> and <state>, change one: receive, presence, vlan, or share:<library>.",
        { Command::Opt("set", "the setting to change: receive, presence, vlan or share:<library>"),
          Command::Opt("state", "on or off (with --set)", Type::String, false, {"on", "off"}) },
        { "peers defaults", "peers defaults --set share:VidyaGod --state on", "peers defaults --set receive --state off" },
        { "PeerSharing::newPeerDefault", "PeerSharing::setNewPeerDefault", "PeerSharing::newPeerShareDefaults", "PeerSharing::setNewPeerShareDefault" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("set"))
            {
                const std::string K = Command::Str(A, "set");
                if (!A.contains("state")) throw Command::BadArgs("--set needs --state on|off");
                const bool On = Command::Str(A, "state") == "on";
                if (K.rfind("share:", 0) == 0) C.Model->peers().setNewPeerShareDefault(QString::fromStdString(K.substr(6)), On);
                else if (K == "receive" || K == "presence" || K == "vlan") C.Model->peers().setNewPeerDefault(QString::fromStdString(K), On);
                else throw Command::BadArgs("no default '" + K + "' (receive, presence, vlan, share:<library>)");
            }
            json Share = json::array();
            for (const QString &L : C.Model->peers().newPeerShareDefaults()) Share.push_back(L.toStdString());
            return json{{"share", Share}, {"receive", C.Model->peers().newPeerDefault("receive")},
                        {"presence", C.Model->peers().newPeerDefault("presence")}, {"vlan", C.Model->peers().newPeerDefault("vlan")}};
        }, {} });

    Command::Register({
        "peers received", "what a peer shares with you, as you hold it",
        "The snapshot of a peer's shared libraries this machine holds (only while you receive from them): per "
        "library, every package — its folder CID and title.", { Command::Pos("peer", "their nickname, peer code, or the start of it") },
        { "peers received LGZ-MacBook", "peers received LGZ-MacBook --json" },
        { "PeerSharing::hasPeerLibraries" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "peer"));
            const json &Fl = C.Config->value("PeerLibraries", json::object());
            if (!Fl.contains(P)) return json::array();
            json Out = json::array();
            for (const auto &[Lib, Items] : Fl[P].items())
                for (const auto &I : Items)
                    Out.push_back({{"library", Lib}, {"package", I.value("pkg", std::string())}, {"folder", I.value("cid", std::string())}});
            return Out;
        }, {} });

    Command::Register({
        "peers refresh", "ask a peer for what they share now",
        "Re-request a peer's snapshot (and land what it lists), as reconnecting does.", { Command::Pos("peer", "their nickname, peer code, or the start of it") },
        { "peers refresh LGZ-MacBook" }, { "PeerSharing::reconcileReceivedPeer", "PeerSharing::requestPeerLibraries", "IpfsWrapper::RequestPeerLibraries" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "peer"));
            C.Model->peers().reconcileReceivedPeer(QString::fromStdString(P));
            return json{{"asked", P}};
        }, {} });

    Command::Register({
        "peers ping", "check whether a peer is reachable now",
        "Probe the peer's reachability: online, offline, or not knowable (the node is down).",
        { Command::Pos("peer", "their nickname, peer code, or the start of it") }, { "peers ping LGZ-MacBook" },
        { "IpfsWrapper::PeerPing" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string P = Peer(Command::Str(A, "peer"));
            const int R = IpfsWrapper::PeerPing(P);
            return json{{"peer", P}, {"reachable", R == 1 ? json(true) : R == 0 ? json(false) : json("unknown (node down)")}};
        }, {} });

    Command::Register({
        "peers lan", "your virtual LAN: each peer's address and link",
        "Every peer's virtual-LAN address and the live state of the link to it (direct, relayed, connecting, down) "
        "with its round trip.", {}, { "peers lan" }, { "IpfsWrapper::LanPeers" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (const auto &L : IpfsWrapper::LanPeers())
                Out.push_back({{"peer", L.Nick}, {"address", L.Vip}, {"online", L.Online}, {"link", L.Link},
                               {"rtt_ms", L.RttMs < 0 ? json() : json(L.RttMs)}});
            return Out;
        }, {} });

    Command::Register({
        "peers stop-receiving", "stop receiving a peer's libraries (their stubs go, installs stay)", "",
        { Command::Pos("peer", "the peer's peer ID") }, { "peers stop-receiving 12D3KooW…" },
        { "PeerSharing::stopReceivingFromPeer", "PeerSharing::dropReceivedStubs", "PeerSharing::sharedStubFiles" }, Command::Needs::Model, false,
        [](Command::Context &C, const json &A) {
            C.Model->peers().stopReceivingFromPeer(QString::fromStdString(Command::Str(A, "peer")));
            return json{{"stopped", Command::Str(A, "peer")}};
        }, {} });
}
