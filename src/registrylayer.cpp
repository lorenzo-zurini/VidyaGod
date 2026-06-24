#include "registrylayer.h"
#include "fileedits.h"       // FileEdits::ProcessFileEdits (base FileEdits → DEFAULTDATA)
#include "varsubst.h"        // VarSubst::StringVariableSubstitution (wineboot arg/env expansion)
#include "registrywrapper.h" // hive load/save/merge
#include "processenv.h"      // SystemToolEnv (system runner, not AppImage libs)
#include "commonutils.h"     // Log*

#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

//The registry-hive directory under a layer/runtime root: the prefix root (CONTENT_ROOT up to /drive_c) is
//where system.reg/user.reg/userdef.reg live. "" for plain wine (hives at the root), "pfx" for proton.
static std::filesystem::path HiveDir(const struct ContainerParams &CP, const std::filesystem::path &Base)
{
    return CP.PrefixRoot.empty() ? Base : Base / CP.PrefixRoot;
}

//Returns true if SubComponentsArray contains at least one RegEdit whose OVERRIDE flag == WantOverride.
static bool HasRegEdits(const struct ContainerParams &ContainerParams, bool WantOverride)
{
    for (const auto &Sub : ContainerParams.SubComponentsArray)
        if (Sub.value("TYPE", std::string()) == "RegEdit" && Sub.value("OVERRIDE", false) == WantOverride)
            return true;
    return false;
}

//Builds the DEFAULTDATA layer: a dedicated, regenerated-each-launch read-only layer that holds every
//package-encoded BASE (non-OVERRIDE) edit. It mounts between the component layers and the WRITELAYER
//(see BuildLayerSpec), so its edits override the package's own content but the user's persisted writes
//(WRITELAYER) shadow them in turn — PortableApps-style "factory defaults under live user data".
//
//Contents:
//  - Base FileEdits  → files at their root-relative paths under DEFAULTDATA. Applied for ANY runner type
//                      (Wine: drive_c/…; emulator/native: the runtime root).
//  - Base RegEdits   → full user.reg/system.reg/userdef.reg = DEFPREFIX hives + edits (shadow DEFPREFIX's;
//                      vidyagodfs COWs from this highest layer when Wine writes the registry). WINE-ONLY.
//  - Persisted KEEP registry-subtrees merged in AFTER base edits, so the user's saved key state wins. WINE-ONLY.
//
//DEFPREFIX itself is NEVER mutated (read-only source for the hives) — pristine for both runner models.
//OVERRIDE edits are NOT handled here; they go post-mount straight to the runtime (COW → WRITELAYER).
//Creates nothing when there are no base edits, so edit-less packages get no (empty) DEFAULTDATA layer.
//MUST BE RUN AFTER VARIABLE SUBSTITUTION (BuildSubComponentsArray) and after DEFPREFIX is provisioned.
bool RegistryLayer::BuildDefaultData(struct ContainerParams &ContainerParams)
{
    const bool HavePrefix = ContainerParams.PrefixGenerate;

    bool HaveBaseFileEdits = false;
    for (const auto &S : ContainerParams.SubComponentsArray)
        if (S.value("TYPE", std::string()) == "FileEdit" && S.value("OVERRIDE", false) == false)
            { HaveBaseFileEdits = true; break; }

    //Registry only exists when the runner generates a wine prefix; ROM/native runners have no hives.
    const bool HaveBaseReg = HavePrefix && HasRegEdits(ContainerParams, /*WantOverride=*/false);
    const std::filesystem::path RegKeyStore = ContainerParams.UserDataPath / "__REGKEYS__";
    const bool HavePersistKeys = HavePrefix && !ContainerParams.PersistAll && !ContainerParams.KeepRegKeys.empty()
                                 && std::filesystem::exists(RegKeyStore);

    if (!HaveBaseFileEdits && !HaveBaseReg && !HavePersistKeys) return true; // nothing to materialise

    std::error_code Ec;
    std::filesystem::create_directories(ContainerParams.DefaultDataPath, Ec);

    //Base FileEdits → DEFAULTDATA (at their root-relative paths).
    if (HaveBaseFileEdits)
        FileEdits::ProcessFileEdits(ContainerParams, /*OverridePass=*/false, ContainerParams.DefaultDataPath);

    //Base RegEdits + persisted reg-key subtrees → DEFAULTDATA hives, built from DEFPREFIX (never mutating it).
    if (HaveBaseReg || HavePersistKeys)
    {
        //No wineserver quiesce needed: InitializeDefPrefix / the installed artifact leave DEFPREFIX quiescent,
        //and we only READ its hives here — the edited copies are written into DEFAULTDATA.
        RegistryWrapper RW;
        RW.LoadPrefix(HiveDir(ContainerParams, ContainerParams.DefPrefixPath));    // hives at <artifact>/<PrefixRoot>
        if (HaveBaseReg)
            RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/false);
        if (HavePersistKeys)
        {
            RegistryWrapper Durable;
            Durable.LoadPrefix(RegKeyStore);
            for (const std::string &RegPath : ContainerParams.KeepRegKeys)
                if (RW.MergeKeyFrom(Durable, RegPath))
                    LogOut("RegistryLayer::BuildDefaultData", "Seeded persisted key " + RegPath);
        }
        const std::filesystem::path HiveOut = HiveDir(ContainerParams, ContainerParams.DefaultDataPath);
        std::filesystem::create_directories(HiveOut, Ec);
        if (!RW.SavePrefix(HiveOut))                                                // mounts at root → lands at /<PrefixRoot>
            LogWarn("RegistryLayer::BuildDefaultData", "Failed to write DEFAULTDATA hives.");
    }
    LogSucc("RegistryLayer::BuildDefaultData", "DEFAULTDATA layer built at " + ContainerParams.DefaultDataPath.string());
    return true;
}

//Applies OVERRIDE:true RegEdit subcomponents into the mounted runtime hives, post-MountVFS. Loading
//RuntimePath reads the effective union view (DEFPREFIX shadowed by any persisted seed); saving it
//back COWs the whole hive file into the RW WRITELAYER (file-level union shadowing), so the override
//values win over DEFPREFIX and prior COW state — matching the former `reg import` outcome. No wine
//runs on RuntimePath before launch, so no wineserver quiesce is needed here.
bool RegistryLayer::ApplyOverrideRegEdits(struct ContainerParams &ContainerParams)
{
    if (!HasRegEdits(ContainerParams, /*WantOverride=*/true)) return true; // no overrides → leave hives untouched

    LogOut("RegistryLayer::ApplyOverrideRegEdits", "Applying OVERRIDE RegEdits to mounted runtime hives.");

    const std::filesystem::path Hives = HiveDir(ContainerParams, ContainerParams.RuntimePath);
    RegistryWrapper RW;
    RW.LoadPrefix(Hives);
    RW.ApplyRegEdits(ContainerParams.SubComponentsArray, /*WantOverride=*/true);
    if (!RW.SavePrefix(Hives))
    {
        LogErr("RegistryLayer::ApplyOverrideRegEdits", "Failed to write runtime hives.");
        return false;
    }
    LogSucc("RegistryLayer::ApplyOverrideRegEdits", "OVERRIDE RegEdits applied to runtime.");
    return true;
}

//Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner.
//Exclusive for wine / proton runners.
//Must be made to work with any runner, not just UMU...
//
//After a successful wineboot, DefPrefixPath becomes the base (lowest-priority) read-only layer of the
//vidyagodfs spec (see BuildLayerSpec), so the prefix's drive_c structure shows at the runtime root.
//The prefix must be initialized before any game layers are stacked on top.
bool RegistryLayer::InitializeDefPrefix(struct ContainerParams &ContainerParams)
{
    //Build the base wine prefix by running `wineboot` through the runner. Fully manifest-driven: the umu vs
    //proton difference is just the runner's EXECUTABLE/ARGS/ENV plus the prefix layout. The runner ENV points
    //its prefix var (WINEPREFIX / STEAM_COMPAT_DATA_PATH) at %RuntimePath%; during init we bind %RuntimePath%
    //to the DEF-prefix dir we're building, so the wineboot lands in DefPrefixPath (mounted whole as base later).
    LogOut("RegistryLayer::InitializeDefPrefix", "Initialising DefPrefix, path: " + ContainerParams.DefPrefixPath.string());
    std::filesystem::create_directories(ContainerParams.DefPrefixPath);

    std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();
    Vars["RuntimePath"] = ContainerParams.DefPrefixPath.string();

    //EXECUTABLE may reference %RunnerRuntimePath% (e.g. the Proton build's proton script).
    std::string Program = ContainerParams.RunnerExecutable;
    VarSubst::StringVariableSubstitution(Program, Vars);

    QProcessEnvironment RunProcessEnvironment = SystemToolEnv();                 // system runner, not AppImage libs
    QProcess * RunProcess = new QProcess;
    RunProcess->setProgram(QString::fromStdString(Program));

    //Args: the runner's ARGS with the content target swapped for "wineboot" — the launcher verb (e.g. proton's
    //"waitforexitandrun"; empty for umu/wine) is kept, the content-bearing arg dropped, then wineboot appended.
    QStringList Arguments;
    for (const std::string &Raw : ContainerParams.RunnerArgs)
    {
        if (ArgReferencesContent(Raw)) continue;
        std::string Arg = Raw;
        VarSubst::StringVariableSubstitution(Arg, Vars);
        Arguments.append(QString::fromStdString(Arg));
    }
    Arguments.append("wineboot");
    RunProcess->setArguments(Arguments);

    //Env: runner REMOVE_ENV then the runner ENV block (with %var% expansion), exactly as at launch.
    for (const std::string &Key : ContainerParams.RunnerRemoveEnv)
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    for (auto &[Key, Value] : ContainerParams.RunnerEnv.items())
    {
        std::string ExpandedValue = Value.get<std::string>();
        VarSubst::StringVariableSubstitution(ExpandedValue, Vars);
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(ExpandedValue));
    }

    LogOut("RegistryLayer::InitializeDefPrefix", "wineboot: " + Program + " " + Arguments.join(' ').toStdString());
    RunProcess->setProcessEnvironment(RunProcessEnvironment);
    RunProcess->start();
    RunProcess->waitForFinished(-1);

    std::cout << RunProcess->readAllStandardError().toStdString() << std::endl;
    std::cout << RunProcess->readAllStandardOutput().toStdString() << std::endl;

    if (RunProcess->exitCode() == 0)
    {
        delete RunProcess;
        //The prefix is added to the layer-spec as the base (root) layer by BuildLayerSpec.
        LogSucc("RegistryLayer::InitializeDefPrefix", "Prefix initialisation successful!");
        return true;
    }
    else
    {
        delete RunProcess;
        LogErr("RegistryLayer::InitializeDefPrefix", "Prefix initialisation failed!");
        return false;
    }
}

//Seeds previously-persisted KEEP hive files from UserDataPath/__REGISTRY__/ into the ephemeral
//WRITELAYER before the union mounts, so they shadow the DEFPREFIX base. Wine always writes the
//complete file, so a whole persisted reg file is correct (no stripped-delta shadowing). Only the
//KEEP-declared hives (KeepRegHives — e.g. just user.reg for a "KEEP HKCU") are seeded.
bool RegistryLayer::SeedPersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the reg files
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "__REGISTRY__";
    //Shadow at the prefix root in the union (/<PrefixRoot>/*.reg) — "" for wine, "pfx" for proton.
    const std::filesystem::path WriteHives = HiveDir(ContainerParams, ContainerParams.WriteLayerPath);
    std::error_code ec;
    std::filesystem::create_directories(WriteHives, ec);
    for (const std::string &Name : ContainerParams.KeepRegHives)
    {
        const std::filesystem::path SrcReg = RegStore / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = WriteHives / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("RegistryLayer::SeedPersistRegistry", "Could not seed " + Name + ": " + ec.message());
        else    LogOut("RegistryLayer::SeedPersistRegistry", "Seeded persisted " + Name);
    }
    return true;
}

//Captures the session's KEEP hives by copying RuntimePath/<prefixroot>/<hive>.reg into UserDataPath/__REGISTRY__/.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped. Bounded copy of small metadata files.
bool RegistryLayer::CapturePersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //already durable
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "__REGISTRY__";
    std::error_code ec;
    std::filesystem::create_directories(RegStore, ec);
    const std::filesystem::path RunHives = HiveDir(ContainerParams, ContainerParams.RuntimePath);
    for (const std::string &Name : ContainerParams.KeepRegHives)
    {
        const std::filesystem::path SrcReg = RunHives / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = RegStore / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("RegistryLayer::CapturePersistRegistry", "Could not capture " + Name + ": " + ec.message());
        else    LogOut("RegistryLayer::CapturePersistRegistry", "Captured " + Name);
    }
    return true;
}

//Extracts each KEEP registry-subtree from the mounted RuntimePath hives and merges it into the
//durable store UserDataPath/__REGKEYS__ (partial hive files holding only the persisted keys). Runs
//during Cleanup BEFORE unmount. A key absent from the session (never created) is left as-is in the
//store rather than dropped. No-op under MODE:all.
bool RegistryLayer::CapturePersistRegKeys(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll || ContainerParams.KeepRegKeys.empty()) return true;
    const std::filesystem::path Store = ContainerParams.UserDataPath / "__REGKEYS__";

    RegistryWrapper Session;
    Session.LoadPrefix(HiveDir(ContainerParams, ContainerParams.RuntimePath));

    RegistryWrapper Durable;
    if (std::filesystem::exists(Store)) Durable.LoadPrefix(Store); // accumulate across sessions

    int Captured = 0;
    for (const std::string &RegPath : ContainerParams.KeepRegKeys)
        if (Durable.MergeKeyFrom(Session, RegPath)) { LogOut("RegistryLayer::CapturePersistRegKeys", "Captured " + RegPath); ++Captured; }
        else LogOut("RegistryLayer::CapturePersistRegKeys", "Key absent in session (kept prior): " + RegPath);

    if (Captured > 0)
    {
        std::error_code ec;
        std::filesystem::create_directories(Store, ec);
        if (!Durable.SavePrefix(Store))
            LogWarn("RegistryLayer::CapturePersistRegKeys", "Failed to write durable __REGKEYS__ store.");
    }
    return true;
}