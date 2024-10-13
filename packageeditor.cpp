#include "packageeditor.h"
#include "ui_packageeditor.h"

//#include "penewtabwidget.h"
#include "filesystemoperations.h"

//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(QJsonDocument * GlobalConfigJSON, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);


    InitPackage();

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;

    PackageEditor::PackageJSONModel = new QJsonModel;
    PackageEditor::PackageJSONModel->GetRootItem()->setType(QJsonValue::Object);

    ui->PackageJSONTreeView->setModel(PackageJSONModel);

    SetupPackageDataFrame();
}

PackageEditor::~PackageEditor()
{
    delete ui;
}

bool PackageEditor::SetupPackageDataFrame()
{

    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PATH"))
        {
            QJsonTreeItem * NewParamItem = new QJsonTreeItem();
            NewParamItem->setType(QJsonValue::String);
            NewParamItem->setKey(Key);

            PackageEditor::PackageJSONModel->GetRootItem()->appendChild(NewParamItem);
        }
    }

    QJsonTreeItem * NewSubgamesItem = new QJsonTreeItem();
    NewSubgamesItem->setType(QJsonValue::Array);
    NewSubgamesItem->setKey("SUBGAMES");

    PackageEditor::PackageJSONModel->GetRootItem()->appendChild(NewSubgamesItem);
    RefreshJSONView();
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    QJsonTreeItem * NewSubGameItem = new QJsonTreeItem();
    NewSubGameItem->setType(QJsonValue::Object);

    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PARENTPACKAGE"))
        {

            QJsonTreeItem * NewParamItem = new QJsonTreeItem();
            NewParamItem->setType(QJsonValue::String);
            NewParamItem->setKey(Key);

            NewSubGameItem->appendChild(NewParamItem);
        }
    }

    PackageEditor::PackageJSONModel->GetRootItem()->ChildByKey("SUBGAMES")->appendChild(NewSubGameItem);
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    //PENewTabWidget * NewTabWidget = new class PENewTabWidget;
    QWidget * NewTabWidget = new QWidget(this);
    //QVBoxLayout * NewTabWidgetVerticalLayout

    ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component " + QString::number(ui->PackageEditorTabWidget->count())));
}

void PackageEditor::on_SaveButton_clicked()
{
    qDebug().noquote() << "[OUT] SAVING JSONMODEL:" << PackageEditor::PackageJSONModel->json();
    QFile ManifestFile(MetadataDir->filePath("MANIFEST.json"));
    QTextStream OutFile(&ManifestFile);
    if (ManifestFile.open(QFile::WriteOnly | QFile::Truncate))
    {
        OutFile << PackageJSONModel->json();
        ManifestFile.close();
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open MANIFEST.json file for writing.";
    }
}

void PackageEditor::RefreshJSONView()
{
    ui->PackageJSONTreeView->setModel(PackageEditor::PackageJSONModel);
    ui->PackageJSONTreeView->expandAll();
    ui->PackageJSONTreeView->resizeColumnToContents(0);
    ui->PackageJSONTreeView->resizeColumnToContents(1);
}

void PackageEditor::InitPackage()
{
    PackageEditor::PackageDir = new QDir(QFileDialog::getExistingDirectory(this, "Select package directory..."));
    PackageEditor::MetadataDir = new QDir(FSOps::SubPath((*PackageDir).path(), "METADATA"));
    PackageEditor::PackageFilesDir = new QDir(FSOps::SubPath((*PackageDir).path(), "PACKAGEFILES"));

    if (!FSOps::CheckPackageValid(PackageDir))
    {
        MetadataDir->mkdir(MetadataDir->path());
        PackageFilesDir->mkdir(PackageFilesDir->path());
    }
    else
    {
        QFile ManifestJSONFile(MetadataDir->filePath("MANIFEST.json"));
        if (ManifestJSONFile.open(QFile::ReadOnly))
        {
            qDebug() << "TEST1";
            PackageEditor::PackageJSONModel->load(&ManifestJSONFile);
            qDebug() << "TEST2";
        }
        else
        {
            qDebug() << QTime::currentTime() << " [ERR] Could not open MANIFEST.json file for reading.";
        };
    }
}
