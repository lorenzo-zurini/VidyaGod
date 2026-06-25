// Tests for DeltaTree's check propagation — specifically that ticking a directory captures THAT directory, never its
// parent chain (the auto-tristate footgun: a lone deep dir would otherwise promote every single-child ancestor to
// fully-checked and root the capture at the empty parent).

#include <QtTest>

#include "deltatree.h"

namespace {
// Find the item at a '/'-separated path in the tree, or nullptr.
QTreeWidgetItem * itemAt(QTreeWidget * T, const QString & path)
{
    QTreeWidgetItem * cur = nullptr;
    for (const QString & seg : path.split('/', Qt::SkipEmptyParts))
    {
        QTreeWidgetItem * next = nullptr;
        const int n = cur ? cur->childCount() : T->topLevelItemCount();
        for (int i = 0; i < n; ++i)
        {
            QTreeWidgetItem * c = cur ? cur->child(i) : T->topLevelItem(i);
            if (c->text(0) == seg) { next = c; break; }
        }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}
}

class DeltaTreeTest : public QObject
{
    Q_OBJECT
private slots:
    // Ticking a deep dir in a single-child chain captures ONLY that dir; ancestors stay PartiallyChecked (not Checked),
    // so they're neither capture roots nor (later) greened.
    void tick_dir_does_not_promote_empty_parents()
    {
        DeltaTree T;
        T.setPaths({ "pfx/drive_c/Morrowind/Morrowind.exe",
                     "pfx/drive_c/Morrowind/Data Files/Morrowind.esm" });

        QTreeWidgetItem * mw = itemAt(&T, "pfx/drive_c/Morrowind");
        QVERIFY(mw != nullptr);
        mw->setCheckState(0, Qt::Checked);              // user ticks the Morrowind dir

        // Capture root is exactly Morrowind — not pfx / drive_c.
        QCOMPARE(T.checkedRoots(), QStringList{ "pfx/drive_c/Morrowind" });

        // The single-child ancestors are PartiallyChecked, never fully Checked.
        QCOMPARE(itemAt(&T, "pfx/drive_c")->checkState(0), Qt::PartiallyChecked);
        QCOMPARE(itemAt(&T, "pfx")->checkState(0),         Qt::PartiallyChecked);

        // The subtree under Morrowind is checked (so it captures fully).
        QCOMPARE(itemAt(&T, "pfx/drive_c/Morrowind/Morrowind.exe")->checkState(0), Qt::Checked);
    }

    // Ticking a parent directly DOES make it the capture root (and checks its subtree).
    void tick_parent_directly_roots_there()
    {
        DeltaTree T;
        T.setPaths({ "game/bin/run.exe", "game/data/a.pak" });
        itemAt(&T, "game")->setCheckState(0, Qt::Checked);
        QCOMPARE(T.checkedRoots(), QStringList{ "game" });
        QCOMPARE(itemAt(&T, "game/bin/run.exe")->checkState(0), Qt::Checked);
    }

    // Unchecking the lone checked dir clears the partial ancestors back to Unchecked.
    void untick_clears_ancestors()
    {
        DeltaTree T;
        T.setPaths({ "a/b/c/file.txt" });
        QTreeWidgetItem * c = itemAt(&T, "a/b/c");
        c->setCheckState(0, Qt::Checked);
        QVERIFY(!T.checkedRoots().isEmpty());
        c->setCheckState(0, Qt::Unchecked);
        QVERIFY(T.checkedRoots().isEmpty());
        QCOMPARE(itemAt(&T, "a")->checkState(0), Qt::Unchecked);
        QCOMPARE(itemAt(&T, "a/b")->checkState(0), Qt::Unchecked);
    }

    // Two separate deep selections yield two distinct roots, not a merged parent.
    void sibling_selections_stay_separate()
    {
        DeltaTree T;
        T.setPaths({ "root/x/one.txt", "root/y/two.txt" });
        itemAt(&T, "root/x")->setCheckState(0, Qt::Checked);
        itemAt(&T, "root/y")->setCheckState(0, Qt::Checked);
        QStringList roots = T.checkedRoots();
        roots.sort();
        QCOMPARE(roots, (QStringList{ "root/x", "root/y" }));
        QCOMPARE(itemAt(&T, "root")->checkState(0), Qt::PartiallyChecked);   // parent NOT promoted to Checked
    }
};

QTEST_MAIN(DeltaTreeTest)
#include "test_deltatree.moc"
