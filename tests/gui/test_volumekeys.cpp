// The handheld's volume keys: what wpctl and pactl say the level is, the commands that change it, and the toast. (That
// the layouts route the keys to VidyaGod in its session is test_layouts'.)

#include <QtTest>

#include "volumekeys.h"

class VolumeKeysTest : public QObject
{
    Q_OBJECT
private slots:
    // wpctl's and pactl's own words, the level read from them; nothing from words that are not a level. Teeth: read
    // 0.55 as 0 or 55 as 0.55; miss the mute; take a non-level for 0%.
    void the_level_is_read_from_wpctl_and_pactl()
    {
        auto W = Volume::ParseWpctl("Volume: 0.55\n");
        QVERIFY(W && W->Percent == 55 && !W->Muted);
        W = Volume::ParseWpctl("Volume: 0.30 [MUTED]\n");
        QVERIFY(W && W->Percent == 30 && W->Muted);
        W = Volume::ParseWpctl("Volume: 1.00\n");
        QVERIFY(W && W->Percent == 100);
        QVERIFY(!Volume::ParseWpctl("Translate ID error: '@DEFAULT_AUDIO_SINK@' is not a valid ID\n"));
        auto P = Volume::ParsePactl("Volume: front-left: 42598 /  65% / -11.23 dB,   front-right: 42598 /  65% / -11.23 dB\n", "Mute: no\n");
        QVERIFY(P && P->Percent == 65 && !P->Muted);
        P = Volume::ParsePactl("Volume: front-left: 0 /   0% / -inf dB\n", "Mute: yes\n");
        QVERIFY(P && P->Percent == 0 && P->Muted);
        QVERIFY(!Volume::ParsePactl("Connection failure: Connection refused\n", ""));
    }

    // A step up never goes past 100% (wpctl -l 1.0; pactl, which has no limit, to the level from the one known); down
    // and mute on the default output, for either tool. Teeth: pactl stepped relatively past 100%.
    void the_commands_step_the_default_output()
    {
        QCOMPARE(Volume::StepCommand("wpctl", 5).second, (QStringList{ "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+" }));
        QCOMPARE(Volume::StepCommand("wpctl", -5).second, (QStringList{ "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%-" }));
        QCOMPARE(Volume::StepCommand("pactl", -5).second, (QStringList{ "set-sink-volume", "@DEFAULT_SINK@", "-5%" }));
        QCOMPARE(Volume::StepCommand("pactl", 5, 98).second, (QStringList{ "set-sink-volume", "@DEFAULT_SINK@", "100%" }));   // never past
        QCOMPARE(Volume::StepCommand("pactl", 5, 50).second, (QStringList{ "set-sink-volume", "@DEFAULT_SINK@", "+5%" }));
        QVERIFY2(Volume::StepCommand("pactl", 5, 120).first.isEmpty(), "a level set above 100% elsewhere, lowered by Up");
        QCOMPARE(Volume::StepCommand("pactl", -5, 120).second, (QStringList{ "set-sink-volume", "@DEFAULT_SINK@", "-5%" }));
        QCOMPARE(Volume::MuteCommand("wpctl").second, (QStringList{ "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle" }));
        QCOMPARE(Volume::MuteCommand("pactl").second, (QStringList{ "set-sink-mute", "@DEFAULT_SINK@", "toggle" }));
        QCOMPARE(Volume::ReadCommands("pactl").size(), size_t(2));
    }

    // The toast shows the level, and goes by itself.
    void the_toast_shows_the_level_for_a_moment()
    {
        VolumeToast T;
        T.present({ 40, false });
        QVERIFY(T.isVisible());
        QCOMPARE(T.level().Percent, 40);
        QTRY_VERIFY_WITH_TIMEOUT(!T.isVisible(), VolumeToast::ShowMs + 2000);
    }
};

QTEST_MAIN(VolumeKeysTest)
#include "test_volumekeys.moc"
