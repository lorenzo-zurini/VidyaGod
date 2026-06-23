#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <QString>

// ---------------------------------------------------------------------------
// AutoStart — manages the XDG "start on login" entry: a .desktop file in ~/.config/autostart that the desktop
// environment runs at login. The entry launches VidyaGod with --tray so it comes up quietly in the system tray
// (background seeding from login). The Exec target is resolved for the way the app actually runs: $APPIMAGE for an
// AppImage, `flatpak run <id>` under flatpak, else the binary path.
//
// Only meaningful in Normal run mode (see AppPaths::AutostartSupported / [[project_four_run_modes]]); the Settings
// page greys the toggle out otherwise. (The eventual flatpak should prefer the xdg-desktop-portal Background API
// over a raw autostart file — future.)
// ---------------------------------------------------------------------------
namespace AutoStart
{
//True if the autostart .desktop entry currently exists (and isn't disabled).
bool IsEnabled();

//Create (On=true) or remove (On=false) the autostart entry. Returns false (with *Error) on a filesystem failure.
bool SetEnabled(bool On, QString * Error = nullptr);
}

#endif // AUTOSTART_H
