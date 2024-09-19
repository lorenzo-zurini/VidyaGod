#include "newsubgameframe.h"
#include "ui_newsubgameframe.h"

NewSubGameFrame::NewSubGameFrame(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::NewSubGameFrame)
{
    ui->setupUi(this);
}

NewSubGameFrame::~NewSubGameFrame()
{
    delete ui;
}
