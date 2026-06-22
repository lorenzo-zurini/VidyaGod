#ifndef PERSISTLAYER_H
#define PERSISTLAYER_H

#include "launchparams.h"   // ContainerParams

// PersistLayer — selective file-persistence (PersistFile) seed/capture, lifted out of ContainerWrapper. The
// directory-persist (PersistDir) and whole-runtime (PersistAll) cases are handled by the VFS layer spec; the
// registry-persist cases live in RegistryLayer. Free functions over the shared ContainerParams.
namespace PersistLayer
{
//Seeds each previously-persisted PersistFile (UserDataPath/<rel>) into WriteLayerPath/<rel> before MountVFS so it
//shadows the lower layers. No-op when PersistAll or none persisted yet.
bool SeedPersistFiles(struct ContainerParams &ContainerParams);

//Copies each PersistFile from RuntimePath/<rel> into UserDataPath/<rel> on Cleanup, capturing the session's
//writes. Must run BEFORE the runtime is unmounted/wiped. No-op when PersistAll.
bool CapturePersistFiles(struct ContainerParams &ContainerParams);
}

#endif // PERSISTLAYER_H
