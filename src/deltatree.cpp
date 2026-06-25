#include "deltatree.h"

#include <QBrush>
#include <QColor>
#include <QFont>
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
    BoldRoots.clear(); Dimmed.clear(); Captured.clear();   // clear() below deletes the items these point at
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
    // NB: directories are NOT given Qt::ItemIsAutoTristate. Auto-tristate would promote a parent to fully-checked
    // whenever all its children are checked — so ticking a lone deep dir (the common pfx/drive_c/<game> single-child
    // chain) would climb the capture root up to the empty parent. Parent state is managed manually in onItemChanged:
    // upward propagation only ever yields PartiallyChecked; a directory is fully Checked only when ticked directly.
    Updating = false;
}

void DeltaTree::checkAll(bool On)
{
    Updating = true;
    const Qt::CheckState St = On ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < topLevelItemCount(); ++i) { topLevelItem(i)->setCheckState(0, St); setSubtreeChecked(topLevelItem(i), St); }
    Updating = false;
    restyleSelection();
    emit checkedChanged();
}

void DeltaTree::onItemChanged(QTreeWidgetItem * It, int /*Col*/)
{
    if (Updating) return;                              // ignore our own propagation / restyle writes
    Updating = true;
    const Qt::CheckState St = It->checkState(0);
    if (It->childCount() > 0 && St != Qt::PartiallyChecked)
        setSubtreeChecked(It, St);                     // a real toggle on a directory → set its whole subtree

    // Propagate UPWARD as PartiallyChecked only: a parent reflects "something inside me is selected" but is NEVER
    // auto-promoted to fully Checked just because all its children are. So ticking a directory captures THAT directory
    // (and greens only it), never its parent chain — even when the parent has no other children. The user makes a
    // parent a capture root by ticking the parent itself. Bottom-up: each ancestor reads its just-updated children.
    for (QTreeWidgetItem * A = It->parent(); A; A = A->parent())
    {
        bool AnyOn = false;
        for (int i = 0; i < A->childCount(); ++i)
            if (A->child(i)->checkState(0) != Qt::Unchecked) { AnyOn = true; break; }
        A->setCheckState(0, AnyOn ? Qt::PartiallyChecked : Qt::Unchecked);
    }
    Updating = false;
    restyleSelection();                                  // re-bold the capture-root rows (the "levels")
    emit checkedChanged();
}

QList<QTreeWidgetItem *> DeltaTree::rootItems() const
{
    QList<QTreeWidgetItem *> Out;
    std::function<void(QTreeWidgetItem *)> Walk = [&](QTreeWidgetItem * It){
        const Qt::CheckState St = It->checkState(0);
        if (St == Qt::Checked) { Out << It; return; }                       // maximal — don't descend
        if (St == Qt::PartiallyChecked) for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) Walk(topLevelItem(i));
    return Out;
}

void DeltaTree::restyleSelection()
{
    static const QBrush Green(QColor(120, 200, 120));   // captured
    static const QBrush Dim(QColor(115, 122, 132));     // a stripped ancestor of a capture root
    Updating = true;                                    // setFont/setForeground fire itemChanged — suppress re-entry
    // Clear the previous bold roots + dimmed ancestors (restoring the base colour: green if captured, else default).
    for (QTreeWidgetItem * It : BoldRoots) { QFont F = It->font(0); F.setBold(false); It->setFont(0, F); }
    for (QTreeWidgetItem * It : Dimmed)    It->setForeground(0, Captured.contains(It) ? Green : QBrush());
    Dimmed.clear();
    // The capture roots: bold them (this is the exact level the capture roots at).
    BoldRoots = rootItems();
    for (QTreeWidgetItem * It : BoldRoots) { QFont F = It->font(0); F.setBold(true); It->setFont(0, F); }
    // Dim each root's ancestors — the parent folders that get stripped — so the "captured at this level" is visible.
    for (QTreeWidgetItem * Root : BoldRoots)
        for (QTreeWidgetItem * P = Root->parent(); P; P = P->parent())
        {
            if (Captured.contains(P)) continue;          // captured-green wins over dim
            P->setForeground(0, Dim);
            Dimmed << P;
        }
    Updating = false;
}

void DeltaTree::markCaptured(const QStringList & Paths)
{
    if (Paths.isEmpty()) return;
    static const QBrush Green(QColor(120, 200, 120));
    Updating = true;                                    // setForeground fires itemChanged — suppress re-entry
    std::function<void(QTreeWidgetItem *, bool)> Walk = [&](QTreeWidgetItem * It, bool Under){
        bool IsCap = Under;
        if (!IsCap)
        {
            const QString FP = fullPath(It);
            for (const QString & P : Paths) if (FP == P || FP.startsWith(P + Separator)) { IsCap = true; break; }
        }
        if (IsCap) { It->setForeground(0, Green); Captured.insert(It); }
        for (int i = 0; i < It->childCount(); ++i) Walk(It->child(i), IsCap);
    };
    for (int i = 0; i < topLevelItemCount(); ++i) Walk(topLevelItem(i), false);
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
    QStringList Out;
    for (QTreeWidgetItem * It : rootItems()) Out << fullPath(It);
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
