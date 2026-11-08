#ifndef DOWNLOADDIALOG_H
#define DOWNLOADDIALOG_H


#include <map>
#include <optional>
#include <string>
#include <vector>

class AppModel;
class DownloadManager;
class LibraryGameCard;
class QWidget;

//What the download dialog picked: the variants to fetch (the ticked content, the package's other games, the graft a
//card presents), the runners to install with them, and the optional content's ticks (graft key → ticked).
struct DownloadSelection
{
    std::vector<std::string> LaunchIds, RunnerIds;
    std::map<std::string, bool> Toggles;
};

//The per-package download dialog, modal: what a card's download takes — its content, other games, optional content,
//runners — with the selection's size against the free space. Opens at once; everything it derives (endpoints,
//optional content, sizes) is walked off the GUI thread. Nothing: cancelled, or nothing picked.
std::optional<DownloadSelection> AskDownload(AppModel &Model, const LibraryGameCard &Card, QWidget *Parent);

//A card's Download: the node asked to be ready first, then the dialog, then the download — none while one is in flight.
void StartCardDownload(DownloadManager &Downloads, AppModel &Model, LibraryGameCard *Card, QWidget *Parent);
//A card's Cancel: asked first, then what the download alone needs is aborted.
void CancelCardDownload(DownloadManager &Downloads, LibraryGameCard *Card, QWidget *Parent);

#endif // DOWNLOADDIALOG_H
