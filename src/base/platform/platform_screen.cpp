// The host's screen, on every OS: Qt's when asked on the GUI thread, else the display server's.
#include "platform/platform.h"

#include <QGuiApplication>
#include <QScreen>
#include <QThread>

#include <cstdio>
#include <cstdlib>

namespace Platform {

void PrimaryScreen(std::string &Width, std::string &Height)
{
    if (qApp && QThread::currentThread() == qApp->thread())
        if (QScreen *Scr = QGuiApplication::primaryScreen())
        {
            Width  = std::to_string(Scr->geometry().width());
            Height = std::to_string(Scr->geometry().height());
            return;
        }
#ifndef _WIN32
    //Ask the display server directly — cheap, and it is what the game would have seen anyway.
    if (const char *Disp = std::getenv("DISPLAY"); Disp && *Disp)
        if (FILE *P = ::popen("xrandr --current 2>/dev/null | awk '/\\*/{print $1; exit}'", "r"))
        {
            char Buf[64] = {0};
            if (std::fgets(Buf, sizeof(Buf), P))
            {
                const std::string Mode(Buf);
                if (const size_t X = Mode.find('x'); X != std::string::npos)
                {
                    Width  = Mode.substr(0, X);
                    Height = Mode.substr(X + 1, Mode.find_first_not_of("0123456789", X + 1) - X - 1);
                }
            }
            ::pclose(P);
        }
#endif
}

} // namespace Platform
