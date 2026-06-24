#ifndef AUTHORINGSESSIONWINDOW_H
#define AUTHORINGSESSIONWINDOW_H

#include <QWidget>

#include <string>

class PackageEditorModel;
class AuthoringSessionModel;
class DeltaTree;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;

// ---------------------------------------------------------------------------
// AuthoringSessionWindow — a first-class top-level QWidget that hosts a live-runtime authoring session: a thin
// COMPOSITION ROOT. All state/threading lives in AuthoringSessionModel (the hub); this builds the subwidgets (runner
// picker + run tools, the checkable DeltaTree, the capture panel) and wires them to the model via signals/slots — the
// subwidgets never talk to each other or to the engine directly. Closing the window deletes the model, which ends the
// session (unmount + wipe).
// ---------------------------------------------------------------------------
class AuthoringSessionWindow : public QWidget
{
    Q_OBJECT
public:
    AuthoringSessionWindow(PackageEditorModel * Editor, const std::string & TargetNodeId, QWidget * parent = nullptr);

private:
    AuthoringSessionModel * Model = nullptr;
    DeltaTree *  Tree         = nullptr;
    QLabel *     StatusLabel  = nullptr;
    QLabel *     InfoLabel    = nullptr;
    QComboBox *  RunnerCombo  = nullptr;
    QComboBox *  TargetCombo  = nullptr;
    QLineEdit *  StripEdit    = nullptr;
    QLineEdit *  TargetEdit   = nullptr;
    QLineEdit *  DestNameEdit = nullptr;
    QPushButton * RunExeBtn = nullptr;
    QPushButton * BrowseBtn = nullptr;
    QPushButton * RegBtn    = nullptr;
    QPushButton * RefreshBtn = nullptr;
    QPushButton * CaptureFilesBtn = nullptr;
    QPushButton * CaptureRegBtn   = nullptr;
    QPushButton * EndBtn    = nullptr;
};

#endif // AUTHORINGSESSIONWINDOW_H
