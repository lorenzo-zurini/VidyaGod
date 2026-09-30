#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QDialog>
#include <QString>

#include "nlohmann/json.hpp"

class PackageEditorModel;
class JsonRawEditor;
class PkgCanvasPanel;
class PkgActions;
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
    void validate();
    void fixCase();
    void seedAndShare();
    void updateTitle();
    void status(const QString & Text, int Ms = 5000);

    static PackageEditor * Live;                  // the one open editor, or null

    PackageEditorModel * Model  = nullptr;
    PkgCanvasPanel *     Canvas = nullptr;
    JsonRawEditor *      Json   = nullptr;
    PkgActions *         Actions = nullptr;
    QLabel *             StatusText = nullptr;
    QString              LastVerdict;                 // the validation summary last shown
    bool                 ValidationAsked = false;     // Validate was pressed: say the result even if unchanged
    QAction *            SaveAct = nullptr, * UndoAct = nullptr, * RedoAct = nullptr;
};

#endif // PACKAGEEDITOR_H
