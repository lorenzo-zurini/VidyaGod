#ifndef PACKAGESVIEW_H
#define PACKAGESVIEW_H

#include <QWidget>

class AppModel;
class QScrollArea;

// ---------------------------------------------------------------------------
// PackagesView — the "Installed Packages" Settings page: the +Add Local Package / Package Editor buttons and the
// list of hydrated library packages (each with a Remove button). Reads the catalog/config through the AppModel,
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
    void addLocalPackages();   // scan a chosen dir for packages + add the new ones to the library

    AppModel &Model;
    QScrollArea *PackagesScrollArea = nullptr;
};

#endif // PACKAGESVIEW_H
