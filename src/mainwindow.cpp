#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "commonutils.h"

#include "packageeditor.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"

//TO-DO: ADD VARIABLE SUBSTITUTION WITH ENV VARS AND AUTOCALC
//TO-DO: FIX EXE COMMAND LINE PARSING AND WORKDIR
//IMPLEMENT WINEDLLOVERRIDES
//IMPLEMENT MIRRORFS COMPONENTS
//ADD MORE RUNNERS BASED ON PLATFORM

MainWindow::MainWindow(nlohmann::ordered_json * PassedGlobalConfigJSON, QDir * PassedAppDataDir, QWidget * parent)
    : GlobalConfigJSON(PassedGlobalConfigJSON), AppDataDir(PassedAppDataDir), QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    BuildStaticUI();
    BuildLibraryGameCards();
    BuildLibraryDynamicUI();
    BuildPackagesDynamicUI();
    //this->setFixedWidth(1280); //Fixed Steam Deck resolution.
    //this->setFixedHeight(800);
}

MainWindow::~MainWindow()
{
    delete ui;
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
    LibraryTabWidgetLayout->setContentsMargins(0, 0, 0, 0);
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
        std::string PackagePath = (*GlobalConfigJSON)["LIBRARY"][i]["PATH"];
        nlohmann::ordered_json PackageManifest;
        QFile ManifestFile(QString::fromStdString(PackagePath + "/METADATA/MANIFEST.json"));
        if (JSONOps::LoadJSON(&ManifestFile, &PackageManifest))
        {
            LogErr("MainWindow", "Could not load MANIFEST for " + PackagePath + ", skipping.");
            continue;
        }

        for (int j = 0; j < (int)PackageManifest["SUBGAMES"].size(); j++)
        {
            std::string SubgameID = PackageManifest["SUBGAMES"][j].contains("SUBGAMEID") && !PackageManifest["SUBGAMES"][j]["SUBGAMEID"].is_null()
                                    ? std::string(PackageManifest["SUBGAMES"][j]["SUBGAMEID"]) : "";
            LibraryGameCard * NewGameCard = new LibraryGameCard(GlobalConfigJSON, i, SubgameID, nullptr);
            NewGameCard->InitializeClassVariables();
            LibraryGameCards->append(NewGameCard);
        }
    }
}

void MainWindow::BuildLibraryDynamicUI()
{
    LogOut("MainWindow", "Building LibraryDynamicUI");
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
    LibraryWidgetLayout->setContentsMargins(5, 5, 5, 5);
    LibraryWidgetLayout->setAlignment(Qt::AlignTop);
    LibraryWidgetLayout->setSpacing(10);

    if (LibraryGameCards->empty())
    {
        LogErr("MainWindow", "GameCard array is empty, aborting building LibraryDynamicUI");
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
        LogErr("MainWindow", "Invalid package, aborting..");
        return;
    }

    //Catch nullptr return value of the JSON, returned if parser errorred.

    nlohmann::ordered_json * MANIFESTJSON = new nlohmann::ordered_json;
    if(JSONOps::LoadJSON(new QFile(QDir::cleanPath(PackageDir->path() + QDir::separator() + "METADATA" + QDir::separator() + "MANIFEST.json")), MANIFESTJSON))
    {
        LogErr("MainWindow", "Parser returned nullptr.");
        delete MANIFESTJSON;
        return;
    }

    for (int i = 0; i < (*GlobalConfigJSON)["LIBRARY"].size(); i++)
    {
        if ((*GlobalConfigJSON)["LIBRARY"][i]["PACKAGEUID"] == (*MANIFESTJSON)["PACKAGEUID"])
        {
            LogErr("MainWindow", "Package already exists in library.");
            return;
        }
    }

    nlohmann::ordered_json SlimEntry;
    SlimEntry["PACKAGEUID"]     = (*MANIFESTJSON)["PACKAGEUID"];
    SlimEntry["PACKAGENAME"]    = (*MANIFESTJSON)["PACKAGENAME"];
    SlimEntry["PACKAGEVERSION"] = (*MANIFESTJSON)["PACKAGEVERSION"];
    SlimEntry["PATH"]           = PackageDir->path().toStdString();
    (*GlobalConfigJSON)["LIBRARY"].push_back(SlimEntry);
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
    LogOut("MainWindow", "Set GridSize " + std::to_string(Slider->value()));
    (*GlobalConfigJSON)["Settings"]["LibraryGridSize"] = Slider->value();
    MainWindow::SaveGlobalConfigJSON();
    BuildLibraryDynamicUI();
}

LibraryGameCard::LibraryGameCard(nlohmann::ordered_json * PassedGlogalConfigJSON, int PassedGame, std::string PassedSubgameID, QWidget * parent)
    : GlobalConfigJSON(PassedGlogalConfigJSON), Game(PassedGame), SubgameID(PassedSubgameID), QWidget(parent)
{
    LibraryGameCardLayout = new QVBoxLayout(this);
    //this->setFlat(true);
    this->setLayout(LibraryGameCardLayout);
    LibraryGameCardLayout->setSpacing(0);
    LibraryGameCardLayout->setContentsMargins(0, 0, 0, 0);

    CoverLabel = new QLabel(this);
    CoverLabel->setAlignment(Qt::AlignCenter);
    CoverLabel->setScaledContents(true);
    CoverLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    LibraryGameCardLayout->addWidget(CoverLabel);

    ButtonsGroupBox = new QWidget(this);
    ButtonsGroupBoxLayout = new QHBoxLayout(ButtonsGroupBox);
    ButtonsGroupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ButtonsGroupBoxLayout->setSpacing(0);
    ButtonsGroupBoxLayout->setContentsMargins(0, 0, 0, 0);

    PlayButton = new QPushButton(ButtonsGroupBox);
    PlayButton->setFlat(true);
    PlayButton->setText("Play");
    ButtonsGroupBoxLayout->addWidget(PlayButton);
    QObject::connect(PlayButton, &QPushButton::clicked, this, &LibraryGameCard::on_PlayGameButton_clicked);

    EditButton = new QPushButton(ButtonsGroupBox);
    EditButton->setFlat(true);
    EditButton->setText("...");
    ButtonsGroupBoxLayout->addWidget(EditButton);
    LibraryGameCardLayout->addWidget(ButtonsGroupBox);

    //ButtonsGroupBox->setHidden(true);
    //QObject::connect(this, &QGroupBox::clicked, this, &LibraryGameCard::on_GameCard_clicked);
}

void LibraryGameCard::InitializeClassVariables()
{
    this->PackagePath = std::filesystem::path(std::string((*GlobalConfigJSON)["LIBRARY"][Game]["PATH"]));

    this->MANIFESTJSON = new nlohmann::ordered_json;
    QFile ManifestFile(QString::fromStdString(this->PackagePath.string() + "/METADATA/MANIFEST.json"));
    if (JSONOps::LoadJSON(&ManifestFile, this->MANIFESTJSON))
    {
        LogErr("LibraryGameCard", "Could not load MANIFEST from " + this->PackagePath.string());
        return;
    }

    int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, this->SubgameID);
    if (SubgameIdx == -1) return;
    this->GameTitle = QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["TITLE"]);
    this->CoverLabel->setPixmap(QPixmap(QDir::cleanPath(QString::fromStdString(this->PackagePath.string()) + QDir::separator() + "METADATA" + QDir::separator() + QString::fromStdString((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["COVER"]))));
}

void LibraryGameCard::on_PlayGameButton_clicked()
{
    int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, this->SubgameID);
    LogOut("MainWindow", "Running game " + (SubgameIdx != -1 ? std::string((*this->MANIFESTJSON)["SUBGAMES"][SubgameIdx]["TITLE"]) : this->SubgameID));

    struct ContainerParams NewContainerParams = ContainerParams(this->PackagePath, this->SubgameID);
    class ContainerWrapper NewContainerWrapper = ContainerWrapper((*GlobalConfigJSON), (*this->MANIFESTJSON), NewContainerParams);
    if (!NewContainerWrapper.BuildContainerRuntime())
    {
        NewContainerWrapper.Cleanup();
        QMessageBox::critical(nullptr, "Launch failed", "Failed to build container runtime.\nCheck that all components are defined and their zip files exist.");
        return;
    }
    if (!NewContainerWrapper.Execute())
    {
        NewContainerWrapper.Cleanup();
        QMessageBox::critical(nullptr, "Launch failed", "Failed to execute the game.\nCheck the runner configuration and file paths.");
        return;
    }
    QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");
    NewContainerWrapper.Cleanup();

    //Runner * GameRunner = new Runner(new QDir(this->PackagePath), this->MANIFESTJSON, this->GlobalConfigJSON, this->SubGame);

    //std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Executing pre-run cleanup." << std::endl;
    //GameRunner->Cleanup();
    //std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Building runtime." << std::endl;
    //GameRunner->BuildRuntime();
    //std::cout << QTime::currentTime().toString().toStdString() << "[OUT] MainWindow:" << "Running game" << (*this->MANIFESTJSON)["SUBGAMES"][this->SubGame - 1]["TITLE"] << std::endl;
    //GameRunner->Run();
    //QMessageBox::warning(nullptr, "Ready for cleanup...", "Press OK to start cleanup");
    //GameRunner->Cleanup();

    //delete NewContainerWrapper;
    //delete MANIFESTJSON;
}

void LibraryGameCard::on_GameCard_clicked()
{
    //qDebug() << "CLICKED GAME CARD" << this->GameTitle;
}

void LibraryGameCard::resizeEvent(QResizeEvent * event) {
    QWidget::resizeEvent(event);
    this->CoverLabel->setFixedHeight(this->CoverLabel->width() * 3 / 2);
    //this->ButtonsGroupBox->setAlignment(Qt::AlignBottom);
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

void LibraryGameCard::enterEvent(QEnterEvent * event)
{
    QWidget::enterEvent(event);
    //this->ButtonsGroupBox->setVisible(true);

    //this->CoverLabel->setFixedHeight(this->CoverLabel->width() * 3 / 2);
}

void LibraryGameCard::leaveEvent(QEvent * event)
{
    QWidget::leaveEvent(event);
    //this->ButtonsGroupBox->setVisible(false);

    //this->CoverLabel->setFixedHeight(this->CoverLabel->width() * 3 / 2);
}
