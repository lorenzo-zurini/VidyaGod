#include "packageeditor.h"
#include "ui_packageeditor.h"

//#include "penewtabwidget.h"
#include "filesystemoperations.h"
#include "jsonoperations.h"

//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(QJsonDocument * GlobalConfigJSON, QWidget * parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;

    InitPackage();
    InitMANIFESTJSON();
    RefreshJSONView();
}

PackageEditor::~PackageEditor()
{
    delete ui;
}

bool PackageEditor::InitMANIFESTJSON()
{
    QJsonObject RootJSONObject;
    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PATH"))
        {
            RootJSONObject[Key] = ".....";
        }
    }

    PackageEditor::MANIFESTJSON = new QJsonDocument(RootJSONObject);
    RefreshJSONView();
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    QJsonObject NewSubGameObject;
    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PARENTPACKAGE"))
        {
            NewSubGameObject[Key] = ".....";
        }
    }
    (*MANIFESTJSON)["SUBGAMES"].toArray().append(NewSubGameObject);
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
    qDebug().noquote() << "[OUT] SAVING JSON:" << MANIFESTJSON->toJson();
    QFile ManifestFile(MetadataDir->filePath("MANIFEST.json"));
    QTextStream OutFile(&ManifestFile);
    if (ManifestFile.open(QFile::WriteOnly | QFile::Truncate))
    {
        OutFile << MANIFESTJSON->toJson();
        ManifestFile.close();
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open MANIFEST.json file for writing.";
    }
}

void PackageEditor::RefreshJSONView()
{
    ui->JSONTextEdit->setText(PackageEditor::MANIFESTJSON->toJson());
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
        QFile MANIFESTJSONFile(MetadataDir->filePath("MANIFEST.json"));
        PackageEditor::MANIFESTJSON = JSONOps::LoadJSON(&MANIFESTJSONFile);
    }
}

void PackageEditor::on_JSONTextEdit_textChanged()
{
    if (QJsonDocument::fromJson(ui->JSONTextEdit->toPlainText().toUtf8()).isNull())
    {
        qDebug() << "INVALID JSON";
    }
    else
    {
        qDebug() << "VALID JSON";
    }
}

