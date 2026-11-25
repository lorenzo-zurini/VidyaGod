// Controller mode's own Peers and Settings pages (DeckShell::peersRows / settingsRows): the rows a DeckList shows,
// read from the same models the desktop pages read, every one a toggle, a left/right value or one press. What only a
// desktop has (the tray, starting at login, identity backup…) is behind each page's Advanced….
#include "deckshell.h"
#include "decklist.h"
#include "deckshell_p.h"

#include "appmodel.h"
#include "guiformat.h"     // HumanBytesQ
#include "ipfsmodel.h"
#include "mainwindow.h"   // applyControllerSettings — a controller setting applies at once
#include "ipfswrapper.h"
#include "networktab.h"    // NetworkTab::Nearby — the devices nearby
#include "packagecatalog.h"
#include "peersharing.h"
#include "screensaver.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QStorageInfo>

#include <set>

namespace {
QString NameOf(const std::string &Nick, const QString &Peer) { return Nick.empty() ? Peer.left(12) + "…" : QString::fromStdString(Nick); }
const char *ReceiveSays = "Their shared games appear in your Catalog.";
const char *VlanSays = "Games you launch share a private LAN with them: LAN multiplayer across the internet.";
const char *PresenceSays = "They see whether you are online and what you play.";
} // namespace

QList<DeckRow> DeckShell::peersRows()
{
    QList<DeckRow> R;
    PeerSharing &F = Model.peers();
    const std::string Code = IpfsWrapper::PeerCode();
    const bool Online = Model.networkingEnabled() && IpfsWrapper::Available() && !Code.empty();
    const auto Note = [this](const QString &T) { PeersPage->setNote(T); };
    if (!Model.networkingEnabled())
    {
        R << DeckRow::Info("net", "Networking is off", "Nobody can reach you, and you reach nobody.");
        DeckRow On; On.Id = "net.on"; On.Title = "Turn networking on";
        On.Press = [this] { Model.setNetworkingEnabled(true); };
        R << On;
    }
    else if (!Online) R << DeckRow::Info("net", "Networking is starting…");

    std::vector<IpfsWrapper::Contact> Book = IpfsWrapper::PeerList();
    std::set<QString> Known;
    for (const auto &C : Book) Known.insert(QString::fromStdString(C.PeerID));

    // Waiting on you: each request, accepted with what new peers get, or declined.
    bool Waiting = false;
    for (const auto &C : Book)
    {
        if (C.State != "incoming") continue;
        if (!Waiting) { R << DeckRow::Header("Waiting on you"); Waiting = true; }
        const QString P = QString::fromStdString(C.PeerID), N = NameOf(C.Nick, P);
        R << DeckRow::Info("waiting:" + P, N + " wants to pair", "Short code " + PeerSharing::ShortCode(P) + " — check their screen shows the same.");
        DeckRow A; A.Id = "accept:" + P; A.Title = "Accept " + N; A.Says = "They get what new peers get; change it under Peers below.";
        A.Press = [this, P, N, Note] { Note(Model.peers().acceptPairing(P, Model.peers().defaultPermissions()) ? "Paired with " + N + "."
                                                                                                                : "Could not accept " + N + ": networking is off or still starting."); };
        DeckRow D; D.Id = "decline:" + P; D.Title = "Decline " + N;
        D.Press = [this, P] { Model.peers().declinePeer(P); };
        R << A << D;
    }

    // On your network: one press pairs.
    R << DeckRow::Header("On your network");
    QStringList NearIds;
    bool AnyNear = false;
    for (const auto &Nb : NetworkTab::Nearby())
    {
        const QString P = QString::fromStdString(Nb.Peer);
        NearIds << P;
        if (!Nb.State.empty() || Known.count(P)) continue;
        AnyNear = true;
        const QString N = NameOf(Nb.Nick, P);
        DeckRow Pr; Pr.Id = "pair:" + P; Pr.Title = "Pair with " + N;
        Pr.Says = "Short code " + PeerSharing::ShortCode(P) + ". They accept it on their screen.";
        Pr.Enabled = Online;
        Pr.Press = [this, P, N, Note] {
            std::string Err;
            Note(Model.peers().pairWith(P, Model.peers().defaultPermissions(), &Err) ? "Asked " + N + " to pair: accept it on " + N + "."
                                                                                       : "Could not ask " + N + " to pair: " + QString::fromStdString(Err));
        };
        R << Pr;
    }
    if (!AnyNear) R << DeckRow::Info("nearby.none", "No other VidyaGod on this network", "Start VidyaGod on another device here and it shows up.");
    {
        DeckRow S; S.Id = "shortcode"; S.Title = "Enter a short code…";
        S.Says = "Pair with the device on your network whose screen shows that code.";
        S.Enabled = Online;
        S.Press = [this, NearIds, Note] {
            bool Ok = false;
            const QString T = QInputDialog::getText(this, "Pair by short code", "The short code on the other device's Peers page:",
                                                    QLineEdit::Normal, QString(), &Ok);
            if (!Ok || T.trimmed().isEmpty()) return;
            const QString P = PeerSharing::PeerForShortCode(T, NearIds);
            if (P.isEmpty()) { Note("No device on this network shows " + T.trimmed().toUpper() + "."); return; }
            std::string Err;
            Note(Model.peers().pairWith(P, Model.peers().defaultPermissions(), &Err) ? "Asked them to pair: accept it on their screen."
                                                                                     : "Could not ask them: " + QString::fromStdString(Err));
        };
        R << S;
    }

    // Peers: each with its switches, and Block / Remove (each asked twice).
    const QStringList Libs = F.myLibraryNames();
    for (const auto &C : Book)
    {
        if (C.State == "incoming") continue;
        const QString P = QString::fromStdString(C.PeerID), N = NameOf(C.Nick, P);
        const bool Accepted = C.State == "accepted", Can = Accepted || C.State == "pending";
        R << DeckRow::Header(N);
        QString Where = Accepted ? (C.Online ? "Paired · online now" : "Paired · offline")
                      : C.State == "pending" ? "Asked: waiting for them to accept" : "Blocked";
        if (F.isReceivingFrom(P))
        {
            const PeerSharing::Received Got = F.receivedFrom(P);
            Where += QString(" · %1 of the %2 games they share are here").arg(Got.Here).arg(Got.Packages);
        }
        R << DeckRow::Info("peer:" + P, Where, "Short code " + PeerSharing::ShortCode(P));
        const auto Toggle = [&](const QString &Id, const QString &Title, const QString &Says, bool On, std::function<void(bool)> Set) {
            DeckRow T; T.K = DeckRow::Kind::Toggle; T.Id = Id; T.Title = Title; T.Says = Says; T.On = On; T.Enabled = Can; T.Toggle = std::move(Set);
            R << T;
        };
        for (const QString &L : Libs)
            Toggle("share:" + L + ":" + P, "Share “" + L + "”", "They see your library “" + L + "” in their Catalog.", F.isSharingWithPeer(P, L),
                   [this, P, L](bool On) { if (On) Model.peers().shareLibraryWithPeer(P, L); else Model.peers().stopSharingWithPeer(P, L); });
        Toggle("receive:" + P, "Receive their games", ReceiveSays, F.isReceivingFrom(P), [this, P](bool On) { Model.peers().setReceivingFrom(P, On); });
        Toggle("vlan:" + P, "Virtual LAN", VlanSays, F.isInVlan(P), [this, P](bool On) { Model.peers().setInVlan(P, On); });
        Toggle("presence:" + P, "Presence", PresenceSays, F.isPresenceSharedWith(P), [this, P](bool On) { Model.peers().setPresenceSharedWith(P, On); });
        DeckRow B; B.Id = "block:" + P; B.Title = "Block " + N; B.Confirm = true; B.Enabled = C.State != "blocked";
        B.Says = "They can no longer reach you or ask again; their games leave your Catalog.";
        B.Press = [this, P] { Model.peers().blockPeer(P); };
        DeckRow Rm; Rm.Id = "remove:" + P; Rm.Title = "Remove " + N; Rm.Confirm = true;
        Rm.Says = "You stop sharing with each other. Games you installed from them stay.";
        Rm.Press = [this, P] { Model.peers().removePeer(P); };
        R << B << Rm;
    }

    // From afar: the code, to scan with a phone and send; its short code, checked on both screens.
    R << DeckRow::Header("Pair from afar");
    if (Online)
    {
        DeckRow Q; Q.K = DeckRow::Kind::Qr; Q.Id = "code"; Q.Code = QString::fromStdString(Code);
        Q.Title = PeerSharing::ShortCode(Q.Code);
        Q.Says = "Your code. Scan it with a phone to send it to someone: they paste it into their Peers page. The short code "
                 "above is what both screens show once you are paired.";
        R << Q;
    }
    else R << DeckRow::Info("code", "Your code shows here once networking is on");
    R << DeckRow::Header("More");
    DeckRow Adv; Adv.Id = "advanced"; Adv.Title = "Advanced…";
    Adv.Says = "The full Peers page: your name, defaults for new peers, auto-accept, identity backup.";
    Adv.Press = [this] { openAdvanced(Advanced::Peers); };
    R << Adv;
    return R;
}

QList<DeckRow> DeckShell::settingsRows()
{
    QList<DeckRow> R;
    nlohmann::ordered_json &Cfg = *Model.config();
    const auto Apply = [this] {
        Model.save();
        if (auto *W = qobject_cast<MainWindow *>(window())) W->applyControllerSettings();
    };

    R << DeckRow::Header("Controller");
    {
        DeckRow S; S.K = DeckRow::Kind::Choice; S.Id = "controller.start"; S.Title = "Start in";
        S.Values = { "Automatic", "Controller mode", "Desktop" };
        S.Says = "Automatic: controller mode inside Steam's Game Mode, the desktop window elsewhere.";
        const ControllerMode::Start Now = ControllerMode::StartSetting(Cfg);
        S.Value = Now == ControllerMode::Start::Auto ? 0 : Now == ControllerMode::Start::Controller ? 1 : 2;
        S.Choose = [this, &Cfg](int V) {
            ControllerMode::SetStart(Cfg, V == 0 ? ControllerMode::Start::Auto : V == 1 ? ControllerMode::Start::Controller : ControllerMode::Start::Desktop);
            Model.save();
        };
        R << S;
        DeckRow H; H.K = DeckRow::Kind::Toggle; H.Id = "controller.haptics"; H.Title = "Rumble as the focus moves";
        H.On = ControllerMode::Haptics(Cfg);
        H.Toggle = [&Cfg, Apply](bool On) { ControllerMode::SetHaptics(Cfg, On); Apply(); };
        R << H;
        DeckRow D; D.K = DeckRow::Kind::Toggle; D.Id = "controller.desktop"; D.Title = "A controller drives the desktop window too";
        D.Says = "There, Start with Back held comes back to controller mode.";
        D.On = ControllerMode::DesktopNavigation(Cfg);
        D.Toggle = [&Cfg, Apply](bool On) { ControllerMode::SetDesktopNavigation(Cfg, On); Apply(); };
        R << D;
    }

    R << DeckRow::Header("Screen");
    {
        static const int Seconds[] = { 0, 30, 60, 120, 300, 600, 1800 };
        DeckRow S; S.K = DeckRow::Kind::Choice; S.Id = "screen.saver"; S.Title = "Screen saver after";
        S.Values = { "Never", "30 seconds", "1 minute", "2 minutes", "5 minutes", "10 minutes", "30 minutes" };
        S.Says = "The digital rain after this long with no input, so nothing burns into an OLED screen.";
        const int Now = ScreenSaver::SecondsIn(Cfg);
        S.Value = 0;
        for (int I = 0; I < 7; ++I) if (std::abs(Seconds[I] - Now) < std::abs(Seconds[S.Value] - Now)) S.Value = I;
        S.Choose = [this, &Cfg](int V) { Cfg["Settings"]["ScreenSaverSeconds"] = Seconds[V]; Model.save(); };
        R << S;
    }

    R << DeckRow::Header("Networking");
    {
        DeckRow N; N.K = DeckRow::Kind::Toggle; N.Id = "net"; N.Title = "Networking";
        N.On = Model.networkingEnabled();
        const IpfsModel::NodeStatus &St = Ipfs.nodeStatus();
        N.Says = !N.On ? "Off: nothing downloads or seeds, and peers cannot reach you. Installed games still play."
               : !IpfsWrapper::Available() ? "Starting…"
               : QString("Online · %1 peer%2 · ↓ %3/s · ↑ %4/s").arg(St.peers).arg(St.peers == 1 ? "" : "s")
                     .arg(HumanBytesQ(static_cast<qint64>(St.downBps)), HumanBytesQ(static_cast<qint64>(St.upBps)));
        N.Toggle = [this](bool On) { Model.setNetworkingEnabled(On); };
        R << N;
        DeckRow T; T.Id = "advanced.node"; T.Title = "Transfers and the node…";
        T.Says = "Every transfer and what is seeded; recheck, unpin, seed a folder.";
        T.Press = [this] { openAdvanced(Advanced::Node); };
        R << T;
    }

    R << DeckRow::Header("Storage");
    {
        const QString Root = QString::fromStdString(PackageCatalog::LibraryRootDir(Cfg));
        const QStorageInfo Disk(Root);
        QString Says = Library->statusText();
        if (Disk.isValid()) Says += (Says.isEmpty() ? "" : " · ") + HumanBytesQ(Disk.bytesAvailable()) + " free on its disk";
        R << DeckRow::Info("storage", "Library: " + Root, Says);
    }

    R << DeckRow::Header("Sources");
    {
        int N = 0;
        if (Cfg.contains("Settings") && Cfg["Settings"].contains("PackageSources") && Cfg["Settings"]["PackageSources"].is_array())
            for (const auto &S : Cfg["Settings"]["PackageSources"])
            {
                const std::string Dir = PackageCatalog::PackageSourceDir(Cfg, S);
                const QString Name = QString::fromStdString(S.is_object() ? S.value("NAME", std::string()) : std::string());
                const QString Cid = QString::fromStdString(PackageCatalog::PackageSourceCID(S));
                std::error_code Ec;
                const bool Here = !Dir.empty() && std::filesystem::is_directory(Dir, Ec);
                R << DeckRow::Info("source:" + Cid, Name.isEmpty() ? Cid.left(16) + "…" : Name,
                                   Here ? "Fetched: its packages are in the Catalog." : "Not fetched yet: it is fetched once networking is on.");
                ++N;
            }
        if (N == 0) R << DeckRow::Info("sources.none", "No package sources", "Add one under All settings… → Sources.");
        DeckRow S; S.Id = "sources.sync"; S.Title = "Check the sources now";
        S.Enabled = Model.networkingEnabled();
        S.Press = [this] { Model.syncSources(); SettingsPage->setNote("Checking the package sources…"); };
        R << S;
    }

    R << DeckRow::Header("Runners");
    {
        int Installed = 0, Offered = 0;
        const auto &Hyd = Model.hydration();
        for (const auto &[Id, N] : Model.catalogIndex().Nodes)
        {
            if (!N.IsRunner()) continue;
            const auto It = Hyd.find(N.Key());
            (It != Hyd.end() && It->second.Hydrated ? Installed : Offered) += 1;
        }
        R << DeckRow::Info("runners", QString("%1 runner%2 installed").arg(Installed).arg(Installed == 1 ? "" : "s"),
                           Offered ? QString("%1 more can be downloaded.").arg(Offered) : QString("Every runner the library knows is here."));
        DeckRow A; A.Id = "advanced.runners"; A.Title = "Runners…";
        A.Says = "Which runner a game uses, and installing or removing one.";
        A.Press = [this] { openAdvanced(Advanced::Settings, "Runners"); };
        R << A;
    }

    R << DeckRow::Header("More");
    DeckRow All; All.Id = "advanced"; All.Title = "All settings…";
    All.Says = "Every setting, as the desktop has them.";
    All.Press = [this] { openAdvanced(Advanced::Settings); };
    R << All;
    return R;
}
