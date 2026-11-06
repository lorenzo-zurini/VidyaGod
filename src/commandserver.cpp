#include "commandserver.h"
#include "commonutils.h"
#include "appmodel.h"     // catalogSnapshot — the index a Background command reads
#include "asyncwork.h"
#include "platform/platform.h"   // ReleaseFreedMemory — after a long command

#include <QCryptographicHash>
#include <QDir>
#include <QPointer>
#include <QLocalServer>
#include <QLocalSocket>
#include <filesystem>
#include <thread>

CommandServer::CommandServer(Command::Context C, QObject *Parent) : QObject(Parent), Ctx(C) { Ctx.Live = true; }

CommandServer::~CommandServer()
{
    if (Server) Server->close();
}

std::string CommandServer::SocketName(const std::string &AppDataDir)
{
#ifdef _WIN32
    //A pipe name, unique per data dir: two VidyaGods on two data dirs each answer their own.
    const QByteArray H = QCryptographicHash::hash(QDir(QString::fromStdString(AppDataDir)).absolutePath().toUtf8(),
                                                  QCryptographicHash::Sha256).toHex().left(16);
    return "vidyagod-" + H.toStdString();
#else
    return (std::filesystem::path(QDir(QString::fromStdString(AppDataDir)).absolutePath().toStdString()) / "vidyagod.sock").string();
#endif
}

bool CommandServer::Start(const std::string &AppDataDir, std::string *Error)
{
    const QString Name = QString::fromStdString(SocketName(AppDataDir));
    QLocalServer::removeServer(Name);   // we hold the data dir's lock: a socket file there is a dead run's
    Server = new QLocalServer(this);
    Server->setSocketOptions(QLocalServer::UserAccessOption);   // this user only: the socket drives the whole app
    if (!Server->listen(Name))
    {
        if (Error) *Error = Server->errorString().toStdString();
        return false;
    }
    connect(Server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *S = Server->nextPendingConnection()) Serve(S);
    });
    LogOut("CommandServer", "answering commands at " + Name.toStdString());
    return true;
}

void CommandServer::Serve(QLocalSocket *S)
{
    auto Buffer = std::make_shared<QByteArray>();
    connect(S, &QLocalSocket::disconnected, S, &QObject::deleteLater);
    connect(S, &QLocalSocket::readyRead, this, [this, S, Buffer] {
        Buffer->append(S->readAll());
        const qsizetype Nl = Buffer->indexOf('\n');
        if (Nl < 0) return;
        const nlohmann::ordered_json Req = nlohmann::ordered_json::parse(Buffer->left(Nl).toStdString(), nullptr, false);
        const auto Answer = [](QLocalSocket *To, const nlohmann::ordered_json &Reply) {
            To->write(QByteArray::fromStdString(Reply.dump() + "\n"));
            To->flush();
            To->disconnectFromServer();
        };
        if (Req.is_discarded() || !Req.is_object() || !Req.contains("name") || !Req["name"].is_string())
        {
            Answer(S, {{"ok", false}, {"error", "a request is one JSON line: {\"name\": …, \"args\": {…}}"}, {"usage", true}});
            return;
        }
        const std::string Name = Req["name"].get<std::string>();
        const nlohmann::ordered_json Args = Req.value("args", nlohmann::ordered_json::object());
        const Command::Spec *Spec = Command::Find(Name);
        if (Spec && Spec->Background && Ctx.Model)
        {
            //Off this thread, on the index as it is now: the window keeps painting and other commands keep answering.
            auto Bg = std::make_shared<Command::Context>(Ctx);
            Bg->Snapshot = Ctx.Model->catalogSnapshot();
            auto Reply = std::make_shared<nlohmann::ordered_json>();
            QPointer<QLocalSocket> To(S);
            AsyncWork::Run(this, [Bg, Reply, Name, Args] { *Reply = Command::Run(Name, Args, *Bg); },
                           [To, Reply, Answer] {
                               if (To) Answer(To, *Reply);
                               std::thread([] { Platform::ReleaseFreedMemory(); }).detach();   // off the GUI thread
                           });
            return;
        }
        Answer(S, Command::Run(Name, Args, Ctx));
    });
}

bool CommandServer::Send(const std::string &AppDataDir, const std::string &Name, const nlohmann::ordered_json &Args,
                         nlohmann::ordered_json &Reply, int TimeoutMs)
{
    QLocalSocket S;
    S.connectToServer(QString::fromStdString(SocketName(AppDataDir)));
    if (!S.waitForConnected(1500)) return false;   // nothing answers: no app up (or one from before the socket)
    const nlohmann::ordered_json Req = {{"name", Name}, {"args", Args}};
    S.write(QByteArray::fromStdString(Req.dump() + "\n"));
    if (!S.waitForBytesWritten(5000)) return false;
    QByteArray In;
    const int Budget = TimeoutMs < 0 ? 600000 : TimeoutMs;
    while (!In.contains('\n'))
    {
        if (!S.waitForReadyRead(Budget))
        {
            if (S.state() != QLocalSocket::ConnectedState && In.isEmpty()) return false;
            break;
        }
        In.append(S.readAll());
    }
    Reply = nlohmann::ordered_json::parse(In.left(In.indexOf('\n')).toStdString(), nullptr, false);
    if (Reply.is_discarded()) Reply = {{"ok", false}, {"error", "the app's reply was not readable"}};
    return true;
}
