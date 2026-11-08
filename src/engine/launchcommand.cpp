#include "launchcommand.h"
#include "commonutils.h"
#include "launchresolver.h"   // ComposeGuestTarget — a cross-namespace chain's inner target
#include "varsubst.h"

#include <numeric>   // std::accumulate (WINEDLLOVERRIDES join)

LaunchCommand LaunchCommand::Compose(ContainerParams CP, QProcessEnvironment Base, const std::string &OverrideExe,
                                     const std::vector<std::string> &OverrideArgs)
{
    LaunchCommand Out;
    const bool Override = !OverrideExe.empty();
    auto Subst = [&](std::string S){ VarSubst::StringVariableSubstitution(S, CP.GetVariablesMap()); return S; };

    //----- CROSS-NAMESPACE NESTING: when an inner runner runs inside the boundary's guest fs (e.g. a win32 emulator
    //under proton), redirect the boundary's content target at the inner runners' guest command. ComposeGuestTarget
    //returns {CrossNamespace:false} for every classic chain, so this whole block is a no-op there. The inner runners'
    //own args (the ROM, etc.) are appended after the boundary's ARGS. Not applied under an OverrideExe (tooling). -----
    std::vector<std::string> CrossTrailingArgs;
    if (!Override)
    {
        LaunchResolver::GuestTarget GT = LaunchResolver::ComposeGuestTarget(CP);
        if (GT.CrossNamespace)
        {
            //Point the boundary's %ContentPath%/%Content% at the inner runner's guest exe (its ARGS template composes
            //the guest path from these), and carry the inner args to append after the boundary's ARGS.
            CP.ExePathRelative = std::filesystem::path(GT.ContentRel);
            CP.ExePathComplete = CP.ProgramPath / GT.ContentRel;
            CrossTrailingArgs = GT.TrailingArgs;
            LogOut("LaunchCommand::Compose", "Cross-namespace target: " + GT.ContentRel
                   + " (+" + std::to_string(CrossTrailingArgs.size()) + " inner arg(s))");
        }
    }

    //----- BASE command: the BOUNDARY runner runs the content (runner daisy-chaining) -----
    //The legacy single-runner fields are the boundary runner's view (the link that owns the FUSE mount / prefix —
    //proton for a win32 game, or the native terminal for native content). An EMPTY RunnerExecutable = native
    //passthrough: run the content's own executable (%Content%) directly. Otherwise the runner's EXECUTABLE + ARGS ARE
    //the command (author composes the guest path into ARGS, e.g. proton's ["waitforexitandrun","%GameDir%\\..."]).
    VarSubst::StringVariableSubstitution(CP.RunnerExecutable, CP.GetVariablesMap());
    const bool NativeBoundary = CP.RunnerExecutable.empty();
    std::string Program;
    QStringList Arguments;
    if (NativeBoundary)
    {
        //Native passthrough boundary: exec the content directly (or the OverrideExe for tooling).
        Program = Override ? OverrideExe : CP.ExePathComplete.string();
        for (const std::string &A : Override ? OverrideArgs : CP.ExeArgs) Arguments.append(QString::fromStdString(A));
    }
    else
    {
        Program = CP.RunnerExecutable;
        //For an OverrideExe (tooling/install), drop the content-bearing ARGS and append the override after the verb.
        for (const std::string &Raw : CP.RunnerArgs)
        {
            if (Override && ArgReferencesContent(Raw)) continue;
            Arguments.append(QString::fromStdString(Subst(Raw)));
        }
        //Cross-namespace: after the boundary's ARGS (which now reference the inner runner's guest exe via the
        //redirected %ContentPath%), append the inner runners' own guest args (e.g. the ROM path).
        for (const std::string &A : CrossTrailingArgs) Arguments.append(QString::fromStdString(A));
        if (Override) Arguments.append(QString::fromStdString(OverrideExe));
        for (const std::string &A : Override ? OverrideArgs : CP.ExeArgs) Arguments.append(QString::fromStdString(A));
    }

    //----- NESTING: wrap the base command with each OUTER same-namespace link (native wrappers + the native terminal) -----
    //chain = innermost(content)→outermost(machine). The boundary is the last cross-namespace link (or the first link for
    //a native chain); links AFTER it run IN THE HOST NAMESPACE and simply wrap the inner argv (`wrapper <args> <innercmd>`).
    //An empty-EXECUTABLE link is a passthrough terminal (forwards unchanged). Cross-namespace nesting (an inner emulator
    //inside this runner's guest fs) is Phase C — skipped here with a warning. Wrappers are not applied under OverrideExe.
    int BoundaryIdx = 0;
    for (int i = 0; i < (int)CP.RunnerChain.size(); ++i)
        if (!CP.RunnerChain[i].NativeNamespace()) BoundaryIdx = i;
    std::vector<const RunnerLink*> OuterWrappers;
    if (!Override)
        for (int i = BoundaryIdx + 1; i < (int)CP.RunnerChain.size(); ++i)
        {
            const RunnerLink &L = CP.RunnerChain[i];
            if (!L.NativeNamespace())
            { LogWarn("LaunchCommand::Compose", "Cross-namespace wrapper '" + L.NodeId + "' not yet supported (Phase C) — skipped."); continue; }
            //A native terminal whose EXECUTABLE is empty or runs %Content% is a passthrough — as an OUTER wrapper it
            //forwards the inner argv unchanged (the inner command IS its content). Only a real wrapper TOOL (e.g.
            //gamescope/mangohud, EXECUTABLE=a program) actually wraps. This keeps the appended native terminal a no-op
            //for the common [proton, native] chain (byte-identical to a direct proton launch).
            if (L.Executable.empty() || ArgReferencesContent(L.Executable)) continue;
            QStringList Wrapped;
            for (const std::string &Raw : L.Args) Wrapped.append(QString::fromStdString(Subst(Raw)));
            Wrapped.append(QString::fromStdString(Program));                     // inner program ...
            Wrapped.append(Arguments);                                          // ... then its args
            Program   = Subst(L.Executable);
            Arguments = Wrapped;
            OuterWrappers.push_back(&L);
            Out.Wrappers.push_back(L.NodeId);
            LogOut("LaunchCommand::Compose", "Chain wrap: " + L.NodeId + " (" + Program + ")");
        }

    //----- ENV: one process tree, so every link's ENV/REMOVE_ENV applies. Boundary (legacy fields) first, then the
    //outer same-namespace wrappers — boundary (innermost, content-proximate) WINS on key conflicts. -----
    QProcessEnvironment RunProcessEnvironment = std::move(Base);
    //ORDER: outermost first, boundary LAST, so the innermost (content-proximate) declaration wins on a shared
    //key — which is what the rest of the engine promises. It used to run the other way: RunnerEnv went in
    //first and each outer wrapper's Env overwrote it. That merely mis-ordered runner-vs-runner until a
    //launchable's own ENV started being merged into RunnerEnv, at which point a chained outer link could
    //silently beat the GAME's environment, contradicting "the more specific one is the game's".
    for (auto It = OuterWrappers.rbegin(); It != OuterWrappers.rend(); ++It)
    {
        const RunnerLink *L = *It;
        for (const std::string &Key : L->RemoveEnv) RunProcessEnvironment.remove(QString::fromStdString(Key));
        for (auto &[Key, Value] : L->Env.items())
        {
            if (!Value.is_string())
            {
                LogWarn("LaunchCommand::Compose",
                        "chained-runner ENV key '" + Key + "' is " + std::string(Value.type_name())
                        + ", not a string — skipped. Environment values are strings (quote the number).");
                continue;
            }
            RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(Subst(Value.get<std::string>())));
        }
    }
    //The boundary's removals and env go last: closest to the content, so highest priority.
    for (const std::string &Key : CP.RunnerRemoveEnv)
        RunProcessEnvironment.remove(QString::fromStdString(Key));
    for (auto &[Key, Value] : CP.RunnerEnv.items())
    {
        //get<std::string>() THROWS on a non-string, uncaught, at the last step of a launch. A runner ENV comes
        //from a package file like any other field, so this is an authoring mistake, not an invariant.
        if (!Value.is_string())
        {
            LogWarn("LaunchCommand::Compose",
                    "runner ENV key '" + Key + "' is " + std::string(Value.type_name()) + ", not a string — "
                    "skipped. Environment values are strings (quote the number).");
            continue;
        }
        RunProcessEnvironment.insert(QString::fromStdString(Key), QString::fromStdString(Subst(Value.get<std::string>())));
    }

    // WINEDLLOVERRIDES — only meaningful for runners that have a wine prefix. MERGED with any value already in
    // the environment rather than replacing it: this line silently clobbered an operator's debugging override
    // (WINEDLLOVERRIDES="d3d11,dxgi=b" to force wined3d on a GPU whose Vulkan is too old for DXVK) and cost an
    // hour of "why does my env var do nothing" during the 2026-09-01 laptop triage. External entries go LAST —
    // measured empirically in the same triage: with a dll named in both lists, wine honored the LATER entry
    // (external-first left the package's d3d11=n winning while dxgi flipped) — so last place is what makes a
    // human's override beat the package's.
    if (CP.PrefixGenerate && !CP.DLLOverrides.empty())
    {
        QString Composed = QString::fromStdString(std::accumulate(CP.DLLOverrides.begin(), CP.DLLOverrides.end(), std::string{}, [](const std::string &a, const std::string &b){ return a.empty() ? b : a + ";" + b; }));
        const QString External = RunProcessEnvironment.value("WINEDLLOVERRIDES");   // an operator's, from the host env
        if (!External.isEmpty())
        {
            LogOut("LaunchCommand::Compose", "WINEDLLOVERRIDES from the environment merged ahead of the package's: " + External.toStdString());
            Composed = (Composed.isEmpty() ? QString() : Composed + ";") + External;
        }
        RunProcessEnvironment.insert("WINEDLLOVERRIDES", Composed);
    }

    Out.Program   = std::move(Program);
    Out.Arguments = std::move(Arguments);
    Out.Env       = std::move(RunProcessEnvironment);
    return Out;
}
