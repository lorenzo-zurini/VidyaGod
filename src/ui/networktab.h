#ifndef NETWORKTAB_H
#define NETWORKTAB_H

#include <QWidget>

#include <functional>
#include <map>
#include <vector>

class AppModel;
class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;
class QCheckBox;
class QGroupBox;
class QTimer;
class QVBoxLayout;
namespace IpfsWrapper { struct NearbyPeer; }

// NetworkTab — the unified "Network" tab (replaces the old Friends + Sharing tabs). Header: friend code + nickname,
// add-a-peer, a global auto-accept toggle, Verify&Publish, identity backup. Body: the per-peer matrix — rows are a
// "New peers" defaults template + one per contact; columns are Accepted, Share:<lib> (one per published library),
// Receive, vLAN, Presence (checkboxes), plus a per-row menu for Decline/Block/Remove. Thin view over AppModel +
// IpfsWrapper::FriendList + the FriendsManager signal hub.
// Between them, "On your network": the VidyaGod devices nearby (IpfsWrapper::NearbyPeers) and the requests waiting on
// us, each one press from paired (Pair / Accept), with what to share ticked in the same row — no code to type.
class NetworkTab : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkTab(AppModel & model, QWidget * parent = nullptr);

    // Where "On your network" reads the nearby devices from (IpfsWrapper::NearbyPeers by default): a test sets a list.
    static void SetNearbySource(std::function<std::vector<IpfsWrapper::NearbyPeer>()> Source);

public slots:
    void setActive(bool on);

private:
    void buildUi();
    void refresh();
    void scheduleRefresh();          // coalesced, deferred (avoids re-entrancy from a cell toggle / open menu)
    void addPeerClicked();
    void setNickClicked();
    void publishClicked();
    void backupIdentityClicked();
    void restoreIdentityClicked();
    void refreshNearby();            // rebuilds the rows only when what they show changed (a rebuild takes a pad's focus)
    void pairClicked(const QString & peer, const QString & name);
    void acceptPairingClicked(const QString & peer, const QString & name);

    AppModel &     Model;
    QLabel *       CodeValue     = nullptr;
    QPushButton *  CopyButton    = nullptr;
    QLineEdit *    NickEdit      = nullptr;
    QLineEdit *    AddEdit       = nullptr;
    QCheckBox *    AutoAccept    = nullptr;
    QPushButton *  PublishButton = nullptr;
    QLabel *       PublishStatus = nullptr;
    QLabel *       StatusHint    = nullptr;
    QPushButton *  TurnOnButton  = nullptr;   // beside StatusHint while networking is off
    QLabel *       EmptyHint     = nullptr;   // under the table while there is no friend
    QTableWidget * Table         = nullptr;
    bool           NickEdited    = false;
    bool           RefreshQueued = false;
    QGroupBox *    NearbyBox     = nullptr;
    QVBoxLayout *  NearbyRows    = nullptr;
    QLabel *       NearbyHint    = nullptr;   // nobody nearby, nothing waiting
    QLabel *       NearbyStatus  = nullptr;   // what the last Pair / Accept did
    QTimer *       NearbyTimer   = nullptr;   // polls the nearby list while the page shows
    QString        NearbyShown;               // what the rows show now
    struct PairTicks { bool Share = false, Receive = false; };
    std::map<QString, PairTicks> Ticks;       // per peer: kept across rebuilds of the rows
};

#endif // NETWORKTAB_H
