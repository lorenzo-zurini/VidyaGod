#ifndef EVENTLOG_H
#define EVENTLOG_H

#include <nlohmann/json.hpp>
#include <QObject>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// EventLog — every signal the app's own objects emit, recorded with its arguments: what `VidyaGod events` lists and
// follows. A signal is how the app says something happened (a download finished, a friend's library arrived, the
// catalog changed); with this the CLI sees each one as it happens, in order, without a debugger.
//
// Attach(Obj) records every signal Obj's own classes declare (not Qt's: QObject::destroyed, QWidget's window-title
// changes); AttachTree also does every descendant. The last 5000 events are kept, each with a sequence number.
// ---------------------------------------------------------------------------
namespace EventLog
{

void Attach(QObject *Obj, const QString &Name = QString());
void AttachTree(QObject *Root);

// Events after Since (0: from the oldest kept), optionally only from objects/signals containing the filters.
nlohmann::ordered_json Since(std::uint64_t Since, const std::string &ObjectFilter = {}, const std::string &SignalFilter = {},
                             std::size_t Limit = 1000);
// The sequence number of the newest event (0: none yet).
std::uint64_t Latest();

} // namespace EventLog

#endif // EVENTLOG_H
