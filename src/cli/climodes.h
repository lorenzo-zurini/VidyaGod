#ifndef CLIMODES_H
#define CLIMODES_H

#include <nlohmann/json.hpp>
#include <QDir>

struct LaunchParameters;

//The headless CLI mode families, extracted from the former ~1300-line main() (Overhaul P6). Each Run*
//executes its family's mode when the parsed flags select one and returns the process exit code; -1 means
//"no mode of mine was requested" and main() falls through to the next family / the GUI.
namespace CliModes {
int RunIpfsModes       (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
int RunContentModes    (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
int RunMaintenanceModes(LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
// --audit-packages: resolve EVERY launchable in-process and report the warnings/errors nobody reads, plus static
// checks for authoring that silently does nothing. Non-zero exit when anything was found (so it can gate a
// publish). See cliaudit.cpp for why static validation is not enough.
int RunAuditPackages(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Scope = {});
int RunNodeLaunch      (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
}

#endif // CLIMODES_H
