#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <QDialog>
#include <QScreen>

#include <QLineEdit>
#include <QLabel>
#include <QLayout>
#include <QFormLayout>
#include <QStackedLayout>
#include <QBoxLayout>
#include <QBoxLayout>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QAbstractItemModel>

namespace Ui {
class PackageEditor;
}

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    explicit PackageEditor(QJsonDocument * GlobalConfigJSON, QWidget *parent = nullptr);
    ~PackageEditor();

private slots:
    void on_AddSubGameButton_clicked();
    void on_AddComponentButton_clicked();

private:
    bool SetupPackageDataFrame();

    Ui::PackageEditor *ui;
    QJsonDocument * GlobalConfigJSON;
};

#endif // PACKAGEEDITOR_H
