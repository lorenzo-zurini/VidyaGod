#include "deltatree.h"

#include <QHash>

#include <functional>

DeltaTree::DeltaTree(QWidget * parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setUniformRowHeights(true);
    connect(this, &QTreeWidget::itemChanged, this, &DeltaTree::onItemChanged);
}

void DeltaTree::setPaths(const QStringList & Paths)
{
    Updating = true;                  // bulk build: don't run the propagation handler per inserted item
    clear();

    // parent item (nullptr = top level) → (segment text → child item), so building is O(total segments), not O(n²).
    QHash<QTreeWidgetItem *, QHash<QString, QTreeWidgetItem *>> Kids;
    for (const QString & P : Paths)
    {
        QTreeWidgetItem * Parent = nullptr;
        const QStringList Segs = P.split(Separator, Qt::SkipEmptyParts);
        for (const QString & Seg : Segs)
        {
            QHash<QString, QTreeWidgetItem *> & Level = Kids[Parent];
            QTreeWidgetItem * Node = Level.value(Seg, nullptr);
            if (!Node)
            {
                Node = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(this);
                Node->setText(0, Seg);
                Node->setFlags(Node->flags() | Qt::ItemIsUserCheckable);
                Node->setCheckState(0, Qt::Unchecked);
                Level.insert(Seg, Node);
            }
            Parent = Node;
        }
        if (Parent) Parent->setData(0, Qt::UserRole, true);   // the path's terminal node is a real entry (file / key)
    }
    // Directories (items with children) reflect their subtree via auto-tristate.
    std::function<void(QTreeWidgetItem *)> Mark = [&](QTreeWidgetItem * It){
        if (It->childCount() > 0) It->setFlags(It->flags() | Qt::ItemIsAutoTristate);
        for (int i = 0; i < It->childCount(); ++i) Mark(It->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) Mark(topLevelItem(i));
    Updating = false;
}

void DeltaTree::checkAll(bool On)
{
    Updating = true;
    const Qt::CheckState St = On ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < topLevelItemCount(); ++i) { topLevelItem(i)->setCheckState(0, St); setSubtreeChecked(topLevelItem(i), St); }
    Updating = false;
}

void DeltaTree::onItemChanged(QTreeWidgetItem * It, int /*Col*/)
{
    if (Updating || It->childCount() == 0) return;
    const Qt::CheckState St = It->checkState(0);
    if (St == Qt::PartiallyChecked) return;            // child-driven auto update — nothing to push down
    Updating = true;
    setSubtreeChecked(It, St);                         // a real toggle on a directory → set its whole subtree
    Updating = false;
}

void DeltaTree::setSubtreeChecked(QTreeWidgetItem * It, Qt::CheckState St)
{
    for (int i = 0; i < It->childCount(); ++i)
    {
        QTreeWidgetItem * C = It->child(i);
        C->setCheckState(0, St);
        setSubtreeChecked(C, St);
    }
}

QStringList DeltaTree::checkedRoots() const
{
    // Maximal fully-checked nodes: a Checked node short-circuits (its subtree is implied); recurse only into Partial.
    QStringList Out;
    std::function<void(QTreeWidgetItem *)> Walk = [&](QTreeWidgetItem * It){
        const Qt::CheckState St = It->checkState(0);
        if (St == Qt::Checked) { Out << fullPath(It); return; }
        if (St == Qt::PartiallyChecked) for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) Walk(topLevelItem(i));
    return Out;
}

QStringList DeltaTree::checkedEntries() const
{
    // Every checked ENTRY node at any level (a node that terminated an input path). Recurse through checked/partial.
    QStringList Out;
    std::function<void(QTreeWidgetItem *)> Walk = [&](QTreeWidgetItem * It){
        if (It->checkState(0) == Qt::Unchecked) return;
        if (It->checkState(0) == Qt::Checked && It->data(0, Qt::UserRole).toBool()) Out << fullPath(It);
        for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) Walk(topLevelItem(i));
    return Out;
}

QString DeltaTree::fullPath(const QTreeWidgetItem * It) const
{
    QStringList Segs;
    for (const QTreeWidgetItem * C = It; C; C = C->parent()) Segs.prepend(C->text(0));
    return Segs.join(Separator);
}
