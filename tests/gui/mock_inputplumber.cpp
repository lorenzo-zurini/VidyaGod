// mock_inputplumber — stands in for InputPlumber in the controller-service tests, so a test never runs the real one
// (it would take the controllers plugged into the machine running the tests). Serves the part of InputPlumber's D-Bus
// API VidyaGod uses, on the bus INPUTPLUMBER_BUS names (as the real one does, with our patch):
//   /org/shadowblip/InputPlumber/Manager            org.shadowblip.InputManager       ManageAllDevices (rw)
//   /org/shadowblip/InputPlumber/CompositeDeviceN   org.shadowblip.Input.CompositeDevice
//        Name, ProfileName, SourceDevicePaths; LoadProfileFromYaml(s) (refuses YAML that is no DeviceProfile)
// Its controllers: CompositeDevice0, plus CompositeDevice1 after MOCK_IP_HOTPLUG_MS (with an InterfacesAdded signal).
// Their source nodes: MOCK_IP_SOURCES ("a,b" for device 0; device 1 gets "<first>-1"). MOCK_IP_REPORT names a file
// it writes its environment and working folder to.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
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
public:
    Device(QString N, QStringList S) : Nm(std::move(N)), Src(std::move(S)) {}
    QString name() const { return Nm; }
    QString profileName() const { return Profile; }
    QStringList sources() const { return Src; }
public slots:
    void LoadProfileFromYaml(const QString &Yaml)
    {
        if (!Yaml.contains(QLatin1String("kind: DeviceProfile")))
        {
            sendErrorReply(QDBusError::Failed, QStringLiteral("not a DeviceProfile"));
            return;
        }
        static const QRegularExpression N(QStringLiteral("(?m)^name:\\s*(.+)$"));
        Profile = N.match(Yaml).captured(1).trimmed();
    }
private:
    QString Nm, Profile = QStringLiteral("Default");
    QStringList Src;
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

    //The name last: whoever waits for it finds the objects already there (InputPlumber does the same).
    if (!Conn.registerService(QStringLiteral("org.shadowblip.InputPlumber"))) return 3;
    return App.exec();
}

#include "mock_inputplumber.moc"
