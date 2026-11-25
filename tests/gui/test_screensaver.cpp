// The screensaver: the digital rain (MatrixRain) and when it shows (ScreenSaver) — after the idle interval, never while
// suspended or at 0, ended by any input that then does nothing else.

#include <QtTest>
#include <QImage>
#include <QLineEdit>
#include <QScreen>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "matrixrain.h"
#include "padnav.h"
#include "screensaver.h"

using json = nlohmann::ordered_json;

class ScreenSaverTest : public QObject
{
    Q_OBJECT

private:
    static int Green(const QImage &I, int *Bright = nullptr)
    {
        int N = 0;
        if (Bright) *Bright = 0;
        for (int Y = 0; Y < I.height(); ++Y)
            for (int X = 0; X < I.width(); ++X)
            {
                const QColor C = I.pixelColor(X, Y);
                if (C.green() > 60 && C.green() > C.red() + 30 && C.green() > C.blue() + 30) ++N;
                if (Bright && C.red() > 170 && C.green() > 200 && C.blue() > 170) ++*Bright;
            }
        return N;
    }

private slots:
    // The rain: a screen of columns, green trails behind near-white heads on black, and it moves — after a while no
    // cell shows what it showed (nothing stays lit to burn in). Teeth: a frame that never changes; no heads; no
    // trails; a rain that stops (every drop gone, none spawned).
    void the_rain_falls_with_bright_heads_and_green_trails()
    {
        MatrixRain R(nullptr, /*Seed=*/7);
        R.resize(640, 400);
        R.start();
        QVERIFY(R.columns() > 20);
        QVERIFY(R.rows() > 10);
        const QImage A = R.grab().toImage();
        int Heads = 0;
        QVERIFY2(Green(A, &Heads) > 500, "green trails");
        QVERIFY2(Heads > 5, "near-white heads");
        for (int I = 0; I < 300; ++I) R.advance(1.0 / MatrixRain::FramesPerSecond);   // ten seconds
        const QImage B = R.grab().toImage();
        int Same = 0, Lit = 0;
        for (int Y = 0; Y < A.height(); Y += 2)
            for (int X = 0; X < A.width(); X += 2)
                if (A.pixel(X, Y) != qRgb(0, 0, 0)) { ++Lit; if (A.pixel(X, Y) == B.pixel(X, Y)) ++Same; }
        QVERIFY(Lit > 0);
        QVERIFY2(Same * 5 < Lit, qPrintable(QString("%1 of %2 lit pixels unchanged after 10 s").arg(Same).arg(Lit)));
        for (int I = 0; I < 1800; ++I) R.advance(1.0 / MatrixRain::FramesPerSecond);  // a minute on: still raining
        QVERIFY(R.litCells() > R.columns());
        R.stop();
        QVERIFY(!R.isVisible());
    }

    // Its glyphs are the film's: half-width katakana, the digits and a few symbols — the katakana drawn whatever fonts
    // the machine has (a Steam Deck has no Japanese one), each its own shape. Teeth: draw the katakana with a font
    // (blank, or one tofu box for all); lose them from the set.
    void the_glyphs_are_katakana_digits_and_symbols()
    {
        const QString G = MatrixRain::glyphs();
        QVERIFY(G.contains(QChar(0xFF71)) && G.contains(QChar(0xFF9C)));   // ｱ ﾜ
        QVERIFY(G.contains('7') && G.contains(':'));
        const auto Lit = [](const QImage &I) { int N = 0; for (int Y = 0; Y < I.height(); ++Y) for (int X = 0; X < I.width(); ++X) N += qAlpha(I.pixel(X, Y)) > 128; return N; };
        const QImage A = MatrixRain::glyph(QChar(0xFF71), 24, 36, Qt::green), Wa = MatrixRain::glyph(QChar(0xFF9C), 24, 36, Qt::green);
        QVERIFY2(Lit(A) > 20 && Lit(Wa) > 20, "a katakana drawn blank");
        QVERIFY2(A != Wa, "two katakana drawn the same");
    }

    // After the interval with no input the rain covers the screen; a key ends it and types nothing, the next key
    // types; a controller press ends it and presses nothing; the wait starts over each time. Teeth: show without the
    // wait, or never; let the ending key through (the field gets it); route the ending press to the screen.
    void it_shows_after_the_idle_interval_and_any_input_ends_it()
    {
        QWidget Screen;
        Screen.resize(640, 400);
        auto *L = new QVBoxLayout(&Screen);
        auto *Field = new QLineEdit(&Screen);
        L->addWidget(Field);
        Screen.show();
        Field->setFocus();
        QVERIFY(QTest::qWaitForWindowExposed(&Screen));
        ScreenSaver S(&Screen, [] { return 1; });
        QVERIFY(!S.showing());
        QTRY_VERIFY_WITH_TIMEOUT(S.showing(), 3000);
        QVERIFY(S.rain()->isWindow());                                  // over every window, not inside this one
        QCOMPARE(S.rain()->geometry(), Screen.screen()->geometry());   // the whole screen
        QTest::keyClick(Field, Qt::Key_X);
        QVERIFY(!S.showing());
        QCOMPARE(Field->text(), QString());                            // the key that ended it typed nothing
        QTest::keyClick(Field, Qt::Key_Y);
        QCOMPARE(Field->text(), QString("y"));
        QTRY_VERIFY_WITH_TIMEOUT(S.showing(), 3000);
        QCOMPARE(PadRouter::instance().route(Pad::Button::A), QString("overlay"));   // the rain took the press
        QVERIFY(!S.showing());
        QVERIFY(S.idleRemainingMs() > 0);                              // and the wait started over
    }

    // Never at 0, never while suspended (a game runs); a changed interval applies at the next input; suspended while
    // its window is hidden, it stops. Teeth: show at 0; show while suspended; keep the old interval; judge showing by
    // the rain's visibility (hidden with its window, it ran on).
    void it_waits_the_interval_set_and_not_while_suspended()
    {
        QWidget Screen;
        Screen.resize(320, 200);
        Screen.show();
        int Seconds = 0;
        ScreenSaver S(&Screen, [&] { return Seconds; });
        QCOMPARE(S.idleRemainingMs(), -1);
        QTest::qWait(1200);
        QVERIFY(!S.showing());
        Seconds = 1;
        S.activity();
        QVERIFY2(S.idleRemainingMs() > 0 && S.idleRemainingMs() <= 1100, qPrintable(QString::number(S.idleRemainingMs())));   // a coarse timer: up to 5% late
        S.setSuspended(true);
        QCOMPARE(S.idleRemainingMs(), -1);
        QTest::qWait(1300);
        QVERIFY(!S.showing());
        S.showNow();
        QVERIFY(!S.showing());
        S.setSuspended(false);
        QTRY_VERIFY_WITH_TIMEOUT(S.showing(), 3000);
        S.setSuspended(true);                                          // a game starts while it shows: it goes
        QVERIFY(!S.showing());
        S.setSuspended(false);
        QTRY_VERIFY_WITH_TIMEOUT(S.showing(), 3000);
        Screen.hide();                                                 // its window hidden (the tray) while it shows,
        S.setSuspended(true);                                          // then suspended: it stops, not rains unseen
        QVERIFY(!S.showing());
        QVERIFY(!S.rain()->running());
    }

    // The setting: Settings.ScreenSaverSeconds, a minute when unset or not a whole number ≥ 0, 0 to turn it off.
    void the_setting_is_seconds_a_minute_by_default()
    {
        QCOMPARE(ScreenSaver::SecondsIn(json::object()), 60);
        QCOMPARE(ScreenSaver::SecondsIn(json{{"Settings", {{"ScreenSaverSeconds", 300}}}}), 300);
        QCOMPARE(ScreenSaver::SecondsIn(json{{"Settings", {{"ScreenSaverSeconds", 0}}}}), 0);
        QCOMPARE(ScreenSaver::SecondsIn(json{{"Settings", {{"ScreenSaverSeconds", -5}}}}), 60);
        QCOMPARE(ScreenSaver::SecondsIn(json{{"Settings", {{"ScreenSaverSeconds", "soon"}}}}), 60);
    }
};

QTEST_MAIN(ScreenSaverTest)
#include "test_screensaver.moc"
