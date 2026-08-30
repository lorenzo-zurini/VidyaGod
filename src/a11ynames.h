#ifndef A11YNAMES_H
#define A11YNAMES_H

#include <QObject>
class QWidget;

// ---------------------------------------------------------------------------
// A11yNames — make every control in the GUI addressable BY NAME, both to
// assistive technology (AT-SPI: screen readers, tools/a11ydrive.py) and to the
// widget tests (findChild<T*>("name")).
//
// Doing this by hand at ~164 widget construction sites would be enormous and
// would rot the moment someone adds a button. Instead the names are DERIVED
// from what the widget already shows the user — its text, its form-layout
// label, its placeholder — and applied automatically when each widget is
// polished, which also covers widgets built dynamically at runtime (the
// Sources page rebuilds its cards on every change).
//
// An explicitly-set accessibleName/objectName is never overwritten: hand-naming
// stays authoritative where a human picked something better.
// ---------------------------------------------------------------------------
namespace A11yNames {

// Derive + set accessibleName (and objectName when empty) for W itself.
void ApplyToWidget(QWidget *W);

// Same, for W and every descendant. Use after building a page in one go.
void ApplyToTree(QWidget *Root);

// Install the app-wide auto-namer: every widget gets named as it is polished.
// Call once, right after the QApplication exists and before the main window.
void Install(QObject *App);

}   // namespace A11yNames

#endif // A11YNAMES_H
