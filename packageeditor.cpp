#include "packageeditor.h"
#include "ui_packageeditor.h"

using json = nlohmann::ordered_json;
//MUST MAKE THIS CLASS MDI BASED!!!

PackageEditor::PackageEditor(nlohmann::ordered_json * GlobalConfigJSON, QWidget * parent)
    : QDialog(parent)
    , ui(new Ui::PackageEditor)
{
    ui->setupUi(this);
    this->setGeometry(0, 0, QGuiApplication::primaryScreen()->geometry().width(), QGuiApplication::primaryScreen()->geometry().height());
    this->setWindowState(Qt::WindowMaximized);

    PackageEditor::GlobalConfigJSON = GlobalConfigJSON;
    PackageEditor::MANIFESTJSON = new json;

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
    if (PackageEditor::MANIFESTJSON->empty())
    {
        json RootJSONObject;
        for (auto Item : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].items())
        {
            if (!(Item.key() == "PATH"))
            {
                RootJSONObject[Item.key()] = nullptr;
            }
        }

        (*PackageEditor::MANIFESTJSON) = RootJSONObject;
        RefreshJSONView();
    }
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    json NewSubGameObject;
    for (auto Item : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].items())
    {
        if (!(Item.key() == "PARENTPACKAGE"))
        {
            NewSubGameObject[Item.key()] = nullptr;
        }
    }

    (*MANIFESTJSON)[json::json_pointer("/SUBGAMES")].push_back(NewSubGameObject);
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    (*MANIFESTJSON)[json::json_pointer("/COMPONENTS")].push_back(json::object({{"NAME", nullptr}}));
    RefreshJSONView();

    QWidget * NewTabWidget = new QWidget(this);
    ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component " + QString::number(ui->PackageEditorTabWidget->count())));
}

void PackageEditor::on_SaveButton_clicked()
{
    qDebug().noquote() << "[OUT] SAVING JSON:" << QString::fromStdString(MANIFESTJSON->dump(4));
    QFile ManifestFile(MetadataDir->filePath("MANIFEST.json"));
    QTextStream OutFile(&ManifestFile);
    if (ManifestFile.open(QFile::WriteOnly | QFile::Truncate))
    {
        OutFile << QString::fromStdString(MANIFESTJSON->dump(4));
        ManifestFile.close();
    }
    else
    {
        qDebug() << QTime::currentTime() << " [ERR] Could not open MANIFEST.json file for writing.";
    }
}

void PackageEditor::RefreshJSONView()
{
    ui->JSONTextEdit->setText(QString::fromStdString(MANIFESTJSON->dump(4)));
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
        if (!MANIFESTJSONFile.exists())
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] File " << MANIFESTJSONFile.fileName() << " does not exist.";
        }
        if (MANIFESTJSONFile.open(QFile::ReadOnly))
        {
            qDebug() << QTime::currentTime().toString() << " [OUT] File " << MANIFESTJSONFile.fileName() << "opened for reading successfully!";
            QByteArray MANIFESTJSONFileData = MANIFESTJSONFile.readAll();
            if (json::accept(MANIFESTJSONFileData))
            {
                qDebug() << "VALID JSON";
                (*PackageEditor::MANIFESTJSON) = json::parse(MANIFESTJSONFileData);
                qDebug() << "COCOROBOSCOS";
            }
            else
            {
                qDebug() << "INVALID JSON";
                delete this;
            }
        }
        else
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] Could not open file for reading!";
        }
    }
}

void PackageEditor::on_JSONTextEdit_textChanged()
{
    if (json::accept(ui->JSONTextEdit->toPlainText().toUtf8()))
    {
        qDebug() << "VALID JSON";
        ui->JSONTextEdit->setStyleSheet("");
        (*PackageEditor::MANIFESTJSON) = json::parse(ui->JSONTextEdit->toPlainText().toUtf8());
    }
    else
    {
        qDebug() << "INVALID JSON";
        ui->JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
    }
}

