#include "launchresolver.h"
#include "apppaths.h"        // AppPaths::DataRoot — the app data root the launch TEMP hangs off of
#include "varsubst.h"        // VarSubst::StringVariableSubstitution / RenderValue
#include "packagecatalog.h"  // GetPackageUserSettings (catalog/user-settings service)
#include "runnerwrapper.h"   // RunnerWrapper::ExecutableAvailable / DefPrefixDir
#include "commonutils.h"     // Log*

#include <QDir>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <filesystem>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

//The pure manifest queries + VFS-layer helpers live in ManifestModel; the catalog/user-settings service in
//PackageCatalog. Bring both in unqualified so the resolver code (moved verbatim out of ContainerWrapper) reads
//naturally (FindComponentIndex / GetPackageUserSettings / MachinePlatform / IsVfsLayer / ...).
using namespace ManifestModel;
using namespace PackageCatalog;

//Resolves every CustomVar (TYPE:"CustomVar") in the closure into ONE GLOBAL NAMESPACE (ContainerParams.CustomVariables,
//keyed by the bare token "KEY"). ABSOLUTE SCOPE: a var's final value is visible to every reference — including inside
//another var's DEFAULT — regardless of declaration order. Hierarchy is respected: the LATER (most-specific) declaration
//of a key wins (closure order: parents before children, the launchable last; then the active runner components). Bare
//keys are one shared knob, so a game seeding a runner's knob (re-declaring its KEY) is the feature.
//
//Done in two phases (see body): (1) collect each key's winning RAW source by priority; (2) fixpoint-substitute all
//sources against the built-in tokens + every other var until stable, so forward references and post-override values
//resolve. No encoding here — that is a use-site concern (%KEY:format%). A reference cycle leaves a residual %token%.
//
//Per-key source priority (highest to lowest):
//  1. ContainerParams.VariableOverrides — set from --var KEY=VALUE CLI flags or UI picker
//  2. GlobalConfigJSON["USERSETTINGS"][PackageUID]["VARIABLES"] — persisted user choices
//  3. the winning DEFAULT (or, for a secret+POOL var with nothing persisted yet, one pool entry drawn ONCE as a
//     seed and reported in ContainerParams.PickedSecrets for the caller to persist)

// P6 split: the unified Persist derivation (KEEP/DROP) — see launchresolver.cpp for the spine.
bool LaunchResolver::DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    ContainerParams.PersistAll = false;        // set true only by a KEEP of the runtime root (whole-runtime persist)
    ContainerParams.KeepDirs.clear();
    ContainerParams.KeepFiles.clear();
    ContainerParams.KeepRegKeys.clear();
    ContainerParams.KeepRegHives.clear();
    ContainerParams.DropPaths.clear();

    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();

    auto ToUpper = [](std::string S){ for (char &C : S) C = (char)std::toupper((unsigned char)C); return S; };
    auto ToLower = [](std::string S){ for (char &C : S) C = (char)std::tolower((unsigned char)C); return S; };
    auto AddUnique = [](std::vector<std::string> &V, const std::string &S){ if (std::find(V.begin(), V.end(), S) == V.end()) V.push_back(S); };

    //A registry hive root → its Wine .reg file (HKCU→user, HKLM/HKCR/HKCC→system, HKU→userdef). "" if not a root.
    auto HiveRootFile = [&](const std::string &Root) -> std::string {
        const std::string R = ToUpper(Root);
        if (R == "HKCU" || R == "HKEY_CURRENT_USER")   return "user.reg";
        if (R == "HKLM" || R == "HKEY_LOCAL_MACHINE")  return "system.reg";
        if (R == "HKCR" || R == "HKEY_CLASSES_ROOT")   return "system.reg";
        if (R == "HKCC" || R == "HKEY_CURRENT_CONFIG") return "system.reg";
        if (R == "HKU"  || R == "HKEY_USERS")          return "userdef.reg";
        return "";
    };
    //A runtime-root-relative path: directory (→ live RW passthrough) or single file (→ copy). Trailing slash or an
    //existing dir ⇒ directory; an existing file ⇒ file; otherwise a dotted leaf (an extension) ⇒ file, else directory.
    auto IsDirTarget = [&](const std::string &T) -> bool {
        if (!T.empty() && (T.back() == '/' || T.back() == '\\')) return true;
        std::error_code Ec;
        const std::filesystem::path Abs = ContainerParams.UserDataPath / T;
        if (std::filesystem::is_directory(Abs, Ec))    return true;
        if (std::filesystem::is_regular_file(Abs, Ec)) return false;
        const auto Slash = T.find_last_of("/\\");
        const std::string Leaf = (Slash == std::string::npos) ? T : T.substr(Slash + 1);
        return Leaf.find('.') == std::string::npos;   // no extension ⇒ directory
    };
    auto NormalizeRel = [](std::string T){
        while (!T.empty() && (T.back() == '/' || T.back() == '\\')) T.pop_back();
        while (!T.empty() && (T.front() == '/' || T.front() == '\\')) T.erase(T.begin());
        return T;
    };
    auto StripTrail = [](std::string S){ while (S.size() > 1 && (S.back() == '/' || S.back() == '\\')) S.pop_back(); return S; };
    //A KEEP target that names the runtime mount root itself (`%RuntimePath%`, or the bare root "."/"/") ⇒ persist the
    //WHOLE runtime: the durable UserDataPath becomes the writable branch. The elegant replacement for the old MODE:all.
    auto IsRuntimeRoot = [&](const std::string &T) -> bool {
        if (T == "." || T == "/" || T == "./") return true;
        if (ContainerParams.RuntimePath.empty()) return false;
        return StripTrail(T) == StripTrail(ContainerParams.RuntimePath.string());
    };

    auto ClassifyKeep = [&](std::string T){
        VarSubst::StringVariableSubstitution(T, Vars);
        if (T.empty()) { LogWarn("DerivePersistence", "  KEEP with empty target (skipped)."); return; }
        if (T.rfind("host:", 0) == 0) { LogWarn("DerivePersistence", "  KEEP host: target not yet supported (reserved for native containment): " + T); return; }
        if (IsRuntimeRoot(T)) { ContainerParams.PersistAll = true; LogOut("DerivePersistence", "  KEEP %RuntimePath% (whole runtime durable)"); return; }
        if (ToLower(T) == "registry")
        { AddUnique(ContainerParams.KeepRegHives, "user.reg"); AddUnique(ContainerParams.KeepRegHives, "system.reg"); AddUnique(ContainerParams.KeepRegHives, "userdef.reg"); LogOut("DerivePersistence", "  KEEP registry (all hives)"); return; }
        const auto Sep = T.find_first_of("\\/");
        const std::string Root = (Sep == std::string::npos) ? T : T.substr(0, Sep);
        if (const std::string Hive = HiveRootFile(Root); !Hive.empty())
        {
            if (Sep == std::string::npos) { AddUnique(ContainerParams.KeepRegHives, Hive); LogOut("DerivePersistence", "  KEEP hive " + Root + " (" + Hive + ")"); }
            else                          { AddUnique(ContainerParams.KeepRegKeys, T);     LogOut("DerivePersistence", "  KEEP regkey " + T); }
            return;
        }
        if (IsDirTarget(T)) { AddUnique(ContainerParams.KeepDirs,  NormalizeRel(T)); LogOut("DerivePersistence", "  KEEP dir  " + NormalizeRel(T)); }
        else                { AddUnique(ContainerParams.KeepFiles, NormalizeRel(T)); LogOut("DerivePersistence", "  KEEP file " + NormalizeRel(T)); }
    };
    auto ClassifyDrop = [&](std::string T){
        VarSubst::StringVariableSubstitution(T, Vars);
        if (T.empty()) { LogWarn("DerivePersistence", "  DROP with empty target (skipped)."); return; }
        if (ToLower(T) == "registry" || !HiveRootFile((T.find_first_of("\\/") == std::string::npos) ? T : T.substr(0, T.find_first_of("\\/"))).empty())
        { LogWarn("DerivePersistence", "  DROP of a registry target is not supported (paths only): " + T); return; }
        AddUnique(ContainerParams.DropPaths, NormalizeRel(T)); LogOut("DerivePersistence", "  DROP path " + NormalizeRel(T));
    };

    auto Scan = [&](const nlohmann::ordered_json &S){
        if (!S.is_object() || S.value("TYPE", std::string()) != "Persist") return;
        if (S.contains("KEEP") && S["KEEP"].is_string()) ClassifyKeep(S["KEEP"]);
        if (S.contains("DROP") && S["DROP"].is_string()) ClassifyDrop(S["DROP"]);
    };

    LogOut("DerivePersistence", "Resolving Persist policy (runner keep-set + Recipe)...");
    //Persistence is purely additive (KEEP adds, DROP removes), so scan order is immaterial — the runner's platform
    //keep-set and the game's Persist layers simply union.
    if (ContainerParams.RunnerPersistLayers.is_array())
        for (const auto &S : ContainerParams.RunnerPersistLayers) Scan(S);
    for (const std::string &CompID : ContainerParams.Recipe)
    {
        int Idx = FindComponentIndex(MANIFESTJSON, CompID);
        if (Idx == -1) continue;
        const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
        if (!Comp.contains("SUBCOMPONENTS") || !Comp["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : Comp["SUBCOMPONENTS"]) Scan(S);
    }

    LogSucc("DerivePersistence",
            "PERSIST: ALL=" + std::string(ContainerParams.PersistAll ? "true" : "false") +
            " KEEP[dirs=" + std::to_string(ContainerParams.KeepDirs.size()) +
            " files=" + std::to_string(ContainerParams.KeepFiles.size()) +
            " hives=" + std::to_string(ContainerParams.KeepRegHives.size()) +
            " regkeys=" + std::to_string(ContainerParams.KeepRegKeys.size()) + "]" +
            " DROP=" + std::to_string(ContainerParams.DropPaths.size()));
    return true;
}

//Finds the variant in MANIFESTJSON matching ContainerParams.VariantID
//under SUBGAMES[ContainerParams.subgame_id].VARIANTS and populates exe/work-dir/args fields.
//Must be called AFTER BuildSubComponentsArray and BEFORE BuildContainerRuntime.
// A VFS-anchored CONTENTPATH/WORKDIR (authored the same way as layer TARGETs — e.g.
// "%PrefixRoot%/drive_c/%PackageUID%/Foo.exe", substituting to "pfx/drive_c/17260/Foo.exe") expressed relative to the
// runner's content root. The guest-path template ("C:\<uid>\%REL%" for wine, identity for native) and the runner's
// %ContentPath% consume this relative form. ContentRoot is the runner's mount root ("pfx/drive_c/%PackageUID%" for
// proton/wine; "" for a native runner whose content root IS the VFS root — a no-op here). Both inputs are pre-
// substituted; separators/edge slashes are normalized before comparison.
