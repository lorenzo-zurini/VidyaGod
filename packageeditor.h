#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <QDialog>
#include <QScreen>
#include <QDir>

#include <QLineEdit>
#include <QLabel>
#include <QLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedLayout>
#include <QBoxLayout>
#include <QGroupBox>

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
    explicit PackageEditor(QJsonDocument * GlobalConfigJSON, QDir PackageDir, QWidget *parent = nullptr);
    ~PackageEditor();

private slots:
    void on_AddSubGameButton_clicked();
    void on_AddComponentButton_clicked();

private:
    bool SetupPackageDataFrame();
    void SavePackage();

    QDir PackageDir;

    Ui::PackageEditor * ui;
    QJsonDocument * GlobalConfigJSON;
    QJsonDocument * MANIFESTJSON;
};

#endif // PACKAGEEDITOR_H
