#ifndef DOWNLOADSPAGE_H
#define DOWNLOADSPAGE_H

#include <QWidget>

class AppModel;

// ---------------------------------------------------------------------------
// DownloadsPage — the Settings "Downloads" page: the max-simultaneous-downloads control. Reads/writes
// Settings.MaxConcurrentDownloads through the AppModel.
// ---------------------------------------------------------------------------
class DownloadsPage : public QWidget
{
    Q_OBJECT
public:
    DownloadsPage(AppModel &model, QWidget *parent = nullptr);

private:
    AppModel &Model;
};

#endif // DOWNLOADSPAGE_H
