#include "launchthread.h"
#include "containerwrapper.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "commonutils.h"

#include "ipfswrapper.h"     // LanLaunchVars + GetProfile — the virtual-LAN session vars
#include <QApplication>
#include <QMessageBox>
#include <algorithm>
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
        // Forward the raw line to the console widget — EVERY engine log line is shown, unfiltered.
        emit logLine(static_cast<int>(level), QString::fromStdString(ctx), QString::fromStdString(msg));

        // ---- progress / status: the LaunchStep CONTRACT (commonutils.h LogStep) ----
        // The engine announces every phase as context "LaunchStep", message "<k>/<n> <label>", and times it
        // on completion ("… — done in N ms"). Progress is k/n of the bar (the last few percent are reserved
        // for the game's own startup); status shows the label verbatim. This replaced the old fragile
        // phrase-matching heuristics — the marker is an explicit interface, not log wording.
        if (ctx == "LaunchStep")
        {
            const size_t Slash = msg.find('/');
            const size_t Space = msg.find(' ');
            if (Slash != std::string::npos && Space != std::string::npos && Slash < Space)
            {
                const int K = std::atoi(msg.substr(0, Slash).c_str());
                const int N = std::atoi(msg.substr(Slash + 1, Space - Slash - 1).c_str());
                if (K > 0 && N > 0)
                {
                    emit progressChanged(std::min(97, K * 97 / N));
                    emit statusChanged(QString::fromStdString(msg.substr(Space + 1)));
                }
            }
        }
        else if (ctx.find("Execute") != std::string::npos && msg.find("Process exited") != std::string::npos)
        {
            emit progressChanged(100);
            emit statusChanged("Done.");
        }
    });

    //Start counting every WARN/ERR this launch produces. A launch prints hundreds of lines, so a warning in the
    //middle is invisible — Worms 4 Mayhem logged one on EVERY launch for months and the only visible symptom was
    //a wrong aspect ratio. The verdict is reported LOUDLY at the end, whatever the outcome.
    Diagnostics::Begin();

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
    //Engine-injected session facts: the virtual-LAN vars (SELF_VIP / PEER_VIPS / PEER_NAMES / SUBNET / SANDBOX)
    //plus the player's own display name. Lowest priority (see ContainerParams::SessionVars) and seeded early enough
    //to reach EXEARGS / other CustomVar DEFAULTs. LanLaunchVars is empty when the node is down, so SELF_NAME needs a
    //local fallback too — a package writes it into a game config verbatim and must never get an empty name.
    Params.SessionVars = IpfsWrapper::LanLaunchVars();
    if (!Params.SessionVars.count("VIDYAGOD_SELF_NAME") || Params.SessionVars["VIDYAGOD_SELF_NAME"].empty())
    {
        const std::string Nick = IpfsWrapper::GetProfile().Nick;
        Params.SessionVars["VIDYAGOD_SELF_NAME"] = Nick.empty() ? std::string("Player") : Nick;
    }
    //The chosen runner chain is set BEFORE construction so the resolver honours it. Screen geometry was captured on
    //the main thread (see onLaunchClicked) — the engine uses these instead of querying QGuiApplication here.
    Params.RunnerID  = this->RunnerID;
    //Runner daisy-chain: the explicit picker/CLI chain wins; else fall back to the single RunnerID as a 1-step pin.
    if (!this->RunnerChain.empty())      Params.RunnerChainIds = this->RunnerChain;
    else if (!this->RunnerID.empty())    Params.RunnerChainIds = { this->RunnerID };
    Params.ScreenWidth  = this->ScreenWidth;
    Params.ScreenHeight = this->ScreenHeight;

    //Build the global node index here (worker thread) and point the engine at the launch node. Index is a local
    //that outlives LocalWrapper (which holds ContainerParams by reference). BuildCatalogIndex = repos + locally-added
    //packages, so a launch of an externally-added bundle resolves (same source the library lists from).
    LogStep(1, 12, "Indexing the package catalog");
    auto Index = std::make_shared<NodeIndex>(PackageCatalog::BuildCatalogIndex(GlobalConfigJSON));
    Params.NodeIdx      = Index.get();
    Params.NodeIdxOwned = Index;         // the wrapper's ContainerParams copy co-owns the index — no dangling
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
    if (!LaunchResolver::ResolveExecutableDefinition(UnusedManifest, LocalWrapper->ContainerParams))
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

    //THE VERDICT. Printed for every launch, clean or not — "0 warnings" is information too, and a summary that
    //only appears on failure trains you to ignore its absence. When something did go wrong this is deliberately
    //hard to miss: a banner, the distinct messages (deduplicated — one authoring bug can emit the same line for
    //900 files), and a statement that the game may have misbehaved because of it.
    {
        const Diagnostics::Report R = Diagnostics::ReportVerdict("Launch of '" + this->LaunchNodeId + "'");
        emit diagnosticsReady(static_cast<int>(R.Errors), static_cast<int>(R.Warnings));
    }

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
