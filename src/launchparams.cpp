#include "launchparams.h"
#include "commonutils.h"   // LogOut
#include "varsubst.h"

#include <functional>
#include <map>

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
    VariablesMap["RuntimePath"]  = this->RuntimePath.string();   // the VFS mount root as a host path (WINEPREFIX / STEAM_COMPAT_DATA_PATH)
    VariablesMap["RunnerMount"]  = this->UnifiedRuntime ? this->RuntimePath.string() : this->RunnerMountPath.string();
    VariablesMap["TempPath"]     = this->TempPath.string();
    //Exe locator — the resolved per-variant CONTENTPATH (guest-relative + absolute host path).
    VariablesMap["ContentPath"]  = this->ExePathRelative.string();
    VariablesMap["Content"]      = this->ExePathComplete.string();
    //Custom variables last (may shadow a built-in).
    for (auto &[Key, Value] : this->CustomVariables)
        VariablesMap[Key] = Value;
    //A guest-root anchor (GUEST_ROOTS {"%GameDir%": "C:\\%PackageUID%", "%UserProfile%": "C:\\users\\steamuser"}) resolves
    //to its guest path — what a value naming it (a registry InstallPath, an argument, a config line) must say. A path
    //the launch PLACES is then mapped through the runner's DRIVES (GuestToLayout). A custom variable is not shadowed.
    //An anchor may be spelled from another (%AppData% = "%UserProfile%\\AppData\\Roaming"), declared in any order: each
    //anchor is resolved once, after the anchors it names. Anchors spelled from each other have no value — CheckLayer
    //refuses such a runner; were one to get here anyway, the cycle stays unexpanded (reported), never grows: repeated
    //passes over "%A%" = "%A%%A%" doubled it every pass (≈25 GB at 30 anchors).
    if (this->GuestRoots.is_object())
    {
        std::map<std::string, std::string> Raw;                         // anchor KEY → its raw spelling
        for (const auto &[Anchor, Place] : this->GuestRoots.items())
        {
            if (Anchor.size() < 3 || Anchor.front() != '%' || Anchor.back() != '%' || !Place.is_string()) continue;
            const std::string Key = Anchor.substr(1, Anchor.size() - 2);
            if (!this->CustomVariables.count(Key)) Raw[Key] = Place.get<std::string>();
        }
        std::map<std::string, int> State;                               // 1 = resolving, 2 = resolved
        std::function<void(const std::string &)> Resolve = [&](const std::string &Key) {
            State[Key] = 1;
            for (const std::string &Dep : VarSubst::TokenKeys(Raw[Key]))
                if (Raw.count(Dep) && State[Dep] == 0) Resolve(Dep);
                else if (Raw.count(Dep) && State[Dep] == 1)
                    LogErr("ContainerParams::GetVariablesMap", "GUEST_ROOTS %" + Key + "% and %" + Dep
                                                             + "% are spelled from each other — left unresolved");
            std::string R = Raw[Key];
            VarSubst::StringVariableSubstitution(R, VariablesMap);
            VariablesMap[Key] = R;
            State[Key] = 2;
        };
        for (const auto &[Key, Spelling] : Raw)
            if (State[Key] == 0) Resolve(Key);
    }
    return VariablesMap;
}
