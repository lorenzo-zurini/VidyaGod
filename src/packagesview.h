#ifndef PACKAGESVIEW_H
#define PACKAGESVIEW_H

#include <QWidget>

#include "packagecatalog.h"   // PackageCatalog::PackageUpdate

class QPushButton;

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// PackagesView — the "Installed Packages" Settings page: the packages installed here (PackageCatalog::InstalledPackages,
// from the index), by library, each with its size, versions and Open folder (and Forget, for one added from outside). Reads the catalog/config through the AppModel,
// removes via the model, and rebuilds its list on the model's catalogChanged signal. The Package Editor's save is
// routed through the app-wide MainWindow::RefreshPackage hook (which rebuilds the catalog + reloads prelaunches).
// ---------------------------------------------------------------------------
class PackagesView : public QWidget
{
    Q_OBJECT
public:
    PackagesView(AppModel &model, QWidget *parent = nullptr);

public slots:
    void rebuildList();        // (re)build the installed-packages list (on catalogChanged)

private:
    void buildUi();
    // The "Updates" section: installed packages a friend shares in another version, each with "Update from <friend>".
    // Found off the GUI thread (it builds each package's folder); applying asks first, showing what changes.
    void rebuildUpdates();
    void offerUpdate(const PackageCatalog::PackageUpdate &U, QPushButton *Button);
    void showList(const std::vector<PackageCatalog::InstalledPackage> &Pkgs);

    AppModel &Model;
    QScrollArea *PackagesScrollArea = nullptr;
    QWidget *UpdatesBox = nullptr;
    bool Listing = false, ListAgain = false;   // one installed-list scan at a time; a change meanwhile asks for one more
    unsigned UpdatesGeneration = 0;            // a refresh's result is shown only if no later refresh started
    bool Switching = false;                    // one switch at a time: every button waits while one runs
};

#endif // PACKAGESVIEW_H
