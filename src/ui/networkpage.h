#ifndef NETWORKPAGE_H
#define NETWORKPAGE_H

#include <QWidget>

class AppModel;
class QPushButton;
class QTreeWidget;
class QLabel;

// Settings → Network: the "is my firewall the problem?" panel, in two halves.
//  LIVE SERVICE HEALTH (top): a passive introspection of every Go service (node, host, reachability, DHT,
//  bitswap, deliverability, announce, mDNS, friends, LAN links, overlay, tri-plane, DoH) polled every few
//  seconds while the page is visible — a service that silently died shows up here as ✗ within seconds.
//  NETWORK TEST (bottom): the on-demand ACTIVE sweep that probes the outside world (up to ~15s) — firewall/
//  NAT/multicast truths that passive introspection cannot know. Every network feature fails SOFT in normal
//  use — a blocked UDP port just looks like "slow" — so this page exists to turn those silences into sentences.
class QTimer;
class NetworkPage : public QWidget
{
    Q_OBJECT
public:
    explicit NetworkPage(AppModel &model, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;   // start/stop the health poll with visibility —
    void hideEvent(QHideEvent *e) override;   // no background polling while the page is hidden

private slots:
    void runTest();
    void refreshHealth();

private:
    AppModel     &Model;
    QTreeWidget  *HealthTree   = nullptr;
    QLabel       *HealthStatus = nullptr;
    QTimer       *HealthTimer  = nullptr;
    bool          HealthBusy   = false;   // one poll in flight at a time
    QPushButton  *RunButton   = nullptr;
    QTreeWidget  *ResultTree  = nullptr;
    QLabel       *StatusLabel = nullptr;
};

#endif // NETWORKPAGE_H
