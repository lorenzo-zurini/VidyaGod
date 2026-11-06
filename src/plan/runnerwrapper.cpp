#include "runnerwrapper.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace RunnerWrapper {

std::string DefPrefixDir(const std::string &BundleDir)
{
    if (BundleDir.empty()) return std::string();
    return QDir::cleanPath(QString::fromStdString(BundleDir + "/DEFPREFIX/default")).toStdString();
}

bool ExecutableAvailable(const nlohmann::ordered_json &Exec)
{
    std::string Exe = Exec.value("EXECUTABLE", std::string());
    // Trim surrounding whitespace.
    const auto b = Exe.find_first_not_of(" \t");
    if (b == std::string::npos) return true;                          // empty/blank → native passthrough (runs the game's own exe)
    Exe = Exe.substr(b, Exe.find_last_not_of(" \t") - b + 1);
    if (Exe.find('%') != std::string::npos) return true;             // resolves from a mounted build / %VAR% — not a bare system command
    // A bare command (or an absolute/relative path): must be found on PATH or exist as an executable file.
    return !QStandardPaths::findExecutable(QString::fromStdString(Exe)).isEmpty();
}

} // namespace RunnerWrapper
