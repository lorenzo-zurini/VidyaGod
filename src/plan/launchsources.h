#ifndef LAUNCHSOURCES_H
#define LAUNCHSOURCES_H

#include <string>
#include <vector>

#include "launchparams.h"   // ContainerParams

// LaunchSources — dependency pre-flight + content materialization (the IPFS-facing readiness/fetch checks),
// lifted out of ContainerWrapper. Free functions over the shared ContainerParams.
namespace LaunchSources
{
//Returns unresolved dependency locators (missing VFS layer sources) for a built container — drives the
//portable/standalone readiness warning.
std::vector<std::string> VerifyDependencies(const struct ContainerParams &ContainerParams);

//Pre-flight CHECK (never fetches; GUI-thread safe) that every dependency is satisfiable — runner build CIDs cached,
//the wine DEFPREFIX artifact present, and every game VFS layer either local or backend-fetchable. Returns false
//(blocking the launch) otherwise.
bool EnsureSources(struct ContainerParams &ContainerParams);

//Worker-thread MATERIALIZER (may block on a download): fetches each game VFS layer whose local content is missing
//from a backend (IPFS) straight to its expected local PATH. Returns false if a layer can't be fetched.
bool MaterializeLayers(struct ContainerParams &ContainerParams);
}

#endif // LAUNCHSOURCES_H
