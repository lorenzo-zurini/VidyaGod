#ifndef LONGOP_H
#define LONGOP_H

#include <QString>

#include <functional>

class QWidget;

//A long operation as every one started from a window runs: its work on a worker (AsyncWork), the window waiting under
//a window-modal progress dialog that says where the work is ("game.zip — file 3 of 12 · 1.2 GB of 4.0 GB") and offers
//Cancel, which the work reads at its next safe point. Run returns when the work is done, so a caller keeps its shape;
//meanwhile the GUI thread paints and answers, and nothing else in that window can be clicked (the document under the
//work cannot change). The work never touches a widget: it says where it is through its Step.
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
//False when the user cancelled (what the work did before its safe point stays done).
bool Run(QWidget *Parent, const QString &Title, const std::function<void(const Step &)> &Work);
//TEST ONLY: works Run has finished on a thread other than the GUI's.
int DebugRanOnWorker();

} // namespace LongOp

#endif // LONGOP_H
