// `VidyaGod friends …` — the Network tab from the command line: your friend code and nickname, the address book,
// requests, and per friend what you share, what you receive, presence and the virtual LAN. Every command calls the
// AppModel / IpfsWrapper function the tab calls.
#include "command.h"
#include "appmodel.h"
#include "ipfswrapper.h"

#include <algorithm>

namespace
{

using json = nlohmann::ordered_json;

// A friend named on the command line: a full peer id, a unique start of one, or a nickname (any case).
std::string Peer(const std::string &Q)
{
    const auto All = IpfsWrapper::FriendList();
    for (const auto &C : All) if (C.PeerID == Q) return Q;
    std::vector<const IpfsWrapper::Contact *> Hits;
    for (const auto &C : All)
        if (C.PeerID.rfind(Q, 0) == 0 || QString::fromStdString(C.Nick).compare(QString::fromStdString(Q), Qt::CaseInsensitive) == 0)
            Hits.push_back(&C);
    if (Hits.size() == 1) return Hits.front()->PeerID;
    if (Hits.empty())
    {
        if (Q.rfind("12D3KooW", 0) == 0 && Q.size() > 40) return Q;   // a full code not (yet) in the book
        throw Command::BadArgs("no friend '" + Q + "' (VidyaGod friends list shows them)");
    }
    std::string Names;
    for (const auto *C : Hits) Names += (Names.empty() ? "" : ", ") + (C->Nick.empty() ? C->PeerID : C->Nick + " (" + C->PeerID.substr(0, 16) + "…)");
    throw Command::BadArgs("'" + Q + "' matches " + std::to_string(Hits.size()) + " friends: " + Names);
}

json ContactJson(const IpfsWrapper::Contact &C, AppModel *M)
{
    json J = {{"friend", C.Nick.empty() ? C.PeerID.substr(0, 16) + "…" : C.Nick}, {"state", C.State}, {"online", C.Online},
              {"peer", C.PeerID}};
    if (M)
    {
        const QString P = QString::fromStdString(C.PeerID);
        json Shared = json::array();
        for (const QString &L : M->myLibraryNames()) if (M->isSharingWithFriend(P, L)) Shared.push_back(L.toStdString());
        J["sharing"] = Shared;
        J["receiving"] = M->isReceivingFrom(P);
        J["presence"] = M->isPresenceSharedWith(P);
        J["vlan"] = M->isInVlan(P);
    }
    if (C.LastSeen > 0) J["last_seen_ms"] = C.LastSeen;
    return J;
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
    Command::RegisterGroup("friends", "your friend code, friends, requests, and what you share and receive");

    Command::Register({
        "friends code", "your friend code and nickname",
        "The code a friend pastes to add you (your node's peer id), and the nickname they see.", {},
        { "friends code" }, { "IpfsWrapper::FriendCode", "IpfsWrapper::GetProfile" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            const std::string Code = IpfsWrapper::FriendCode();
            if (Code.empty()) throw std::runtime_error("the node is not up — no friend code yet");
            return json{{"code", Code}, {"nickname", IpfsWrapper::GetProfile().Nick}};
        }, {} });

    Command::Register({
        "friends nick", "set the nickname friends see",
        "What the Network tab's Nickname field + Save does.", { Command::Pos("name", "the nickname") },
        { "friends nick LGZ-PC" }, { "AppModel::setNickname", "IpfsWrapper::SetProfile" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            if (!C.Model->setNickname(QString::fromStdString(Command::Str(A, "name")), &Err))
                throw std::runtime_error(Fail("could not set the nickname", Err));
            return json{{"nickname", IpfsWrapper::GetProfile().Nick}};
        }, {} });

    Command::Register({
        "friends list", "every friend and request, with what you share and receive",
        "The Network tab's table: each contact's state (accepted, incoming, pending, blocked), whether it is online, the "
        "libraries you share with it, and whether you receive from it, show it your presence, and have it in your "
        "virtual LAN.", { Command::Opt("state", "only contacts in this state", Type::String, false, {"accepted", "incoming", "pending", "blocked"}) },
        { "friends list", "friends list --state incoming", "friends list --json" },
        { "IpfsWrapper::FriendList", "AppModel::myLibraryNames", "AppModel::isSharingWithFriend", "AppModel::isReceivingFrom",
          "AppModel::isPresenceSharedWith", "AppModel::isInVlan" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string Only = Command::Str(A, "state", "");
            auto All = IpfsWrapper::FriendList();
            std::sort(All.begin(), All.end(), [](const auto &X, const auto &Y) { return X.Nick < Y.Nick; });
            json Out = json::array();
            for (const auto &K : All) if (Only.empty() || K.State == Only) Out.push_back(ContactJson(K, C.Model));
            return Out;
        }, {} });

    Command::Register({
        "friends add", "send a friend request",
        "Ask a peer to be friends, by their friend code — the Network tab's \"Send request\".",
        { Command::Pos("code", "their friend code (12D3KooW…)") },
        { "friends add 12D3KooWQs9BMz6DtSWeFM6x98vjspCTVFSwTKwGmGDwjjgjXtuA" },
        { "AppModel::sendFriendRequest", "IpfsWrapper::FriendAdd" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            std::string Err;
            const std::string Code = Command::Str(A, "code");
            if (!C.Model->sendFriendRequest(QString::fromStdString(Code), &Err)) throw std::runtime_error(Fail("could not send the request", Err));
            return json{{"requested", Code}};
        }, {} });

    Command::Register({
        "friends accept", "accept a friend request",
        "Accept an incoming request (ticking its Accepted box): the friend is added, and the New-peer defaults applied "
        "to them (what you share with them, receive, presence, vLAN).",
        { Command::Pos("friend", "their nickname, friend code, or the start of it") },
        { "friends accept LGZ-MacBook" }, { "AppModel::acceptPeer", "AppModel::applyNewPeerDefaults" }, Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "friend"));
            C.Model->acceptPeer(QString::fromStdString(P));
            for (const auto &K : IpfsWrapper::FriendList()) if (K.PeerID == P) return ContactJson(K, C.Model);
            return json{{"peer", P}};
        }, {} });

    for (const auto &[Verb, Summary, Fn] : std::vector<std::tuple<const char *, const char *, void (AppModel::*)(const QString &)>>{
             { "decline", "decline a friend request", &AppModel::declineFriend },
             { "block", "block a peer (drops all their traffic)", &AppModel::blockFriend },
             { "remove", "remove a friend (both directions of sharing end)", &AppModel::removeFriend } })
    {
        const std::string V = Verb;
        Command::Register({
            "friends " + V, Summary,
            std::string(Summary) + ". You stop sharing with each other, and their shared games leave your catalog; games you "
            "installed from them are kept. The Network tab's ⋮ menu.",
            { Command::Pos("friend", "their nickname, friend code, or the start of it") },
            { "friends " + V + " LGZ-MacBook" },
            { std::string("AppModel::") + (V == "decline" ? "declineFriend" : V == "block" ? "blockFriend" : "removeFriend"),
              "AppModel::forgetFriend", std::string("IpfsWrapper::Friend") + (V == "decline" ? "Decline" : V == "block" ? "Block" : "Remove") },
            Needs::Model, true,
            [Fn = Fn, V](Command::Context &C, const json &A) {
                const std::string P = Peer(Command::Str(A, "friend"));
                (C.Model->*Fn)(QString::fromStdString(P));
                return json{{V == "decline" ? "declined" : V == "block" ? "blocked" : "removed", P}};
            }, {} });
    }

    Command::Register({
        "friends share", "share one of your libraries with a friend (or stop: --off)",
        "Tick (or untick) a friend's \"Share: <library>\" box: they get the library's packages — and the library packages "
        "those need — as a snapshot they can receive.",
        { Command::Pos("friend", "their nickname, friend code, or the start of it"), Command::Pos("library", "one of your libraries (VidyaGod, VidyaGodLibraries, VidyaGodRunners, …)"),
          Command::Flag("off", "stop sharing it instead") },
        { "friends share LGZ-MacBook VidyaGod", "friends share LGZ-MacBook VidyaGodRunners --off" },
        { "AppModel::shareLibraryWithFriend", "AppModel::stopSharingWithFriend", "AppModel::pushSharesTo", "AppModel::sharedWith" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const QString P = QString::fromStdString(Peer(Command::Str(A, "friend")));
            const QString L = QString::fromStdString(Command::Str(A, "library"));
            if (!C.Model->myLibraryNames().contains(L))
                throw Command::BadArgs("you have no library '" + L.toStdString() + "' (" + C.Model->myLibraryNames().join(", ").toStdString() + ")");
            if (Command::Bool(A, "off", false)) C.Model->stopSharingWithFriend(P, L); else C.Model->shareLibraryWithFriend(P, L);
            json Shared = C.Model->sharedWith(P.toStdString());
            json Out = json::object();
            for (const auto &[Lib, Items] : Shared.items()) Out[Lib] = Items.size();
            return json{{"friend", P.toStdString()}, {"library", L.toStdString()}, {"shared", C.Model->isSharingWithFriend(P, L)},
                        {"packages_shared", Out}};
        }, {} });

    for (const auto &[Verb, What, Set, Get, SetName, GetName] :
         std::vector<std::tuple<const char *, const char *, void (AppModel::*)(const QString &, bool), bool (AppModel::*)(const QString &) const, const char *, const char *>>{
             { "receive", "receive a friend's shared libraries (or stop: --off)", &AppModel::setReceivingFrom, &AppModel::isReceivingFrom, "setReceivingFrom", "isReceivingFrom" },
             { "presence", "show a friend you are online (or hide it: --off)", &AppModel::setPresenceSharedWith, &AppModel::isPresenceSharedWith, "setPresenceSharedWith", "isPresenceSharedWith" },
             { "vlan", "put a friend in your virtual LAN (or take them out: --off)", &AppModel::setInVlan, &AppModel::isInVlan, "setInVlan", "isInVlan" } })
    {
        const std::string V = Verb;
        Command::Register({
            "friends " + V, What,
            std::string("The friend's \"") + (V == "receive" ? "Receive" : V == "presence" ? "Presence" : "vLAN") + "\" box: "
            + What + (V == "receive" ? ". Nothing they send is kept before this is on; turning it on asks them for "
                                       "what they share, turning it off drops what was received (installed games stay)." : "."),
            { Command::Pos("friend", "their nickname, friend code, or the start of it"), Command::Flag("off", "turn it off instead") },
            { "friends " + V + " LGZ-MacBook", "friends " + V + " LGZ-MacBook --off" },
            { std::string("AppModel::") + SetName, std::string("AppModel::") + GetName }, Needs::Model, true,
            [Set = Set, Get = Get, V](Command::Context &C, const json &A) {
                const QString P = QString::fromStdString(Peer(Command::Str(A, "friend")));
                (C.Model->*Set)(P, !Command::Bool(A, "off", false));
                return json{{"friend", P.toStdString()}, {V, (C.Model->*Get)(P)}};
            }, {} });
    }

    Command::Register({
        "friends auto-accept", "accept every friend request by itself (server mode), or not",
        "The Network tab's \"Auto-accept peer requests (server mode)\": with it on, a request is accepted the moment it "
        "arrives, with the New-peer defaults.", { Command::Pos("state", "on or off", Type::String, {"on", "off"}) },
        { "friends auto-accept on", "friends auto-accept off" },
        { "AppModel::setAutoAcceptEnabled", "AppModel::autoAcceptEnabled" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            C.Model->setAutoAcceptEnabled(Command::Str(A, "state") == "on");
            return json{{"auto_accept", C.Model->autoAcceptEnabled()}};
        }, {} });

    Command::Register({
        "friends defaults", "what a newly accepted friend gets (or change one)",
        "The \"New peers (defaults)\" row: the libraries a new friend is shared, and whether you receive from them, show "
        "them your presence, put them in your vLAN. With <setting> and <state>, change one: a setting is receive, "
        "presence, vlan, or share:<library>.",
        { Command::Opt("set", "the setting to change: receive, presence, vlan or share:<library>"),
          Command::Opt("state", "on or off (with --set)", Type::String, false, {"on", "off"}) },
        { "friends defaults", "friends defaults --set share:VidyaGod --state on", "friends defaults --set receive --state off" },
        { "AppModel::newPeerDefault", "AppModel::setNewPeerDefault", "AppModel::newPeerShareDefaults", "AppModel::setNewPeerShareDefault" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            if (A.contains("set"))
            {
                const std::string K = Command::Str(A, "set");
                if (!A.contains("state")) throw Command::BadArgs("--set needs --state on|off");
                const bool On = Command::Str(A, "state") == "on";
                if (K.rfind("share:", 0) == 0) C.Model->setNewPeerShareDefault(QString::fromStdString(K.substr(6)), On);
                else if (K == "receive" || K == "presence" || K == "vlan") C.Model->setNewPeerDefault(QString::fromStdString(K), On);
                else throw Command::BadArgs("no default '" + K + "' (receive, presence, vlan, share:<library>)");
            }
            json Share = json::array();
            for (const QString &L : C.Model->newPeerShareDefaults()) Share.push_back(L.toStdString());
            return json{{"share", Share}, {"receive", C.Model->newPeerDefault("receive")},
                        {"presence", C.Model->newPeerDefault("presence")}, {"vlan", C.Model->newPeerDefault("vlan")}};
        }, {} });

    Command::Register({
        "friends received", "what a friend shares with you, as you hold it",
        "The snapshot of a friend's shared libraries this machine holds (only while you receive from them): per "
        "library, every package — its folder CID and title.", { Command::Pos("friend", "their nickname, friend code, or the start of it") },
        { "friends received LGZ-PC", "friends received LGZ-PC --json" },
        { "AppModel::hasFriendLibraries" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "friend"));
            const json &Fl = C.Config->value("FriendLibraries", json::object());
            if (!Fl.contains(P)) return json::array();
            json Out = json::array();
            for (const auto &[Lib, Items] : Fl[P].items())
                for (const auto &I : Items)
                    Out.push_back({{"library", Lib}, {"package", I.value("pkg", std::string())}, {"folder", I.value("cid", std::string())}});
            return Out;
        }, {} });

    Command::Register({
        "friends refresh", "ask a friend for what they share now",
        "Re-request a friend's snapshot (and land what it lists), as reconnecting does.", { Command::Pos("friend", "their nickname, friend code, or the start of it") },
        { "friends refresh LGZ-PC" }, { "AppModel::reconcileReceivedFriend", "AppModel::requestFriendLibraries", "IpfsWrapper::RequestFriendLibraries" },
        Needs::Model, true,
        [](Command::Context &C, const json &A) {
            const std::string P = Peer(Command::Str(A, "friend"));
            C.Model->reconcileReceivedFriend(QString::fromStdString(P));
            return json{{"asked", P}};
        }, {} });

    Command::Register({
        "friends ping", "check whether a friend is reachable now",
        "Probe the friend's reachability: online, offline, or not knowable (the node is down).",
        { Command::Pos("friend", "their nickname, friend code, or the start of it") }, { "friends ping LGZ-MacBook" },
        { "IpfsWrapper::FriendPing" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            const std::string P = Peer(Command::Str(A, "friend"));
            const int R = IpfsWrapper::FriendPing(P);
            return json{{"friend", P}, {"reachable", R == 1 ? json(true) : R == 0 ? json(false) : json("unknown (node down)")}};
        }, {} });

    Command::Register({
        "friends lan", "your virtual LAN: each friend's address and link",
        "Every friend's virtual-LAN address and the live state of the link to it (direct, relayed, connecting, down) "
        "with its round trip.", {}, { "friends lan" }, { "IpfsWrapper::LanPeers" }, Needs::Config, true,
        [](Command::Context &, const json &) {
            json Out = json::array();
            for (const auto &L : IpfsWrapper::LanPeers())
                Out.push_back({{"friend", L.Nick}, {"address", L.Vip}, {"online", L.Online}, {"link", L.Link},
                               {"rtt_ms", L.RttMs < 0 ? json() : json(L.RttMs)}});
            return Out;
        }, {} });
}
