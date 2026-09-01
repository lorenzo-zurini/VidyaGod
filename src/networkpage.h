#ifndef NETWORKPAGE_H
#define NETWORKPAGE_H

#include <QWidget>

class AppModel;
class QPushButton;
class QTreeWidget;
class QLabel;

// Settings → Network: the "is my firewall the problem?" panel. One button runs the node's full diagnostic
// sweep (HTTPS/DoH, peer+transport breakdown, inbound reachability, mDNS multicast, DHT, friend links) on a
// worker and renders one ✓/⚠/✗ row per subsystem. Every network feature here fails SOFT in normal use — a
// blocked UDP port just looks like "slow", a filtered 5353 like "can't see my friend on the couch" — so this
// page exists to turn those silences into sentences.
class NetworkPage : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkPage(AppModel &model, QWidget *parent = nullptr);

private slots:
    void runTest();

private:
    AppModel     &Model;
    QPushButton  *RunButton   = nullptr;
    QTreeWidget  *ResultTree  = nullptr;
    QLabel       *StatusLabel = nullptr;
};

#endif // NETWORKPAGE_H
