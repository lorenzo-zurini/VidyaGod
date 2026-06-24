#ifndef DELTATREE_H
#define DELTATREE_H

#include <QTreeWidget>
#include <QStringList>
#include <QSet>

// ---------------------------------------------------------------------------
// DeltaTree — a checkable tree built from a flat list of separator-delimited paths (the authoring session's write-
// delta of files, or its changed registry keys — which can be huge). Every level is checkable: toggling a node
// checks/unchecks its whole subtree; a node reflects its children as checked / unchecked / partial (Qt auto-tristate).
// Each input path's terminal node is an "entry" (a real file / registry key); intermediate path components are not.
//   • checkedRoots()   — the MAXIMAL fully-checked nodes (capture a file selection "at that level": each root's
//                        parent is stripped by the caller, so ticking a folder captures it without its ancestors).
//   • checkedEntries() — every checked ENTRY node at any level (the registry case: absolute keys, no stripping).
// The separator is '/' by default; set it to '\\' for registry key paths.
// ---------------------------------------------------------------------------
class DeltaTree : public QTreeWidget
{
    Q_OBJECT
public:
    explicit DeltaTree(QWidget * parent = nullptr);

    void        setSeparator(QChar Sep) { Separator = Sep; }
    void        setPaths(const QStringList & Paths);      // rebuild the tree (collapsed) from the path list
    QStringList checkedRoots() const;                     // maximal fully-checked nodes (the capture "levels")
    QStringList checkedEntries() const;                   // all checked terminal (entry) nodes
    void        markCaptured(const QStringList & Paths);  // tint these paths (and their subtrees) green

public slots:
    void checkAll(bool On);

signals:
    void checkedChanged();                                // the user changed the checked set (live preview hook)

private slots:
    void onItemChanged(QTreeWidgetItem * It, int Col);

private:
    static void                  setSubtreeChecked(QTreeWidgetItem * It, Qt::CheckState St);
    QString                      fullPath(const QTreeWidgetItem * It) const;
    QList<QTreeWidgetItem *>     rootItems() const;       // maximal fully-checked items (the capture roots)
    void                         restyleSelection();      // bold the capture roots + dim their stripped ancestors

    QChar                        Separator = '/';
    bool                         Updating  = false;       // re-entrancy guard (check propagation + restyle)
    QList<QTreeWidgetItem *>     BoldRoots;               // currently-bolded root rows (to un-bold on change)
    QList<QTreeWidgetItem *>     Dimmed;                  // currently-dimmed ancestor rows (to restore on change)
    QSet<QTreeWidgetItem *>      Captured;                // captured (green) items — persist; win over dim
};

#endif // DELTATREE_H
