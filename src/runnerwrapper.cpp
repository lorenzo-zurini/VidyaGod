#include "runnerwrapper.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <filesystem>
#include <system_error>

namespace RunnerWrapper {

std::string DefPrefixDir(const std::string &BundleDir)
{
    if (BundleDir.empty()) return std::string();
    const std::filesystem::path Dir = std::filesystem::path(BundleDir) / "DEFPREFIX";
    // One-time migration from the legacy "__DEFPREFIX__" name: rename the whole artifact dir in place (its `default`
    // subtree + completion sentinel move with it) so an existing install is adopted without a costly wineboot rebuild.
    // Cheap (a stat in the common case where DEFPREFIX already exists); idempotent + race-safe (a lost rename no-ops).
    std::error_code Ec;
    const std::filesystem::path Legacy = std::filesystem::path(BundleDir) / "__DEFPREFIX__";
    if (!std::filesystem::exists(Dir, Ec) && std::filesystem::exists(Legacy, Ec))
        std::filesystem::rename(Legacy, Dir, Ec);
    return QDir::cleanPath(QString::fromStdString((Dir / "default").string())).toStdString();
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
