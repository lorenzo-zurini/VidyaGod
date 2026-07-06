#ifndef FRIENDSTAB_H
#define FRIENDSTAB_H

#include <QWidget>

class AppModel;
class QLabel;
class QLineEdit;
class QTableWidget;
class QPushButton;

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
    void addFriendClicked();
    void setNickClicked();

    AppModel &     Model;
    QLabel *       CodeValue   = nullptr;
    QPushButton *  CopyButton  = nullptr;
    QLineEdit *    NickEdit    = nullptr;
    QLineEdit *    AddEdit     = nullptr;
    QLabel *       StatusHint  = nullptr;
    QTableWidget * Table       = nullptr;
    bool           NickEdited  = false;   // don't clobber the field the user is typing in on a refresh
};

#endif // FRIENDSTAB_H
