#include "gamepicker.h"
#include "libraryview.h"     // LibraryView + LibraryGameCard
#include "packagecatalog.h"  // PresentableGroups
#include "covercache.h"      // lazy cover refresh
#include "commonutils.h"     // Log*

#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QIcon>

#include <nlohmann/json.hpp>

GamePicker::GamePicker(nlohmann::ordered_json * config, const NodeIndex * index, QWidget * parent)
    : QWidget(parent), Config(config), Index(index)
{
    setWindowTitle("VidyaGod — choose a game");
    setWindowIcon(QIcon(":/vidyagod.png"));
    resize(900, 650);

    QVBoxLayout * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QLabel * heading = new QLabel("  This package contains several games — pick one to launch:", this);
    heading->setStyleSheet("padding:8px;color:#c7d0d8;");
    layout->addWidget(heading);

    View = new LibraryView(this);   // default mode: a card click calls play() (opens the game's PreLaunchWindow)
    layout->addWidget(View, 1);

    buildCards();

    // Covers load lazily (CoverCache) — coalesce a burst of arrivals into one re-scale/repaint.
    CoverRefresh = new QTimer(this);
    CoverRefresh->setSingleShot(true);
    CoverRefresh->setInterval(200);
    connect(CoverRefresh, &QTimer::timeout, this, [this]{ View->refreshVisuals(); });
    connect(CoverCache::instance(), &CoverCache::coverReady, this, [this](const QString &){
        if (CoverRefresh && !CoverRefresh->isActive()) CoverRefresh->start();
    });
}

GamePicker::~GamePicker()
{
    qDeleteAll(Cards);
}

void GamePicker::buildCards()
{
    qDeleteAll(Cards);
    Cards.clear();

    // One card per presentable game group in this bundle.
    for (const std::vector<const Node *> & Group : PackageCatalog::PresentableGroups(*Index))
    {
        std::vector<std::string> Ids;
        Ids.reserve(Group.size());
        for (const Node * N : Group) Ids.push_back(N->NodeId);
        if (Ids.empty()) continue;
        auto * c = new LibraryGameCard(Config, Index, std::move(Ids));
        c->InitializeClassVariables();
        Cards.append(c);
    }
    View->setCards(&Cards);
    LogOut("GamePicker", "Multi-game package — " + std::to_string(Cards.size()) + " game card(s).");

    int CardW = 185;
    if (Config && Config->contains("Settings") && (*Config)["Settings"].contains("CardPixelWidth")
        && (*Config)["Settings"]["CardPixelWidth"].is_number_integer())
        CardW = int((*Config)["Settings"]["CardPixelWidth"]);
    View->prescaleCovers(CardW);
    View->layoutCards(CardW);
}
