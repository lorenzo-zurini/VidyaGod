#ifndef PACKAGEEDITORMDICHILD_H
#define PACKAGEEDITORMDICHILD_H

#include <QWidget>

namespace Ui {
class PackageEditorMDIChild;
}

class PackageEditorMDIChild : public QWidget
{
    Q_OBJECT

public:
    explicit PackageEditorMDIChild(QWidget *parent = nullptr);
    ~PackageEditorMDIChild();

private:
    Ui::PackageEditorMDIChild *ui;
};

#endif // PACKAGEEDITORMDICHILD_H
