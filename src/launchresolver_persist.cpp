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
    ContainerParams.KeepDirs.clear();
    ContainerParams.KeepFiles.clear();
    ContainerParams.KeepRegKeys.clear();
    ContainerParams.KeepRegHives.clear();

    const std::map<std::string, std::string> Vars = ContainerParams.GetVariablesMap();

    auto ToLower = [](std::string S){ for (char &C : S) C = (char)std::tolower((unsigned char)C); return S; };
    auto AddUnique = [](std::vector<std::string> &V, const std::string &S){ if (std::find(V.begin(), V.end(), S) == V.end()) V.push_back(S); };
    //A runtime-root-relative PATH: directory (→ live RW passthrough) or single file (→ copy). Decided by shape only
    //(the durable store now lives at UserDataPath/<TARGET>, not mirrored at UserDataPath/<PATH>, so there is nothing
    //to stat here): a trailing slash ⇒ directory; a dotted last component (an extension) ⇒ file; otherwise directory.
    auto IsDirTarget = [&](const std::string &T) -> bool {
        if (!T.empty() && (T.back() == '/' || T.back() == '\\')) return true;
        const auto Slash = T.find_last_of("/\\");
        const std::string Leaf = (Slash == std::string::npos) ? T : T.substr(Slash + 1);
        return Leaf.find('.') == std::string::npos;   // no extension ⇒ directory
    };
    auto NormalizeRel = [](std::string T){
        while (!T.empty() && (T.back() == '/' || T.back() == '\\')) T.pop_back();
        while (!T.empty() && (T.front() == '/' || T.front() == '\\')) T.erase(T.begin());
        return T;
    };
    //The last path component (the durable-name default): "pfx/.../Saved Games/Foo" → "Foo".
    auto Leaf = [](std::string T){ while (!T.empty() && (T.back() == '/' || T.back() == '\\')) T.pop_back();
        const auto S = T.find_last_of("/\\"); return S == std::string::npos ? T : T.substr(S + 1); };
    //A durable TARGET must be a single safe path segment (it names a subdir directly under the instance dir). Trailing
    //dots/spaces are STRIPPED: Win32 silently drops them at the filesystem, so "instance.json." or "registry " would
    //otherwise sail past the reserved/dedup guards below yet land on the reserved sibling on disk (Windows port).
    auto SanSeg = [](const std::string &S){ std::string O; for (unsigned char C : S)
        O.push_back((std::isalnum(C) || C == '.' || C == '_' || C == '-' || C == ' ') ? static_cast<char>(C) : '_');
        while (!O.empty() && (O.back() == '.' || O.back() == ' ')) O.pop_back();
        return (O.empty() || O == "." || O == "..") ? std::string("_") : O; };

    //Every file/dir persist lands at UserDataPath/<TARGET> — a NAMED sibling of the instance's OWN state
    //(instance.json + the REGISTRY/REGKEYS stores). Two guards make that namespace safe:
    //  (a) RESERVED names are refused, so a persist can never seed the launcher's secrets into a game-visible mount
    //      or overwrite the instance config / registry stores (SanSeg keeps the segment shape but not the identity).
    //  (b) DUPLICATE targets are refused (case-insensitively — durable dirs travel to case-insensitive filesystems),
    //      so two persists can never clobber one file at capture or mount the same durable dir RW twice and corrupt it.
    //Registry keys/hives live under REGISTRY//REGKEYS/ (a disjoint namespace), so only the file/dir targets share one.
    std::map<std::string, std::string> UsedFileTargets;   // Target(lower) → the Path that claimed it
    static const std::set<std::string> Reserved = { "instance.json", "registry", "regkeys" };
    auto ClaimFileTarget = [&](const std::string &Target, const std::string &Path) -> bool {
        std::string Low = ToLower(Target);
        if (Reserved.count(Low))
        { LogErr("DerivePersistence", "  persist TARGET '" + Target + "' (for PATH '" + Path + "') is RESERVED for the instance's own state — skipped (choose another TARGET)."); return false; }
        auto It = UsedFileTargets.find(Low);
        if (It != UsedFileTargets.end())
        {
            //An IDENTICAL (Path, Target) re-declaration is harmless (a game repeating its runner's keep-set) — accept
            //it as an idempotent no-op, silently. Only a SAME-target/DIFFERENT-path collision is the clobber hazard.
            if (It->second == Path) { LogOut("DerivePersistence", "  persist TARGET '" + Target + "' already declared for the same PATH — skipping duplicate."); return false; }
            LogErr("DerivePersistence", "  persist TARGET '" + Target + "' (for PATH '" + Path + "') COLLIDES with an earlier persist of a DIFFERENT path ('" + It->second + "') — skipped to avoid clobbering saved data (give it a distinct TARGET)."); return false;
        }
        UsedFileTargets.emplace(Low, Path);
        return true;
    };

    //Parse one DeclarePersist node. SCOPE=file|registry; PATH = runtime source ("" = the whole runtime / all hives,
    //an AUTHORING aid that warns); TARGET = durable subdir under UserDataPath (defaults to PATH's last component,
    //REQUIRED when PATH is empty); CLOUD (default true) is the future Cloud-Saves flag (false = machine-specific).
    auto ParsePersist = [&](const nlohmann::ordered_json &S){
        const std::string Scope = ToLower(S.value("SCOPE", std::string("file")));
        std::string Path = S.value("PATH", std::string());
        VarSubst::StringVariableSubstitution(Path, Vars);
        const bool Cloud = S.value("CLOUD", true);
        if (Scope == "registry")
        {
            if (Path.empty())   // whole-hive registry persist — authoring aid
            { AddUnique(ContainerParams.KeepRegHives, "user.reg"); AddUnique(ContainerParams.KeepRegHives, "system.reg"); AddUnique(ContainerParams.KeepRegHives, "userdef.reg");
              LogWarn("DerivePersistence", "  registry persist PATH \"\" (all hives) — AUTHORING AID: persists EVERY hive so you can discover what to keep; narrow to specific keys before shipping."); return; }
            AddUnique(ContainerParams.KeepRegKeys, Path); LogOut("DerivePersistence", "  persist regkey " + Path); return;
        }
        // SCOPE=file
        const std::string DeclaredTarget = S.value("TARGET", std::string());
        std::string Target = SanSeg(DeclaredTarget.empty() ? Leaf(Path) : DeclaredTarget);
        if (Path.empty())   // whole-runtime persist — authoring aid; no leaf to default the TARGET from → require it
        {
            if (DeclaredTarget.empty())
            { LogErr("DerivePersistence", "  file persist PATH \"\" (whole runtime) requires an explicit TARGET (the durable subdir name) — skipped."); return; }
            if (!ClaimFileTarget(Target, "")) return;
            ContainerParams.KeepDirs.push_back({ std::string(), Target, Cloud });
            LogWarn("DerivePersistence", "  file persist PATH \"\" → TARGET '" + Target + "' — AUTHORING AID: persists the WHOLE runtime; narrow to specific dirs/files before shipping.");
            return;
        }
        const std::string Rel = NormalizeRel(Path);
        if (!ClaimFileTarget(Target, Rel)) return;
        if (IsDirTarget(Path)) { ContainerParams.KeepDirs.push_back({ Rel, Target, Cloud });  LogOut("DerivePersistence", "  persist dir  " + Rel + " → " + Target + (Cloud ? "" : " (local-only)")); }
        else                   { ContainerParams.KeepFiles.push_back({ Rel, Target, Cloud }); LogOut("DerivePersistence", "  persist file " + Rel + " → " + Target + (Cloud ? "" : " (local-only)")); }
    };

    auto Scan = [&](const nlohmann::ordered_json &S){
        if (!S.is_object() || S.value("TYPE", std::string()) != "DeclarePersist") return;
        //A WHEN gates this layer (it is consumed HERE, not mounted — the general layer gate deliberately skips
        //DeclarePersist, which once left a conditional persist applying unconditionally). Same resolved var map.
        if (S.contains("WHEN") && S["WHEN"].is_string()
            && !VarSubst::EvaluateCondition(S["WHEN"].get<std::string>(), Vars))
        {
            LogOut("DerivePersistence", "  skipped (WHEN false: " + S["WHEN"].get<std::string>() + ")");
            return;
        }
        ParsePersist(S);
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
            "PERSIST: dirs=" + std::to_string(ContainerParams.KeepDirs.size()) +
            " files=" + std::to_string(ContainerParams.KeepFiles.size()) +
            " hives=" + std::to_string(ContainerParams.KeepRegHives.size()) +
            " regkeys=" + std::to_string(ContainerParams.KeepRegKeys.size()));
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
