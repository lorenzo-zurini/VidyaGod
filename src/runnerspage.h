#ifndef RUNNERSPAGE_H
#define RUNNERSPAGE_H

#include <QWidget>

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// RunnersPage — the Settings "Runners" page: one card per runner node in the catalog (global runners always shown;
// embedded runners only once installed), with an Import button for runners that ship a build over IPFS. Reads the
// catalog through the AppModel, imports via the model, and rebuilds itself on the model's catalogChanged signal.
// ---------------------------------------------------------------------------
class RunnersPage : public QWidget
{
    Q_OBJECT
public:
    RunnersPage(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuild();            // (re)build the runner cards (on catalogChanged)

private:
    AppModel &Model;
    QScrollArea *Scroll = nullptr;
};

#endif // RUNNERSPAGE_H
