#include "packageeditormdichild.h"
#include "ui_packageeditormdichild.h"

PackageEditorMDIChild::PackageEditorMDIChild(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PackageEditorMDIChild)
{
    ui->setupUi(this);
}

PackageEditorMDIChild::~PackageEditorMDIChild()
{
    delete ui;
}
