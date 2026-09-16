#ifndef PERSISTLAYER_H
#define PERSISTLAYER_H

#include "launchparams.h"   // ContainerParams

// PersistLayer — single-file persist seed/capture (a file DeclarePersist target), lifted out of ContainerWrapper.
// The directory-persist (live RW passthrough) and whole-runtime (empty-PATH) cases are handled by the VFS layer spec;
// the registry persist cases live in RegistryLayer. Free functions over the shared ContainerParams.
namespace PersistLayer
{
//Seeds each previously-persisted file (UserDataPath/<Target>) into WriteLayerPath/<Path> before MountVFS so it
//shadows the lower layers. No-op when none persisted yet.
bool SeedPersistFiles(struct ContainerParams &ContainerParams);

//Copies each persisted file from RuntimePath/<Path> into UserDataPath/<Target> on Cleanup, capturing the session's
//writes. Must run BEFORE the runtime is unmounted/wiped. No-op when no file persists are declared.
bool CapturePersistFiles(struct ContainerParams &ContainerParams);
}

#endif // PERSISTLAYER_H
