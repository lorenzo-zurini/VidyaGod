#ifndef VALIDATIONPANEL_H
#define VALIDATIONPANEL_H

#include <QGroupBox>
#include <string>

class PackageEditorModel;
class QListWidget;
class QLabel;

// The package's validation results, one row per problem; clicking a row about a node selects and frames that node.
class ValidationPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit ValidationPanel(PackageEditorModel * model, QWidget * parent = nullptr);
    //A message as a person reads it: the node named by its label, not its CID.
    static QString readable(const std::string & message);
    //...and a message tagged by CID alone ("node 'bafk...': ..."), named by the label of that node of the package.
    QString named(const std::string & message) const;
    QString labelOf(const QString & handle) const;   // "" when the package holds no such node

signals:
    void nodeClicked(const QString & handle);
    void fixClicked(const QString & action);       // a problem with a package-wide fix (PkgGraph::FixFor) was clicked

private slots:
    void refresh();

private:
    PackageEditorModel * Model = nullptr;
    QListWidget *        List  = nullptr;
    QLabel *             Hint  = nullptr;
};

#endif // VALIDATIONPANEL_H
