#ifndef PRELAUNCHWINDOW_P_H
#define PRELAUNCHWINDOW_P_H

// What PreLaunchWindow's files share (prelaunchwindow.cpp and prelaunch_<concern>.cpp: runners, grafts, options,
// lan, instances). Not for anyone else.

#include <map>
#include <string>
#include <vector>

class QObject;
class QTreeWidget;
class QTreeWidgetItem;

namespace PreLaunchUi
{
//The graft rows of the graft tree, depth-first (section rows carry no key) — the order grafts apply in.
std::vector<QTreeWidgetItem*> GraftRows(QTreeWidget* T);
//The row of a SECTION path ("UserPatch/Interface": '/' nests) — created on first use, collapsed; "" is the root.
QTreeWidgetItem* SectionRow(QTreeWidget* T, std::map<std::string, QTreeWidgetItem*>& Made, const std::string& Path);
//The options' values as the form holds them now (key → value), from Group's controls.
std::map<std::string, std::string> CollectVarValues(QObject* Group);
}
using namespace PreLaunchUi;

#endif // PRELAUNCHWINDOW_P_H
