#include "ipfswrapper.h"
#include "commonutils.h"
#include "processenv.h"

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
#include <set>
#include <mutex>

namespace IpfsWrapper {

//Optional sink for transfer lifecycle events (installed by IpfsManager). Invoked on whatever thread the
//fetch runs on — the installed callback is responsible for marshalling to the GUI thread.
static TransferCallback g_TransferCb;
void SetTransferCallback(TransferCallback Callback) { g_TransferCb = std::move(Callback); }
static void Emit(const TransferEvent &E) { if (g_TransferCb) g_TransferCb(E); }

//Cancellation: CIDs marked here make an in-flight FetchToPath abort at its next checkpoint (and kill `ipfs get`).
static std::set<std::string> g_Cancel;
static std::mutex            g_CancelMx;
static bool IsCancelled(const std::string &Cid) { std::lock_guard<std::mutex> L(g_CancelMx); return g_Cancel.count(Cid) > 0; }
void RequestCancel(const std::string &Cid) { std::lock_guard<std::mutex> L(g_CancelMx); g_Cancel.insert(Cid); }
void ClearCancel(const std::string &Cid)   { std::lock_guard<std::mutex> L(g_CancelMx); g_Cancel.erase(Cid); }

// Runs `ipfs` with the given args, blocking up to TimeoutMs (-1 = wait indefinitely). Captures stdout
// into *Out when provided. Returns true on a clean (exit-0) finish.
static bool RunIpfs(const QStringList &Args, QString *Out = nullptr, int TimeoutMs = 120000)
{
    QProcess P;
    P.setProcessEnvironment(SystemToolEnv());                                   // system ipfs must not inherit the AppImage LD_LIBRARY_PATH
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

// Runs `ipfs get <Cid> -o <Dest>`, pumping stderr to report the latest progress percentage through Emit().
// Long-but-finite overall timeout: a several-hundred-MB build can take a while, but a stuck DHT lookup must
// not hang the launch forever.
static bool RunIpfsGet(const std::string &Cid, const QString &Dest)
{
    QProcess P;
    P.setProcessEnvironment(SystemToolEnv());
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
        if (IsCancelled(Cid))          { P.kill(); P.waitForFinished(2000); return false; }   // user cancelled
        if (Timer.hasExpired(1800000)) { P.kill(); P.waitForFinished(2000); return false; }
    }
    P.waitForFinished(2000);
    return P.exitStatus() == QProcess::NormalExit && P.exitCode() == 0;
}

std::string AddNoCopy(const std::string &PathStr, std::string *Error)
{
    auto Fail = [&](const std::string &Msg) -> std::string { if (Error) *Error = Msg; return std::string(); };

    if (PathStr.empty())  return Fail("empty path");
    if (!Available())     return Fail("Kubo (ipfs) is not installed — cannot add " + PathStr);
    const QString Path = QString::fromStdString(PathStr);
    if (!QFileInfo::exists(Path)) return Fail("path does not exist: " + PathStr);

    QString Added;
    if (!RunIpfs({"add", "--nocopy", "--pin=true", "-Q", Path}, &Added, 600000))
        return Fail("`ipfs add --nocopy` failed for " + PathStr + " (is Filestore enabled?)");
    const std::string Cid = Added.trimmed().toStdString();
    if (Cid.empty()) return Fail("`ipfs add` produced no CID for " + PathStr);
    return Cid;
}

std::string FetchToPath(const std::string &Cid, const std::string &DestPathStr, std::string *Error)
{
    auto Fail = [&](const std::string &Msg) -> std::string {
        if (Error) *Error = Msg;
        LogErr("IpfsWrapper::FetchToPath", Msg);
        Emit({ TransferEvent::Finished, Cid, -1.0, false });
        return std::string();
    };

    if (Cid.empty())         return Fail("empty CID");
    if (DestPathStr.empty()) return Fail("empty destination path");
    const QString Dest = QString::fromStdString(DestPathStr);
    if (QFileInfo::exists(Dest)) { LogOut("IpfsWrapper::FetchToPath", "Already present: " + DestPathStr); return DestPathStr; }
    if (IsCancelled(Cid))    return Fail("cancelled");
    if (!Available())        return Fail("Kubo (ipfs) is not installed — cannot fetch CID " + Cid);

    QDir().mkpath(QFileInfo(Dest).absolutePath());          // ensure the layer's parent dirs exist
    const QString TmpDest = Dest + ".tmp";
    QDir(TmpDest).removeRecursively();                       // clear any partial previous attempt
    QFile::remove(TmpDest);

    LogOut("IpfsWrapper::FetchToPath", "Fetching CID " + Cid + " -> " + DestPathStr);
    Emit({ TransferEvent::Started, Cid, -1.0, false });
    if (!RunIpfsGet(Cid, TmpDest))
    {
        QDir(TmpDest).removeRecursively(); QFile::remove(TmpDest);
        return Fail("`ipfs get` failed for CID " + Cid + " (is the daemon running / CID reachable?)");
    }

    // Atomically publish the fetched content at the destination.
    QFile::remove(Dest);
    QDir(Dest).removeRecursively();
    if (!QDir().rename(TmpDest, Dest))
        return Fail("failed to move fetched CID into place: " + DestPathStr);

    // Seed from the destination via Filestore (--nocopy): the blocks REFERENCE the file in place — no second
    // copy. If the re-add yields a different CID (content not nocopy-hostable), fall back to a normal pin so we
    // still seed the correct CID. Best-effort — a missing daemon just defers it.
    const std::string AddedCid = AddNoCopy(Dest.toStdString());                  // single seed implementation
    const bool NocopyOk = !AddedCid.empty();
    if (NocopyOk && AddedCid == Cid)
        LogSucc("IpfsWrapper::FetchToPath", "Seeding " + Cid + " via Filestore from " + DestPathStr);
    else
    {
        if (NocopyOk && !AddedCid.empty())
        {
            LogWarn("IpfsWrapper::FetchToPath", "nocopy add produced a different CID (" + AddedCid + ") — using a normal pin.");
            RunIpfs({"pin", "rm", QString::fromStdString(AddedCid)}, nullptr, 30000);
        }
        if (!RunIpfs({"pin", "add", QString::fromStdString(Cid)}, nullptr, 60000))
            LogWarn("IpfsWrapper::FetchToPath", "could not pin CID " + Cid + " (start `ipfs daemon` to seed it).");
    }

    LogSucc("IpfsWrapper::FetchToPath", "Materialized CID " + Cid + " at " + DestPathStr);
    Emit({ TransferEvent::Finished, Cid, 100.0, true });
    return DestPathStr;
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
    QString Size, Max;
    for (const QString &Line : Out.split('\n', Qt::SkipEmptyParts))
    {
        if (Line.startsWith("RepoSize:"))   Size = Line.mid(QString("RepoSize:").size()).trimmed();
        else if (Line.startsWith("StorageMax:")) Max = Line.mid(QString("StorageMax:").size()).trimmed();
    }
    if (Size.isEmpty()) return std::string();
    return (Max.isEmpty() ? Size : Size + " / " + Max).toStdString();
}

long long CidSize(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    QString Out;
    if (!RunIpfs({"files", "stat", QString::fromStdString("/ipfs/" + Cid)}, &Out, 20000)) return -1;
    for (const QString &Line : Out.split('\n', Qt::SkipEmptyParts))
        if (Line.startsWith("CumulativeSize:"))
        {
            bool Ok = false;
            const long long N = Line.mid(QString("CumulativeSize:").size()).trimmed().toLongLong(&Ok);
            return Ok ? N : -1;
        }
    return -1;
}

int ProviderCount(const std::string &Cid)
{
    if (Cid.empty()) return -1;
    // findprovs STREAMS one peer id per line, then keeps walking the DHT until --num-providers or exhaustion. For
    // low-provider content that runs long, so we read the stream ourselves and stop at a cap or a short deadline —
    // keeping whatever we found (RunIpfs would discard it on timeout). This is a "how replicated" signal, not a census.
    QProcess P;
    P.setProcessEnvironment(SystemToolEnv());
    P.start("ipfs", {"routing", "findprovs", "--num-providers=10", QString::fromStdString(Cid)});
    if (!P.waitForStarted(10000)) return -1;

    std::set<std::string> Provs;
    QByteArray Buf;
    QElapsedTimer Timer; Timer.start();
    auto Drain = [&] {
        Buf += P.readAllStandardOutput();
        int Nl;
        while ((Nl = Buf.indexOf('\n')) >= 0)
        {
            const QString L = QString::fromUtf8(Buf.left(Nl)).trimmed();
            Buf.remove(0, Nl + 1);
            if (!L.isEmpty()) Provs.insert(L.toStdString());
        }
    };
    while (P.state() != QProcess::NotRunning)
    {
        P.waitForReadyRead(400);
        Drain();
        if ((int)Provs.size() >= 10 || Timer.hasExpired(8000)) break;
    }
    const bool Killed = (P.state() != QProcess::NotRunning);
    if (Killed) { P.kill(); P.waitForFinished(2000); }
    Drain();
    { const QString L = QString::fromUtf8(Buf).trimmed(); if (!L.isEmpty()) Provs.insert(L.toStdString()); }

    if (!Provs.empty()) return (int)Provs.size();
    if (!Killed && P.exitCode() != 0) return -1;   // exited on its own with an error (e.g. no daemon) → unknown
    return 0;                                       // ran the deadline with none found → genuinely 0
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
