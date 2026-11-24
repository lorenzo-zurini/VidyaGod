// Controller layouts (ControllerLayout) and their configurator (LayoutEditor). Every layout VidyaGod can make is checked
// by InputPlumber's own code: `vidyagod-inputplumber profile check` (our fork's offline command — no daemon, no
// device), run in a sandbox with no devices at all.
#include <QtTest/QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

#include "controllerlayout.h"
#include "layouteditor.h"
#include "controllerservice.h"

using namespace ControllerLayout;

namespace
{
// InputPlumber's verdict on a profile: "" when it loads, else what it says.
QString IpCheck(const ojson &Profile)
{
    QTemporaryDir D;
    const QString F = D.filePath(QStringLiteral("p.json"));
    QFile Out(F);
    if (!Out.open(QIODevice::WriteOnly)) return QStringLiteral("cannot write");
    Out.write(QByteArray::fromStdString(Profile.dump(2)));
    Out.close();
    QProcess P;
    //No /dev but a minimal one: whatever it did, it could reach no input device.
    P.start(QStringLiteral("bwrap"), { QStringLiteral("--ro-bind"), QStringLiteral("/"), QStringLiteral("/"), QStringLiteral("--dev"), QStringLiteral("/dev"),
                                       QStringLiteral("--proc"), QStringLiteral("/proc"), QStringLiteral("--bind"), D.path(), D.path(),
                                       QStringLiteral(VG_INPUTPLUMBER), QStringLiteral("profile"), QStringLiteral("check"), F });
    if (!P.waitForFinished(30000)) return QStringLiteral("hung");
    const QString Said = QString::fromUtf8(P.readAllStandardOutput() + P.readAllStandardError());
    return P.exitCode() == 0 && Said.contains(QLatin1String(": OK")) ? QString() : Said;
}

Binding Btn(const char *Id, std::vector<Action> A) { return Binding{ Id, "", std::move(A), {}, 800 }; }
}

class LayoutsTest : public QObject
{
    Q_OBJECT
private slots:
    // VidyaGod's own layouts load in InputPlumber, and each one says what it is for.
    void every_template_is_a_profile_inputplumber_loads()
    {
        QCOMPARE(Templates().size(), size_t(3));
        for (const Layout &T : Templates())
        {
            QVERIFY2(!T.Description.empty(), T.Name.c_str());
            for (const Binding &B : T.Bindings) QVERIFY2(Check(B).empty(), (T.Name + " " + B.Control + ": " + Check(B)).c_str());
            const QString E = IpCheck(T.toProfile());
            QVERIFY2(E.isEmpty(), qPrintable(QString::fromStdString(T.Name) + QStringLiteral(": ") + E));
        }
    }

    // In VidyaGod's session every layout routes the handheld's volume keys to VidyaGod (its D-Bus events), so the volume
    // works in a game and out — and InputPlumber loads that mapping; on a desktop they are the desktop's. Teeth: a layout
    // without them (the Deck's +/- did nothing); them taken on a desktop (its own volume keys dead).
    void every_layout_routes_the_volume_keys_to_vidyagod()
    {
        SetVolumeKeysToVidyaGod(false);                                  // a desktop's: its keys stay the desktop's
        for (const Layout &T : Templates())
            for (const auto &M : T.toProfile()["mapping"])
                QVERIFY2(!M["source_event"].contains("keyboard"), (T.Name + ": a volume key taken from the desktop").c_str());
        SetVolumeKeysToVidyaGod(true);                                   // VidyaGod's session
        for (const Layout &T : Templates())
        {
            const ojson P = T.toProfile();
            for (const auto &[Key, Event] : { std::pair{ "KeyVolumeUp", "ui_volume_up" }, std::pair{ "KeyVolumeDown", "ui_volume_down" },
                                             std::pair{ "KeyMute", "ui_volume_mute" } })
            {
                bool Found = false;
                for (const auto &M : P["mapping"])
                    if (M["source_event"] == ojson{ { "keyboard", Key } } && M["target_events"] == ojson::array({ { { "dbus", Event } } })) Found = true;
                QVERIFY2(Found, (T.Name + ": " + Key).c_str());
            }
        }
        QVERIFY2(IpCheck(Templates()[0].toProfile()).isEmpty(), "InputPlumber refuses the volume mapping");
        SetVolumeKeysToVidyaGod(false);
    }

    // Everything the configurator offers is something InputPlumber loads: every key, pad button and mouse button (as
    // one chord each), the overlay, a trigger's key and pad button, and every stick mode. Teeth: a name in the lists
    // InputPlumber does not know, a malformed event (a trigger or axis source, a mouse target).
    void every_offered_choice_is_one_inputplumber_loads()
    {
        Layout L{ "Everything", "", {} };
        std::vector<Action> Keys, Pads, Mice;
        for (const std::string &K : KeyboardKeys()) Keys.push_back({ Action::Type::Keyboard, K });
        for (const std::string &P : GamepadButtons()) Pads.push_back({ Action::Type::Gamepad, P });
        for (const std::string &M : MouseButtons()) Mice.push_back({ Action::Type::Mouse, M });
        L.set(Btn("a", Keys));
        L.set(Btn("b", Pads));
        L.set(Btn("x", Mice));
        L.set(Btn("guide", { { Action::Type::Overlay, "" } }));
        L.set(Btn("y", {}));
        L.set({ "l4", "nothing", {}, {}, 800 });
        L.set(Btn("lt", { { Action::Type::Keyboard, "KeyLeftShift" } }));
        L.set(Btn("rt", { { Action::Type::Gamepad, "South" } }));
        L.set({ "left_stick", "keys", {}, { "KeyW", "KeyS", "KeyA", "KeyD" }, 800 });
        L.set({ "right_stick", "mouse", {}, {}, 1500 });
        QCOMPARE(L.Bindings.size(), size_t(9));   // y as is: no binding
        Layout D{ "Dpad", "", {} };
        D.set({ "left_stick", "dpad", {}, {}, 800 });
        for (const Layout *X : { &L, &D })
        {
            for (const Binding &B : X->Bindings) QVERIFY2(Check(B).empty(), (B.Control + ": " + Check(B)).c_str());
            const QString E = IpCheck(X->toProfile());
            QVERIFY2(E.isEmpty(), qPrintable(E));
        }
        //The mappings are what InputPlumber reads: a stick as the mouse is two (horizontal, vertical), as keys four.
        const ojson P = L.toProfile();
        int Mouse = 0, KeysN = 0;
        for (const ojson &M : P["mapping"])
        {
            if (M["source_event"]["gamepad"].contains("axis") && M["target_events"][0].contains("mouse")) ++Mouse;
            if (M["source_event"]["gamepad"].contains("axis") && M["target_events"][0].contains("keyboard")) ++KeysN;
        }
        QCOMPARE(Mouse, 2);
        QCOMPARE(KeysN, 4);
    }

    // The check itself has teeth: a key InputPlumber does not know is refused by InputPlumber.
    void inputplumber_refuses_what_it_does_not_know()
    {
        ojson P = Templates()[0].toProfile();
        P["mapping"] = ojson::array({ { { "name", "bad" }, { "source_event", { { "gamepad", { { "button", "South" } } } } },
                                        { "target_events", ojson::array({ { { "keyboard", "KeyBogus" } } }) } } });
        QVERIFY(!IpCheck(P).isEmpty());
    }

    // What InputPlumber cannot do is never offered, and never accepted from a file.
    void the_layout_refuses_what_inputplumber_cannot_do()
    {
        QVERIFY(!Check(Btn("rt", { { Action::Type::Mouse, "Left" } })).empty());      // trigger → mouse: not translated
        QVERIFY(!Check(Btn("rt", { { Action::Type::Overlay, "" } })).empty());
        QVERIFY(!Check(Btn("gyro", { { Action::Type::Keyboard, "KeyW" } })).empty());
        QVERIFY(!Check(Btn("right_pad", { { Action::Type::Mouse, "Left" } })).empty());
        QVERIFY(!Check(Btn("a", { { Action::Type::Keyboard, "KeyBogus" } })).empty());
        QVERIFY(!Check(Btn("a", { { Action::Type::Gamepad, "Bogus" } })).empty());
        QVERIFY(!Check(Btn("a", { { Action::Type::Mouse, "Bogus" } })).empty());
        QVERIFY(!Check(Btn("nope", { { Action::Type::Keyboard, "KeyW" } })).empty());
        QVERIFY(!Check({ "left_stick", "keys", {}, { "KeyW", "KeyS", "KeyA" }, 800 }).empty());
        QVERIFY(!Check({ "left_stick", "mouse", {}, {}, 0 }).empty());
        QVERIFY(!Check({ "left_stick", "wobble", {}, {}, 800 }).empty());
        QVERIFY(!Check({ "a", "nothing", { { Action::Type::Keyboard, "KeyW" } }, {}, 800 }).empty());
        QVERIFY(Check(Btn("a", { { Action::Type::Mouse, "WheelUp" } })).empty());
        std::string E;
        QVERIFY(!Layout::FromJson(ojson{ { "name", "X" }, { "bindings", ojson::array({ { { "control", "rt" }, { "actions",
                     ojson::array({ { { "type", "mouse" }, { "value", "Left" } } }) } } }) } }, &E));
        QVERIFY2(E.find("rt") != std::string::npos, E.c_str());
    }

    // A layout read back is the layout saved; "as is" leaves no binding; order follows the controller.
    void a_layout_reads_back_as_it_was_written()
    {
        for (const Layout &T : Templates())
        {
            const auto Back = Layout::FromJson(T.toJson());
            QVERIFY(Back);
            QCOMPARE(Back->Name, T.Name);
            QVERIFY(Back->Bindings == T.Bindings);
        }
        Layout L{ "Order", "", {} };
        L.set(Btn("y", { { Action::Type::Keyboard, "KeyY" } }));
        L.set(Btn("a", { { Action::Type::Keyboard, "KeyA" } }));
        QCOMPARE(L.Bindings[0].Control, std::string("a"));
        L.set(Btn("a", {}));
        QVERIFY(!L.find("a"));
        QCOMPARE(L.summary("a"), std::string("As is"));
        QCOMPARE(L.summary("y"), std::string("Key Y"));
        QCOMPARE(L.summary("left_stick"), std::string("Joystick"));
        QCOMPARE(L.summary("right_pad"), std::string("Pointer"));
        QCOMPARE(L.summary("gyro"), std::string("Not available"));
    }

    // The user's layouts on disk: saved, listed, read, removed; which game uses which. VidyaGod's names are taken, a
    // name is a file name, and a file that is no layout (or names another) is not listed.
    void the_users_layouts_live_in_their_folder()
    {
        QTemporaryDir T;
        const std::string Dir = T.path().toStdString();
        Layout Mine = Templates()[1];
        Mine.Name = "Mine";
        std::string E;
        QVERIFY2(Save(Dir, Mine, &E), E.c_str());
        Layout Bad = Mine;
        Bad.Name = "Gamepad";
        QVERIFY(!Save(Dir, Bad, &E));
        for (const char *N : { "", "a/b", ".hidden", "games" }) { Bad.Name = N; QVERIFY2(!Save(Dir, Bad, &E), N); }
        QFile Junk(T.filePath(QStringLiteral("junk.json")));
        QVERIFY(Junk.open(QIODevice::WriteOnly));
        Junk.write("{not json");
        Junk.close();
        QFile Other(T.filePath(QStringLiteral("other.json")));
        QVERIFY(Other.open(QIODevice::WriteOnly));
        Other.write("{\"name\":\"Someone else\",\"bindings\":[]}");
        Other.close();
        const auto All = Saved(Dir);
        QCOMPARE(All.size(), size_t(1));
        QCOMPARE(All[0].Name, std::string("Mine"));
        QVERIFY(All[0].Bindings == Mine.Bindings);
        QVERIFY(Named(Dir, "Gamepad"));
        QVERIFY(Named(Dir, "Mine"));
        QVERIFY(!Named(Dir, "Nope"));
        QCOMPARE(ForGame(Dir, "123"), std::string());
        QVERIFY(SetForGame(Dir, "123", "Mine"));
        QVERIFY(SetForGame(Dir, "456", "Desktop"));
        QCOMPARE(ForGame(Dir, "123"), std::string("Mine"));
        QCOMPARE(Games(Dir).size(), size_t(2));
        QVERIFY(SetForGame(Dir, "123", ""));
        QCOMPARE(ForGame(Dir, "123"), std::string());
        QVERIFY(!Remove(Dir, "Gamepad"));
        QVERIFY(Remove(Dir, "Mine"));
        QVERIFY(Saved(Dir).empty());
    }

    // VidyaGod's own layout: Gamepad until one is chosen, the chosen one by name (a template or the user's), Gamepad again
    // when it is cleared or gone; its file is no layout of the user's, and its name is taken. Teeth: ForApp read from
    // the wrong place; a chosen layout that is gone loaded anyway (nothing); app.json listed as a layout.
    void vidyagods_own_layout_is_chosen_by_name()
    {
        QTemporaryDir T;
        const std::string Dir = T.path().toStdString();
        QCOMPARE(AppLayout(Dir).Name, std::string("Gamepad"));
        QCOMPARE(ForApp(Dir), std::string());
        QVERIFY(SetForApp(Dir, "Desktop"));
        QCOMPARE(ForApp(Dir), std::string("Desktop"));
        QCOMPARE(AppLayout(Dir).Name, std::string("Desktop"));
        Layout Mine = Templates()[1];
        Mine.Name = "Menus";
        std::string E;
        QVERIFY2(Save(Dir, Mine, &E), E.c_str());
        QVERIFY(SetForApp(Dir, "Menus"));
        QCOMPARE(AppLayout(Dir).Name, std::string("Menus"));
        QVERIFY(AppLayout(Dir).Bindings == Mine.Bindings);
        QCOMPARE(Saved(Dir).size(), size_t(1));                                 // app.json is not a layout
        QVERIFY(Remove(Dir, "Menus"));
        QCOMPARE(AppLayout(Dir).Name, std::string("Gamepad"));                  // gone: the controller as it is
        QVERIFY(SetForApp(Dir, "Desktop"));
        QVERIFY(SetForApp(Dir, ""));
        QCOMPARE(ForApp(Dir), std::string());
        QVERIFY(!ValidName("app"));
        // A game's: its own, else the controller as it is — never VidyaGod's (Desktop here). Teeth: the game's choice
        // ignored; VidyaGod's taken for a game with none.
        QVERIFY(SetForApp(Dir, "Desktop"));
        QCOMPARE(GameLayout(Dir, "g").Name, std::string("Gamepad"));
        QVERIFY(SetForGame(Dir, "g", "Keyboard and mouse (WASD)"));
        QCOMPARE(GameLayout(Dir, "g").Name, std::string("Keyboard and mouse (WASD)"));
        QVERIFY(SetForGame(Dir, "g", "Gone"));
        QCOMPARE(GameLayout(Dir, "g").Name, std::string("Gamepad"));
    }

    // The names people read: keys as a keyboard prints them, pad buttons as a controller labels them.
    void labels_are_what_people_read()
    {
        QCOMPARE(KeyLabel("KeyW"), std::string("W"));
        QCOMPARE(KeyLabel("KeyLeftCtrl"), std::string("Left Ctrl"));
        QCOMPARE(KeyLabel("KeyKp7"), std::string("Keypad 7"));
        QCOMPARE(KeyLabel("KeyVolumeDown"), std::string("Volume Down"));
        QCOMPARE(KeyLabel("KeyF11"), std::string("F11"));
        QCOMPARE(KeyLabel("KeySlash"), std::string("/"));
        QCOMPARE(GamepadLabel("South"), std::string("A"));
        QCOMPARE(GamepadLabel("DPadLeft"), std::string("D-pad left"));
        QCOMPARE(MouseLabel("WheelDown"), std::string("Wheel down"));
    }

    // The configurator, driven as a person drives it: a control picked on the drawn Deck selects it in the list; a
    // trigger is never offered the mouse; a binding shows in the list at once; VidyaGod's own layout is never
    // overwritten; saved under a name and chosen for the game; deleted only with a second press.
    void the_configurator_edits_saves_and_chooses_for_the_game()
    {
        QTemporaryDir T;
        LayoutEditor E(T.path(), QStringLiteral("777"), QStringLiteral("Some Game"));
        E.resize(1100, 640);
        E.show();
        QVERIFY(QTest::qWaitForWindowExposed(&E));
        QCOMPARE(QString::fromStdString(E.current().Name), QStringLiteral("Gamepad"));
        auto *Pic = E.findChild<ControllerPicture *>();
        QVERIFY(Pic);
        QTest::mouseClick(Pic, Qt::LeftButton, {}, Pic->hotspot(QStringLiteral("b")).center().toPoint());
        QCOMPARE(E.selectedControl(), QStringLiteral("b"));
        QTest::mouseClick(Pic, Qt::LeftButton, {}, Pic->hotspot(QStringLiteral("l3")).center().toPoint());
        QCOMPARE(E.selectedControl(), QStringLiteral("l3"));   // the click inside the stick, not the stick

        QVERIFY(E.selectControl(QStringLiteral("rt")));
        auto *Type = E.findChild<QComboBox *>(QStringLiteral("actionType"));
        QVERIFY(Type->findData(QStringLiteral("mouse")) < 0);
        QVERIFY(E.selectControl(QStringLiteral("rb")));
        QVERIFY(Type->findData(QStringLiteral("mouse")) >= 0);
        Type->setCurrentIndex(Type->findData(QStringLiteral("keyboard")));   // as a person (or a controller) picks it
        auto *Value = E.findChild<QComboBox *>(QStringLiteral("actionValue"));
        Value->setCurrentIndex(Value->findData(QStringLiteral("KeySpace")));
        QVERIFY(E.dirty());
        auto *Tree = E.findChild<QTreeWidget *>(QStringLiteral("controlsTree"));
        QCOMPARE(Tree->currentItem()->text(1), QStringLiteral("Key Space"));

        QVERIFY(E.selectControl(QStringLiteral("right_stick")));
        auto *Mode = E.findChild<QComboBox *>(QStringLiteral("stickMode"));
        Mode->setCurrentIndex(Mode->findData(QStringLiteral("mouse")));
        QCOMPARE(QString::fromStdString(E.current().find("right_stick")->Mode), QStringLiteral("mouse"));

        QString Err;
        QVERIFY(!E.useForGame(&Err));                 // unsaved: a game uses a saved layout
        QVERIFY(!E.saveAs(QStringLiteral("Gamepad"), &Err));
        QVERIFY(E.saveAs(QStringLiteral("Some Game pad"), &Err));
        QVERIFY(QFile::exists(T.filePath(QStringLiteral("Some Game pad.json"))));
        QVERIFY(E.useForGame(&Err));
        QCOMPARE(ForGame(T.path().toStdString(), "777"), std::string("Some Game pad"));
        const auto Back = Named(T.path().toStdString(), "Some Game pad");
        QVERIFY(Back && Back->find("rb") && Back->find("rb")->Actions[0].Value == "KeySpace");

        auto *Del = E.findChild<QPushButton *>(QStringLiteral("deleteLayout"));
        QVERIFY(Del->isEnabled());
        Del->click();
        QVERIFY(QFile::exists(T.filePath(QStringLiteral("Some Game pad.json"))));   // armed, not gone
        Del->click();
        QVERIFY(!QFile::exists(T.filePath(QStringLiteral("Some Game pad.json"))));
        QCOMPARE(QString::fromStdString(E.current().Name), QStringLiteral("Gamepad"));
        QVERIFY(!Del->isEnabled());                   // VidyaGod's own cannot be deleted
    }

    // The configurator makes a saved layout VidyaGod's own and puts it on the controller; a layout tried there is let go
    // when it closes (VidyaGod's own back); deleting VidyaGod's own makes it Gamepad again. Teeth: Use for VidyaGod
    // not loading it; Try now left on after the close; a deleted own layout left chosen.
    void the_configurator_chooses_vidyagods_own_layout()
    {
        QTemporaryDir T;
        ControllerService &S = ControllerService::instance();
        const auto Restore = qScopeGuard([&] { S.loadLayout({}); });
        auto Loaded = [&] { return QString::fromStdString(ojson::parse(S.layout().toStdString(), nullptr, false).value("name", std::string())); };
        {
            LayoutEditor E(T.path());
            QVERIFY(E.openLayout(QStringLiteral("Desktop")));
            QString Err;
            QVERIFY(E.useForApp(&Err));
            QCOMPARE(ForApp(T.path().toStdString()), std::string("Desktop"));
            QCOMPARE(Loaded(), QStringLiteral("Desktop"));
            QVERIFY(E.findChild<QLabel *>(QStringLiteral("usedBy"))->text().contains(QLatin1String("VidyaGod uses: Desktop")));
            QVERIFY(E.openLayout(QStringLiteral("Keyboard and mouse (WASD)")));
            QVERIFY(E.tryNow(&Err));
            QCOMPARE(Loaded(), QStringLiteral("Keyboard and mouse (WASD)"));
            E.reject();
        }
        QCOMPARE(Loaded(), QStringLiteral("Desktop"));                          // tried, then let go
        {
            LayoutEditor E(T.path());
            QVERIFY(E.openLayout(QStringLiteral("Desktop")));
            QVERIFY(E.selectControl(QStringLiteral("y")));
            QString Err;
            QVERIFY(E.bindSelected(Binding{ "y", "", { Action{ Action::Type::Keyboard, "KeySpace" } }, {}, 800 }, &Err));
            QVERIFY(E.saveAs(QStringLiteral("Mine"), &Err));
            QVERIFY(E.useForApp(&Err));
            QCOMPARE(Loaded(), QStringLiteral("Mine"));
            auto *Del = E.findChild<QPushButton *>(QStringLiteral("deleteLayout"));
            Del->click();
            Del->click();
            QCOMPARE(ForApp(T.path().toStdString()), std::string());
            QCOMPARE(Loaded(), QStringLiteral("Gamepad"));
        }
    }

    // Opened for a game that has a layout, it opens that one; a refused binding says why and changes nothing.
    void the_configurator_opens_the_games_layout_and_refuses_what_cannot_be()
    {
        QTemporaryDir T;
        QVERIFY(SetForGame(T.path().toStdString(), "42", "Desktop"));
        LayoutEditor E(T.path(), QStringLiteral("42"), QStringLiteral("G"));
        QCOMPARE(QString::fromStdString(E.current().Name), QStringLiteral("Desktop"));
        QVERIFY(E.selectControl(QStringLiteral("lt")));
        QString Err;
        QVERIFY(!E.bindSelected(Btn("lt", { { Action::Type::Mouse, "Left" } }), &Err));
        QVERIFY2(Err.contains(QLatin1String("trigger")), qPrintable(Err));
        QVERIFY(!E.dirty());
        QVERIFY(!E.bindSelected(Btn("a", { { Action::Type::Keyboard, "KeyW" } }), &Err));   // not the selected control
    }
};

QTEST_MAIN(LayoutsTest)
#include "test_layouts.moc"
