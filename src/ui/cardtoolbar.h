#ifndef CARDTOOLBAR_H
#define CARDTOOLBAR_H

#include <QSet>
#include <QString>

#include <functional>
#include <utility>
#include <vector>

#include "libraryview.h"   // SortMode

class AppModel;
class QHBoxLayout;
class QLabel;
class QWidget;

// What the Library and the Catalog tabs share above their card grids: the sort buttons, an optional status line, the
// title search, the size picker and a separator — the tab adds its own buttons to Row after it. The size buttons set
// the model's card width (cardSizeChanged then reaches every tab; SyncSizeButtons re-checks the right one).
struct CardToolbar
{
    QWidget     *Bar = nullptr;
    QHBoxLayout *Row = nullptr;
    QLabel      *Status = nullptr;   // null unless asked for
};
CardToolbar MakeCardToolbar(QWidget *Owner, AppModel &Model, const std::vector<std::pair<QString, SortMode>> &Sorts,
                            SortMode Current, std::function<void(SortMode)> OnSort,
                            std::function<void(const QString &)> OnSearch, const QString &StatusName = QString());
void SyncSizeButtons(QWidget *Owner, int CardWidth);

// The collapsed sections a grid remembers, under Settings[Key]: read once, written on every toggle.
QSet<QString> LoadCollapsed(AppModel &Model, const char *Key);
void SaveCollapsed(AppModel &Model, const char *Key, const QSet<QString> &Collapsed);

#endif // CARDTOOLBAR_H
