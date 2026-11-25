#ifndef PACKAGESVIEW_H
#define PACKAGESVIEW_H

#include <QWidget>

#include <functional>

#include "packagecatalog.h"   // PackageCatalog::PackageUpdate

class QPushButton;
class QLabel;

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

    int updateScans() const { return UpdateScans; }   // test observability: "other versions" scans started
    //What Uninstall asks, from the verdict (PackageCatalog::JudgeUninstall): the button's own label ("" = no uninstall),
    //and the question with its consequence — what goes, where it comes back from, or that it never can.
    struct UninstallAsk { QString Button, Title, Text, Confirm; bool DefaultNo = false, Refused = false; };
    static UninstallAsk uninstallAsk(const PackageCatalog::UninstallVerdict &V, const QString &Title, unsigned long long Bytes);
    //The question for V (uninstallAsk) and, on a yes, the uninstall: once the verdict is in (judged off the GUI thread).
    void askUninstall(const PackageCatalog::UninstallVerdict &V, const QString &Dir, const QString &Title, unsigned long long Bytes);

public slots:
    void rebuildList();        // (re)build the installed-packages list (on catalogChanged; hidden: when next shown)

protected:
    void showEvent(QShowEvent *E) override;

private:
    void buildUi();
    // The "Updates" section: installed packages a peer shares in another version, each with "Update from <peer>".
    // Found off the GUI thread (it builds each package's folder); applying asks first, showing what changes.
    void rebuildUpdates();
    void offerUpdate(const PackageCatalog::PackageUpdate &U, QPushButton *Button);
    void askSwitch(const PackageCatalog::StagedUpdate &S, std::function<void(bool)> Answer);   // the plan, put to the person
    void showList(const std::vector<PackageCatalog::InstalledPackage> &Pkgs);

    AppModel &Model;
    QScrollArea *PackagesScrollArea = nullptr;
    QWidget *UpdatesBox = nullptr;
    bool Listing = false, ListAgain = false;   // one installed-list scan at a time; a change meanwhile asks for one more
    bool Updating = false, UpdateAgain = false; // one "other versions" scan at a time; asked meanwhile: one more after
    bool Stale = false;                        // asked while hidden: rebuilt when shown
    int UpdateScans = 0;
    QWidget *SwitchBar = nullptr;              // a switch under way: where it is, and its Cancel (UpdateService)
    QLabel *SwitchText = nullptr;
    QPushButton *SwitchCancel = nullptr;
};

#endif // PACKAGESVIEW_H
