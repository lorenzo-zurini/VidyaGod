#ifndef NETWORKTAB_H
#define NETWORKTAB_H

#include <QWidget>

class AppModel;
class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;
class QCheckBox;

// NetworkTab — the unified "Network" tab (replaces the old Friends + Sharing tabs). Header: friend code + nickname,
// add-a-peer, a global auto-accept toggle, Verify&Publish, identity backup. Body: the per-peer matrix — rows are a
// "New peers" defaults template + one per contact; columns are Accepted, Share:<lib> (one per published library),
// Receive, vLAN, Presence (checkboxes), plus a per-row menu for Decline/Block/Remove. Thin view over AppModel +
// IpfsWrapper::FriendList + the FriendsManager signal hub.
class NetworkTab : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkTab(AppModel & model, QWidget * parent = nullptr);

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

    AppModel &     Model;
    QLabel *       CodeValue     = nullptr;
    QPushButton *  CopyButton    = nullptr;
    QLineEdit *    NickEdit      = nullptr;
    QLineEdit *    AddEdit       = nullptr;
    QCheckBox *    AutoAccept    = nullptr;
    QPushButton *  PublishButton = nullptr;
    QLabel *       PublishStatus = nullptr;
    QLabel *       StatusHint    = nullptr;
    QTableWidget * Table         = nullptr;
    bool           NickEdited    = false;
    bool           RefreshQueued = false;
};

#endif // NETWORKTAB_H
