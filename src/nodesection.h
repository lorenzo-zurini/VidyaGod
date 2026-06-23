#ifndef NODESECTION_H
#define NODESECTION_H

#include <QGroupBox>

#include <nlohmann/json.hpp>

class PackageEditorModel;

// ---------------------------------------------------------------------------
// NodeSection — base for a PackageEditor per-node section: a titled QGroupBox bound to ONE node (the model + the
// node's array index). It provides the shared JSONPath field-edit slot that bound QLineEdits connect to. Every
// section edits the working document only through the model (value edits → SaveNodes(); structural edits →
// requestReload()), so the sections never touch each other or the shell.
// ---------------------------------------------------------------------------
class NodeSection : public QGroupBox
{
    Q_OBJECT
public:
    NodeSection(const QString & title, PackageEditorModel * model, int nodeIndex, QWidget * parent = nullptr);

public slots:
    //A bound QLineEdit finished editing → write its "JSONPath" property's value into the working doc + save.
    //Public so the derived sections can wire their QLineEdits to it via &NodeSection::onFieldEdited.
    void onFieldEdited();

protected:
    PackageEditorModel * Model = nullptr;
    int                  N = 0;   // this node's index in Model->doc()["NODES"]
};

#endif // NODESECTION_H
