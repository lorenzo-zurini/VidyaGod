#ifndef DELTATREE_H
#define DELTATREE_H

#include <QTreeWidget>
#include <QStringList>

// ---------------------------------------------------------------------------
// DeltaTree — a checkable file tree built from a flat list of '/'-separated relative paths (the authoring session's
// write-delta, which can be huge). Every level is checkable: toggling a directory checks/unchecks its whole subtree;
// a directory reflects its children as checked / unchecked / partial (Qt auto-tristate). checkedFiles() returns the
// full paths of the checked LEAVES — the selection to capture.
// ---------------------------------------------------------------------------
class DeltaTree : public QTreeWidget
{
    Q_OBJECT
public:
    explicit DeltaTree(QWidget * parent = nullptr);

    void        setPaths(const QStringList & RelPaths);   // rebuild the tree (collapsed) from the path list
    QStringList checkedFiles() const;                     // full rel paths of checked leaf files

public slots:
    void checkAll(bool On);

private slots:
    void onItemChanged(QTreeWidgetItem * It, int Col);

private:
    static void    setSubtreeChecked(QTreeWidgetItem * It, Qt::CheckState St);
    static QString fullPath(const QTreeWidgetItem * It);
    bool Updating = false;   // re-entrancy guard while propagating a parent's state to its children
};

#endif // DELTATREE_H
