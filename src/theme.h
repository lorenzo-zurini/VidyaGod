#ifndef THEME_H
#define THEME_H

class QApplication;
class QWidget;

// The app's one look: Fusion with the navy palette the Library and the editor always had, on every surface (Settings
// and the dialogs too), whatever the desktop's own theme is — and the few shared styles (the Settings sidebar, the
// banner a page shows when networking is off, the details of the launch window).
namespace Theme
{
void Apply(QApplication &App);
// Page as a Settings page shows it: its content no wider than Max (a line of text or a field stretched over a wide
// window reads badly), left-aligned, the rest of the width empty.
QWidget *Capped(QWidget *Page, int Max = 960);
}

#endif // THEME_H
