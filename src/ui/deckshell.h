#ifndef DECKSHELL_H
#define DECKSHELL_H

#include "padnav.h"   // PadTarget

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <optional>

#include "nlohmann/json.hpp"

class AppModel;
class DownloadManager;
class IpfsModel;
class LibraryGameCard;
class QStackedWidget;
class QTimer;

class DeckHome;
class DeckShelf;
class DeckGamePage;
class DeckDownloads;
class DeckHosted;
class DeckMenu;

// ---------------------------------------------------------------------------
// ControllerMode — the settings of the controller UI (Settings.Controller) and whether the app starts in it.
// ---------------------------------------------------------------------------
namespace ControllerMode
{
enum class Start { Auto, Controller, Desktop };
// Settings.Controller.Start: "auto" (default: controller mode inside Steam's Game Mode), "controller" or "desktop".
Start StartSetting(const nlohmann::ordered_json &Config);
void SetStart(nlohmann::ordered_json &Config, Start S);
std::string StartName(Start S);
std::optional<Start> StartFrom(const std::string &Name);
// Settings.Controller.Desktop (default on): a controller drives the desktop window too.
bool DesktopNavigation(const nlohmann::ordered_json &Config);
void SetDesktopNavigation(nlohmann::ordered_json &Config, bool On);
// Settings.Controller.Haptics (default off): a short rumble as focus moves.
bool Haptics(const nlohmann::ordered_json &Config);
void SetHaptics(nlohmann::ordered_json &Config, bool On);
// Running inside Steam's Game Mode (a gamescope session: the Steam Deck's own, or a TV's Big Picture session).
bool InGameMode();
// Whether this start comes up in controller mode: --controller / --desktop first, then the setting.
bool StartsInController(const nlohmann::ordered_json &Config, bool FlagController, bool FlagDesktop);
}

// ---------------------------------------------------------------------------
// DeckShell — the controller UI: a full-screen shell for a Steam Deck or a TV, every part of the app behind a few
// buttons. The shoulders turn its sections (Home, Library, Catalog, Downloads, Friends, Node, Settings); a cover's
// page plays, configures, downloads or marks it; Start opens the menu (desktop mode, networking, quit). Friends, Node
// and Settings are the desktop pages themselves, made large and driven by PadNav, so nothing the app does is out of
// a controller's reach. It is a view of the same models the desktop tabs show.
// ---------------------------------------------------------------------------
class DeckShell : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    enum class Section { Home, Library, Catalog, Downloads, Friends, Node, Settings };
    static constexpr int SectionCount = 7;

    DeckShell(AppModel &Model, IpfsModel &Ipfs, DownloadManager &Downloads, QWidget *Parent = nullptr);
    ~DeckShell() override;

    static QString SectionName(Section S);                       // "Home", "Library", …
    static std::optional<Section> SectionFrom(const QString &Name);   // case-insensitive
    void showSection(Section S);
    Section section() const { return Current; }

    // The page of one game (its GameKey), from the Library or the Catalog; false when no shelf shows it.
    bool openGame(const QString &Key);
    void closeGame();
    bool gameOpen() const;
    void openMenu();
    void closeMenu();
    bool menuOpen() const;

    // The card of a game shown on a shelf (the Library's first, then the Catalog's), or null. Valid until the next
    // catalog rebuild.
    LibraryGameCard *card(const QString &Key) const;
    bool installed(const QString &Key) const;   // it is on the Library shelf
    // Every card of each shelf (the Catalog's secondaries among them), whatever its search: what Home draws from.
    const QList<LibraryGameCard *> &libraryCards() const;
    const QList<LibraryGameCard *> &catalogCards() const;

    AppModel &model() const { return Model; }
    IpfsModel &ipfs() const { return Ipfs; }
    DownloadManager &downloads() const { return Downloads; }

    // Controller mode on or off: on, the shell is what the window shows, controllers are read, and every dialog the
    // app opens takes the controller mode's size (the launch window full screen).
    void setActive(bool On);
    bool active() const { return Active; }
    static bool On();   // controller mode is on in this process

    // The status line under the screen (what the section shows: "297 games · 694 GB").
    QString statusText() const;
    double scale() const;

    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;

signals:
    void desktopRequested();   // the menu's "Desktop mode"
    void quitRequested();      // the menu's "Quit"

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;
    bool eventFilter(QObject *Watched, QEvent *Event) override;

private:
    void rebuildAll();             // the catalog changed: every shelf, then what shows their cards
    void focusSection();           // focus lands where the section's controls start
    void place();                  // the body, the game page and the menu between the header and the footer
    QRectF tabRect(int I) const;   // a section's title in the header
    void styleWindow(QWidget *W);  // a dialog opened in controller mode: large, and the launch window full screen
    QString battery() const;

    AppModel &Model;
    IpfsModel &Ipfs;
    DownloadManager &Downloads;
    Section Current = Section::Home;
    bool Active = false;
    bool NativeDialogsWere = false;   // AA_DontUseNativeDialogs before controller mode turned it on
    QStackedWidget *Body = nullptr;
    DeckHome *Home = nullptr;
    DeckShelf *Library = nullptr, *Catalog = nullptr;
    DeckDownloads *DownloadsPage = nullptr;
    DeckHosted *Friends = nullptr, *Node = nullptr, *Settings = nullptr;
    DeckGamePage *Game = nullptr;
    DeckMenu *Menu = nullptr;
    QTimer *Clock = nullptr;
    mutable QString BatteryText;
    mutable qint64 BatteryAt = 0;
};

#endif // DECKSHELL_H
