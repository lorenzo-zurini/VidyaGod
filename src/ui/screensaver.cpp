#include "screensaver.h"

#include "matrixrain.h"
#include "padinput.h"
#include "padnav.h"

#include <QApplication>
#include <QEvent>
#include <QScreen>
#include <QWidget>

// The rain as the controller's overlay while it shows: every press and stick ends it, and does nothing else.
class ScreenSaver::View : public MatrixRain, public PadTarget
{
public:
    View(QWidget *Parent, ScreenSaver &Owner) : MatrixRain(Parent), Owner(Owner) {}
    bool padPressed(Pad::Button) override { Owner.dismiss(); return true; }
    bool padScrolled(double, double) override { Owner.dismiss(); return true; }
    nlohmann::ordered_json padState() const override { return { {"screensaver", true} }; }

private:
    ScreenSaver &Owner;
};

ScreenSaver::ScreenSaver(QWidget *CoveredWidget, std::function<int()> SecondsFn, QObject *Parent)
    : QObject(Parent), Covered(CoveredWidget), Seconds(std::move(SecondsFn))
{
    Idle.setSingleShot(true);
    connect(&Idle, &QTimer::timeout, this, &ScreenSaver::showNow);
    //A window of its own over every other (the launch window, the on-screen keyboard, a dialog): the whole screen goes
    //dark, not only the shell under them. It takes no focus: the input that ends it is caught app-wide (eventFilter).
    Rain = new View(nullptr, *this);
    Rain->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    Rain->setAttribute(Qt::WA_ShowWithoutActivating);
    Rain->hide();
    if (CoveredWidget) CoveredWidget->installEventFilter(this);   // its size: the rain's
    qApp->installEventFilter(this);                                // every key, click, touch and move
    //A controller in use: the wait starts over. A press while the rain shows reaches it as the overlay (and ends it).
    connect(&PadInput::instance(), &PadInput::used, this, [this] { if (!showing()) activity(); });
    rearm();
}

ScreenSaver::~ScreenSaver()
{
    if (qApp) qApp->removeEventFilter(this);
    if (Rain) PadRouter::instance().popOverlay(Rain);
    delete Rain;   // a window of its own: no parent deletes it
}

//The screen the covered widget's window is on: what the rain covers.
static QRect ScreenOf(const QWidget *Covered)
{
    if (const QScreen *S = Covered->window()->screen()) return S->geometry();
    return Covered->window()->geometry();
}

bool ScreenSaver::showing() const { return Showing; }   // its own: the rain's visibility depends on its window's
MatrixRain *ScreenSaver::rain() const { return Rain; }

int ScreenSaver::idleRemainingMs() const { return Idle.isActive() ? Idle.remainingTime() : -1; }

void ScreenSaver::setSuspended(bool On)
{
    Suspended = On;
    if (On) { Idle.stop(); if (showing()) dismiss(); }
    else rearm();
}

void ScreenSaver::showNow()
{
    if (Suspended || !Covered || showing()) return;
    Idle.stop();
    Rain->setGeometry(ScreenOf(Covered));
    Rain->start();
    Showing = true;
    PadRouter::instance().pushOverlay(Rain);
    emit shown();
}

void ScreenSaver::dismiss()
{
    if (!showing()) { rearm(); return; }
    Showing = false;
    PadRouter::instance().popOverlay(Rain);
    Rain->stop();
    rearm();
    emit dismissed();
}

void ScreenSaver::activity()
{
    if (!showing()) rearm();
}

void ScreenSaver::rearm()
{
    Idle.stop();
    const int S = Seconds ? Seconds() : DefaultSeconds;
    if (Suspended || S <= 0) return;
    Idle.start(S * 1000);
}

bool ScreenSaver::eventFilter(QObject *Watched, QEvent *E)
{
    if (Watched == Covered && (E->type() == QEvent::Resize || E->type() == QEvent::Move) && showing()) Rain->setGeometry(ScreenOf(Covered));
    if (!E->spontaneous()) return false;   // a person's input only: Qt's own moves (a widget shown under the pointer) are not
    switch (E->type())
    {
    case QEvent::KeyPress: case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: case QEvent::TouchBegin:
    case QEvent::Wheel: case QEvent::TabletPress:
        if (showing()) { dismiss(); return true; }   // the input that ends it does nothing else
        activity();
        break;
    case QEvent::KeyRelease: case QEvent::MouseButtonRelease: case QEvent::TouchEnd:
        if (showing()) return true;
        break;
    case QEvent::MouseMove: case QEvent::TouchUpdate:
        if (showing()) dismiss(); else activity();
        break;
    default: break;
    }
    return false;
}
