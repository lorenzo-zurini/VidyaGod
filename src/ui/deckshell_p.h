#ifndef DECKSHELL_P_H
#define DECKSHELL_P_H

// The controller UI's screens (DeckShell's parts): each a view of the shell's models, each driven by the controller
// through PadTarget and read back through its padState (`controller state`).

#include "deckshell.h"
#include "libraryview.h"   // LibraryGameCard, SortMode

#include <QHash>
#include <QPixmap>
#include <QPointer>
class QTimer;
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QWidget>

class DeckGrid;

// Home: the game played last, large, with Play one press away; under it what was played recently and what peers
// have to download.
class DeckHome : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    explicit DeckHome(DeckShell &Shell);
    void refresh();   // the shelves were rebuilt: pick the hero and fill the strips again
    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;
    QString statusText() const;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;

private:
    void setArea(int A);
    void loadBackdrop();
    QRectF heroRect() const;
    QRectF buttonRect(int I) const;
    QStringList buttons() const;
    void press(int I);

    DeckShell &Shell;
    DeckGrid *Recent = nullptr, *Fresh = nullptr;
    QString HeroKey;             // the hero's game ("" when the Library is empty)
    QString HeroTitle, HeroLine;
    bool HeroPlayed = false;     // the hero was ever played: "Continue playing", else "In your library"
    QPixmap HeroCover, Backdrop;
    int Area = 0;                // 0 the hero's buttons, 1 the first strip, 2 the second
    int Button = 0;
};

// A shelf of covers: the Library (installed games) or the Catalog (games to download), sorted (Y), searched (X).
class DeckShelf : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    enum class Kind { Library, Catalog };
    DeckShelf(DeckShell &Shell, Kind K);
    ~DeckShelf() override;
    Kind kind() const { return K; }
    // The catalog changed: new cards (focus stays on the same game). Returns the old ones, for the caller to delete
    // once nothing shows them: Home shows the shelves' own cards.
    [[nodiscard]] QList<LibraryGameCard *> rebuild();
    void refreshCovers();                // covers landed: the cards still without one look again
    const QList<LibraryGameCard *> &cards() const { return Pool; }
    LibraryGameCard *find(const QString &Key) const;
    DeckGrid *grid() const { return Grid; }
    void setSearch(const QString &Text);
    QString search() const { return Search; }
    void setSort(SortMode M);
    SortMode sort() const { return Sort; }
    void cycleSort();
    void setDownload(const QString &Key, bool On, double Percent);   // a download's state, onto its cards
    QString statusText() const;

    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;

signals:
    void sizesCounted();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void apply();                        // sort, search and sections, onto the grid
    void countSizes();                   // what each package holds on disk and when it was played (off the GUI thread)

    DeckShell &Shell;
    Kind K;
    DeckGrid *Grid = nullptr;
    QList<LibraryGameCard *> Pool;       // owned
    QList<LibraryGameCard *> Shown;
    QString Search;
    SortMode Sort = SortMode::Name;
    QHash<QString, double> Active;       // downloads in flight: GameKey → percent (-1 unknown)
    qint64 TotalBytes = -1;
    bool Counting = false, CountAgain = false;
};

// One game: its cover and facts, and what can be done with it — Play, its launch options, golden; or Download /
// Cancel; and the package's other games.
class DeckGamePage : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    explicit DeckGamePage(DeckShell &Shell);
    void showGame(const QString &Key);   // this game's page
    void refresh();                  // the shelves were rebuilt, or a download moved: read the game again
    QString key() const { return Key; }
    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;
    QStringList buttons() const;     // the buttons shown, in order
    void press(const QString &Button);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;

private:
    void loadCover();
    QRectF coverRect() const;
    QRectF buttonRect(int I) const;

    DeckShell &Shell;
    DeckGrid *Others = nullptr;      // the package's other games (Catalog)
    QString Key, LoadedCoverFor;
    QString Title, Line1, Line2, Status;
    QStringList Chips, Versions;
    QPixmap Cover, Backdrop;
    bool Installed = false, Golden = false, Downloading = false;
    double Percent = -1;
    int Area = 0, Button = 0;
    QString Confirm;                 // a button pressed once that asks for a second press
};

// Downloads: what is coming down, each with its progress and speed; X cancels, Y puts it first.
class DeckDownloads : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    explicit DeckDownloads(DeckShell &Shell);
    void refresh();
    void setProgress(const QString &Key, double Percent);
    QStringList keys() const { return Keys; }
    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;
    QString statusText() const;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;

private:
    struct Row { QString Title, Line; double Percent = -1; bool Bad = false; };
    Row row(const QString &Key) const;
    QRectF rowRect(int I) const;

    DeckShell &Shell;
    QStringList Keys;
    QHash<QString, double> Progress;
    int Current = 0;
    double Scroll = 0;
    QString Firsted;                 // the key just put first (Y): its line says so for a moment
    QTimer *FirstedTimer = nullptr;
    QString Confirm;                 // the key whose cancel waits for a second X
};

// A desktop page (Peers, the node, Settings) made large for the couch: its own widgets, driven by PadNav.
class DeckHosted : public QWidget
{
    Q_OBJECT
public:
    DeckHosted(QWidget *Page, double Scale, QWidget *Parent);
    QWidget *page() const { return Page; }
    void setScale(double S);

private:
    QWidget *Page = nullptr;
};

// The Start menu: desktop mode, networking, a library check, quit.
class DeckMenu : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    explicit DeckMenu(DeckShell &Shell);
    void present();    // shown over the shell, its first item current
    void dismiss();
    QStringList items() const;
    void choose(const QString &Item);
    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;

private:
    QRectF itemRect(int I) const;
    DeckShell &Shell;
    int Current = 0;
    bool QuitArmed = false;   // Quit was chosen once: the next A on it quits
};

#endif // DECKSHELL_P_H
