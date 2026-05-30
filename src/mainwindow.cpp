#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "commonutils.h"

#include "packageeditor.h"
#include "jsonoperations.h"
#include "filesystemoperations.h"
#include "prelaunchwindow.h"

//TO-DO: ADD VARIABLE SUBSTITUTION WITH ENV VARS AND AUTOCALC
//TO-DO: FIX EXE COMMAND LINE PARSING AND WORKDIR
//IMPLEMENT WINEDLLOVERRIDES
//IMPLEMENT MIRRORFS COMPONENTS
//ADD MORE RUNNERS BASED ON PLATFORM

//Constructs the main window, wires up the UI file, and runs all three build phases
//(static skeleton, game card population, dynamic layout) so the window is fully
//populated before show() is called by main().
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

//Creates the fixed structural widgets that exist for the lifetime of the window:
//the tab widget, Library tab with its scroll area, and Packages tab with its toolbar.
//Dynamic content (game cards, package rows) is added separately in the Build*DynamicUI methods.
void MainWindow::BuildStaticUI()
{
    MainWindowTabWidget = new QTabWidget(ui->MainWidget);
    ui->MainWidget->layout()->addWidget(MainWindowTabWidget);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    //Library tab: a scroll area that will hold the game card grid.
    //Zero margins so the cards fill the full tab area.
    LibraryTabWidget = new QWidget(MainWindowTabWidget);
    LibraryTabWidgetLayout = new QVBoxLayout(LibraryTabWidget);
    LibraryTabWidgetLayout->setContentsMargins(0, 0, 0, 0);
    LibraryTabWidget->setLayout(LibraryTabWidgetLayout);

    MainWindowTabWidget->addTab(LibraryTabWidget, "Library");

    LibraryScrollArea = new QScrollArea(LibraryTabWidget);
    LibraryScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    LibraryScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); //Cards reflow to fit width; horizontal scroll is never needed
    LibraryTabWidgetLayout->addWidget(LibraryScrollArea);
    LibraryScrollArea->setWidgetResizable(1);

    //Grid size slider — disabled for now, hardcoded to 4 columns in BuildLibraryDynamicUI.
    //QSlider * GridSizeSlider = new QSlider(Qt::Horizontal, LibraryTabWidget);
    //GridSizeSlider->setValue((*GlobalConfigJSON)["Settings"]["LibraryGridSize"]);
    //GridSizeSlider->setMinimum(3);
    //GridSizeSlider->setMaximum(7);
    //GridSizeSlider->setSingleStep(1);
    //LibraryTabWidgetLayout->addWidget(GridSizeSlider);
    //QObject::connect(GridSizeSlider, &QSlider::sliderReleased, this, &MainWindow::MainWindowGridSizeChanged);

    ///////////////////////////////////////////////////////////////////////////////////////////////
    //Packages tab: toolbar (add / open editor buttons) above a scroll area for the package list.
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
    //Lambda opens a PackageEditor dialog without blocking the main window.
    QObject::connect(OpenPackageEditorButton, &QPushButton::clicked, this, [this]{PackageEditor * NewPackageEditor = new PackageEditor(this->GlobalConfigJSON, this); NewPackageEditor->show();});

    PackagesScrollArea = new QScrollArea(PackagesTabWidget);
    PackagesScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    PackagesScrollArea->setWidgetResizable(1);
    PackagesTabWidgetLayout->addWidget(PackagesScrollArea);


    ///////////////////////////////////////////////////////////////////////////////////////////////
}

//Iterates GlobalConfigJSON["LIBRARY"], loads each package's MANIFEST.json, and creates
//one LibraryGameCard per SUBGAMES entry. Skips packages whose MANIFEST cannot be parsed.
//Must be called before BuildLibraryDynamicUI so LibraryGameCards is populated.
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

        //One card per subgame — a multi-game package produces multiple cards in the grid.
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

//Tears down the old LibraryWidget (if any) and rebuilds the game card grid.
//Cards are reparented to nullptr before the old widget is deleted so they are not
//destroyed prematurely — they are then re-parented into the new LibraryWidget.
//Currently hardcoded to 4 columns; the slider-based GridSize path is disabled.
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
    LibraryWidgetLayout->setAlignment(Qt::AlignTop); //Cards stick to the top; empty rows don't expand upward
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
        //Row = i / GridSize, column = i % GridSize — fills left-to-right, top-to-bottom.
        LibraryWidgetLayout->addWidget(LibraryGameCards->at(i), i / GridSize, i % GridSize);
    }

    //Equal stretch on every column so cards share available width uniformly.
    for (int column = 0; column < GridSize; ++column)
    {
        LibraryWidgetLayout->setColumnStretch(column, 1);
    }

    //Add a stretching empty row after the last card row so cards don't stretch vertically.
    LibraryWidgetLayout->setRowStretch((LibraryGameCards->count() + GridSize - 1) / GridSize, 1);
}

//Rebuilds the Packages tab content from GlobalConfigJSON["LIBRARY"].
//Each row shows package name, UID, version, and a Remove button.
//The Remove button erases the entry by index and immediately calls RebuildDynamicUI
//to keep the view consistent, then persists the change to disk.
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
        //Capture i by value so each lambda removes the correct entry even after the loop ends.
        QObject::connect(RemovePackageButton, &QPushButton::clicked, this, [this, i]{(*GlobalConfigJSON)["LIBRARY"].erase(i); this->RebuildDynamicUI(); this->SaveGlobalConfigJSON();});
        PackagesWidgetLayout->addWidget(RemovePackageButton, i, 3);
    }

    //Stretching row at the end prevents the last real row from expanding to fill the scroll area.
    PackagesWidgetLayout->setRowStretch(PackagesWidgetLayout->rowCount(), 1);
}

//Rebuilds the complete dynamic UI (game cards + library grid + packages list).
//Called after any library mutation (add / remove package).
void MainWindow::RebuildDynamicUI()
{
    this->BuildLibraryGameCards();
    this->BuildLibraryDynamicUI();
    this->BuildPackagesDynamicUI();
    return;
}

//Opens a native directory picker, validates the selection as a VidyaGod package,
//guards against duplicate entries by PACKAGEUID, then appends a slim record to
//GlobalConfigJSON["LIBRARY"] and saves to disk before rebuilding the UI.
void MainWindow::on_AddGameButton_clicked()
{
    QString SelectedPath = QFileDialog::getExistingDirectory(this, "Select package or directory...");
    if (SelectedPath.isEmpty()) return;

    //Collect all valid package directories: the selected dir itself and/or any direct or
    //nested subdirectories that contain a METADATA/MANIFEST.json.
    QStringList PackagePaths;
    std::function<void(const QString &)> ScanDir = [&](const QString &DirPath)
    {
        QDir Dir(DirPath);
        if (FSOps::CheckPackageValid(&Dir))
        {
            PackagePaths.append(DirPath);
            return; // Don't recurse into a package directory itself
        }
        // Not a package — recurse into immediate subdirectories
        for (const QString &SubDir : Dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            ScanDir(QDir::cleanPath(DirPath + QDir::separator() + SubDir));
    };
    ScanDir(SelectedPath);

    if (PackagePaths.isEmpty())
    {
        QMessageBox::warning(this, "No packages found",
            "The selected directory does not contain any valid packages (no METADATA subdirectory found).");
        return;
    }

    //Add each discovered package, skipping duplicates by PACKAGEUID.
    int Added = 0, Skipped = 0;
    for (const QString &Path : PackagePaths)
    {
        nlohmann::ordered_json MANIFESTJSON;
        QFile ManifestFile(QDir::cleanPath(Path + "/METADATA/MANIFEST.json"));
        if (JSONOps::LoadJSON(&ManifestFile, &MANIFESTJSON))
        {
            LogErr("MainWindow", "Could not parse MANIFEST for " + Path.toStdString() + ", skipping.");
            Skipped++;
            continue;
        }

        //Duplicate check by PACKAGEUID
        bool Duplicate = false;
        for (auto &Entry : (*GlobalConfigJSON)["LIBRARY"])
        {
            if (Entry["PACKAGEUID"] == MANIFESTJSON["PACKAGEUID"])
            {
                LogOut("MainWindow", "Package " + std::string(MANIFESTJSON["PACKAGENAME"]) + " already in library, skipping.");
                Duplicate = true;
                Skipped++;
                break;
            }
        }
        if (Duplicate) continue;

        nlohmann::ordered_json SlimEntry;
        SlimEntry["PACKAGEUID"]     = MANIFESTJSON["PACKAGEUID"];
        SlimEntry["PACKAGENAME"]    = MANIFESTJSON["PACKAGENAME"];
        SlimEntry["PACKAGEVERSION"] = MANIFESTJSON["PACKAGEVERSION"];
        SlimEntry["PATH"]           = Path.toStdString();
        (*GlobalConfigJSON)["LIBRARY"].push_back(SlimEntry);
        LogSucc("MainWindow", "Added package: " + std::string(MANIFESTJSON["PACKAGENAME"]));
        Added++;
    }

    if (Added > 0)
    {
        MainWindow::SaveGlobalConfigJSON();
        this->RebuildDynamicUI();
    }

    QMessageBox::information(this, "Done",
        QString("Added %1 package(s). %2 skipped (already in library or invalid).").arg(Added).arg(Skipped));
}

//Serializes GlobalConfigJSON to GlobalConfig.JSON in AppDataDir.
bool MainWindow::SaveGlobalConfigJSON()
{
    return JSONOps::SaveJSON(MainWindow::GlobalConfigJSON, new QFile(AppDataDir->filePath("GlobalConfig.JSON")));
}

//Reads the new grid size from the slider that emitted the signal, persists it to
//GlobalConfigJSON, saves to disk, and redraws the library grid at the new column count.
void MainWindow::MainWindowGridSizeChanged()
{
    QSlider * Slider = qobject_cast<QSlider *>(QObject::sender());
    LogOut("MainWindow", "Set GridSize " + std::to_string(Slider->value()));
    (*GlobalConfigJSON)["Settings"]["LibraryGridSize"] = Slider->value();
    MainWindow::SaveGlobalConfigJSON();
    BuildLibraryDynamicUI();
}

//Constructs the card widget with a cover label and Play/"..." button row.
//Cover and title are NOT loaded here — call InitializeClassVariables() afterwards
//so the widget tree exists before any file I/O or subgame index lookups occur.
LibraryGameCard::LibraryGameCard(nlohmann::ordered_json * PassedGlogalConfigJSON, int PassedGame, std::string PassedSubgameID, QWidget * parent)
    : GlobalConfigJSON(PassedGlogalConfigJSON), Game(PassedGame), SubgameID(PassedSubgameID), QWidget(parent)
{
    LibraryGameCardLayout = new QVBoxLayout(this);
    this->setLayout(LibraryGameCardLayout);
    LibraryGameCardLayout->setSpacing(0);
    LibraryGameCardLayout->setContentsMargins(0, 0, 0, 0);

    //Cover label fills all available space; aspect ratio is enforced in resizeEvent.
    CoverLabel = new QLabel(this);
    CoverLabel->setAlignment(Qt::AlignCenter);
    CoverLabel->setScaledContents(true);
    CoverLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); //Ignored so the label shrinks below its pixmap's natural size
    LibraryGameCardLayout->addWidget(CoverLabel);

    //Fixed-height button row beneath the cover.
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

//Loads PackagePath from the library entry, reads MANIFEST.json, and sets the cover
//pixmap and game title for the resolved SubgameID.
//Safe to call only after the card widget has been fully constructed.
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
    //COVER lives under METADATA since v2 of the manifest schema; fall back to flat for old manifests.
    auto &SubgameMeta = (*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["METADATA"];
    std::string CoverFile = (SubgameMeta.is_object() && SubgameMeta.contains("COVER") && SubgameMeta["COVER"].is_string())
                            ? std::string(SubgameMeta["COVER"])
                            : ((*MANIFESTJSON)["SUBGAMES"][SubgameIdx].contains("COVER") ? std::string((*MANIFESTJSON)["SUBGAMES"][SubgameIdx]["COVER"]) : "");
    if (!CoverFile.empty())
        this->CoverLabel->setPixmap(QPixmap(QDir::cleanPath(QString::fromStdString(this->PackagePath.string()) + QDir::separator() + "METADATA" + QDir::separator() + QString::fromStdString(CoverFile))));
}

//Opens the PreLaunchWindow for this card's subgame.
//If the user has previously checked "Remember & skip" and is NOT holding Shift,
//the dialog is bypassed and the game launches directly with the saved settings.
void LibraryGameCard::on_PlayGameButton_clicked()
{
    int SubgameIdx = ContainerWrapper::FindSubgameIndex(*MANIFESTJSON, this->SubgameID);
    LogOut("MainWindow", "Play clicked for " + (SubgameIdx != -1
        ? std::string((*this->MANIFESTJSON)["SUBGAMES"][SubgameIdx]["TITLE"])
        : this->SubgameID));

    // Retrieve PackageUID for USERSETTINGS lookups.
    std::string PackageUID;
    if (this->MANIFESTJSON->contains("PACKAGEUID") && !(*this->MANIFESTJSON)["PACKAGEUID"].is_null())
        PackageUID = std::string((*this->MANIFESTJSON)["PACKAGEUID"]);

    // Check whether the user has opted to skip the dialog for this package.
    bool SkipDialog = false;
    if (!PackageUID.empty())
    {
        auto US = ContainerWrapper::GetPackageUserSettings(*GlobalConfigJSON, PackageUID);
        if (US.contains("SKIP_LAUNCH_DIALOG") && US["SKIP_LAUNCH_DIALOG"].is_boolean())
            SkipDialog = bool(US["SKIP_LAUNCH_DIALOG"]);
    }

    // Holding Shift forces the dialog even when "skip" is set.
    bool ShiftHeld = (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;

    if (!SkipDialog || ShiftHeld)
    {
        // Show PreLaunchWindow as a non-modal dialog.
        PreLaunchWindow * Dialog = new PreLaunchWindow(
            this->GlobalConfigJSON,
            this->MANIFESTJSON,
            this->PackagePath.string(),
            this->SubgameID,
            nullptr);
        Dialog->show();
        return;
    }

    // ----------------------------------------------------------------
    // Skip-dialog fast path: show the window but auto-click Launch so
    // the user can still see progress and kill if needed.
    // ----------------------------------------------------------------
    PreLaunchWindow * Dialog = new PreLaunchWindow(
        this->GlobalConfigJSON,
        this->MANIFESTJSON,
        this->PackagePath.string(),
        this->SubgameID,
        nullptr);
    Dialog->show();
    // Auto-click Launch after the event loop processes the show event.
    QMetaObject::invokeMethod(Dialog, "onLaunchClicked", Qt::QueuedConnection);
}


void LibraryGameCard::on_GameCard_clicked()
{
    //qDebug() << "CLICKED GAME CARD" << this->GameTitle;
}

//Enforces a 2:3 cover aspect ratio (portrait) by fixing the label height to width * 3/2
//whenever the card is resized. This keeps cover art proportional at any grid column width.
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
