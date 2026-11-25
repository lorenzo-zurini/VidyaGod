#ifndef LONGOP_H
#define LONGOP_H

#include <QString>

#include <functional>

class QWidget;

//A long operation as every one started from a window runs: its work on a worker (AsyncWork), the window waiting under
//a window-modal progress dialog that says where the work is ("game.zip — file 3 of 12 · 1.2 GB of 4.0 GB") and offers
//Cancel, which the work reads at its next safe point. Run returns when the work is done (a quit too waits for it), so a
//caller keeps its shape; meanwhile the GUI thread paints and answers, and nothing else in that window can be clicked
//(the document under the work cannot change). The work never touches a widget: it says where it is through its Step.
namespace LongOp {

struct Step
{
    //Item (a file's name) is the Done-th of Total; Bytes of TotalBytes are behind it (TotalBytes 0: not counted).
    std::function<void(const QString &Item, int Done, int Total, unsigned long long Bytes, unsigned long long TotalBytes)> Report;
    std::function<bool()> Cancelled;          // the user asked to stop
    std::function<void(bool)> CanCancel;      // false while stopping would leave things half done (a save's writes)
};

//The dialog's text for a Report (what the label says).
QString Describe(const QString &Item, int Done, int Total, unsigned long long Bytes, unsigned long long TotalBytes);
//False when the user cancelled, or the app began to quit (what the work did before its safe point stays done). Never
//returns before the work has ended: a quit waits for it.
bool Run(QWidget *Parent, const QString &Title, const std::function<void(const Step &)> &Work);
//A Run under Parent is going on (the window waits): what changes its document is refused meanwhile (the CLI's edits),
//as the window's own input is. GUI thread.
bool RunningIn(const QWidget *Parent);
//W lies in a window a Run waits over (its window-modal dialog blocks a person's input there): the CLI's input into it
//(ui click, set, …) is refused, as a person's is.
bool Blocks(const QWidget *W);
//TEST ONLY: works Run has finished on a thread other than the GUI's.
int DebugRanOnWorker();

} // namespace LongOp

#endif // LONGOP_H
