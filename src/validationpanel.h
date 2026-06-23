#ifndef VALIDATIONPANEL_H
#define VALIDATIONPANEL_H

#include <QGroupBox>

class PackageEditorModel;
class QTextEdit;

// ---------------------------------------------------------------------------
// ValidationPanel — the persistent "Validation" box docked beneath the editor tabs. It renders the model's latest
// ValidateNodeGraph results (errors red, warnings amber, ✓ when clean) and self-refreshes on the model's
// validationChanged signal — the editor never pokes it.
// ---------------------------------------------------------------------------
class ValidationPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit ValidationPanel(PackageEditorModel * model, QWidget * parent = nullptr);

private slots:
    void refresh();

private:
    PackageEditorModel * Model = nullptr;
    QTextEdit *          View  = nullptr;
};

#endif // VALIDATIONPANEL_H
