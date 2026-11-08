#ifndef PALETTE_H
#define PALETTE_H

#include <QString>

// The app's colours by name (the look itself is Theme, in ui/theme.h): what every surface — the editor's included —
// writes into a style sheet or a QColor instead of a hex literal of its own.
namespace Theme
{
// The named colours every surface draws with: secondary text, plain text, the accent, and the three states.
inline const QString Dim    = QStringLiteral("#8f98a0");   // secondary text: hints, captions, a status line
inline const QString Text   = QStringLiteral("#c6d4df");   // primary text on the navy ground
inline const QString Accent = QStringLiteral("#4a90d9");   // the selection / current choice
inline const QString Ok     = QStringLiteral("#5fb55f");   // ready, installed, healthy
inline const QString Warn   = QStringLiteral("#c6a15f");   // needs attention
inline const QString Bad    = QStringLiteral("#c0726a");   // failed, refused, too little space
}

#endif // PALETTE_H
