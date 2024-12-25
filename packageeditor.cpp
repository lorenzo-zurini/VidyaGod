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
    BuildUI();
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
    BuildUI();
    RefreshJSONView();
}

void PackageEditor::on_AddComponentButton_clicked()
{
    (*MANIFESTJSON)[json::json_pointer("/COMPONENTS")].push_back(json::object({{"NAME", nullptr}}));
    RefreshJSONView();

    QWidget * NewTabWidget = new QWidget(this);
    ui->PackageEditorTabWidget->addTab(NewTabWidget, QString("Component " + QString::number(ui->PackageEditorTabWidget->count() - 1)));
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
    JSONTextEdit->setText(QString::fromStdString(MANIFESTJSON->dump(4)));
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

bool PackageEditor::BuildUI()
{
    //JSON TAB
    ui->PackageEditorTabWidget->clear();

    delete PackageEditor::ManifestTabWidget;

    PackageEditor::JSONTabWidget = new QWidget(ui->PackageEditorTabWidget);
    QVBoxLayout * JSONTabWidgetLayout = new QVBoxLayout(JSONTabWidget);
    JSONTabWidget->setLayout(JSONTabWidgetLayout);

    PackageEditor::JSONTextEdit = new QTextEdit(JSONTabWidget);
    JSONTabWidgetLayout->addWidget(JSONTextEdit);
    JSONTextEdit->setText(QString::fromStdString(MANIFESTJSON->dump(4)));
    QObject::connect(JSONTextEdit, &QTextEdit::textChanged, this, &PackageEditor::JSONQTextEditChanged);

    PackageEditor::SaveJSONButton = new QPushButton(JSONTabWidget);
    SaveJSONButton->setText("Save JSON");
    JSONTabWidgetLayout->addWidget(SaveJSONButton);
    QObject::connect(SaveJSONButton, &QPushButton::clicked, this, &PackageEditor::SaveJSONButtonPressed);

    ui->PackageEditorTabWidget->addTab(JSONTabWidget, "JSON");

    //MANIFEST TAB
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

        if(!(*PackageEditor::MANIFESTJSON)[JSONPointer].is_null())
        {
            NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
        }

        QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
        PackageDataGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
    }

    //SUBGAMES TABS WIDGET
    QTabWidget * SubGamesTabWidget = new QTabWidget(ManifestTabWidget);
    ManifestTabWidgetLayout->addWidget(SubGamesTabWidget);

    for (int i = 0; i < (*PackageEditor::MANIFESTJSON)["SUBGAMES"].size(); i++)
    {
        QWidget * SubGameTabWidget = new QWidget(SubGamesTabWidget);
        SubGameTabWidget->setProperty("JSONPath", QString("/SUBGAMES/%1").arg(QString::number(i)));
        QVBoxLayout * SubGameTabLayout = new QVBoxLayout(SubGameTabWidget);
        SubGameTabWidget->setLayout(SubGameTabLayout);

        QScrollArea * SubGameScrollArea = new QScrollArea(SubGameTabWidget);
        QVBoxLayout * SubGameScrollAreaLayout = new QVBoxLayout(SubGameScrollArea);
        SubGameScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        SubGameScrollArea->setWidgetResizable(1);
        SubGameScrollArea->setLayout(SubGameScrollAreaLayout);
        SubGameTabLayout->addWidget(SubGameScrollArea);

        QGroupBox * SubGameGroupBox = new QGroupBox(SubGameScrollArea);
        SubGameScrollAreaLayout->addWidget(SubGameGroupBox);
        SubGameScrollArea->setWidget(SubGameGroupBox);
        QFormLayout * SubGameGroupBoxLayout = new QFormLayout(SubGameGroupBox);
        SubGameGroupBox->setLayout(SubGameGroupBoxLayout);

        for (auto Item : (*PackageEditor::MANIFESTJSON)["SUBGAMES"][i].items())
        {
            qDebug().noquote() << QTime::currentTime().toString() << "[OUT] Adding parameter editor:" << QString::fromStdString(Item.key());
            QLineEdit * NewParamField = new QLineEdit(SubGameGroupBox);
            QString JSONPath = QString("/SUBGAMES/%1/%2").arg(QString::number(i)).arg(QString::fromStdString(Item.key()));
            nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
            NewParamField->setProperty("JSONPath", JSONPath);

            if(!(*PackageEditor::MANIFESTJSON)[JSONPointer].is_null())
            {
                NewParamField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
            }

            QObject::connect(NewParamField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
            SubGameGroupBoxLayout->addRow(QString::fromStdString(Item.key()), NewParamField);
        }
        SubGamesTabWidget->addTab(SubGameTabWidget, QString("Subgame %1").arg(QString::number(i + 1)));
    }
    ui->PackageEditorTabWidget->addTab(ManifestTabWidget, "MANIFEST");

    //INDIVIDUAL COMPONENTS TABS
    for (int i = 0; i < (*PackageEditor::MANIFESTJSON)["COMPONENTS"].size(); i++)
    {
        QWidget * ComponentTabWidget = new QWidget(ui->PackageEditorTabWidget);
        ComponentTabWidget->setProperty("JSONPath", QString("/COMPONENTS/%1").arg(QString::number(i)));
        QVBoxLayout * ComponentTabWidgetLayout = new QVBoxLayout(ComponentTabWidget);
        ComponentTabWidget->setLayout(ComponentTabWidgetLayout);

            QGroupBox * ComponentNameGroupBox = new QGroupBox(ComponentTabWidget);
            ComponentTabWidgetLayout->addWidget(ComponentNameGroupBox);
            QFormLayout * ComponentNameGroupBoxLayout = new QFormLayout(ComponentNameGroupBox);
            ComponentNameGroupBox->setLayout(ComponentNameGroupBoxLayout);

                QLineEdit * ComponentNameField = new QLineEdit(ComponentNameGroupBox);
                QString JSONPath = QString("/COMPONENTS/%1/NAME").arg(QString::number(i));
                nlohmann::ordered_json::json_pointer JSONPointer(JSONPath.toStdString());
                ComponentNameField->setProperty("JSONPath", JSONPath);

                if(!(*PackageEditor::MANIFESTJSON)[JSONPointer].is_null())
                {
                    ComponentNameField->setText(QString::fromStdString((*PackageEditor::MANIFESTJSON)[JSONPointer]));
                }

                QObject::connect(ComponentNameField, &QLineEdit::editingFinished, this, &PackageEditor::JSONQLineEditChanged);
                ComponentNameGroupBoxLayout->addRow("NAME", ComponentNameField);


            QScrollArea * SubComponentsScrollArea = new QScrollArea(ComponentTabWidget);
            ComponentTabWidgetLayout->addWidget(SubComponentsScrollArea);
            QVBoxLayout * SubComponentsScrollAreaLayout = new QVBoxLayout(SubComponentsScrollArea);
            SubComponentsScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            SubComponentsScrollArea->setWidgetResizable(1);

                QPushButton * RunExeButton = new QPushButton(ComponentTabWidget);
                RunExeButton->setText("Run EXE");
                SubComponentsScrollAreaLayout->addWidget(RunExeButton);



        ui->PackageEditorTabWidget->addTab(ComponentTabWidget, QString("Component %1").arg(QString::number(i + 1)));
    }

    return true;
}

void PackageEditor::JSONQLineEditChanged()
{
    QLineEdit * Editor = qobject_cast<QLineEdit *>(QObject::sender());
    QString String = Editor->text();
    nlohmann::ordered_json::json_pointer JSONPointer(Editor->property("JSONPath").toString().toStdString());
    (*PackageEditor::MANIFESTJSON)[JSONPointer] = String.toStdString();
    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] JSON value:" << Editor->text() << "Submitted to:" << Editor->property("JSONPath").toString();
    PackageEditor::SaveManifestJSON();
    PackageEditor::RefreshJSONView();
}
void PackageEditor::JSONQTextEditChanged()
{
    qDebug() << "JSON TEXT EDITOR CHANGED";
    if (nlohmann::ordered_json::accept(PackageEditor::JSONTextEdit->toPlainText().toUtf8()))
    {
        qDebug().noquote() << QTime::currentTime().toString() << " [OUT] Valid JSON!";
        PackageEditor::JSONTextEdit->setStyleSheet("");
        PackageEditor::SaveJSONButton->setDisabled(false);
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << " [ERR] Invalid JSON!";
        PackageEditor::JSONTextEdit->setStyleSheet("background-color:#58111A; color: white;");
        PackageEditor::SaveJSONButton->setDisabled(true);
    }
}

void PackageEditor::SaveJSONButtonPressed()
{
    qDebug() << "SAVE TRIGGERED";
    (*PackageEditor::MANIFESTJSON) = nlohmann::ordered_json::parse(PackageEditor::JSONTextEdit->toPlainText().toUtf8());
    PackageEditor::SaveManifestJSON();
    BuildUI();
}
