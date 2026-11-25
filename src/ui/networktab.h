#ifndef NETWORKTAB_H
#define NETWORKTAB_H

#include <QWidget>

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "peersharing.h"   // PeerSharing::Permissions — what pairing's ticks hold

class AppModel;
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QGroupBox;
class QTableWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;
namespace IpfsWrapper { struct NearbyPeer; }

// NetworkTab — the Peers page: the desktop's Peers tab and controller mode's Peers page (hosted there). Every
// peer is the same: a row of the table with its permissions — Accepted, Share: <library> (one per library of mine),
// Receive, vLAN, Presence — under a "New peers" row of defaults, and its menu (Decline / Block / Remove). Pairing asks
// for those same permissions where it happens, as ticks that start as the defaults row:
//  - what you share, in one line, and the one press it may need (prepare a library for sharing, turn networking on);
//  - waiting on you: each request, with the ticks, accepted or declined;
//  - the table;
//  - add someone: the VidyaGod devices on your network, or a peer code, each with the ticks and one press;
//  - advanced, folded: name, auto-accept, publish everything, identity backup.
// Thin view over PeerSharing (the model of all of it) + IpfsWrapper::PeerList / NearbyPeers.
class NetworkTab : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkTab(AppModel & model, QWidget * parent = nullptr);

    // Where the nearby devices are read from (IpfsWrapper::NearbyPeers by default): a test sets a list.
    static void SetNearbySource(std::function<std::vector<IpfsWrapper::NearbyPeer>()> Source);

    int refreshes() const { return Refreshes; }   // test observability: full refreshes made

public slots:
    void setActive(bool on);

private:
    void buildUi();
    void refresh();
    void scheduleRefresh();          // coalesced, deferred (never rebuilds under a running handler); hidden: on show
    bool Showing = false;            // setActive: the page is the one showing
    bool Stale = false;              // something changed while it was hidden
    int  Refreshes = 0;
    void rebuildRows();              // the lists and the table, rebuilt only when what they show changed
    void rebuildTable();
    void addPeerClicked();
    void setNickClicked();
    void publishClicked();
    void backupIdentityClicked();
    void restoreIdentityClicked();
    void pairClicked(const QString & peer, const QString & name);
    void acceptClicked(const QString & peer, const QString & name);
    QWidget * permissionTicks(QWidget * row, const QString & key);   // the table's columns, as ticks, for one pairing
    PeerSharing::Permissions & ticks(const QString & key);         // a pairing's ticks (the defaults row at first)

    AppModel &     Model;
    QLabel *       ShareState    = nullptr;
    QPushButton *  PrepareButton = nullptr;   // a library's first publish, while one shared has none
    QPushButton *  TurnOnButton  = nullptr;   // while networking is off
    QPushButton *  SendButton    = nullptr;   // "Send request": only while a request can go
    QGroupBox *    WaitingBox    = nullptr;
    QVBoxLayout *  WaitingRows   = nullptr;
    QTableWidget * Table         = nullptr;
    QLabel *       EmptyHint     = nullptr;   // under the table while there is nobody
    QGroupBox *    AddBox        = nullptr;
    QVBoxLayout *  NearbyRows    = nullptr;
    QLabel *       NearbyHint    = nullptr;
    QLabel *       Status        = nullptr;   // what the last press did
    QLabel *       CodeValue     = nullptr;
    QPushButton *  CopyButton    = nullptr;
    QLineEdit *    AddEdit       = nullptr;
    QToolButton *  AdvancedToggle = nullptr;
    QWidget *      Advanced      = nullptr;
    QLineEdit *    NickEdit      = nullptr;
    QCheckBox *    AutoAccept    = nullptr;
    QPushButton *  PublishButton = nullptr;
    QLabel *       PublishStatus = nullptr;
    QTimer *       NearbyTimer   = nullptr;   // polls the nearby devices while the page shows
    QString        Shown = QStringLiteral("\x01");   // what the rows show now (never what an empty page shows: the first pass builds)
    QString        TableShown = QStringLiteral("\x01");
    bool           NickEdited    = false;
    bool           RefreshQueued = false;
    std::map<QString, PeerSharing::Permissions> Ticks;   // per pairing (a peer, or "code"), kept across rebuilds
};

#endif // NETWORKTAB_H
