#ifndef IPFSTAB_H
#define IPFSTAB_H

#include <QWidget>
#include <QHash>
#include <QString>

class IpfsModel;
class QLabel;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

// ---------------------------------------------------------------------------
// IpfsTab — the "IPFS" tab, now a PURE VIEW over IpfsModel. It renders the node status strip and the unified
// category → package → CID tree (live transfers AND seeded content), and wires the per-row Cancel/Prioritize/Copy/
// Unpin buttons to the model. All transfer state, polling, health gathering and label building live in IpfsModel;
// this class only paints its signals (cidChanged / cidRemoved / modelReset / nodeStatusChanged) and forwards user
// actions back to it.
// ---------------------------------------------------------------------------
class IpfsTab : public QWidget
{
    Q_OBJECT
public:
    explicit IpfsTab(IpfsModel & model, QWidget * parent = nullptr);

public slots:
    void setActive(bool on);   // tab shown/hidden → start/stop the model's periodic refresh

private:
    void buildUi();
    QTreeWidgetItem * ensureLeaf(const QString & cid);   // find/create a CID's leaf under its category→package group
    void renderLeaf(const QString & cid);                // paint one CID's columns from the model's CidState
    void removeLeaf(const QString & cid);                // drop a CID's leaf (+ its buttons)
    void reconcile();                                    // full rebuild from the model's CID set (on modelReset)
    void paintStatus();                                  // paint the status strip from the model's NodeStatus
    void updateGroupTotals();                            // per-group / per-category item + byte totals; prune empties

    IpfsModel & Model;

    QTableWidget * IpfsStatusTable = nullptr;   // one-row status strip: Network|Peers|Seeded|↓|↑|Repo|Disk
    QLabel       * IpfsHintLabel   = nullptr;   // shown only when the node is off
    QTreeWidget  * IpfsPins        = nullptr;   // unified tree: Name|Size|Progress|Speed|Status|Health|CID|actions
    QPushButton  * SeedBtn         = nullptr;   // held so seed-progress can update its label
    bool           IpfsFitColumnsOnShow = true; // fit every column to content on first render / whenever shown

    QHash<QString, QTreeWidgetItem*> IpfsPinCategories; // category label → top-level row
    QHash<QString, QTreeWidgetItem*> IpfsPinGroups;     // "<category>\x1f<package>" → group row
    QHash<QString, QTreeWidgetItem*> IpfsPinChildren;   // CID → leaf row
    QHash<QString, QPushButton*>     IpfsCancelBtns;    // CID → its leaf's "Cancel" button
    QHash<QString, QPushButton*>     IpfsPrioBtns;      // CID → its leaf's "Prioritize" button
};

#endif // IPFSTAB_H
