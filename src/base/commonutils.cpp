#include "commonutils.h"

#include <cctype>
#include <cstdlib>
#include <cstdint>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <set>

//Static callback — null by default. Set via SetLogCallback(); cleared via ClearLogCallback().
//MUTEX-GUARDED: the old "set once from the main thread before the worker starts" comment was wrong —
//LaunchThread installs it FROM the worker while other threads are logging, which mutated a live
//std::function under concurrent readers. Log() copies the callback under the lock, then invokes the
//copy outside it (so a slow sink never serializes unrelated logging, and Clear during a call is safe).
static std::mutex   g_LogCallbackMtx;
static LogCallback  g_LogCallback;

//Installs a secondary log sink. Replaces any previously installed callback.
bool VerboseLogging()
{
    static const bool On = []{ const char *V = std::getenv("VG_VERBOSE"); return V && *V == '1'; }();
    return On;
}

static std::atomic<bool> g_LogMillis{ false };
void SetLogMillis(bool On) { g_LogMillis = On; }

void SetLogCallback(LogCallback callback)
{
    std::lock_guard<std::mutex> G(g_LogCallbackMtx);
    g_LogCallback = std::move(callback);
}

//Removes the secondary log sink so Log() reverts to stdout-only output.
void ClearLogCallback()
{
    std::lock_guard<std::mutex> G(g_LogCallbackMtx);
    g_LogCallback = nullptr;
}

//Writes a single log line to stdout with a HH:MM:SS timestamp, a color-coded severity
//label, the calling context, and the message. Uses ANSI escape codes for terminal color.
//All output goes to stdout so it can be captured alongside QProcess output in one stream.
//If a LogCallback has been installed via SetLogCallback(), it is also invoked with the
//raw (unformatted) level/context/message so UI components can display colored log lines.
static thread_local int g_Quiet = 0;
static std::atomic<bool> g_EchoStderr{true};   // the CLI app's default: off — its stdout is the answer, --verbose turns it on
void SetLogEcho(bool On) { g_EchoStderr = On; }
QuietLog::QuietLog()  { ++g_Quiet; }
QuietLog::~QuietLog() { --g_Quiet; }

static thread_local LogRecorder *g_Recorder = nullptr;   // the innermost recorder on this thread
static thread_local LogBuffer *g_Buffer = nullptr;       // the innermost buffer on this thread
static thread_local LogSink *g_Sink = nullptr;           // the innermost sink on this thread

LogRecorder::LogRecorder(std::set<std::string> C) : Contexts(std::move(C)), Outer(g_Recorder) { g_Recorder = this; }
//Out of the chain whatever order they end in: a launch kept across commands (`launch build`) outlives the command's own
//recorder it was made inside, and the one made after it must not be left pointing at it.
LogRecorder::~LogRecorder()
{
    if (g_Recorder == this) { g_Recorder = Outer; return; }
    for (LogRecorder *R = g_Recorder; R; R = R->Outer) if (R->Outer == this) { R->Outer = Outer; return; }
}

void LogRecorder::Note(LogLevel Level, const std::string &Context, const std::string &Message)
{
    if (Contexts.empty() || Contexts.count(Context)) Kept.push_back({Level, Context, Message});
    if (Outer) Outer->Note(Level, Context, Message);
}

std::vector<Diagnostics::Entry> LogRecorder::Take() { return std::move(Kept); }

LogSink::LogSink(LogCallback L, StepFn S) : Line(std::move(L)), Step(std::move(S)), Outer(g_Sink) { g_Sink = this; }
LogSink::~LogSink()
{
    if (g_Sink == this) { g_Sink = Outer; return; }
    for (LogSink *S = g_Sink; S; S = S->Outer) if (S->Outer == this) { S->Outer = Outer; return; }
}
LogSink *CurrentLogSink() { return g_Sink; }
LogBuffer::LogBuffer() : Outer(g_Buffer) { g_Buffer = this; }
LogBuffer::~LogBuffer() { g_Buffer = Outer; }
void LogBuffer::Keep(LogLevel Level, const std::string &Context, const std::string &Message) { Lines.push_back({Level, Context, Message}); }
std::vector<Diagnostics::Entry> LogBuffer::Take() { return std::move(Lines); }

void LogReplay(const std::vector<Diagnostics::Entry> &Lines)
{
    for (const auto &E : Lines) Log(E.Level, E.Context, E.Message);
}

void Log(LogLevel level, const std::string& context, const std::string& message)
{
    if (g_Buffer) { g_Buffer->Keep(level, context, message); return; }   // a parallel loop's item: logged in order later

    //Format current wall-clock time as HH:MM:SS for the log prefix (and the milliseconds of the same instant).
    const auto Now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(Now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);                                   // the reentrant forms: lines are logged from worker threads
#else
    localtime_r(&t, &tm);
#endif
    char timebuf[16];
    std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);
    if (g_LogMillis)   // the CLI's -v: where the time goes, to the millisecond
    {
        const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(Now.time_since_epoch()).count() % 1000;
        std::snprintf(timebuf + 8, sizeof(timebuf) - 8, ".%03d", static_cast<int>(Ms));
    }

    //ANSI color codes: reset to default, then pick the severity color.
    const char* reset = "\033[0m";
    const char* color;
    const char* label;

    //Each severity maps to a distinct color so the log is scannable at a glance:
    //  ERR  → red, WARN → yellow, SUCC → green, OUT → no color (terminal default).
    switch (level)
    {
        case LogLevel::ERR:  color = "\033[31m"; label = "ERR "; break;
        case LogLevel::WARN: color = "\033[33m"; label = "WAR "; break;
        case LogLevel::SUCC: color = "\033[32m"; label = "SUC "; break;
        default:             color = reset;       label = "OUT "; break;
    }

    //Print: timestamp  [LABEL] context message  (reset clears the color at EOL)
    //LOGS GO TO STDERR. stdout is reserved for machine-readable output (`--cid` prints "<cid>\t<path>", the
    //remint prints its CID table), so those verbs stay pipeable — mixing the two made `--cid … | cut` return
    //timestamps instead of a CID.
    if (g_EchoStderr)   // one write per line, so lines from two threads never interleave mid-line
        std::cerr << (std::string(timebuf) + " " + color + "[" + label + "] " + context + " " + message + reset + "\n") << std::flush;

    if (g_Quiet) return;                                    // a look-ahead (QuietLog): stderr only

    if (g_Recorder && (level == LogLevel::WARN || level == LogLevel::ERR)) g_Recorder->Note(level, context, message);

    //Tally BEFORE the sink: counting here means no caller can opt out and no callback arrangement can hide a
    //warning. This is the mechanism that makes a quiet per-launch error impossible to miss.
    Diagnostics::Note(level, context, message);

    if (g_Sink) g_Sink->Note(level, context, message);   // this thread's own (a launch's console)
    //Forward to the optional UI callback with the raw (uncolored) values.
    LogCallback Sink;
    { std::lock_guard<std::mutex> G(g_LogCallbackMtx); Sink = g_LogCallback; }
    if (Sink) Sink(level, context, message);
}

// ---------------------------------------------------------------------------
// Diagnostics tally (see commonutils.h).
// ---------------------------------------------------------------------------
namespace {
std::mutex                 g_DiagMtx;
bool                       g_DiagOn = false;
Diagnostics::Report        g_Diag;
//Deduplicate by context+message: a warning emitted once per file in a 900-entry loop is ONE authoring bug, and
//900 identical lines would bury the other findings. The cap then bounds memory for a pathological run.
std::set<std::string>      g_DiagSeen;
constexpr size_t           kDiagCap = 200;
}

namespace Diagnostics
{
void Note(LogLevel Level, const std::string &Context, const std::string &Message)
{
    if (Level != LogLevel::WARN && Level != LogLevel::ERR) return;
    std::lock_guard<std::mutex> G(g_DiagMtx);
    if (!g_DiagOn) return;
    (Level == LogLevel::ERR ? g_Diag.Errors : g_Diag.Warnings)++;
    const std::string Key = Context + '\x1f' + Message;
    if (g_DiagSeen.count(Key) || g_Diag.Entries.size() >= kDiagCap) return;
    g_DiagSeen.insert(Key);
    g_Diag.Entries.push_back({Level, Context, Message});
}

void Begin()
{
    std::lock_guard<std::mutex> G(g_DiagMtx);
    g_Diag = Report{};
    g_DiagSeen.clear();
    g_DiagOn = true;
}

Report Take()
{
    //TAKES the report: hands it over AND clears it. Leaving the tally behind would let a later Peek() report a
    //previous launch's verdict as if it were this one's — a stale-state footgun in exactly the mechanism whose
    //whole job is to be trustworthy.
    std::lock_guard<std::mutex> G(g_DiagMtx);
    Report Out = std::move(g_Diag);
    g_Diag = Report{};
    g_DiagSeen.clear();
    g_DiagOn = false;
    return Out;
}

Report Peek()
{
    std::lock_guard<std::mutex> G(g_DiagMtx);
    return g_Diag;
}

bool Collecting()
{
    std::lock_guard<std::mutex> G(g_DiagMtx);
    return g_DiagOn;
}

Report ReportVerdict(const std::string &What) { return PrintVerdict(What, Take()); }

Report Summarize(const std::vector<Entry> &Lines)
{
    Report R;
    std::set<std::string> Seen;
    for (const Entry &E : Lines)
    {
        if (E.Level != LogLevel::WARN && E.Level != LogLevel::ERR) continue;
        (E.Level == LogLevel::ERR ? R.Errors : R.Warnings)++;
        if (Seen.insert(E.Context + '\x1f' + E.Message).second && R.Entries.size() < kDiagCap) R.Entries.push_back(E);
    }
    return R;
}

Report PrintVerdict(const std::string &What, Report R)
{
    //Printed for EVERY run, clean or not. A summary that only appears on failure trains you to ignore its
    //absence; "0 warnings" is a claim, and one worth making explicitly.
    LogOut("LaunchDiagnostics", "══════════════════════════════════════════════════════════════");
    if (R.Empty())
        LogSucc("LaunchDiagnostics", What + " completed with 0 warnings and 0 errors.");
    else
    {
        LogErr("LaunchDiagnostics", "!!  " + What + " produced " + std::to_string(R.Errors)
               + " ERROR(S) and " + std::to_string(R.Warnings) + " WARNING(S).");
        LogErr("LaunchDiagnostics", "!!  Something was silently not applied — the run may LOOK fine and be wrong.");
        LogErr("LaunchDiagnostics", "!!  Distinct messages (" + std::to_string(R.Entries.size()) + "):");
        for (const auto &E : R.Entries)
            Log(E.Level, "LaunchDiagnostics", "!!    [" + E.Context + "] " + E.Message);
        LogErr("LaunchDiagnostics", "!!  Fix these. A run that 'works' while logging errors is how a bug hides for months.");
    }
    LogOut("LaunchDiagnostics", "══════════════════════════════════════════════════════════════");
    return R;
}
} // namespace Diagnostics

std::string HumanBytes(long long Bytes)
{
    if (Bytes < 0) return "?";
    static const char *U[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double V = double(Bytes); int I = 0;
    while (V >= 1024.0 && I < 5) { V /= 1024.0; ++I; }
    char Buf[32];
    std::snprintf(Buf, sizeof Buf, (I > 0 && V < 10.0) ? "%.1f %s" : "%.0f %s", V, U[I]);
    return Buf;
}

bool WildcardMatch(const std::string &Pattern, const std::string &Name)
{
    const auto Low = [](char C) { return (char)std::tolower((unsigned char)C); };
    size_t P = 0, N = 0, Star = std::string::npos, Mark = 0;
    while (N < Name.size())
    {
        if (P < Pattern.size() && (Pattern[P] == '?' || Low(Pattern[P]) == Low(Name[N]))) { ++P; ++N; }
        else if (P < Pattern.size() && Pattern[P] == '*') { Star = P++; Mark = N; }
        else if (Star != std::string::npos) { P = Star + 1; N = ++Mark; }   // let the last '*' take one more
        else return false;
    }
    while (P < Pattern.size() && Pattern[P] == '*') ++P;
    return P == Pattern.size();
}
