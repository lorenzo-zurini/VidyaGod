#ifndef JSONRAWEDITOR_H
#define JSONRAWEDITOR_H

#include <QWidget>

#include <string>

class PackageEditorModel;
class QLabel;
class QPlainTextEdit;
class QTimer;

// ---------------------------------------------------------------------------
// JsonRawEditor — the selected node's JSON, editable. Follows the canvas selection (by handle); valid edits apply to
// the document live (debounced, one undo step), invalid text is marked and never applied. The node's CID is shown
// and a save that renames the node keeps it selected.
// ---------------------------------------------------------------------------
class JsonRawEditor : public QWidget
{
    Q_OBJECT
public:
    explicit JsonRawEditor(PackageEditorModel * model, QWidget * parent = nullptr);

public slots:
    void showNode(const QString & handle);   // "" = nothing selected
    void refresh();                          // the node may have changed: show it (unless it is being typed into)

private slots:
    void onTextChanged();
    void onApplyTimeout();

private:
    PackageEditorModel * Model = nullptr;
    std::string          Handle;
    QLabel *             Title = nullptr;
    QLabel *             Status = nullptr;
    QPlainTextEdit *     Text = nullptr;
    QTimer *             ApplyTimer = nullptr;
    bool                 Applying = false;   // our own apply must not rewrite the text being typed
    std::string          Shown;              // the JSON text last shown (to tell "changed elsewhere" from "typed")
};

#endif // JSONRAWEDITOR_H
