#ifndef JSONRAWEDITOR_H
#define JSONRAWEDITOR_H

#include <QWidget>

class PackageEditorModel;
class QComboBox;
class QTextEdit;
class QPushButton;

// ---------------------------------------------------------------------------
// JsonRawEditor — the "JSON" tab: pick a node from the combo and edit its raw JSON, then Save Node. Validates the
// text live (red background + disabled Save on parse error). Reads/writes the working document through the model;
// re-shows the selected node's JSON whenever the model reports a disk write. (The shell recreates this widget on a
// structural rebuild, so it always reflects the current node set.)
// ---------------------------------------------------------------------------
class JsonRawEditor : public QWidget
{
    Q_OBJECT
public:
    explicit JsonRawEditor(PackageEditorModel * model, QWidget * parent = nullptr);

private slots:
    void refreshText();    // re-show the selected node's JSON (was PackageEditor::RefreshJSONView)
    void onTextChanged();  // live JSON validity → style + Save enabled (was JSONQTextEditChanged)
    void onSavePressed();  // parse + replace the node through the model (was SaveJSONButtonPressed)

private:
    PackageEditorModel * Model = nullptr;
    QComboBox *          FileCombo = nullptr;
    QTextEdit *          Text = nullptr;
    QPushButton *        SaveBtn = nullptr;
};

#endif // JSONRAWEDITOR_H
