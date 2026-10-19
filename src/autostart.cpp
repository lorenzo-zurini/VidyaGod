#include "autostart.h"
#include "commonutils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdlib>

#ifdef _WIN32
#include <QSettings>

namespace
{
// The per-user list of programs Windows starts at login: one value per program, its data the command line.
const QString RunKey   = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const QString RunValue = "VidyaGod";

// The installed exe, quoted (Program Files has spaces), with --tray so login starts go quiet to the tray.
QString LaunchCommand()
{
    return '"' + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + "\" --tray";
}
}

namespace AutoStart
{
bool IsEnabled()
{
    return QSettings(RunKey, QSettings::NativeFormat).contains(RunValue);
}

bool SetEnabled(bool On, QString * Error)
{
    QSettings Run(RunKey, QSettings::NativeFormat);
    if (On) Run.setValue(RunValue, LaunchCommand());
    else    Run.remove(RunValue);
    Run.sync();
    if (Run.status() != QSettings::NoError)
    { if (Error) *Error = "could not update the login entry under " + RunKey; return false; }
    LogOut("AutoStart", std::string(On ? "Added" : "Removed") + " the login entry " + RunKey.toStdString() + "\\VidyaGod");
    return true;
}
}

#else   // ---- Linux: the XDG .desktop autostart entry ----
namespace
{
// ~/.config/autostart/org.vidyagod.VidyaGod.desktop (honors XDG_CONFIG_HOME).
QString AutostartDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart";
}
QString AutostartFile()
{
    return AutostartDir() + "/org.vidyagod.VidyaGod.desktop";
}

// The command the autostart entry should run, resolved for how the app is actually packaged. Quote a path with
// spaces (Desktop-entry Exec quoting). Always appends --tray so login starts go quiet to the system tray.
QString LaunchCommand()
{
    QString Exec;
    if (const char * AppImage = std::getenv("APPIMAGE"))        Exec = QString::fromUtf8(AppImage);      // AppImage
    else if (const char * FpId = std::getenv("FLATPAK_ID"))     Exec = "flatpak run " + QString::fromUtf8(FpId);
    else                                                        Exec = QCoreApplication::applicationFilePath();  // dev/installed binary

    if (!Exec.startsWith("flatpak ") && Exec.contains(' ')) Exec = '"' + Exec + '"';
    return Exec + " --tray";
}
}

namespace AutoStart
{
bool IsEnabled()
{
    return QFile::exists(AutostartFile());
}

bool SetEnabled(bool On, QString * Error)
{
    const QString Path = AutostartFile();

    if (!On)
    {
        if (QFile::exists(Path) && !QFile::remove(Path))
        { if (Error) *Error = "could not remove " + Path; return false; }
        return true;
    }

    if (!QDir().mkpath(AutostartDir()))
    { if (Error) *Error = "could not create " + AutostartDir(); return false; }

    QFile F(Path);
    if (!F.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    { if (Error) *Error = "could not write " + Path; return false; }

    QTextStream Out(&F);
    Out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=VidyaGod\n"
        << "Comment=Game library + P2P distribution — runs in the tray to keep seeding\n"
        << "Exec=" << LaunchCommand() << "\n"
        << "Icon=org.vidyagod.VidyaGod\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
    F.close();
    LogOut("AutoStart", "Wrote login autostart entry: " + Path.toStdString());
    return true;
}
}
#endif
