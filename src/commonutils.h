#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <string>
#include <iostream>
#include <ctime>

enum class LogLevel { OUT = 1, SUCC = 2, WARN = 3, ERR = 4 };

void Log(LogLevel level, const std::string& context, const std::string& message);

inline void LogOut (const std::string& ctx, const std::string& msg) { Log(LogLevel::OUT,  ctx, msg); }
inline void LogErr (const std::string& ctx, const std::string& msg) { Log(LogLevel::ERR,  ctx, msg); }
inline void LogWarn(const std::string& ctx, const std::string& msg) { Log(LogLevel::WARN, ctx, msg); }
inline void LogSucc(const std::string& ctx, const std::string& msg) { Log(LogLevel::SUCC, ctx, msg); }

#endif // COMMONUTILS_H
