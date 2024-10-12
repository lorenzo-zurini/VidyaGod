#include "packageeditor.h"
#include "ui_packageeditor.h"

#include "penewtabwidget.h"

//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(QJsonDocument * GlobalConfigJSON, QDir PackageDir, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;
    PackageEditor::MANIFESTJSON = new QJsonDocument;
    PackageEditor::PackageDir = PackageDir;

    SetupPackageDataFrame();
}

PackageEditor::~PackageEditor()
{
    delete ui;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    QGroupBox * NewSubGameBox = new QGroupBox(ui->SubGamesScrollArea);
    QFormLayout * NewSubGameBoxLayout = new QFormLayout(NewSubGameBox);
    NewSubGameBox->setLayout(NewSubGameBoxLayout);

    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].toObject().keys())
    {
        if (Key == "PARENTPACKAGE")
        {
            continue;
        }

        QLineEdit * NewParamLineEdit = new QLineEdit(NewSubGameBox);
        NewParamLineEdit->setObjectName(Key);
        NewSubGameBoxLayout->addRow(Key, NewParamLineEdit);
    }

    NewSubGameBoxLayout->addRow(new QPushButton("Remove"), new QPushButton("Duplicate"));
    ui->MetaDataScrollAreaContents->layout()->addWidget(NewSubGameBox);
}

bool PackageEditor::SetupPackageDataFrame()
{
    QFormLayout * PackageDataFrameLayout = new QFormLayout(ui->PackageDataFrame);
    ui->PackageDataFrame->setLayout(PackageDataFrameLayout);

    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PATH"))
        {
            QLineEdit * NewParamLineEdit = new QLineEdit(ui->PackageDataFrame);
            NewParamLineEdit->setObjectName(Key);
            PackageDataFrameLayout->addRow(Key, NewParamLineEdit);
        }
    }

    return true;
}

void PackageEditor::on_AddComponentButton_clicked()
{
    //PENewTabWidget * NewTabWidget = new class PENewTabWidget;
    QWidget * NewTabWidget = new QWidget(this);
    //QVBoxLayout * NewTabWidgetVerticalLayout

    //ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component " + QString::number(ui->PackageEditorTabWidget->count())));
}

void PackageEditor::SavePackage()
{

}
