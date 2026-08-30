#include "commonutils.h"

#include <mutex>

//Static callback — null by default. Set via SetLogCallback(); cleared via ClearLogCallback().
//MUTEX-GUARDED: the old "set once from the main thread before the worker starts" comment was wrong —
//LaunchThread installs it FROM the worker while other threads are logging, which mutated a live
//std::function under concurrent readers. Log() copies the callback under the lock, then invokes the
//copy outside it (so a slow sink never serializes unrelated logging, and Clear during a call is safe).
static std::mutex   g_LogCallbackMtx;
static LogCallback  g_LogCallback;

//Installs a secondary log sink. Replaces any previously installed callback.
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
void Log(LogLevel level, const std::string& context, const std::string& message)
{
    //Format current wall-clock time as HH:MM:SS for the log prefix.
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char timebuf[9];
    std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

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
    std::cerr << timebuf << " " << color << "[" << label << "] " << context << " " << message << reset << std::endl;

    //Forward to the optional UI callback with the raw (uncolored) values.
    LogCallback Sink;
    { std::lock_guard<std::mutex> G(g_LogCallbackMtx); Sink = g_LogCallback; }
    if (Sink) Sink(level, context, message);
}

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
