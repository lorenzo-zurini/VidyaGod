#ifndef SOURCESPAGE_H
#define SOURCESPAGE_H

#include <QWidget>

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// SourcesPage — the Settings "Sources" page: add/remove/sync the IPFS package sources (folder CIDs of dehydrated
// packages) that make up the shared catalog. Reads the source list from the AppModel's config, performs
// add/remove/sync through the model (which does the IPFS fetch off-thread), and rebuilds itself on the model's
// packageSourcesChanged signal. Replaces the old git RepositoriesPage — sharing is now content-addressed only.
// ---------------------------------------------------------------------------
class SourcesPage : public QWidget
{
    Q_OBJECT
public:
    SourcesPage(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuild();            // (re)build the source cards + add row (on packageSourcesChanged)

private:
    AppModel &Model;
    QScrollArea *Scroll = nullptr;
};

#endif // SOURCESPAGE_H
