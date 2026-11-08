// LaunchCommand::Compose — the command and environment a launch runs, from its resolved parameters alone. It was the
// untested middle of ContainerWrapper::Execute: every launch's argv and env go through it, and a mis-ordered layer or
// a dropped argument shows up only as a game that starts wrong. Each case pins one rule of the composition.

#include <QtTest/QtTest>

#include "launchcommand.h"

namespace {

QStringList Q(std::initializer_list<const char *> L) { QStringList Out; for (const char *S : L) Out << S; return Out; }

ContainerParams Game()
{
    ContainerParams CP;
    CP.ExePathComplete = "/rt/game/game.exe";
    CP.ExeArgs = { "-windowed" };
    return CP;
}

RunnerLink Link(const std::string &Id, const std::string &Exe, std::vector<std::string> Args, bool Boundary)
{
    RunnerLink L;
    L.NodeId = Id; L.Executable = Exe; L.Args = std::move(Args);
    if (Boundary) L.ContentRoot = "drive_c";   // a cross-namespace link: the boundary
    return L;
}

} // namespace

class LaunchCommandTest : public QObject
{
    Q_OBJECT
private slots:
    // A native chain runs the content itself, with its own args. Teeth: run the runner field regardless.
    void launch_command_native_runs_the_content()
    {
        const LaunchCommand C = LaunchCommand::Compose(Game(), QProcessEnvironment());
        QVERIFY(C.Program == "/rt/game/game.exe");
        QVERIFY(C.Arguments == Q({ "-windowed" }));
    }

    // A runner's EXECUTABLE + ARGS are the command, the content's args after them. Teeth: drop the content's args.
    void launch_command_runner_args_then_the_contents()
    {
        ContainerParams CP = Game();
        CP.RunnerExecutable = "/r/proton";
        CP.RunnerArgs = { "waitforexitandrun", "C:\\game.exe" };
        const LaunchCommand C = LaunchCommand::Compose(CP, QProcessEnvironment());
        QVERIFY(C.Program == "/r/proton");
        QVERIFY(C.Arguments == Q({ "waitforexitandrun", "C:\\game.exe", "-windowed" }));
    }

    // An override (tooling, an installer) keeps the verb, drops what names the content, follows the verb with itself.
    // Teeth: keep the content argument.
    void launch_command_override_swaps_the_target()
    {
        ContainerParams CP = Game();
        CP.RunnerExecutable = "/r/proton";
        CP.RunnerArgs = { "waitforexitandrun", "%Content%" };
        const LaunchCommand C = LaunchCommand::Compose(CP, QProcessEnvironment(), "setup.exe", { "/S" });
        QVERIFY(C.Arguments == Q({ "waitforexitandrun", "setup.exe", "/S" }));
    }

    // An outer same-namespace wrapper (gamescope) wraps the whole inner command; a passthrough terminal does not. Teeth:
    // skip the wrap; wrap with the passthrough too.
    void launch_command_outer_wrappers_wrap_the_inner_command()
    {
        ContainerParams CP = Game();
        CP.RunnerExecutable = "/r/proton";
        CP.RunnerArgs = { "run" };
        CP.RunnerChain = { Link("proton", "/r/proton", { "run" }, true), Link("scope", "gamescope", { "-f" }, false),
                           Link("native", "", {}, false) };
        const LaunchCommand C = LaunchCommand::Compose(CP, QProcessEnvironment());
        QVERIFY(C.Program == "gamescope");
        QVERIFY(C.Arguments == Q({ "-f", "/r/proton", "run", "-windowed" }));
        QVERIFY(C.Wrappers == std::vector<std::string>{ "scope" });
    }

    // The environment: Base, then each outer wrapper's, the boundary's last — the content-proximate declaration wins; a
    // removal takes a key out of Base; a non-string value is skipped, not thrown on. Teeth: apply the boundary first.
    void launch_command_env_layers_with_the_boundary_last()
    {
        ContainerParams CP = Game();
        CP.RunnerExecutable = "/r/proton";
        RunnerLink W = Link("scope", "gamescope", {}, false);
        W.Env = { {"A", "wrapper"}, {"B", "wrapper"} };
        CP.RunnerChain = { Link("proton", "/r/proton", {}, true), W };
        CP.RunnerEnv = { {"A", "boundary"}, {"N", 3} };
        CP.RunnerRemoveEnv = { "GONE" };
        QProcessEnvironment Base;
        Base.insert("GONE", "x");
        Base.insert("KEPT", "y");
        const LaunchCommand C = LaunchCommand::Compose(CP, Base);
        QVERIFY(C.Env.value("A") == "boundary");
        QVERIFY(C.Env.value("B") == "wrapper");
        QVERIFY(!C.Env.contains("GONE"));
        QVERIFY(C.Env.value("KEPT") == "y");
        QVERIFY(!C.Env.contains("N"));
    }

    // WINEDLLOVERRIDES for a prefix runner: the package's entries, then an operator's own from the host environment —
    // last, so wine honours the human's. Teeth: replace the operator's; put it first.
    void launch_command_dll_overrides_keep_the_operators_last()
    {
        ContainerParams CP = Game();
        CP.RunnerExecutable = "/r/proton";
        CP.PrefixGenerate = true;
        CP.DLLOverrides = { "d3d11=n", "dxgi=n" };
        QProcessEnvironment Base;
        Base.insert("WINEDLLOVERRIDES", "dxgi=b");
        const LaunchCommand C = LaunchCommand::Compose(CP, Base);
        QVERIFY(C.Env.value("WINEDLLOVERRIDES") == "d3d11=n;dxgi=n;dxgi=b");
    }
};

QTEST_MAIN(LaunchCommandTest)
#include "test_launchcommand.moc"
