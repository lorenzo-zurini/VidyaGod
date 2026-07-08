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
    //MINIMAL variable set (deliberate — see the format spec's variables chapter). Author-facing tokens are ONLY the
    //ones NOT composable from other tokens. Every path INSIDE the runtime/prefix is composed explicitly by the author
    //from the structural anchor %PrefixRoot% ("" wine-at-root / "pfx" proton) + %PackageUID% + the known guest layout,
    //e.g. content root = "%PrefixRoot%/drive_c/%PackageUID%", system dir = "%PrefixRoot%/drive_c/windows/syswow64".
    //So the old DERIVED path tokens (ProgramPath / ContentDir / WorkDir* / UserDataPath / WriteLayerPath / DefPrefixPath
    /// DefaultData / PackagePath / RunnerRuntimePath) were redundant and were dropped. What remains:
    //  · non-path scalars                       — identity/metadata, not composable
    //  · %PrefixRoot%                           — the one mount-relative placement anchor
    //  · host-absolute ROOTS                    — RuntimePath/RunnerMount/TempPath: real host paths a runner's ENV /
    //                                             EXECUTABLE need; can't be composed from mount-relative primitives
    //  · exe locator (ContentPath/Content)      — the resolved per-variant CONTENTPATH; feeds the runner (guest-path
    //                                             templating via %REL%, and direct exec by native runners)
    //%REL% is injected by the guest-path templater (LaunchResolver), not here. CustomVariables are appended last and
    //may shadow a built-in.
    std::map<std::string, std::string> VariablesMap;
    //Non-path scalars
    VariablesMap["PackageUID"]   = this->PackageUID;
    VariablesMap["PackageName"]  = this->PackageName;
    VariablesMap["GameName"]     = this->GameName;
    VariablesMap["UMUID"]        = this->UMUID;
    VariablesMap["ScreenWidth"]  = this->ScreenWidth;
    VariablesMap["ScreenHeight"] = this->ScreenHeight;
    //Structural placement anchor — the prefix dir relative to the VFS root. All inside-prefix placement composes off it.
    VariablesMap["PrefixRoot"]   = this->PrefixRoot;
    //Host-absolute roots (not composable from mount-relative tokens) — consumed by runner ENV/EXECUTABLE.
    VariablesMap["RuntimePath"]  = this->RuntimePath;   // the VFS mount root as a host path (WINEPREFIX / STEAM_COMPAT_DATA_PATH)
    VariablesMap["RunnerMount"]  = this->UnifiedRuntime ? this->RuntimePath.string() : this->RunnerMountPath.string();
    VariablesMap["TempPath"]     = this->TempPath;
    //Exe locator — the resolved per-variant CONTENTPATH (guest-relative + absolute host path).
    VariablesMap["ContentPath"]  = this->ExePathRelative;
    VariablesMap["Content"]      = this->ExePathComplete;
    //Custom variables last (may shadow a built-in).
    for (auto &[Key, Value] : this->CustomVariables)
        VariablesMap[Key] = Value;
    return VariablesMap;
}
