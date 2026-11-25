#ifndef UPDATESERVICE_H
#define UPDATESERVICE_H

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

#include "packagecatalog.h"   // PackageUpdate / StagedUpdate

class AppModel;

// Switching an installed package to a peer's version, from start to end, off the GUI thread: the version's node files
// are fetched into its stage and the switch planned (PackageCatalog::StageUpdate), the plan is put to the person (Ask,
// on the GUI thread), the switch is made (ApplyStagedUpdate), and the library is read again (AppModel::refreshFromDisk:
// built and hydrated on a worker, swapped in). Each step says where it is (progress); every end leaves no stage behind —
// done, refused, declined or cancelled (a switch that began and could not finish keeps its journal: the next start
// finishes it). One switch at a time. At the node's start it finishes the switches a crash cut short and drops every
// stage nothing will apply (PackageCatalog::SweepUpdateStages). GUI thread.
class UpdateService : public QObject
{
    Q_OBJECT
public:
    explicit UpdateService(AppModel & Model, QObject * Parent = nullptr);

    enum class Outcome { Done, Failed, Declined, Cancelled };
    Q_ENUM(Outcome)
    // The plan, for the person: Answer(true) switches, Answer(false) drops the stage. Called on the GUI thread.
    using Ask = std::function<void(const PackageCatalog::StagedUpdate & Plan, std::function<void(bool)> Answer)>;

    bool busy() const { return Busy; }
    QString current() const { return Current; }        // the package being switched ("" when none)
    bool start(const PackageCatalog::PackageUpdate & U, Ask Confirm);   // false: one is under way already
    void cancel();                                      // stops the fetch; a switch already applying finishes

signals:
    void started(const QString & package, const QString & peer);   // a switch begins (the activity strip lists it)
    void progress(double fraction, const QString & detail);    // fraction 0..1, or -1: no measure
    void finished(UpdateService::Outcome outcome, const QString & result);
    void swept(const QStringList & finished, const QStringList & errors);   // the start's sweep is done

private:
    void sweep();
    void end(Outcome O, const QString & Result);
    AppModel & Model;
    bool Busy = false, Swept = false;
    QString Current;
    std::shared_ptr<std::atomic<bool>> CancelFlag;
};

#endif // UPDATESERVICE_H
