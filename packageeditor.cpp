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

    InitPackage();
    InitMANIFESTJSON();
    RefreshJSONView();

    BuildUI();
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
    if (PackageEditor::SaveManifestJSON())
    {
        qDebug().noquote() << QTime::currentTime() << "[OUT] Save successful.";
    }
    else
    {
        qDebug().noquote() << QTime::currentTime() << "[ERR] Save failed.";
    }
}


bool PackageEditor::SaveManifestJSON()
{
    return JSONOps::SaveJSON(PackageEditor::MANIFESTJSON, new QFile(MetadataDir->filePath("MANIFEST.json")));
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

    PackageEditor::MANIFESTJSON = JSONOps::LoadJSON(new QFile(MetadataDir->filePath("MANIFEST.json")));
    if (!(PackageEditor::MANIFESTJSON == nullptr))
    {
        return;
    }

    qDebug().noquote() << QTime::currentTime() << " [ERR] Parser returner nullptr, creating new JSON.";
    PackageEditor::MANIFESTJSON = new nlohmann::ordered_json;
}

void PackageEditor::on_JSONTextEdit_textChanged()
{
    if (json::accept(ui->JSONTextEdit->toPlainText().toUtf8()))
    {
        qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Valid JSON!";
        ui->JSONTextEdit->setStyleSheet("");
        (*PackageEditor::MANIFESTJSON) = json::parse(ui->JSONTextEdit->toPlainText().toUtf8());
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << " [ERR] Invalid JSON!";
        ui->JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
    }
}

bool PackageEditor::BuildUI()
{
    PackageEditor::ManifestTabWidget = new QWidget(ui->PackageEditorTabWidget);
    QVBoxLayout * ManifestTabWidgetLayout = new QVBoxLayout(ManifestTabWidget);
    ManifestTabWidget->setLayout(ManifestTabWidgetLayout);

    QGroupBox * PackageDataGroupBox = new QGroupBox(ManifestTabWidget);
    ManifestTabWidgetLayout->addWidget(PackageDataGroupBox);
    QFormLayout * PackageDataGroupBoxLayout = new QFormLayout(PackageDataGroupBox);
    PackageDataGroupBox->setLayout(PackageDataGroupBoxLayout);

    for (auto Item : (*PackageEditor::MANIFESTJSON).items())
    {
        if ((Item.key() == "SUBGAMES") || (Item.key() == "COMPONENTS"))
        {
            continue;
        }

        qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Adding parameter editor:" << Item.key();
        QLineEdit * NewParamField = new QLineEdit(PackageDataGroupBox);
        QString JSONPath = QString::fromStdString(Item.key()).prepend("/");
        nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
        NewParamField->setProperty("JSONPath", JSONPath);
        NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
        QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONEditorEdited);
        PackageDataGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
    }

    for (auto Item : (*PackageEditor::MANIFESTJSON)["SUBGAMES"].items())
    {
        QGroupBox * SubGameGroupBox = new QGroupBox(ManifestTabWidget);
        ManifestTabWidgetLayout->addWidget(SubGameGroupBox);
        QFormLayout * SubGameGroupBoxLayout = new QFormLayout(SubGameGroupBox);
        SubGameGroupBox->setLayout(SubGameGroupBoxLayout);

        nlohmann::ordered_json SubGameJSON = Item.value();
        for (int i = 0; i < SubGameJSON.size(); i++)
        {
            //qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Adding parameter editor:" << QString::fromStdString(SubGameJSON.items().);
            //QLineEdit * NewParamField = new QLineEdit(SubGameGroupBox);
            //QString JSONPath = QString::fromStdString(Item.key()).prepend("/");
            //nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
            //NewParamField->setProperty("JSONPath", JSONPath);
            //NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
            //QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONEditorEdited);
            //PackageDataGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
        }
    }
    ui->PackageEditorTabWidget->addTab(ManifestTabWidget, "MANIFEST");
    return true;
}

void PackageEditor::JSONEditorEdited()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(QObject::sender());
    QString String = Editor->text();
    nlohmann::ordered_json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = String.toStdString();
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] JSON value:" << Editor->text() << "Submitted to:" << Editor->property("JSONPath").toString();
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}
