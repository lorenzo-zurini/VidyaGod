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

// P6 split: exec/CONTENTPATH resolution + session path derivation — see launchresolver.cpp for the spine.
static std::string ContentRootRelative(const std::string &RawPath, const std::string &RawContentRoot)
{
    std::string Path        = NormalizeTargetPath(RawPath);
    std::string ContentRoot = NormalizeTargetPath(RawContentRoot);
    if (ContentRoot.empty()) return Path;
    if (Path == ContentRoot) return std::string();
    const std::string Prefix = ContentRoot + "/";
    if (Path.rfind(Prefix, 0) == 0) return Path.substr(Prefix.size());
    return Path;
}

bool LaunchResolver::ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams)
{
    if (ContainerParams.VariantID.empty())
    {
        LogWarn("ResolveExecutableDefinition", "No VariantID set — skipping.");
        return true;
    }
    // Source the exec definition from the node graph — every launch is node-native (the pre-cutover
    // MANIFEST/GAMES/VARIANTS branch that used to live here was unreachable: NodeIdx is always set).
    nlohmann::ordered_json Resolved = nlohmann::ordered_json::object();
    if (ContainerParams.ComposedExec.is_object() && !ContainerParams.ComposedExec.empty())
        Resolved = ContainerParams.ComposedExec;          // the closure-composed DeclareExec (variant overrides base)
    else if (const Node *L = ContainerParams.NodeIdx->Find(ContainerParams.subgame_id); L && L->Exec.is_object())
        Resolved = L->Exec;                               // empty EXEC = self-contained launchable (e.g. gemrb) — OK
    LogSucc("ResolveExecutableDefinition", "Resolved exec for: " + ContainerParams.subgame_id);
    // The variant declares ONE universal target path — CONTENTPATH: the VFS-root-anchored path (authored like layer
    // TARGETs, e.g. "%PrefixRoot%/drive_c/%PackageUID%/Foo.exe") to whatever the runner runs or loads (an exe, a ROM,
    // a data root, or nothing for a self-contained runner). Its absolute host path is that location under the runtime
    // mount; the runner composes how it's used from %ContentPath% (content-root-relative) / %Content% (absolute host).
    const std::map<std::string, std::string> ExecVars = ContainerParams.GetVariablesMap();
    {
        std::string ExeStr = Resolved.value("CONTENTPATH", std::string());
        VarSubst::StringVariableSubstitution(ExeStr, ExecVars);
        for (char &c : ExeStr) if (c == '\\') c = '/';
        while (!ExeStr.empty() && ExeStr.front() == '/') ExeStr.erase(ExeStr.begin());
        while (!ExeStr.empty() && ExeStr.back()  == '/') ExeStr.pop_back();
        if (ExeStr.empty())                                                  // self-contained runner (no CONTENTPATH)
        {
            ContainerParams.ExePathRelative.clear();
            ContainerParams.ExePathComplete = ContainerParams.ProgramPath;
        }
        else
        {
            ContainerParams.ExePathComplete = ContainerParams.RuntimePath / ExeStr;
            ContainerParams.ExePathRelative = std::filesystem::path(ContentRootRelative(ExeStr, ContainerParams.ContentRoot));
        }
    }
    LogOut("ResolveExecutableDefinition", "ContentPath: " + ContainerParams.ExePathRelative.string());
    LogOut("ResolveExecutableDefinition", "Content: " + ContainerParams.ExePathComplete.string());
    auto &WorkDirVal = Resolved["WORKDIR"];
    if (!WorkDirVal.is_null() && WorkDirVal.is_string() && !std::string(WorkDirVal).empty())
    {
        std::string WorkDirStr = std::string(WorkDirVal);
        VarSubst::StringVariableSubstitution(WorkDirStr, ExecVars);
        for (char &c : WorkDirStr) if (c == '\\') c = '/';
        while (!WorkDirStr.empty() && WorkDirStr.front() == '/') WorkDirStr.erase(WorkDirStr.begin());
        while (!WorkDirStr.empty() && WorkDirStr.back()  == '/') WorkDirStr.pop_back();
        ContainerParams.WorkDirPathRelative = std::filesystem::path(ContentRootRelative(WorkDirStr, ContainerParams.ContentRoot));
        ContainerParams.WorkDirPathComplete = ContainerParams.RuntimePath / WorkDirStr;
    }
    else if (!ContainerParams.ExePathRelative.empty())
    {
        // No explicit WORKDIR: default to the executable's OWN directory (not the content-mount root) — games
        // resolve their data/DLLs via paths relative to the exe's folder. This is the historical default and is
        // a no-op when the exe sits at the root (parent_path is empty → ProgramPath).
        ContainerParams.WorkDirPathRelative = ContainerParams.ExePathRelative.parent_path();
        ContainerParams.WorkDirPathComplete = ContainerParams.ExePathComplete.parent_path();
    }
    else ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;   // self-contained runner (no CONTENTPATH)
    LogOut("ResolveExecutableDefinition", "WorkDirPathComplete: " + ContainerParams.WorkDirPathComplete.string());
    ContainerParams.ExeArgs.clear();
    auto &ExeArgsVal = Resolved["EXEARGS"];
    if (!ExeArgsVal.is_null() && ExeArgsVal.is_string() && !std::string(ExeArgsVal).empty())
    {
        std::string Args = std::string(ExeArgsVal);
        VarSubst::StringVariableSubstitution(Args, ExecVars);
        std::istringstream iss(Args);
        for (std::string tok; std::getline(iss, tok, ' ');) ContainerParams.ExeArgs.push_back(tok);
    }
    return true;
}

//Iterates all COMPONENTS in MANIFEST order, collects SUBCOMPONENTS from those whose
//COMPONENTID appears in Recipe, and appends them to SubComponentsArray.
//Variable substitution is applied to each subcomponent's JSON string at this point
//so all downstream consumers receive already-expanded values.
//CustomVar and Persist* subcomponents are skipped — they are handled by ResolveCustomVariables and
//DerivePersistence. MANIFEST order is preserved, which determines VFS layer stacking order.

bool LaunchResolver::DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON)
{
    if (ContainerParams.ScreenWidth.empty() || ContainerParams.ScreenHeight.empty())
    {
        //A headless run builds no QApplication (it must never touch the display), so there is no screen to ask.
        //The fallback must still be a REAL resolution: packages pass these straight to the game (Worms 3D's
        //EXEARGS are "/W %ScreenWidth% /H %ScreenHeight% /FS"), and a game handed 0x0 asks for a 0x0 display
        //mode, gets a null device back and dereferences it — that looked exactly like a broken game.
        //Order: explicit --var override > the Qt screen > the X/Wayland display > a sane default.
        auto Override = [&](const char *Key) {
            auto It = ContainerParams.VariableOverrides.find(Key);
            return It == ContainerParams.VariableOverrides.end() ? std::string() : It->second;
        };
        ContainerParams.ScreenWidth  = Override("ScreenWidth");
        ContainerParams.ScreenHeight = Override("ScreenHeight");

        if (ContainerParams.ScreenWidth.empty() && qApp && QThread::currentThread() == qApp->thread())
            if (QScreen * Scr = QGuiApplication::primaryScreen())
            {
                ContainerParams.ScreenWidth  = std::to_string(Scr->geometry().width());
                ContainerParams.ScreenHeight = std::to_string(Scr->geometry().height());
            }
        if (ContainerParams.ScreenWidth.empty())
        {
            //Ask the display server directly — cheap, and it is what the game would have seen anyway.
            if (const char *Disp = ::getenv("DISPLAY"); Disp && *Disp)
                if (FILE *P = ::popen("xrandr --current 2>/dev/null | awk '/\\*/{print $1; exit}'", "r"))
                {
                    char Buf[64] = {0};
                    if (std::fgets(Buf, sizeof(Buf), P))
                    {
                        std::string Mode(Buf);
                        if (const size_t X = Mode.find('x'); X != std::string::npos)
                        {
                            ContainerParams.ScreenWidth  = Mode.substr(0, X);
                            ContainerParams.ScreenHeight = Mode.substr(X + 1, Mode.find_first_not_of("0123456789", X + 1) - X - 1);
                        }
                    }
                    ::pclose(P);
                }
        }
        if (ContainerParams.ScreenWidth.empty() || ContainerParams.ScreenHeight.empty()
            || ContainerParams.ScreenWidth == "0" || ContainerParams.ScreenHeight == "0")
        {
            ContainerParams.ScreenWidth  = "1280";
            ContainerParams.ScreenHeight = "720";
            LogWarn("DerivePaths", "No display to size the game against — defaulting to 1280x720.");
        }
    }
    std::filesystem::path TempRoot = AppPaths::DataRoot() / "TEMP";
    if (GlobalConfigJSON.contains("Settings") && GlobalConfigJSON["Settings"].is_object())
    {
        const auto &S = GlobalConfigJSON["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object()
            && S["Paths"].contains("TempRoot") && S["Paths"]["TempRoot"].is_string()
            && !std::string(S["Paths"]["TempRoot"]).empty())
            TempRoot = std::filesystem::path(std::string(S["Paths"]["TempRoot"]));
    }
    ContainerParams.TempPath = TempRoot / ContainerParams.PackageUID;
    if (ContainerParams.RunnerShipsBuild && !ContainerParams.UnifiedRuntime)
        ContainerParams.RunnerMountPath = ContainerParams.TempPath / "RUNNER";
    ContainerParams.RuntimePath     = ContainerParams.TempPath / "RUNTIME";
    ContainerParams.WriteLayerPath  = ContainerParams.TempPath / "WRITELAYER";
    ContainerParams.DefaultDataPath = ContainerParams.TempPath / "DEFAULTDATA";
    ContainerParams.UserDataPath    = ContainerParams.PackagePath / "USERDATA";
    //CLI / in-package overrides (--runtime-dir / --userdata-dir): point these exact paths wherever asked (in-package
    //sets both = the package dir, making the package a self-contained, portable runnable unit).
    if (!AppPaths::RuntimePathOverride().empty())  ContainerParams.RuntimePath  = AppPaths::RuntimePathOverride();
    if (!AppPaths::UserDataPathOverride().empty()) ContainerParams.UserDataPath = AppPaths::UserDataPathOverride();
    VarSubst::StringVariableSubstitution(ContainerParams.ContentRoot, ContainerParams.GetVariablesMap());
    ContainerParams.ProgramPath = ContainerParams.ContentRoot.empty()
        ? ContainerParams.RuntimePath : ContainerParams.RuntimePath / ContainerParams.ContentRoot;
    {
        const std::string &CR = ContainerParams.ContentRoot;
        const auto Pos = CR.find("drive_c");
        ContainerParams.PrefixRoot = (Pos == std::string::npos) ? std::string()
            : (Pos == 0 ? std::string() : CR.substr(0, Pos - 1));
    }
    if (ContainerParams.PrefixGenerate)
        ContainerParams.DefPrefixPath = ContainerParams.RunnerShipsBuild
            ? std::filesystem::path(RunnerWrapper::DefPrefixDir(ContainerParams.RunnerPackagePath.string()))
            : ContainerParams.TempPath / "DEFPREFIX";
    ContainerParams.WorkDirPathComplete = ContainerParams.ProgramPath;
    LogOut("DerivePaths", "RuntimePath: " + ContainerParams.RuntimePath.string()
           + " | ContentRoot: " + ContainerParams.ContentRoot + " (PrefixRoot: '" + ContainerParams.PrefixRoot + "')");
    return true;
}

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available).
//Priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > best qualifying default, where "best" ranks
//RECOMMENDED runners first, then a runner shipped in the launch node's OWN package (embedded > global — a package
//that carries a runner uses its own copy; runners are global wherever they live, there is no embedded flag), then
//the lowest node-id for determinism. This keeps the default from being an arbitrary alphabetical accident.
