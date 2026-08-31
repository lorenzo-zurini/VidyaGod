#ifndef FRIENDSTAB_H
#define FRIENDSTAB_H

#include <QWidget>

class AppModel;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;
class QVBoxLayout;

// ---------------------------------------------------------------------------
// FriendsTab — the "Friends" tab: the user-facing front of the DHT multiplayer social layer. Shows your shareable
// friend code (= your libp2p peer ID), lets you set your nickname, add a friend by pasting their code, and manage
// the address book (accept/decline incoming requests, see who's online, remove). It is a self-contained view over
// IpfsWrapper's Friends API + the FriendsManager signal hub (which marshals node events onto the GUI thread) — no
// state of its own beyond the rendered table.
// ---------------------------------------------------------------------------
class FriendsTab : public QWidget
{
    Q_OBJECT
public:
    explicit FriendsTab(AppModel & model, QWidget * parent = nullptr);

public slots:
    void setActive(bool on);   // tab shown → refresh (the friend code appears once networking is up)

private:
    void buildUi();
    void refresh();            // repaint the code + nickname + contacts table from the backend
    void refreshSuggestions(); // the "players you met" list
    void addFriendClicked();
    void setNickClicked();
    // Open the pre-launch window on the friend's game with their address filled in. Warn-only if we don't have it.
    void joinClicked(const QString & Peer);

    AppModel &     Model;
    QLabel *       CodeValue   = nullptr;
    QPushButton *  CopyButton  = nullptr;
    QLineEdit *    NickEdit    = nullptr;
    QLineEdit *    AddEdit     = nullptr;
    QLabel *       StatusHint  = nullptr;
    QTableWidget * Table       = nullptr;
    QCheckBox *    InvisibleBox = nullptr;   // hide what I'm playing from every friend
    QCheckBox *    OpenBox      = nullptr;   // advisory "open to join" badge; purely manual
    QGroupBox *    MetBox       = nullptr;   // "Players you met" — hidden when empty
    QVBoxLayout *  MetLayout    = nullptr;
    bool           NickEdited  = false;   // don't clobber the field the user is typing in on a refresh
};

#endif // FRIENDSTAB_H
