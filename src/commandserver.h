#ifndef COMMANDSERVER_H
#define COMMANDSERVER_H

#include "command.h"
#include <QObject>
#include <string>

class QLocalServer;
class QLocalSocket;

// ---------------------------------------------------------------------------
// CommandServer — the running app's end of `VidyaGod --call`. With the GUI up, the data dir's single-instance lock
// keeps every other VidyaGod out; the command socket beside the lock lets a CLI call in instead: one JSON line
// {"name": …, "args": {…}} in, one JSON line (Command::Run's reply) out, run on the GUI thread against the live
// model and windows. A command that opens a modal dialog keeps the socket served (the dialog's own event loop), so
// a second call can read the dialog and answer it.
// ---------------------------------------------------------------------------
class CommandServer : public QObject
{
    Q_OBJECT
public:
    CommandServer(Command::Context Ctx, QObject *Parent = nullptr);
    ~CommandServer() override;
    // Listen at the data dir's command socket. False (with *Error) when it cannot.
    bool Start(const std::string &AppDataDir, std::string *Error = nullptr);

    // The socket name for a data dir: a path beside the lock on Unix, a pipe name derived from it on Windows.
    static std::string SocketName(const std::string &AppDataDir);
    // The CLI side: send one call to the app running on AppDataDir. False when none answers (then run it here).
    static bool Send(const std::string &AppDataDir, const std::string &Name, const nlohmann::ordered_json &Args,
                     nlohmann::ordered_json &Reply, int TimeoutMs = -1);

private:
    void Serve(QLocalSocket *S);
    Command::Context Ctx;
    QLocalServer *Server = nullptr;
};

#endif // COMMANDSERVER_H
