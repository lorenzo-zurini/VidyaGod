#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <string>
#include <vector>
#include <iostream>
#include <ctime>
#include <functional>

//Severity levels used by the Log() function.
//Determines the ANSI color and label printed to stdout.
enum class LogLevel { OUT = 1, SUCC = 2, WARN = 3, ERR = 4 };

//Core logging function — formats a timestamped, color-coded line to stdout.
//context is the calling function/class name; message is the human-readable detail.
void Log(LogLevel level, const std::string& context, const std::string& message);

//Optional secondary sink for Log() output. When set, every Log() call also invokes
//this callback with the same level/context/message so UI components can display live logs.
using LogCallback = std::function<void(LogLevel, const std::string&, const std::string&)>;
void SetLogCallback(LogCallback callback);
void ClearLogCallback();

// ---------------------------------------------------------------------------
// Diagnostics tally — every WARN/ERR that passes through Log(), counted.
//
// WHY: a launch prints hundreds of lines, so a single warning in the middle is invisible. Worms 4 Mayhem logged
// "Could not open file for reading" on EVERY launch for months; the only visible symptom was a wrong aspect
// ratio, and nobody read the log. Counting inside Log() itself means no code path can opt out and no callback
// arrangement can hide it — the tally is taken before the sink is even consulted.
//
// Usage: Diagnostics::Begin() at the start of an operation, Diagnostics::Take() at the end; if it is non-empty,
// say so LOUDLY. LaunchThread does exactly this around a game launch.
// ---------------------------------------------------------------------------
namespace Diagnostics
{
    struct Entry { LogLevel Level; std::string Context, Message; };
    struct Report
    {
        size_t Warnings = 0, Errors = 0;
        std::vector<Entry> Entries;                    // deduplicated, capped
        bool Empty() const { return Warnings == 0 && Errors == 0; }
    };
    //Called by Log() for every line; not for direct use.
    void   Note(LogLevel Level, const std::string &Context, const std::string &Message);
    void   Begin();          // start (or restart) collecting on this process
    Report Take();           // snapshot + stop collecting
    Report Peek();           // snapshot without stopping
    bool   Collecting();
    //Take the report and print the VERDICT for `What`, loudly when it is non-empty. Shared by every launch path
    //so the GUI and the CLI say the same thing — the CLI is what tooling and debugging use, and it had no tally
    //at all until this existed. Returns the report so the caller can also surface it in a UI.
    Report ReportVerdict(const std::string &What);
}

//Convenience wrappers so callers don't have to specify the enum each time.
//All four delegate directly to Log() with no extra overhead.
inline void LogOut (const std::string& ctx, const std::string& msg) { Log(LogLevel::OUT,  ctx, msg); }
inline void LogErr (const std::string& ctx, const std::string& msg) { Log(LogLevel::ERR,  ctx, msg); }
inline void LogWarn(const std::string& ctx, const std::string& msg) { Log(LogLevel::WARN, ctx, msg); }
inline void LogSucc(const std::string& ctx, const std::string& msg) { Log(LogLevel::SUCC, ctx, msg); }

//Human-readable byte size ("1.4 GB"); "?" for a negative (unknown) count. THE single implementation —
//it was copy-pasted in three files (ipfswrapper/downloadmanager/ipfstab); QString callers wrap with
//QString::fromStdString.
std::string HumanBytes(long long Bytes);

//Structured launch-step marker: context "LaunchStep", message "<k>/<n> <label>". This is a CONTRACT, not
//log phrasing: the prelaunch progress bar/status parse exactly this shape (LaunchThread), so the engine
//announces every phase through here — never by ad-hoc log-line heuristics.
inline void LogStep(int K, int N, const std::string &Label)
{
    Log(LogLevel::OUT, "LaunchStep", std::to_string(K) + "/" + std::to_string(N) + " " + Label);
}

#endif // COMMONUTILS_H
