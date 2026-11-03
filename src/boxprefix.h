#ifndef BOXPREFIX_H
#define BOXPREFIX_H

// ---------------------------------------------------------------------------
// BoxPrefix — on Windows, a game's Sandboxie box is its prefix, the way a Wine prefix is under Proton. The game folder
// stays on the vidyagodfs mount (granted to the box, served zero-copy); everything a package places OUTSIDE it — a DLL
// in %SysDir32%, a config under %AppData%, fonts — is copied into the box before the game starts, where the boxed game
// finds it at the machine's real path (Sandboxie shows a box's files over the real ones). The package's registry is
// imported into the box. After the game, what it KEEPs outside its game folder is copied back out of the box, and the
// box is emptied: every launch starts pristine, as on Linux. The box and a kept folder are written by the game: nothing
// here follows a link in them (walks, copies and deletes stop at one).
//
// The runtime lays guest paths out by the runner's DRIVES ({"C:": "drive_c"}): C:\Users\u\AppData\Roaming\x is placed
// at drive_c/Users/u/AppData/Roaming/x, and lands in the box where Sandboxie keeps that real path (BoxRelForReal).
// Everything here is plain files and text — the Sandboxie calls are SandboxLayer's — so it is testable anywhere.
// ---------------------------------------------------------------------------

#include "launchparams.h"   // PersistTarget

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class RegistryWrapper;

namespace BoxPrefix {

// Where a box keeps its copy of a real path, relative to the box root (Sandboxie-Plus 1.18, measured on Windows 11):
// the user's profile under user/current, the all-users folder (C:\ProgramData) under user/all, the public profile under
// user/public, any other path under drive/<letter>. Host = Platform::HostFolders(). "" when RealPath names no drive.
std::string BoxRelForReal(const std::string &RealPath, const std::map<std::string, std::string> &Host);

// The real path a runtime-relative layout path stands for, through DRIVES (a drive -> its layout root, resolved):
// "drive_c/Users/u/x" -> "C:\Users\u\x". "" when no drive lays it out.
std::string RealForLayout(const std::string &LayoutRel, const nlohmann::ordered_json &Drives);

// True when LayoutRel is in the box's part of the runtime: below a drive's layout root, and not in the game folder
// (ContentRoot, which the game reads live from the mount).
bool InBox(const std::string &LayoutRel, const std::string &ContentRoot, const nlohmann::ordered_json &Drives);

// Copies every file the runtime has below a drive's layout root, but for the game folder, into BoxRoot (BoxRelForReal).
// Errors names each file not copied; returns false when there is one. *Placed gets the box path of each file copied:
// what the box was given, the only files a later SyncKeepsBack may take for deleted when the box no longer has them.
bool Materialise(const std::filesystem::path &RuntimePath, const std::string &ContentRoot, const nlohmann::ordered_json &Drives,
                 const std::map<std::string, std::string> &Host, const std::filesystem::path &BoxRoot,
                 std::vector<std::string> &Errors, std::vector<std::string> *Placed = nullptr);

// After the game: each kept folder or file in the box's part of the runtime is copied from the box back to its runtime
// path, where the usual capture (the kept folder's passthrough, PersistLayer::CapturePersistFiles) stores it. A file the
// box was given (Placed, from Materialise) and no longer has, the game deleted: it is deleted from the runtime too.
// Nothing else is ever taken for deleted — a file whose copy into the box failed, an empty folder, a box that is
// missing. Errors names what could not be synced; returns false when there is one.
bool SyncKeepsBack(const std::filesystem::path &RuntimePath, const std::string &ContentRoot, const nlohmann::ordered_json &Drives,
                   const std::map<std::string, std::string> &Host, const std::filesystem::path &BoxRoot,
                   const std::vector<PersistTarget> &KeepDirs, const std::vector<PersistTarget> &KeepFiles,
                   const std::vector<std::string> &Placed, std::vector<std::string> &Errors);

// Before the game: the kept files in the box's part of the runtime (each file below a kept folder, each kept file or
// pattern match) that the box does not hold whole — missing, a link, or another size — as "runtime -> box" lines. A
// launch must not start with any: the game would write its defaults, and the capture after it would store them over
// the user's.
std::vector<std::string> KeptNotInBox(const std::filesystem::path &RuntimePath, const std::string &ContentRoot,
                                      const nlohmann::ordered_json &Drives, const std::map<std::string, std::string> &Host,
                                      const std::filesystem::path &BoxRoot, const std::vector<PersistTarget> &KeepDirs,
                                      const std::vector<PersistTarget> &KeepFiles);
// Whether P is a link: a symlink, or on Windows any reparse point (a junction too; MinGW's std::filesystem cannot tell).
bool IsLink(const std::filesystem::path &P);

// Deletes Root and everything below it, removing each link met as the link (never what it names). Errors names what
// stayed; true when nothing did.
bool RemoveTree(const std::filesystem::path &Root, std::vector<std::string> &Errors);

// The registry as a "Windows Registry Editor Version 5.00" file (UTF-8 text; write it with Utf8ToUtf16Le for reg.exe).
// HKU\.Default is left out: importing it needs rights a game's box does not have. A string with a line break is
// written hex(1), as regedit does (quoted, it would break the file). *Skipped counts what was left out
// (that hive's keys, and values of a type the format cannot carry).
std::string ToRegFile(const RegistryWrapper &RW, size_t *Skipped = nullptr);
// Reads a "Windows Registry Editor Version 5.00" file (UTF-8 text) — what reg.exe export writes — into Out. Deletions
// ([-key], "v"=-) are ignored. False (with Err) on a line it cannot read.
bool FromRegFile(const std::string &Text, RegistryWrapper &Out, std::string &Err);

// reg.exe writes and reads UTF-16LE with a byte-order mark.
std::string Utf8ToUtf16Le(const std::string &Utf8);
std::string Utf16LeToUtf8(const std::string &Bytes);

} // namespace BoxPrefix

#endif // BOXPREFIX_H
