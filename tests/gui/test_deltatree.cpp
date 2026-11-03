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
    // A capture opens on what the install made: the deepest folder holding most of the changed files (the game's
    // folder, not drive_c nor one of its subfolders), revealed and - asked to - ticked. Teeth: the shallowest such
    // folder; a subfolder holding fewer; reveal without ticking; stay in the data folder beside the program.
    void the_tree_opens_on_the_installed_folder()
    {
        QStringList P;
        for (int I = 0; I < 30; ++I) P << QString("pfx/drive_c/GOG Games/Game/data%1.pak").arg(I);
        for (int I = 0; I < 10; ++I) P << QString("pfx/drive_c/GOG Games/Game/bin/f%1.dll").arg(I);
        P << "pfx/drive_c/users/steamuser/Temp/log.txt" << "pfx/system.reg";
        QCOMPARE(DeltaTree::MainFolder(P), QString("pfx/drive_c/GOG Games/Game"));
        QCOMPARE(DeltaTree::MainFolder({"a.txt", "b/c.txt"}), QString());
        //Most files under one subfolder (BioShock's Content): the install is where its uninstaller is.
        QStringList B;
        for (int I = 0; I < 50; ++I) B << QString("pfx/drive_c/GOG Games/BioShock/Content/f%1.bsm").arg(I);
        B << "pfx/drive_c/GOG Games/BioShock/Builds/Release/Bioshock.exe" << "pfx/drive_c/GOG Games/BioShock/unins000.exe";
        QCOMPARE(DeltaTree::MainFolder(B), QString("pfx/drive_c/GOG Games/BioShock"));
        //GOG's spare copy of its info file elsewhere does not make that folder the install.
        B << "pfx/drive_c/AAA GOG copy/goggame-1.info" << "pfx/drive_c/GOG Games/BioShock/goggame-1.info";
        QCOMPARE(DeltaTree::MainFolder(B), QString("pfx/drive_c/GOG Games/BioShock"));
        //No uninstaller (Installer VISE leaves uninstal.log), the bulk in the data folder: the install is the folder
        //over it, where the program sits; with no program anywhere on the way, the bulk stays.
        QStringList O;
        for (int I = 0; I < 50; ++I) O << QString("pfx/drive_c/Program Files (x86)/Oni/GameDataFolder/level%1.dat").arg(I);
        QCOMPARE(DeltaTree::MainFolder(O), QString("pfx/drive_c/Program Files (x86)/Oni/GameDataFolder"));
        O << "pfx/drive_c/Program Files (x86)/Oni/Oni.exe" << "pfx/drive_c/Program Files (x86)/Oni/uninstal.log";
        QCOMPARE(DeltaTree::MainFolder(O), QString("pfx/drive_c/Program Files (x86)/Oni"));
        //The program in a sibling folder (GTA: gtadata beside gtawin): the folder over both.
        QStringList G;
        for (int I = 0; I < 50; ++I) G << QString("pfx/drive_c/GTA/gtadata/f%1.dat").arg(I);
        G << "pfx/drive_c/GTA/gtawin/Grand Theft Auto.exe" << "pfx/drive_c/GTA/gtawin/MSS32.DLL" << "pfx/drive_c/GTA/README.TXT";
        QCOMPARE(DeltaTree::MainFolder(G), QString("pfx/drive_c/GTA"));
        //A program inside the bulk keeps the bulk: a program elsewhere over it is no reason to climb.
        QStringList K;
        for (int I = 0; I < 50; ++I) K << QString("pfx/drive_c/Games/X/f%1.dat").arg(I);
        K << "pfx/drive_c/Games/X/x.exe" << "pfx/drive_c/Games/Other/tool.exe";
        QCOMPARE(DeltaTree::MainFolder(K), QString("pfx/drive_c/Games/X"));
        DeltaTree T;
        T.setPaths(P);
        T.reveal("pfx/drive_c/GOG Games/Game", true);
        QCOMPARE(T.checkedRoots(), QStringList{"pfx/drive_c/GOG Games/Game"});
        QVERIFY(T.currentItem() && T.currentItem()->text(0) == "Game");
    }

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
