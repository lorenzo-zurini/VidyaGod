#ifndef AUTHORINGSESSIONWINDOW_H
#define AUTHORINGSESSIONWINDOW_H

#include <QDialog>

#include <memory>
#include <string>

#include "authoringsession.h"

class PackageEditorModel;
class QLabel;
class QListWidget;
class QComboBox;
class QLineEdit;
class QPushButton;

// ---------------------------------------------------------------------------
// AuthoringSessionWindow — the live-runtime authoring UI (a first-class top-level window). It owns one
// AuthoringSession built from a target node's OWN closure and held mounted, and exposes the install→capture loop the
// pristine PERSIST default removed:
//   • run executables (installer / regedit / explorer) against the live mount,
//   • see the WRITELAYER delta (what those runs changed), select subtrees, and capture them into the bundle as a
//     VFSDirLayer with an EDITABLE TARGET (option c) + a smart default,
//   • capture the registry baseline-vs-now diff as RegEdit subcomponents.
// Wine-only affordances (Run EXE / regedit / explorer) are gated on the session being a wine prefix; the file capture
// is platform-agnostic. Closing the window ends the session (unmount + wipe).
// ---------------------------------------------------------------------------
class AuthoringSessionWindow : public QDialog
{
    Q_OBJECT
public:
    AuthoringSessionWindow(PackageEditorModel * Model, const std::string & TargetNodeId, QWidget * parent = nullptr);
    ~AuthoringSessionWindow() override;

protected:
    void closeEvent(QCloseEvent * e) override;   // ends the session (cleanup) on close

private:
    void initialStart();          // first build (auto runner) + one-time UI population
    void populateRunnerCombo();   // list runners serving the target node's platform
    void startSession(const std::vector<std::string> & RunnerChainIds);  // (re)build the runtime; empty = auto-resolve
    void refreshDelta();          // re-enumerate the WRITELAYER delta into the list
    void onRunExe();              // pick + run an installer in the live prefix
    void onCaptureFiles();        // copy selected delta subtrees → VFSDirLayer on the target node
    void onCaptureRegistry();     // baseline-vs-now diff → RegEdit on the target node
    void setBusy(bool On, const QString & What = QString());

    PackageEditorModel *               Model = nullptr;
    std::string                        TargetNodeId;
    std::unique_ptr<AuthoringSession>  Session;

    QLabel *      InfoLabel    = nullptr;
    QLabel *      StatusLabel  = nullptr;
    QListWidget * DeltaList    = nullptr;
    QComboBox *   TargetCombo  = nullptr;
    QLineEdit *   StripEdit    = nullptr;
    QLineEdit *   TargetEdit   = nullptr;
    QLineEdit *   DestNameEdit = nullptr;
    QComboBox *   RunnerCombo  = nullptr;
    bool          DefaultsDone = false;        // one-time target-combo + capture-defaults population
    QPushButton * RunExeBtn    = nullptr;
    QPushButton * RegBtn       = nullptr;
    QPushButton * BrowseBtn    = nullptr;
    QPushButton * RefreshBtn   = nullptr;
    QPushButton * CaptureFilesBtn = nullptr;
    QPushButton * CaptureRegBtn   = nullptr;
    QPushButton * EndBtn       = nullptr;
};

#endif // AUTHORINGSESSIONWINDOW_H
