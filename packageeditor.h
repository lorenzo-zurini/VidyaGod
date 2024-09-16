#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <QDialog>
#include <QScreen>

namespace Ui {
class PackageEditor;
}

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    explicit PackageEditor(QWidget *parent = nullptr);
    ~PackageEditor();

private:
    Ui::PackageEditor *ui;
};

#endif // PACKAGEEDITOR_H
