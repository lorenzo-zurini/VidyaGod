#include <QSet>
#include <cmath>
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

QString DeltaTree::MainFolder(const QStringList & Paths, QChar Sep, double Share)
{
    if (Paths.isEmpty()) return {};
    //An installer marks the folder it installed to: its uninstaller (unins000.exe) or GOG's goggame-<id>.info sits
    //right in it. One such folder is the install, whatever its subfolders hold.
    QSet<QString> Marked;
    for (const QString & P : Paths)
    {
        const QString Leaf = P.section(Sep, -1).toLower();
        if (P.contains(Sep) && ((Leaf.startsWith("unins") && Leaf.endsWith(".exe")) || (Leaf.startsWith("goggame-") && Leaf.endsWith(".info"))))
            Marked.insert(P.section(Sep, 0, -2));
    }
    if (Marked.size() == 1) return *Marked.begin();
    //Several (GOG keeps a copy of its info file in ProgramData): the install is the marked one holding the most.
    if (Marked.size() > 1)
    {
        QString Best; int BestN = 0;
        for (const QString & M : Marked)
        {
            int N = 0;
            for (const QString & P : Paths) if (P.startsWith(M + Sep)) ++N;
            if (N > BestN) { Best = M; BestN = N; }
        }
        return Best;
    }
    QHash<QString, int> Under;                            // folder -> paths beneath it
    for (const QString & P : Paths)
    {
        const QStringList Segs = P.split(Sep, Qt::SkipEmptyParts);
        QString At;
        for (int I = 0; I + 1 < Segs.size(); ++I) { At += (I ? QString(Sep) : QString()) + Segs[I]; ++Under[At]; }
    }
    const int Need = (int)std::ceil(Share * Paths.size());
    QString Best;
    int BestDepth = -1, BestN = 0;
    for (auto It = Under.cbegin(); It != Under.cend(); ++It)
    {
        if (It.value() < Need) continue;
        const int Depth = (int)It.key().count(Sep);
        if (Depth > BestDepth || (Depth == BestDepth && It.value() > BestN)) { Best = It.key(); BestDepth = Depth; BestN = It.value(); }
    }
    //The bulk of a game is often its data folder (Oni's GameDataFolder, GTA's gtadata), its program beside it - in the
    //folder over it or a sibling (GTA's gtawin). The install is the nearest folder, from the bulk up, holding a
    //program anywhere beneath; none on the way, the bulk.
    auto HoldsProgram = [&](const QString & Dir) {
        for (const QString & P : Paths) if (P.startsWith(Dir + Sep) && P.endsWith(".exe", Qt::CaseInsensitive)) return true;
        return false;
    };
    for (QString At = Best; !At.isEmpty(); At = At.contains(Sep) ? At.section(Sep, 0, -2) : QString())
        if (HoldsProgram(At)) return At;
    return Best;
}

void DeltaTree::reveal(const QString & Path, bool Check)
{
    QTreeWidgetItem * At = nullptr;
    for (const QString & Seg : Path.split(Separator, Qt::SkipEmptyParts))
    {
        QTreeWidgetItem * Next = nullptr;
        const int N = At ? At->childCount() : topLevelItemCount();
        for (int I = 0; I < N && !Next; ++I)
        {
            QTreeWidgetItem * C = At ? At->child(I) : topLevelItem(I);
            if (C->text(0) == Seg) Next = C;
        }
        if (!Next) return;
        if (At) At->setExpanded(true);
        At = Next;
    }
    if (!At) return;
    scrollToItem(At, QAbstractItemView::PositionAtTop);
    setCurrentItem(At);
    if (Check) At->setCheckState(0, Qt::Checked);         // through onItemChanged: subtree + the preview signal
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
