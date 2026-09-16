#include "persistlayer.h"
#include "commonutils.h"   // Log*

#include <filesystem>
#include <string>

//Seeds each single-file persist from its durable home (UserDataPath/<Target>) into WriteLayerPath/<Path> before
//the union mounts, so the persisted file shadows the lower read-only layers. Single files are copied (not unioned
//live): a file layer can't merge, and copy mirrors the registry model. Durable name (Target) is decoupled from the
//runtime location (Path). No-op when nothing is stored yet.
bool PersistLayer::SeedPersistFiles(struct ContainerParams &ContainerParams)
{
    std::error_code ec;
    bool Ok = true;
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.UserDataPath  / F.Target; //durable source
        if (!std::filesystem::exists(SrcFile)) continue;                                 //nothing persisted yet
        const std::filesystem::path DstFile = ContainerParams.WriteLayerPath / F.Path;   //shadow at the runtime location
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + F.Path + ": " + ec.message()); Ok = false; }
        else    LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + F.Path + " (from " + F.Target + ")");
    }
    return Ok;
}

//Captures each single-file persist by copying RuntimePath/<Path> into its durable home UserDataPath/<Target>.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped, mirroring CapturePersistRegistry.
bool PersistLayer::CapturePersistFiles(struct ContainerParams &ContainerParams)
{
    std::error_code ec;
    bool Ok = true;
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.RuntimePath  / F.Path;   //session result in the mounted union
        if (!std::filesystem::exists(SrcFile)) continue;                                //game never created it
        const std::filesystem::path DstFile = ContainerParams.UserDataPath / F.Target; //durable home
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + F.Path + ": " + ec.message()); Ok = false; }
        else    LogOut("PersistLayer::CapturePersistFiles", "Captured file " + F.Path + " (to " + F.Target + ")");
    }
    return Ok;
}