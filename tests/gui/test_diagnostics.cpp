// test_diagnostics.cpp — the launch WARN/ERR tally (commonutils.h, Diagnostics).
//
// This exists because of a real failure mode: Worms 4 Mayhem logged "Could not open file for reading" on EVERY
// launch for months, and the only visible symptom was a slightly wrong aspect ratio. A launch prints hundreds of
// lines, so one warning in the middle is invisible. The tally is what makes that impossible — so its guarantees
// deserve to be pinned down hard, including the boring ones.

#include <QtTest/QtTest>
#include <thread>
#include <vector>

#include "commonutils.h"

class DiagnosticsTest : public QObject
{
    Q_OBJECT
private slots:
    void init()    { Diagnostics::Take(); }   // always start from "not collecting"
    void cleanup() { Diagnostics::Take(); ClearLogCallback(); }

    // Off by default: a tool that never calls Begin() must pay nothing and accumulate nothing.
    void inactive_by_default_collects_nothing()
    {
        QVERIFY(!Diagnostics::Collecting());
        LogErr("ctx", "an error nobody asked us to count");
        LogWarn("ctx", "a warning nobody asked us to count");
        const auto R = Diagnostics::Peek();
        QCOMPARE(R.Errors, size_t(0));
        QCOMPARE(R.Warnings, size_t(0));
        QVERIFY(R.Empty());
    }

    void counts_errors_and_warnings_separately()
    {
        Diagnostics::Begin();
        QVERIFY(Diagnostics::Collecting());
        LogErr("a", "e1");
        LogErr("a", "e2");
        LogWarn("a", "w1");
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Errors, size_t(2));
        QCOMPARE(R.Warnings, size_t(1));
        QCOMPARE(R.Entries.size(), size_t(3));
        QVERIFY(!R.Empty());
    }

    // Only the severities that indicate something WRONG are counted. Counting OUT/SUCC would make every launch
    // "dirty" and the signal worthless.
    void informational_levels_are_not_counted()
    {
        Diagnostics::Begin();
        LogOut("a", "just information");
        LogSucc("a", "something went well");
        const auto R = Diagnostics::Take();
        QVERIFY(R.Empty());
        QCOMPARE(R.Entries.size(), size_t(0));
    }

    // A single authoring mistake can emit the identical line once per file in a 900-entry loop. Counting each
    // occurrence is right (the count is the severity), but STORING each would bury every other finding.
    void repeats_are_counted_but_stored_once()
    {
        Diagnostics::Begin();
        for (int I = 0; I < 500; ++I) LogErr("loop", "the same message every time");
        LogErr("loop", "a different message");
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Errors, size_t(501));          // every occurrence counted
        QCOMPARE(R.Entries.size(), size_t(2));    // but only the distinct ones kept
    }

    // Same message from a different context is a different finding — the context is what tells you where to look.
    void same_message_different_context_is_distinct()
    {
        Diagnostics::Begin();
        LogErr("FileEdits", "could not open file");
        LogErr("RegistryLayer", "could not open file");
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Entries.size(), size_t(2));
    }

    // A pathological run must not grow memory without bound, and must still report the true count.
    void entries_are_capped_but_the_count_is_not()
    {
        Diagnostics::Begin();
        for (int I = 0; I < 1000; ++I) LogWarn("bulk", QString("distinct %1").arg(I).toStdString());
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Warnings, size_t(1000));
        QVERIFY2(R.Entries.size() <= 200, "stored entries must be capped");
        QVERIFY(R.Entries.size() > 0);
    }

    void take_stops_collecting_and_peek_does_not()
    {
        Diagnostics::Begin();
        LogErr("a", "first");
        const auto Peeked = Diagnostics::Peek();
        QCOMPARE(Peeked.Errors, size_t(1));
        QVERIFY(Diagnostics::Collecting());       // Peek is non-destructive

        LogErr("a", "second");
        const auto Taken = Diagnostics::Take();
        QCOMPARE(Taken.Errors, size_t(2));
        QVERIFY(!Diagnostics::Collecting());      // Take stops it

        LogErr("a", "after take");                // must not be counted
        QCOMPARE(Diagnostics::Peek().Errors, size_t(0));
    }

    // Begin() must reset, or a second launch would inherit the first one's verdict.
    void begin_resets_previous_state()
    {
        Diagnostics::Begin();
        LogErr("a", "from the first run");
        Diagnostics::Begin();
        LogWarn("a", "from the second run");
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Errors, size_t(0));
        QCOMPARE(R.Warnings, size_t(1));
        QCOMPARE(R.Entries.size(), size_t(1));
    }

    // The tally is taken inside Log() BEFORE the sink is consulted, so an installed UI callback cannot hide a
    // warning — and a throwing/slow sink cannot lose one either.
    void tally_is_independent_of_the_log_callback()
    {
        int Seen = 0;
        SetLogCallback([&](LogLevel, const std::string &, const std::string &){ ++Seen; });
        Diagnostics::Begin();
        LogErr("a", "counted regardless of the sink");
        ClearLogCallback();
        LogErr("a2", "counted with no sink at all");
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Errors, size_t(2));
        QCOMPARE(Seen, 1);
    }

    // ---- ReportVerdict: what every launch path prints at the end ----

    // A clean run must SAY it is clean. A summary that only appears on failure teaches you to ignore its absence,
    // which is how the original problem (a per-launch error nobody read) happened in the first place.
    void verdict_announces_a_clean_run()
    {
        std::vector<std::string> Lines;
        Diagnostics::Begin();
        SetLogCallback([&](LogLevel, const std::string &C, const std::string &M){ Lines.push_back(C + "|" + M); });
        const auto R = Diagnostics::ReportVerdict("Launch of 'x'");
        ClearLogCallback();

        QVERIFY(R.Empty());
        bool SaysClean = false;
        for (const auto &L : Lines)
            if (L.find("LaunchDiagnostics") == 0 && L.find("0 warnings and 0 errors") != std::string::npos)
                SaysClean = true;
        QVERIFY2(SaysClean, "a clean run must state that it was clean");
    }

    // A dirty run must name the counts AND reproduce the distinct messages, so the verdict alone is actionable
    // without scrolling back through hundreds of lines.
    void verdict_reports_counts_and_the_distinct_messages()
    {
        std::vector<std::string> Lines;
        Diagnostics::Begin();
        LogErr("FileEdits::ConfigWrite", "Could not open file for reading: Default.cfg");
        LogWarn("SomethingElse", "a warning");
        LogErr("FileEdits::ConfigWrite", "Could not open file for reading: Default.cfg");   // repeat
        SetLogCallback([&](LogLevel, const std::string &C, const std::string &M){ Lines.push_back(C + "|" + M); });
        const auto R = Diagnostics::ReportVerdict("Launch of 'w4m_base_game'");
        ClearLogCallback();

        QCOMPARE(R.Errors, size_t(2));
        QCOMPARE(R.Warnings, size_t(1));
        bool SaysCounts = false, RepeatsMessage = false, NamesTheRun = false;
        for (const auto &L : Lines)
        {
            if (L.find("2 ERROR(S)") != std::string::npos && L.find("1 WARNING(S)") != std::string::npos) SaysCounts = true;
            if (L.find("Could not open file for reading: Default.cfg") != std::string::npos) RepeatsMessage = true;
            if (L.find("w4m_base_game") != std::string::npos) NamesTheRun = true;
        }
        QVERIFY2(SaysCounts,      "the verdict must state how many errors and warnings there were");
        QVERIFY2(RepeatsMessage,  "the verdict must reproduce the actual message, not just a count");
        QVERIFY2(NamesTheRun,     "the verdict must name what was run");
    }

    // ReportVerdict takes the report, so a second call cannot re-report the first run's problems.
    void verdict_consumes_the_report()
    {
        Diagnostics::Begin();
        LogErr("a", "boom");
        QCOMPARE(Diagnostics::ReportVerdict("first").Errors, size_t(1));
        QVERIFY(Diagnostics::ReportVerdict("second").Empty());
    }

    // Log() is called from the launch worker while other threads log; the tally must not corrupt or lose counts.
    void concurrent_logging_is_counted_exactly()
    {
        Diagnostics::Begin();
        constexpr int Threads = 8, Each = 250;
        std::vector<std::thread> Ts;
        Ts.reserve(Threads);
        for (int T = 0; T < Threads; ++T)
            Ts.emplace_back([T]{
                for (int I = 0; I < Each; ++I)
                    LogErr("t" + std::to_string(T), "msg " + std::to_string(I));
            });
        for (auto &T : Ts) T.join();
        const auto R = Diagnostics::Take();
        QCOMPARE(R.Errors, size_t(Threads * Each));
    }
};

QTEST_MAIN(DiagnosticsTest)
#include "test_diagnostics.moc"
