#include "commonutils.h"

//Static callback — null by default. Set via SetLogCallback(); cleared via ClearLogCallback().
//Access is intentionally unsynchronized: the callback is set once from the main thread before
//the worker thread starts, and cleared after it finishes, so no concurrent mutation occurs.
static LogCallback g_LogCallback;

//Installs a secondary log sink. Replaces any previously installed callback.
void SetLogCallback(LogCallback callback)
{
    g_LogCallback = std::move(callback);
}

//Removes the secondary log sink so Log() reverts to stdout-only output.
void ClearLogCallback()
{
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
    std::cout << timebuf << " " << color << "[" << label << "] " << context << " " << message << reset << std::endl;

    //Forward to the optional UI callback with the raw (uncolored) values.
    if (g_LogCallback)
        g_LogCallback(level, context, message);
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
