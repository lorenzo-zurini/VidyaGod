#include "commonutils.h"

void Log(LogLevel level, const std::string& context, const std::string& message)
{
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    char timebuf[9];
    std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

    const char* reset = "\033[0m";
    const char* color;
    const char* label;

    switch (level)
    {
        case LogLevel::ERR:  color = "\033[31m"; label = "ERR "; break;
        case LogLevel::WARN: color = "\033[33m"; label = "WAR "; break;
        case LogLevel::SUCC: color = "\033[32m"; label = "SUC "; break;
        default:             color = reset;       label = "OUT "; break;
    }

    std::cout << timebuf << " " << color << "[" << label << "] " << context << " " << message << reset << std::endl;
}
