// mock_inputplumber — stands in for InputPlumber in the controller-service tests, so a test never runs the real one
// (it would take the controllers plugged into the machine running the tests). Serves the part of InputPlumber's D-Bus
// API VidyaGod uses, on the bus INPUTPLUMBER_BUS names (as the real one does, with our patch):
//   /org/shadowblip/InputPlumber/Manager            org.shadowblip.InputManager       ManageAllDevices (rw)
//   /org/shadowblip/InputPlumber/CompositeDeviceN   org.shadowblip.Input.CompositeDevice
//        Name, ProfileName, SourceDevicePaths; LoadProfileFromYaml(s) (refuses YAML that is no DeviceProfile), LoadProfilePath(s)
// Its controllers: CompositeDevice0, plus CompositeDevice1 after MOCK_IP_HOTPLUG_MS (with an InterfacesAdded signal).
// Their source nodes: MOCK_IP_SOURCES ("a,b" for device 0; device 1 gets "<first>-1"). MOCK_IP_REPORT names a file
// it writes its environment and working folder to. InterceptMode on each controller; Press(event) on
// /org/vidyagod/MockControl emits an intercepted input as InputPlumber's DBusDevice does.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

class Manager : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.shadowblip.InputManager")
    Q_PROPERTY(bool ManageAllDevices READ manageAll WRITE setManageAll)
public:
    bool manageAll() const { return All; }
    void setManageAll(bool On) { All = On; }
private:
    bool All = false;
};

class Device : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.shadowblip.Input.CompositeDevice")
    Q_PROPERTY(QString Name READ name)
    Q_PROPERTY(QString ProfileName READ profileName)
    Q_PROPERTY(QStringList SourceDevicePaths READ sources)
    Q_PROPERTY(uint InterceptMode READ intercept WRITE setIntercept)
public:
    uint intercept() const { return Intercept; }
    void setIntercept(uint M) { Intercept = M; }
    Device(QString N, QStringList S) : Nm(std::move(N)), Src(std::move(S)) {}
    QString name() const { return Nm; }
    QString profileName() const { return Profile; }
    QStringList sources() const { return Src; }
public slots:
    void LoadProfileFromYaml(const QString &Yaml)
    {
        if (!Yaml.contains(QLatin1String("kind: DeviceProfile")) && !Yaml.contains(QLatin1String("\"kind\":\"DeviceProfile\"")))
        {
            sendErrorReply(QDBusError::Failed, QStringLiteral("not a DeviceProfile"));
            return;
        }
        //YAML ("name: X") or JSON (YAML's superset: {"name":"X"}), as InputPlumber reads both.
        static const QRegularExpression N(QStringLiteral("(?m)^name:\\s*(.+)$|\"name\"\\s*:\\s*\"([^\"]*)\""));
        const QRegularExpressionMatch M = N.match(Yaml);
        Profile = (M.captured(1).isEmpty() ? M.captured(2) : M.captured(1)).trimmed();
    }
    void LoadProfilePath(const QString &Path) { Profile = QStringLiteral("Default from ") + QFileInfo(Path).fileName(); }
private:
    QString Nm, Profile = QStringLiteral("Default");
public:
    uint Intercept = 0;
private:
    QStringList Src;
};

// A test's hand on the mock: Press(event) emits what InputPlumber's DBusDevice emits for a press and its release —
// and, as InputPlumber does, the Guide button while only it is intercepted (1) switches to intercepting everything (2).
class Control : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.vidyagod.MockControl")
public:
    Control(QDBusConnection C, Device *D) : Conn(std::move(C)), Dev(D) {}
public slots:
    void Press(const QString &Event)
    {
        if (Event == QLatin1String("ui_guide") && Dev->Intercept == 1) Dev->Intercept = 2;
        for (const double V : { 1.0, 0.0 })
        {
            QDBusMessage S = QDBusMessage::createSignal(QStringLiteral("/org/shadowblip/InputPlumber/devices/target/dbus0"),
                                                        QStringLiteral("org.shadowblip.Input.DBusDevice"), QStringLiteral("InputEvent"));
            S << Event << V;
            Conn.send(S);
        }
    }
private:
    QDBusConnection Conn;
    Device *Dev;
};

int main(int argc, char **argv)
{
    QCoreApplication App(argc, argv);
    const QString Bus = qEnvironmentVariable("INPUTPLUMBER_BUS");
    QDBusConnection Conn = Bus == QLatin1String("session") ? QDBusConnection::sessionBus() : QDBusConnection::connectToBus(Bus, QStringLiteral("mock"));
    if (!Conn.isConnected()) return 2;

    if (const QString Report = qEnvironmentVariable("MOCK_IP_REPORT"); !Report.isEmpty())
    {
        QFile F(Report);
        if (F.open(QIODevice::WriteOnly))
        {
            F.write("INSECURE_DISABLE_POLKIT=" + qgetenv("INSECURE_DISABLE_POLKIT") + "\n");
            F.write("XDG_DATA_DIRS=" + qgetenv("XDG_DATA_DIRS") + "\n");
            F.write("PWD=" + QDir::currentPath().toUtf8() + "\n");
        }
    }

    const QStringList Sources = qEnvironmentVariable("MOCK_IP_SOURCES").split(QLatin1Char(','), Qt::SkipEmptyParts);
    const QString Root = QStringLiteral("/org/shadowblip/InputPlumber");
    Manager M;
    Device D0(QStringLiteral("Mock Deck"), Sources);
    Conn.registerObject(Root + QStringLiteral("/Manager"), &M, QDBusConnection::ExportAllProperties);
    Conn.registerObject(Root + QStringLiteral("/CompositeDevice0"), &D0, QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots);

    Device D1(QStringLiteral("Mock Pad"), Sources.isEmpty() ? QStringList() : QStringList{ Sources.first() + QStringLiteral("-1") });
    if (const int Ms = qEnvironmentVariableIntValue("MOCK_IP_HOTPLUG_MS"); Ms > 0)
        QTimer::singleShot(Ms, &App, [&] {
            Conn.registerObject(Root + QStringLiteral("/CompositeDevice1"), &D1, QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSlots);
            QDBusMessage S = QDBusMessage::createSignal(Root, QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("InterfacesAdded"));
            S << QVariant::fromValue(QDBusObjectPath(Root + QStringLiteral("/CompositeDevice1"))) << QVariantMap();
            Conn.send(S);
        });

    Control Ctl(Conn, &D0);
    Conn.registerObject(QStringLiteral("/org/vidyagod/MockControl"), &Ctl, QDBusConnection::ExportAllSlots);

    //The name last: whoever waits for it finds the objects already there (InputPlumber does the same).
    if (!Conn.registerService(QStringLiteral("org.shadowblip.InputPlumber"))) return 3;
    return App.exec();
}

#include "mock_inputplumber.moc"
