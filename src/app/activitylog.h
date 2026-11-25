#ifndef ACTIVITYLOG_H
#define ACTIVITYLOG_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

//What the app is doing in the background, and what it did: one list every long job reports into — downloads, publish,
//seeding a folder, a force recheck, a source sync, an editor's seed or case fix — so its progress, its Cancel and its
//result are in one place (the main window's activity strip) instead of a modal box popped from a page the user is
//not on. A job is started (running: a fraction, or busy), told where it is, then finished with a result that stays
//until the user dismisses it. Kinds group the jobs ("Downloads", "Publish", "Seed folder", "Force recheck", "Sources",
//"Package editor"). GUI thread only (a worker posts through AsyncWork::Job::Post).
class ActivityLog : public QObject
{
    Q_OBJECT
public:
    struct Entry
    {
        int Id = 0;
        QString Kind, Title, Detail;      // Detail: where it is, while running; the result, once finished
        double Fraction = -1;             // 0..1, or -1: busy (no measure)
        bool Running = true, Failed = false;
        bool CanCancel = false;
        QDateTime Started, Ended;
    };

    static ActivityLog & Get();

    //A job begins (its Cancel, if it can be stopped). Returns its id.
    int start(const QString & Kind, const QString & Title, std::function<void()> Cancel = {});
    //Where it is: a fraction 0..1 (or -1: busy) and a line.
    void progress(int Id, double Fraction, const QString & Detail = {});
    //It is done: Ok or not, and what came of it — kept until dismissed.
    void finish(int Id, bool Ok, const QString & Result);
    //A result with nothing running before it (a failure found by a background check). Returns its id.
    int note(const QString & Kind, const QString & Title, bool Ok, const QString & Result);
    //A job of Kind and Title that is still running, or 0: one kind of job that runs once (a source sync) is not listed twice.
    int runningOf(const QString & Kind, const QString & Title = {}) const;
    void cancel(int Id);                  // asks the job to stop (its Cancel); it still finishes itself
    void dismiss(int Id);                 // a finished entry goes
    void dismissFinished();               // every finished one goes

    QVector<Entry> entries() const { return Items; }
    const Entry * find(int Id) const;
    int running() const;
    int failedUnseen() const { return Unseen; }   // failures since the list was last looked at
    void markSeen();

signals:
    void changed();

private:
    explicit ActivityLog(QObject * Parent = nullptr) : QObject(Parent) {}
    Entry * at(int Id);
    QVector<Entry> Items;
    QVector<std::pair<int, std::function<void()>>> Cancels;
    int NextId = 1, Unseen = 0;
};

#endif // ACTIVITYLOG_H
