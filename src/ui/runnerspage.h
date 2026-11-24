#ifndef RUNNERSPAGE_H
#define RUNNERSPAGE_H

#include <QWidget>
#include <map>
#include <string>

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

    int rebuilds() const { return Rebuilds; }     // test observability
    int counts() const { return Counts; }

public slots:
    void rebuild();            // (re)build the table (on catalogChanged; hidden: when next shown)

protected:
    void showEvent(QShowEvent *E) override;

private:
    void filter(const QString &Text);   // show the rows whose name, family or platforms have Text
    //How many games each runner can run, off the GUI thread, one count at a time: a count asked while one runs runs
    //once after it, for the table as it is then (a count per rebuild ran them all, each a pass over the library).
    void countGames();
    std::map<std::string, class QTreeWidgetItem *> CountRows;   // runner key → its row, in the table as it is
    bool Counting = false, CountAgain = false, Stale = false;
    int Rebuilds = 0, Counts = 0;

    AppModel &Model;
    QLineEdit *Search = nullptr;
    QTreeWidget *Table = nullptr;
};

#endif // RUNNERSPAGE_H
