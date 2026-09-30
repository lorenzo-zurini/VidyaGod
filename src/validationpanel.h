#ifndef VALIDATIONPANEL_H
#define VALIDATIONPANEL_H

#include <QGroupBox>

class PackageEditorModel;
class QListWidget;
class QLabel;

// The package's validation results, one row per problem; clicking a row about a node selects and frames that node.
class ValidationPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit ValidationPanel(PackageEditorModel * model, QWidget * parent = nullptr);

signals:
    void nodeClicked(const QString & handle);

private slots:
    void refresh();

private:
    PackageEditorModel * Model = nullptr;
    QListWidget *        List  = nullptr;
    QLabel *             Hint  = nullptr;
};

#endif // VALIDATIONPANEL_H
