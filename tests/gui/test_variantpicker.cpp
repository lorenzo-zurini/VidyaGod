// VariantPicker scale + behavior tests: the widget must handle an ARBITRARY variant count (the 903-version
// Minecraft tile was the motivating hang) — construction stays fast because it is model/view (no widget per row),
// filtering works, natural version ordering holds (1.9 < 1.10), recommended entries are pinned first, and the
// checked/current accessors round-trip.

#include <QtTest>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListView>
#include <QSortFilterProxyModel>

#include "variantpicker.h"

class TestVariantPicker : public QObject
{
    Q_OBJECT
private slots:

    void hugeListConstructsFast()
    {
        std::vector<VariantPicker::Entry> Es;
        Es.reserve(50000);
        for (int i = 0; i < 50000; ++i)
            Es.push_back({ "node_" + std::to_string(i), QString("Version %1.%2.%3").arg(i / 1000).arg((i / 10) % 100).arg(i % 10), false });
        QElapsedTimer T; T.start();
        VariantPicker P(VariantPicker::Mode::Checkable);
        P.setEntries(Es, { "node_0" });
        QVERIFY2(T.elapsed() < 3000, qPrintable(QString("50k rows took %1 ms").arg(T.elapsed())));
        QCOMPARE(P.checkedIds(), std::vector<std::string>{ "node_0" });
        P.grab();                                           // render headless — virtualization means this stays cheap
    }

    void filterNarrows()
    {
        std::vector<VariantPicker::Entry> Es;
        for (int i = 0; i < 100; ++i) Es.push_back({ "n" + std::to_string(i), QString("1.%1").arg(i), false });
        VariantPicker P(VariantPicker::Mode::Single);
        P.setEntries(Es);
        auto * Search = P.findChild<QLineEdit*>();
        auto * Proxy  = P.findChild<QSortFilterProxyModel*>();
        QVERIFY(Search && Proxy);
        QVERIFY(Search->isVisible() || Search->isVisibleTo(&P));   // big list → search shown
        Search->setText("1.42");
        QCOMPARE(Proxy->rowCount(), 1);
        Search->clear();
        QCOMPARE(Proxy->rowCount(), 100);
    }

    void naturalOrderAndRecommendedFirst()
    {
        VariantPicker P(VariantPicker::Mode::Single);
        P.setEntries({ { "b", "1.10", false }, { "c", "1.9", false }, { "a", "1.2", true } });
        // Recommended pinned first regardless of label; then 1.9 before 1.10 (numeric, not lexicographic).
        QCOMPARE(P.currentId(), std::string("a"));                 // Single mode selects the first (recommended) row
        auto * Proxy = P.findChild<QSortFilterProxyModel*>();
        QCOMPARE(Proxy->data(Proxy->index(1, 0), Qt::UserRole).toString(), QString("c"));
        QCOMPARE(Proxy->data(Proxy->index(2, 0), Qt::UserRole).toString(), QString("b"));
    }

    void checkRoundTrip()
    {
        VariantPicker P(VariantPicker::Mode::Checkable);
        P.setEntries({ { "x", "X", false }, { "y", "Y", false } }, { "y" });
        QCOMPARE(P.checkedIds(), std::vector<std::string>{ "y" });
    }
};

QTEST_MAIN(TestVariantPicker)
#include "test_variantpicker.moc"
