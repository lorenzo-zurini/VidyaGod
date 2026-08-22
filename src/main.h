#ifndef MAIN_H
#define MAIN_H
#endif // MAIN_H

//Intentional: includes live outside the guard so this header can be used as a
//forward-declaration-only unit; the actual types are pulled in by the includes below.
#include <filesystem>
#include <iostream>
#include <fstream>
#include <list>
#include <map>
#include <vector>

#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QMessageBox>
#include <QApplication>
#include <QStyleFactory>
#include <QFile>

#include "nlohmann/json.hpp"
#include "filesystemoperations.h"
#include "mainwindow.h"
#include "jsonoperations.h"

//Holds all parameters resolved from command-line arguments and auto-detection at startup.
//Populated once by ParseCommandLineArguments() and consulted by main() to decide
//whether to run headless (launch a package directly) or open the full GUI.
struct LaunchParameters
{
    std::filesystem::path CurrentPath = std::filesystem::current_path(); //Working directory at launch — used to auto-detect package dirs
    bool RunningHeadless = false;                                         //True when no GUI should be shown
    bool RunningInPackageDir = false;                                     //True when auto-detected as running inside a package directory
    bool StartInTray = false;                                            //--tray: come up hidden in the system tray (used by the start-on-login autostart entry)
    bool HasHeadlessPackagePath = false;                                  //True when a package path has been resolved (CLI or auto-detect)
    std::filesystem::path HeadlessPackagePath;                            //Path to the package to launch in headless mode
    std::string LaunchNodeId;                                            //--node <NODE_ID>: launch a launchable node from the global node graph (everything-is-a-node)
    bool ResolveOnly = false;                                            //--resolve-only <NODE_ID>: resolve the node graph + dump ContainerParams to a file, then exit (no mount/launch) — golden-compare + hang-free verification
    bool ValidateNodes = false;                                          //--validate-nodes [pkg]: validate the node graph (dangling/cyclic PARENTS, layer PATHs, runner resolution, ...) then exit
    std::string ValidateScope;                                           //optional package UID / bundle dir / node id after --validate-nodes: validate only that package + its PARENTS closure (fast pre-publish check), instead of the whole catalog
    bool FixCaseConflicts = false;                                       //--fix-case-conflicts: canonicalize cross-layer case-colliding zip entries (unpack/rename/repackage) so patches/add-ons override cleanly, then exit
    bool ListNodes = false;                                              //--list-nodes: print the presentable library tiles (grouped launchable nodes) + hydration status, then exit
    std::vector<std::string> ConvertDeltaChain;                          //--convert-delta-chain <baseNode> <node2> ...: reduce a version chain to a base full zip + per-version .vgdelta layers (byte-verified), then exit
    std::string SeedDir;                                                  //--seed <DIR>: add a folder's published content (LAYER/COVER SOURCE CIDs) to the IPFS node by reference, then exit
    bool SeedCoversOnly = false;                                          //--seed-covers <DIR>: like --seed but ONLY re-pins META.COVER refs (skips the big layers)
    bool SeedOverwrite = false;                                           //--overwrite: seed in OVERWRITE mode (re-reference every file; default is additive — only new/orphaned)
    std::string FetchCid;                                                 //--fetch <CID> <DEST>: fetch a CID to DEST then exit (download throughput test)
    std::string FetchDest;
    bool FetchDirMode = false;                                            //--fetch-dir <CID> <DEST>: recursively materialize a FOLDER CID to DEST (add-by-CID verify)
    bool PrintPeerId = false;                                             //--peer-id: print this node's peer ID + dialable addrs, then exit
    bool PrintPinLs = false;                                              //--pin-ls: print the recursively-pinned (seeded) CIDs in this repo, then exit
    std::string UnpinCid;                                                 //--unpin <cid>: drop a recursive pin (stop seeding/showing it) then exit
    std::string DropRefCid;                                               //--drop-ref <cid>: delete a CID's filestore-reference closure + unpin (so a later add re-references it at a new path) then exit — orphan/stale-ref repair
    std::string ConnectAddr;                                              //--connect <MULTIADDR>: dial this peer before fetching (direct peering / controlled benchmark)
    std::string DataDir;                                                  //--data-dir <PATH>: override the app data dir (testing: second instance with its own repo + lock)
    std::string PackageDirOverride;                                       //--package-dir <PATH>: override the launch package/bundle path
    std::string RuntimeDirOverride;                                       //--runtime-dir <PATH>: override the container runtime (FUSE mount) path
    std::string UserDataDirOverride;                                      //--userdata-dir <PATH>: override the save/persist (USERDATA) path
    std::map<std::string, std::string> VariableOverrides;                 //Custom variable overrides from --var KEY=VALUE flags
    std::map<std::string, bool> ModuleStates;                             //Module toggles from --module COMPONENT=on|off
    std::string VariantID;                                                //Variant override from --variant (selects which variant to build)
    std::string RunnerID;                                                 //Runner override from --runner (RUNNER_ID; = chain front, back-compat)
    std::vector<std::string> RunnerChain;                                 //Runner daisy-chain from repeated --runner (innermost→outermost); empty = auto-resolve
    std::string ImportRunnerId;                                          //--import-runner: install this runner (fetch build + build DEFPREFIX) then exit
    std::string ImportPackageUid;                                        //--import-package: install this catalog game (fetch IPFS content + add to LIBRARY) then exit
    std::string PublishPackageDir;                                       //--publish <pkgdir>: dehydrate this local package (seed content -> CIDs into the manifest) then exit
    std::string PublishToDir;                                            //--publish-to <dir>: also export a manifest-only dehydrated copy there
    std::string PublishCidDir;                                           //--publish-cid <dir>: recursively add a folder to IPFS, print its folder CID (for add-by-CID), then exit
    std::string PublishMetaSrc;                                          //--publish-meta <dir>: mint a JSON-only Meta-CID (bundle or collection) seeded in place from its *.json, print it, then exit
    // ---- friends / multiplayer social layer (headless test harness) ----
    bool PrintFriendCode = false;                                        //--friend-code: print this node's shareable friend code (peer ID), then exit
    bool FriendListOnly = false;                                         //--friend-ls: print the address book (contacts + state), then exit
    std::string FriendAddCode;                                          //--friend-add <CODE>: send a friend request to this peer ID, observe, then exit
    bool FriendServe = false;                                           //--friend-serve: stay online, auto-accept incoming requests, observe events, then exit
    std::string FriendNick;                                            //--friend-nick <NICK>: set this node's profile nickname before add/serve
    int FriendSecs = 30;                                               //--friend-secs <N>: how long the add/serve handlers stay online observing (default 30)
    bool LanHarness = false;                                           //--lan: warm online + print the friend-LAN roster (self + online friends' vIPs) for --friend-secs, then exit
    bool OverlayUp = false;                                             //--overlay: with --lan, bring up the sandboxed overlay TUN and ping the first online friend over the virtual LAN
};

int main(int argc, char *argv[]);

//Loads GlobalConfig.JSON from AppDataDir (or starts empty), then guarantees the used shape via
//EnsureGlobalConfigDefaults: RUNNERS (built-in defaults), an empty LIBRARY, and a Settings object.
//The file is written when created or when a missing key had to be seeded.
//Returns true on failure, false on success (shell exit-code convention).
bool InitializeGlobalConfigJSON(nlohmann::ordered_json * GlobalConfigJSON, QDir * AppDataDir);

//Parses argc/argv and returns a populated LaunchParameters struct.
//Recognized flags: --package <path>, --subgame <id>, --component <id>.
static LaunchParameters ParseCommandLineArguments(int argc, char* argv[]);

//Returns true if CurrentPath looks like a VidyaGod package directory
//(i.e. FSOps::CheckPackageValid passes for it).
//Enables automatic headless launch when the binary is run from inside a package.
static bool IsRunningInPackageDir(const std::filesystem::path &CurrentPath);
