#include "packageeditor.h"
#include "ui_packageeditor.h"

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
    nlohmann::ordered_json RootJSONObject;
    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["PACKAGES"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PATH"))
        {
            RootJSONObject[Key.toStdString()] = nullptr;
        }
    }

    PackageEditor::MANIFESTJSON = new nlohmann::ordered_json(RootJSONObject);
    RefreshJSONView();
    return true;
}

void PackageEditor::on_AddSubGameButton_clicked()
{
    nlohmann::ordered_json NewSubGameObject;
    for (auto Key : (*GlobalConfigJSON)["DefaultTables"]["LIBRARY"]["COLUMNS"].toObject().keys())
    {
        if (!(Key == "PARENTPACKAGE"))
        {
            NewSubGameObject[Key.toStdString()] = nullptr;
        }
    }

    (*MANIFESTJSON)[nlohmann::ordered_json::json_pointer("/SUBGAMES")].push_back(NewSubGameObject);
    //nlohmann::ordered_json
    //QJsonPath::set((*PackageEditor::MANIFESTJSON), QVariantList{"SUBGAMES[-1]"}, NewSubGameObject);
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

        qDebug() << QTime::currentTime().toString() << " [OUT] Parsing JSON " << MANIFESTJSONFile.fileName();

        if (!MANIFESTJSONFile.exists())
        {
            qDebug() << QTime::currentTime().toString() << " [ERR] File " << MANIFESTJSONFile.fileName() << " does not exist.";
        }
        if (MANIFESTJSONFile.open(QFile::ReadOnly))
        {
            qDebug() << QTime::currentTime().toString() << " [OUT] File " << MANIFESTJSONFile.fileName() << " opened for reading successfully!";
            QJsonDocument * JSONDocument = new QJsonDocument(QJsonDocument::fromJson(MANIFESTJSONFile.readAll()));
            MANIFESTJSONFile.close();
            qDebug() << QTime::currentTime().toString() << " [OUT] Parse done!";

            if (JSONDocument->isNull() || JSONDocument->isEmpty())
            {
                qDebug() << QTime::currentTime().toString() << " [ERR] Null or empty JSON.";
                delete JSONDocument;
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
    if (nlohmann::ordered_json::accept(ui->JSONTextEdit->toPlainText().toUtf8()))
    {
        qDebug() << "VALID JSON";
        ui->JSONTextEdit->setStyleSheet("");
        (*PackageEditor::MANIFESTJSON) = nlohmann::ordered_json::parse(ui->JSONTextEdit->toPlainText().toUtf8());
    }
    else
    {
        qDebug() << "INVALID JSON";
        ui->JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
    }
}

