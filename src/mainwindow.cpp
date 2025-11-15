#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "packageeditor.h"
#include "jsonoperations.h"
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
    BuildStaticUI();
    BuildLibraryGameCards();
    BuildLibraryDynamicUI();
    BuildPackagesDynamicUI();
    this->setFixedWidth(1280); //Fixed Steam Deck resolution.
    this->setFixedHeight(800);
}

MainWindow::~MainWindow()
{
    delete ui;
}

bool MainWindow::InitClassVariables()
{
    ApplicationPath = QCoreApplication::applicationDirPath();

    AppDataDir = new QDir(QDir::homePath() +  "/.VidyaGod");
    AppDataDir->mkpath(".");

    GlobalConfigFile = new QFile(AppDataDir->filePath("GlobalConfig.JSON"));
    if (!GlobalConfigFile->exists())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "JSONOperations:" << "[OUT] Config flie not deteced. Creating... " << std::endl;
        QFile * DefaultConfigFile = new QFile("DefaultConfig.JSON");
        if (!DefaultConfigFile->exists())
        {
            std::cout << "DefaultConfig.JSON not found, aborting." << std::endl;
            return false;
        }

        DefaultConfigFile->copy(GlobalConfigFile->fileName());
        delete DefaultConfigFile;
    }
    GlobalConfigJSON = JSONOps::LoadJSON(GlobalConfigFile);

    ProtonPath = "/usr/share/steam/compatibilitytools.d/proton-ge-custom/";

    return true;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    //if (LibraryScrollArea->widget())
    //    LibraryScrollArea->widget()->setFixedWidth(LibraryScrollArea->viewport()->width());

    QMainWindow::resizeEvent(event);
}

void MainWindow::BuildStaticUI()
{
    MainWindowTabWidget = new QTabWidget(ui->MainWidget);
    ui->MainWidget->layout()->addWidget(MainWindowTabWidget);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    LibraryTabWidget = new QWidget(MainWindowTabWidget);
    LibraryTabWidgetLayout = new QVBoxLayout(LibraryTabWidget);
    LibraryTabWidget->setLayout(LibraryTabWidgetLayout);

    MainWindowTabWidget->addTab(LibraryTabWidget, "Library");

    LibraryScrollArea = new QScrollArea(LibraryTabWidget);
    LibraryScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    LibraryScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    LibraryTabWidgetLayout->addWidget(LibraryScrollArea);
    LibraryScrollArea->setWidgetResizable(1);

    //QSlider * GridSizeSlider = new QSlider(Qt::Horizontal, LibraryTabWidget);
    //GridSizeSlider->setValue((*GlobalConfigJSON)["Settings"]["LibraryGridSize"]);
    //GridSizeSlider->setMinimum(3);
    //GridSizeSlider->setMaximum(7);
    //GridSizeSlider->setSingleStep(1);
    //LibraryTabWidgetLayout->addWidget(GridSizeSlider);
    //QObject::connect(GridSizeSlider, &QSlider::sliderReleased, this, &MainWindow::MainWindowGridSizeChanged);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    PackagesTabWidget = new QWidget(MainWindowTabWidget);
    PackagesTabWidgetLayout = new QVBoxLayout(PackagesTabWidget);
    PackagesTabWidget->setLayout(PackagesTabWidgetLayout);

    MainWindowTabWidget->addTab(PackagesTabWidget, "Packages");

    QGroupBox * PackagesToolbarGroupBox = new QGroupBox(PackagesTabWidget);
    QHBoxLayout * PackagesToolbarGroupBoxLayout = new QHBoxLayout(PackagesToolbarGroupBox);
    PackagesToolbarGroupBox->setLayout(PackagesToolbarGroupBoxLayout);
    PackagesTabWidgetLayout->addWidget(PackagesToolbarGroupBox);

    QPushButton * AddPackageButton = new QPushButton("+", PackagesToolbarGroupBox);
    AddPackageButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    PackagesToolbarGroupBoxLayout->addWidget(AddPackageButton);
    QObject::connect(AddPackageButton, &QPushButton::clicked, this, &MainWindow::on_AddGameButton_clicked);

    QPushButton * OpenPackageEditorButton = new QPushButton("Package Editor", PackagesToolbarGroupBox);
    OpenPackageEditorButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    PackagesToolbarGroupBoxLayout->addWidget(OpenPackageEditorButton);
    QObject::connect(OpenPackageEditorButton, &QPushButton::clicked, this, [this]{PackageEditor * NewPackageEditor = new PackageEditor(this->GlobalConfigJSON, this); NewPackageEditor->show();});

    PackagesScrollArea = new QScrollArea(PackagesTabWidget);
    PackagesScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    PackagesScrollArea->setWidgetResizable(1);
    PackagesTabWidgetLayout->addWidget(PackagesScrollArea);


    ///////////////////////////////////////////////////////////////////////////////////////////////
}

void MainWindow::BuildLibraryGameCards()
{
    LibraryGameCards->clear();
    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        for (int j = 0; j < (*GlobalConfigJSON)["LIBRARY"][i]["SUBGAMES"].size(); j++)
        {
            LibraryGameCard * NewGameCard = new LibraryGameCard(nullptr);
            NewGameCard->PassGlobalConfigJSON(GlobalConfigJSON);
            NewGameCard->SetGame(i, j + 1);
            NewGameCard->InitializeClassVariables();
            LibraryGameCards->append(NewGameCard);
        }
    }
}

void MainWindow::BuildLibraryDynamicUI()
{
    std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[OUT] Building LibraryDynamicUI" << std::endl;
    if (LibraryScrollArea->widget() != nullptr)
    {
        //Reparent all LibraryGameCards so they survive LibraryWidget being deleted.
        for (int i = 0; i < LibraryGameCards->count(); i++)
        {
            LibraryGameCards->at(i)->setParent(nullptr);
        }
        LibraryScrollArea->widget()->deleteLater();
    }

    QWidget * LibraryWidget = new QWidget(LibraryScrollArea);
    QGridLayout * LibraryWidgetLayout = new QGridLayout(LibraryWidget);
    LibraryWidget->setLayout(LibraryWidgetLayout);

    LibraryScrollArea->setWidget(LibraryWidget);
    LibraryScrollArea->setWidgetResizable(true);

    LibraryWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    LibraryWidgetLayout->setContentsMargins(0, 0, 5, 0);
    LibraryWidgetLayout->setSpacing(30);

    if (LibraryGameCards->empty())
    {
        std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[ERR] GameCard array is empty, aborting building LibraryDynamicUI" << std::endl;
        return;
    }

    int GridSize = 4;
    //int GridSize = (*GlobalConfigJSON)["Settings"]["LibraryGridSize"];
    for (int i = 0; i < LibraryGameCards->count(); i++)
    {
        LibraryGameCards->at(i)->setParent(LibraryWidget);
        LibraryGameCards->at(i)->GridSize = GridSize;
        LibraryWidgetLayout->addWidget(LibraryGameCards->at(i), i / GridSize, i % GridSize);
    }

    for (int column = 0; column < GridSize; ++column)
    {
        LibraryWidgetLayout->setColumnStretch(column, 1);
    }

    LibraryWidgetLayout->setRowStretch((LibraryGameCards->count() + GridSize - 1) / GridSize, 1);
}

void MainWindow::BuildPackagesDynamicUI()
{
    if (PackagesScrollArea->widget() != nullptr)
    {
        PackagesScrollArea->widget()->deleteLater();
    }

    QWidget * PackagesWidget = new QWidget(PackagesScrollArea);
    PackagesScrollArea->setWidget(PackagesWidget);
    QGridLayout * PackagesWidgetLayout = new QGridLayout(PackagesWidget);
    PackagesWidget->setLayout(PackagesWidgetLayout);

    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        PackagesWidgetLayout->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGENAME"]), PackagesWidget), i, 0);
        PackagesWidgetLayout->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEUID"]), PackagesWidget), i, 1);
        PackagesWidgetLayout->addWidget(new QLabel(QString::fromStdString((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEVERSION"]), PackagesWidget), i, 2);

        QPushButton * RemovePackageButton = new QPushButton("Remove", PackagesWidget);
        QObject::connect(RemovePackageButton, &QPushButton::clicked, this, [this, i]{(*GlobalConfigJSON)["LIBRARY"].erase(i); this->RebuildDynamicUI(); this->SaveGlobalConfigJSON();});
        PackagesWidgetLayout->addWidget(RemovePackageButton, i, 3);
    }

    PackagesWidgetLayout->setRowStretch(PackagesWidgetLayout->rowCount(), 1);
}

void MainWindow::RebuildDynamicUI()
{
    this->BuildLibraryGameCards();
    this->BuildLibraryDynamicUI();
    this->BuildPackagesDynamicUI();
    return;
}

void MainWindow::on_AddGameButton_clicked()
{
    QDir * PackageDir = new QDir(QFileDialog::getExistingDirectory(this, "Select GAMEDIR..."));

    if(!FSOps::CheckPackageValid(PackageDir))
    {
        std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[ERR] Invalid package, aborting.." << std::endl;
        return;
    }

    //Catch nullptr return value of the JSON, returned if parser errorred.
    nlohmann::ordered_json * MANIFESTJSON = JSONOps::LoadJSON(new QFile(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")));
    if (MANIFESTJSON == nullptr)
    {
        std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[ERR] Parser returned nullptr." << std::endl;
        delete MANIFESTJSON;
        return;
    }
    else
    {
        std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[OUT] Parser returned non-empty JSON." << std::endl;
    }

    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        if ((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEUID"] == (*MANIFESTJSON)["PACKAGEUID"])
        {
            std::cout << QTime::currentTime().toString().toStdString() << "MainWindow:" << "[ERR] Package already exists in library." << std::endl;
            return;
        }
    }

    (*MANIFESTJSON)["PACKAGEPATH"] = PackageDir->path().toStdString();
    (*GlobalConfigJSON)["LIBRARY"].push_back(*MANIFESTJSON);
    MainWindow::SaveGlobalConfigJSON();

    delete PackageDir;
    delete MANIFESTJSON;

    this->RebuildDynamicUI();
}

bool MainWindow::SaveGlobalConfigJSON()
{
    return JSONOps::SaveJSON(MainWindow::GlobalConfigJSON, new QFile(AppDataDir->filePath("GlobalConfig.JSON")));
}

void MainWindow::MainWindowGridSizeChanged()
{
    QSlider * Slider = qobject_cast<QSlider *>(QObject::sender());
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Set GridSize" << Slider->value() << std::endl;
    (*GlobalConfigJSON)["Settings"]["LibraryGridSize"] = Slider->value();
    MainWindow::SaveGlobalConfigJSON();
    BuildLibraryDynamicUI();
}

LibraryGameCard::LibraryGameCard(QWidget * parent) : QGroupBox(parent)
{   
    LibraryGameCardLayout = new QVBoxLayout(this);
    //this->setFlat(true);
    this->setLayout(LibraryGameCardLayout);
    LibraryGameCardLayout->setSpacing(0);
    LibraryGameCardLayout->setContentsMargins(0, 0, 0, 0);

    CoverLabel = new QLabel(this);
    CoverLabel->setAlignment(Qt::AlignCenter);
    CoverLabel->setScaledContents(true); // We'll scale manually to maintain aspect ratio
    CoverLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    LibraryGameCardLayout->addWidget(CoverLabel);

    //TitleLabel = new QLabel(this);
    //TitleLabel->setAlignment(Qt::AlignCenter);
    //TitleLabel->setWordWrap(true);
    //LibraryGameCardLayout->addWidget(TitleLabel, 1, 0, 1, 2);

    ButtonsGroupBox = new QGroupBox(this);
    ButtonsGroupBox->setFlat(true);
    ButtonsGroupBoxLayout = new QHBoxLayout(ButtonsGroupBox);
    ButtonsGroupBox->setFixedHeight(40);
    ButtonsGroupBoxLayout->setSpacing(0);
    ButtonsGroupBoxLayout->setContentsMargins(0, 0, 0, 0);
    LibraryGameCardLayout->addWidget(ButtonsGroupBox);

    PlayButton = new QPushButton(ButtonsGroupBox);
    PlayButton->setFlat(true);
    PlayButton->setText("Play");
    //PlayButton->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MediaPlaybackStart));
    ButtonsGroupBoxLayout->addWidget(PlayButton);
    QObject::connect(PlayButton, &QPushButton::clicked, this, &LibraryGameCard::on_PlayGameButton_clicked);

    EditButton = new QPushButton(ButtonsGroupBox);
    EditButton->setFlat(true);
    EditButton->setText("...");
    ButtonsGroupBoxLayout->addWidget(EditButton);

    //ButtonsGroupBox->setHidden(true);

    //QObject::connect(this, &QGroupBox::clicked, this, &LibraryGameCard::on_GameCard_clicked);
}

void LibraryGameCard::PassGlobalConfigJSON(nlohmann::ordered_json * PassedGlobalConfigJSON)
{
    this->GlobalConfigJSON = PassedGlobalConfigJSON;
    return;
}

void LibraryGameCard::SetGame(int PassedGame, int PasedSubGame)
{
    this->Game = PassedGame;
    this->SubGame = PasedSubGame;
    return;
}

void LibraryGameCard::InitializeClassVariables()
{
    //Don't call this BEFORE SetGame and PassGlobalConfigJSON!!!
    this->MANIFESTJSON = new nlohmann::ordered_json((*GlobalConfigJSON)["LIBRARY"][Game]);
    this->PackagePath = QString::fromStdString((*MANIFESTJSON)["PACKAGEPATH"]);

    this->GameTitle = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][SubGame - 1]["TITLE"]);
    //this->TitleLabel->setText(this->GameTitle);

    this->CoverLabel->setPixmap(QPixmap(QDir::cleanPath(this->PackagePath + QDir::separator() + "METADATA" + QDir::separator() + QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][SubGame - 1]["COVER"]))));
    //this->CoverPixmap = new QPixmap(QDir::cleanPath(this->PackagePath + QDir::separator() + "METADATA" + QDir::separator() + QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][SubGame - 1]["COVER"])));
    //this->CoverLabel->setPixmap(CoverPixmap->scaled(CoverLabel->width(), CoverLabel->width() * this->CoverPixmap->height() / width(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void LibraryGameCard::on_PlayGameButton_clicked()
{
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Running game" << (*this->MANIFESTJSON)["SUBGAMES"][this->SubGame - 1]["TITLE"] << std::endl;

    Runner * GameRunner = new Runner(new QDir(this->PackagePath), this->MANIFESTJSON, this->GlobalConfigJSON, this->SubGame);

    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Executing pre-run cleanup." << std::endl;
    GameRunner->Cleanup();
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Building runtime." << std::endl;
    GameRunner->BuildRuntime();
    std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Running game" << (*this->MANIFESTJSON)["SUBGAMES"][this->SubGame - 1]["TITLE"] << std::endl;
    GameRunner->Run();
    QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");
    GameRunner->Cleanup();

    delete GameRunner;
    //delete MANIFESTJSON;
}

void LibraryGameCard::on_GameCard_clicked()
{
    //qDebug() << "CLICKED GAME CARD" << this->GameTitle;
}

void LibraryGameCard::resizeEvent(QResizeEvent * event) {
    QGroupBox::resizeEvent(event);
    this->CoverLabel->setFixedHeight(this->CoverLabel->width() * 3 / 2);
    this->ButtonsGroupBox->setAlignment(Qt::AlignBottom);
    //this->setFixedHeight(this->width() * 1.8);
    //float CoverAspectRatio = this->CoverPixmap->height() / width();
    //if (!this->CoverPixmap->isNull())
    //{
    //    CoverLabel->setPixmap(CoverPixmap->scaled(CoverLabel->width(), CoverLabel->width() * CoverAspectRatio, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    //}
    //this->TitleLabel->setFixedWidth(this->width());
    //this->TitleLabel->setFixedHeight(this->TitleLabel->width() * 0.2);
    //emit Resized(event->size());
}
