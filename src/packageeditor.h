#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <iostream>
#include <QDialog>
#include <QScreen>

#include <QDir>
#include <QFileDialog>
#include <QLineEdit>

#include <QLabel>
#include <QLayout>
#include <QScrollArea>

#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedLayout>

#include <QBoxLayout>
#include <QGroupBox>
#include <QComboBox>

#include <QTextEdit>
#include <QAbstractItemModel>
#include <QItemDelegate>

#include <QSettings>

#include "filesystemoperations.h"
#include "jsonoperations.h"
#include "nlohmann/json.hpp"

namespace Ui {
class PackageEditor;
}

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr);
    static QString UnquoteString(QString InputString);
    static nlohmann::ordered_json RegFileToJSON(QFile RegFile);
    static nlohmann::ordered_json SubtractJSON(nlohmann::ordered_json OldJSON, nlohmann::ordered_json NewJSON);
    static nlohmann::ordered_json RegDeltaToSubComponentArray(nlohmann::ordered_json RegDeltaJSON, QString Hive);
    ~PackageEditor();

private slots:
    void on_AddSubGameButton_clicked();
    void on_AddComponentButton_clicked();
    void on_SaveButton_clicked();
    void JSONQTextEditChanged();
    void JSONQLineEditChanged();
    void SaveJSONButtonPressed();
    void ParentComponentChanged();
    void RemoveSubgame();
    void RemoveComponent();
    void RunExeInComponent();
    void BrowseInComponent();
    void RegeditInComponent();
    void ExecuteComponent();
    void AnalyzeComponent();
    void AddFileLayer();
    void FinalizeComponent();

private:
    bool InitMANIFESTJSON();
    void SavePackage();
    void RefreshJSONView();
    void InitPackage();
    void CompareComponentsRegistry(const int oldcomponent, const int newcomponent);
    void MergeRegistryDeltaInComponent(nlohmann::ordered_json * DeltaSubComponentArray, const int targetcomponent);
    bool BuildUI();
    bool SaveManifestJSON();

    //int CurrentTab = 0;
    //bool initdone = false;

    QWidget * JSONTabWidget;
    QWidget * ManifestTabWidget;

    QPushButton * SaveJSONButton;
    QTextEdit * JSONTextEdit;

    QDir * PackageDir;
    QDir * MetadataDir;
    QDir * PackageFilesDir;

    Ui::PackageEditor * ui;
    nlohmann::ordered_json * MANIFESTJSON;
    nlohmann::ordered_json * GlobalConfigJSON;
};

#endif // PACKAGEEDITOR_H
