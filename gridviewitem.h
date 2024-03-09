#ifndef GRIDVIEWITEM_H
#define GRIDVIEWITEM_H

#include <QWidget>

namespace Ui {
class GridViewItem;
}

class GridViewItem : public QWidget
{
    Q_OBJECT

public:
    explicit GridViewItem(QWidget *parent = nullptr);
    ~GridViewItem();

private:
    Ui::GridViewItem *ui;
};

#endif // GRIDVIEWITEM_H
