#ifndef REGISTRYLAYER_H
#define REGISTRYLAYER_H

#include "launchparams.h"   // ContainerParams

// RegistryLayer — the Wine prefix + registry/DEFAULTDATA + registry-persistence subsystem, lifted out of
// ContainerWrapper. Free functions over the shared ContainerParams; uses RegistryWrapper for hive I/O.
namespace RegistryLayer
{
//Initializes the Wine prefix at DefPrefixPath by running `wineboot` via the runner. DefPrefixPath becomes the
//base (root) layer of the vidyagodfs spec. Wine/proton runners only.

//Builds the DEFAULTDATA layer (regenerated each launch): all package-encoded BASE (non-OVERRIDE) edits — FileEdits
//as files, RegEdits as full hives copied-from-and-shadowing DEFPREFIX, plus merged persisted KEEP registry-subtrees.
//Mounts between the component layers and the WRITELAYER. DEFPREFIX is never mutated.
bool BuildDefaultData(struct ContainerParams &ContainerParams);

//Applies OVERRIDE:true RegEdit subcomponents to the mounted runtime hives via RegistryWrapper, post-MountVFS.
bool ApplyOverrideRegEdits(struct ContainerParams &ContainerParams);

//Seeds previously-persisted KEEP hive files (UserDataPath/REGISTRY/*.reg) into WriteLayerPath before MountVFS so
//they shadow DEFPREFIX. No-op when PersistAll or no persisted regs exist.
bool SeedPersistRegistry(struct ContainerParams &ContainerParams);

//Copies each KEEP hive (RuntimePath/<prefixroot>/<hive>.reg) into UserDataPath/REGISTRY/ on Cleanup, capturing
//the session's registry. Must run BEFORE the runtime is unmounted/wiped.
bool CapturePersistRegistry(struct ContainerParams &ContainerParams);

//Extracts each KEEP registry-subtree from the mounted RuntimePath hives and merges it into the durable store
//UserDataPath/REGKEYS/*.reg on Cleanup. Must run BEFORE unmount. No-op when PersistAll.
bool CapturePersistRegKeys(struct ContainerParams &ContainerParams);
}

#endif // REGISTRYLAYER_H
