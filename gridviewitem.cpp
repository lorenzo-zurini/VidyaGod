#include "gridviewitem.h"
#include "ui_gridviewitem.h"

GridViewItem::GridViewItem(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::GameItem)
{
    ui->setupUi(this);
}

GridViewItem::~GridViewItem()
{
    delete ui;
}
