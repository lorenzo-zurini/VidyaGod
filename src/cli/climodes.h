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
int RunNodeLaunch      (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
}

#endif // CLIMODES_H
