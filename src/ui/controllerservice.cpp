#include "controllerservice.h"

#include "commonutils.h"    // Log*
#include "launchthread.h"   // LaunchThread::Running — VidyaGod's games
#include "padinput.h"
#include "sandboxlayer.h"   // SandboxLayer::SetHiddenDevicesSource

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QXmlStreamReader>

#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace
{
constexpr const char *Root = "/org/shadowblip/InputPlumber";
constexpr const char *ManagerPath = "/org/shadowblip/InputPlumber/Manager";
constexpr const char *ManagerIface = "org.shadowblip.InputManager";
constexpr const char *DeviceIface = "org.shadowblip.Input.CompositeDevice";
constexpr const char *PropsIface = "org.freedesktop.DBus.Properties";

// The sysfs entry for a device node: /dev/input/eventN and jsN under class/input, /dev/hidrawN under class/hidraw.
QString SysEntry(const QString &Node, const QString &SysRoot)
{
    const QString Name = QFileInfo(Node).fileName();
    if (Node.startsWith(QLatin1String("/dev/input/"))) return SysRoot + QStringLiteral("/class/input/") + Name;
    if (Name.startsWith(QLatin1String("hidraw"))) return SysRoot + QStringLiteral("/class/hidraw/") + Name;
    return {};
}
}

bool IsVirtualInputNode(const QString &Node, const QString &SysRoot)
{
    const QString Entry = SysEntry(Node, SysRoot);
    if (Entry.isEmpty()) return false;
    const QString Real = QFileInfo(Entry).canonicalFilePath();
    return !Real.isEmpty() && Real.contains(QLatin1String("/devices/virtual/"));
}

QStringList JoystickSiblings(const QString &EventNode, const QString &SysRoot)
{
    QStringList Out;
    if (!QFileInfo(EventNode).fileName().startsWith(QLatin1String("event"))) return Out;
    const QString Real = QFileInfo(SysEntry(EventNode, SysRoot)).canonicalFilePath();   // .../inputX/eventN
    if (Real.isEmpty()) return Out;
    static const QRegularExpression Js(QStringLiteral("^js\\d+$"));
    for (const QString &E : QDir(QFileInfo(Real).path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (Js.match(E).hasMatch()) Out << QStringLiteral("/dev/input/") + E;
    return Out;
}

ControllerService &ControllerService::instance()
{
    static ControllerService S;
    return S;
}

ControllerService::ControllerService()
{
    //A game's sandbox covers the real controllers this service owns.
    SandboxLayer::SetHiddenDevicesSource([] { return ControllerService::instance().hiddenNodes(); });
}

std::vector<std::string> ControllerService::hiddenNodes() const
{
    std::lock_guard<std::mutex> L(HiddenLock);
    return Hidden;
}

void ControllerService::refreshHidden()
{
    std::vector<std::string> Now;
    for (const QString &N : sourceNodes()) Now.push_back(N.toStdString());
    std::lock_guard<std::mutex> L(HiddenLock);
    Hidden = std::move(Now);
}

ControllerService::~ControllerService()
{
    //At exit: the controllers go back to everything else (InputPlumber lets go of them on SIGTERM).
    if (Proc) { Proc->disconnect(this); Proc->terminate(); Proc->waitForFinished(3000); }
}

QString ControllerService::NeedName(Need N)
{
    switch (N)
    {
    case Need::ControllerMode: return QStringLiteral("controller mode");
    case Need::Game:           return QStringLiteral("game");
    case Need::Session:        return QStringLiteral("session");
    }
    return {};
}

void ControllerService::setProgram(const QString &Path) { Program = Path; update(); }

void ControllerService::setBus(const QString &Bus_)
{
    if (Bus_ == Bus) return;
    if (Proc) stop();
    Bus = Bus_;
    BusConnName.clear();
    update();
}

QStringList ControllerService::needs() const
{
    QStringList Out;
    for (Need N : { Need::ControllerMode, Need::Game, Need::Session }) if (wants(N)) Out << NeedName(N);
    return Out;
}

void ControllerService::want(Need N, bool On)
{
    const std::uint8_t Bit = static_cast<std::uint8_t>(N);
    const std::uint8_t Was = Needs;
    Needs = On ? (Needs | Bit) : (Needs & ~Bit);
    if (Needs != Was) update();
}

void ControllerService::reset()
{
    Needs = 0;
    Layout.clear();
    update();
}

QString ControllerService::unavailable() const
{
#ifndef __linux__
    return QStringLiteral("only on Linux");
#else
    if (Program.isEmpty()) return QStringLiteral("no InputPlumber program");
    if (!QFileInfo(Program).isExecutable()) return QStringLiteral("InputPlumber not found at ") + Program;
    //A distro that ships InputPlumber runs it system-wide: two would fight over the same controllers.
    if (!Proc)
        if (QDBusConnection Sys = QDBusConnection::systemBus(); Sys.isConnected() && Sys.interface()
            && Sys.interface()->isServiceRegistered(QString::fromLatin1(BusName)))
            return QStringLiteral("a system-wide InputPlumber already runs");
    return {};
#endif
}

void ControllerService::update()
{
    //A game only keeps it running: it never starts it (see the header).
    const bool Should = (Needs & ~static_cast<std::uint8_t>(Need::Game)) != 0 || (Proc && wants(Need::Game));
    if (Should && !Proc && available()) start();
    else if (!Should && Proc) stop();
    //Read the Steam Deck's own controller directly only while controller mode needs it and InputPlumber owns no
    //controller (not available here, or not up yet): the two would fight over its hidraw.
    refreshHidden();
    const bool Owns = !hiddenNodes().empty();
    PadInput::instance().setDeckDirect(wants(Need::ControllerMode) && !Owns);
    PadInput::instance().setOnlyVirtual(Owns);
}

QDBusConnection ControllerService::connection() const
{
    if (Bus == QLatin1String("session")) return QDBusConnection::sessionBus();
    auto *Self = const_cast<ControllerService *>(this);
    if (Self->BusConnName.isEmpty())
    {
        Self->BusConnName = QStringLiteral("vidyagod-controllers");
        QDBusConnection::disconnectFromBus(Self->BusConnName);
        return QDBusConnection::connectToBus(Bus, Self->BusConnName);
    }
    return QDBusConnection(BusConnName);
}

void ControllerService::start()
{
    Proc = new QProcess(this);
    QProcessEnvironment Env = QProcessEnvironment::systemEnvironment();
    Env.insert(QStringLiteral("INPUTPLUMBER_BUS"), Bus);
    //Only VidyaGod (its user) reaches it on this bus: no polkit agent to ask.
    Env.insert(QStringLiteral("INSECURE_DISABLE_POLKIT"), QStringLiteral("1"));
    if (!DataDir.isEmpty())
        Env.insert(QStringLiteral("XDG_DATA_DIRS"),
                   DataDir + QLatin1Char(':') + Env.value(QStringLiteral("XDG_DATA_DIRS"), QStringLiteral("/usr/local/share:/usr/share")));
    Proc->setProcessEnvironment(Env);
    Proc->setWorkingDirectory(QStringLiteral("/"));   // it also reads ./rootfs/...: never a folder of ours
#ifdef __linux__
    //It ends with VidyaGod however VidyaGod ends (a crash, a kill, an exit that runs no destructors): an orphan would
    //keep every controller it took from everything else.
    const pid_t Parent = ::getpid();
    Proc->setChildProcessModifier([Parent] {
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != Parent) ::_exit(0);   // VidyaGod already gone before the line above
    });
#endif
    Proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(Proc, &QProcess::readyRead, this, [this] {
        while (Proc && Proc->canReadLine())
            if (const QString L = QString::fromUtf8(Proc->readLine()).trimmed(); !L.isEmpty()) LogOut("InputPlumber", L.toStdString());
    });
    connect(Proc, &QProcess::finished, this, [this](int Code, QProcess::ExitStatus) {
        LogWarn("ControllerService", "InputPlumber ended (exit " + std::to_string(Code) + ")");
        Proc->deleteLater();
        Proc = nullptr;
        Answering = false;
        if (GameWatch) GameWatch->stop();
        emit changed();
        update();   // a crash does not restart it in a loop: start() runs again only when a reason is taken anew
    });

    QDBusConnection Conn = connection();
    auto *Watch = new QDBusServiceWatcher(QString::fromLatin1(BusName), Conn, QDBusServiceWatcher::WatchForRegistration, Proc);
    connect(Watch, &QDBusServiceWatcher::serviceRegistered, this, &ControllerService::onRegistered);
    Conn.connect(QString::fromLatin1(BusName), QString::fromLatin1(Root), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                 QStringLiteral("InterfacesAdded"), this, SLOT(onObjectsChanged()));
    Conn.connect(QString::fromLatin1(BusName), QString::fromLatin1(Root), QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                 QStringLiteral("InterfacesRemoved"), this, SLOT(onObjectsChanged()));

    if (!GameWatch)
    {
        GameWatch = new QTimer(this);
        GameWatch->setInterval(500);
        //A launch beginning or ending, not its state: the reason is taken when one begins and let go when the last ends.
        connect(GameWatch, &QTimer::timeout, this, [this] {
            const bool Now = LaunchThread::Running() > 0;
            if (Now != GamesRan) { GamesRan = Now; want(Need::Game, Now); }
        });
    }
    LogOut("ControllerService", "starting InputPlumber for " + needs().join(QStringLiteral(", ")).toStdString());
    Proc->start(Program, {});
    GamesRan = LaunchThread::Running() > 0;
    if (GamesRan) Needs |= static_cast<std::uint8_t>(Need::Game);
    GameWatch->start();
}

void ControllerService::stop()
{
    if (!Proc) return;
    LogOut("ControllerService", "stopping InputPlumber: the controllers go back to everything else");
    if (GameWatch) GameWatch->stop();
    Answering = false;
    QProcess *P = Proc;
    Proc = nullptr;
    P->disconnect(this);
    P->terminate();
    if (!P->waitForFinished(3000)) { P->kill(); P->waitForFinished(1000); }
    P->deleteLater();
    Needs &= ~static_cast<std::uint8_t>(Need::Game);
    refreshHidden();
    emit changed();
}

void ControllerService::onRegistered()
{
    if (!Proc) return;
    Answering = true;
    //It takes no controller until told to: take every one it has a configuration for.
    QDBusInterface Props(QString::fromLatin1(BusName), QString::fromLatin1(ManagerPath), QString::fromLatin1(PropsIface), connection());
    const QDBusMessage R = Props.call(QStringLiteral("Set"), QString::fromLatin1(ManagerIface), QStringLiteral("ManageAllDevices"),
                                      QVariant::fromValue(QDBusVariant(true)));
    if (R.type() == QDBusMessage::ErrorMessage) LogWarn("ControllerService", "ManageAllDevices: " + R.errorMessage().toStdString());
    LogOut("ControllerService", "InputPlumber is up");
    applyLayout();
    emit changed();
    update();
}

void ControllerService::onObjectsChanged()
{
    applyLayout();
    emit changed();
    update();
}

QList<ControllerService::Device> ControllerService::devices() const
{
    QList<Device> Out;
    if (!Answering) return Out;
    const QDBusConnection Conn = connection();
    QDBusInterface Intro(QString::fromLatin1(BusName), QString::fromLatin1(Root), QStringLiteral("org.freedesktop.DBus.Introspectable"), Conn);
    const QDBusReply<QString> Xml = Intro.call(QStringLiteral("Introspect"));
    if (!Xml.isValid()) return Out;
    QXmlStreamReader X(Xml.value());
    while (!X.atEnd())
    {
        X.readNext();
        if (!X.isStartElement() || X.name() != QLatin1String("node")) continue;
        const QString N = X.attributes().value(QLatin1String("name")).toString();
        if (!N.startsWith(QLatin1String("CompositeDevice"))) continue;
        Device D;
        D.Path = QString::fromLatin1(Root) + QLatin1Char('/') + N;
        QDBusInterface Dev(QString::fromLatin1(BusName), D.Path, QString::fromLatin1(DeviceIface), Conn);
        D.Name = Dev.property("Name").toString();
        D.Profile = Dev.property("ProfileName").toString();
        D.Sources = Dev.property("SourceDevicePaths").toStringList();
        Out << D;
    }
    return Out;
}

QStringList ControllerService::sourceNodes() const
{
    QStringList Out;
    for (const Device &D : devices())
        for (const QString &S : D.Sources)
        {
            if (IsVirtualInputNode(S)) continue;   // never a device of its own making
            Out << S;
            Out << JoystickSiblings(S);
        }
    Out.removeDuplicates();
    return Out;
}

bool ControllerService::loadLayout(const QString &Yaml, QString *Error)
{
    Layout = Yaml;
    if (!Answering) return true;   // loaded when it comes up
    for (const Device &D : devices())
    {
        QDBusInterface Dev(QString::fromLatin1(BusName), D.Path, QString::fromLatin1(DeviceIface), connection());
        const QDBusMessage R = Yaml.isEmpty() ? QDBusMessage() : Dev.call(QStringLiteral("LoadProfileFromYaml"), Yaml);
        if (R.type() == QDBusMessage::ErrorMessage)
        {
            if (Error) *Error = D.Name + QStringLiteral(": ") + R.errorMessage();
            return false;
        }
    }
    emit changed();
    return true;
}

void ControllerService::applyLayout()
{
    if (Layout.isEmpty()) return;
    QString Error;
    if (!loadLayout(Layout, &Error)) LogWarn("ControllerService", "layout refused: " + Error.toStdString());
}
