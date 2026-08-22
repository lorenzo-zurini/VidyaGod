#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <string>
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

#endif // COMMONUTILS_H
