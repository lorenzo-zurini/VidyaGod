#include "quitsignals.h"

#include "containerwrapper.h"
#include "commonutils.h"   // Log*

#include <QCoreApplication>
#include <QMetaObject>

#include <mutex>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace
{
int g_Pipe[2] = {-1, -1};
std::mutex g_QuitMtx;
std::function<void()> g_Quit;

//Async-signal-safe: one write, then all three back to their default action (sigaction is async-signal-safe), so a
//second signal of any kind ends the process at once.
void OnSignal(int Sig)
{
    struct sigaction Def {};
    Def.sa_handler = SIG_DFL;
    sigemptyset(&Def.sa_mask);
    for (int S : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(S, &Def, nullptr);
    const unsigned char B = static_cast<unsigned char>(Sig);
    [[maybe_unused]] const ssize_t N = ::write(g_Pipe[1], &B, 1);
}

void QuitGui()
{
    //Read now, on the way out: the app may have been torn down while the sessions were ending.
    QCoreApplication *App = QCoreApplication::instance();
    if (!App) return;
    QMetaObject::invokeMethod(App, [] {
        std::function<void()> Quit;
        { std::lock_guard<std::mutex> G(g_QuitMtx); Quit = g_Quit; }
        if (Quit) Quit();
        QCoreApplication::exit(0);
    }, Qt::QueuedConnection);
}

void Handle(int Sig)
{
    const char *Name = Sig == SIGTERM ? "SIGTERM" : Sig == SIGINT ? "SIGINT" : "SIGHUP";
    //Latched first, under the live set's lock: a launch reaching its build after this is refused instead of being cut
    //off mid-mount by the quit.
    ContainerWrapper::StopAll();
    if (ContainerWrapper::LiveCount() == 0)
    {
        LogOut("QuitSignals", std::string(Name) + " — no game running, quitting.");
        if (QCoreApplication::instance()) { QuitGui(); return; }
        std::raise(Sig);                               // the default action, as before
        return;
    }
    LogOut("QuitSignals", std::string(Name) + " — ending the running game; its session is stored and removed before VidyaGod quits.");
    if (!QCoreApplication::instance()) return;          // a headless launch returns by itself once its Cleanup is done
    ContainerWrapper::WaitNoneLive();
    QuitGui();
}
}

void QuitSignals::SetQuitAction(std::function<void()> Quit)
{
    std::lock_guard<std::mutex> G(g_QuitMtx);
    g_Quit = std::move(Quit);
}

void QuitSignals::Install()
{
    if (g_Pipe[0] >= 0) return;
    if (::pipe2(g_Pipe, O_CLOEXEC) != 0)
    {
        LogWarn("QuitSignals::Install", "No pipe — a quit signal ends VidyaGod without ending its game sessions first.");
        return;
    }
    struct sigaction Act {};
    Act.sa_handler = OnSignal;
    Act.sa_flags   = SA_RESTART;
    sigemptyset(&Act.sa_mask);
    for (int S : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(S, &Act, nullptr);
    std::thread([] {
        unsigned char B = 0;
        ssize_t N;
        do N = ::read(g_Pipe[0], &B, 1); while (N < 0 && errno == EINTR);
        if (N == 1) Handle(B);
    }).detach();
}
#else
void QuitSignals::Install() {}
void QuitSignals::SetQuitAction(std::function<void()>) {}
#endif
