#ifndef OSK_H
#define OSK_H

#include "padnav.h"   // PadTarget

#include <QPointer>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

// ---------------------------------------------------------------------------
// OnScreenKeyboard — typing with a controller: a keyboard drawn over the window, its keys reached with the d-pad and
// pressed with A (or tapped on a touch screen, or typed on a real keyboard). X deletes, Y is a space, the bumpers move
// the cursor, the left trigger is Shift, the right trigger the symbols, Start is Done and B leaves things as they were.
// ---------------------------------------------------------------------------
class OnScreenKeyboard : public QWidget, public PadTarget
{
    Q_OBJECT
public:
    // Type into Field — a line edit, a spin box, a plain or rich text edit — starting from its text. Done writes the
    // text back as typing would (a line edit's textEdited, then Return: whatever an edit finishing does, happens);
    // B leaves the field as it was.
    static OnScreenKeyboard *Open(QWidget *Field);
    // Ask for text with no field. Done calls OnDone; every change calls OnEdit (search as you type); B calls neither.
    static OnScreenKeyboard *Ask(QWidget *Window, const QString &Title, const QString &Text,
                                 std::function<void(const QString &)> OnDone,
                                 std::function<void(const QString &)> OnEdit = {});
    static OnScreenKeyboard *current();   // the one open, if any

    QString text() const { return Text; }
    // Type these characters as if each key were pressed (the CLI's `controller type`).
    void typeText(const QString &S);
    void accept();   // Done
    void cancel();   // B

    bool padPressed(Pad::Button B) override;
    QList<Hint> padHints() const override;
    nlohmann::ordered_json padState() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *E) override;
    void keyPressEvent(QKeyEvent *E) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *Watched, QEvent *Event) override;

private:
    OnScreenKeyboard(QWidget *Window, const QString &Title, const QString &Text);
    enum class Kind { Char, Shift, Symbols, Space, Backspace, Left, Right, Done };
    struct Key { QString Label, Out; Kind K = Kind::Char; double Units = 1; QRectF Rect; };
    void build();                 // the keys of the page shown (letters or symbols), laid out
    void press(const Key &K);
    void insert(const QString &S);
    void backspace();
    void edited();
    void finish(bool Accepted);
    void moveFocus(Pad::Button Dir);
    QRectF panel() const;

    QString Title, Text, Original;
    int Cursor = 0;
    bool Shift = false, Symbols = false, Closing = false;
    QVector<Key> Keys;
    int Focus = 0;                // the key the controller is on
    QPointer<QWidget> Field;      // what Done writes into (null: Ask)
    QPointer<QWidget> Before;     // what had the focus when it opened: it gets it back
    std::function<void(const QString &)> OnDone, OnEdit;
};

#endif // OSK_H
