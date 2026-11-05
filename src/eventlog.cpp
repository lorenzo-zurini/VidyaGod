#include "eventlog.h"

#include <QDateTime>
#include <QMetaMethod>
#include <QMetaObject>
#include <QStringList>
#include <QVariant>
#include <deque>
#include <mutex>
#include <vector>

namespace EventLog
{

namespace
{

constexpr std::size_t Kept = 5000;

struct Event { std::uint64_t Seq; qint64 Ms; std::string Object, Signal; nlohmann::ordered_json Args; };

std::mutex Mu;
std::uint64_t Next = 1;

std::deque<Event> &RingOf()
{
    static std::deque<Event> R;   // function-local: built on first use, never during static initialisation
    return R;
}

nlohmann::ordered_json ArgJson(QMetaType T, void *P)
{
    if (!P) return nullptr;
    const QVariant V(T, P);
    switch (T.id())
    {
    case QMetaType::Bool: return V.toBool();
    case QMetaType::Int: case QMetaType::Short: case QMetaType::Long: case QMetaType::LongLong: return V.toLongLong();
    case QMetaType::UInt: case QMetaType::UShort: case QMetaType::ULong: case QMetaType::ULongLong: return V.toULongLong();
    case QMetaType::Double: case QMetaType::Float: return V.toDouble();
    case QMetaType::QStringList:
    {
        nlohmann::ordered_json A = nlohmann::ordered_json::array();
        for (const QString &S : V.toStringList()) A.push_back(S.toStdString());
        return A;
    }
    default: break;
    }
    if (V.canConvert<QString>()) return V.toString().toStdString();
    return std::string("<") + T.name() + ">";
}

// Receives every attached signal: no moc, so a connection to a slot index past QObject's lands in qt_metacall.
class Recorder : public QObject
{
public:
    struct Hook { std::string Object; QMetaMethod Signal; };

    void Add(QObject *Sender, const QMetaMethod &Signal, const std::string &Object)
    {
        std::lock_guard<std::mutex> Lk(Mu);
        Hooks.push_back({Object, Signal});
        const int Slot = QObject::staticMetaObject.methodCount() + static_cast<int>(Hooks.size()) - 1;
        QMetaObject::connect(Sender, Signal.methodIndex(), this, Slot, Qt::DirectConnection, nullptr);
    }

    int qt_metacall(QMetaObject::Call C, int Id, void **A) override
    {
        const int Base = QObject::staticMetaObject.methodCount();
        if (C != QMetaObject::InvokeMetaMethod || Id < Base) return QObject::qt_metacall(C, Id, A);
        Hook H;
        {
            std::lock_guard<std::mutex> Lk(Mu);
            if (static_cast<std::size_t>(Id - Base) >= Hooks.size()) return -1;
            H = Hooks[static_cast<std::size_t>(Id - Base)];
        }
        nlohmann::ordered_json Args = nlohmann::ordered_json::object();
        for (int I = 0; I < H.Signal.parameterCount(); ++I)
        {
            const QByteArray N = H.Signal.parameterNames().value(I);
            Args[N.isEmpty() ? "arg" + std::to_string(I) : N.toStdString()] = ArgJson(H.Signal.parameterMetaType(I), A[I + 1]);
        }
        std::lock_guard<std::mutex> Lk(Mu);
        RingOf().push_back({Next++, QDateTime::currentMSecsSinceEpoch(), H.Object, H.Signal.name().toStdString(), std::move(Args)});
        if (RingOf().size() > Kept) RingOf().pop_front();
        return -1;
    }

private:
    std::vector<Hook> Hooks;
};

Recorder &The()
{
    static Recorder *R = new Recorder;   // lives for the process: signals may fire during shutdown
    return *R;
}

std::string NameOf(const QObject *O, const QString &Given)
{
    if (!Given.isEmpty()) return Given.toStdString();
    if (!O->objectName().isEmpty()) return std::string(O->metaObject()->className()) + ":" + O->objectName().toStdString();
    return O->metaObject()->className();
}

} // namespace

void Attach(QObject *Obj, const QString &Name)
{
    if (!Obj) return;
    const std::string N = NameOf(Obj, Name);
    // The signals its own classes declare: walk up while the class is ours (Qt's classes are Q-prefixed).
    for (const QMetaObject *M = Obj->metaObject(); M && M->className()[0] != 'Q'; M = M->superClass())
        for (int I = M->methodOffset(); I < M->methodCount(); ++I)
            if (const QMetaMethod S = M->method(I); S.methodType() == QMetaMethod::Signal) The().Add(Obj, S, N);
}

void AttachTree(QObject *Root)
{
    if (!Root) return;
    Attach(Root);
    for (QObject *C : Root->findChildren<QObject *>()) Attach(C);
}

nlohmann::ordered_json Since(std::uint64_t After, const std::string &ObjectFilter, const std::string &SignalFilter, std::size_t Limit)
{
    std::lock_guard<std::mutex> Lk(Mu);
    nlohmann::ordered_json Out = nlohmann::ordered_json::array();
    for (const Event &E : RingOf())
    {
        if (E.Seq <= After) continue;
        if (!ObjectFilter.empty() && E.Object.find(ObjectFilter) == std::string::npos) continue;
        if (!SignalFilter.empty() && E.Signal.find(SignalFilter) == std::string::npos) continue;
        Out.push_back({{"seq", E.Seq}, {"time", QDateTime::fromMSecsSinceEpoch(E.Ms).toString("HH:mm:ss.zzz").toStdString()},
                       {"object", E.Object}, {"signal", E.Signal}, {"args", E.Args}});
        if (Out.size() >= Limit) break;
    }
    return Out;
}

std::uint64_t Latest()
{
    std::lock_guard<std::mutex> Lk(Mu);
    return Next - 1;
}

} // namespace EventLog
