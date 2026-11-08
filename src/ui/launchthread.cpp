#include "launchthread.h"
#include "commonutils.h"
#include "containerwrapper.h"
#include "launchsession.h"

#include <QApplication>
#include <QMessageBox>
#include <algorithm>


void LaunchThread::kill()
{
    QMutexLocker Locker(&LiveMutex);
    if (Live) Live->Stop();
}

//What a machine that cannot run games yet is missing, said as the launch window says it ("" when nothing core is).
static QString MissingCoreMessage()
{
    const std::string Missing = LaunchSession::Session::MissingCore();
    if (Missing.empty()) return QString();
    return QStringLiteral("VidyaGod can't run games on this machine yet. Missing: ") + QString::fromStdString(Missing)
         + QStringLiteral("\n\nSettings → Dependencies shows what to install.");
}

void LaunchThread::run()
{
    //This launch's console and steps: every line it logs (unfiltered) and every phase it announces, from this thread
    //only — a second launch running meanwhile has its own. The last few percent are the game's own startup.
    LogSink Console([this](LogLevel Level, const std::string &Ctx, const std::string &Msg) {
                        emit logLine(static_cast<int>(Level), QString::fromStdString(Ctx), QString::fromStdString(Msg));
                    },
                    [this](int K, int N, const std::string &Label) {
                        emit progressChanged(std::min(97, K * 97 / N));
                        emit statusChanged(QString::fromStdString(Label));
                    });
    if (LaunchNodeId.empty()) { emit launchFinished(false, "No launch node specified."); return; }
    //The node graph is the one the app already holds (the dialog's snapshot of AppModel's index): a launch reads no
    //other package's files, and no other package's malformed node lands in this launch's verdict.
    if (!Index) { emit launchFinished(false, "No node graph to launch from."); return; }
    LogStep(1, 12, "Reading the library index");

    LaunchSession::Request R;
    R.Index = Index;
    R.NodeId = LaunchNodeId;
    R.GraftBase = GraftBase;
    R.Entrypoint = Entrypoint;
    R.Face = Face;
    R.InstanceName = InstanceName;
    R.Grafts = Grafts;
    R.VariableOverrides = VariableOverrides;
    R.RunnerChain = RunnerChain;
    R.ScreenWidth = ScreenWidth;     // captured on the GUI thread (onLaunchClicked): the engine never asks Qt here
    R.ScreenHeight = ScreenHeight;
    R.DryRun = DryRun;
    LaunchSession::Session S(GlobalConfigJSON, std::move(R));
    { QMutexLocker Locker(&LiveMutex); Live = &S; }
    //Every way out ends the same: the runtime down, the verdict (0/0 included) said, the outcome reported.
    const auto Finish = [&](bool Ok, const QString &Msg) {
        { QMutexLocker Locker(&LiveMutex); Live = nullptr; }
        S.Cleanup();
        const Diagnostics::Report V = S.Verdict("Launch of '" + LaunchNodeId + "'");
        emit diagnosticsReady(static_cast<int>(V.Errors), static_cast<int>(V.Warnings));
        emit launchFinished(Ok, Msg);
    };
    std::string Why;
    if (!S.Resolve(&Why)) return Finish(false, "Could not resolve what to run: " + QString::fromStdString(Why) + "\n\nSee the log for why.");
    if (!S.Build(&Why))
    {
        if (S.Stopped()) return Finish(true, "");   // a deliberate stop is not a failure
        const QString Missing = MissingCoreMessage();
        return Finish(false, !Missing.isEmpty() ? Missing : "The runtime did not build: " + QString::fromStdString(Why) + "\n\nSee the log for the rest.");
    }
    const LaunchSession::Outcome O = S.Run();
    emit progressChanged(100);
    emit statusChanged("Done.");
    //"Inspect runtime" (the launch window's checkbox): the runtime stays up — mounts and files — until the user
    //closes this, then it comes down as always. Shown on the GUI thread; the worker waits for it.
    if (PreserveRuntime && S.Wrapper())
    {
        const QString TempPath = QString::fromStdString(S.Wrapper()->ContainerParams.TempPath.string());
        QMetaObject::invokeMethod(qApp, [TempPath]{
            QMessageBox::information(QApplication::activeWindow(), "Inspect runtime",
                "The run's runtime (mounts + files) is available for inspection at:\n\n" + TempPath +
                "\n\nIt will be cleaned up (unmounted + deleted) as soon as you close this dialog.");
        }, Qt::BlockingQueuedConnection);
        LogOut("LaunchThread", "Runtime inspected at " + TempPath.toStdString() + "; cleaning up now.");
    }
    if (!O.Started && !O.Stopped)
    {
        const QString Missing = MissingCoreMessage();
        return Finish(false, !Missing.isEmpty() ? Missing : QStringLiteral("The game was not started.\n\nSee the log for why."));
    }
    //A deliberate stop is not a failure; a crash or a non-zero exit is — the runner's output is in the terminal/log.
    if (!O.Stopped && (O.Crashed || O.ExitCode != 0))
        return Finish(false, (O.Crashed ? QStringLiteral("The game process crashed.") : QStringLiteral("The game exited with code %1.").arg(O.ExitCode))
                             + QStringLiteral("\n\nIf it didn't launch, check the terminal/log output for the runner's error."));
    Finish(true, "");
}
