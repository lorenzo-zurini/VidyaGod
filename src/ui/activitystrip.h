#ifndef ACTIVITYSTRIP_H
#define ACTIVITYSTRIP_H

#include <QWidget>

class AppModel;
class IpfsModel;
class DownloadManager;
class QFrame;
class QLabel;
class QTimer;
class QToolButton;

//The main window's activity strip (bottom right): a spinner while anything runs and "N tasks" — or the last result —
//with a failure count; a click opens the list (ActivityLog): each job's progress and Cancel while it runs, its result
//once done, kept until dismissed. Background work reports there instead of popping a box from a page the user is not on.
class ActivityStrip : public QWidget
{
    Q_OBJECT
public:
    explicit ActivityStrip(QWidget * Parent = nullptr);
    //What the background reports into the log: downloads, publish, seed folder, force recheck, source sync, a failed
    //uninstall. Once per app (the main window's composition root).
    static void Watch(AppModel & Model, IpfsModel & Ipfs, DownloadManager & Downloads);

    QString summary() const;                  // the strip's text
    QFrame * list() const { return Popup; }   // the list, while open
public slots:
    void openList();

private:
    void refresh();
    void fillList();
    QLabel * Spin = nullptr;
    QToolButton * Button = nullptr;
    QTimer * Anim = nullptr;
    QFrame * Popup = nullptr;
    int Frame = 0;
};

#endif // ACTIVITYSTRIP_H
