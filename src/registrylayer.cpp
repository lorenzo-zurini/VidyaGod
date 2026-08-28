#include "registrylayer.h"
#include "fileedits.h"       // FileEdits::ProcessFileEdits (base FileEdits → DEFAULTDATA)
#include "registrywrapper.h" // hive load/save/merge
#include "commonutils.h"     // Log*

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    const std::filesystem::path RegKeyStore = ContainerParams.UserDataPath / "REGKEYS";
    const bool HavePersistKeys = HavePrefix && !ContainerParams.PersistAll && !ContainerParams.KeepRegKeys.empty()
                                 && std::filesystem::exists(RegKeyStore);
    //Persisted whole-hive KEEPs (user.reg/system.reg) are UNION-merged into the composed hives here —
    //per-key, the user's saved value wins, base edits show through where the user has none. They were
    //previously whole-FILE seeded into the writelayer, which shadowed DEFAULTDATA's hives entirely: any
    //base RegEdit on a kept hive (e.g. an EULA-acceptance seed on HKCU) went invisible from the second
    //session on, because EVERY session's capture re-created a store lacking it.
    const std::filesystem::path RegHiveStore = ContainerParams.UserDataPath / "REGISTRY";
    const bool HaveHiveStore = HavePrefix && !ContainerParams.PersistAll && !ContainerParams.KeepRegHives.empty()
                               && std::filesystem::exists(RegHiveStore);

    if (!HaveBaseFileEdits && !HaveBaseReg && !HavePersistKeys && !HaveHiveStore) return true; // nothing to materialise

    bool Ok = true;
    std::error_code Ec;
    std::filesystem::create_directories(ContainerParams.DefaultDataPath, Ec);
    if (Ec)
    {
        LogErr("RegistryLayer::BuildDefaultData", "Cannot create DEFAULTDATA dir: " + Ec.message());
        return false;
    }

    //Base FileEdits → DEFAULTDATA (at their root-relative paths).
    if (HaveBaseFileEdits && !FileEdits::ProcessFileEdits(ContainerParams, /*OverridePass=*/false, ContainerParams.DefaultDataPath))
        Ok = false;

    //Base RegEdits + persisted reg-key subtrees → DEFAULTDATA hives, built from DEFPREFIX (never mutating it).
    if (HaveBaseReg || HavePersistKeys || HaveHiveStore)
    {
        //No wineserver quiesce needed: the installed artifact leaves DEFPREFIX quiescent,
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
        //HKLM overlay from default_pfx. For a node-declared zero-copy prefix there is no DEFPREFIX, so the base loaded
        //above is EMPTY and the saved system.reg (base RegEdits only) SHADOWS default_pfx's full HKLM. wine.inf writes
        //~thousands of static, machine-independent tables that games read at startup — the COM class registry
        //(Software\Classes → CoCreateInstance of builtin dsound/quartz/shell classes), the ACM codec DriverCache, MCI
        //Extensions/MCI32 (intro-video device resolution), DirectPlay Service Providers (the multiplayer connect list),
        //the DirectX version stamp (dsetup.dll's DirectXSetupGetVersion → MechWarrior 4's "DirectX error"), the service
        //table (RpcSs/MountMgr/…), and every future one. Rather than cherry-pick each broken title's key (this was
        //seven separate carve-outs and growing), overlay the WHOLE HKLM\System + HKLM\Software.
        //
        //HISTORY: this used to be minimal-with-carve-outs because a full overlay was once blamed for regressing NFS
        //Underground 2 (exit-5 / c0000005 during volume enumeration). Re-tested 2026-08-28 on the real GPU: the whole
        //System+Software overlay renders NFS U2 fine. That crash was actually fixed by the mountmgr symlink repair +
        //MountMgr service registry (ff3a4dd) and the .update-timestamp "disable" freeze below (stops wineboot re-running
        //wine.inf mid-launch) — NOT by keeping the registry minimal. The device-state caution (Enum\*/Class\*) outlived
        //the bug it worked around. Both registry views come along for free (32-bit games read Wow6432Node).
        if (auto It = ContainerParams.CustomVariables.find("DefaultPfxDir");
            It != ContainerParams.CustomVariables.end() && !It->second.empty()
            && std::filesystem::exists(std::filesystem::path(It->second)))
        {
            RegistryWrapper Dpfx;
            Dpfx.LoadPrefix(It->second);                                            // default_pfx hives at its root
            RW.MergeKeyFrom(Dpfx, "HKLM\\System");
            RW.MergeKeyFrom(Dpfx, "HKLM\\Software");
            //DEBUG aid: extra default_pfx subtrees to merge, ';'-separated (bisecting registry-dependent crashes)
            if (const char *Extra = std::getenv("VG_REG_MERGE_EXTRA"))
            {
                std::stringstream Ss(Extra);
                for (std::string Key; std::getline(Ss, Key, ';');)
                    if (!Key.empty())
                        LogOut("RegistryLayer::BuildDefaultData", "VG_REG_MERGE_EXTRA " + Key + " -> "
                               + (RW.MergeKeyFrom(Dpfx, Key) ? "merged" : "MISS"));
            }
            //Freeze wine's implicit prefix update: the template's .update-timestamp carries the ORIGINAL proton
            //build's wine.inf mtime, which never matches the delta-chain mount's mtimes → wineboot decides the
            //prefix is stale and re-runs its update ON EVERY COLD LAUNCH, racing the game (services like mountmgr
            //come up mid-flight; with pristine-by-default persistence every launch is cold). "disable" is wineboot's
            //documented sentinel for "never update". Written into DEFAULTDATA so it overlays the template's file.
            const std::filesystem::path TsDir = HiveDir(ContainerParams, ContainerParams.DefaultDataPath);
            std::filesystem::create_directories(TsDir, Ec);
            std::ofstream Ts(TsDir / ".update-timestamp", std::ios::trunc);
            Ts << "disable\n";
            //NOTE: the template's ESCAPING relative symlinks (drivers/mountmgr.sys →
            //../../../../../../lib/wine/…, valid only in proton's own tree) are handled by vidyagodfs's
            //symlink abolition: archive links are chased to their in-archive target and served as REGULAR
            //files, so the driver loads without any symlink existing anywhere. No re-anchoring needed here.
        }
        if (HaveHiveStore)
        {
            RegistryWrapper Durable;
            Durable.LoadPrefix(RegHiveStore);
            for (const std::string &Name : ContainerParams.KeepRegHives)
            {
                const char *Root = Name == "user.reg"   ? "HKCU"
                                 : Name == "system.reg" ? "HKLM" : nullptr;   // userdef.reg keeps the writelayer-seed path
                if (Root && RW.UnionMergeFrom(Durable, Root))
                    LogOut("RegistryLayer::BuildDefaultData", "Union-merged persisted " + Name + " over the composed hive");
            }
        }
        const std::filesystem::path HiveOut = HiveDir(ContainerParams, ContainerParams.DefaultDataPath);
        std::filesystem::create_directories(HiveOut, Ec);
        if (!RW.SavePrefix(HiveOut))                                                // mounts at root → lands at /<PrefixRoot>
        {
            LogErr("RegistryLayer::BuildDefaultData", "Failed to write DEFAULTDATA hives.");
            Ok = false;
        }
    }
    if (Ok) LogSucc("RegistryLayer::BuildDefaultData", "DEFAULTDATA layer built at " + ContainerParams.DefaultDataPath.string());
    return Ok;
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


//Seeds previously-persisted KEEP hive files from UserDataPath/REGISTRY/ into the ephemeral
//WRITELAYER before the union mounts, so they shadow the DEFPREFIX base. Wine always writes the
//complete file, so a whole persisted reg file is correct (no stripped-delta shadowing). Only the
//KEEP-declared hives (KeepRegHives — e.g. just user.reg for a "KEEP HKCU") are seeded.
bool RegistryLayer::SeedPersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the reg files
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "REGISTRY";
    //Shadow at the prefix root in the union (/<PrefixRoot>/*.reg) — "" for wine, "pfx" for proton.
    const std::filesystem::path WriteHives = HiveDir(ContainerParams, ContainerParams.WriteLayerPath);
    std::error_code ec;
    std::filesystem::create_directories(WriteHives, ec);
    bool Ok = !ec;
    for (const std::string &Name : ContainerParams.KeepRegHives)
    {
        const std::filesystem::path SrcReg = RegStore / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        //Already union-merged into DEFAULTDATA (BuildDefaultData)? Then a whole-file writelayer seed would
        //SHADOW that composed hive and erase every base edit — skip; the merged copy carries the user state.
        if (std::filesystem::exists(HiveDir(ContainerParams, ContainerParams.DefaultDataPath) / Name))
        { LogOut("RegistryLayer::SeedPersistRegistry", Name + " union-merged into DEFAULTDATA — writelayer seed skipped"); continue; }
        const std::filesystem::path DstReg = WriteHives / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("RegistryLayer::SeedPersistRegistry", "Could not seed " + Name + ": " + ec.message()); Ok = false; }
        else    LogOut("RegistryLayer::SeedPersistRegistry", "Seeded persisted " + Name);
    }
    return Ok;
}

//Captures the session's KEEP hives by copying RuntimePath/<prefixroot>/<hive>.reg into UserDataPath/REGISTRY/.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped. Bounded copy of small metadata files.
bool RegistryLayer::CapturePersistRegistry(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //already durable
    const std::filesystem::path RegStore = ContainerParams.UserDataPath / "REGISTRY";
    std::error_code ec;
    std::filesystem::create_directories(RegStore, ec);
    bool Ok = !ec;
    const std::filesystem::path RunHives = HiveDir(ContainerParams, ContainerParams.RuntimePath);
    for (const std::string &Name : ContainerParams.KeepRegHives)
    {
        const std::filesystem::path SrcReg = RunHives / Name;
        if (!std::filesystem::exists(SrcReg)) continue;
        const std::filesystem::path DstReg = RegStore / Name;
        std::filesystem::copy_file(SrcReg, DstReg, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("RegistryLayer::CapturePersistRegistry", "Could not capture " + Name + ": " + ec.message()); Ok = false; }
        else    LogOut("RegistryLayer::CapturePersistRegistry", "Captured " + Name);
    }
    return Ok;
}

//Extracts each KEEP registry-subtree from the mounted RuntimePath hives and merges it into the
//durable store UserDataPath/REGKEYS (partial hive files holding only the persisted keys). Runs
//during Cleanup BEFORE unmount. A key absent from the session (never created) is left as-is in the
//store rather than dropped. No-op when PersistAll.
bool RegistryLayer::CapturePersistRegKeys(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll || ContainerParams.KeepRegKeys.empty()) return true;
    const std::filesystem::path Store = ContainerParams.UserDataPath / "REGKEYS";

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
        {
            LogErr("RegistryLayer::CapturePersistRegKeys", "Failed to write durable REGKEYS store.");
            return false;
        }
    }
    return true;
}