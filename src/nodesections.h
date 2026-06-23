#ifndef NODESECTIONS_H
#define NODESECTIONS_H

#include "nodesection.h"

#include <QByteArray>
#include <QString>

class QLabel;
class QNetworkAccessManager;

// The per-node editor sections (one tab's worth, composed by NodeEditor). Each is a NodeSection (titled QGroupBox
// bound to model + node index) that renders + edits one concern of the node, editing only through the model.

class NodeIdentitySection : public NodeSection   // NODE_ID / ROLE / UID / GROUP / LABEL / RECOMMENDED
{ Q_OBJECT public: NodeIdentitySection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeSelectionSection : public NodeSection  // OPTIONAL / DEFAULT / EXCLUDE
{ Q_OBJECT public: NodeSelectionSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeParentsSection : public NodeSection    // PARENTS (catalog-wide id picker; load order)
{ Q_OBJECT public: NodeParentsSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodePlatformSection : public NodeSection   // PLATFORM HOST + (runner) GUEST list
{ Q_OBJECT public: NodePlatformSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeExecSection : public NodeSection       // EXEC: CONTENTPATH/EXEARGS/WORKDIR/EXECUTABLE/CONTENT_ROOT + ARGS/REMOVE_ENV/ENV + PREFIX_GENERATE
{ Q_OBJECT public: NodeExecSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeLayersSection : public NodeSection     // the per-TYPE layer sub-editors (VFS*/RegEdit/DllOverride/FileEdit/Persist*/CustomVar)
{ Q_OBJECT public: NodeLayersSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

// META (cover art + TITLE + metadata). Owns the cover drop-target (drag-and-drop of an image / local file / URL).
class NodeMetaSection : public NodeSection
{
    Q_OBJECT
public:
    NodeMetaSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr);

protected:
    bool eventFilter(QObject * obj, QEvent * event) override;

private:
    void ApplyCoverImage(QLabel * CoverLabel, const QByteArray & Data, const QString & Extension, int NodeIndexInArray);
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // NODESECTIONS_H
