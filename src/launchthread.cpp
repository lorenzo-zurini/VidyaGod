#include "launchthread.h"
#include "containerwrapper.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "commonutils.h"

#include <QApplication>
#include <QMessageBox>
#include <filesystem>

// ============================================================================
// LaunchThread
// ============================================================================

// Forwards a kill request to the active ContainerWrapper (if any).
void LaunchThread::kill()
{
    QMutexLocker Locker(&wrapperMutex);
    if (wrapper)
        wrapper->KillGame();
}

// Full container lifecycle executed on the worker thread.
// Emits progress/status signals at key milestones so the UI can stay in sync.
void LaunchThread::run()
{
    // -----------------------------------------------------------------
    // Install the log callback: emit logLine() (queued automatically by
    // Qt's cross-thread signal delivery) and derive progress/status from
    // well-known context+message patterns.
    // -----------------------------------------------------------------
    SetLogCallback([this](LogLevel level, const std::string& ctx, const std::string& msg)
    {
        // Forward the raw line to the console widget.
        emit logLine(static_cast<int>(level), QString::fromStdString(ctx), QString::fromStdString(msg));

        // ---- progress / status heuristics ----
        if (ctx.find("InitializeContainer") != std::string::npos)
        {
            emit progressChanged(5);
            emit statusChanged("Initializing container...");
        }
        else if (ctx.find("InitializeDefPrefix") != std::string::npos)
        {
            if (msg.find("Initialising") != std::string::npos)
            {
                emit progressChanged(10);
                emit statusChanged("Initializing Wine prefix...");
            }
            else if (msg.find("successful") != std::string::npos ||
                     msg.find("SUC") != std::string::npos ||
                     msg.find("Prefix initialisation successful") != std::string::npos)
            {
                emit progressChanged(25);
                emit statusChanged("Prefix ready.");
            }
        }
        else if (ctx.find("BuildDefaultData") != std::string::npos)
        {
            emit progressChanged(35);
            emit statusChanged("Preparing package edits...");
            if (msg.find("DEFAULTDATA layer built") != std::string::npos)
                emit progressChanged(45);
        }
        else if (ctx.find("MountVFS") != std::string::npos && msg.find("Mounting via") != std::string::npos)
        {
            emit progressChanged(55);
            emit statusChanged("Assembling filesystem...");
        }
        else if (ctx.find("MountVFS") != std::string::npos &&
                 (msg.find("Successfully") != std::string::npos || msg.find("mounted VFS") != std::string::npos))
        {
            emit progressChanged(75);
            emit statusChanged("VFS mounted.");
        }
        else if (ctx.find("BuildContainerRuntime") != std::string::npos &&
                 msg.find("Runtime ready") != std::string::npos)
        {
            emit progressChanged(90);
            emit statusChanged("Runtime ready.");
        }
        else if (ctx.find("Execute") != std::string::npos && msg.find("Executing:") != std::string::npos)
        {
            emit progressChanged(95);
            emit statusChanged("Game running...");
        }
        else if (ctx.find("Execute") != std::string::npos && msg.find("Process exited") != std::string::npos)
        {
            emit progressChanged(100);
            emit statusChanged("Done.");
        }
    });

    if (this->LaunchNodeId.empty())
    {
        ClearLogCallback();
        emit launchFinished(false, "No launch node specified.");
        return;
    }

    // -----------------------------------------------------------------
    // Build ContainerParams and ContainerWrapper (native node-graph launch).
    // -----------------------------------------------------------------
    std::filesystem::path NoPath;
    struct ContainerParams Params(NoPath, std::string(), std::string());
    Params.VariableOverrides = this->VariableOverrides;
    Params.ModuleStates      = this->ModuleStates;
    //The chosen runner is set BEFORE construction so PickRunnerNode honours it. Screen geometry was captured on
    //the main thread (see onLaunchClicked) — the engine uses these instead of querying QGuiApplication here.
    Params.RunnerID  = this->RunnerID;
    Params.ScreenWidth  = this->ScreenWidth;
    Params.ScreenHeight = this->ScreenHeight;

    //Build the global node index here (worker thread) and point the engine at the launch node. Index is a local
    //that outlives LocalWrapper (which holds ContainerParams by reference).
    std::vector<std::filesystem::path> Roots;
    for (const auto &D : PackageCatalog::RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
    NodeIndex Index = ManifestModel::BuildNodeIndex(Roots);
    Params.NodeIdx      = &Index;
    Params.LaunchNodeId = this->LaunchNodeId;

    nlohmann::ordered_json UnusedManifest = nlohmann::ordered_json::object();
    ContainerWrapper* LocalWrapper = new ContainerWrapper(GlobalConfigJSON, UnusedManifest, Params);

    // Store wrapper pointer so kill() can reach it.
    {
        QMutexLocker Locker(&wrapperMutex);
        wrapper = LocalWrapper;
    }

    // -----------------------------------------------------------------
    // Step 1: Resolve executable definition (reads the launch node's EXEC).
    // -----------------------------------------------------------------
    if (!ContainerWrapper::ResolveExecutableDefinition(UnusedManifest, LocalWrapper->ContainerParams))
    {
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Could not resolve variant.\nCheck VARIANT_ID and MODULES in the manifest.");
        return;
    }

    // -----------------------------------------------------------------
    // Step 2: Build runtime (mounts, prefix, registry patches).
    // -----------------------------------------------------------------
    if (!LocalWrapper->BuildContainerRuntime())
    {
        LocalWrapper->Cleanup();
        ClearLogCallback();
        {
            QMutexLocker Locker(&wrapperMutex);
            wrapper = nullptr;
        }
        delete LocalWrapper;
        emit launchFinished(false, "Failed to build container runtime.\nCheck that all components are defined and their zip files exist.");
        return;
    }

    // -----------------------------------------------------------------
    // Step 3: Execute game (blocks until process exits or is killed).
    // -----------------------------------------------------------------
    LocalWrapper->Execute();

    // Capture the run outcome BEFORE the wrapper is destroyed, so we can report a failed launch.
    const bool Crashed   = LocalWrapper->LastCrashed;
    const int  ExitCode  = LocalWrapper->LastExitCode;
    const bool UserKilled = LocalWrapper->UserKilled;

    // -----------------------------------------------------------------
    // Step 4: Cleanup. If the user ticked the prelaunch "Inspect runtime" checkbox, first pause here with a modal
    // dialog while the runtime (mounts + files) is STILL in place, so they can examine it; cleanup runs the moment
    // that dialog is dismissed. Cleanup ALWAYS happens either way — the runtime is never left dangling. The prompt
    // runs on the GUI thread (blocking) so the worker waits for it. (CLI --node path never reaches here.)
    // -----------------------------------------------------------------
    std::filesystem::path WriteLayerPath = LocalWrapper->ContainerParams.WriteLayerPath;

    if (PreserveRuntime)
    {
        const QString TempPath = QString::fromStdString(LocalWrapper->ContainerParams.TempPath.string());
        QMetaObject::invokeMethod(qApp, [TempPath]{
            QMessageBox::information(QApplication::activeWindow(), "Inspect runtime",
                "The run's runtime (mounts + files) is available for inspection at:\n\n" + TempPath +
                "\n\nIt will be cleaned up (unmounted + deleted) as soon as you close this dialog.");
        }, Qt::BlockingQueuedConnection);
        LogOut("LaunchThread", "Runtime inspected at " + LocalWrapper->ContainerParams.TempPath.string()
                               + "; cleaning up now.");
    }
    LocalWrapper->Cleanup();

    if (this->DryRun)
    {
        //WRITELAYER is ephemeral and normally already removed by Cleanup(); this also covers
        //the preserve case so a dry run never leaves write-layer state behind.
        std::error_code ec;
        std::filesystem::remove_all(WriteLayerPath, ec);
        if (ec) LogWarn("LaunchThread", "Dry run: could not remove WRITELAYER: " + ec.message());
        else    LogSucc("LaunchThread", "Dry run: WRITELAYER deleted.");
    }

    ClearLogCallback();

    {
        QMutexLocker Locker(&wrapperMutex);
        wrapper = nullptr;
    }
    delete LocalWrapper;

    // A deliberate user-kill is not a failure. Otherwise a crash or a non-zero exit code means the game didn't
    // run cleanly — surface it (the runner's output was forwarded live to the terminal/log for details).
    if (!UserKilled && (Crashed || ExitCode != 0))
    {
        QString Msg = Crashed
            ? QStringLiteral("The game process crashed.")
            : QStringLiteral("The game exited with code %1.").arg(ExitCode);
        Msg += QStringLiteral("\n\nIf it didn't launch, check the terminal/log output for the runner's error.");
        emit launchFinished(false, Msg);
        return;
    }

    emit launchFinished(true, "");
}
