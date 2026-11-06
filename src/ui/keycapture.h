#ifndef KEYCAPTURE_H
#define KEYCAPTURE_H

#include <QWidget>

#include <string>

class QKeyEvent;
class QPushButton;
class QToolButton;
class QTimer;

// KeyCapture — the launch window's control for a `key` option (spec 08 §8.3): a button showing the bound key;
// click it, then press a key, a mouse button over it, or a gamepad button to bind that. Focus leaving it cancels;
// × unbinds; ↺ puts back the option's default, which the button names while the binding differs from it.
namespace KeyCapture
{
//The neutral key name of a key press ("" when it is no key a `key` option can hold). The key cap the user's layout
//shows; Shift/Ctrl/Alt/Super sides from the native key (X11/Wayland keysym, Windows VK + scancode).
std::string NameOf(const QKeyEvent &E);
//The neutral name of a mouse button ("" for none).
std::string NameOf(Qt::MouseButton B);
//The neutral name of a turn of the wheel by DeltaY (QWheelEvent::angleDelta().y(): away from the user is up); "" for none.
std::string NameOfWheel(int DeltaY);
//The neutral name of an SDL game-controller button (SDL_GameControllerButton), or of an axis (SDL_GameControllerAxis)
//pushed to Value: a trigger, or a stick's direction by its sign (SDL's Y grows downward); "" for anything else.
//A/B/X/Y are positions (the Xbox layout), as SDL reports them.
std::string NameOfPadButton(int SdlButton);
std::string NameOfPadAxis(int SdlAxis, int Value);
}

class KeyCaptureField : public QWidget
{
    Q_OBJECT
public:
    KeyCaptureField(const std::string &Value, const std::string &Default, QWidget *Parent = nullptr);
    std::string value() const { return Value; }
    void setValue(const std::string &V);
    bool capturing() const { return Capturing; }
    QPushButton *button() const { return Button; }
    QToolButton *clearButton() const { return Clear; }
    QToolButton *resetButton() const { return Reset; }
    //Marks the binding as clashing with another option ("" clears the mark).
    void setConflict(const QString &With);

    bool eventFilter(QObject *Watched, QEvent *Ev) override;

signals:
    void changed();

private:
    void startCapture();
    void stopCapture();
    void refresh();

    std::string Value;
    std::string Default;
    bool Capturing = false;
    QString ConflictWith;
    QPushButton *Button = nullptr;
    QToolButton *Clear = nullptr;
    QToolButton *Reset = nullptr;
    QTimer *PadPoll = nullptr;     // while capturing: reads the gamepads' new presses
};

#endif // KEYCAPTURE_H
