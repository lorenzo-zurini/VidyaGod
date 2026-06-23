#include "nodesection.h"
#include "packageeditormodel.h"

#include <QLineEdit>

NodeSection::NodeSection(const QString & title, PackageEditorModel * model, int nodeIndex, QWidget * parent)
    : QGroupBox(title, parent), Model(model), N(nodeIndex)
{
}

void NodeSection::onFieldEdited()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(sender());
    if (!Editor) return;
    nlohmann::ordered_json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    Model->doc()[JSONPointer] = Editor->text().toStdString();
    Model->SaveNodes();
}
