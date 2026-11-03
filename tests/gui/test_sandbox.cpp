// Tests for SandboxLayer — the MANDATORY bubblewrap wrapper for the launched game. Wrap() is pure argv construction
// (no bwrap needed), so we assert: the network mode reads from VIDYAGOD_SANDBOX_NET; and the produced argv mounts the
// host root read-only, unshares the net only when isolated, and ends with `-- <origProgram> <origArgs...>` so the real command is preserved verbatim.

#include <QtTest>

#include "sandboxlayer.h"
#include "launchparams.h"

#include <QProcess>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

class SandboxTest : public QObject
{
    Q_OBJECT

    static ContainerParams params(const std::map<std::string, std::string> &Vars)
    {
        ContainerParams CP(std::filesystem::path("/tmp"), std::string("node"), std::string());
        CP.PackagePath = "/tmp";
        CP.UsesVFS = false;
        CP.CustomVariables = Vars;
        return CP;
    }

    // A stand-in for the user's real home, OUTSIDE /tmp: /tmp is bound writable into every sandbox, the real home is not.
    static QString fakeHomeTemplate() { return QCoreApplication::applicationDirPath() + "/realhome-XXXXXX"; }

    // Sets HOME and XDG_CACHE_HOME (unset when empty) for one test: FromParams reads the launcher's own environment.
    struct ScopedHome
    {
        QByteArray OldHome = qgetenv("HOME"), OldCache = qgetenv("XDG_CACHE_HOME");
        bool HadHome = qEnvironmentVariableIsSet("HOME"), HadCache = qEnvironmentVariableIsSet("XDG_CACHE_HOME");
        explicit ScopedHome(const QString &Home, const QString &Cache = {})
        {
            qputenv("HOME", Home.toUtf8());
            if (Cache.isEmpty()) qunsetenv("XDG_CACHE_HOME"); else qputenv("XDG_CACHE_HOME", Cache.toUtf8());
        }
        ~ScopedHome()
        {
            if (HadHome) qputenv("HOME", OldHome); else qunsetenv("HOME");
            if (HadCache) qputenv("XDG_CACHE_HOME", OldCache); else qunsetenv("XDG_CACHE_HOME");
        }
    };

    static bool touch(const QString &Path) { QFile F(Path); return F.open(QIODevice::WriteOnly); }

    // The (source, destination) pairs of every --bind in a bwrap argv, in order.
    static QList<QPair<QString, QString>> binds(const QStringList &Args)
    {
        QList<QPair<QString, QString>> Out;
        for (int I = 0; I + 2 < Args.size() && Args.at(I) != "--"; ++I)
            if (Args.at(I) == "--bind") Out.append({Args.at(I + 1), Args.at(I + 2)});
        return Out;
    }

    static QProcessEnvironment env(const std::map<QString, QString> &Vars)
    {
        QProcessEnvironment E;
        for (const auto &[K, V] : Vars) E.insert(K, V);
        return E;
    }

private slots:
    // A runner's LD_LIBRARY_PATH (its bundled libraries) must not reach VidyaGod's own --sandbox-init: the bundled
    // GLib of an Ubuntu 22.04 build broke our Qt before main() ("undefined symbol", exit 127) on Raze and ECWolf.
    void the_games_loader_path_is_taken_out_of_the_inits_environment()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}, {"HOME", "/home/u"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"HOME", "/home/u"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Name), QStringLiteral("LD_LIBRARY_PATH"));
        QVERIFY(Game[0].Set);
        QCOMPARE(QString::fromStdString(Game[0].Value), QStringLiteral("/runner/lib"));
        QVERIFY(!Run.contains("LD_LIBRARY_PATH"));
        QCOMPARE(Run.value("HOME"), QStringLiteral("/home/u"));   // only loader variables move
    }

    void the_init_keeps_the_launchers_own_loader_path()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"LD_LIBRARY_PATH", "/app/lib"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Value), QStringLiteral("/runner/lib"));
        QCOMPARE(Run.value("LD_LIBRARY_PATH"), QStringLiteral("/app/lib"));
    }

    void a_loader_variable_the_game_removes_is_removed_for_the_game_only()
    {
        QProcessEnvironment Run;
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, env({{"LD_PRELOAD", "/host/hook.so"}}));
        QCOMPARE(Game.size(), size_t(1));
        QCOMPARE(QString::fromStdString(Game[0].Name), QStringLiteral("LD_PRELOAD"));
        QVERIFY(!Game[0].Set);
        QCOMPARE(Run.value("LD_PRELOAD"), QStringLiteral("/host/hook.so"));
    }

    void loader_variables_the_game_leaves_alone_are_not_passed()
    {
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/app/lib"}});
        QVERIFY(SandboxLayer::TakeLoaderEnv(Run, env({{"LD_LIBRARY_PATH", "/app/lib"}})).empty());
        QCOMPARE(Run.value("LD_LIBRARY_PATH"), QStringLiteral("/app/lib"));
    }

    void a_runners_LD_AUDIT_and_tunables_are_taken_too()
    {
        QProcessEnvironment Run = env({{"LD_AUDIT", "/runner/audit.so"}, {"GLIBC_TUNABLES", "glibc.rtld.x=1"}, {"QT_X", "1"}});
        const auto Game = SandboxLayer::TakeLoaderEnv(Run, QProcessEnvironment());
        QCOMPARE(Game.size(), size_t(2));
        QVERIFY(!Run.contains("LD_AUDIT") && !Run.contains("GLIBC_TUNABLES"));
        QVERIFY(Run.contains("QT_X"));
    }

    // The launch's entry point: the run environment the init gets loses the game's loader path, the init's
    // arguments carry it.
    void a_launch_wraps_with_the_loader_path_moved_to_the_init()
    {
#ifdef _WIN32
        QSKIP("the re-invoked init is the Linux bwrap backend");
#else
        QProcessEnvironment Run = env({{"LD_LIBRARY_PATH", "/runner/lib"}, {"HOME", "/home/u"}});
        std::string Program = "/opt/game/run.sh"; QStringList Args;
        SandboxLayer::WrapGame(SandboxLayer::Options{}, Program, Args, Run, env({{"HOME", "/home/u"}}));
        QVERIFY(!Run.contains("LD_LIBRARY_PATH"));
        const int E = Args.indexOf("--env");
        QVERIFY(E >= 0 && E < Args.lastIndexOf("--"));
        QCOMPARE(Args.at(E + 1), QStringLiteral("LD_LIBRARY_PATH=/runner/lib"));
#endif
    }

    void wrap_hands_the_games_loader_variables_to_the_init()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend; Windows sandboxing is the separate Sandboxie backend");
#endif
        SandboxLayer::Options O;
        O.GameEnv = {{"LD_LIBRARY_PATH", true, "/runner/lib"}, {"LD_PRELOAD", false, ""}};
        std::string Program = "/opt/game/run.sh"; QStringList Args{"x"};
        SandboxLayer::Wrap(O, Program, Args);
        const int Init = Args.indexOf("--sandbox-init"), Game = Args.lastIndexOf("--");
        const int E = Args.indexOf("--env"), U = Args.indexOf("--unenv");
        QVERIFY(Init >= 0 && E > Init && U > Init && E < Game && U < Game);
        QCOMPARE(Args.at(E + 1), QStringLiteral("LD_LIBRARY_PATH=/runner/lib"));
        QCOMPARE(Args.at(U + 1), QStringLiteral("LD_PRELOAD"));
        QCOMPARE(Args.at(Game + 1), QStringLiteral("/opt/game/run.sh"));
    }

    // The init itself: it starts with its own environment and the game it execs gets the passed values.
    void the_init_applies_the_loader_variables_to_the_game_it_execs()
    {
#ifdef _WIN32
        QSKIP("the sandbox init is Linux-only");
#else
        QTemporaryDir Dir;
        const std::string Out = (Dir.path() + "/env.txt").toStdString();
        const std::string Script = "printf '%s|%s' \"$LD_LIBRARY_PATH\" \"${LD_PRELOAD-unset}\" > '" + Out + "'";
        const pid_t Pid = fork();
        if (Pid == 0)
        {
            setenv("LD_PRELOAD", "", 1);   // present in the init's own environment, removed for the game
            std::vector<std::string> A{"VidyaGod", "--sandbox-init", "--env", "LD_LIBRARY_PATH=/runner/lib",
                                       "--unenv", "LD_PRELOAD", "--", "/bin/sh", "-c", Script};
            std::vector<char *> V; for (auto &S : A) V.push_back(S.data()); V.push_back(nullptr);
            _exit(SandboxLayer::RunSandboxInit(int(A.size()), V.data()));
        }
        int Status = 0; waitpid(Pid, &Status, 0);
        QVERIFY(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
        QFile F(QString::fromStdString(Out)); QVERIFY(F.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(F.readAll()), QStringLiteral("/runner/lib|unset"));
#endif
    }

    // The real home is not one of the sandbox's writable binds: a runner that writes ~/.config wrote the user's own
    // ~/.config through it (2026-10-21: ~/.cache/bbport, ~/.cache/JNA). The session still is.
    void the_real_home_is_not_a_writable_bind()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend");
#else
        QTemporaryDir Real(fakeHomeTemplate()), Temp;
        ScopedHome H(Real.path());
        ContainerParams CP = params({});
        CP.TempPath = Temp.path().toStdString();
        CP.UserDataPath = (Real.path() + "/.VidyaGod/USERDATA/game/DefaultInstance").toStdString();
        std::filesystem::create_directories(CP.UserDataPath);
        SandboxLayer::Options O = SandboxLayer::FromParams(CP);
        std::string Program = "/bin/true"; QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        const auto B = binds(Args);
        for (const auto &[Src, Dst] : B) QVERIFY2(Src != Real.path() && Dst != Real.path(), qPrintable(Src));
        QVERIFY(B.contains({Temp.path(), Temp.path()}));
        QCOMPARE(QString::fromStdString(O.Home), Temp.path() + "/HOME");
#endif
    }

    // Of the instance folder only the kept folders are bound: the folder itself holds instance.json (VARIABLES,
    // RUNNER_CHAIN) and the stores the launcher writes outside the sandbox (kept files, REGKEYS), none of them the
    // game's to change (spec 17.3). A kept folder that is a link is not bound either: bwrap refuses to mount on one.
    // Teeth: bind UserDataPath whole (it shows up); bind every kept target (the link shows up, the launch fails).
    void only_the_kept_folders_of_the_instance_are_bound()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend");
#else
        QTemporaryDir Real(fakeHomeTemplate()), Temp;
        ScopedHome H(Real.path());
        ContainerParams CP = params({});
        CP.TempPath = Temp.path().toStdString();
        CP.UserDataPath = (Real.path() + "/.VidyaGod/USERDATA/game/DefaultInstance").toStdString();
        CP.KeepDirs = {{"pfx/drive_c/game/saves", "Saves", true}, {"pfx/drive_c/game/maps", "Maps", true}};
        std::filesystem::create_directories(CP.UserDataPath / "Saves");
        std::filesystem::create_symlink(Real.path().toStdString(), CP.UserDataPath / "Maps");
        SandboxLayer::Options O = SandboxLayer::FromParams(CP);
        std::string Program = "/bin/true"; QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        const auto B = binds(Args);
        const QString Inst = QString::fromStdString(CP.UserDataPath.string());
        QVERIFY(B.contains({Inst + "/Saves", Inst + "/Saves"}));
        for (const auto &[Src, Dst] : B)
            QVERIFY2(Src != Inst && !Src.startsWith(Inst + "/Maps") && Dst != Inst, qPrintable(Src));
#endif
    }

    void the_launch_environment_points_home_and_xdg_at_the_private_home()
    {
#ifdef _WIN32
        QSKIP("the private home is the Linux backend");
#else
        QTemporaryDir Real(fakeHomeTemplate());
        SandboxLayer::Options O;
        O.Home = "/tmp/vg/game/inst/HOME"; O.RealHome = Real.path().toStdString();
        QProcessEnvironment E = env({{"HOME", Real.path()}, {"XDG_CONFIG_HOME", Real.path() + "/.config"}});
        SandboxLayer::HomeEnv(O, E);
        QCOMPARE(E.value("HOME"), QStringLiteral("/tmp/vg/game/inst/HOME"));
        QCOMPARE(E.value("XDG_CONFIG_HOME"), QStringLiteral("/tmp/vg/game/inst/HOME/.config"));
        QCOMPARE(E.value("XDG_DATA_HOME"), QStringLiteral("/tmp/vg/game/inst/HOME/.local/share"));
        QCOMPARE(E.value("XDG_STATE_HOME"), QStringLiteral("/tmp/vg/game/inst/HOME/.local/state"));
        QCOMPARE(E.value("XDG_CACHE_HOME"), QStringLiteral("/tmp/vg/game/inst/HOME/.cache"));
        QVERIFY(!E.contains("XAUTHORITY"));   // no real ~/.Xauthority: nothing to point at

        // Xlib looks for its cookie in $HOME/.Xauthority when XAUTHORITY is unset: point it at the real one.
        QFile Cookie(Real.path() + "/.Xauthority");
        QVERIFY(Cookie.open(QIODevice::WriteOnly));
        Cookie.close();
        QProcessEnvironment X;
        SandboxLayer::HomeEnv(O, X);
        QCOMPARE(X.value("XAUTHORITY"), Real.path() + "/.Xauthority");
        QProcessEnvironment Set = env({{"XAUTHORITY", "/run/user/1000/xauth_abc"}});
        SandboxLayer::HomeEnv(O, Set);
        QCOMPARE(Set.value("XAUTHORITY"), QStringLiteral("/run/user/1000/xauth_abc"));
#endif
    }

    // The GPU driver's shader cache stays the real one (else every launch recompiles every shader), bound where the
    // driver looks under the private home, and only the ones that exist.
    void the_gpu_shader_caches_are_carried_into_the_private_home()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend");
#else
        QTemporaryDir Real(fakeHomeTemplate()), Temp;
        std::filesystem::create_directories((Real.path() + "/.cache/nvidia").toStdString());
        std::filesystem::create_directories((Real.path() + "/.cache/mesa_shader_cache_db").toStdString());
        ScopedHome H(Real.path());
        ContainerParams CP = params({});
        CP.TempPath = Temp.path().toStdString();
        SandboxLayer::Options O = SandboxLayer::FromParams(CP);
        std::string Program = "/bin/true"; QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        const auto B = binds(Args);
        const int Nv = B.indexOf({Real.path() + "/.cache/nvidia", Temp.path() + "/HOME/.cache/nvidia"});
        const int Mesa = B.indexOf({Real.path() + "/.cache/mesa_shader_cache_db", Temp.path() + "/HOME/.cache/mesa_shader_cache_db"});
        QVERIFY(Nv >= 0 && Mesa >= 0);
        QVERIFY(Nv > B.indexOf({"/tmp", "/tmp"}));   // a later /tmp bind would cover them
        for (const auto &[Src, Dst] : B)
            QVERIFY2(!Src.endsWith("/.nv") && !Src.endsWith("/mesa_shader_cache"), qPrintable(Src));   // absent: not bound
#endif
    }

    // The driver's cache root is the launcher's own XDG_CACHE_HOME when that is absolute; a relative one is invalid
    // (XDG spec) and falls back to ~/.cache. NVIDIA's ~/.nv is carried to the same spot in the private home.
    void the_shader_cache_root_follows_the_users_xdg_cache_home()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend");
#else
        QTemporaryDir Real(fakeHomeTemplate()), Temp;
        const QString R = Real.path(), Home = Temp.path() + "/HOME";
        for (const char *D : {"/xdgcache/nvidia", "/.cache/nvidia", "/.nv"})
            std::filesystem::create_directories((R + D).toStdString());
        ContainerParams CP = params({});
        CP.TempPath = Temp.path().toStdString();
        const auto wrapped = [&]{
            SandboxLayer::Options O = SandboxLayer::FromParams(CP);
            std::string Program = "/bin/true"; QStringList Args;
            SandboxLayer::Wrap(O, Program, Args);
            return binds(Args);
        };
        {
            ScopedHome H(R, R + "/xdgcache");
            const auto B = wrapped();
            QVERIFY(B.contains({R + "/xdgcache/nvidia", Home + "/.cache/nvidia"}));
            QVERIFY(!B.contains({R + "/.cache/nvidia", Home + "/.cache/nvidia"}));
            QVERIFY(B.contains({R + "/.nv", Home + "/.nv"}));
        }
        {
            ScopedHome H(R, "xdgcache");   // relative: ignored
            QVERIFY(wrapped().contains({R + "/.cache/nvidia", Home + "/.cache/nvidia"}));
        }
#endif
    }

    // The program controls its home, including replacing it with a link: teardown must not walk where that points.
    void leftovers_never_follow_a_link()
    {
        QTemporaryDir Elsewhere, Temp;
        QVERIFY(touch(Elsewhere.path() + "/secret"));
        const QString Home = Temp.path() + "/HOME";
        QVERIFY(QFile::link(Elsewhere.path(), Home));                                      // the home itself a link
        QVERIFY(SandboxLayer::HomeLeftovers(Home.toStdString()).empty());
        QVERIFY(QFile::remove(Home));
        QVERIFY(QDir().mkpath(Home + "/.config"));
        QVERIFY(QFile::link(Elsewhere.path(), Home + "/.config/linked"));                  // a link inside it
        QCOMPARE(SandboxLayer::HomeLeftovers(Home.toStdString()), std::vector<std::string>{".config/linked"});
    }

    void leftovers_are_capped_and_names_are_made_printable()
    {
        QTemporaryDir Temp;
        const QString Home = Temp.path() + "/HOME";
        QVERIFY(QDir().mkpath(Home));
        for (const char *N : {"a", "b", "c", "d"}) QVERIFY(touch(Home + "/" + N));
        QCOMPARE(SandboxLayer::HomeLeftovers(Home.toStdString(), 3).size(), size_t(3));
        QVERIFY(touch(Home + "/e\n\x1b[31m"));
        const auto All = SandboxLayer::HomeLeftovers(Home.toStdString());
        QVERIFY(std::find(All.begin(), All.end(), "e??[31m") != All.end());
    }

    // The real thing: bwrap runs this binary as the sandbox init (main() below), which execs a shell that tries to
    // write the real home and the places a program is meant to write.
    void a_sandboxed_launch_cannot_write_the_real_home()
    {
#ifdef _WIN32
        QSKIP("the bwrap sandbox is Linux-only");
#else
        if (!SandboxLayer::Available()) QSKIP("bwrap cannot create a sandbox on this machine");
        QTemporaryDir Real(fakeHomeTemplate()), Temp;
        const QString R = Real.path();
        std::filesystem::create_directories((R + "/.cache/nvidia").toStdString());
        std::filesystem::create_directories((R + "/.config").toStdString());
        ScopedHome H(R);
        ContainerParams CP = params({});
        CP.TempPath = Temp.path().toStdString();
        CP.UserDataPath = (R + "/.VidyaGod/USERDATA/game/DefaultInstance").toStdString();
        CP.KeepDirs = {{"pfx/drive_c/game/saves", "Saves", true}};
        std::filesystem::create_directories(CP.UserDataPath / "Saves");
        { std::ofstream(CP.UserDataPath / "instance.json") << "{}"; }
        SandboxLayer::Options O = SandboxLayer::FromParams(CP);

        QProcessEnvironment Run = QProcessEnvironment::systemEnvironment();
        Run.insert("VG_REAL", R);
        Run.insert("VG_INST", QString::fromStdString(CP.UserDataPath.string()));
        Run.insert("VG_OUT", Temp.path() + "/result");
        SandboxLayer::HomeEnv(O, Run);
        const QString Script = QStringLiteral(R"SH(
            r=
            mount -o remount,bind,rw / 2>/dev/null && r="${r}root-remounted-writable "
            cap=$(awk '/^CapEff/ { print $2 }' /proc/self/status)
            [ $((0x$cap & 0x200000)) -eq 0 ] || r="${r}game-has-CAP_SYS_ADMIN "
            for d in "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME"; do
                [ -d "$d" ] || r="${r}missing:$d "
            done
            touch "$VG_REAL/leak" 2>/dev/null && r="${r}real-home-writable "
            mkdir -p "$VG_REAL/.config/game" 2>/dev/null && r="${r}real-config-writable "
            [ "$HOME" = "$VG_REAL" ] && r="${r}home-is-real "
            touch "$HOME/home-file" || r="${r}private-home-unwritable "
            mkdir -p "$XDG_CONFIG_HOME/game" "$XDG_DATA_HOME/game" "$XDG_STATE_HOME/game" || r="${r}xdg-unwritable "
            touch "$XDG_CACHE_HOME/nvidia/shader" || r="${r}shader-cache-not-carried "
            touch "$VG_INST/Saves/save" || r="${r}kept-folder-unwritable "
            echo '{"VARIABLES":{}}' 2>/dev/null > "$VG_INST/instance.json" && r="${r}instance-config-writable "
            ln -s "$VG_REAL/.bashrc" "$VG_INST/Config" 2>/dev/null && r="${r}instance-folder-writable "
            printf '%s' "$r" > "$VG_OUT"
        )SH");
        std::string Program = "/bin/sh"; QStringList Args{"-c", Script};
        SandboxLayer::WrapGame(O, Program, Args, Run, QProcessEnvironment::systemEnvironment());
        QProcess P;
        P.setProcessEnvironment(Run);
        P.setProcessChannelMode(QProcess::ForwardedChannels);
        P.start(QString::fromStdString(Program), Args);
        QVERIFY(P.waitForFinished(30000));
        QCOMPARE(P.exitCode(), 0);

        QFile Out(Temp.path() + "/result");
        QVERIFY2(Out.open(QIODevice::ReadOnly), "the sandboxed shell never ran");
        QCOMPARE(QString::fromUtf8(Out.readAll()), QString());
        QVERIFY(!QFile::exists(R + "/leak") && !QFile::exists(R + "/.config/game"));
        QVERIFY(QFile::exists(Temp.path() + "/HOME/home-file"));
        QVERIFY(QFile::exists(Temp.path() + "/HOME/.config/game"));
        QVERIFY(QFile::exists(R + "/.cache/nvidia/shader"));
        QVERIFY(QFile::exists(R + "/.VidyaGod/USERDATA/game/DefaultInstance/Saves/save"));

        // Teardown names what the program left in its private home: the file, not the empty folders around it, and
        // not the shader cache (the real one, carried in: on the host it is an empty mount point).
        QCOMPARE(SandboxLayer::HomeLeftovers(O.Home), std::vector<std::string>{"home-file"});
#endif
    }

    // The Sandboxie box (Windows): measured on Windows 11 + Sandboxie-Plus 1.18.8 with a WinFsp mount, a boxed program
    // reaches the mount only through a grant naming the mountpoint itself, backslashed, and the volume it names at this
    // launch; SbieIni "set" replaces every earlier value, "append" adds one (the box is re-used by the next launch, whose
    // grants used to pile up).
    void box_grants_each_mountpoint_and_its_volume_once_backslashed_replacing_the_old()
    {
        SandboxLayer::Options O;
        O.Mounts = {{"C:/t/vidyagodfs.spec.json", "C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance\\RUNTIME"},
                    {"C:/t/vidyagodfs.runner.spec.json", "C:/Users/u/Temp/VidyaGod/9/DefaultInstance/RUNNER/"},
                    {"C:/t/again.spec.json", "c:\\users\\u\\temp\\vidyagod\\9\\defaultinstance\\RUNTIME"}};
        // Reached through the mount, never granted: a grant there lets the game write behind the mount's back.
        O.WritableBinds = {"C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance", "C:/vg/USERDATA\\9\\DefaultInstance/saves"};
        O.WorkDir = "C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance\\RUNTIME\\drive_c/9";   // inside the mount: no grant
        O.BoxRoot = "C:/Users/u/Temp/VidyaGod/9/DefaultInstance/BOX/";
        const auto Vol = [](const std::string &P) {
            return P.find("RUNTIME") != std::string::npos ? std::string("\\Device\\Volume{9a}\\") : std::string();
        };
        const std::vector<QStringList> Want = {
            {"set", "Box", "Enabled", "y"},
            {"set", "Box", "AutoDelete", "n"},   // the box prefix is filled before the game: never deleted in between
            {"set", "Box", "FileRootPath", "C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance\\BOX"},
            {"set", "Box", "DropAdminRights", "y"},
            {"set", "Box", "OpenFilePath", "C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance\\RUNTIME"},
            {"append", "Box", "OpenFilePath", "\\Device\\Volume{9a}"},
            {"append", "Box", "OpenFilePath", "C:\\Users\\u\\Temp\\VidyaGod\\9\\DefaultInstance\\RUNNER"},
        };
        QCOMPARE(SandboxLayer::BoxIniCommands("Box", O, Vol), Want);

        // No box root of its own: Sandboxie's default, deleted with its last program. Nothing to grant still clears
        // what an earlier launch granted.
        const std::vector<QStringList> Bare = SandboxLayer::BoxIniCommands("Box", SandboxLayer::Options{},
                                                                           [](const std::string &) { return std::string(); });
        QCOMPARE(Bare, (std::vector<QStringList>{{"set", "Box", "Enabled", "y"}, {"set", "Box", "AutoDelete", "y"},
                                                {"set", "Box", "FileRootPath", QString()}, {"set", "Box", "DropAdminRights", "y"},
                                                {"set", "Box", "OpenFilePath", QString()}}));
    }

    // Start.exe's arguments (Windows): the program path backslashed (a '/' in it was "cannot find the file", in a dialog
    // that waited for a click) and no working-directory option (it has none: "/wrk:" made it exit 1 without the game).
    void box_start_args_backslash_the_program_and_keep_its_arguments()
    {
        const QStringList A = SandboxLayer::BoxStartArgs("Box", "C:\\T\\RUNTIME\\drive_c/9/probe.exe",
                                                         {"C:\\T\\RUNTIME\\drive_c\\9", "a/b"});
        QCOMPARE(A, QStringList({"/box:Box", "/silent", "/wait", "C:\\T\\RUNTIME\\drive_c\\9\\probe.exe",
                                 "C:\\T\\RUNTIME\\drive_c\\9", "a/b"}));
    }

    void net_mode_from_var()
    {
        QCOMPARE(SandboxLayer::FromParams(params({})).Net == SandboxLayer::NetMode::Host, true);
        QCOMPARE(SandboxLayer::FromParams(params({{"VIDYAGOD_SANDBOX_NET", "isolated"}})).Net
                     == SandboxLayer::NetMode::Isolated, true);
    }

    void wrap_nests_userns_readonly_root_and_sandbox_init()
    {
#ifdef _WIN32
        QSKIP("bwrap argv is the Linux backend; Windows sandboxing is the separate Sandboxie backend (sandboxlayer_win.cpp)");
#endif
        SandboxLayer::Options O = SandboxLayer::FromParams(params({}));
        O.Mounts.push_back({"/tmp/spec.json", "/tmp/mnt"});
        O.WorkDir = "/tmp/mnt/game";
        std::string Program = "/opt/game/run.sh";
        QStringList Args = {"--fullscreen", "level1"};
        SandboxLayer::Wrap(O, Program, Args);

        QCOMPARE(QString::fromStdString(Program), QStringLiteral("bwrap"));
        // One hermetic userns with CAP_SYS_ADMIN retained (so the init can FUSE-mount), host root read-only.
        QVERIFY(Args.contains("--unshare-user"));
        QVERIFY(Args.contains("--cap-add") && Args.contains("CAP_SYS_ADMIN"));
        const int Ro = Args.indexOf("--ro-bind");
        QVERIFY(Ro >= 0 && Args.at(Ro + 1) == "/" && Args.at(Ro + 2) == "/");
        // Host net mode → NO --unshare-net.
        QVERIFY(!Args.contains("--unshare-net"));
        // The payload re-invokes the sandbox init, passing the deferred mount + the chdir.
        QVERIFY(Args.contains("--sandbox-init"));
        const int Mnt = Args.indexOf("--mount");
        QVERIFY(Mnt >= 0 && Args.at(Mnt + 1) == "/tmp/spec.json" && Args.at(Mnt + 2) == "/tmp/mnt");
        const int Cd = Args.indexOf("--chdir");
        QVERIFY(Cd >= 0 && Args.at(Cd + 1) == "/tmp/mnt/game");
        // The real game command is preserved verbatim after the LAST "--".
        const int Sep = Args.lastIndexOf("--");
        QVERIFY(Sep >= 0);
        QCOMPARE(Args.at(Sep + 1), QStringLiteral("/opt/game/run.sh"));
        QCOMPARE(Args.at(Sep + 2), QStringLiteral("--fullscreen"));
        QCOMPARE(Args.at(Sep + 3), QStringLiteral("level1"));
    }

    void isolated_unshares_net_and_keeps_net_admin()
    {
#ifdef _WIN32
        QSKIP("bwrap --unshare-net is the Linux backend; Windows sandboxing is the separate Sandboxie backend");
#endif
        SandboxLayer::Options O = SandboxLayer::FromParams(
            params({{"VIDYAGOD_SANDBOX_NET", "isolated"}}));
        O.TunName = "vg-abcdef"; O.TunCidr = "10.66.7.2/24"; O.TunSock = "/tmp/t.sock";
        std::string Program = "/bin/true";
        QStringList Args;
        SandboxLayer::Wrap(O, Program, Args);
        QVERIFY(Args.contains("--unshare-net"));
        QVERIFY(Args.contains("CAP_NET_ADMIN"));
        // --unshare-net must come before the payload separator.
        QVERIFY(Args.indexOf("--unshare-net") < Args.indexOf("--sandbox-init"));
        // The TUN handoff is passed to the init.
        const int Tun = Args.indexOf("--tun");
        QVERIFY(Tun >= 0 && Args.at(Tun + 1) == "vg-abcdef" && Args.at(Tun + 2) == "10.66.7.2/24" && Args.at(Tun + 3) == "/tmp/t.sock");
    }
};

// The real sandboxed run re-invokes this binary as bwrap's payload, so it answers --sandbox-init the way the app's
// main() does.
int main(int argc, char **argv)
{
    if (SandboxLayer::IsSandboxInit(argc, argv)) return SandboxLayer::RunSandboxInit(argc, argv);
    QCoreApplication App(argc, argv);
    SandboxTest T;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&T, argc, argv);
}
#include "test_sandbox.moc"
