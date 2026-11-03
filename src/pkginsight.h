#ifndef PKGINSIGHT_H
#define PKGINSIGHT_H

#include <QObject>
#include <QString>

class PackageEditorModel;
class QLabel;
class QLineEdit;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QWidget;

// ---------------------------------------------------------------------------
// PkgInsight — what the selected node amounts to, beside the canvas that says how it is built: the files the game
// sees when it runs (each with the layer it comes from and what it covers), its fold in the order it is applied
// (later wins), and its card on the shelf. Read-only; recomputed a moment after the node or the package changes.
// ---------------------------------------------------------------------------
class PkgInsight : public QObject
{
    Q_OBJECT
public:
    //Its pages go into Tabs, beside what is there (the node's JSON).
    PkgInsight(PackageEditorModel *model, QTabWidget *tabs);

public slots:
    void showNode(const QString &handle);      // "" = nothing selected
    void refresh();                            // the package changed: recompute (soon)

private:
    void recompute();
    PackageEditorModel *Model;
    QString Handle;
    QTimer *Soon = nullptr;
    QWidget *SeesPage = nullptr, *FoldPage = nullptr, *CardPage = nullptr;
    QLineEdit *SeesFilter = nullptr;
    QTreeWidget *Sees = nullptr, *Fold = nullptr;
    QLabel *SeesNotes = nullptr, *Cover = nullptr, *CardTitle = nullptr, *CardVersions = nullptr;
};

#endif // PKGINSIGHT_H
