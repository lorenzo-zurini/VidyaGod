#include "penewtabwidget.h"
#include "ui_penewtabwidget.h"

PENewTabWidget::PENewTabWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PENewTabWidget)
{
    ui->setupUi(this);
}

PENewTabWidget::~PENewTabWidget()
{
    delete ui;
}
