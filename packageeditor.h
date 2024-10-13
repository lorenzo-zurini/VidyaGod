#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <QDialog>
#include <QScreen>
#include <QDir>
#include "QJsonModelEx/QJsonModel.hpp"

#include <QFileDialog>
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
#include <QItemDelegate>

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
    void on_SaveButton_clicked();

    void on_JSONTextEdit_textChanged();

private:
    bool InitMANIFESTJSON();
    void SavePackage();
    void RefreshJSONView();
    void InitPackage();

    QDir * PackageDir;
    QDir * MetadataDir;
    QDir * PackageFilesDir;

    Ui::PackageEditor * ui;
    QJsonDocument * MANIFESTJSON;
    QJsonDocument * GlobalConfigJSON;
};

#endif // PACKAGEEDITOR_H
