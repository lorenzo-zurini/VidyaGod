#include "variantpicker.h"

#include <QCollator>
#include <QLineEdit>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace {
constexpr int kSearchThreshold = 9;    // show the filter box only when browsing alone stops being practical
constexpr int kMaxVisibleRows  = 10;   // cap the widget's height at ~this many rows; more scrolls
} // namespace

VariantPicker::VariantPicker(Mode M, QWidget * parent)
    : QWidget(parent), PickMode(M)
{
    auto * L = new QVBoxLayout(this);
    L->setContentsMargins(0, 0, 0, 0);
    L->setSpacing(4);

    Search = new QLineEdit(this);
    Search->setPlaceholderText("Filter…");
    Search->setClearButtonEnabled(true);
    Search->setVisible(false);
    L->addWidget(Search);

    Model = new QStandardItemModel(this);
    Proxy = new QSortFilterProxyModel(this);
    Proxy->setSourceModel(Model);
    Proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    View = new QListView(this);
    View->setModel(Proxy);
    View->setUniformItemSizes(true);                       // enables true row virtualization in QListView
    View->setEditTriggers(QAbstractItemView::NoEditTriggers);
    View->setSelectionMode(PickMode == Mode::Single ? QAbstractItemView::SingleSelection
                                                    : QAbstractItemView::NoSelection);
    L->addWidget(View);

    connect(Search, &QLineEdit::textChanged, this, [this](const QString & T){
        Proxy->setFilterFixedString(T);                    // "contains", case-insensitive
    });
    if (PickMode == Mode::Checkable)
        connect(Model, &QStandardItemModel::itemChanged, this, [this](QStandardItem *){ emit checkedChanged(); });
    else
        connect(View->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex &, const QModelIndex &){ emit currentChanged(); });
}

void VariantPicker::setEntries(const std::vector<Entry> & Entries, const std::vector<std::string> & Checked)
{
    // Natural version order (1.9 < 1.10 < 1.10.2), recommended pinned first. Stable so equal keys keep author order.
    QCollator Coll;
    Coll.setNumericMode(true);
    Coll.setCaseSensitivity(Qt::CaseInsensitive);
    std::vector<Entry> Sorted = Entries;
    std::stable_sort(Sorted.begin(), Sorted.end(), [&Coll](const Entry & A, const Entry & B){
        if (A.Recommended != B.Recommended) return A.Recommended;
        return Coll.compare(A.Label, B.Label) < 0;
    });

    // NOTE: no blockSignals here — the proxy learns rows exist only from the model's insert signals (blocking them
    // left the proxy permanently empty). Configuring each item BEFORE appendRow means no itemChanged fires anyway.
    const std::set<std::string> Want(Checked.begin(), Checked.end());
    Model->clear();
    for (const Entry & E : Sorted)
    {
        auto * It = new QStandardItem((E.Recommended ? QStringLiteral("⭐ ") : QString()) + E.Label);
        It->setData(QString::fromStdString(E.Id), Qt::UserRole);
        It->setEditable(false);
        if (PickMode == Mode::Checkable)
        {
            It->setCheckable(true);
            It->setCheckState(Want.count(E.Id) ? Qt::Checked : Qt::Unchecked);
        }
        Model->appendRow(It);
    }

    Search->setVisible((int)Sorted.size() >= kSearchThreshold);

    // Height: exactly fit small lists; cap large ones (the view scrolls). sizeHintForRow needs a populated model.
    if (Model->rowCount() > 0)
    {
        int RowH = View->sizeHintForRow(0);
        if (RowH <= 0) RowH = 22;
        const int Rows = std::min(Model->rowCount(), kMaxVisibleRows);
        View->setFixedHeight(Rows * RowH + 2 * View->frameWidth() + 4);
    }

    if (PickMode == Mode::Single && Proxy->rowCount() > 0)
        View->setCurrentIndex(Proxy->index(0, 0));
}

std::vector<std::string> VariantPicker::checkedIds() const
{
    std::vector<std::string> Out;
    for (int i = 0; i < Model->rowCount(); ++i)
    {
        QStandardItem * It = Model->item(i);
        if (It && It->checkState() == Qt::Checked)
            Out.push_back(It->data(Qt::UserRole).toString().toStdString());
    }
    return Out;
}

std::string VariantPicker::currentId() const
{
    const QModelIndex Cur = View->currentIndex();
    if (!Cur.isValid()) return {};
    return Proxy->data(Cur, Qt::UserRole).toString().toStdString();
}

void VariantPicker::setCurrentId(const std::string & Id)
{
    const QString Want = QString::fromStdString(Id);
    for (int i = 0; i < Proxy->rowCount(); ++i)
    {
        const QModelIndex Ix = Proxy->index(i, 0);
        if (Proxy->data(Ix, Qt::UserRole).toString() == Want) { View->setCurrentIndex(Ix); return; }
    }
}
