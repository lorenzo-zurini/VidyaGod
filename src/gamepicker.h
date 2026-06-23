#ifndef GAMEPICKER_H
#define GAMEPICKER_H

#include <QWidget>
#include <QList>

#include "nlohmann/json.hpp"
#include "manifestmodel.h"   // NodeIndex

class LibraryView;
class LibraryGameCard;
class QTimer;

// ---------------------------------------------------------------------------
// GamePicker — the in-package MULTI-GAME picker: a bare LibraryView card grid over ONE bundle's PresentableGroups
// (reusing the exact tile/cover/click→PreLaunchWindow machinery the Library tab uses). Shown only when a package
// dir contains more than one game; clicking a card opens that game's PreLaunchWindow (LibraryGameCard::play()).
// Borrows the config + the package NodeIndex (kept alive by main()'s in-package branch).
// ---------------------------------------------------------------------------
class GamePicker : public QWidget
{
    Q_OBJECT
public:
    GamePicker(nlohmann::ordered_json * config, const NodeIndex * index, QWidget * parent = nullptr);
    ~GamePicker() override;

private:
    void buildCards();

    nlohmann::ordered_json *   Config = nullptr;
    const NodeIndex *          Index  = nullptr;
    LibraryView *              View   = nullptr;
    QList<LibraryGameCard *>   Cards;             // owned
    QTimer *                   CoverRefresh = nullptr;
};

#endif // GAMEPICKER_H
