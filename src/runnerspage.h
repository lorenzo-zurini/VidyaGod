#ifndef RUNNERSPAGE_H
#define RUNNERSPAGE_H

#include <QWidget>

class AppModel;
class QLineEdit;
class QTreeWidget;

// ---------------------------------------------------------------------------
// RunnersPage — the Settings "Runners" page: every runner node of the catalog (global runners always; embedded ones
// once installed) as one row of a table grouped by family (all of ares's builds under "ares"): its name, what it
// runs on what, whether it is ready, how many games can use it, and Download for one whose build is not here yet. A
// search box filters it. Reads the catalog through the AppModel, imports via the model, rebuilds on catalogChanged.
// ---------------------------------------------------------------------------
class RunnersPage : public QWidget
{
    Q_OBJECT
public:
    RunnersPage(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuild();            // (re)build the table (on catalogChanged)

private:
    void filter(const QString &Text);   // show the rows whose name, family or platforms have Text

    AppModel &Model;
    QLineEdit *Search = nullptr;
    QTreeWidget *Table = nullptr;
    unsigned Generation = 0;            // the games-per-runner count is shown only for the latest rebuild
};

#endif // RUNNERSPAGE_H
