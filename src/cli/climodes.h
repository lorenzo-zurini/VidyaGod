#ifndef CLIMODES_H
#define CLIMODES_H

#include "manifestmodel.h"
#include "ipfswrapper.h"
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <QDir>

struct LaunchParameters;

//The headless CLI mode families, extracted from the former ~1300-line main() (Overhaul P6). Each Run*
//executes its family's mode when the parsed flags select one and returns the process exit code; -1 means
//"no mode of mine was requested" and main() falls through to the next family / the GUI.
namespace CliModes {
int RunIpfsModes       (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
int RunContentModes    (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);

// Catalog-wide target collection — the --download-all / full-mirror pump, split out so it is testable: it performs
// NO fetches (CollectContentTargets may seed an already-present local cover, nothing more). Pools EVERY node's
// content closure + every runner node's build into Out (FetchTargetsConcurrent dedups by CID, so closure overlap
// is free); a launchable's runner chain is made of catalog nodes, so the every-node walk already covers it.
// Best-effort per node: a layer that is missing with no IPFS source is COUNTED (Stats.SourcelessNodes, first few
// reported via OnSourceless) but never fatal — a full mirror must not abort on one un-sourced runner prefix
// (%DefaultPfxDir% is generated at launch).
struct CatalogTargetStats { int Nodes = 0; int Launchables = 0; int Runners = 0; int SourcelessNodes = 0; };
void CollectCatalogTargets(const NodeIndex &Idx,
                           std::vector<IpfsWrapper::FetchTarget> &Out, CatalogTargetStats *Stats = nullptr,
                           const std::function<void(const std::string &NodeId, const std::string &Err)> &OnSourceless = nullptr);
int RunMaintenanceModes(LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
// --audit-packages: resolve EVERY launchable in-process and report the warnings/errors nobody reads, plus static
// checks for authoring that silently does nothing. Non-zero exit when anything was found (so it can gate a
// publish). See cliaudit.cpp for why static validation is not enough.
int RunAuditPackages(nlohmann::ordered_json &GlobalConfigJSON, const std::string &Scope = {});
int RunNodeLaunch      (LaunchParameters &LP, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir);
}

#endif // CLIMODES_H
