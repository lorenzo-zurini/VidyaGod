#include "main.h"
#include "apppaths.h"
#include "commonutils.h"
#include "manifestmodel.h"
#include "packagecatalog.h"
#include "containerwrapper.h"
#include "ipfswrapper.h"
#include "vgipfsapi.h"

#include <QComboBox>
#include <QAbstractScrollArea>
#include <QWheelEvent>

#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

//Single-instance guard. Opens (creating if needed) a lock file under ~/.VidyaGod and takes an
//exclusive, non-blocking flock on it. Returns the held fd on success, or -1 if another live instance
//already holds it. The lock is owned by the open fd, so the kernel releases it automatically when this
//process exits OR crashes — a previous crashed instance never blocks a new one, and no stale-lock
//bookkeeping is needed. The returned fd is intentionally kept open for the whole process lifetime.
static int AcquireSingleInstanceLock(const std::string &LockPath)
{
    //O_CLOEXEC is essential: spawned children (wine/proton/git/vidyagodfs) must NOT inherit this fd, or a lingering
    //wine background process (services.exe/wineserver/…) keeps the flock held after VidyaGod exits, making every
    //later run falsely abort with "another instance is already running".
    int Fd = ::open(LockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (Fd < 0) return -1;
    if (::flock(Fd, LOCK_EX | LOCK_NB) != 0) { ::close(Fd); return -1; }
    return Fd;
}

static QString DependencyImportHint(const std::string &Dep); // defined after main()

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

//Serializes the resolved ContainerParams that feed the (frozen) runtime — the exact input the engine produces
//after the front-end resolution. Used by --resolve-only for golden-compare (synthesis bridge vs native consumer)
//and as a hang-free verification (no mount, no game). Field order is stable for diffing.
static nlohmann::ordered_json DumpResolution(const struct ContainerParams &CP)
{
    nlohmann::ordered_json J;
    J["PackageUID"]        = CP.PackageUID;
    J["GameName"]          = CP.GameName;
    J["Platform"]          = CP.Platform;
    J["Recipe"]            = CP.Recipe;
    J["RunnerName"]        = CP.RunnerName;
    J["RunnerExecutable"]  = CP.RunnerExecutable;
    J["RunnerArgs"]        = CP.RunnerArgs;
    J["RunnerEnv"]         = CP.RunnerEnv;
    J["RunnerRemoveEnv"]   = CP.RunnerRemoveEnv;
    J["ContentRoot"]       = CP.ContentRoot;
    J["PrefixRoot"]        = CP.PrefixRoot;
    J["PrefixGenerate"]    = CP.PrefixGenerate;
    J["ContentPath"]       = CP.ExePathRelative.string();
    J["Content"]           = CP.ExePathComplete.string();
    J["ExeArgs"]           = CP.ExeArgs;
    J["DLLOverrides"]      = CP.DLLOverrides;
    J["PersistAll"]        = CP.PersistAll;
    J["PersistDirs"]       = CP.PersistDirs;
    J["PersistFiles"]      = CP.PersistFiles;
    J["PersistRegistry"]   = CP.PersistRegistry;
    J["PersistRegKeys"]    = CP.PersistRegKeys;
    J["CustomVariables"]   = CP.CustomVariables;
    J["RunnerShipsBuild"]  = CP.RunnerShipsBuild;
    J["UnifiedRuntime"]    = CP.UnifiedRuntime;
    J["SubComponentsArray"]= CP.SubComponentsArray;
    return J;
}

int main(int argc, char *argv[])
{
    //Parse command line arguments and initialize RuntimeParameters struct.
    //Must happen before QApplication so headless runs never touch the display.
    LaunchParameters LaunchParameters = ParseCommandLineArguments(argc, argv);
    LogOut("main.cpp", "Running VidyaGod in " + LaunchParameters.CurrentPath.string());
    LogOut("main.cpp", "Headless PackagePath: " + LaunchParameters.HeadlessPackagePath.string());
    LogOut("main.cpp", "Headless GameID: " + LaunchParameters.HeadlessGameID);
    LogOut("main.cpp", "Headless ComponentID: " + LaunchParameters.HeadlessComponentID);

    //Check if dependencies exist in the system.
    //Non-fatal: warns but continues so the GUI still opens when VFS is not needed. In GUI mode a dialog
    //(below, once QApplication exists) lists anything missing with install instructions.
    std::list<std::string> MissingDeps = CheckExecutableDependencies();

    //Auto-detect package-directory mode: if no --package flag was passed, check whether
    //the current working directory itself is a package. This lets users simply cd into a
    //package and run the binary directly without any extra flags.
    if (!LaunchParameters.HasHeadlessPackagePath)
    {
        //Detect ./METADATA/MANIFEST.json to detemine if running in packagedir.
        //Start in GUI-less mode if so.
        LogOut("main.cpp", "Checking if running in a PACKAGEDIR.");
        if(IsRunningInPackageDir(LaunchParameters.CurrentPath))
        {
            LaunchParameters.HasHeadlessPackagePath = true;
            LaunchParameters.RunningInPackageDir = true;
            LaunchParameters.HeadlessPackagePath = LaunchParameters.CurrentPath;
            LaunchParameters.RunningHeadless = true;
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
        std::error_code SelfEc;
        std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", SelfEc);
        if (!SelfEc) AppLocation = Self.parent_path();
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
    LogOut("main.cpp", (Portable ? "Portable mode — data dir: " : "Data dir: ") + AppDataPath.toStdString());

    //Single-instance guard (GUI and headless alike): VidyaGod may only run once at a time. This both
    //prevents two instances from fighting over the same runtime/TEMP and makes the stale-mount sweep
    //below safe — since we hold the lock, any leftover mounts must be from a crashed run, never a live
    //sibling. The fd is held for the whole process lifetime (released by the kernel on exit/crash).
    int InstanceLockFd = AcquireSingleInstanceLock((AppDataDir.absolutePath() + "/vidyagod.lock").toStdString());
    if (InstanceLockFd < 0)
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
    if (InitializeGlobalConfigJSON(&GlobalConfigJSON, &AppDataDir))
    {
        //InitializeGlobalConfigJSON returns true on FAILURE (shell exit-code convention).
        LogErr("main.cpp", "Fatal error. GlobalConfigJSON initialization failed, aborting.");
        return 1;
    }

    //Embedded IPFS node (libvgipfs): its private repo lives INSIDE the app data dir (<AppDataDir>/ipfs) so it shares
    //the lifecycle of the content it references (deleting the data dir wipes pins/refs = true clean slate).
    //Networking is OFF by default in the GUI and the node start is DEFERRED to after the window shows (it delays
    //startup) — MainWindow brings it up when the user enables networking. Headless CLI modes that actually need the
    //node (fetch/seed/import/publish/peer-id/--node launch) start it up front here; the path-only modes
    //(validate/list/resolve) and in-package launch never touch it.
    const std::string IpfsRepo = (AppDataDir.absolutePath() + "/ipfs").toStdString();
    const bool HeadlessNeedsNode =
        LaunchParameters.PrintPeerId || !LaunchParameters.FetchCid.empty() || !LaunchParameters.SeedDir.empty()
        || !LaunchParameters.ImportRunnerId.empty() || !LaunchParameters.ImportPackageUid.empty()
        || !LaunchParameters.PublishPackageDir.empty() || !LaunchParameters.LaunchNodeId.empty();
    if (HeadlessNeedsNode)
        IpfsWrapper::StartNode(IpfsRepo);   // non-fatal: a failed start just means fetches/seeds report errors

    //HEADLESS: print this node's peer ID + dialable addrs, then exit (so another node can --connect to it).
    if (LaunchParameters.PrintPeerId)
    {
        for (int i = 0; i < 30 && !IpfsWrapper::DaemonRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        LogOut("main.cpp", "PeerID: " + IpfsWrapper::PeerID());
        for (const std::string &A : IpfsWrapper::ListenAddrs()) LogOut("main.cpp", "addr: " + A);
        return 0;
    }

    //HEADLESS: fetch a single CID to a destination path then exit — a download throughput probe (any public/private
    //CID). Waits for the node's network to come up, times the transfer, and reports MB/s.
    if (!LaunchParameters.FetchCid.empty())
    {
        LogOut("main.cpp", "Fetch test: " + LaunchParameters.FetchCid + " -> " + LaunchParameters.FetchDest);
        // Warm up first: wait for a real peer set (or 30 s) so the timed fetch reflects a RUNNING node (the GUI),
        // not a cold start with 0 peers + an empty DHT routing table.
        for (int i = 0; i < 30 && IpfsWrapper::PeerCount() < 30; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        // Optional direct peering: dial a known seed so the transfer is peer-to-peer direct (no DHT/relay lottery).
        if (!LaunchParameters.ConnectAddr.empty())
        {
            const bool Ok = IpfsWrapper::Connect(LaunchParameters.ConnectAddr);
            LogOut("main.cpp", std::string("direct connect to ") + LaunchParameters.ConnectAddr + (Ok ? " : ok" : " : FAILED"));
        }
        LogOut("main.cpp", std::string("warmed: online=") + (IpfsWrapper::DaemonRunning() ? "yes" : "no")
               + " peers=" + std::to_string(IpfsWrapper::PeerCount()));
        const auto T0 = std::chrono::steady_clock::now();
        std::string Err;
        const std::string Got = IpfsWrapper::FetchToPath(LaunchParameters.FetchCid, LaunchParameters.FetchDest, &Err);
        const double Secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
        if (Got.empty()) { LogErr("main.cpp", "Fetch failed: " + Err); return 1; }
        std::error_code Ec; const auto Sz = std::filesystem::file_size(LaunchParameters.FetchDest, Ec);
        LogSucc("main.cpp", "Fetched " + std::to_string(Ec ? 0 : Sz) + " bytes in "
                + std::to_string(Secs) + "s  (" + std::to_string((Ec ? 0.0 : double(Sz)) / 1048576.0 / Secs) + " MB/s)");
        return 0;
    }

    //HEADLESS: seed a folder of published packages into the IPFS node (re-establish seeding from a master), then exit.
    if (!LaunchParameters.SeedDir.empty())
    {
        LogOut("main.cpp", std::string(LaunchParameters.SeedCoversOnly ? "Seeding COVERS ONLY from: " : "Seeding referenced content from: ")
                           + LaunchParameters.SeedDir);
        int Mismatched = 0;
        const int Seeded = PackageCatalog::SeedDirectory(LaunchParameters.SeedDir,
            [](int done, int total, const std::string &name){
                if (done == total || done % 10 == 0) LogOut("seed", std::to_string(done) + "/" + std::to_string(total) + "  " + name);
            }, &Mismatched, LaunchParameters.SeedCoversOnly, LaunchParameters.SeedOverwrite);
        LogSucc("main.cpp", "Seeded " + std::to_string(Seeded) + " file(s)"
                + (Mismatched ? ("; " + std::to_string(Mismatched) + " changed/un-seedable") : std::string()));
        return 0;
    }

    //HEADLESS: import a runner NODE (fetch its IPFS build + generate its DEFPREFIX artifact) then exit.
    if (!LaunchParameters.ImportRunnerId.empty())
    {
        //--import-runner <RUNNER_NODE_ID> (node runners have no variants — a stray ":suffix" is ignored).
        std::string Id = LaunchParameters.ImportRunnerId;
        if (auto Colon = Id.find(':'); Colon != std::string::npos) Id = Id.substr(0, Colon);
        LogOut("main.cpp", "Importing runner node: " + Id);
        std::vector<std::filesystem::path> Roots;
        for (const auto &D : PackageCatalog::RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
        NodeIndex Index = ManifestModel::BuildNodeIndex(Roots);
        std::string Err;
        const bool Ok = RunnerInstall::ImportRunnerNode(GlobalConfigJSON, Index, Id, &Err);
        LogOut("main.cpp", Ok ? "Runner imported." : ("Runner import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: hydrate a library game in place (fetch its node closure's IPFS content) then exit. Resolve the
    //launchable node whose UID (or NODE_ID) matches the requested id, then hydrate its content closure.
    if (!LaunchParameters.ImportPackageUid.empty())
    {
        const std::string Uid = LaunchParameters.ImportPackageUid;
        LogOut("main.cpp", "Importing package (node closure) for: " + Uid);
        std::vector<std::filesystem::path> Roots;
        for (const auto &D : PackageCatalog::RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
        NodeIndex Index = ManifestModel::BuildNodeIndex(Roots);
        std::string LaunchId;
        for (const auto &[NId, N] : Index.Nodes)
            if (N.IsLaunchable() && (N.Uid == Uid || N.NodeId == Uid)) { LaunchId = NId; break; }
        if (LaunchId.empty()) { LogErr("main.cpp", "No launchable node found for '" + Uid + "'."); return 1; }
        std::string Err;
        const bool Ok = PackageCatalog::HydrateNode(Index, LaunchId, {}, &Err);
        LogOut("main.cpp", Ok ? "Package imported." : ("Package import failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: publish (dehydrate) a local package — seed its layer content over IPFS, record the CIDs into the
    //manifest fragments in place, and optionally export a manifest-only copy — then exit.
    if (!LaunchParameters.PublishPackageDir.empty())
    {
        const std::string Dir  = LaunchParameters.PublishPackageDir;
        const std::string Dest = LaunchParameters.PublishToDir;
        LogOut("main.cpp", "Publishing package: " + Dir + (Dest.empty() ? "" : (" -> " + Dest)));
        std::string Err;
        const bool Ok = PackageCatalog::PublishPackage(Dir, Dest, &Err);
        LogOut("main.cpp", Ok ? "Package published." : ("Package publish failed: " + Err));
        return Ok ? 0 : 1;
    }

    //HEADLESS: validate the whole node graph (dangling/cyclic PARENTS, layer PATHs, runner resolution, ...).
    if (LaunchParameters.ValidateNodes)
    {
        std::vector<std::filesystem::path> Roots;
        for (const auto &D : PackageCatalog::RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
        NodeIndex Index = ManifestModel::BuildNodeIndex(Roots);
        std::vector<std::string> Errors, Warnings;
        ManifestModel::ValidateNodeGraph(Index, Errors, Warnings);
        for (const auto &W : Warnings) LogWarn("validate-nodes", W);
        for (const auto &E : Errors)   LogErr ("validate-nodes", E);
        LogOut("validate-nodes", "Validated " + std::to_string(Index.Nodes.size()) + " node(s): "
               + std::to_string(Errors.size()) + " error(s), " + std::to_string(Warnings.size()) + " warning(s).");
        return Errors.empty() ? 0 : 1;
    }

    //HEADLESS: print the presentable library tiles (grouped launchable nodes) + hydration status.
    if (LaunchParameters.ListNodes)
    {
        NodeIndex Index = PackageCatalog::BuildCatalogIndex(GlobalConfigJSON);
        auto Groups = PackageCatalog::PresentableGroups(Index);
        LogOut("list-nodes", std::to_string(Groups.size()) + " library tile(s):");
        for (const auto &G : Groups)
        {
            const Node *Rep = G.front();
            std::string Editions;
            for (const Node *N : G)
                Editions += " " + N->NodeId + (PackageCatalog::NodeHydrated(Index, N->NodeId) ? "[hydrated]" : "[remote]");
            LogOut("list-nodes", "  " + (Rep->Meta.is_object() ? Rep->Meta.value("TITLE", Rep->NodeId) : Rep->NodeId)
                   + "  (group " + Rep->GroupKey() + "):" + Editions);
        }
        return 0;
    }

    //HEADLESS (node graph): launch a launchable node from the global, cross-bundle node index — the engine
    //resolves EVERYTHING natively from the node graph (LaunchResolver::InitializeFromNode). No manifest.
    if (!LaunchParameters.LaunchNodeId.empty())
    {
        LogOut("main.cpp", "Launching node '" + LaunchParameters.LaunchNodeId + "' from the global node graph.");
        std::vector<std::filesystem::path> Roots;
        for (const auto &D : PackageCatalog::RepositoryDirs(GlobalConfigJSON)) Roots.emplace_back(D);
        NodeIndex Index = ManifestModel::BuildNodeIndex(Roots);
        if (!Index.Find(LaunchParameters.LaunchNodeId))
        { LogErr("main.cpp", "Node '" + LaunchParameters.LaunchNodeId + "' not found in the catalog, aborting."); return 1; }

        nlohmann::ordered_json MANIFESTJSON = nlohmann::ordered_json::object();   // engine fills this from the node graph
        struct ContainerParams NewContainerParams = ContainerParams(std::filesystem::path(), LaunchParameters.LaunchNodeId, std::string());
        NewContainerParams.NodeIdx           = &Index;
        NewContainerParams.LaunchNodeId      = LaunchParameters.LaunchNodeId;
        NewContainerParams.VariableOverrides = LaunchParameters.VariableOverrides;
        NewContainerParams.ModuleStates      = LaunchParameters.ModuleStates;
        NewContainerParams.VariantID         = "default";
        NewContainerParams.RunnerID          = LaunchParameters.RunnerID;
        class ContainerWrapper NewContainerWrapper = ContainerWrapper(GlobalConfigJSON, MANIFESTJSON, NewContainerParams);
        if (!LaunchResolver::ResolveExecutableDefinition(MANIFESTJSON, NewContainerWrapper.ContainerParams))
        { LogErr("main.cpp", "ResolveExecutableDefinition failed, aborting."); return 1; }
        if (LaunchParameters.ResolveOnly)
        {
            //Dump the resolved params (no mount, no game) for golden-compare / hang-free verification.
            //Under the app data dir so ALL VidyaGod-produced data lives in ~/.VidyaGod (a single delete = clean slate).
            const std::filesystem::path Out = std::filesystem::path(AppDataDir.absolutePath().toStdString())
                                              / ("vg_resolve_" + LaunchParameters.LaunchNodeId + ".json");
            std::ofstream OF(Out);
            OF << DumpResolution(NewContainerWrapper.ContainerParams).dump(2) << std::endl;
            LogSucc("main.cpp", "Resolved '" + LaunchParameters.LaunchNodeId + "' -> " + Out.string());
            return 0;
        }
        //Bail if the runtime couldn't be built (no compatible runner, unmountable/compressed layers, missing
        //dependencies, …). Without this the engine would proceed to Execute() an empty/garbage command and report
        //a misleading clean exit (code 0) for a launch that never actually ran.
        if (!NewContainerWrapper.BuildContainerRuntime())
        { LogErr("main.cpp", "Failed to build the container runtime for '" + LaunchParameters.LaunchNodeId
                 + "' — aborting launch (check the log above)."); NewContainerWrapper.Cleanup(); return 1; }
        NewContainerWrapper.Execute();
        if (NewContainerWrapper.LastCrashed || NewContainerWrapper.LastExitCode != 0)
            LogWarn("main.cpp", "Game did not exit cleanly (code " + std::to_string(NewContainerWrapper.LastExitCode) + ").");
        return NewContainerWrapper.LastCrashed ? 1 : NewContainerWrapper.LastExitCode;
    }

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
        for (const std::string &Dep : MissingDeps) Msg += "•  " + DependencyImportHint(Dep) + "\n\n";
        Msg += "You can still browse and manage your library; launching a game will fail until these are installed.";
        QMessageBox::warning(nullptr, "Missing dependencies", Msg);
    }

    //Create and launch MainWindow. Passes GlobalConfigJSON and AppDataDir by pointer so
    //the window can persist changes (add/remove packages, save settings) to disk.
    MainWindow MainWindow(&GlobalConfigJSON, &AppDataDir);
    MainWindow.startup(LaunchParameters.StartInTray);   // show(), or come up hidden if --tray / Start-in-tray / remembered
    return Application.exec();
}

//Checks for all external binaries that the VFS and runner subsystems depend on.
//Uses `which` via std::system() rather than QProcess to keep this path dependency-free.
//Returns true if any binary is missing, false if everything is present.
std::list<std::string> CheckExecutableDependencies()
{
    //The custom vidyagodfs FUSE filesystem replaces unionfs/fuse-zip/bindfs. fusermount(3) (part of
    //libfuse) still tears mounts down; umu-run is the default Wine runner.
    std::list<std::string> ExecutableDependencies = {"fusermount3", "umu-run"};
    std::list<std::string> Missing;
    for (const std::string& Binary : ExecutableDependencies) {
        std::string Command = "which " + Binary + " > /dev/null 2>&1";
        if (std::system(Command.c_str()) == 0)
            LogOut("main.cpp", Binary + " found on the system.");
        else
        {
            LogErr("main.cpp", Binary + " NOT FOUND ON THE SYSTEM, VFS WILL NOT WORK.");
            Missing.push_back(Binary);
        }
    }

    //vidyagodfs ships beside the binary (not on PATH); resolve via /proc/self/exe (no QApplication needed).
    std::error_code SelfEc;
    std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", SelfEc);
    std::filesystem::path Helper = SelfEc ? std::filesystem::path() : (Self.parent_path() / "vidyagodfs");
    if (!Helper.empty() && std::filesystem::exists(Helper)) LogOut("main.cpp", "vidyagodfs found at " + Helper.string());
    else { LogErr("main.cpp", "vidyagodfs NOT FOUND beside the binary, VFS WILL NOT WORK."); Missing.push_back("vidyagodfs"); }

    return Missing;
}

//Human-friendly install guidance for a missing dependency, shown in the GUI startup dialog.
static QString DependencyImportHint(const std::string &Dep)
{
    if (Dep == "fusermount3")
        return "fusermount3 — part of FUSE 3 (needed to mount game runtimes).\n"
               "    Arch: sudo pacman -S fuse3   •   Debian/Ubuntu: sudo apt install fuse3   •   Fedora: sudo dnf install fuse3";
    if (Dep == "umu-run")
        return "umu-run — the Wine/Proton game runner (umu-launcher).\n"
               "    Arch (AUR): umu-launcher   •   others: https://github.com/Open-Wine-Components/umu-launcher";
    if (Dep == "vidyagodfs")
        return "vidyagodfs — ships with VidyaGod, so your install looks incomplete.\n"
               "    Reinstall the AppImage (or rebuild — it must sit next to the VidyaGod binary).";
    return QString::fromStdString(Dep);
}

// The hosted repository of built-in runner packages (and other shared packages). Cloned into
// ~/.VidyaGod/LIBRARY/<repo> by PackageCatalog::SyncRepositories and indexed like any other repository.
static std::string DefaultRunnerRepoURL() { return "https://github.com/lorenzo-zurini/VidyaGodRunners.git"; }

//Guarantees the GlobalConfig has the shape the app actually uses, seeding any missing piece:
//  LIBRARY (array of packages), Settings (object), Settings.Repositories (the package catalog sources).
//Runners (and every other shared package) live as packages in the configured Repositories.
//Returns true if it added anything, so the caller can persist a freshly-seeded config.
//TODO(sharing): the LIBRARY is just the games-view of the catalog; the catalog itself is the union of
//every package across all Repositories (+ locally-added entries), globally cross-referenceable.
static bool EnsureGlobalConfigDefaults(nlohmann::ordered_json & gc)
{
    bool Changed = false;
    if (!gc.is_object())                                        { gc = nlohmann::ordered_json::object();             Changed = true; }
    if (!gc.contains("LIBRARY")  || !gc["LIBRARY"].is_array())  { gc["LIBRARY"]  = nlohmann::ordered_json::array();  Changed = true; }
    if (!gc.contains("Settings") || !gc["Settings"].is_object()){ gc["Settings"] = nlohmann::ordered_json::object(); Changed = true; }
    //Repositories: ordered list of git repos. Each: { NAME, PATH } where PATH is a clone URL, cloned
    //into ~/.VidyaGod/LIBRARY/<repo> and indexed. The sole default is the hosted VidyaGodRunners repository
    //(built-in runners + shared packages).
    if (!gc["Settings"].contains("Repositories") || !gc["Settings"]["Repositories"].is_array())
    {
        gc["Settings"]["Repositories"] = nlohmann::ordered_json::array({
            nlohmann::ordered_json{ {"NAME", "VidyaGodRunners"}, {"PATH", DefaultRunnerRepoURL()} } });
        Changed = true;
    }
    return Changed;
}

//Loads GlobalConfig.JSON from AppDataDir (or starts empty if it does not exist), then ensures the
//config has the shape the app uses via EnsureGlobalConfigDefaults. The file is (re)written when it
//was freshly created or when a missing top-level key had to be seeded.
//Returns true on FAILURE, false on SUCCESS — matches shell exit-code convention so
//callers can write `if (InitializeGlobalConfigJSON(...)) { /* handle error */ }`.
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir)
{
    QFile GlobalConfigFile(AppDataDir->filePath("GlobalConfig.JSON"));
    const bool Existed = GlobalConfigFile.exists();

    if (Existed && JSONOps::LoadJSON(&GlobalConfigFile, GlobalConfigJSON))
    {
        //LoadJSON returns non-zero on failure — don't silently overwrite a corrupt config.
        LogErr("main.cpp", "Failed to parse GlobalConfig.JSON, aborting.");
        return true; //fail
    }
    if (!Existed) LogOut("main.cpp", "Config file not detected. Creating defaults...");

    EnsureGlobalConfigDefaults(*GlobalConfigJSON);

    //Sync the configured Repositories: clone/pull each into ~/.VidyaGod/LIBRARY/<repo> (the clone IS the library)
    //and upsert the un-hydrated LIBRARY index. This mutates the config (so always persist afterwards), building a
    //full un-hydrated library of manifests that imports later hydrate in place beside each manifest.
    PackageCatalog::SyncRepositories(*GlobalConfigJSON);

    if (!JSONOps::SaveJSON(GlobalConfigJSON, &GlobalConfigFile))
    {
        LogErr("main.cpp", "GlobalConfig.JSON could not be written.");
        return true; //fail
    }

    LogSucc("main.cpp", "GlobalConfigJSON initialized successfully.");
    return false; //success
}

//Delegates to FSOps::CheckPackageValid to test whether CurrentPath contains MANIFEST.json.
bool IsRunningInPackageDir(std::filesystem::path CurrentPath)
{
    if (FSOps::CheckPackageValid(new QDir(CurrentPath))) //Convert FSOPS to stdlib! Remove unnecessary heap variable!
    {
        LogOut("main.cpp", "Running in PACKAGEDIR, running headless.");
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
        else if (arg == "--game" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessGameID = argv[++i];
        }
        else if (arg == "--component" && i + 1 < argc)
        {
            RuntimeParameters.HeadlessComponentID = argv[++i];
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
        else if (arg == "--fetch" && i + 2 < argc)
        {
            //Download-throughput probe: fetch a CID to a destination path then exit.
            RuntimeParameters.FetchCid         = argv[++i];
            RuntimeParameters.FetchDest        = argv[++i];
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--peer-id")
        {
            RuntimeParameters.PrintPeerId      = true;
            RuntimeParameters.RunningHeadless  = true;
        }
        else if (arg == "--tray")            // come up hidden in the tray (the start-on-login autostart entry uses this)
        {
            RuntimeParameters.StartInTray = true;
        }
        else if (arg == "--data-dir" && i + 1 < argc)
        {
            RuntimeParameters.DataDir          = argv[++i];
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
        else if (arg == "--variant" && i + 1 < argc)
        {
            RuntimeParameters.VariantID = argv[++i];
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
        else if (arg == "--runner" && i + 1 < argc)
        {
            RuntimeParameters.RunnerID = argv[++i];
        }
    }
    return RuntimeParameters;
}
