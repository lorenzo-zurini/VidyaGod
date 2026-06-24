#ifndef PERSISTLAYER_H
#define PERSISTLAYER_H

#include "launchparams.h"   // ContainerParams

// PersistLayer — KEEP-file seed/capture (a single-file persist target), lifted out of ContainerWrapper. The
// KEEP-dir (live RW passthrough) and MODE:all (whole-runtime) cases are handled by the VFS layer spec; the
// registry KEEP cases live in RegistryLayer. Free functions over the shared ContainerParams.
namespace PersistLayer
{
//Seeds each previously-persisted KEEP file (UserDataPath/<rel>) into WriteLayerPath/<rel> before MountVFS so it
//shadows the lower layers. No-op under MODE:all or when none persisted yet.
bool SeedPersistFiles(struct ContainerParams &ContainerParams);

//Copies each KEEP file from RuntimePath/<rel> into UserDataPath/<rel> on Cleanup, capturing the session's
//writes. Must run BEFORE the runtime is unmounted/wiped. No-op under MODE:all.
bool CapturePersistFiles(struct ContainerParams &ContainerParams);
}

#endif // PERSISTLAYER_H
