#include "packageeditor.h"
#include "ui_packageeditor.h"

//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(QJsonDocument * GlobalConfigJSON, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;
    SetupPackageDataFrame();
}

PackageEditor::~PackageEditor()
{
    delete ui;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    QFrame * NewSubGameFrame = new QFrame(ui->SubGamesScrollArea);
    QFormLayout * NewSubGameFrameLayout = new QFormLayout(NewSubGameFrame);
    NewSubGameFrame->setLayout(NewSubGameFrameLayout);

    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].toObject().keys())
    {
        if (Key == "PARENTPACKAGE")
        {
            continue;
        }

        QLineEdit * NewParamLineEdit = new QLineEdit(NewSubGameFrame);
        NewParamLineEdit->setObjectName(Key);
        NewSubGameFrameLayout->addRow(Key, NewParamLineEdit);
    }

    NewSubGameFrameLayout->addRow(new QPushButton("Remove"), new QPushButton("Duplicate"));
    ui->MetaDataScrollAreaContents->layout()->addWidget(NewSubGameFrame);
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
    //ui->PackageEditorTabWidget->addTab(QString("Component " + QString::number(ui->PackageEditorTabWidget->count())), );
}

