#include "persistlayer.h"
#include "commonutils.h"   // Log*

#include <filesystem>
#include <string>

//Seeds each KEEP file from its durable home (UserDataPath/<rel>) into WriteLayerPath/<rel> before
//the union mounts, so the persisted file shadows the lower read-only layers. Single files are
//copied (not unioned live): a file layer can't merge, and copy mirrors the registry model.
//No-op under MODE:all (the durable RW branch already holds everything) or when nothing is stored yet.
bool PersistLayer::SeedPersistFiles(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //durable RW branch already holds the files
    std::error_code ec;
    for (const std::string &Rel : ContainerParams.KeepFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.UserDataPath  / Rel; //durable source (in package)
        if (!std::filesystem::exists(SrcFile)) continue;                            //nothing persisted yet
        const std::filesystem::path DstFile = ContainerParams.WriteLayerPath / Rel; //shadow in the RW top layer
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + Rel + ": " + ec.message());
        else    LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + Rel);
    }
    return true;
}

//Captures each KEEP file by copying RuntimePath/<rel> into its durable home UserDataPath/<rel>.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped, mirroring CapturePersistRegistry.
//No-op under MODE:all.
bool PersistLayer::CapturePersistFiles(struct ContainerParams &ContainerParams)
{
    if (ContainerParams.PersistAll) return true; //already durable
    std::error_code ec;
    for (const std::string &Rel : ContainerParams.KeepFiles)
    {
        const std::filesystem::path SrcFile = ContainerParams.RuntimePath  / Rel; //session result in the mounted union
        if (!std::filesystem::exists(SrcFile)) continue;                          //game never created it
        const std::filesystem::path DstFile = ContainerParams.UserDataPath / Rel; //durable home (in package)
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + Rel + ": " + ec.message());
        else    LogOut("PersistLayer::CapturePersistFiles", "Captured file " + Rel);
    }
    return true;
}