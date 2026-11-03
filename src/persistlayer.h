#ifndef PERSISTLAYER_H
#define PERSISTLAYER_H

#include "launchparams.h"   // ContainerParams

#include <filesystem>
#include <string>

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

//Whether the user has a saved copy of the runtime-relative file RuntimeRel: a kept single file whose durable copy
//exists, or a file under a kept directory present in its durable home — or deleted there by the game (a vidyagodfs
//deletion marker). A package default for a user-owned file (an EDIT lowered with IF_UNSAVED) applies only while there
//is none.
bool HasSavedCopy(const struct ContainerParams &ContainerParams, const std::string &RuntimeRel);

//Readies Root/RelFile for this process to write as a plain file, never through a link: every folder below Root is
//made a real folder (created when missing, a link in its place removed and replaced) and a link at RelFile itself is
//removed. The places kept data is written to (the instance folder, the session's writelayer) are, or were, reachable
//by the sandboxed game, which can leave a link there to any file of the user's; a write that followed it would put
//the game's bytes in that file. A link holds no data, so replacing one loses nothing. Root itself is trusted (created
//when missing). False, with Why, when RelFile is not a plain relative path or a component is some other file.
bool PrepareWritePath(const std::filesystem::path &Root, const std::filesystem::path &RelFile, std::string &Why);

//Name, with control characters (a game can choose a link target holding terminal escapes) shown as '?': for a log.
std::string Printable(std::string Name);

//The same for a folder written file by file by code that follows links (the registry store): Root/RelDir is made a
//real folder as above, and everything directly inside it that is neither a plain file nor a folder (a link, a FIFO
//that would block the reader) is removed.
bool PrepareWriteDir(const std::filesystem::path &Root, const std::filesystem::path &RelDir, std::string &Why);
}

#endif // PERSISTLAYER_H
