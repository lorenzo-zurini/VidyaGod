#ifndef NODEEDITOR_H
#define NODEEDITOR_H

#include <QWidget>

#include <nlohmann/json.hpp>

class PackageEditorModel;
class QLabel;
class QNetworkAccessManager;

// ---------------------------------------------------------------------------
// NodeEditor — the editor for ONE node (one tab in PackageEditor). Built from (model, nodeArrayIndex), it renders
// the node's sections (authoring toolbar + Identity / Selection / PARENTS / Platform / EXEC / META(cover-drop) /
// LAYERS) and edits the working document through PackageEditorModel: value edits persist via SaveNodes(),
// structural edits call requestReload() so the shell rebuilds the tabs. It talks only to the model — never to the
// shell or sibling tabs.
// ---------------------------------------------------------------------------
class NodeEditor : public QWidget
{
    Q_OBJECT
public:
    NodeEditor(PackageEditorModel * model, int nodeArrayIndex, QWidget * parent = nullptr);

protected:
    bool eventFilter(QObject * obj, QEvent * event) override;   // META cover drop (image / file / URL)

private slots:
    void onFieldEdited();   // a bound QLineEdit finished editing → write its JSONPath into the doc + save
    // LAYERS add-actions (append a TYPE-tagged layer to this node's LAYERS).
    void AddVFSDirLayer();
    void AddVFSZipLayer();
    void AddVFSFileLayer();
    void AddRegEdit();
    void AddDllOverride();
    void AddFileEdit();
    void AddCustomVar();
    void AddPersistDir();
    void AddPersistFile();
    void AddRegPersist();
    void AddRegKeyPersist();

private:
    void    appendLayer(const nlohmann::ordered_json & layer);                 // push onto this node's LAYERS + reload
    QString ImportLayerFile(const QString & Selected, bool IsDir);             // bring a file/dir into the bundle (copy/move)
    void    ApplyCoverImage(QLabel * CoverLabel, const QByteArray & Data, const QString & Extension, int NodeIndexInArray);

    PackageEditorModel *    Model = nullptr;
    int                     N = 0;          // this node's index in doc()["NODES"]
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // NODEEDITOR_H
