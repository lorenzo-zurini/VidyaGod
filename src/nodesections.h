#ifndef NODESECTIONS_H
#define NODESECTIONS_H

#include "nodesection.h"

#include <QByteArray>
#include <QString>

class QLabel;
class QNetworkAccessManager;

// The per-node editor sections (one tab's worth, composed by NodeEditor). Each is a NodeSection (titled QGroupBox
// bound to model + node index) that renders + edits one concern of the node, editing only through the model.

class NodeIdentitySection : public NodeSection   // NODE_ID + a read-only identity badge derived from the Declare* layers
{ Q_OBJECT public: NodeIdentitySection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeSelectionSection : public NodeSection  // OPTIONAL / DEFAULT / EXCLUDE
{ Q_OBJECT public: NodeSelectionSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

class NodeParentsSection : public NodeSection    // PARENTS (catalog-wide id picker; load order)
{ Q_OBJECT public: NodeParentsSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr); };

// The per-TYPE layer sub-editors, incl. the Declare* identity layers. Owns the DeclareLibraryItem cover drop-target
// (drag-and-drop of an image / local file / URL → COVER inside that layer).
class NodeLayersSection : public NodeSection
{
    Q_OBJECT
public:
    NodeLayersSection(PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr);

protected:
    bool eventFilter(QObject * obj, QEvent * event) override;

private:
    void ApplyCoverImage(QLabel * CoverLabel, const QByteArray & Data, const QString & Extension, int NodeIdx, int LayerIdx);
    QNetworkAccessManager * NetMgr = nullptr;
};

#endif // NODESECTIONS_H
