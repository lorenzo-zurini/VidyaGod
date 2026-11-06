#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <functional>
#include <QDialog>
#include <QString>

#include "nlohmann/json.hpp"

class PackageEditorModel;
class JsonRawEditor;
class PkgCanvasPanel;
class PkgActions;
class PkgInsight;
class QAction;
class QLabel;
class QSplitter;

// ---------------------------------------------------------------------------
// PackageEditor — the package editor window: the node canvas, the selected node's JSON, validation, and the
// package-level commands (open, save, undo/redo, validate, fix case collisions, seed & share). One at a time (the
// canvas's Dear ImGui context is global) — OpenFor is the door.
// ---------------------------------------------------------------------------
class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent = nullptr, const QString & PackagePath = "");
    ~PackageEditor() override;

    static PackageEditor * OpenFor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent = nullptr,
                                   const QString & PackagePath = "", bool * Created = nullptr);
    QString bundleDir() const;
    //The one open editor (null when none) and what it edits: the CLI's `build` and `canvas` commands act on these.
    static PackageEditor * live() { return Live; }
    //What the host does with every editor OpenFor makes — wherever it is opened from (the library, a launch window, a
    //command): the app installs it once (MainWindow), so no opener can wire an editor differently.
    static void SetOnCreated(std::function<void(PackageEditor *)> Hook) { OnCreated = std::move(Hook); }
    PackageEditorModel * model() const { return Model; }
    PkgCanvasPanel * canvas() const { return Canvas; }
    PkgActions * actions() const { return Actions; }

signals:
    void packageSaved(const QString & PackagePath);
    void publishToLibraryRequested();

protected:
    void closeEvent(QCloseEvent * E) override;
    void reject() override;                       // Esc: close through the same unsaved-changes question

private:
    bool save();
    bool maybeSave(const QString & Why);          // unsaved changes: save / discard / cancel. False = cancelled
    void openPackage(const QString & Dir);
    void openPackagePicker();
    void newPackage();
    void validate();
    void fixCase();
    void seedAndShare();
    void updateTitle();
    void status(const QString & Text, int Ms = 5000);

    static PackageEditor * Live;                  // the one open editor, or null
    static inline std::function<void(PackageEditor *)> OnCreated;

    PackageEditorModel * Model  = nullptr;
    PkgCanvasPanel *     Canvas = nullptr;
    JsonRawEditor *      Json   = nullptr;
    PkgInsight *         Insight = nullptr;
    class QListWidget *  History = nullptr;               // the undo steps, by name: click one to go back there
    void refreshHistory();
    void offerDraft();                                    // a draft of unsaved work for the open package: restore it?
    PkgActions *         Actions = nullptr;
    QLabel *             StatusText = nullptr;
    QString              LastVerdict;                 // the validation summary last shown
    bool                 ValidationAsked = false;     // Validate was pressed: say the result even if unchanged
    QAction *            SaveAct = nullptr, * UndoAct = nullptr, * RedoAct = nullptr;
};

#endif // PACKAGEEDITOR_H
