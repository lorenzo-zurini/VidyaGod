#include "commonutils.h"

//Writes a single log line to stdout with a HH:MM:SS timestamp, a color-coded severity
//label, the calling context, and the message. Uses ANSI escape codes for terminal color.
//All output goes to stdout so it can be captured alongside QProcess output in one stream.
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
}
