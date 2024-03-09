#include "gridviewitem.h"
#include "ui_gridviewitem.h"

GridViewItem::GridViewItem(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::GridViewItem)
{
    ui->setupUi(this);
}

GridViewItem::~GridViewItem()
{
    delete ui;
}
