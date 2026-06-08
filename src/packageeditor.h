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


class PackageEditor : public QDialog
{
    Q_OBJECT

public:
    //If PackagePath is non-empty, the file-picker dialog is skipped and that package is opened directly.
    explicit PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget *parent = nullptr, const QString &PackagePath = "");
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
    void AddCustomVar();
    void AddPersistDir();
    void AddPersistFile();
    void AddRegPersist();
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
    //Refreshes the persistent validation box (below the tabs) from ValErrors/ValWarnings.
    void UpdateValidationBox();
    //Returns the PLATFORM of the subgame with the given SUBGAMEID in the assembled manifest ("" if none).
    std::string SubgamePlatform(const std::string &SubgameID);

    //MODULAR MANIFESTS (fragment-aware editing)
    //Loads every *.json fragment in the package into the assembled MANIFESTJSON, tagging each
    //routable element (subgames, components, customvars, variants, runners) with a hidden
    //"__FILE__" provenance key. Tracks FragmentFiles and IdentityFile.
    void LoadFragmentsAndAssemble();
    //Decomposes the tagged MANIFESTJSON back into one document per fragment file (by "__FILE__"),
    //stripping the tags. Returns filename -> document.
    std::map<QString, nlohmann::ordered_json> DecomposeByFile();
    //Prompts the user to choose which fragment file a new element should live in (dropdown of
    //existing files + "New file…"). Returns the chosen filename, or empty on cancel.
    QString PromptTargetFile(const QString &Title);
    //Runs JSONOps::ValidateManifest on a tag-stripped copy of MANIFESTJSON into ValErrors/ValWarnings.
    void RevalidateManifest();
    //Builds a "File: [dropdown]" row bound to the "__FILE__" tag of the element at ElementPointer
    //(a json_pointer string into MANIFESTJSON, e.g. "/COMPONENTS/1" or "/SUBGAMES/0/VARIANTS/2").
    //Changing it re-routes that element to another fragment file.
    QWidget * MakeFileTagWidget(const std::string &ElementPointer);
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
    QComboBox * JSONFileCombo = nullptr; // selects which fragment file the raw JSON tab edits

    QDir * PackageDir;

    //Fragment files present in the package (filenames, no path). IdentityFile holds PACKAGE*/PERSIST.
    QStringList FragmentFiles;
    QString IdentityFile;
    std::vector<std::string> ValErrors, ValWarnings;

    QTabWidget * PackageEditorTabWidget = nullptr;
    //Persistent validation panel docked below the tabs (always visible), refreshed by UpdateValidationBox().
    QGroupBox * ValidationBox  = nullptr;
    QTextEdit * ValidationView = nullptr;
    nlohmann::ordered_json * MANIFESTJSON;
    nlohmann::ordered_json * GlobalConfigJSON;
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // PACKAGEEDITOR_H
