#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <string>
#include <iostream>
#include <ctime>

//Severity levels used by the Log() function.
//Determines the ANSI color and label printed to stdout.
enum class LogLevel { OUT = 1, SUCC = 2, WARN = 3, ERR = 4 };

//Core logging function — formats a timestamped, color-coded line to stdout.
//context is the calling function/class name; message is the human-readable detail.
void Log(LogLevel level, const std::string& context, const std::string& message);

//Convenience wrappers so callers don't have to specify the enum each time.
//All four delegate directly to Log() with no extra overhead.
inline void LogOut (const std::string& ctx, const std::string& msg) { Log(LogLevel::OUT,  ctx, msg); }
inline void LogErr (const std::string& ctx, const std::string& msg) { Log(LogLevel::ERR,  ctx, msg); }
inline void LogWarn(const std::string& ctx, const std::string& msg) { Log(LogLevel::WARN, ctx, msg); }
inline void LogSucc(const std::string& ctx, const std::string& msg) { Log(LogLevel::SUCC, ctx, msg); }

#endif // COMMONUTILS_H
