#ifndef LAUNCHTHREAD_H
#define LAUNCHTHREAD_H

#include <QThread>
#include <QMutex>

#include <map>
#include <string>

#include "nlohmann/json.hpp"

class ContainerWrapper;   // pointer member only — full definition needed in launchthread.cpp

// ---------------------------------------------------------------------------
// LaunchThread
// Runs the full container lifecycle (InitializeContainer → BuildContainerRuntime → Execute → Cleanup) on a
// worker thread so the UI stays responsive. Fields must be populated before start(); progress/status/log/
// finished are reported via signals (queued to the GUI thread).
// ---------------------------------------------------------------------------
class LaunchThread : public QThread
{
    Q_OBJECT
public:
    // Fields must be populated before calling start().
    nlohmann::ordered_json GlobalConfigJSON;
    std::string            LaunchNodeId;      // Native node-graph launch: the ROLE:"launchable" node to run
    std::string                        RunnerID;          // RUNNER_ID chosen in the picker (resolved before construction)
    std::map<std::string, std::string> VariableOverrides; // CustomVar values from picker / variant FORCEVARS seeds
    std::map<std::string, bool>        ModuleStates;      // Optional-module toggles from the prelaunch tree (component → enabled)
    std::string                        ScreenWidth;       // Captured on the MAIN thread before start() — never query Qt GUI from run()
    std::string                        ScreenHeight;      // (QGuiApplication screen access off the main thread is undefined behaviour)
    bool                               DryRun = false;    // If true, WRITELAYER is deleted after cleanup
    bool                               PreserveRuntime = false; // If true, pause after exit with a modal dialog while
                                                            // the runtime (mounts + files) is still in place for
                                                            // inspection; cleanup runs the moment it's dismissed.
                                                            // Cleanup always happens — never left dangling. Set from
                                                            // the prelaunch checkbox.

    // Forcibly kills the running game process (if any).
    void kill();

signals:
    // Raw log line forwarded from the Log() callback.
    void logLine(int level, QString context, QString message);
    // Short human-readable status string for the status label.
    void statusChanged(QString status);
    // Progress 0-100 for the progress bar.
    void progressChanged(int value);
    // Emitted once when the full lifecycle has completed (success or failure).
    void launchFinished(bool success, QString errorMsg);

protected:
    void run() override;

private:
    ContainerWrapper* wrapper = nullptr;
    QMutex            wrapperMutex;
};

#endif // LAUNCHTHREAD_H
