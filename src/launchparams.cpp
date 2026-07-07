#include "launchparams.h"
#include "commonutils.h"   // LogOut

//Minimal constructor — stores only the three PASSED values; everything else is derived later
//by the LaunchResolver pipeline once the node graph and GlobalConfig are available.
ContainerParams::ContainerParams(std::filesystem::path Passed_PackagePath, std::string Passed_subgame_id, std::string Passed_component_id)
    : PackagePath(Passed_PackagePath), subgame_id(Passed_subgame_id), component_id(Passed_component_id)
{
    LogOut("ContainerParams::ContainerParams", "ContainerParams object created...");
}

std::map<std::string, std::string> ContainerParams::GetVariablesMap()
{
    std::map<std::string, std::string> VariablesMap;
    VariablesMap["PackagePath"] = this->PackagePath;
    VariablesMap["PackageName"] = this->PackageName;
    VariablesMap["PackageUID"] = this->PackageUID;
    VariablesMap["GameName"] = this->GameName;
    VariablesMap["UMUID"] = this->UMUID;
    VariablesMap["ScreenWidth"] = this->ScreenWidth;
    VariablesMap["ScreenHeight"] = this->ScreenHeight;
    //The single mount root — where the VFS mounts; STEAM_COMPAT_DATA_PATH / WINEPREFIX point here.
    VariablesMap["RuntimePath"] = this->RuntimePath;
    VariablesMap["RunnerRuntimePath"] = this->RunnerRuntimePath;
    //The runner build's mount (installed-runner model); unified runners run from inside the game RUNTIME.
    VariablesMap["RunnerMount"] = this->UnifiedRuntime ? this->RuntimePath.string() : this->RunnerMountPath.string();
    VariablesMap["WriteLayerPath"] = this->WriteLayerPath;
    VariablesMap["UserDataPath"] = this->UserDataPath;
    VariablesMap["TempPath"] = this->TempPath;
    VariablesMap["ProgramPath"] = this->ProgramPath;
    VariablesMap["DefPrefixPath"] = this->DefPrefixPath;
    VariablesMap["DefaultData"] = this->DefaultDataPath;
    //Content primitives: %ContentPath% (relative to the program mount) and %Content% (absolute host path).
    //Authors compose guest paths from these, e.g. ARGS: "C:\\%PackageUID%\\%ContentPath%".
    VariablesMap["ContentPath"] = this->ExePathRelative;
    VariablesMap["Content"] = this->ExePathComplete;
    //%ContentDir% — the exe's directory RELATIVE to the content root ("" when the exe is at the root). Lets a reusable
    //content node (e.g. a generic DirectPlay package) drop its files next to whatever game's exe via a layer TARGET of
    //"%ContentDir%". Emitted ONLY once the exe is resolved (ExePathRelative set): while it's still unresolved (e.g.
    //during BuildSubComponentsArray, which substitutes layer TARGETs BEFORE the exe is picked) the token is left in the
    //map's absence, so varsubst leaves "%ContentDir%" untouched and it survives to mount-time substitution
    //(BuildLayerSpec) where the exe IS known — otherwise it would bake to "" too early. Robust to '/'- or '\\'-CONTENTPATHs.
    if (!this->ExePathRelative.empty())
    {
        std::string Cp = this->ExePathRelative.generic_string();
        for (char & c : Cp) if (c == '\\') c = '/';
        const auto S = Cp.find_last_of('/');
        VariablesMap["ContentDir"] = (S == std::string::npos) ? std::string() : Cp.substr(0, S);
    }
    VariablesMap["WorkDirPathRelative"] = this->WorkDirPathRelative;
    VariablesMap["WorkDirPathComplete"] = this->WorkDirPathComplete;
    //Custom variables are appended last; they can shadow built-in names if needed.
    for (auto &[Key, Value] : this->CustomVariables)
        VariablesMap[Key] = Value;
    return VariablesMap;
}
