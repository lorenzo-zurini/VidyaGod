#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "packageeditor.h"
#include "jsonoperations.h"
#include "dboperations.h"
#include "filesystemoperations.h"

//TO-DO: ADD VARIABLE SUBSTITUTION WITH ENV VARS AND AUTOCALC
//TO-DO: FIX EXE COMMAND LINE PARSING AND WORKDIR
//IMPLEMENT WINEDLLOVERRIDES
//IMPLEMENT MIRRORFS COMPONENTS
//ADD MORE RUNNERS BASED ON PLATFORM

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    InitClassVariables();
    BuildUI();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::InitClassVariables()
{
    ApplicationPath = QCoreApplication::applicationDirPath();
    ProtonPath = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";
    GlobalConfigJSON = JSONOps::InitGlobalConfigJSON(new QFile("GlobalConfig.JSON"));
}

void MainWindow::on_TestButton_clicked()
{
    qDebug().noquote() << QTime::currentTime().toString() << "MainWindow:" << "[OUT] TEST";
}

void MainWindow::BuildUI()
{   
    QScrollArea * LibraryScrollArea = new QScrollArea(ui->LibraryTab);
    QVBoxLayout * LibraryScrollAreaLayout = new QVBoxLayout(LibraryScrollArea);
    LibraryScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    LibraryScrollArea->setWidgetResizable(1);
    LibraryScrollArea->setLayout(LibraryScrollAreaLayout);
    ui->LibraryTab->layout()->addWidget(LibraryScrollArea);

    QGroupBox * LibraryGroupBox = new QGroupBox(LibraryScrollArea);
    LibraryScrollAreaLayout->addWidget(LibraryGroupBox);
    LibraryScrollArea->setWidget(LibraryGroupBox);
    QFormLayout * LibraryGroupBoxLayout = new QFormLayout(LibraryGroupBox);
    LibraryGroupBox->setLayout(LibraryGroupBoxLayout);

    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        for (int j = 0; j < (*GlobalConfigJSON)["LIBRARY"][i]["SUBGAMES"].size(); j++)
        {
            QGroupBox * GameGroupBox = new QGroupBox(LibraryGroupBox);
            QGridLayout * GameGroupBoxLayout = new QGridLayout(GameGroupBox);
            GameGroupBox->setLayout(GameGroupBoxLayout);
            LibraryGroupBoxLayout->addWidget(GameGroupBox);

            GameGroupBox->setProperty("JSONPath", QString("/LIBRARY/%1/SUBGAMES/%2").arg(QString::number(i)).arg(QString::number(j)));
            GameGroupBox->setProperty("Game", i);
            GameGroupBox->setProperty("SubGame", j + 1);

            QPixmap * GameCover = new QPixmap(QDir::cleanPath(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEPATH"]) + QDir::separator() + "METADATA" + QDir::separator() + QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["SUBGAMES"][j]["COVER"])));
            qDebug() << "PIXMAP:" <<  QDir::cleanPath(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEPATH"]) + QDir::separator() + QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["SUBGAMES"][j]["COVER"]));
            QLabel * GameCoverLabel = new QLabel();
            GameCoverLabel->setPixmap(*GameCover);
            GameGroupBoxLayout->addWidget(GameCoverLabel, 0, 0, 1, 2);

            GameGroupBoxLayout->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["SUBGAMES"][j]["TITLE"])), 1, 0, 1, 2);

            QPushButton * PlayGameButton = new QPushButton(GameGroupBox);
            PlayGameButton->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart));
            GameGroupBoxLayout->addWidget(PlayGameButton, 2, 0, 1, 1);
            QObject::connect(PlayGameButton, &QPushButton::clicked, this, &MainWindow::on_PlayGameButton_clicked);

            QPushButton * EditGameButton = new QPushButton(GameGroupBox);
            EditGameButton->setText("...");
            GameGroupBoxLayout->addWidget(EditGameButton, 2, 1, 1, 1);
            //QObject::connect(EditGameButton, &QPushButton::clicked, this, &PackageEditor::RegeditInComponent);
        }
    }

    QSlider * GridSizeSlider = new QSlider(Qt::Horizontal, ui->LibraryTab);
    GridSizeSlider->setMinimum(3);
    GridSizeSlider->setMaximum(7);
    GridSizeSlider->setSingleStep(1);
    ui->LibraryTab->layout()->addWidget(GridSizeSlider);
}

void MainWindow::on_AddGameButton_clicked()
{
    QDir * PackageDir = new QDir(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));

    if(!FSOps::CheckPackageValid(PackageDir))
    {
        qDebug().noquote() << QTime::currentTime().toString() << "MainWindow:" << "[ERR] Invalid package, aborting..";
        return;
    }

    //Catch nullptr return value of the JSON, returned if parser errorred.
    nlohmann::ordered_json * MANIFESTJSON = JSONOps::LoadJSON(new QFile(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")));
    if (MANIFESTJSON == nullptr)
    {
        qDebug().noquote() << QTime::currentTime().toString() << "MainWindow:" << "[ERR] Parser returned nullptr.";
        delete MANIFESTJSON;
        return;
    }
    else
    {
        qDebug().noquote() << QTime::currentTime().toString() << "MainWindow:" << "[OUT] Parser returned non-empty JSON.";
    }

    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        if ((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEUID"] == (*MANIFESTJSON)["PACKAGEUID"])
        {
            qDebug().noquote() << QTime::currentTime().toString() << "MainWindow:" << "[ERR] Package already exists in library.";
            return;
        }
    }

    (*MANIFESTJSON)["PACKAGEPATH"] = PackageDir->path().toStdString();
    (*GlobalConfigJSON)["LIBRARY"].push_back(*MANIFESTJSON);
    MainWindow::SaveGlobalConfigJSON();

    delete PackageDir;
    delete MANIFESTJSON;

    BuildUI();
}

void MainWindow::on_PlayGameButton_clicked()
{
    QPushButton * Button = qobject_cast<QPushButton *>(QObject::sender());
    nlohmann::ordered_json::json_pointer JSONPointer(Button->parent()->property("JSONPath").toString().toStdString());
    int subgame = Button->parent()->property("SubGame").toInt();
    nlohmann::ordered_json * MANIFESTJSON = new nlohmann::ordered_json((*GlobalConfigJSON)[JSONPointer.parent_pointer().parent_pointer()]);

    qDebug().noquote() << QTime::currentTime().toString() << "[OUT] MainWindow:" << "Running game" << QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][subgame - 1]["TITLE"]);

    Runner * GameRunner = new Runner(new QDir(QString::fromStdString((*MANIFESTJSON)["PACKAGEPATH"])), MANIFESTJSON, GlobalConfigJSON, subgame);

    GameRunner->Cleanup();
    GameRunner->BuildRuntime();
    GameRunner->Run();
    QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");
    GameRunner->Cleanup();

    delete GameRunner;
    delete MANIFESTJSON;
}

void MainWindow::on_EditGameButton_clicked()
{

}

void MainWindow::on_CreatePackageButton_clicked()
{
    PackageEditor * PackageEditor = new class PackageEditor(GlobalConfigJSON, this);
    PackageEditor->show();
}

bool MainWindow::SaveGlobalConfigJSON()
{
    return JSONOps::SaveJSON(MainWindow::GlobalConfigJSON, new QFile("GlobalConfig.JSON"));
}
