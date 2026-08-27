#ifndef VARIANTPICKER_H
#define VARIANTPICKER_H

#include <QWidget>
#include <QString>

#include <string>
#include <vector>

class QLineEdit;
class QListView;
class QStandardItemModel;
class QSortFilterProxyModel;

// Natural version-aware label order: digit runs compare numerically ("1.9" < "1.10" < "1.10.2"), the rest
// case-insensitively. Hand-rolled — QCollator's numeric mode is backend/locale-dependent (differed across the two
// dev machines and is unsupported for the C locale), and variant lists must sort identically everywhere.
bool NaturalLess(const QString & A, const QString & B);

// ---------------------------------------------------------------------------
// VariantPicker — a variant list that scales to an ARBITRARY number of entries (a game with 3 editions or 903
// Minecraft versions alike). Model/view (virtualized — no widget per row), with:
//   • type-to-filter search box (auto-shown once the list is big enough to need it),
//   • natural version ordering (QCollator numeric: 1.9 < 1.10 < 1.10.2), recommended entries pinned first,
//   • Checkable mode (download dialog: tick what to fetch) or Single mode (pick one),
//   • height capped to ~10 rows — beyond that it scrolls.
// Rows are (id, label, recommended); NO per-row derived data (file counts, sizes) — anything whose cost scales
// with the closure is the CALLER's job, computed asynchronously for the current selection only.
// ---------------------------------------------------------------------------
class VariantPicker : public QWidget
{
    Q_OBJECT
public:
    enum class Mode { Single, Checkable };

    struct Entry {
        std::string Id;
        QString     Label;
        bool        Recommended = false;
    };

    explicit VariantPicker(Mode M, QWidget * parent = nullptr);

    // (Re)populate. Sorts: recommended first, then natural label order. In Checkable mode `checked` seeds the
    // ticked set; in Single mode the first entry (= recommended, if any) becomes current.
    void setEntries(const std::vector<Entry> & Entries, const std::vector<std::string> & Checked = {});

    std::vector<std::string> checkedIds() const;   // Checkable mode: the ticked ids (model order)
    std::string              currentId()  const;   // Single mode: the selected id ("" if none)
    void setCurrentId(const std::string & Id);

signals:
    void checkedChanged();   // any tick toggled (Checkable mode)
    void currentChanged();   // selection moved (Single mode)

private:
    Mode                    PickMode;
    QLineEdit *             Search  = nullptr;
    QListView *             View    = nullptr;
    QStandardItemModel *    Model   = nullptr;
    QSortFilterProxyModel * Proxy   = nullptr;
};

#endif // VARIANTPICKER_H
