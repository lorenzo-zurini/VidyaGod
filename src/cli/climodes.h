#ifndef CLIMODES_H
#define CLIMODES_H

#include "manifestmodel.h"
#include "ipfswrapper.h"
#include "commonutils.h"   // Diagnostics::Report
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <QDir>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct LaunchParameters;
class ContainerWrapper;

//The headless CLI mode families, extracted from the former ~1300-line main() (Overhaul P6). Each Run*
//executes its family's mode when the parsed flags select one and returns the process exit code; -1 means
//"no mode of mine was requested" and main() falls through to the next family / the GUI.
namespace LaunchSession { class Session; }

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
//One launch of a node (or of a tile's row): resolve it and — unless ResolveOnly — build its runtime, run it to its
//exit and take the runtime down; what --node / --tile do, and the `launch` commands. Returns the exit code (0: a
//clean resolve, or the game's own code; 1: it did not resolve, build or start). Resolution, when given, receives the
//resolved launch (paths, runner chain, layers, variables, …) once it resolved.
struct LaunchRequest
{
    std::string LaunchNodeId, LaunchTile, LaunchVariant, Entrypoint, InstanceName;
    std::vector<std::string> RunnerChain;
    std::optional<std::vector<std::string>> Grafts;
    std::map<std::string, std::string> VariableOverrides;
    bool ResolveOnly = false;
};
//Verdict: the warnings and errors the launch logged, for a caller that shows them (they are printed regardless).
int Launch(const LaunchRequest &Req, nlohmann::ordered_json &GlobalConfigJSON, QDir &AppDataDir,
           nlohmann::ordered_json *Resolution = nullptr, Diagnostics::Report *Verdict = nullptr);
//The first half of a launch: the node found and its session resolved (no mount, nothing run). Null with Err when it
//does not resolve. The rest is the session's: Build, Run, Cleanup, Verdict. Index: the catalog, when loaded already.
struct PreparedLaunch
{
    std::shared_ptr<const NodeIndex> Index;
    std::unique_ptr<LaunchSession::Session> S;
    std::string Name;
    ~PreparedLaunch();
};
std::unique_ptr<PreparedLaunch> PrepareLaunch(const LaunchRequest &Req, nlohmann::ordered_json &GlobalConfigJSON, std::string &Err,
                                              std::shared_ptr<const NodeIndex> Index = nullptr);
nlohmann::ordered_json ResolutionOf(PreparedLaunch &P);   // the resolved launch, as --resolve-only writes it
}

#endif // CLIMODES_H
