#ifndef IPFSSETTINGSPAGE_H
#define IPFSSETTINGSPAGE_H

#include <QWidget>

class AppModel;

// ---------------------------------------------------------------------------
// IpfsSettingsPage — the Settings "IPFS" page: the master "Enable networking" toggle (OFF by default — all P2P
// download/seed activity is opt-in; launching already-downloaded games works offline) and the max-simultaneous-
// downloads control. Writes Settings.IPFS.Enabled (via AppModel, which starts/stops the node + greys the Catalog/
// IPFS tabs) and Settings.MaxConcurrentDownloads.
// ---------------------------------------------------------------------------
class IpfsSettingsPage : public QWidget
{
    Q_OBJECT
public:
    IpfsSettingsPage(AppModel & model, QWidget * parent = nullptr);

private:
    AppModel & Model;
};

#endif // IPFSSETTINGSPAGE_H
