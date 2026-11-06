#include "launchsession.h"
#include "apppaths.h"
#include "commonutils.h"     // QuietLog, Log*
#include "instancestore.h"   // the instance's config the values read back go to
#include "launchresolver.h"
#include "optionreadback.h"
#include "vfsmount.h"        // SessionOwned — no read-back while the instance runs

bool LaunchSession::ResolveTakingBack(ContainerParams &CP, const nlohmann::ordered_json &Config, bool Reresolve)
{
    if (!AppPaths::UserDataPathOverride().empty()) return LaunchResolver::InitializeFromNode(CP, Config);
    const ContainerParams Input = CP;
    bool Looked = false;
    {
        QuietLog Quiet;
        Looked = LaunchResolver::InitializeFromNode(CP, Config);
    }
    if (Looked && !VfsMount::SessionOwned(CP.TempPath))
    {
        const auto Found = OptionReadBack::Read(CP);
        nlohmann::ordered_json Instance = Found.empty() ? nlohmann::ordered_json()
                                        : InstanceStore::ReadConfig(Config, CP.PackageUID, CP.InstanceName);
        bool Touched = false;
        const std::vector<std::string> Changed = Found.empty() ? std::vector<std::string>()
                                               : OptionReadBack::Adopt(Instance, Found, OptionReadBack::Current(CP), Touched);
        std::string Err;
        if (Touched && !InstanceStore::WriteConfig(Config, CP.PackageUID, CP.InstanceName, Instance, &Err))
            LogErr("LaunchSession", "could not store the options read back from the game's files (" + Err + ")");
        else if (!Changed.empty())
        {
            std::string List;
            for (const std::string &K : Changed) List += (List.empty() ? "" : ", ") + K + " = " + Instance["VARIABLES"][K].dump();
            LogOut("LaunchSession", "Changed in the game since the last launch, read back from its files: " + List);
        }
    }
    if (Looked && !Reresolve) return true;
    CP = Input;
    return LaunchResolver::InitializeFromNode(CP, Config);
}
