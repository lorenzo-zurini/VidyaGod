#ifndef PACKAGEEDITOR_H
#define PACKAGEEDITOR_H

#include <QWidget>
#include <iostream>
#include <QDialog>
#include <QScreen>
#include <QToolButton>
#include <QMenu>

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
#include <QCheckBox>

#include <QTextEdit>
#include <QAbstractItemModel>
#include <QItemDelegate>

#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QPixmap>
#include <QFileInfo>

#include "filesystemoperations.h"
#include "jsonoperations.h"
#include "containerwrapper.h"
#include "nlohmann/json.hpp"

namespace Ui {
class PackageEditor;
}

class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    //If PackagePath is non-empty, the file-picker dialog is skipped and that package is opened directly.
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr, const QString &PackagePath = "");
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
    void PlatformChanged();
    void RemoveSubgame();
    void RemoveComponent();
    void RunExeInComponent();
    void BrowseInComponent();
    void RegeditInComponent();
    void ExecuteComponent();
    void AnalyzeComponent();
    void AddVFSDirLayer();
    void AddVFSZipLayer();
    void AddVFSFileLayer();
    void AddEntrypoint();
    void AddRegEdit();
    void AddDllOverride();
    void AddFileEdit();
    void FinalizeComponent();
    void MoveComponentUp();
    void MoveComponentDown();

private:
    bool InitMANIFESTJSON();
    void SavePackage();
    void RefreshJSONView();
    void InitPackage(const QString &PreselectedPath = "");
    void CompareComponentsRegistry(const std::string &oldcomponent_id, const std::string &newcomponent_id);
    void MergeRegistryDeltaInComponent(nlohmann::ordered_json * DeltaSubComponentArray, const std::string &targetcomponent_id);
    bool BuildUI();
    bool SaveManifestJSON();
    std::string GetRunnerType(const std::string &platform);
    bool eventFilter(QObject *obj, QEvent *event) override;
    void ApplyCoverImage(QLabel *CoverLabel, const QByteArray &Data, const QString &Extension, int SubgameIndex);

    //int CurrentTab = 0;
    //bool initdone = false;

    QWidget * JSONTabWidget;
    QWidget * ManifestTabWidget;
    QTabWidget * SubGamesTabWidget = nullptr; // kept as member for position save/restore

    //Saved UI state — restored after BuildUI() to keep the user on the same tab/subgame.
    int SavedMainTab = 1;
    int SavedSubgameTab = 0;

    QPushButton * SaveJSONButton;
    QTextEdit * JSONTextEdit;

    QDir * PackageDir;
    QDir * MetadataDir;
    QDir * PackageFilesDir;

    Ui::PackageEditor * ui;
    nlohmann::ordered_json * MANIFESTJSON;
    nlohmann::ordered_json * GlobalConfigJSON;
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // PACKAGEEDITOR_H
