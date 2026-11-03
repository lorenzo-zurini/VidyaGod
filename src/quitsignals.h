#ifndef QUITSIGNALS_H
#define QUITSIGNALS_H

#include <functional>

// QuitSignals — what VidyaGod does when it is told to quit (SIGTERM, SIGINT, SIGHUP: a logout, Ctrl+C, a harness's
// TERM). With no game session in this process it quits as before (the GUI through its event loop, so it saves what it
// saves on a close; a headless run by the signal's default action). With sessions, it ends them as the user's Stop does
// (ContainerWrapper::StopAll): the game is killed and each launch runs its Cleanup — what the game kept is stored, the
// runtime is unmounted and removed — and then the GUI quits; a headless launch returns as after any run. A second
// signal of any of the three is not handled: it ends the process at once, as before. Linux only (Windows has no such
// signals to a GUI).
namespace QuitSignals
{
void Install();
//How the GUI quits for real (its tray Quit: no hiding to the tray, which would cancel the quit). Run on the GUI thread;
//without one the event loop is ended with QCoreApplication::exit, which nothing cancels.
void SetQuitAction(std::function<void()> Quit);
}

#endif // QUITSIGNALS_H
