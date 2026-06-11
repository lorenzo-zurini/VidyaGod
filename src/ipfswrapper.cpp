#include "ipfswrapper.h"
#include "commonutils.h"

#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QCoreApplication>

#include <utility>

namespace IpfsWrapper {

//Optional sink for transfer lifecycle events (installed by IpfsManager). Invoked on whatever thread the
//fetch runs on — the installed callback is responsible for marshalling to the GUI thread.
static TransferCallback g_TransferCb;
void SetTransferCallback(TransferCallback Callback) { g_TransferCb = std::move(Callback); }
static void Emit(const TransferEvent &E) { if (g_TransferCb) g_TransferCb(E); }

// ~/.VidyaGod/DOWNLOADS/ipfs — the CID cache root (mirrors DownloadsDir() in containerwrapper.cpp).
static std::string IpfsCacheRoot()
{
    return QDir::cleanPath(QDir::homePath() + "/.VidyaGod/DOWNLOADS/ipfs").toStdString();
}

// Runs `ipfs` with the given args, blocking up to TimeoutMs (-1 = wait indefinitely). Captures stdout
// into *Out when provided. Returns true on a clean (exit-0) finish.
static bool RunIpfs(const QStringList &Args, QString *Out = nullptr, int TimeoutMs = 120000)
{
    QProcess P;
    P.start("ipfs", Args);
    if (!P.waitForStarted(10000)) return false;
    if (!P.waitForFinished(TimeoutMs)) { P.kill(); P.waitForFinished(2000); return false; }
    if (Out) *Out = QString::fromUtf8(P.readAllStandardOutput());
    return P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0;
}

bool Available()
{
    return !QStandardPaths::findExecutable("ipfs").isEmpty();
}

bool DaemonRunning()
{
    // `ipfs swarm peers` only succeeds in online mode (a running daemon).
    if (!Available()) return false;
    return RunIpfs({"swarm", "peers"}, nullptr, 10000);
}

std::string CachePath(const std::string &Cid)
{
    if (Cid.empty()) return std::string();
    return QDir::cleanPath(QString::fromStdString(IpfsCacheRoot() + "/" + Cid)).toStdString();
}

// The completion marker sits beside the cache entry (which may be a file or a directory).
static std::string CompleteMarker(const std::string &Cid)
{
    const std::string P = CachePath(Cid);
    return P.empty() ? std::string() : (P + ".complete");
}

bool IsCached(const std::string &Cid)
{
    const std::string P = CachePath(Cid), M = CompleteMarker(Cid);
    return !P.empty() && QFileInfo::exists(QString::fromStdString(P))
                      && QFileInfo::exists(QString::fromStdString(M));
}

// Runs `ipfs get <Cid> -o <Dest>`, pumping stderr to report the latest progress percentage through Emit().
// Long-but-finite overall timeout: a several-hundred-MB build can take a while, but a stuck DHT lookup must
// not hang the launch forever.
static bool RunIpfsGet(const std::string &Cid, const QString &Dest)
{
    QProcess P;
    P.start("ipfs", {"get", QString::fromStdString(Cid), "-o", Dest});
    if (!P.waitForStarted(10000)) return false;

    static const QRegularExpression PctRe(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)%"));
    QElapsedTimer Timer; Timer.start();
    while (P.state() != QProcess::NotRunning)
    {
        P.waitForReadyRead(500);
        const QString Err = QString::fromUtf8(P.readAllStandardError());
        if (!Err.isEmpty())
        {
            double Pct = -1.0;                                   // Kubo prints a CR progress bar; take the last %
            auto It = PctRe.globalMatch(Err);
            while (It.hasNext()) Pct = It.next().captured(1).toDouble();
            if (Pct >= 0.0) Emit({ TransferEvent::Progress, Cid, Pct, false });
        }
        if (Timer.hasExpired(1800000)) { P.kill(); P.waitForFinished(2000); return false; }
    }
    P.waitForFinished(2000);
    return P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0;
}

std::string FetchSync(const std::string &Cid, std::string *Error)
{
    auto Fail = [&](const std::string &Msg) -> std::string {
        if (Error) *Error = Msg;
        LogErr("IpfsWrapper::FetchSync", Msg);
        Emit({ TransferEvent::Finished, Cid, -1.0, false });
        return std::string();
    };

    if (Cid.empty())     return Fail("empty CID");
    if (!Available())    return Fail("Kubo (ipfs) is not installed — cannot fetch CID " + Cid);

    const std::string Dest = CachePath(Cid);
    if (IsCached(Cid)) { LogOut("IpfsWrapper::FetchSync", "CID already cached: " + Cid); return Dest; }

    QDir().mkpath(QString::fromStdString(IpfsCacheRoot()));
    const QString TmpDest = QString::fromStdString(Dest) + ".tmp";
    QDir(TmpDest).removeRecursively();                 // clear any partial previous attempt
    QFile::remove(TmpDest);                             // (in case the entry was a file)

    LogOut("IpfsWrapper::FetchSync", "Fetching CID " + Cid + " -> " + Dest);
    Emit({ TransferEvent::Started, Cid, -1.0, false });
    if (!RunIpfsGet(Cid, TmpDest))
    {
        QDir(TmpDest).removeRecursively();
        QFile::remove(TmpDest);
        return Fail("`ipfs get` failed for CID " + Cid + " (is the daemon running / CID reachable?)");
    }

    // Atomically publish the completed fetch, then drop the completion marker.
    QFile::remove(QString::fromStdString(Dest));
    QDir(QString::fromStdString(Dest)).removeRecursively();
    if (!QDir().rename(TmpDest, QString::fromStdString(Dest)))
        return Fail("failed to move fetched CID into place: " + Dest);

    QFile Marker(QString::fromStdString(CompleteMarker(Cid)));
    if (Marker.open(QIODevice::WriteOnly)) Marker.close();

    // Pin so the content keeps seeding (best-effort — a missing daemon just means it isn't served yet).
    if (!RunIpfs({"pin", "add", QString::fromStdString(Cid)}, nullptr, 60000))
        LogWarn("IpfsWrapper::FetchSync", "could not pin CID " + Cid + " (start `ipfs daemon` to seed it).");

    LogSucc("IpfsWrapper::FetchSync", "Fetched + pinned CID " + Cid);
    Emit({ TransferEvent::Finished, Cid, 100.0, true });
    return Dest;
}

int PeerCount()
{
    QString Out;
    if (!RunIpfs({"swarm", "peers"}, &Out, 10000)) return 0;
    const QStringList Lines = Out.split('\n', Qt::SkipEmptyParts);
    return Lines.size();
}

std::string RepoSizeHuman()
{
    QString Out;
    if (!RunIpfs({"repo", "stat", "--human"}, &Out, 15000)) return std::string();
    for (const QString &Line : Out.split('\n', Qt::SkipEmptyParts))
        if (Line.startsWith("RepoSize:"))
            return Line.mid(QString("RepoSize:").size()).trimmed().toStdString();
    return std::string();
}

std::vector<PinEntry> Pins()
{
    std::vector<PinEntry> Result;
    QString Out;
    if (!RunIpfs({"pin", "ls", "--type=recursive"}, &Out, 30000)) return Result;
    for (const QString &Line : Out.split('\n', Qt::SkipEmptyParts))
    {
        const QString Cid = Line.section(' ', 0, 0).trimmed();   // "<cid> recursive"
        if (!Cid.isEmpty()) Result.push_back({ Cid.toStdString() });
    }
    return Result;
}

bool Unpin(const std::string &Cid)
{
    if (Cid.empty() || !Available()) return false;
    return RunIpfs({"pin", "rm", QString::fromStdString(Cid)}, nullptr, 30000);
}

} // namespace IpfsWrapper

// ---------------------------------------------------------------------------
// IpfsManager
// ---------------------------------------------------------------------------
IpfsManager::IpfsManager(QObject * parent) : QObject(parent)
{
    //Relay backend transfer events (which may fire on a launch-worker thread) onto this object's thread
    //(the GUI thread) as Qt signals, via a queued invocation.
    IpfsWrapper::SetTransferCallback([this](const IpfsWrapper::TransferEvent &E) {
        const QString Cid = QString::fromStdString(E.Cid);
        switch (E.Kind)
        {
        case IpfsWrapper::TransferEvent::Started:
            QMetaObject::invokeMethod(this, [this, Cid]{ emit transferStarted(Cid); }, Qt::QueuedConnection);
            break;
        case IpfsWrapper::TransferEvent::Progress: {
            const double Pct = E.Percent;
            QMetaObject::invokeMethod(this, [this, Cid, Pct]{ emit transferProgress(Cid, Pct); }, Qt::QueuedConnection);
            break; }
        case IpfsWrapper::TransferEvent::Finished: {
            const bool Ok = E.Ok;
            QMetaObject::invokeMethod(this, [this, Cid, Ok]{ emit transferFinished(Cid, Ok); }, Qt::QueuedConnection);
            break; }
        }
    });
}

IpfsManager * IpfsManager::instance()
{
    //Parented to the application so it outlives any tab rebuild and is created on the GUI thread.
    static IpfsManager * Inst = new IpfsManager(qApp);
    return Inst;
}
