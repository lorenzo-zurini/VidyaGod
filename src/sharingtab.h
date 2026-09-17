#ifndef SHARINGTAB_H
#define SHARINGTAB_H

#include <QWidget>

class AppModel;
class QLabel;
class QPushButton;
class QLineEdit;

// SharingTab — the PUBLISH side of the IPNS library layer (project_ipns_friendcode_library). Your friend code IS your
// library address; this tab lets you (Verify &) Publish your libraries under it and back up / restore the identity key
// that address is derived from. The CLIENT side (browsing friends' libraries) stays in the Catalog as source sections.
class SharingTab : public QWidget
{
    Q_OBJECT
public:
    explicit SharingTab(AppModel & model, QWidget * parent = nullptr);
    void setActive(bool on);

private slots:
    void publishClicked();
    void addGameClicked();
    void backupIdentityClicked();
    void restoreIdentityClicked();
    void refresh();

private:
    void buildUi();

    AppModel &    Model;
    QLabel *      AddressValue = nullptr;
    QPushButton * CopyButton   = nullptr;
    QPushButton * PublishButton = nullptr;
    QLabel *      PublishStatus = nullptr;
    QLabel *      LibrariesLabel = nullptr;
    QLineEdit *   AddCidEdit    = nullptr;
    QPushButton * AddButton     = nullptr;
    QLabel *      AddStatus     = nullptr;
};

#endif // SHARINGTAB_H
