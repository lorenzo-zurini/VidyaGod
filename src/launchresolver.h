#ifndef LAUNCHRESOLVER_H
#define LAUNCHRESOLVER_H

#include <nlohmann/json.hpp>

#include "launchparams.h"    // ContainerParams
#include "manifestmodel.h"   // NodeIndex, Node

// LaunchResolver — resolves a launch request into a fully-populated ContainerParams from the global node graph:
// the param / recipe / runner-pick / persistence / exec / path resolution lifted out of ContainerWrapper. Free
// functions over the shared ContainerParams; the %token% engine they rely on lives in VarSubst.
//
// (These are Qt + PackageCatalog + RunnerWrapper coupled, so they are NOT in the light unit-test target — the
// pure substitution seam in VarSubst is what's unit-tested. They were a verbatim move out of ContainerWrapper.)
namespace LaunchResolver
{
//Native node-graph init — populates ContainerParams + the component pool (ComponentPool, out) DIRECTLY from the
//node graph (ContainerParams.NodeIdx / .LaunchNodeId): runner pick, recipe, paths, custom vars, persistence.
bool InitializeFromNode(struct ContainerParams &ContainerParams, nlohmann::ordered_json &ComponentPool, const nlohmann::ordered_json &GlobalConfigJSON);

//Picks the best ROLE:"runner" node for a launch node (GUEST ∋ launch host, HOST==machine, executable available),
//priority: explicit RunnerID pin > USERSETTINGS PREFERRED_RUNNER > first qualifying (node-id order). nullptr if none.
const Node *PickRunnerNode(const NodeIndex &Idx, const Node &Launch, const struct ContainerParams &CP, const nlohmann::ordered_json &GlobalConfigJSON);

//Derives the session paths (Temp/Runtime/WriteLayer/DefaultData/UserData/Program/DefPrefix + ContentRoot
//resolution + PrefixRoot + screen geometry) from already-set ContainerParams fields.
bool DerivePaths(struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);

//Reads the launch node's EXEC (CONTENTPATH/EXEARGS/WORKDIR) into the exec fields. After BuildSubComponentsArray
//and before BuildContainerRuntime.
bool ResolveExecutableDefinition(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

//Resolves every CustomVar subcomponent in the Recipe (priority: CLI override > USERSETTINGS > DEFAULT). Must run
//before BuildSubComponentsArray so custom %KEY% tokens are available for substitution.
bool ResolveCustomVariables(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams, const nlohmann::ordered_json &GlobalConfigJSON);

//Derives persistence (PersistAll vs declared PersistDir/PersistFile/RegPersist/RegKeyPersist) from the Recipe.
bool DerivePersistence(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);

//Collects all SUBCOMPONENTS from the Recipe's components into SubComponentsArray (with %VAR% substitution),
//skipping CustomVar / Persist* (handled by ResolveCustomVariables / DerivePersistence).
bool BuildSubComponentsArray(const nlohmann::ordered_json &MANIFESTJSON, struct ContainerParams &ContainerParams);
}

#endif // LAUNCHRESOLVER_H
