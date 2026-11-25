// The controller UI: the pure rules a controller is read by (Pad), generic navigation over any window (PadNav,
// PadRouter), the on-screen keyboard, and the controller shell (DeckShell) over a small real library.

#include <QtTest/QtTest>
#include <QScrollArea>
#include <set>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "appmodel.h"
#include "command.h"
#include "commandserver.h"
#include "deckgrid.h"
#include "deckshell.h"
#include "cardpool.h"
#include "downloaddialog.h"
#include "librarytab.h"
#include "deckstyle.h"
#include "prelaunchwindow.h"
#include "layouteditor.h"
#include "manifestmodel.h"
#include "generalpage.h"
#include "settingstab.h"
#include "theme.h"
#include "downloadmanager.h"
#include "instancestore.h"
#include "keycapture.h"
#include <cstring>
#include <malloc.h>   // mallopt(M_PERTURB): a read of freed memory crashes
#include <atomic>
#include <thread>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include "ipfsmodel.h"
#include "libraryview.h"
#include "osk.h"
#include "packagecatalog.h"
#include "padinput.h"
#include <SDL.h>
#include "padnav.h"
#include "nodefixture.h"
#include "decklist.h"
#include "networktab.h"
#include "peersharing.h"
#include "qrcode.h"
#include "ipfswrapper.h"
#include <QImage>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>

#include <filesystem>
#include <fstream>

using json = nlohmann::ordered_json;
using B = Pad::Button;

namespace
{
int ScreenSaverSeconds(const json &Cfg)
{
    return Cfg.contains("Settings") && Cfg["Settings"].contains("ScreenSaverSeconds") ? Cfg["Settings"]["ScreenSaverSeconds"].get<int>() : 60;
}

//A press as a controller makes it (PadInput's delivery), then the UI's queued work.
void Press(B Btn, int N = 1)
{
    for (int I = 0; I < N; ++I)
    {
        PadInput::instance().inject(Btn);
        QApplication::processEvents();
    }
}

//A window shown and made the one presses go to.
void Front(QWidget &W)
{
    W.show();
    W.activateWindow();
    QApplication::processEvents();
    (void)QTest::qWaitForWindowActive(&W, 500);
}

//A window that counts the presses its PadTarget is offered (it takes none).
class Counting : public QWidget, public PadTarget
{
public:
    int Offered = 0;
    bool padPressed(Pad::Button) override { ++Offered; return false; }
};

//A small library: Installed games with their zip here (the Library), and Remote ones naming a CID with none here (the
//Catalog).
struct LibraryFixture
{
    QTemporaryDir Dir;
    QDir AppDir{Dir.path()};
    json Cfg;
    struct Game { std::string Uid, Title, Date, Series; bool Here; };
    std::vector<Game> Games{
        {"1", "Alpha", "2001-01-01", "Saga", true}, {"2", "Bravo", "1999-05-05", "", true}, {"3", "Charlie", "2005-03-03", "Saga", true},
        {"4", "Delta", "1995-02-02", "", true}, {"5", "Echo", "2010-10-10", "", true}, {"6", "Foxtrot", "2003-06-06", "", true},
        {"7", "Golf", "1990-01-01", "", true}, {"8", "Hotel", "2012-12-12", "", true},
        {"9", "India", "2000-01-01", "", false}, {"10", "Juliet", "2002-01-01", "", false}, {"11", "Kilo", "2004-01-01", "", false}};
    // CoverUid: that game's tile has a cover, a 600x900 cover.png in its package.
    explicit LibraryFixture(const std::string &CoverUid = "")
    {
        const std::string Root = (Dir.path() + "/LIBRARY").toStdString();
        Cfg = json{{"Settings", {{"Paths", {{"LibraryRoot", Root}, {"UserDataRoot", (Dir.path() + "/USERDATA").toStdString()}}}}},
                   {"LIBRARY", json::array()}};
        for (const Game &G : Games)
        {
            const std::string Pkg = Root + "/VidyaGod/[" + G.Uid + "] " + G.Title;
            std::filesystem::create_directories(Pkg);
            json Meta{{"RELEASEDATE", G.Date}, {"DEVELOPER", "Studio " + G.Title}};
            if (!G.Series.empty()) Meta["SERIES"] = G.Series;
            json T = NodeFixture::Tile(G.Uid, G.Title, Meta);
            //The card reads SERIES and RELEASEDATE off the tile itself too (the sort keys).
            T["LAYERS"][0]["EXEC"][0]["TILE"]["RELEASEDATE"] = G.Date;
            if (!G.Series.empty()) T["LAYERS"][0]["EXEC"][0]["TILE"]["SERIES"] = G.Series;
            if (G.Uid == CoverUid)
            {
                T["LAYERS"][0]["EXEC"][0]["TILE"]["COVER"] = {{"FILE", "cover.png"}};
                QImage Art(600, 900, QImage::Format_RGB32);
                Art.fill(Qt::darkCyan);
                Art.save(QString::fromStdString(Pkg + "/cover.png"));
            }
            NodeFixture::WriteNodes(Pkg + "/game.json",
                NodeFixture::Chain("g" + G.Uid, {T, NodeFixture::Exec("win32", "g.exe"),
                                                 NodeFixture::ContentCid("zip", "game.zip", "bafkreigame" + G.Uid)}, {}, {{"VARIANT", "Edition"}}));
            if (G.Here) std::ofstream(Pkg + "/game.zip") << "content";
        }
    }
};
}

class ControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { PadRouter::instance().install(); }

    // Every button has one name that reads back as it; names are read without case; there is no "select". Teeth: a
    // name table out of step with the enum (a button reads back as another).
    void button_names_round_trip()
    {
        QCOMPARE(static_cast<int>(Pad::Names().size()), Pad::ButtonCount);
        for (const std::string &N : Pad::Names())
        {
            const std::optional<B> Btn = Pad::FromName(N);
            QVERIFY2(Btn, N.c_str());
            QCOMPARE(Pad::Name(*Btn), N);
        }
        QVERIFY(Pad::FromName("Start") == B::Start);
        QVERIFY(!Pad::FromName("select"));
        QVERIFY(Pad::IsDirection(B::Left) && !Pad::IsDirection(B::A));
    }

    // A stick holds a direction past 0.55 and lets it go under 0.35 — no chatter at the edge — and only one way at a
    // time, the stronger. Teeth: no hysteresis (0.4 held lets go); diagonals hold both.
    void a_stick_holds_one_direction_with_hysteresis()
    {
        QVERIFY(Pad::StickDirections(0.6, 0, {}).Right);
        QVERIFY(!Pad::StickDirections(0.5, 0, {}).Right);
        Pad::Stick Held;
        Held.Right = true;
        QVERIFY2(Pad::StickDirections(0.4, 0, Held).Right, "held: let go only under 0.35");
        QVERIFY(!Pad::StickDirections(0.3, 0, Held).Right);
        const Pad::Stick D = Pad::StickDirections(0.8, 0.6, {});
        QVERIFY2(D.Right && !D.Down, "a diagonal moves one way: the stronger axis");
        const Pad::Stick U = Pad::StickDirections(-0.6, -0.9, {});
        QVERIFY(U.Up && !U.Left);
        QVERIFY(Pad::StickDirections(-0.7, 0, {}).Left);
        QVERIFY(Pad::StickDirections(0, 0.7, {}).Down);
    }

    // A trigger is held past 0.6 and let go under 0.3. Teeth: one threshold both ways.
    void a_trigger_holds_with_hysteresis()
    {
        QVERIFY(Pad::TriggerHeld(0.61, false));
        QVERIFY(!Pad::TriggerHeld(0.59, false));
        QVERIFY(Pad::TriggerHeld(0.31, true));
        QVERIFY(!Pad::TriggerHeld(0.29, true));
    }

    // A held direction repeats after 380 ms, then every 85 ms, every 45 ms once held 1.4 s; a poll that came late
    // owes one repeat, not a burst; a face button never repeats; a released one stops. Teeth: repeat at once; catch
    // up a late poll with a burst; never speed up; repeat A.
    void a_held_direction_repeats_on_a_schedule()
    {
        Pad::Repeater R;
        R.press(B::Down, 1000);
        QVERIFY(R.held(B::Down));
        std::vector<long long> At;
        for (long long T = 1000; T <= 3000; ++T)
            for (B Got : R.due(T)) { QCOMPARE(Got, B::Down); At.push_back(T - 1000); }
        QVERIFY(At.size() > 5);
        QCOMPARE(At[0], static_cast<long long>(Pad::Repeater::First));
        QCOMPARE(At[1] - At[0], static_cast<long long>(Pad::Repeater::Every));
        QCOMPARE(At.back() - At[At.size() - 2], static_cast<long long>(Pad::Repeater::Fast));
        Pad::Repeater Late;
        Late.press(B::Up, 0);
        QCOMPARE(Late.due(5000).size(), static_cast<size_t>(1));
        Late.release(B::Up);
        QVERIFY(Late.due(9000).empty());
        Pad::Repeater Face;
        Face.press(B::A, 0);
        QVERIFY(Face.due(5000).empty());
    }

    // Down picks the control straight below over a nearer one off to the side; nothing that way is -1. Teeth: score
    // by plain distance (the diagonal wins); accept a candidate level with From.
    void nearest_prefers_straight_ahead()
    {
        const QRect From(100, 100, 50, 50);
        //The diagonal one is nearer centre to centre (117 px against 130) but off to the side.
        const QVector<QRect> C{ QRect(100, 230, 50, 50), QRect(200, 160, 50, 50), QRect(300, 100, 50, 50), QRect(100, 0, 50, 50),
                                QRect(160, 100, 50, 50) };
        QCOMPARE(PadNav::Nearest(From, B::Down, C), 0);
        QCOMPARE(PadNav::Nearest(From, B::Right, C), 4);
        QCOMPARE(PadNav::Nearest(From, B::Up, C), 3);
        QCOMPARE(PadNav::Nearest(From, B::Left, C), -1);
        QCOMPARE(PadNav::Nearest(From, B::A, C), -1);
        //A wide control (an instance list): Right goes to the button beside it on its row, not to a button on the row
        //above that starts before its right edge. Teeth: the weighed score alone (the row above: no distance along).
        const QRect Wide(100, 300, 300, 30);
        const QVector<QRect> Row{ QRect(380, 268, 120, 30), QRect(420, 300, 30, 30) };   // the row above: 3 px up
        QCOMPARE(PadNav::Nearest(Wide, B::Right, Row), 1);
        //In line, straight ahead counts: from the overlay's Resume, Down is its layout list centred below, not the small
        //Edit button off to the side that starts sooner (the overlay's own rectangles). Teeth: centring weighed 0.05.
        const QVector<QRect> Below{ QRect(286, 328, 222, 65), QRect(522, 316, 102, 89) };
        QCOMPARE(PadNav::Nearest(QRect(176, 213, 448, 89), B::Down, Below), 0);
    }

    // With nothing focused a direction focuses the dialog's default button; then each direction the control that
    // way. Teeth: focus the first child instead; ignore geometry.
    void a_direction_focuses_the_nearest_control()
    {
        QDialog D;
        auto *G = new QGridLayout(&D);
        auto *A1 = new QPushButton("a1"), *A2 = new QPushButton("a2"), *B1 = new QPushButton("b1"), *B2 = new QPushButton("b2");
        auto *E = new QLineEdit;
        G->addWidget(A1, 0, 0); G->addWidget(A2, 0, 1); G->addWidget(B1, 1, 0); G->addWidget(B2, 1, 1); G->addWidget(E, 2, 0, 1, 2);
        B2->setDefault(true);
        for (QWidget *W : { static_cast<QWidget *>(A1), static_cast<QWidget *>(A2), static_cast<QWidget *>(B1), static_cast<QWidget *>(B2), static_cast<QWidget *>(E) })
            W->setFocusPolicy(Qt::StrongFocus);
        D.resize(400, 300);
        Front(D);
        if (QWidget *F = D.focusWidget()) F->clearFocus();   // no control focused yet
        QVERIFY(PadNav::Move(&D, B::Down));
        QCOMPARE(D.focusWidget(), B2);
        Press(B::Left);
        QCOMPARE(D.focusWidget(), B1);
        Press(B::Up);
        QCOMPARE(D.focusWidget(), A1);
        Press(B::Right);
        QCOMPARE(D.focusWidget(), A2);
        Press(B::Down, 2);
        QCOMPARE(D.focusWidget(), E);
    }

    // A by what the control is: a button is clicked, a check box ticked, a combo box opened (and a direction then A
    // picks in it), a checkable group toggled. Teeth: click everything (the combo never opens); send A to the window.
    void a_presses_toggles_and_opens_by_kind()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *Btn = new QPushButton("go");
        auto *Chk = new QCheckBox("tick");
        auto *Cmb = new QComboBox;
        Cmb->addItems({ "one", "two", "three" });
        auto *Grp = new QGroupBox("group");
        Grp->setCheckable(true);
        Grp->setChecked(false);
        Grp->setFocusPolicy(Qt::StrongFocus);
        L->addWidget(Btn); L->addWidget(Chk); L->addWidget(Cmb); L->addWidget(Grp);
        D.resize(300, 300);
        Front(D);
        QSignalSpy Clicked(Btn, &QPushButton::clicked);
        Btn->setFocus();
        Press(B::A);
        QCOMPARE(Clicked.count(), 1);
        Chk->setFocus();
        Press(B::A);
        QVERIFY(Chk->isChecked());
        Cmb->setFocus();
        Press(B::A);
        QTRY_VERIFY_WITH_TIMEOUT(Cmb->view()->isVisible(), 2000);
        Press(B::Down);
        Press(B::A);
        QTRY_VERIFY_WITH_TIMEOUT(!Cmb->view()->isVisible(), 2000);
        QCOMPARE(Cmb->currentIndex(), 1);
        Grp->setFocus();
        Press(B::A);
        QVERIFY(Grp->isChecked());
    }

    // A on a text field opens the on-screen keyboard on its text; Done writes the text back as typing would (the edit
    // signals, then Return); B leaves the field as it was. Teeth: setText (no textEdited); no Return (editingFinished
    // never fires); B writes anyway.
    void a_on_a_field_types_through_the_keyboard()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *E = new QLineEdit;
        E->setPlaceholderText("Search");
        L->addWidget(E);
        D.resize(1000, 700);
        Front(D);
        QSignalSpy Edited(E, &QLineEdit::textEdited), Returned(E, &QLineEdit::returnPressed);
        E->setFocus();
        Press(B::A);
        OnScreenKeyboard *K = OnScreenKeyboard::current();
        QVERIFY(K);
        QCOMPARE(K->padState().value("title", std::string()), std::string("Search"));
        K->typeText("doom");
        K->accept();
        QApplication::processEvents();
        QCOMPARE(E->text(), QString("doom"));
        QVERIFY(Edited.count() >= 1);
        QCOMPARE(Returned.count(), 1);
        QVERIFY(!OnScreenKeyboard::current());
        Press(B::A);
        QVERIFY(OnScreenKeyboard::current());
        OnScreenKeyboard::current()->typeText("x");
        Press(B::B);
        QApplication::processEvents();
        QVERIFY(!OnScreenKeyboard::current());
        QCOMPARE(E->text(), QString("doom"));
    }

    // While the keyboard is open every press is its: directions move between keys, A types the key, X deletes, Y is
    // a space — and the window under it is offered nothing. Teeth: let a press through to the window's targets.
    void the_keyboard_takes_every_press_while_open()
    {
        Counting W;
        auto *L = new QVBoxLayout(&W);
        auto *E = new QLineEdit;
        L->addWidget(E);
        W.resize(1000, 700);
        Front(W);
        E->setFocus();
        Press(B::A);
        QVERIFY(OnScreenKeyboard::current());
        const int Before = W.Offered;
        //A first keyboard opens on the letters ("q"), not the digits. Teeth: open on the first key.
        QCOMPARE(OnScreenKeyboard::current()->padState().value("key", std::string()), std::string("q"));
        Press(B::Right, 2);
        QCOMPARE(OnScreenKeyboard::current()->padState().value("key", std::string()), std::string("e"));
        Press(B::A);
        QCOMPARE(OnScreenKeyboard::current()->text(), QString("e"));
        Press(B::Y);
        QCOMPARE(OnScreenKeyboard::current()->text(), QString("e "));
        Press(B::X, 2);
        QCOMPARE(OnScreenKeyboard::current()->text(), QString());
        Press(B::Down);
        Press(B::A);
        QCOMPARE(OnScreenKeyboard::current()->text(), QString("s"));   // the row below is staggered: under "e" is "s"
        QCOMPARE(W.Offered, Before);
        Press(B::B);
        QApplication::processEvents();
        QVERIFY(!OnScreenKeyboard::current());
        QCOMPARE(E->text(), QString());
        //The next keyboard opens on the key pressed last. Teeth: always "q".
        Press(B::A);
        QVERIFY(OnScreenKeyboard::current());
        QCOMPARE(OnScreenKeyboard::current()->padState().value("key", std::string()), std::string("s"));
        Press(B::B);
        QApplication::processEvents();
    }

    // B backs out of a dialog (rejects it, as Escape); and a press in a dialog never reaches the window behind it,
    // though that window is the dialog's parent. Teeth: walk the focus chain past the window; B does nothing.
    void b_rejects_a_dialog_and_never_reaches_the_window_behind()
    {
        Counting W;
        W.resize(600, 400);
        Front(W);
        auto *D = new QDialog(&W);
        auto *L = new QVBoxLayout(D);
        L->addWidget(new QPushButton("ok"));
        D->resize(300, 200);
        Front(*D);
        QSignalSpy Rejected(D, &QDialog::rejected);
        Press(B::Right);
        Press(B::Down);
        Press(B::B);
        QCOMPARE(Rejected.count(), 1);
        QCOMPARE(W.Offered, 0);
        delete D;
        //A dialog opened with no parent is backed out of too (it is no other window's child to close).
        QDialog Alone;
        auto *AL = new QVBoxLayout(&Alone);
        AL->addWidget(new QPushButton("ok"));
        Alone.resize(300, 200);
        Front(Alone);
        QSignalSpy AloneRejected(&Alone, &QDialog::rejected);
        Press(B::B);
        QCOMPARE(AloneRejected.count(), 1);
    }

    // In a list, up and down move the current row; at its end, focus leaves the list. Teeth: leave at once; stay
    // forever.
    void a_list_moves_inside_then_lets_focus_leave()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *List = new QListWidget;
        List->addItems({ "a", "b", "c" });
        auto *Below = new QPushButton("below");
        L->addWidget(List); L->addWidget(Below);
        D.resize(300, 400);
        Front(D);
        List->setFocus();
        List->setCurrentRow(-1);
        Press(B::Down);
        QCOMPARE(List->currentRow(), 0);
        Press(B::Down, 2);
        QCOMPARE(List->currentRow(), 2);
        QCOMPARE(D.focusWidget(), List);
        Press(B::Down);
        QCOMPARE(D.focusWidget(), Below);
    }

    // A on a table cell holding a check box presses the check box; on a plain cell it never ticks the item (every
    // QTableWidgetItem is flagged user-checkable, ticked or not). Teeth: tick by the flag (a phantom check appears).
    void a_cell_checkbox_toggles_and_a_plain_cell_gets_no_tick()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *T = new QTableWidget(1, 2);
        auto *Holder = new QWidget;
        auto *HL = new QHBoxLayout(Holder);
        auto *Chk = new QCheckBox;
        HL->addWidget(Chk);
        T->setCellWidget(0, 0, Holder);
        T->setItem(0, 1, new QTableWidgetItem("plain"));
        L->addWidget(T);
        D.resize(400, 300);
        Front(D);
        T->setFocus();
        T->setCurrentCell(0, 0);
        Press(B::A);
        QVERIFY(Chk->isChecked());
        T->setCurrentCell(0, 1);
        Press(B::A);
        QVERIFY2(!T->item(0, 1)->data(Qt::CheckStateRole).isValid(), "no phantom tick on a plain cell");
        QVERIFY(Chk->isChecked());
    }

    // Left and right step a slider; at its end the direction moves focus on instead. Teeth: never leave; never step.
    void left_right_adjust_a_slider_and_leave_at_its_end()
    {
        QDialog D;
        auto *L = new QHBoxLayout(&D);
        auto *S = new QSlider(Qt::Horizontal);
        S->setRange(0, 2);
        S->setValue(1);
        auto *Next = new QPushButton("next");
        L->addWidget(S); L->addWidget(Next);
        D.resize(400, 100);
        Front(D);
        S->setFocus();
        Press(B::Right);
        QCOMPARE(S->value(), 2);
        QCOMPARE(D.focusWidget(), S);
        Press(B::Right);
        QCOMPARE(D.focusWidget(), Next);
    }

    // The shoulders turn the pages of the tab widget the focus is in, focus landing on the new page's first control
    // as the eye reads it (not the first in the tab chain, where Qt would put it); at the last page they stop.
    // Teeth: wrap round; leave focus where Qt puts it.
    void the_shoulders_turn_tabs()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *Tabs = new QTabWidget;
        QList<QPushButton *> Btns;
        for (const char *N : { "p0", "p1", "p2" })
        {
            auto *Page = new QWidget;
            auto *PL = new QVBoxLayout(Page);
            //Put on the page first, so first in its focus chain (where Qt would put the focus), but laid out below
            //the page's top control.
            PL->addWidget(new QPushButton(QString(N) + " lower"));
            Btns << new QPushButton(N);
            PL->insertWidget(0, Btns.back());
            Tabs->addTab(Page, N);
        }
        L->addWidget(Tabs);
        D.resize(400, 300);
        Front(D);
        Btns[0]->setFocus();
        Press(B::RB);
        QCOMPARE(Tabs->currentIndex(), 1);
        QCOMPARE(D.focusWidget(), Btns[1]);
        Press(B::RB, 2);
        QCOMPARE(Tabs->currentIndex(), 2);
        Press(B::LB);
        QCOMPARE(Tabs->currentIndex(), 1);
    }

    // The ring shows once the controller is used, round the focused control; a click puts it away. Teeth: always
    // show it; never show it; draw it round the window.
    void the_focus_ring_follows_the_controller_not_the_mouse()
    {
        QDialog D;
        auto *L = new QVBoxLayout(&D);
        auto *A = new QPushButton("a"), *Bt = new QPushButton("b");
        L->addWidget(A); L->addWidget(Bt);
        D.resize(300, 300);
        Front(D);
        A->setFocus();
        Press(B::Down);
        QCOMPARE(D.focusWidget(), Bt);
        QVERIFY(PadRouter::instance().padMode());
        QTRY_COMPARE_WITH_TIMEOUT(PadRouter::instance().ringRect(), PadNav::FocusRect(Bt), 2000);
        QTest::mouseClick(A, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!PadRouter::instance().padMode());
        QTRY_VERIFY_WITH_TIMEOUT(PadRouter::instance().ringRect().isEmpty(), 2000);
    }

    // The hint bar reads in one order whoever said what: A first, the shoulders, B last. Teeth: the order they were
    // collected in (B, from the window's own screen, before generic navigation's A).
    void hints_come_in_one_order()
    {
        struct Says : QDialog, PadTarget
        {
            bool padPressed(Pad::Button) override { return false; }
            QList<Hint> padHints() const override { return { { B::B, "Leave" } }; }
        } D;
        auto *L = new QVBoxLayout(&D);
        auto *Tabs = new QTabWidget;
        auto *Page = new QWidget;
        auto *PL = new QVBoxLayout(Page);
        auto *Chk = new QCheckBox("tick");
        PL->addWidget(Chk);
        Tabs->addTab(Page, "one");
        Tabs->addTab(new QWidget, "two");
        L->addWidget(Tabs);
        D.resize(300, 300);
        Front(D);
        Chk->setFocus();
        std::vector<std::string> Got;
        for (const PadTarget::Hint &H : PadRouter::instance().hints()) Got.push_back(Pad::Name(H.B));
        QCOMPARE(Got, (std::vector<std::string>{ "a", "lb", "b" }));
    }

    // A chord: what is held reads as held while the last button is pressed, and not after. Teeth: mark nothing held.
    void a_chord_is_read_while_held()
    {
        bool HeldThen = false;
        auto C = connect(&PadInput::instance(), &PadInput::pressed, this,
                         [&HeldThen](B Btn) { if (Btn == B::Start) HeldThen = PadInput::instance().held(B::Back); });
        PadInput::instance().injectChord({ B::Back }, B::Start);
        disconnect(C);
        QVERIFY(HeldThen);
        QVERIFY(!PadInput::instance().held(B::Back));
    }

    // How the app starts: --controller, then --desktop, then the setting; "auto" is controller mode only inside
    // Steam's Game Mode. Teeth: the setting over the flags; auto always one way.
    void controller_mode_starts_by_flag_then_setting_then_game_mode()
    {
        qunsetenv("SteamGamepadUI");
        qunsetenv("XDG_CURRENT_DESKTOP");
        qunsetenv("GAMESCOPE_WAYLAND_DISPLAY");
        json C = json::object();
        QCOMPARE(ControllerMode::StartSetting(C), ControllerMode::Start::Auto);
        QVERIFY(!ControllerMode::StartsInController(C, false, false));
        QVERIFY(ControllerMode::StartsInController(C, true, false));
        ControllerMode::SetStart(C, ControllerMode::Start::Controller);
        QVERIFY(ControllerMode::StartsInController(C, false, false));
        QVERIFY(!ControllerMode::StartsInController(C, false, true));
        ControllerMode::SetStart(C, ControllerMode::Start::Auto);
        qputenv("SteamGamepadUI", "1");
        QVERIFY(ControllerMode::InGameMode());
        QVERIFY(ControllerMode::StartsInController(C, false, false));
        qunsetenv("SteamGamepadUI");
        QVERIFY(ControllerMode::DesktopNavigation(C));
        ControllerMode::SetDesktopNavigation(C, false);
        QVERIFY(!ControllerMode::DesktopNavigation(C));
        QVERIFY(!ControllerMode::Haptics(C));
        ControllerMode::SetHaptics(C, true);
        QVERIFY(ControllerMode::Haptics(C));
        QVERIFY(ControllerMode::StartFrom("desktop") == ControllerMode::Start::Desktop);
        QVERIFY(!ControllerMode::StartFrom("tv"));
    }

    // ── The shell, over a real library ──

    // The shelves hold what the Library and Catalog tabs hold (CardPool): the games here, and the ones to download.
    // The shoulders walk the sections and stop at the ends. Teeth: build the shelves by another rule; wrap round.
    void the_shell_shows_the_shelves_and_the_shoulders_walk_its_sections()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        QCOMPARE(Sh.libraryCards().size(), 8);
        QCOMPARE(Sh.catalogCards().size(), 3);
        QVERIFY(Sh.installed("1") && !Sh.installed("9") && Sh.card("9"));
        QCOMPARE(Sh.section(), DeckShell::Section::Home);
        Press(B::RB);
        QCOMPARE(Sh.section(), DeckShell::Section::Library);
        Press(B::RB, 10);
        QCOMPARE(Sh.section(), DeckShell::Section::Settings);
        Press(B::LB);
        QCOMPARE(Sh.section(), DeckShell::Section::Peers);
        Press(B::B);
        QCOMPARE(Sh.section(), DeckShell::Section::Home);
        QCOMPARE(DeckShell::SectionFrom("catalog"), std::optional<DeckShell::Section>(DeckShell::Section::Catalog));
        //The Guide button is Home from anywhere (a console's). Teeth: Guide unhandled in the shell.
        Sh.showSection(DeckShell::Section::Peers);
        Press(B::Guide);
        QCOMPARE(Sh.section(), DeckShell::Section::Home);
        Sh.setActive(false);
    }

    // A game is in the Library as soon as one edition of it is installed (with that edition), and its Catalog card
    // offers the rest marked IN LIBRARY; a game with nothing here and nothing to fetch it from is shown MISSING, not
    // dropped from every tab. Teeth: the Library asking every edition installed (Lima gone), or the missing one skipped.
    void a_game_with_one_edition_installed_is_in_the_library()
    {
        namespace NF = NodeFixture;
        LibraryFixture L;
        const std::string Root = (L.Dir.path() + "/LIBRARY/VidyaGod/").toStdString();
        const std::string Lima = Root + "[12] Lima", Mike = Root + "[13] Mike";
        std::filesystem::create_directories(Lima);
        std::filesystem::create_directories(Mike);
        NF::WriteNodes(Lima + "/a.json", NF::Chain("la", {NF::Tile("12", "Lima"), NF::Exec("win32", "g.exe"),
                                                          NF::ContentCid("zip", "a.zip", "bafkreilimaa")}, {}, {{"VARIANT", "A"}}));
        NF::WriteNodes(Lima + "/b.json", NF::Chain("lb", {NF::Tile("12", "Lima"), NF::Exec("win32", "g.exe"),
                                                          NF::ContentCid("zip", "b.zip", "bafkreilimab")}, {}, {{"VARIANT", "B"}}));
        std::ofstream(Lima + "/a.zip") << "content";
        NF::WriteNodes(Mike + "/m.json", NF::Chain("mk", {NF::Tile("13", "Mike"), NF::Exec("win32", "g.exe"),
                                                          NF::Content("zip", "m.zip")}, {}, {{"VARIANT", "Edition"}}));
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        const QList<LibraryGameCard *> Here = CardPool::Installed(M), Get = CardPool::Available(M);
        const auto Find = [](const QList<LibraryGameCard *> &Cards, const char *Uid) -> LibraryGameCard * {
            for (LibraryGameCard *C : Cards) if (C->GameKey == Uid) return C;
            return nullptr;
        };
        LibraryGameCard *LimaHere = Find(Here, "12"), *LimaGet = Find(Get, "12"), *MikeHere = Find(Here, "13");
        QVERIFY(LimaHere && !LimaHere->Missing);
        QCOMPARE(LimaHere->GroupNodeIds.size(), std::size_t(1));                   // the installed edition only
        QVERIFY(LimaGet && LimaGet->InLibrary);
        QVERIFY(MikeHere && MikeHere->Missing && !Find(Get, "13"));
        QVERIFY(Find(Here, "1") && !Find(Here, "1")->Missing && !Find(Here, "9"));   // the plain cases stay as they were
        QVERIFY(!Find(Get, "9")->InLibrary);
        qDeleteAll(Here);
        qDeleteAll(Get);
    }

    // The right stick scrolls the grid and the selection comes along (it stays a tile on screen); LT/RT move it a
    // screenful. Teeth: scroll the view only (the selection left off screen); LT/RT doing nothing.
    void the_stick_takes_the_selection_along_and_the_triggers_page()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        const QList<LibraryGameCard *> Cards = CardPool::Installed(M);
        QVERIFY(Cards.size() >= 8);
        DeckGrid G;
        G.resize(420, 300);   // a row or so on screen
        G.setCards(Cards);
        G.show();
        QCoreApplication::processEvents();
        G.setCurrentIndex(0);
        G.padScrolled(0, 5000);
        QVERIFY(G.currentIndex() > 0);
        QVERIFY(G.rect().contains(G.tileRect(G.currentIndex()).toRect()));
        const int Was = G.currentIndex();
        G.padPressed(Pad::Button::LT);
        QVERIFY(G.currentIndex() < Was);
        G.setCurrentIndex(0);
        G.padPressed(Pad::Button::RT);
        QVERIFY(G.currentIndex() > 0);
        qDeleteAll(Cards);
    }

    // A search matches a game's title, its series and who made it (Library and Catalog alike); the card grid takes
    // Home, End and a page at a time from the keyboard. Teeth: title only; no Home/End/PgDn.
    void a_search_matches_series_and_developer_and_the_grid_takes_home_end_and_pages()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        QList<LibraryGameCard *> Cards = CardPool::Installed(M);
        LibraryGameCard *Alpha = nullptr;
        for (LibraryGameCard *C : Cards) if (C->GameTitle == "Alpha") Alpha = C;
        QVERIFY(Alpha);
        QVERIFY(CardPool::Matches(*Alpha, "studio alpha"));   // its DEVELOPER
        QVERIFY(CardPool::Matches(*Alpha, "SAGA"));           // its SERIES
        QVERIFY(!CardPool::Matches(*Alpha, "zzz"));
        LibraryView V;
        V.resize(420, 300);
        V.setCards(&Cards);
        V.show();
        V.layoutCards(120);
        QApplication::processEvents();
        V.setFocus();
        QTest::keyClick(&V, Qt::Key_End);
        QCOMPARE(V.property("padIndex").toInt(), int(Cards.size()) - 1);
        QTest::keyClick(&V, Qt::Key_Home);
        QCOMPARE(V.property("padIndex").toInt(), 0);
        QTest::keyClick(&V, Qt::Key_PageDown);
        QVERIFY(V.property("padIndex").toInt() > 0);
        V.setCards(nullptr);
        qDeleteAll(Cards);
        //Ctrl+F goes to the tab's search. Teeth: no shortcut.
        LibraryTab Tab(M);
        Tab.resize(1000, 600);
        Front(Tab);
        QTest::keySequence(&Tab, QKeySequence::Find);
        auto *F = qobject_cast<QLineEdit *>(QApplication::focusWidget());
        QVERIFY(F && F->placeholderText() == QStringLiteral("Search…"));
    }

    // The download dialog names what a blocked game waits for (the package, the peer whose share holds it), and its
    // Download is off with the reason while nothing is picked. Teeth: the old generic sentence; Download left on.
    void the_download_dialog_names_what_it_waits_for_and_needs_a_pick()
    {
        namespace NF = NodeFixture;
        LibraryFixture L;
        const std::string Base = "bafkreibasepackmissingaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string Root = (L.Dir.path() + "/LIBRARY/VidyaGod/[14] Needs Base").toStdString();
        std::filesystem::create_directories(Root);
        NF::WriteNodes(Root + "/g.json", NF::Chain("nb", {NF::Tile("14", "Needs Base"), NF::Exec("win32", "g.exe"),
                                                         NF::ContentCid("zip", "g.zip", "bafkreineedsbasezip")}, {Base}, {{"VARIANT", "Edition"}}));
        L.Cfg["PeerLibraries"] = json{{"12D3KooWFriendBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", {{"Games", json::array({
            json{{"cid", Base}, {"pkg", "Base Pack"}, {"node", "base.json"}} })}}}};
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        const QList<LibraryGameCard *> Get = CardPool::Available(M);
        LibraryGameCard *Needs = nullptr, *India = nullptr;
        for (LibraryGameCard *C : Get) { if (C->GameKey == "14") Needs = C; if (C->GameKey == "9") India = C; }
        QVERIFY(Needs && India);
        QString Said;
        QTimer::singleShot(400, this, [&] {
            if (QWidget *D = QApplication::activeModalWidget())
            {
                if (auto *W = D->findChild<QLabel *>("downloadWaitsFor")) Said = W->text();
                qobject_cast<QDialog *>(D)->reject();
            }
        });
        AskDownload(M, *Needs, nullptr);
        QVERIFY2(Said.contains("Base Pack") && Said.contains("Games"), qPrintable(Said));
        bool OffWithReason = false;
        QTimer::singleShot(400, this, [&] {
            QWidget *D = QApplication::activeModalWidget();
            if (!D) return;
            for (QCheckBox *C : D->findChildren<QCheckBox *>()) C->setChecked(false);
            QTimer::singleShot(900, D, [&, D] {
                if (auto *Go = D->findChild<QPushButton *>("downloadGo")) OffWithReason = !Go->isEnabled() && Go->toolTip().contains("Nothing");
                qobject_cast<QDialog *>(D)->reject();
            });
        });
        AskDownload(M, *India, nullptr);
        QVERIFY(OffWithReason);
        qDeleteAll(Get);
    }

    // A game's page offers its controller layout: the configurator, opened for that game. Teeth: the button missing,
    // or the configurator opened for no game (it could not then choose the layout for it).
    void a_games_page_opens_its_controller_layout()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.showSection(DeckShell::Section::Library);
        Press(B::Right);
        Press(B::A);
        QVERIFY(Sh.gameOpen());
        QString Seen;
        QTimer::singleShot(300, this, [&] {
            for (QWidget *W : QApplication::topLevelWidgets())
                if (auto *E = qobject_cast<LayoutEditor *>(W); E && E->isVisible()) { Seen = E->windowTitle(); E->reject(); }
        });
        Press(B::Right, 2);
        Press(B::A);
        QTRY_COMPARE_WITH_TIMEOUT(Seen, QStringLiteral("Controller layout — Bravo"), 3000);
        Sh.setActive(false);
    }

    // A Steam Deck without Steam running (a VidyaGod session of its own) has nothing else reading its controller, and
    // its kernel gamepad is silent in lizard mode: controller mode reads the Deck itself, through SDL's driver for it;
    // the desktop lets it go, so its trackpad is the mouse again. Seen on the Deck: every button dead in controller
    // mode. Teeth: the shell not turning it on, or not off again, or the SDL hint not following.
    void a_steam_deck_is_read_directly_in_controller_mode_only()
    {
        const auto Hint = [] { const char *H = SDL_GetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK"); return QString::fromUtf8(H ? H : ""); };
        PadInput &In = PadInput::instance();
        In.setDeckDirect(false);
        QCOMPARE(Hint(), QStringLiteral("0"));
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.setActive(true);
        QVERIFY(In.deckDirect());
        QCOMPARE(Hint(), QStringLiteral("1"));
        Sh.setActive(false);
        QVERIFY(!In.deckDirect());
        QCOMPARE(Hint(), QStringLiteral("0"));
    }

    // Y sorts the shelf (by name, recently played, release, series) and the sort is remembered for the next start;
    // the focus stays on the game it was on, wherever the order puts it. Teeth: forget the sort; sort nothing.
    void y_sorts_and_the_sort_is_remembered()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        {
            DeckShell Sh(M, Ipfs, Dl);
            Sh.resize(1280, 800);
            Front(Sh);
            Sh.setActive(true);
            Sh.showSection(DeckShell::Section::Library);
            const auto Title = [&] { return Sh.padState()["page"]["grid"]["card"].value("title", std::string()); };
            QCOMPARE(Title(), std::string("Alpha"));
            Press(B::Y, 2);   // Recently played, then Release date
            QCOMPARE(Sh.padState()["page"].value("sort", std::string()), std::string("Release date"));
            //Focus stays on the game it was on, now fourth: Golf 1990, Delta 1995, Bravo 1999, Alpha 2001.
            QCOMPARE(Title(), std::string("Alpha"));
            QCOMPARE(Sh.padState()["page"]["grid"].value("index", -1), 3);
            QCOMPARE(L.Cfg["Settings"]["Controller"]["LibrarySort"].get<int>(), static_cast<int>(SortMode::Date));
            Sh.setActive(false);
        }
        DeckShell Again(M, Ipfs, Dl);
        Again.resize(1280, 800);
        Again.showSection(DeckShell::Section::Library);
        QCOMPARE(Again.padState()["page"].value("sort", std::string()), std::string("Release date"));
    }

    // X searches the shelf through the keyboard (live, as typed); B clears the search before it backs out. Teeth: B
    // leaves the section with the search still on; the search filters nothing.
    void x_searches_and_b_clears_the_search()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.showSection(DeckShell::Section::Library);
        Press(B::X);
        QVERIFY(OnScreenKeyboard::current());
        //A live search docks a lower keyboard: more of the grid it filters stays in sight (under 42% of the screen, not
        //56%). Teeth: the one height for every keyboard.
        QVERIFY(OnScreenKeyboard::current()->padState().value("top", 0) > Sh.height() / 2);
        OnScreenKeyboard::current()->typeText("arl");
        QCOMPARE(Sh.padState()["page"].value("shown", 0), 1);   // live
        OnScreenKeyboard::current()->accept();
        QApplication::processEvents();
        QCOMPARE(Sh.padState()["page"].value("search", std::string()), std::string("arl"));
        QVERIFY(Sh.statusText().contains("match"));
        Press(B::B);
        QCOMPARE(Sh.section(), DeckShell::Section::Library);
        QCOMPARE(Sh.padState()["page"].value("shown", 0), 8);
        Press(B::B);
        QCOMPARE(Sh.section(), DeckShell::Section::Home);
        Sh.setActive(false);
    }

    // A opens the focused game's page; B closes it, focus on that game's cover. Its page marks it golden (this
    // machine's config only), and every card of the family shows it. Teeth: open the first card whatever is focused;
    // leave the focus where it was when the page was another game's; mark without saving the marker.
    void a_opens_a_games_page_b_returns_and_it_marks_golden()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.showSection(DeckShell::Section::Library);
        Press(B::Right);
        Press(B::A);
        QVERIFY(Sh.gameOpen());
        QCOMPARE(Sh.padState()["game"].value("title", std::string()), std::string("Bravo"));
        QCOMPARE(Sh.padState()["game"]["buttons"][2].get<std::string>(), std::string("Controller layout"));
        QCOMPARE(Sh.padState()["game"]["buttons"][3].get<std::string>(), std::string("Mark golden"));
        Press(B::Right, 3);
        Press(B::A);
        QVERIFY(!PackageCatalog::GoldenSince(L.Cfg, "2").empty());
        QVERIFY(Sh.card("2")->Golden);
        QCOMPARE(Sh.padState()["game"]["buttons"][3].get<std::string>(), std::string("Unmark golden"));
        Press(B::B);
        QVERIFY(!Sh.gameOpen());
        QCOMPARE(Sh.padState()["page"]["grid"]["card"].value("title", std::string()), std::string("Bravo"));
        //A page opened straight onto another game (Home's hero, the CLI): back on the shelf, the focus is on it.
        QVERIFY(Sh.openGame("5"));
        Press(B::B);
        QCOMPARE(Sh.padState()["page"]["grid"]["card"].value("title", std::string()), std::string("Echo"));
        Sh.setActive(false);
    }

    // Home leads with the game played last, Play one press away, drawn from its large cover — which a burst of
    // covers landing never swaps for the small tile one. Teeth: lead with the first by name; take the tile's cover
    // on every refresh.
    void home_leads_with_the_game_played_last()
    {
        LibraryFixture L("6");
        InstanceStore::TouchLastRun(L.Cfg, "6");
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        //The play times are read off the GUI thread, after the shelves are built.
        QTRY_COMPARE_WITH_TIMEOUT(Sh.padState()["page"]["hero"].value("title", std::string()), std::string("Foxtrot"), 10000);
        QCOMPARE(Sh.padState()["page"].value("button", std::string()), std::string("▶  Play"));
        //The large cover is decoded off the GUI thread at the hero's height (about 470 px here); a tile's is a shelf
        //cover's (about 270 px at this scale).
        const auto HeroH = [&Sh] { const json C = Sh.padState()["page"]["hero"]["cover"]; return C.is_array() ? C[1].get<int>() : 0; };
        QTRY_VERIFY_WITH_TIMEOUT(HeroH() > 400, 10000);
        const int Large = HeroH();
        emit M.coversReady();
        QCOMPARE(HeroH(), Large);
        Press(B::Down);
        QCOMPARE(Sh.padState()["page"].value("area", std::string()), std::string("recent"));
        Sh.setActive(false);
    }

    // Controller mode is drawn in the app's own look, its mark's: titles in the display serif that ships inside the
    // app (a Deck has none of its own), and the cover behind a page in the mark's two tones, its darks plum and its
    // lights magenta. Teeth: the face is not loaded (the default face stands in); the duotone keeps a cover's colours.
    void controller_mode_is_drawn_in_the_marks_own_look()
    {
        const QFontInfo Title(DeckStyle::Display(30, 1)), Label(DeckStyle::Display(30, 1, true));
        QVERIFY2(Title.family().contains("Noto Serif Display"), qPrintable(Title.family()));
        QVERIFY2(Label.family().contains("Noto Serif Display"), qPrintable(Label.family()));
        QVERIFY(Label.italic());
        const auto Tone = [](const QColor &In) {
            QImage I(4, 4, QImage::Format_RGB32);
            I.fill(In);
            return DeckStyle::Duotone(I).pixelColor(1, 1);
        };
        const QColor Dark = Tone(Qt::black), Light = Tone(Qt::white), Green = Tone(QColor(0, 200, 0));
        QVERIFY2(Dark.value() < 40 && Dark.red() > Dark.green() && Dark.blue() > Dark.green(), qPrintable(Dark.name()));   // plum
        QVERIFY2(Light.red() > 140 && Light.blue() > 140 && Light.green() < 90, qPrintable(Light.name()));                  // magenta
        QVERIFY2(Green.green() < Green.red() && Green.green() < Green.blue(), qPrintable(Green.name()));                    // no longer green
    }

    // The lead game's cover, washed behind the hero, stays behind the hero: scaled to cover it, it reached down over
    // the rows under it. Teeth: the wash is not clipped to the hero.
    void the_backdrop_stays_behind_the_hero()
    {
        LibraryFixture L("6");
        InstanceStore::TouchLastRun(L.Cfg, "6");
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        //The cover and its wash are decoded together, off the GUI thread.
        const auto HeroH = [&Sh] { const json C = Sh.padState()["page"]["hero"]["cover"]; return C.is_array() ? C[1].get<int>() : 0; };
        QTRY_VERIFY_WITH_TIMEOUT(HeroH() > 400, 10000);
        auto *Recent = Sh.findChild<DeckGrid *>("controllerHomeRecent");
        QVERIFY(Recent && Recent->isVisible());
        //Right of the one game played: nothing is drawn there but the ground.
        const QPoint Under = Recent->mapTo(&Sh, QPoint(Recent->width() - 40, Recent->height() / 2));
        const QColor C = Sh.grab().toImage().pixelColor(Under);
        QVERIFY2(C.red() < 32 && C.blue() < 36, qPrintable(C.name()));
        Sh.setActive(false);
    }

    // A desktop page in controller mode takes controller mode's colours, under the desktop's own theme as the app runs
    // it: the Node page's filter lists too, which the desktop colours by name. Teeth: the lists keep a colour of their
    // own; controller mode's sheet does not name them.
    void a_desktop_page_takes_controller_modes_colours()
    {
        const QString SheetWas = qApp->styleSheet();
        const QPalette PaletteWas = qApp->palette();
        const auto Restore = qScopeGuard([&] { qApp->setStyleSheet(SheetWas); qApp->setPalette(PaletteWas); });
        Theme::Apply(*qApp);
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.openAdvanced(DeckShell::Advanced::Node);
        QListWidget *List = nullptr;
        for (QListWidget *W : Sh.findChildren<QListWidget *>())
            if (W->isVisible() && W->count() > 1) { List = W; break; }
        QVERIFY(List);
        //The right end of a row that is not the current one: no text, no selection, the list's own ground.
        const QImage Img = List->viewport()->grab().toImage();
        const QRect Row = List->visualItemRect(List->item(1));
        const QColor C = Img.pixelColor(Img.width() - 4, Row.center().y());
        QCOMPARE(C.name(), DeckStyle::GroundLow.name());
        Sh.setActive(false);
    }

    // A hosted page in a scroll area (Peers) is walked by pad: focus lands on a control on the page, never on the scroll
    // area holding it, and the directions move through the page's controls. Teeth: a plain scroll area as a stop (it
    // took focus first and no press ever left it).
    void a_hosted_pages_controls_are_reached_by_pad()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.openAdvanced(DeckShell::Advanced::Peers);
        QCoreApplication::processEvents();
        QWidget *F = QApplication::focusWidget();
        QVERIFY(F && !qobject_cast<QScrollArea *>(F));
        std::set<QWidget *> Seen{F};
        for (int I = 0; I < 6; ++I) { Press(B::Down); Seen.insert(QApplication::focusWidget()); }
        QVERIFY2(Seen.size() >= 3, "the directions do not move through the page");
        Sh.setActive(false);
    }

    // B inside a Settings page goes back to its sidebar, where the user came from; B on the sidebar goes Home. Teeth:
    // B from the page straight Home (the place lost).
    void b_in_a_settings_page_goes_back_to_its_sidebar()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.openAdvanced(DeckShell::Advanced::Settings);
        QCoreApplication::processEvents();
        auto *Side = Sh.findChild<QListWidget *>("SettingsSidebar");
        QVERIFY(Side);
        //General (the tray, starting at login) is not offered in controller mode. Teeth: no showEvent rule.
        QVERIFY(!Side->findItems("General", Qt::MatchExactly).isEmpty() && Side->findItems("General", Qt::MatchExactly).first()->isHidden());
        QVERIFY(Side->currentItem() && !Side->currentItem()->isHidden());
        QWidget *InPage = nullptr;
        for (QCheckBox *C : Sh.findChildren<QCheckBox *>()) if (C->isVisible() && !Side->isAncestorOf(C)) { InPage = C; break; }
        QVERIFY(InPage);
        InPage->setFocus();
        QCoreApplication::processEvents();
        Press(B::B);
        QCOMPARE(Sh.section(), DeckShell::Section::Settings);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(Side));
        Press(B::B);                                   // the sidebar: back to controller mode's own Settings page
        QCOMPARE(Sh.section(), DeckShell::Section::Settings);
        QVERIFY(!Sh.advanced());
        QCOMPARE(Sh.padState()["page"]["kind"].get<std::string>(), std::string("controllerSettings"));
        Press(B::B);
        QCOMPARE(Sh.section(), DeckShell::Section::Home);
        Sh.setActive(false);
    }

    // Start opens the menu; it takes every press; its first item leaves for the desktop. Teeth: the menu lets presses
    // through to the shelf; choose nothing.
    void the_menu_opens_with_start_and_leaves_for_the_desktop()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        QSignalSpy Desktop(&Sh, &DeckShell::desktopRequested);
        Press(B::Start);
        QVERIFY(Sh.menuOpen());
        Press(B::RB);
        QCOMPARE(Sh.section(), DeckShell::Section::Home);   // the menu's
        Press(B::Down);
        Press(B::Up);
        Press(B::A);
        QVERIFY(!Sh.menuOpen());
        QCOMPARE(Desktop.count(), 1);
        Press(B::Start);
        Press(B::B);
        QVERIFY(!Sh.menuOpen());
        Sh.setActive(false);
    }

    // Controller mode's Settings is a page of its own: what matters on a handheld, every row a toggle or a left/right
    // value that writes the setting, nothing only a desktop has (the tray, starting at login); its Advanced… is the
    // desktop's, and B comes back. Teeth: a value or a toggle not reaching the config; a desktop-only row; Advanced… not
    // opening, or B leaving for Home from it.
    void controller_settings_is_a_page_of_its_own()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.showSection(DeckShell::Section::Settings);
        QCoreApplication::processEvents();
        json Page = Sh.padState()["page"];
        QCOMPARE(Page["kind"].get<std::string>(), std::string("controllerSettings"));
        QCOMPARE(Page["current"].get<std::string>(), std::string("controller.start"));
        std::set<std::string> Ids;
        for (const auto &R : Page["rows"])
        {
            Ids.insert(R.value("id", std::string()));
            const QString T = QString::fromStdString(R.value("title", std::string())).toLower();
            QVERIFY2(!T.contains("tray") && !T.contains("login"), qPrintable(T));
        }
        for (const char *Want : { "controller.start", "controller.haptics", "controller.desktop", "screen.saver", "net", "storage",
                                  "sources.sync", "runners", "advanced.node", "advanced.runners", "advanced" })
            QVERIFY2(Ids.count(Want), Want);
        Press(B::Right);
        QVERIFY(ControllerMode::StartSetting(L.Cfg) == ControllerMode::Start::Controller);
        Press(B::Down);
        QCOMPARE(Sh.padState()["page"]["current"].get<std::string>(), std::string("controller.haptics"));
        const bool Was = ControllerMode::Haptics(L.Cfg);
        Press(B::A);
        QCOMPARE(ControllerMode::Haptics(L.Cfg), !Was);
        Press(B::Down, 2);
        QCOMPARE(Sh.padState()["page"]["current"].get<std::string>(), std::string("screen.saver"));
        const int Before = ScreenSaverSeconds(L.Cfg);
        Press(B::Left);
        QVERIFY(ScreenSaverSeconds(L.Cfg) < Before);
        QVERIFY(Sh.settingsPage()->press("advanced"));
        QVERIFY(Sh.advanced() == DeckShell::Advanced::Settings);
        QVERIFY(Sh.findChild<SettingsTab *>() && Sh.findChild<SettingsTab *>()->isVisible());
        for (int I = 0; I < 3 && Sh.advanced(); ++I) Press(B::B);
        QVERIFY(!Sh.advanced());
        QCOMPARE(Sh.section(), DeckShell::Section::Settings);
        QVERIFY(Sh.settingsPage()->isVisible());
        Sh.setActive(false);
    }

    // Controller mode's Peers is a page of its own, read from the node: a request waiting (Accept / Decline), a device
    // nearby (one press pairs), each peer with its permissions as switches and Block / Remove asked twice, and the code
    // to pair from afar as a QR with its short code. Teeth: a switch not reaching the model; Remove on one press; the QR
    // not the peer code; the short code not the code's.
    void controller_peers_is_a_page_of_its_own()
    {
        QTemporaryDir Dir;
        const QString Repo = Dir.path() + "/ipfs";
        QDir().mkpath(Repo);
        {
            std::ofstream OF((Repo + "/social.json").toStdString());
            OF << R"({"profile":{"nick":"me"},"contacts":[)"
                  R"({"peer":"12D3KooWSavedPal","nick":"savedpal","state":"accepted","added":1},)"
                  R"({"peer":"12D3KooWAsking","nick":"LGZ-MacBook","state":"incoming","added":2}]})";
        }
        std::string Err;
        QVERIFY2(IpfsWrapper::StartNode(Repo.toStdString(), &Err), Err.c_str());
        const auto Stop = qScopeGuard([] { NetworkTab::SetNearbySource({}); IpfsWrapper::StopNode(); });
        NetworkTab::SetNearbySource([] { return std::vector<IpfsWrapper::NearbyPeer>{ { "12D3KooWDeckDeviceA1b2c3", "LGZ-SteamDeck", "" } }; });
        json Cfg = json{{"Settings", {{"IPFS", {{"Enabled", true}}}}}, {"LIBRARY", json::array()},
                        {"ShareSheet", json{{"bafkreia", "Games/[1] A/a.json"}}}};
        QDir AppDir(Dir.path());
        AppModel M(&Cfg, &AppDir);
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.showSection(DeckShell::Section::Peers);
        QCoreApplication::processEvents();
        std::map<std::string, json> Rows;
        const auto Read = [&] { Rows.clear(); for (const auto &R : Sh.padState()["page"]["rows"]) if (R.contains("id")) Rows[R["id"].get<std::string>()] = R; };
        Read();
        for (const char *Want : { "accept:12D3KooWAsking", "decline:12D3KooWAsking", "pair:12D3KooWDeckDeviceA1b2c3", "shortcode",
                                  "share:Games:12D3KooWSavedPal", "receive:12D3KooWSavedPal", "vlan:12D3KooWSavedPal",
                                  "presence:12D3KooWSavedPal", "block:12D3KooWSavedPal", "remove:12D3KooWSavedPal", "code", "advanced" })
            QVERIFY2(Rows.count(Want), Want);
        QVERIFY(!Rows.count("pair:12D3KooWSavedPal"));                       // in the book: not offered again
        const QString Code = QString::fromStdString(IpfsWrapper::PeerCode());
        QCOMPARE(QString::fromStdString(Rows["code"]["code"].get<std::string>()), Code);
        QCOMPARE(QString::fromStdString(Rows["code"]["title"].get<std::string>()), PeerSharing::ShortCode(Code));
        QVERIFY(Rows["pair:12D3KooWDeckDeviceA1b2c3"]["says"].get<std::string>().find(PeerSharing::ShortCode("12D3KooWDeckDeviceA1b2c3").toStdString())
                != std::string::npos);

        QVERIFY(!M.peers().isReceivingFrom("12D3KooWSavedPal"));
        QVERIFY(Sh.peersPage()->press("receive:12D3KooWSavedPal"));
        QVERIFY(M.peers().isReceivingFrom("12D3KooWSavedPal"));
        QTRY_VERIFY_WITH_TIMEOUT([&] { Read(); return Rows["receive:12D3KooWSavedPal"].value("on", false); }(), 3000);

        QVERIFY(Sh.peersPage()->press("remove:12D3KooWSavedPal"));         // once: asked
        QCOMPARE(Sh.padState()["page"].value("armed", std::string()), std::string("remove:12D3KooWSavedPal"));
        bool Still = false;
        for (const auto &C : IpfsWrapper::PeerList()) Still = Still || C.PeerID == "12D3KooWSavedPal";
        QVERIFY(Still);
        Press(B::Down);                                                      // moving off disarms it
        QVERIFY(!Sh.padState()["page"].contains("armed"));
        Sh.setActive(false);
    }

    // The short code: six characters that never read alike, the same for the same peer, typed in any case, with or
    // without its dash, O for 0 and I or L for 1; a code two devices share names neither. Teeth: case-sensitive
    // matching; the dash required; a clash taken as the first.
    void a_short_code_is_typed_as_it_is_read()
    {
        const QString A = "12D3KooWDeckDeviceA1b2c3", Bp = "12D3KooWLaptopXyZ";
        const QString Sa = PeerSharing::ShortCode(A);
        QCOMPARE(Sa, PeerSharing::ShortCode(A));
        QCOMPARE(Sa.size(), 7);
        QCOMPARE(Sa[3], QChar('-'));
        for (QChar C : QString(Sa).remove('-')) QVERIFY2(QString("0123456789ABCDEFGHJKMNPQRSTVWXYZ").contains(C), qPrintable(Sa));
        QVERIFY(Sa != PeerSharing::ShortCode(Bp));
        QCOMPARE(PeerSharing::PeerForShortCode(Sa, { A, Bp }), A);
        QCOMPARE(PeerSharing::PeerForShortCode(Sa.toLower().remove('-'), { A, Bp }), A);
        QString Spoken = Sa;
        Spoken.replace('0', 'O').replace('1', 'l');
        QCOMPARE(PeerSharing::PeerForShortCode(Spoken, { A, Bp }), A);
        QVERIFY(PeerSharing::PeerForShortCode(Sa, { Bp }).isEmpty());
        QVERIFY(PeerSharing::PeerForShortCode(Sa, { A, A + "" }) == A);    // the same device twice is one
        QVERIFY(PeerSharing::PeerForShortCode("ZZZ", { A }).isEmpty());
        //Two devices whose codes clash: the code names neither (found by trying: 30 bits clash within ~2^15 IDs).
        QHash<QString, QString> Seen;
        QString P1, P2;
        for (int I = 0; I < 400000 && P1.isEmpty(); ++I)
        {
            const QString Id = "12D3KooWClash" + QString::number(I);
            const QString C = PeerSharing::ShortCode(Id);
            if (Seen.contains(C)) { P1 = Seen[C]; P2 = Id; }
            else Seen.insert(C, Id);
        }
        QVERIFY(!P1.isEmpty());
        QVERIFY(PeerSharing::PeerForShortCode(PeerSharing::ShortCode(P1), { P1, P2 }).isEmpty());
        QCOMPARE(PeerSharing::PeerForShortCode(PeerSharing::ShortCode(P1), { P1 }), P1);
    }

    // The QR a phone reads: what Encode draws decodes to the text, at every size a peer code (and more) takes. Teeth:
    // any bit of the encoder wrong (the reader finds nothing, or other text). Needs zbarimg; skipped without it.
    void the_peer_code_qr_reads_back()
    {
        const QString Zbar = QStandardPaths::findExecutable("zbarimg");
        if (Zbar.isEmpty()) QSKIP("zbarimg is not installed");
        QTemporaryDir D;
        for (const QString &T : { QString("12D3KooWDeckDeviceA1b2c3"), QString("12D3KooW") + QString(44, 'x'),
                                  QString(120, 'Q'), QString(200, 'z') })
        {
            const auto M = QrCode::Encode(T.toStdString());
            QVERIFY(!M.empty());
            const int N = static_cast<int>(M.size()), Cell = 6, Side = (N + 8) * Cell;
            QImage Img(Side, Side, QImage::Format_Grayscale8);
            Img.fill(255);
            for (int Y = 0; Y < N; ++Y)
                for (int X = 0; X < N; ++X)
                    if (M[Y][X])
                        for (int Dy = 0; Dy < Cell; ++Dy)
                            for (int Dx = 0; Dx < Cell; ++Dx) Img.setPixel((X + 4) * Cell + Dx, (Y + 4) * Cell + Dy, qRgb(0, 0, 0));
            const QString F = D.filePath("q.png");
            QVERIFY(Img.save(F));
            QProcess P;
            P.start(Zbar, { "-q", "--raw", F });
            QVERIFY(P.waitForFinished(20000));
            QCOMPARE(QString::fromUtf8(P.readAllStandardOutput()).trimmed(), T);
        }
        QVERIFY(QrCode::Encode(std::string(300, 'x')).empty());                // past version 10: none
    }

    // The menu's "Controller settings" opens Settings at its Controller page: the controller's settings are a page of
    // Settings of their own, General keeps the tray. Teeth: the item opens Settings at whatever page it was on; the
    // controller's settings stay on General.
    void the_menus_controller_settings_open_the_controller_page()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Press(B::Start);
        QVERIFY(Sh.menuOpen());
        QCOMPARE(Sh.padState()["menu"]["items"][5].get<std::string>(), std::string("Controller settings"));
        Press(B::Up);      // no wrap: Up at the first item stays there
        QCOMPARE(Sh.padState()["menu"]["current"].get<std::string>(), std::string("Desktop mode"));
        Press(B::Down, 6);  // the last item: Quit, asked twice
        Press(B::A);
        QVERIFY(Sh.menuOpen());
        QCOMPARE(Sh.padState()["menu"]["current"].get<std::string>(), std::string("Press A again to quit"));
        Press(B::Up);      // moving off disarms it
        Press(B::Down);
        QCOMPARE(Sh.padState()["menu"]["current"].get<std::string>(), std::string("Quit VidyaGod"));
        Press(B::Up);
        Press(B::A);
        QVERIFY(!Sh.menuOpen());
        QCOMPARE(Sh.section(), DeckShell::Section::Settings);
        QCOMPARE(Sh.padState()["page"]["current"].get<std::string>(), std::string("controller.start"));
        //And its Advanced…: the desktop's Controller page, with its Start in.
        Sh.openAdvanced(DeckShell::Advanced::Settings, "Controller");
        auto *T = Sh.findChild<SettingsTab *>();
        QVERIFY(T);
        auto *Side = T->findChild<QListWidget *>("SettingsSidebar");
        QVERIFY(Side && Side->currentItem());
        QCOMPARE(Side->currentItem()->text(), QStringLiteral("Controller"));
        QComboBox *Start = nullptr;
        for (QComboBox *C : T->findChildren<QComboBox *>())
            if (C->accessibleName() == "Controller Start in") Start = C;
        QVERIFY(Start && Start->isVisible());
        Sh.setActive(false);
        GeneralPage G(M);
        for (QComboBox *C : G.findChildren<QComboBox *>()) QVERIFY(C->accessibleName() != "Controller Start in");
    }

    // A library refresh rebuilds the shelves and Home's rows, which show the shelves' own cards: the old cards go only
    // once nothing reads them. Freed memory is filled (malloc's perturb) so a read of an old card crashes here every
    // time rather than now and then. Teeth: a shelf deletes its old cards before Home has let go of them.
    void a_library_refresh_never_reads_a_card_it_let_go()
    {
        LibraryFixture L("6");
        InstanceStore::TouchLastRun(L.Cfg, "6");
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        QTRY_COMPARE_WITH_TIMEOUT(Sh.padState()["page"]["hero"].value("title", std::string()), std::string("Foxtrot"), 10000);
        auto *Recent = Sh.findChild<DeckGrid *>("controllerHomeRecent");
        QVERIFY(Recent && Recent->current());
        const QString Was = Recent->current()->GameKey;
        mallopt(M_PERTURB, 0xA5);
        const auto Restore = qScopeGuard([] { mallopt(M_PERTURB, 0); });
        emit M.catalogChanged();
        emit M.catalogChanged();
        QVERIFY(Recent->current());
        QCOMPARE(Recent->current()->GameKey, Was);   // still on the same game
        Sh.setActive(false);
    }

    // The launch window, opened in controller mode, is a screen of its own laid out for a controller: Play under the
    // game's name beside the cover, Start playing from anywhere, the shoulders turning its tabs, every option section
    // open and each option reachable with the d-pad, a choice turning with left and right. Teeth: the desktop layout
    // (Play in the footer, under the tabs); folded sections; an option taken for the tree's open editor (never
    // reached); the tab widget taken for a stop (focus lost to its tab bar); a choice that does not turn; the
    // shoulders not going round; the shoulders and Start doing nothing.
    void the_launch_window_is_a_screen_laid_out_for_a_controller()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        M.rebuildCatalog();
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);

        const auto Parse = [](const json &J) { Node N; ManifestModel::ParseNode(J, "f.json", "/tmp/vg_deck_launch", N); return N; };
        const json Choices = json::array({ "640x480", "800x600", "1024x768" });
        NodeIndex Idx;
        Idx.Nodes["game"] = Parse(json{ {"CID", "game"}, {"LABEL", "game"}, {"VARIANT", "v1"},
            {"LAYERS", json::array({ json{{"DIR", "game"}},
                json{{"VARS", {
                    {"lang", {{"DEFAULT", "English"}, {"UI", {{"CONTROL", "enum"}, {"LABEL", "Language"}, {"CHOICES", json::array({ "English", "French" })}}}}},
                    {"res", {{"DEFAULT", "640x480"}, {"UI", {{"CONTROL", "enum"}, {"SECTION", "Display"}, {"LABEL", "Resolution"}, {"CHOICES", Choices}}}}}}}},
                json{{"EXEC", json::array({ json{{"LABEL", "Play"}, {"HOST", ManifestModel::MachinePlatform()}, {"EXE", "game.exe"},
                    {"TILE", {{"UID", "37"}, {"TITLE", "Game"}}}} })}} })} });
        ManifestModel::DeriveFacts(Idx);
        json Cfg = json{{"Settings", json::object()}};
        auto *W = new PreLaunchWindow(&Cfg, &Idx, {"game"});
        const auto Close = qScopeGuard([&] { W->close(); QApplication::processEvents(); });
        Front(*W);
        QTRY_VERIFY(W->isActiveWindow());
        auto *Play = W->findChild<QPushButton *>("playButton");
        auto *Tabs = W->findChild<QTabWidget *>("prelaunchTabs");
        auto *Cover = W->findChild<QLabel *>("CoverLabel");
        QVERIFY(Play && Tabs && Cover && Play->isEnabled());
        const auto At = [W](QWidget *X) { return QRect(X->mapTo(W, QPoint(0, 0)), X->size()); };
        QVERIFY(At(Play).bottom() < At(Tabs).top());     // Play under the name, above the options
        QVERIFY(At(Play).left() > At(Cover).right());    // beside the cover
        QComboBox *Res = nullptr;
        for (QComboBox *C : W->findChildren<QComboBox *>()) if (C->property("CVKey").toString() == "res") Res = C;
        QVERIFY(Res && Res->isVisible());                 // its section open
        //Down, from Play, reaches the option in the section.
        Play->setFocus();
        for (int I = 0; I < 8 && QApplication::focusWidget() != Res; ++I) Press(B::Down);
        QVERIFY2(QApplication::focusWidget() == Res,
                 qPrintable(QString("focus on %1").arg(QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "nothing")));
        Press(B::Right);
        QCOMPARE(Res->currentText(), QStringLiteral("800x600"));
        Press(B::Left);
        QCOMPARE(Res->currentText(), QStringLiteral("640x480"));
        //The shoulders turn the tabs from anywhere; the hint bar says to which.
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()), QStringLiteral("Options"));
        Play->setFocus();
        bool Says = false;
        for (const PadTarget::Hint &H : PadRouter::instance().hints()) if (H.B == B::RB && H.Label == "Advanced") Says = true;
        QVERIFY(Says);
        Press(B::RB);
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()), QStringLiteral("Advanced"));
        Press(B::LB);
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()), QStringLiteral("Options"));
        //They go round: from the first tab, LB goes to the last, and says so.
        bool Round = false;
        for (const PadTarget::Hint &H : PadRouter::instance().hints()) if (H.B == B::LB && H.Label == Tabs->tabText(Tabs->count() - 1)) Round = true;
        QVERIFY(Round);
        Press(B::LB);
        QCOMPARE(Tabs->currentIndex(), Tabs->count() - 1);
        Press(B::RB);
        QCOMPARE(Tabs->tabText(Tabs->currentIndex()), QStringLiteral("Options"));
        //Start plays, wherever the focus is (the launch itself is not this test's: Play's own handler is let go).
        QObject::disconnect(Play, &QAbstractButton::clicked, W, nullptr);
        QSignalSpy Played(Play, &QAbstractButton::clicked);
        Res->setFocus();
        Press(B::Start);
        QCOMPARE(Played.count(), 1);
        //A game's run ends: the window reads its options back and builds them anew — still every section open.
        QVERIFY(QMetaObject::invokeMethod(W, "onLaunchFinished", Qt::DirectConnection, Q_ARG(bool, true), Q_ARG(QString, QString())));
        QApplication::processEvents();
        QComboBox *Rebuilt = nullptr;
        for (QComboBox *C : W->findChildren<QComboBox *>()) if (C->property("CVKey").toString() == "res") Rebuilt = C;
        QVERIFY(Rebuilt && Rebuilt != Res);
        QVERIFY(Rebuilt->isVisible());
        Sh.setActive(false);
    }

    // A command runs inside its socket's read handler and may spin an event loop of its own (a press letting the UI
    // settle, a dialog's exec): a client that hangs up meanwhile — an early answer told it all — must not have the
    // socket freed under that handler. The app keeps answering. Teeth: delete the socket the moment it disconnects
    // (the app crashes in QIODevice::channelReadyRead).
    void a_client_hanging_up_mid_command_does_not_take_the_app_down()
    {
        //A press that opens a dialog answers from inside its exec(); the client has what it wanted and hangs up; a
        //second command closes the dialog (`controller press b`); the first one's exec() returns, and it lets the UI
        //settle — the launch window's sequence the crash was seen in.
        static QPointer<QEventLoop> Open;
        static QList<QPointer<QLocalSocket>> Served;   // the app's end of each connection, newest last
        static bool FreedUnderIt = false;
        Command::Register({ "test hangup", "answers early from inside a dialog's loop", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &C, const json &) {
                const QPointer<QLocalSocket> Mine = Served.isEmpty() ? nullptr : Served.back();
                if (C.Early) C.Early(json{{"early", true}});
                QEventLoop Dialog;
                Open = &Dialog;
                Dialog.exec();
                QElapsedTimer T;
                T.start();
                do QApplication::processEvents(QEventLoop::AllEvents, 10); while (T.elapsed() < 100);
                //Still inside the socket's readyRead: freeing the socket now is a use-after-free once this returns
                //(a crash in QIODevice::channelReadyRead, when the freed memory has been reused).
                if (!Mine) FreedUnderIt = true;
                return json{{"late", true}};
            }, {} });
        Command::Register({ "test close", "closes that dialog, and lets the UI settle", "", {}, {}, {}, Command::Needs::Config, false,
            [](Command::Context &, const json &) {
                if (Open) Open->quit();
                QElapsedTimer T;
                T.start();
                do QApplication::processEvents(QEventLoop::AllEvents, 10); while (T.elapsed() < 100);
                return json{{"closed", true}};
            }, {} });
        QTemporaryDir Data;
        json Cfg = json::object();
        QDir AppDir(Data.path());
        Command::Context Ctx;
        Ctx.Config = &Cfg;
        Ctx.AppDataDir = &AppDir;
        CommandServer Server(Ctx);
        std::string Err;
        QVERIFY2(Server.Start(Data.path().toStdString(), &Err), Err.c_str());
        //The server takes each connection first (its own handler was connected first): its socket is there by now.
        auto *Listener = Server.findChild<QLocalServer *>();
        QVERIFY(Listener);
        connect(Listener, &QLocalServer::newConnection, Listener, [Listener] {
            for (QLocalSocket *S : Listener->findChildren<QLocalSocket *>())
                if (!Served.contains(S)) Served << S;
        });
        //The clients are another program's (here a thread's, on plain sockets): the app's event loop answers them.
        const std::string Path = CommandServer::SocketName(Data.path().toStdString());
        const auto Ask = [Path](const char *Name) {
            const int Fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un A{};
            A.sun_family = AF_UNIX;
            std::strncpy(A.sun_path, Path.c_str(), sizeof A.sun_path - 1);
            std::string Got;
            if (::connect(Fd, reinterpret_cast<sockaddr *>(&A), sizeof A) == 0)
            {
                const std::string Req = std::string("{\"name\": \"") + Name + "\", \"args\": {}}\n";
                (void)!::write(Fd, Req.data(), Req.size());
                char Buf[512];
                for (ssize_t N; Got.find('\n') == std::string::npos && (N = ::read(Fd, Buf, sizeof Buf)) > 0;) Got.append(Buf, N);
            }
            ::close(Fd);   // hung up the moment it has its answer
            return Got;
        };
        std::atomic<int> Rounds{0};
        std::atomic<bool> Answered{true};
        std::thread Clients([&] {
            for (int Round = 0; Round < 3; ++Round)
            {
                Answered = Answered && Ask("test hangup").find("early") != std::string::npos;
                std::this_thread::sleep_for(std::chrono::milliseconds(150));   // the app sees it gone, its dialog open
                Answered = Answered && Ask("test close").find("closed") != std::string::npos;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the first command settles and unwinds
                ++Rounds;
            }
        });
        QTRY_COMPARE_WITH_TIMEOUT(Rounds.load(), 3, 20000);
        Clients.join();
        QVERIFY(Answered);
        QVERIFY2(!FreedUnderIt, "a socket was freed under the read handler running its command");
    }

    // A key-binding field destroyed while it captures gives the controller queue back — no FocusOut reaches it then,
    // and navigation would stay dead. Teeth: give it back only on FocusOut.
    void a_capture_ended_by_its_field_going_gives_the_queue_back()
    {
        auto *F = new KeyCaptureField("a", "a");
        F->resize(300, 40);
        Front(*F);
        QTest::mouseClick(F->findChild<QPushButton *>("keycapture"), Qt::LeftButton);
        if (!PadInput::instance().capturing()) QSKIP("no SDL game-controller subsystem here: nothing is lent");
        delete F;
        QVERIFY(!PadInput::instance().capturing());
    }

    // Controller mode uses Qt's own file dialogs (a native one is another program's window: no controller reaches
    // it), and gives the desktop its own back. Teeth: never turn it on; leave it on.
    void controller_mode_uses_dialogs_a_controller_can_drive()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);   // as a desktop session has it
        Sh.setActive(true);
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
        Sh.setActive(false);
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
        //And the menu reaches the Library toolbar's two buttons.
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        Sh.openMenu();
        QVERIFY(Sh.menuOpen());
        const json Items = Sh.padState()["menu"]["items"];
        QVERIFY(std::find(Items.begin(), Items.end(), json("Add a local package…")) != Items.end());
        QVERIFY(std::find(Items.begin(), Items.end(), json("Package editor")) != Items.end());
        Sh.closeMenu();
        Sh.setActive(false);
    }

    // A dialog opened in controller mode is made large and gets the hint strip; a popup menu is left as it is.
    // Teeth: test the window flags with & Qt::Popup (every window matches, no dialog is styled).
    void dialogs_in_controller_mode_get_hints_and_popups_do_not()
    {
        LibraryFixture L;
        AppModel M(&L.Cfg, &L.AppDir);
        IpfsModel Ipfs(M);
        DownloadManager Dl(M, Ipfs);
        DeckShell Sh(M, Ipfs, Dl);
        Sh.resize(1280, 800);
        Front(Sh);
        Sh.setActive(true);
        QVERIFY(DeckShell::On());
        QDialog D;
        auto *Lay = new QVBoxLayout(&D);
        Lay->addWidget(new QPushButton("ok"));
        D.show();
        QApplication::processEvents();
        QVERIFY(D.findChild<QWidget *>("controllerHints"));
        QVERIFY(D.styleSheet().contains("font-size"));
        QMenu Menu;
        Menu.addAction("x");
        Menu.popup(QPoint(10, 10));
        QApplication::processEvents();
        QVERIFY(!Menu.findChild<QWidget *>("controllerHints"));
        Menu.close();
        Sh.setActive(false);
        QVERIFY(!DeckShell::On());
    }
};

QTEST_MAIN(ControllerTest)
#include "test_controller.moc"
