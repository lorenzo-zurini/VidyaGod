#include "packageeditor.h"
#include "ui_packageeditor.h"

PackageEditor::PackageEditor(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);
}

PackageEditor::~PackageEditor()
{
    delete ui;
}
