#ifndef DEPENDENCIESPAGE_H
#define DEPENDENCIESPAGE_H

#include <QWidget>

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// DependenciesPage — the Settings "Dependencies" page: a per-item ✓/✗/⚠ system health check for the tools + libraries
// whose absence crashes Proton/Wine games (the #1 cause of Linux game crashes), grouped by category. Warn-only: it
// names the package that provides each missing item for the detected distro, but never installs anything. Detection
// lives in DepCheck (Qt-free); this is pure presentation + a Re-check button.
// ---------------------------------------------------------------------------
class DependenciesPage : public QWidget
{
    Q_OBJECT
public:
    DependenciesPage(AppModel & model, QWidget * parent = nullptr);

private:
    void rebuild();               // probe the host + (re)render every category
    QScrollArea * Scroll = nullptr;
};

#endif // DEPENDENCIESPAGE_H
