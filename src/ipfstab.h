#ifndef IPFSTAB_H
#define IPFSTAB_H

#include <QWidget>
#include <QHash>
#include <QString>

class IpfsModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QTimer;

// ---------------------------------------------------------------------------
// IpfsTab — the "IPFS" tab, a PURE VIEW over IpfsModel, laid out qBittorrent-style:
//   • left sidebar: Status filters (All/Downloading/Seeding/Queued/Stalled/Errored/Not fetched) + Category filters
//     (Content/Assets/Meta), each with live counts — clicking narrows the tree,
//   • search box filtering by name/package/CID,
//   • the category → package → CID tree (live transfers AND seeded content); per-row actions live in a right-click
//     context menu (copy CID, prioritize, cancel, unpin, re-check health) instead of button columns,
//   • package rows carry the PACKAGE-LEVEL meta-CID (publish/copy via their context menu) — the shareable unit
//     between a file's content CID and a whole source's library CID.
// Default expansion: categories open (except Assets), packages collapsed; active filters auto-expand their matches.
// All transfer state, polling, health gathering and label building live in IpfsModel; this class only paints its
// signals and forwards user actions back to it.
// ---------------------------------------------------------------------------
class IpfsTab : public QWidget
{
    Q_OBJECT
public:
    explicit IpfsTab(IpfsModel & model, QWidget * parent = nullptr);

public slots:
    void setActive(bool on);   // tab shown/hidden → start/stop the model's periodic refresh

private:
    // Status-filter kinds (sidebar rows); Errored also captures seeded pins whose backing files are gone.
    enum StatusFilter : int { SAll = 0, SDownloading, SSeeding, SQueued, SStalled, SErrored, SPending, SCount };

    void buildUi();
    QWidget * buildSidebar();                            // the Status + Categories filter lists
    QTreeWidgetItem * ensureLeaf(const QString & cid);   // find/create a CID's leaf under its category→package group
    void renderLeaf(const QString & cid);                // paint one CID's columns from the model's CidState
    void removeLeaf(const QString & cid);                // drop a CID's leaf
    void reconcile();                                    // full rebuild from the model's CID set (on modelReset)
    void paintStatus();                                  // paint the status strip from the model's NodeStatus
    void updateGroupTotals();                            // per-group / per-category item + byte totals; prune empties
    void paintGroupCid(QTreeWidgetItem * grp, const QString & pkg);   // package-level CID in the group's CID column

    int  effectiveStatus(const QString & cid) const;     // CidState → StatusFilter (missing files ⇒ SErrored)
    bool leafMatches(const QString & cid) const;         // current status/category/search filters
    void applyFilters();                                 // hide non-matching leaves/groups/categories, auto-expand
    void updateFilterCounts();                           // live counts in the sidebar labels
    void showContextMenu(const QPoint & pos);            // right-click actions for leaf/package/category rows

    IpfsModel & Model;

    QTableWidget * IpfsStatusTable = nullptr;   // one-row status strip: Network|Peers|Seeded|↓|↑|Repo|Disk
    QLabel       * IpfsHintLabel   = nullptr;   // shown only when the node is off
    QTreeWidget  * IpfsPins        = nullptr;   // unified tree: Name|Size|Progress|Speed|Status|Health|CID
    QLineEdit    * SearchBox       = nullptr;
    QListWidget  * StatusFilters   = nullptr;
    QListWidget  * CategoryFilters = nullptr;
    QPushButton  * SeedBtn         = nullptr;   // held so seed-progress can update its label
    QTimer       * CountsDebounce  = nullptr;   // coalesces per-leaf changes into one sidebar-count refresh
    bool           IpfsFitColumnsOnShow = true; // fit every column to content on first render / whenever shown

    int     CurrentStatus   = SAll;             // selected status filter
    QString CurrentCategory;                    // selected category filter ("" = all)
    bool    WasFiltered     = false;            // restore default expansion when the last filter is cleared

    QHash<QString, QTreeWidgetItem*> IpfsPinCategories;   // category label → top-level row
    QHash<QString, QTreeWidgetItem*> IpfsPinSourceGroups; // "Content\x1f<source>" → source row (Content only; the extra tier that splits runners/games/libraries apart)
    QHash<QString, QTreeWidgetItem*> IpfsPinGroups;       // package group row: "<cat>\x1f<pkg>" (Assets/Meta) or "Content\x1f<source>\x1f<pkg>" (Content)
    QHash<QString, QTreeWidgetItem*> IpfsPinChildren;     // CID → leaf row
};

#endif // IPFSTAB_H
