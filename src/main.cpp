#include "main.h"
#include "cli/climodes.h"
#include "apppaths.h"
#include "platform/platform.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "vgdelta.h"       // .vgdelta generate + verify (shared with vidyagodfs) — the --convert-delta-chain tool
#include "prelaunchwindow.h"
#include "gamepicker.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "sandboxlayer.h"
#include "depcheck.h"

#include <QComboBox>
#include <QAbstractScrollArea>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>           // portable positioned reads (convert-delta-chain byte-verify)
#ifdef __linux__
#include <unistd.h>          // the overlay-sandbox ping test (Linux/bwrap-only) forks + execvp's + waitpid's
#ifndef _WIN32
#include <fcntl.h>           // --convert-delta-chain maps the archives (open/O_RDONLY)
#include <sys/mman.h>        //   … mmap/madvise: keep multi-GB inputs in evictable page cache, not anon RAM
#include <sys/stat.h>        //   … fstat for the mapped length
#endif
#include <sys/wait.h>
#endif

// Single-instance guard now lives behind the Platform shim (Platform::AcquireSingleInstanceLock):
// POSIX flock on a lock file on Linux, a session-named mutex on Windows. See platform.h.

//Application-wide event filter that stops a QComboBox from changing its value when the mouse
//wheel is scrolled over it (a very easy way to accidentally mutate settings). Qt's
//QComboBox::wheelEvent cycles the selection on hover-scroll regardless of focus, so the wheel is
//blocked on the (closed) combo control unconditionally — the value is still changed by clicking or
//by arrow keys. The open dropdown LIST is a separate widget, so scrolling an actually-open list
//still works. To keep scrollable pages usable, the swallowed wheel is redirected to the nearest
//enclosing scroll area so the page still scrolls past the combo. Installed once on the
//QApplication, so it covers every combo, including ones created dynamically.
class WheelGuard : public QObject
{
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject * Obj, QEvent * Event) override
    {
        if (Event->type() == QEvent::Wheel)
            if (QComboBox * Combo = qobject_cast<QComboBox *>(Obj))
            {
                //Forward the scroll to the surrounding scroll area (if any) instead of the combo.
                for (QWidget * W = Combo->parentWidget(); W; W = W->parentWidget())
                    if (QAbstractScrollArea * SA = qobject_cast<QAbstractScrollArea *>(W))
                    {
                        QCoreApplication::sendEvent(SA->viewport(), Event);
                        break;
                    }
                return true; //never let the hovered combo consume the wheel
            }
        return QObject::eventFilter(Obj, Event);
    }
};


int main(int argc, char *argv[])
{
    //IN-SANDBOX INIT: when invoked as `VidyaGod --sandbox-init …` (bwrap's payload inside a nested sandbox), do the
    //low-level setup (mount vidyagodfs, bring up the overlay TUN + hand its fd to the parent) and execvp the game.
    //Must be the FIRST thing in main() — before the single-instance lock, config init, or any Qt — because this is
    //PID ~1 of a throwaway namespace, not a normal VidyaGod run.
    if (SandboxLayer::IsSandboxInit(argc, argv))
        return SandboxLayer::RunSandboxInit(argc, argv);

    //Parse command line arguments and initialize RuntimeParameters struct.
    //Must happen before QApplication so headless runs never touch the display.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    LogOut("main.cpp", "Running VidyaGod in " + LaunchParameters.CurrentPath.string());
    LogOut("main.cpp", "Headless PackagePath: " + LaunchParameters.HeadlessPackagePath.string());

    //Check if dependencies exist in the system.
    //Non-fatal: warns but continues so the GUI still opens when VFS is not needed. In GUI mode a dialog
    //(below, once QApplication exists) lists anything missing with install instructions.
    const std::vector<std::string> MissingDeps = DepCheck::MissingCritical();

    //Auto-detect package-directory mode: if no --package flag was passed, check whether
    //the current working directory itself is a package. This lets users simply cd into a
    //package and run the binary directly without any extra flags.
    if (!LaunchParameters.HasHeadlessPackagePath && LaunchParameters.DataDir.empty())
    {
        //If the working directory is itself a package bundle (has node files), run it self-contained: a single
        //PreLaunchWindow (no library, no IPFS), with ALL paths inside the package dir — equivalent to
        //  --data-dir DIR --package-dir DIR --runtime-dir DIR --userdata-dir DIR.
        //It is a GUI dialog (not headless) — even one game has runner/variant/module configs to choose.
        LogOut("main.cpp", "Checking if running in a PACKAGEDIR.");
        if (IsRunningInPackageDir(LaunchParameters.CurrentPath))
        {
            const std::string Dir = LaunchParameters.CurrentPath.string();
            LaunchParameters.HasHeadlessPackagePath = true;
            LaunchParameters.RunningInPackageDir = true;
            LaunchParameters.HeadlessPackagePath = LaunchParameters.CurrentPath;
            LaunchParameters.DataDir             = Dir;   // AppDataPath = package dir
            LaunchParameters.PackageDirOverride  = Dir;
            LaunchParameters.RuntimeDirOverride  = Dir;
            LaunchParameters.UserDataDirOverride = Dir;
        }
    }

    //Data dir: normally ~/.VidyaGod, but PORTABLE mode keeps everything beside the app so the whole
    //thing travels on a USB stick. The "app location" the user sees is the AppImage file ($APPIMAGE) when
    //running as one, else the binary's own directory. Portable is enabled by placing either a "portable"
    //marker file or a "VidyaGod_data" directory next to the app.
    std::filesystem::path AppLocation;
    if (const char *AppImagePath = ::getenv("APPIMAGE"))
        AppLocation = std::filesystem::path(AppImagePath).parent_path();
    else
    {
        std::filesystem::path Self = Platform::SelfExe();
        if (!Self.empty()) AppLocation = Self.parent_path();
    }

    QString AppDataPath = QDir::homePath() + "/.VidyaGod";
    bool Portable = false;
    if (!AppLocation.empty())
    {
        std::error_code Ec;
        std::filesystem::path PortableData = AppLocation / "VidyaGod_data";
        if (std::filesystem::exists(AppLocation / "portable", Ec) ||
            std::filesystem::exists(AppLocation / "portable.txt", Ec) ||
            std::filesystem::is_directory(PortableData, Ec))
        {
            AppDataPath = QString::fromStdString(PortableData.string());
            Portable = true;
        }
    }

    //--data-dir override (testing: run a second instance with its own repo + lock alongside the main one).
    if (!LaunchParameters.DataDir.empty())
        AppDataPath = QString::fromStdString(LaunchParameters.DataDir);

    //Find and create AppDataDir. mkpath is a no-op if it already exists.
    QDir AppDataDir(AppDataPath);
    AppDataDir.mkpath(".");
    //Publish the resolved root as the single source of truth for every VidyaGod-produced path (launch TEMP, the
    //LIBRARY default, …) so portable / --data-dir relocate the WHOLE footprint — not just the ipfs repo + lock.
    AppPaths::SetDataRoot(AppDataDir.absolutePath().toStdString());
    //Record the run mode — it gates daemon/tray + start-on-login (see AppPaths::Mode). In-package wins (run + exit,
    //no daemon); then explicit CLI paths; then portable; else normal.
    AppPaths::SetMode(LaunchParameters.RunningInPackageDir ? AppPaths::Mode::InPackage
                      : !LaunchParameters.DataDir.empty()  ? AppPaths::Mode::CliPaths
                      : Portable                           ? AppPaths::Mode::Portable
                                                           : AppPaths::Mode::Normal);
    //Per-launch path overrides (--package-dir / --runtime-dir / --userdata-dir), read by the launch engine.
    if (!LaunchParameters.PackageDirOverride.empty())  AppPaths::SetPackagePathOverride(LaunchParameters.PackageDirOverride);
    if (!LaunchParameters.RuntimeDirOverride.empty())  AppPaths::SetRuntimePathOverride(LaunchParameters.RuntimeDirOverride);
    if (!LaunchParameters.UserDataDirOverride.empty()) AppPaths::SetUserDataPathOverride(LaunchParameters.UserDataDirOverride);
    LogOut("main.cpp", (Portable ? "Portable mode — data dir: " : "Data dir: ") + AppDataPath.toStdString());

    //Single-instance guard (GUI and headless alike): VidyaGod may only run once at a time. This both
    //prevents two instances from fighting over the same runtime/TEMP and makes the stale-mount sweep
    //below safe — since we hold the lock, any leftover mounts must be from a crashed run, never a live
    //sibling. The fd is held for the whole process lifetime (released by the kernel on exit/crash).
    //--bypass-single-instance-lock: proceed even if the lock is held (e.g. a headless --resolve-only/--validate-nodes
    //check while the GUI is open). Skips the guard — so the stale-mount sweep below can no longer assume a leftover
    //mount is from a crashed run, not a live sibling; use it only for read-only/CLI runs you know are safe.
    bool BypassInstanceLock = false;
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--bypass-single-instance-lock") BypassInstanceLock = true;

    bool InstanceLockHeld = Platform::AcquireSingleInstanceLock((AppDataDir.absolutePath() + "/vidyagod.lock").toStdString());
    if (!InstanceLockHeld && BypassInstanceLock)
        LogWarn("main.cpp", "Single-instance lock held, but --bypass-single-instance-lock given — proceeding without the lock.");
    else if (!InstanceLockHeld)
    {
        LogErr("main.cpp", "Another instance of VidyaGod is already running — aborting.");
        if (!LaunchParameters.RunningHeadless)
        {
            QApplication ErrApp(argc, argv);
            QMessageBox::warning(nullptr, "VidyaGod is already running",
                                 "Another instance of VidyaGod is already running.\n\n"
                                 "Only one instance can run at a time.");
        }
        return 1;
    }

    //We are the sole instance, so every mount still present under TEMP is a leftover from a crash.
    //Sweep them all now (the per-launch CleanStaleRuntime only covers the one game about to run).
    VfsMount::CleanStaleRuntime((AppDataDir.absolutePath() + "/TEMP").toStdString());

    //Initialization of GlobalConfigJSON, the central data structure of the program.
    //All runner definitions, library entries, and user settings live here.
    nlohmann::ordered_json GlobalConfigJSON;
    if (!InitializeGlobalConfigJSON(&GlobalConfigJSON, &AppDataDir))
    {
        LogErr("main.cpp", "Fatal error. GlobalConfigJSON initialization failed, aborting.");
        return 1;
    }

    //Embedded IPFS node (libvgipfs): its private repo lives INSIDE the app data dir (<AppDataDir>/ipfs) so it shares
    //the lifecycle of the content it references (deleting the data dir wipes pins/refs = true clean slate).
    //Networking is OFF by default in the GUI and the node start is DEFERRED to after the window shows (it delays
    //startup) — MainWindow brings it up when the user enables networking. Headless CLI modes that actually need the
    //node (fetch/seed/import/publish/peer-id/--node launch) start it up front here; the path-only modes
    //(validate/list/resolve) and in-package launch never touch it.
    const std::string IpfsRepo = (AppDataDir.absolutePath() + "/IPFS").toStdString();
    const bool HeadlessNeedsNode =
        LaunchParameters.PrintPeerId || LaunchParameters.PrintPinLs || !LaunchParameters.UnpinCid.empty()
        || !LaunchParameters.DropRefCid.empty()
        || !LaunchParameters.FetchCid.empty() || !LaunchParameters.SeedDir.empty() || LaunchParameters.HealPins
        || !LaunchParameters.ImportRunnerId.empty() || !LaunchParameters.ImportPackageUid.empty()
        || !LaunchParameters.PublishPackageDir.empty() || !LaunchParameters.PublishCidDir.empty()
        || !LaunchParameters.PublishMetaSrc.empty() || !LaunchParameters.RemintLibraryDir.empty()
        || LaunchParameters.PrintFriendCode || LaunchParameters.FriendListOnly
        || !LaunchParameters.FriendAddCode.empty() || LaunchParameters.FriendServe
        || LaunchParameters.LanHarness
        || !LaunchParameters.LaunchNodeId.empty();
    if (HeadlessNeedsNode)
        IpfsWrapper::StartNode(IpfsRepo);   // non-fatal: a failed start just means fetches/seeds report errors

    //HEADLESS: print this node's peer ID + dialable addrs, then exit (so another node can --connect to it).
    //Headless CLI modes — extracted per family into src/cli/ (P6). First family that handles a mode
    //returns its exit code; -1 = not requested, fall through (ultimately to the GUI bring-up below).
    for (auto *Mode : { &CliModes::RunIpfsModes, &CliModes::RunContentModes,
                        &CliModes::RunMaintenanceModes, &CliModes::RunNodeLaunch })
        if (int Rc = (*Mode)(LaunchParameters, GlobalConfigJSON, AppDataDir); Rc >= 0)
            return Rc;

    //Initializing GUI. MAKE SURE THERE ARE NOT QT GUI-RELATED CALLS ABOVE THIS LINE OR THE PROGRAM WILL CRASH.
    //QApplication must be constructed before any QWidget or QPixmap usage;
    //headless code paths above deliberately avoid creating any Qt GUI objects.
    //---------------------------------------------------------------------------------------------------------
    QApplication Application(argc, argv);
    //Force a consistent Qt style so the UI looks the same regardless of the system theme.
    Application.setStyle(QStyleFactory::create("Fusion"));

    //Set a default font to ensure consistent spacing across desktop environments.
    Application.setFont(QFont("DejaVu Sans", 10));

    //Block accidental hover-scroll from mutating combo boxes app-wide (see WheelGuard).
    Application.installEventFilter(new WheelGuard(&Application));

    //Surface any missing dependencies now that we can show a dialog — with concrete install steps.
    //Non-blocking to the app: the user dismisses it and the library still opens (launching games fails
    //until the deps are present).
    if (!MissingDeps.empty())
    {
        QString Msg = "VidyaGod needs the following to launch games, but couldn't find them:\n\n";
        for (const std::string &Dep : MissingDeps) Msg += "•  " + QString::fromStdString(Dep) + "\n";
        Msg += "\nOpen Settings → Dependencies for the full system check (Vulkan, 32-bit libraries, audio, …) and the "
               "exact package to install for each. You can still browse your library meanwhile.";
        QMessageBox::warning(nullptr, "Missing dependencies", Msg);
    }

    //Create and launch MainWindow. Passes GlobalConfigJSON and AppDataDir by pointer so
    //the window can persist changes (add/remove packages, save settings) to disk.

    //IN-PACKAGE MODE: the working dir is a self-contained package bundle → run JUST its PreLaunchWindow (no library
    //tabs, no IPFS node). Scan the bundle for its launchable nodes and open the prelaunch dialog on the first
    //presentable group. (Multi-game packages — a bundle with several groups — get a picker later; for now the first
    //group, which covers the common single-game-with-configs case.) The dialog's NodeIndex is this local, kept alive
    //by Application.exec().

    if (LaunchParameters.RunningInPackageDir)
    {
        NodeIndex PkgIndex;
        ManifestModel::ScanBundleNodes(LaunchParameters.HeadlessPackagePath.string(), PkgIndex);
        auto Groups = PackageCatalog::PresentableGroups(PkgIndex);
        if (Groups.empty())
        {
            QMessageBox::critical(nullptr, "VidyaGod",
                "This folder has no launchable game in it (no presentable node group).");
            return 1;
        }
        //Multi-game bundle → a picker grid (clicking a game opens its PreLaunchWindow). Single game → straight to it.
        if (Groups.size() > 1)
        {
            GamePicker Picker(&GlobalConfigJSON, &PkgIndex);
            Picker.show();
            return Application.exec();
        }
        std::vector<std::string> GroupNodeIds;
        for (const Node *N : Groups.front()) GroupNodeIds.push_back(N->NodeId);
        PreLaunchWindow Dialog(&GlobalConfigJSON, &PkgIndex, GroupNodeIds);
        Dialog.show();
        return Application.exec();
    }

    MainWindow MainWindow(&GlobalConfigJSON, &AppDataDir);
    MainWindow.startup(LaunchParameters.StartInTray);   // show(), or come up hidden if --tray / Start-in-tray / remembered
    return Application.exec();
}


// The default package source: an IPFS folder CID of the dehydrated built-in runner packages (ge-proton / wine /
// umu-proton / snes9x / native-passthrough). Seeded into Settings.PackageSources so a fresh install has runners;
// fetched by PackageCatalog::SyncPackageSources once the IPFS node is online, then hydrated on install like any
// package. Immutable — bumping the runner set = a new CID here (and an app release). (Git repos were removed.)
static std::string DefaultRunnerSourceCID() { return "Qmc2NAQfunvfwoMwAmskMGa5xTBwptg1sdcBZ7K78QNsxH"; }   // JSON-only runners Meta-CID (symlink-free archives, 2026-08-20)
// Same contract for the built-in LIBRARIES source: shared dependency nodes (DirectPlay, future codecs/redists) that
// games reference via PARENTS. It MUST always be present or a game's library-parent would dangle on a fresh install —
// exactly like the runners source. Immutable — bumping the library set = a new CID here (and an app release).
static std::string DefaultLibrarySourceCID() { return "QmcKipbWccxatkegfid3oHwxSNMo85hhL6Pico5PETaGDC"; }   // JSON-only libraries Meta-CID

//Guarantees the GlobalConfig has the shape the app actually uses, seeding any missing piece:
//  LIBRARY (array of packages), Settings (object), Settings.PackageSources (the CID package sources).
//Runners (and every other shared package) come from the configured PackageSources.
//Returns true if it added anything, so the caller can persist a freshly-seeded config.
static bool EnsureGlobalConfigDefaults(nlohmann::ordered_json & gc)
{
    bool Changed = false;
    if (!gc.is_object())                                        { gc = nlohmann::ordered_json::object();             Changed = true; }
    if (!gc.contains("LIBRARY")  || !gc["LIBRARY"].is_array())  { gc["LIBRARY"]  = nlohmann::ordered_json::array();  Changed = true; }
    if (!gc.contains("Settings") || !gc["Settings"].is_object()){ gc["Settings"] = nlohmann::ordered_json::object(); Changed = true; }
    //Migration: git repositories were removed — drop any legacy Settings.Repositories (its LIBRARY/<repo> clones, if
    //any, become inert local dirs). Sharing is now solely via CID package sources.
    if (gc["Settings"].contains("Repositories")) { gc["Settings"].erase("Repositories"); Changed = true; }
    //PackageSources: ordered list of IPFS folder CIDs of dehydrated packages, fetched into <DataRoot>/LIBRARY/<name>
    //and indexed.
    if (!gc["Settings"].contains("PackageSources") || !gc["Settings"]["PackageSources"].is_array())
    {
        gc["Settings"]["PackageSources"] = nlohmann::ordered_json::array();
        Changed = true;
    }
    //ALWAYS guarantee the built-in runners source is present and points at the CURRENT hardcoded CID. Keyed by its
    //reserved NAME "VidyaGodRunners" (not the CID), so: a fresh config gets it, a user who never had it (or removed it)
    //gets it back, and a release that bumps DefaultRunnerSourceCID() re-points the existing entry — otherwise the
    //runners default only ever seeded on a config with NO PackageSources at all (e.g. one carrying only a library
    //source would silently lack the runners source, and its CID would never surface in Sources / the IPFS tab).
    {
        auto & Sources = gc["Settings"]["PackageSources"];
        auto It = std::find_if(Sources.begin(), Sources.end(), [](const nlohmann::ordered_json & S) {
            return S.is_object() && S.value("NAME", std::string()) == "VidyaGodRunners"; });
        if (It == Sources.end())
        {
            Sources.push_back(nlohmann::ordered_json{ {"NAME", "VidyaGodRunners"}, {"CID", DefaultRunnerSourceCID()} });
            Changed = true;
        }
        else if ((*It).value("CID", std::string()) != DefaultRunnerSourceCID())
        {
            (*It)["CID"] = DefaultRunnerSourceCID();
            Changed = true;
        }
    }
    //Same guarantee for the built-in LIBRARIES source (shared dependency nodes like DirectPlay that games PARENT).
    {
        auto & Sources = gc["Settings"]["PackageSources"];
        auto It = std::find_if(Sources.begin(), Sources.end(), [](const nlohmann::ordered_json & S) {
            return S.is_object() && S.value("NAME", std::string()) == "VidyaGodLibraries"; });
        if (It == Sources.end())
        {
            Sources.push_back(nlohmann::ordered_json{ {"NAME", "VidyaGodLibraries"}, {"CID", DefaultLibrarySourceCID()} });
            Changed = true;
        }
        else if ((*It).value("CID", std::string()) != DefaultLibrarySourceCID())
        {
            (*It)["CID"] = DefaultLibrarySourceCID();
            Changed = true;
        }
    }
    return Changed;
}

//Loads GlobalConfig.JSON from AppDataDir (or starts empty if it does not exist), then ensures the
//config has the shape the app uses via EnsureGlobalConfigDefaults. The file is (re)written when it
//was freshly created or when a missing top-level key had to be seeded.
//Returns true on success (the engine-wide bool convention — the old inverted shell-style
//return was a foot-gun and is gone).
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    const bool Existed = GlobalConfigFile.exists();

    if (Existed && JSONOps::LoadJSON(&GlobalConfigFile, GlobalConfigJSON))
    {
        //LoadJSON returns non-zero on failure — don't silently overwrite a corrupt config.
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return false;
    }
    if (!Existed) LogOut("main.cpp", "Config file not detected. Creating defaults...");

    EnsureGlobalConfigDefaults(*GlobalConfigJSON);

    //Index the configured CID package sources: any already-fetched source dir (under LIBRARY) is scanned into the
    //un-hydrated LIBRARY index here. The FETCH of a not-yet-present source (e.g. the default runners on a first run)
    //needs the IPFS node online, so it happens later once networking is up (MainWindow::onNodeReady → syncSources, or
    //a headless mode that started the node). Mutates the config → always persist afterwards.
    PackageCatalog::SyncPackageSources(*GlobalConfigJSON);

    if (!JSONOps::SaveJSON(GlobalConfigJSON, &GlobalConfigFile))
    {
        LogErr("main.cpp", "GlobalConfig.JSON could not be written.");
        return false;
    }

    LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
    return true;
}

//True when CurrentPath is a package bundle (contains NODE_ID node files — see FSOps::CheckPackageValid).
bool IsRunningInPackageDir(const std::filesystem::path &CurrentPath)
{
    QDir Dir(QString::fromStdString(CurrentPath.string()));   // stack QDir — the old heap one leaked every call
    if (FSOps::CheckPackageValid(&Dir))
    {
        LogOut("main.cpp", "Running in PACKAGEDIR — opening its launcher (self-contained, no library/IPFS).");
        return true;
    }
    else
    {
        LogOut("main.cpp", "Running outside PACKAGEDIR, launching GUI.");
        return false;
    }
}

//Parses argc/argv sequentially.
//Recognized arguments:
//  --package <path>      : path to the package to run headlessly
//  --game <id>           : GAMEID string from the package's manifest
//  --component <id>      : COMPONENTID override (bypasses the game's default component)
//  --var KEY=VALUE       : override a CustomVar variable; may be repeated for multiple vars
//Unknown arguments are silently ignored.
LaunchParameters ParseCommandLineArguments(int argc, char* argv[])
{
    LaunchParameters RuntimeParameters;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "--package" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessPackagePath = argv[++i];
            RuntimeParameters.HasHeadlessPackagePath = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--node" && i + 1 < argc)
        {
            //Launch a launchable node from the global node graph (everything-is-a-node schema).
            RuntimeParameters.LaunchNodeId   = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--resolve-only" && i + 1 < argc)
        {
            //Resolve the node graph + dump ContainerParams to a file, then exit (no mount/launch).
            RuntimeParameters.LaunchNodeId   = argv[++i];
            RuntimeParameters.ResolveOnly    = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--validate-nodes")
        {
            RuntimeParameters.ValidateNodes   = true;
            RuntimeParameters.RunningHeadless = true;
            //Optional scope: `--validate-nodes <pkg>` validates only that package (UID / bundle dir / node id) and
            //its PARENTS closure — fast pre-publish check. Bare `--validate-nodes` still scans the whole catalog.
            if (i + 1 < argc && argv[i + 1][0] != '-') RuntimeParameters.ValidateScope = argv[++i];
        }
        else if (arg == "--fix-case-conflicts")
        {
            RuntimeParameters.FixCaseConflicts = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--convert-delta-chain")
        {
            //Everything after this flag (until the next --option) is a version chain: base node first, then
            //each newer version to reduce to a .vgdelta over the one before it.
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                RuntimeParameters.ConvertDeltaChain.push_back(argv[++i]);
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--seed" && i + 1 < argc)
        {
            //Seed a folder's published content (LAYER/COVER SOURCE CIDs) into the IPFS node by reference, then exit.
            RuntimeParameters.SeedDir         = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--seed-covers" && i + 1 < argc)
        {
            //Re-pin ONLY the cover art (META.COVER refs) by reference — skips re-hashing the large game layers.
            RuntimeParameters.SeedDir         = argv[++i];
            RuntimeParameters.SeedCoversOnly  = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--overwrite")
        {
            //Modifier for --seed/--seed-covers: re-reference every file (default is additive — only new/orphaned).
            RuntimeParameters.SeedOverwrite = true;
        }
        else if (arg == "--heal")
        {
            //Re-point orphaned refs across all configured package sources + prune stale pins, then exit.
            RuntimeParameters.HealPins        = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--fetch" && i + 2 < argc)
        {
            //Download-throughput probe: fetch a CID to a destination path then exit.
            RuntimeParameters.FetchCid         = argv[++i];
            RuntimeParameters.FetchDest        = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--fetch-dir" && i + 2 < argc)
        {
            //Recursively materialize a FOLDER CID to a destination dir then exit (verifies add-by-CID node files).
            RuntimeParameters.FetchCid         = argv[++i];
            RuntimeParameters.FetchDest        = argv[++i];
            RuntimeParameters.FetchDirMode     = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--peer-id")
        {
            RuntimeParameters.PrintPeerId      = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--pin-ls")
        {
            RuntimeParameters.PrintPinLs       = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--unpin" && i + 1 < argc)
        {
            RuntimeParameters.UnpinCid         = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--drop-ref" && i + 1 < argc)
        {
            RuntimeParameters.DropRefCid       = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-code")
        {
            RuntimeParameters.PrintFriendCode  = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-ls")
        {
            RuntimeParameters.FriendListOnly   = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-add" && i + 1 < argc)
        {
            RuntimeParameters.FriendAddCode    = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-serve")
        {
            RuntimeParameters.FriendServe      = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--friend-nick" && i + 1 < argc)
        {
            RuntimeParameters.FriendNick       = argv[++i];
        }
        else if (arg == "--friend-secs" && i + 1 < argc)
        {
            RuntimeParameters.FriendSecs       = std::max(1, std::atoi(argv[++i]));
        }
        else if (arg == "--lan")
        {
            RuntimeParameters.LanHarness       = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--overlay")
        {
            RuntimeParameters.OverlayUp        = true;
        }
        else if (arg == "--overlay-exec" && i + 1 < argc)
        {
            //Bring up the sandboxed overlay TUN and run this command inside it until it exits (stability harness).
            RuntimeParameters.OverlayUp        = true;
            RuntimeParameters.OverlayExec      = argv[++i];
            RuntimeParameters.LanHarness       = true;
        }
        else if (arg == "--tray")            // come up hidden in the tray (the start-on-login autostart entry uses this)
        {
            RuntimeParameters.StartInTray = true;
        }
        else if (arg == "--data-dir" && i + 1 < argc)
        {
            RuntimeParameters.DataDir          = argv[++i];
        }
        else if (arg == "--package-dir" && i + 1 < argc)
        {
            RuntimeParameters.PackageDirOverride = argv[++i];
        }
        else if (arg == "--runtime-dir" && i + 1 < argc)
        {
            RuntimeParameters.RuntimeDirOverride = argv[++i];
        }
        else if (arg == "--userdata-dir" && i + 1 < argc)
        {
            RuntimeParameters.UserDataDirOverride = argv[++i];
        }
        else if (arg == "--connect" && i + 1 < argc)
        {
            //Dial a known peer (full /p2p/ multiaddr) before fetching — direct peering / controlled benchmark.
            RuntimeParameters.ConnectAddr      = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--list-nodes")
        {
            RuntimeParameters.ListNodes       = true;
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--var" && i + 1 < argc)
        {
            //Expects KEY=VALUE format; silently skips malformed entries without '='.
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos)
                RuntimeParameters.VariableOverrides[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (arg == "--module" && i + 1 < argc)
        {
            //Expects COMPONENT=on|off (also true|false / 1|0); toggles one optional module. Malformed skipped.
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos)
            {
                std::string val = kv.substr(eq + 1);
                bool on = (val == "on" || val == "true" || val == "1" || val == "yes");
                RuntimeParameters.ModuleStates[kv.substr(0, eq)] = on;
            }
        }
        else if (arg == "--import-runner" && i + 1 < argc)
        {
            RuntimeParameters.ImportRunnerId = argv[++i];
        }
        else if (arg == "--import-package" && i + 1 < argc)
        {
            RuntimeParameters.ImportPackageUid = argv[++i];
        }
        else if (arg == "--publish" && i + 1 < argc)
        {
            RuntimeParameters.PublishPackageDir = argv[++i];
        }
        else if (arg == "--publish-to" && i + 1 < argc)
        {
            RuntimeParameters.PublishToDir = argv[++i];
        }
        else if (arg == "--publish-cid" && i + 1 < argc)
        {
            RuntimeParameters.PublishCidDir  = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--remint-library" && i + 1 < argc)
        {
            RuntimeParameters.RemintLibraryDir = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--publish-meta" && i + 1 < argc)
        {
            RuntimeParameters.PublishMetaSrc = argv[++i];
            RuntimeParameters.RunningHeadless = true;
        }
        else if (arg == "--runner" && i + 1 < argc)
        {
            //Repeatable: each --runner appends one link to the daisy-chain (innermost→outermost). The first one also
            //sets RunnerID for back-compat (single-runner pin).
            std::string R = argv[++i];
            if (RuntimeParameters.RunnerID.empty()) RuntimeParameters.RunnerID = R;
            RuntimeParameters.RunnerChain.push_back(std::move(R));
        }
    }
    return RuntimeParameters;
}
