#pragma once

#include "controllerlayout.h"

#include <QDialog>
#include <QString>
#include <QWidget>

class QComboBox;
class QFormLayout;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

// A drawn Steam Deck: every control a layout can bind is a hotspot (clicking one selects it), the selected one ringed.
// Not a focus stop for a controller: the list beside it is what a controller walks.
class ControllerPicture : public QWidget
{
    Q_OBJECT
public:
    explicit ControllerPicture(QWidget *Parent = nullptr);
    void setSelected(const QString &ControlId);
    void setChanged(const QStringList &ControlIds);   // the controls the layout changes: outlined
    QString selected() const { return Selected; }
    // Where a control is drawn (its hotspot), in this widget's coordinates; empty when it is not drawn.
    QRectF hotspot(const QString &ControlId) const;
    QSize sizeHint() const override { return QSize(640, 300); }
signals:
    void controlClicked(const QString &ControlId);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
private:
    QString Selected;
    QStringList Changed;
};

// The controller layout configurator (Steam Input's): pick a layout — VidyaGod's own or one of the user's — see what
// every control does, change any of them, save it under a name, try it on the controller now, and choose it for a game.
// Only what InputPlumber can do is offered (ControllerLayout). Opened from a game (its launch window, its page in
// controller mode, the overlay) it offers "Use for this game"; always "Use for VidyaGod" (VidyaGod's own layout).
class LayoutEditor : public QDialog
{
    Q_OBJECT
public:
    // Dir: where the user's layouts live (<data dir>/ControllerLayouts). GameKey/GameTitle: the game it was opened for.
    LayoutEditor(const QString &Dir, const QString &GameKey = {}, const QString &GameTitle = {}, QWidget *Parent = nullptr);

    // What the editor holds now (the layout being edited, unsaved changes included) and whether it changed.
    const ControllerLayout::Layout &current() const { return Current; }
    bool dirty() const { return Dirty; }
    // Open a layout by name (a built-in or a saved one); false when there is none.
    bool openLayout(const QString &Name);
    // Select a control (as clicking it in the picture or the list does).
    bool selectControl(const QString &ControlId);
    QString selectedControl() const;
    // Bind the selected control (as the controls below the list do); false with the reason when it cannot be.
    bool bindSelected(const ControllerLayout::Binding &B, QString *Error = nullptr);
    // Save under Name (the name field's): false with the reason (a built-in's name, a bad name).
    bool saveAs(const QString &Name, QString *Error = nullptr);
    // Use the saved layout for the game the editor was opened for.
    bool useForGame(QString *Error = nullptr);
    // Load it into the controller service now ("Try now"); VidyaGod's own is back when the editor closes.
    bool tryNow(QString *Error = nullptr);
    // Make the saved layout VidyaGod's own ("Use for VidyaGod"): what the controller does in VidyaGod itself (a game has
    // its own, or the controller as it is). Loaded at once.
    bool useForApp(QString *Error = nullptr);
    void done(int R) override;
    QString status() const;   // the status line: saved, used, refused…

private:
    void buildUi();
    void refillLayouts(const QString &Select);
    void refreshTree();
    void showBinding();               // the selected control's binding, in the editor below the list
    void applyFromWidgets();          // the editor's widgets → the selected control's binding
    void setStatus(const QString &S, bool Bad = false);
    QTreeWidgetItem *itemFor(const QString &ControlId) const;

    QString Dir, GameKey, GameTitle;
    ControllerLayout::Layout Current;
    bool Dirty = false, Filling = false, Tried = false;

    QComboBox *LayoutCombo = nullptr;
    QLabel *Description = nullptr, *Status = nullptr, *UsedBy = nullptr;
    ControllerPicture *Picture = nullptr;
    QTreeWidget *Tree = nullptr;
    QLabel *ControlTitle = nullptr;
    QStackedWidget *Editors = nullptr;      // 0: button/trigger, 1: stick, 2: a note (trackpad, gyro)
    QFormLayout *ButtonForm = nullptr;      // page 0's rows (the second action's show only with "Also press")
    QComboBox *TypeCombo = nullptr, *ValueCombo = nullptr, *Type2Combo = nullptr, *Value2Combo = nullptr;
    QCheckBox *Chord = nullptr;
    QComboBox *StickMode = nullptr;
    QSlider *Speed = nullptr;
    QLabel *SpeedLabel = nullptr;
    QComboBox *StickKeys[4] = {};
    QLabel *Note = nullptr;
    QLineEdit *NameEdit = nullptr;
    QPushButton *SaveButton = nullptr, *TryButton = nullptr, *UseButton = nullptr, *AppButton = nullptr, *DeleteButton = nullptr;
};
