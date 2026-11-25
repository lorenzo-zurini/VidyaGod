#ifndef DECKLIST_H
#define DECKLIST_H

#include "padnav.h"   // PadTarget

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

class DeckShell;

//One row of a controller list page. What a press does depends on its kind:
//  Header  — a group's title (not stopped on);
//  Info    — a line to read (not stopped on);
//  Toggle  — A, or Left / Right, turns it off or on;
//  Choice  — Left / Right (and A) step through its values;
//  Action  — A does it; with Confirm, the first A asks ("Press A again to …"), the second does it;
//  Qr      — a code to scan, with its caption (not stopped on).
//Says: what it does, in one sentence under its title.
struct DeckRow
{
    enum class Kind { Header, Info, Toggle, Choice, Action, Qr };
    Kind K = Kind::Action;
    QString Id, Title, Says;
    bool On = false, Enabled = true, Confirm = false;
    QStringList Values;
    int Value = 0;
    QString Code;                                  // Qr: the text it encodes
    std::function<void(bool)> Toggle;
    std::function<void(int)> Choose;
    std::function<void()> Press;
    static DeckRow Header(const QString &Title) { DeckRow R; R.K = Kind::Header; R.Title = Title; return R; }
    static DeckRow Info(const QString &Id, const QString &Title, const QString &Says = {})
    { DeckRow R; R.K = Kind::Info; R.Id = Id; R.Title = Title; R.Says = Says; return R; }
    bool stops() const { return K == Kind::Toggle || K == Kind::Choice || K == Kind::Action; }
};

//A controller page of rows (DeckRow), built from its models by Build and built again by refresh(): Settings and Peers
//in controller mode, every row a toggle, a left/right value or one press, big enough for the couch, the one the
//controller is on lit. It keeps its place (by row id) across rebuilds.
class DeckList : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    DeckList(DeckShell &Shell, const QString &Name, std::function<QList<DeckRow>()> Build);
    void refresh();
    void setNote(const QString &Text);          // a line over the rows: what the last press did ("" clears it)
    QString note() const { return Note; }
    const QList<DeckRow> &rows() const { return Rows; }
    QString current() const;                    // the id of the row the controller is on
    bool setCurrent(const QString &Id);
    bool press(const QString &Id);              // A on that row (tests and the CLI)

    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *E) override;
    void showEvent(QShowEvent *E) override;

private:
    double rowHeight(const DeckRow &R) const;
    QRectF rowRect(int I) const;
    void act(int I, int Step);                  // A (Step 0), Left (-1), Right (+1)
    void keepInSight();
    DeckShell &Shell;
    std::function<QList<DeckRow>()> Build;
    QList<DeckRow> Rows;
    int Current = -1;
    double Scroll = 0;
    QString Note, Armed;                        // Armed: the Confirm row that waits for its second A
};

#endif // DECKLIST_H
