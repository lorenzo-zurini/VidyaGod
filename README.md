# VidyaGod

VidyaGod is a game launcher built around a structured, reproducible package format. Every package is a self-contained directory that describes not just where a game's files are, but the entire runtime environment required to run it — VFS layer stack, registry patches, DLL overrides, config-file edits, and custom variables — in a single declarative JSON manifest. The launcher reads the manifest, builds a temporary union filesystem, applies all configuration in a deterministic order, and launches the game inside the resulting environment.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Dependencies](#2-dependencies)
3. [Building](#3-building)
4. [Usage](#4-usage)
   - [GUI Mode](#gui-mode)
   - [Headless Mode (CLI)](#headless-mode-cli)
5. [Package Directory Structure](#5-package-directory-structure)
6. [MANIFEST.json — Complete Schema Reference](#6-manifestjson--complete-schema-reference)
   - [Top-Level Fields](#top-level-fields)
   - [SUBGAMES](#subgames)
   - [ENTRYPOINTS](#entrypoints)
   - [COMPONENTS](#components)
   - [SUBCOMPONENT Types](#subcomponent-types)
   - [CUSTOMVARS](#customvars)
   - [RUNNERS (Package-Level)](#runners-package-level)
7. [Component Dependency Chain](#7-component-dependency-chain)
8. [Variable Substitution](#8-variable-substitution)
9. [CustomVar Type System](#9-customvar-type-system)
10. [Runner Types](#10-runner-types)
11. [GlobalConfig.JSON](#11-globalconfigjson)
12. [Container Lifecycle](#12-container-lifecycle)
13. [Registry System](#13-registry-system)
14. [CLI Reference](#14-cli-reference)
15. [Package Editor](#15-package-editor)
    - [Opening the Editor](#opening-the-editor)
    - [Tab Structure](#tab-structure)
    - [JSON Tab](#json-tab)
    - [MANIFEST Tab](#manifest-tab)
    - [CUSTOMVARS Tab](#customvars-tab)
    - [Component Tabs](#component-tabs)
    - [Packaging Workflow](#packaging-workflow)
    - [The Analyze Registry Workflow](#the-analyze-registry-workflow)

---

## 1. Overview

### The Problem with Game Preservation

Most game launchers treat software the same way an operating system treats an installed program: one version, one configuration, one set of files on disk. If you want to run a different version of a game, you reinstall it. If a patch breaks something, you hope for an undo. If a game needs a specific registry configuration or a particular DLL override to function, that knowledge lives in someone's head or a forgotten forum post. When the developer's authentication servers go offline or the game is delisted, the ability to run it reliably may disappear entirely. The game files may survive, but the knowledge of how to make them run does not.

This problem compounds for serious preservation: a game like Warcraft III has had over a dozen meaningful patch versions across two decades, each with different behaviour, different network compatibility, and different third-party mod support. An Age of Empires II package may span the original release, the community no-CD patch, multiple unofficial patches, a fan-made FLAC soundtrack, and three separate expansion branches — all sharing the same base installation. A Resident Evil game from 1997 may require a specific dgvoodoo version, a specific DLL override combination, and a Classic REbirth compatibility layer, none of which is documented anywhere a user would easily find.

Traditional launchers have no native way to express any of this. VidyaGod was built specifically to solve it.

---

### The Package Format as a Preservation Standard

A VidyaGod **package** is a self-contained, fully declarative unit of game preservation. Everything required to reproduce a working game environment — not just the files, but the complete runtime configuration — is captured in a single directory:

- The game's files and every patch, expansion, and add-on, stored as compressed archives in `PACKAGEFILES/`
- A `MANIFEST.json` describing exactly how those files layer together, what the registry must contain, which DLLs need overrides, and how the game should be launched
- A `USERDATA/` directory that accumulates only the player's own changes (saves, settings) via copy-on-write, leaving the original files permanently untouched

The game files are **never modified**. The launcher assembles a virtual filesystem from the package's layers at launch time, runs the game inside it, and tears everything down when the game exits. The base files remain exactly as they were when the package was created, indefinitely, regardless of how many times the game is played or how many different configurations are used.

This means a package created today will produce an identical runtime environment in ten years, on any compatible system, without any additional steps. The knowledge of how to run the game is not in someone's memory or a README — it is encoded in the manifest, reproducibly.

---

### What the Package Format Makes Possible

#### Multi-version packages in a single directory

Warcraft III has eight meaningful patch versions between 1.21b and 1.31.1. With VidyaGod, all eight are a single package. Each version's files are a separate ZIP archive. Each version is a separate component. A user opening the PreLaunch window sees a version picker and selects the one they want — the correct files are assembled, the correct registry configuration is applied, and the game launches. Switching versions is a single dropdown change, with no reinstallation, no file duplication, and no risk of corrupting one version while working with another.

The same package also contains both Reign of Chaos and The Frozen Throne as separate subgames, each automatically configuring the game mode via a registry key on every launch — so the in-game mode switcher cannot accidentally leave the game in the wrong state for the next session.

#### Base game plus expansions and DLC on demand

An Age of Empires II package contains the Age of Kings, The Conquerors, and the Forgotten Empires fan expansion as separate subgames sharing a common component chain. The Conquerors builds on top of the Age of Kings component; Forgotten Empires builds on top of The Conquerors. A user can launch any of the three independently, or launch the original Age of Kings with just the no-CD patch and none of the later layers. Every combination is an entrypoint — a single click in the UI, no files extracted or reorganized.

The same pattern applies naturally to any game with a series of expansions: the base game is the root component, each expansion adds a child component, and each entrypoint names how deep to build. Adding a new expansion to the package means adding one component and one entrypoint — nothing else changes.

#### Immutable base files, permanent user data separation

The copy-on-write layer means every modification made while playing — saves, configuration changes, user-created content — goes into `USERDATA/` automatically, with no action required from the user or the packager. The base files in `PACKAGEFILES/` are read-only and never touched. This has several consequences:

- **Multiple users on the same machine** can share the same package directory and have independent save files and settings by using different `USERDATA/` paths.
- **A fresh start** is as simple as deleting `USERDATA/`. The next launch reconstructs the original runtime state exactly.
- **A packager can verify the base state** at any time by launching with `ReadOnlyVFS = true`, which bypasses `USERDATA/` entirely and presents the pure package state.

#### Registry configuration as first-class data

Games from the Windows 98–XP era are heavily registry-dependent: install paths, serial keys, version strings, CD path emulation, language settings. Getting these wrong typically means the game refuses to start, crashes immediately, or runs in a degraded state. Traditional preservation approaches involve manual registry imports, batch scripts, or pre-configured Wine bottles that become opaque and un-auditable over time.

In a VidyaGod package, every registry key the game needs is declared explicitly in the manifest as a `RegEdit` subcomponent, in a human-readable format alongside the VFS layers that provide the files. The keys are applied automatically on every launch, in the correct order, with `%VAR%` substitution for paths that depend on the runtime environment. The `OVERRIDE` flag handles keys that must win over in-game registry changes — like Warcraft III's game-mode selector — ensuring the package's declared configuration is always authoritative.

#### Modding without risk

Because the base files are never modified, mods can be applied as additional VFS layers on top of the base game without any risk to the original installation. A mod component sits above the base in the union stack — its files take precedence where paths conflict, but the original files remain intact beneath. Removing a mod means removing a component; no uninstaller, no file restoration, no diff-and-patch dance.

Multiple mod configurations can coexist as separate component branches from the same base. A user can switch between a vanilla configuration, a graphics-enhancement mod, and a total conversion mod via the entrypoint picker — each one a different `LASTCOMPONENT` selection, each one assembling a different union stack from the same underlying files.

#### Capturing the complete preservation artifact

When a package is created using the Package Editor's Analyze Registry workflow, the following are captured and encoded in the manifest automatically:

- Every registry key the installer wrote, as `RegEdit` subcomponents
- Every file the installer placed, as VFS layer archives
- The DLL override configuration required by compatibility fixes
- The runtime configuration (Wine prefix version, emulator flags)

The result is not just "the game files" — it is a complete, runnable specification of the entire runtime environment, including all the knowledge that would otherwise be lost when the installer is no longer available, the compatibility layer documentation goes offline, or the person who figured out the right DLL combination is no longer around.

---

### The Core Data Model

A VidyaGod package is organized around four concepts:

- **Subgame** — A game title with metadata (cover art, TGDB ID, release date, etc.) and a list of launch configurations called entrypoints. A single package can contain multiple subgames — base game, expansions, and spin-offs — sharing the same component infrastructure.
- **Component** — A named layer of the runtime environment, optionally depending on a parent component. Components stack together to form the full VFS. Each component adds files (VFS layers), registry configuration, DLL overrides, or config file patches — nothing else.
- **Entrypoint** — A launch configuration under a subgame: specifies which component level to build to (`LASTCOMPONENT`), the executable path, arguments, and forced variable values. The entrypoint determines the depth of the VFS stack for a given launch.
- **CustomVar** — A named variable defined at manifest level, resolved through a two-layer pipeline (token substitution then type translation), and substituted into any `%KEY%` token throughout the manifest. CustomVars expose game settings (resolution, language, game mode) to the user in a typed, human-readable form while handling the translation to whatever raw format the game expects.

### Key Design Principles

**Components exclusively construct the runtime.** They contain VFS layers, registry patches, DLL overrides, and config file edits — no execution logic.

**Entrypoints exclusively describe what to run.** They reference a component chain depth and carry execution parameters.

**CustomVars are manifest-wide configuration.** They are not owned by any component; any subcomponent in any component can reference `%KEY%` and the variable will be resolved.

**The VFS union is reproducible.** Given the same manifest and the same PACKAGEFILES, every launch produces an identical starting state. User-modified files (game saves, changed settings) accumulate in `USERDATA/` via copy-on-write, preserving the clean base without modification.

---

## 2. Dependencies

| Tool | Purpose |
|---|---|
| `unionfs-fuse` | Assembles the final union filesystem |
| `fuse-zip` | Mounts `.zip` archives as read-only FUSE filesystems |
| `bindfs` | Bind-mounts directories read-only |
| `fusermount` | Unmounts FUSE filesystems on cleanup |
| `umu-run` | Launches Wine/Proton (used as the default Wine runner) |
| Qt6 (Core, Widgets, Network) | GUI framework |

Optional: `wine` (alternative to umu-run), `snes9x` or other emulators for their respective platforms.

---

## 3. Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The resulting binary is `build/VidyaGod`.

---

## 4. Usage

### GUI Mode

Launch with no arguments to open the library browser:

```bash
./VidyaGod
```

The library is populated by adding packages through the GUI. The **Package Editor** (accessible from the main window or the pre-launch dialog) allows creating and editing manifests directly.

### Headless Mode (CLI)

If `--package` is provided, VidyaGod runs non-interactively and exits when the game exits:

```bash
./VidyaGod --package /path/to/package --subgame my_subgame_id
```

See [CLI Reference](#14-cli-reference) for all flags.

**Auto-detection**: If VidyaGod is invoked from inside a package directory (i.e., `./METADATA/MANIFEST.json` exists relative to the working directory), it automatically enters headless mode without requiring `--package`.

---

## 5. Package Directory Structure

```
[PACKAGEUID][vPACKAGEVERSION] Package Name/
├── METADATA/
│   ├── MANIFEST.json          ← All package configuration (required)
│   └── [cover images]         ← PNG/JPG files referenced in METADATA.COVER
│
├── PACKAGEFILES/              ← Source archives and directories for VFS layers
│   ├── game.zip               ← Mounted read-only via fuse-zip (VFSZipLayer)
│   ├── patch/                 ← Bind-mounted read-only via bindfs (VFSDirLayer)
│   └── config.ini             ← Hard-linked into staging dir (VFSFileLayer)
│
├── USERDATA/                  ← Writable copy-on-write top layer (persistent)
│   └── ...                    ← Game saves, config changes, COW'd registry files
│
├── RUNTIME/                   ← Final mounted union filesystem (ephemeral)
│   ├── drive_c/               ← Wine only: Windows filesystem root
│   │   └── [PACKAGEUID]/      ← Game files visible at C:\[PACKAGEUID]\
│   └── ...                    ← Other runner types: files at root
│
└── TEMP/                      ← Temporary staging (ephemeral)
    ├── 0/                     ← Staging dir for first VFS layer
    │   └── drive_c/           ← Wine mode: layer wrapped at drive_c/[PACKAGEUID]/
    ├── 1/                     ← Staging dir for second VFS layer
    ├── ...
    └── DEFPREFIX/             ← Wine prefix (created by wineboot)
        ├── drive_c/
        ├── system.reg
        ├── user.reg
        ├── RegPatch32.reg     ← Generated; imported then left in prefix
        └── RegPatch64.reg
```

`RUNTIME/` and `TEMP/` are created at launch and removed by `Cleanup()` after the game exits. `USERDATA/` accumulates across sessions.

---

## 6. MANIFEST.json — Complete Schema Reference

The manifest is a single JSON file at `METADATA/MANIFEST.json`. All fields use `UPPER_SNAKE_CASE` keys. Comments are not valid JSON — they are shown here for documentation purposes only.

### Top-Level Fields

```json
{
    "PACKAGENAME":    "Age of Empires II",
    "PACKAGEUID":     "749",
    "PACKAGEVERSION": "v1.0",
    "CUSTOMVARS":     [...],
    "SUBGAMES":       [...],
    "COMPONENTS":     [...],
    "RUNNERS":        {...}
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `PACKAGENAME` | string | yes | Human-readable package name |
| `PACKAGEUID` | string | yes | Unique identifier; used as the Windows install path (`C:\[PACKAGEUID]`) |
| `PACKAGEVERSION` | string | yes | Version string, arbitrary |
| `CUSTOMVARS` | array | no | Manifest-wide configurable variables |
| `SUBGAMES` | array | yes | One or more game titles in this package |
| `COMPONENTS` | array | yes | Runtime layers that build the VFS environment |
| `RUNNERS` | object | no | Custom runner definitions (platform → array of runner objects) |

---

### SUBGAMES

Each entry in `SUBGAMES` represents a distinct game title (or edition) within the package. A package may contain multiple subgames sharing the same component infrastructure — for example, a base game and its expansion, or multiple platform-specific versions.

```json
{
    "SUBGAMEID":            "aoe2_tc",
    "TITLE":                "Age of Empires II - The Conquerors",
    "PLATFORM":             "Microsoft Windows",
    "GAMEUID":              "13006",
    "DEFAULT_ENTRYPOINT_ID":"UserPatch v1.5 Build 6268",
    "ENTRYPOINTS":          [...],
    "METADATA": {
        "TGDBID":           "13006",
        "STEAMAPPID":       null,
        "UMUID":            "0",
        "GOGPRODUCTID":     null,
        "COVER":            "AoE_TC_Cover.jpg",
        "RELEASEDATE":      "2000-08-25",
        "EDITION":          "Original Release",
        "EDITIONDATE":      null,
        "DEVELOPER":        "Ensemble Studios",
        "PUBLISHER":        "Microsoft Studios",
        "SERIES":           "Age of Empires",
        "SERIESSORTNUMBER": "4",
        "SUBSERIES":        null,
        "SUBSERIESSORTNUMBER": null,
        "EDITOR":           "Yes",
        "ONLINEDRM":        "No",
        "NETWORKMULTIPLAYER":"Yes",
        "DIRECTCONNECT":    "Yes",
        "LANMULTIPLAYER":   "Yes",
        "ONLINEMULTIPLAYER":"No",
        "NETWORKCOOP":      "No",
        "LOCALMULTIPLAYER": "No",
        "LOCALCOOP":        "No",
        "OTHERONLINEFEATURES":"NA"
    }
}
```

#### Subgame Fields

| Field | Type | Required | Description |
|---|---|---|---|
| `SUBGAMEID` | string | yes | Unique identifier within the package |
| `TITLE` | string | yes | Display title |
| `PLATFORM` | string | yes | Key into the `RUNNERS` lookup table (e.g. `"Microsoft Windows"`, `"SNES"`, `"Custom"`) |
| `GAMEUID` | string | yes | External database identifier (e.g. TGDB game ID) |
| `DEFAULT_ENTRYPOINT_ID` | string | yes | `ENTRYPOINT_ID` to select when no user preference is saved |
| `ENTRYPOINTS` | array | yes | Launch configurations for this subgame |
| `METADATA` | object | yes | Descriptive metadata (see below) |

#### METADATA Fields

| Field | Description |
|---|---|
| `TGDBID` | TheGamesDB numeric ID |
| `STEAMAPPID` | Steam App ID (null if not on Steam) |
| `UMUID` | UMU/Steam compatibility ID for umu-run. `"0"` means generic Wine launch |
| `GOGPRODUCTID` | GOG Product ID |
| `COVER` | Filename of the cover image, relative to `METADATA/` |
| `RELEASEDATE` | ISO 8601 date of original release |
| `EDITION` | Edition name (e.g. `"Original Release"`, `"GOG Version"`) |
| `EDITIONDATE` | ISO 8601 date of this specific edition |
| `DEVELOPER` | Developer studio name |
| `PUBLISHER` | Publisher name |
| `SERIES` | Series name for sorting |
| `SERIESSORTNUMBER` | Numeric position within the series |
| `SUBSERIES` | Sub-series name (e.g. `"Need for Speed: Underground"`) |
| `SUBSERIESSORTNUMBER` | Position within sub-series |
| `EDITOR` | Whether the game includes a level/content editor |
| `ONLINEDRM` | Whether the game requires online DRM |
| `NETWORKMULTIPLAYER` | Network multiplayer support |
| `DIRECTCONNECT` | Direct IP connection support |
| `LANMULTIPLAYER` | LAN multiplayer support |
| `ONLINEMULTIPLAYER` | Online multiplayer via matchmaking |
| `NETWORKCOOP` | Network co-op support |
| `LOCALMULTIPLAYER` | Local (split-screen/hotseat) multiplayer |
| `LOCALCOOP` | Local co-op support |
| `OTHERONLINEFEATURES` | Any other online features |

All multiplayer/feature fields accept `"Yes"`, `"No"`, `"NA"`, or `null`.

---

### ENTRYPOINTS

Entrypoints live inside their subgame and describe a specific way to launch it. Each entrypoint references a component (its `LASTCOMPONENT`) to determine how deep the VFS stack is built.

```json
{
    "ENTRYPOINT_ID": "UserPatch v1.5 Build 6268",
    "LASTCOMPONENT": "aoe2_tc_userpatch",
    "EXEPATH":       "age2_x1/age2_x1.exe",
    "EXEARGS":       "",
    "WORKDIR":       "",
    "FORCEVARS":     { "MY_VAR": "value" }
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `ENTRYPOINT_ID` | string | yes | Unique identifier within the subgame's entrypoint list |
| `LASTCOMPONENT` | string | yes | `COMPONENTID` of the deepest component to include in the recipe. The runtime is built from this component's root ancestor down to here. |
| `EXEPATH` | string | Wine/Native | Executable path relative to `ProgramPath` (game directory). For Wine: passed as Windows path inside prefix. |
| `ROM` | string | Emulator | ROM or disc image path relative to `ProgramPath`. Passed as the runner's primary argument. |
| `DATAPATH` | string | Custom | Data path relative to `ProgramPath`. Passed as the runner's primary argument for custom runners. |
| `EXEARGS` | string | no | Additional arguments appended after the executable. Supports `%VAR%` substitution. |
| `WORKDIR` | string | no | Working directory relative to `ProgramPath`. Defaults to `ProgramPath` if absent. Supports `%VAR%` substitution. |
| `FORCEVARS` | object | no | Key→value pairs that seed CustomVar values for this entrypoint. Applied before USERSETTINGS, used to wire entrypoint-specific behaviour (e.g. game mode selection) without user interaction. |

**The LASTCOMPONENT concept**: This field is the key to the VFS layering model. Given `LASTCOMPONENT: "aoe2_tc_userpatch"`, the system walks the PARENTCOMPONENT chain upward from `aoe2_tc_userpatch` to the root component, collects all components in ancestor-first order (the Recipe), and builds the VFS by stacking their subcomponents in that order. An entrypoint that wants a shallower stack simply names an earlier component in the chain.

---

### COMPONENTS

Each component is a named collection of SUBCOMPONENTS (VFS layers, registry patches, etc.) that optionally depends on a parent component. Together they form a tree whose branches can be traversed to different depths.

```json
{
    "COMPONENTID":    "aoe2_tc_userpatch",
    "NAME":           "Age of Empires II - The Conquerors - UserPatch v1.5",
    "PARENTCOMPONENT":"aoe2_tc_patch1_0e",
    "SUBCOMPONENTS":  [...]
}
```

| Field | Type | Required | Description |
|---|---|---|---|
| `COMPONENTID` | string | yes | Unique identifier within the package |
| `NAME` | string | yes | Human-readable display name |
| `PARENTCOMPONENT` | string/null | yes | `COMPONENTID` of the parent, or `null` for a root component |
| `SUBCOMPONENTS` | array | yes | Ordered list of subcomponent objects |

**Ordering**: Components must be declared in the manifest in dependency order (root before descendants). This ensures `BuildSubComponentsArray` stacks VFS layers correctly when the Recipe is walked in ancestor-first order.

---

### SUBCOMPONENT Types

Every entry in `SUBCOMPONENTS` has a `TYPE` field. Supported types:

---

#### VFSZipLayer

Mounts a ZIP archive as a read-only layer in the union filesystem.

```json
{
    "TYPE":   "VFSZipLayer",
    "PATH":   "game.zip",
    "TARGET": "drive_c/game/subdir"
}
```

| Field | Required | Description |
|---|---|---|
| `PATH` | yes | Filename of the ZIP archive in `PACKAGEFILES/` |
| `TARGET` | no | Subdirectory within the staging area where the archive contents are mounted. If absent, the archive is mounted at the root of the staging area. In Wine mode the staging area is `drive_c/[PACKAGEUID]/`; TARGET is relative to that. |

**Implementation**: Uses `fuse-zip -r` (read-only). The mount point is registered for cleanup.

---

#### VFSDirLayer

Bind-mounts a directory from `PACKAGEFILES/` as a read-only layer.

```json
{
    "TYPE":   "VFSDirLayer",
    "PATH":   "game_directory",
    "TARGET": ""
}
```

| Field | Required | Description |
|---|---|---|
| `PATH` | yes | Directory name within `PACKAGEFILES/` |
| `TARGET` | no | Same semantics as VFSZipLayer |

**Implementation**: Uses `bindfs -r`. The mount point is registered for cleanup. Can be converted to/from VFSZipLayer via the PackageEditor.

---

#### VFSFileLayer

Places a single file from `PACKAGEFILES/` into the VFS via a hard link.

```json
{
    "TYPE": "VFSFileLayer",
    "PATH": "gemrb-linux-v0.9.5.appimage"
}
```

| Field | Required | Description |
|---|---|---|
| `PATH` | yes | Filename within `PACKAGEFILES/` |

**Implementation**: Hard-links the file into the numbered staging directory. No FUSE mount; no cleanup entry. Used for executables, single config files, or ROMs.

---

#### RegEdit

Writes registry keys into the Wine prefix before the VFS is mounted, or into the live runtime after mount (with `OVERRIDE: true`).

```json
{
    "TYPE":        "RegEdit",
    "REGPATH":     "HKLM\\Software\\MyCompany\\MyGame",
    "ARCHITECTURE":"32",
    "OVERRIDE":    false,
    "KEYVALUES": {
        "InstallDir": "%WindowsProgramPathDoubleBackSlash%",
        "Version":    "1.2",
        "Width":      "dword:00000780",
        "Enabled":    "dword:00000001"
    }
}
```

| Field | Required | Description |
|---|---|---|
| `REGPATH` | yes | Registry path including hive. Must begin with `HKLM` or `HKCU`. |
| `ARCHITECTURE` | yes | `"32"` or `"64"` (selects the 32-bit or 64-bit registry hive) |
| `OVERRIDE` | no | Default `false`. See [Registry System](#13-registry-system). |
| `KEYVALUES` | no | Object mapping value names to values. If absent, the key is created with no values. |

**KEYVALUES value formats**:

| Format | Example | Meaning |
|---|---|---|
| Plain string | `"C:\\MyGame"` | Written as a quoted REG_SZ string. Backslashes are escaped automatically. |
| `dword:XXXXXXXX` | `"dword:00000780"` | Written verbatim as a 32-bit registry DWORD. Hex digits, zero-padded to 8 characters. |
| `hex:XX,XX,...` | `"hex:a4,00,00,00"` | Written verbatim as REG_BINARY. |
| `hex(b):XX,...` | `"hex(b):00,00,00,00,02,00,00,00"` | QWORD (REG_QWORD), little-endian 8 bytes. |
| `null` | `null` | Writes an empty string `""` for the value. |

All values in `KEYVALUES` support `%VAR%` substitution before being written. The `%WindowsProgramPathDoubleBackSlash%` built-in variable produces `C:\\[PACKAGEUID]`, suitable for embedding in `.reg` file string values.

The key `@` refers to the default (unnamed) value of the registry key (equivalent to `(Default)` in regedit).

---

#### DllOverride

Adds a Wine DLL override entry. All DllOverride entries in the component chain are collected and combined into `WINEDLLOVERRIDES` before the game is launched.

```json
{
    "TYPE":        "DllOverride",
    "DLLOVERRIDE": "ddraw=n,b"
}
```

| Field | Required | Description |
|---|---|---|
| `DLLOVERRIDE` | yes | Override string in Wine format: `dll[,dll...]=mode[,mode]`. Common modes: `n` (native), `b` (builtin). Example: `"d3d8=n,b"` tries native first, then builtin. |

Multiple `DllOverride` entries are joined with `;` and set as the `WINEDLLOVERRIDES` environment variable for the game process.

---

#### FileEdit

Patches a configuration file in the mounted runtime. Currently supports `ConfigWrite` mode, which rewrites any line that starts with a given key.

```json
{
    "TYPE":  "FileEdit",
    "MODE":  "ConfigWrite",
    "FILE":  "config/settings.ini",
    "KEY":   "Resolution=",
    "VALUE": "1920x1080"
}
```

| Field | Required | Description |
|---|---|---|
| `MODE` | yes | Edit mode. Currently only `"ConfigWrite"` is supported. |
| `FILE` | yes | Path to the file relative to `ProgramPath` (inside the mounted runtime) |
| `KEY` | yes | Line prefix to match. Any line starting with this string is replaced. |
| `VALUE` | yes | Replacement value appended after the key. The resulting line is `KEY + VALUE`. |

All fields support `%VAR%` substitution (applied during `BuildSubComponentsArray`). FileEdits are processed after the VFS is mounted, operating directly on the runtime.

---

### CUSTOMVARS

Defined at the top level of the manifest. CustomVars are manifest-wide variables: any subcomponent value, entrypoint field, or other CustomVar DEFAULT can reference them via `%KEY%`.

```json
{
    "CUSTOMVARS": [
        {
            "KEY":     "RESOLUTION_WIDTH",
            "LABEL":   "Resolution Width",
            "DEFAULT": "%ScreenWidth%",
            "VARTYPE": "dword",
            "DISPLAY": true,
            "OPTIONS": null
        },
        {
            "KEY":     "WC3_GAME_MODE",
            "LABEL":   "Game Mode",
            "DEFAULT": "dword:00000001",
            "VARTYPE": "options",
            "DISPLAY": false,
            "OPTIONS": [
                { "LABEL": "Reign of Chaos",    "VALUE": "dword:00000000" },
                { "LABEL": "The Frozen Throne",  "VALUE": "dword:00000001" }
            ]
        }
    ]
}
```

| Field | Required | Description |
|---|---|---|
| `KEY` | yes | Variable name. Referenced as `%KEY%` anywhere in the manifest. |
| `LABEL` | yes | Human-readable label shown in the PreLaunch picker. |
| `DEFAULT` | yes | Default value in display format (see [CustomVar Type System](#9-customvar-type-system)). May contain `%VAR%` tokens. |
| `VARTYPE` | yes | Type determining the translation layer and UI widget. See [CustomVar Type System](#9-customvar-type-system). |
| `DISPLAY` | no | Default `true`. If `false`, the variable is not shown in the PreLaunch picker — it is resolved silently from FORCEVARS seeds, USERSETTINGS, or DEFAULT. Use this for variables that are wired automatically by the manifest structure (e.g. game mode determined by which subgame/entrypoint is selected). |
| `OPTIONS` | no | Required when `VARTYPE` is `"options"`. Array of `{"LABEL": "...", "VALUE": "..."}` objects. The `VALUE` is the raw storage value; the `LABEL` is shown to the user. |

**CustomVar declaration order matters**: later entries can reference earlier ones via `%KEY%`. The resolution pipeline processes them in declaration order.

---

### RUNNERS (Package-Level)

A package can define its own runners in addition to (or instead of) the global runners. Package-level runners take precedence over GlobalConfig runners for the same platform.

This is primarily used for packages that bundle a custom interpreter or emulator as a `VFSFileLayer`.

```json
{
    "RUNNERS": {
        "Custom": [
            {
                "NAME":       "gemrb",
                "TYPE":       "custom",
                "EXECUTABLE": "%RuntimePath%/gemrb-linux-v0.9.5.appimage",
                "ARGS":       [],
                "ENV":        {},
                "REMOVE_ENV": []
            }
        ]
    }
}
```

Runner object fields:

| Field | Required | Description |
|---|---|---|
| `NAME` | yes | Human-readable runner name (shown in the picker) |
| `TYPE` | yes | `"wine"`, `"emulator"`, `"native"`, or `"custom"`. Determines argument order and Wine-specific initialization. |
| `EXECUTABLE` | yes | Path to the runner binary. Supports `%VAR%` substitution (e.g. `%RuntimePath%`). |
| `ARGS` | no | Arguments prepended before the game executable (for emulator/custom runners). Support `%VAR%` substitution. |
| `ENV` | no | Environment variables set before launch. Values support `%VAR%` substitution. |
| `REMOVE_ENV` | no | Environment variable names to unset before launch. |

Package runners for the `Custom` platform key are matched to subgames whose `PLATFORM` field equals `"Custom"`. The runner selection at launch time still comes from the PreLaunch picker, subject to USERSETTINGS overrides.

---

## 7. Component Dependency Chain

Components form a directed acyclic tree via `PARENTCOMPONENT`. A root component has `PARENTCOMPONENT: null`. A component can have only one parent but any number of siblings.

**Building the Recipe**: Given a `LASTCOMPONENT` (e.g. `"patch_v1_2"`), the system walks upward:

```
patch_v1_2  →  base_install  →  null (root)
```

The collected chain is reversed so it runs ancestor-first:

```
Recipe = [ "base_install", "patch_v1_2" ]
```

**BuildSubComponentsArray** then iterates the Recipe and collects all SUBCOMPONENTS in that order. This means `base_install`'s VFS layers are stacked below `patch_v1_2`'s layers in the union, so `patch_v1_2`'s files take precedence when paths conflict.

**Branching**: Multiple components can share the same parent, creating parallel branches. An entrypoint from one branch does not include subcomponents from sibling branches.

```
base_install
├── tc_base         ← Conquerors branch (parent: base_install)
│   └── tc_patch
└── aok_patch       ← AoK branch (parent: base_install)
    └── aok_nocd
```

Launching with `LASTCOMPONENT: "tc_patch"` builds:
`Recipe = [base_install, tc_base, tc_patch]` — AoK-specific layers not included.

---

## 8. Variable Substitution

Any string value in the manifest can contain `%VARIABLE_NAME%` tokens. These are expanded by `StringVariableSubstitution()` before the value is used.

**Substitution is applied in `BuildSubComponentsArray`**: each subcomponent's JSON is serialized, substituted, and re-parsed. This means all fields of all subcomponents (paths, registry key values, file edit values, DLL override strings, etc.) have tokens expanded.

**Substitution is also applied to CustomVar values** (Layer 1 of the CustomVar pipeline) and to entrypoint fields (`EXEPATH`, `EXEARGS`, `WORKDIR`), runner `ENV` values, and runner `ARGS`.

### Built-in Variables

| Token | Value |
|---|---|
| `%PackagePath%` | Absolute path to the package root directory |
| `%PackageName%` | Package name from `PACKAGENAME` |
| `%PackageUID%` | Package UID from `PACKAGEUID` |
| `%GameName%` | Title of the selected subgame |
| `%UMUID%` | UMU/Steam App ID (`"0"` if not set) |
| `%ScreenWidth%` | Width of the primary display in pixels (queried at runtime) |
| `%ScreenHeight%` | Height of the primary display in pixels |
| `%RuntimePath%` | Absolute path to the mounted union filesystem (`PACKAGEFILES/../RUNTIME/`) |
| `%MetaDataPath%` | Absolute path to `METADATA/` |
| `%PackageFilesPath%` | Absolute path to `PACKAGEFILES/` |
| `%UserDataPath%` | Absolute path to `USERDATA/` |
| `%TempPath%` | Absolute path to `TEMP/` |
| `%ProgramPath%` | Wine: `RUNTIME/drive_c/[PACKAGEUID]`; others: `RUNTIME/` |
| `%DefPrefixPath%` | Absolute path to the Wine prefix (`TEMP/DEFPREFIX/`) |
| `%ExePathRelative%` | Executable path relative to ProgramPath |
| `%ExePathComplete%` | Absolute path to the executable on the host |
| `%ExePathInPrefix%` | Windows-style path to the exe inside the Wine prefix |
| `%WindowsProgramPath%` | `C:\[PACKAGEUID]` — Windows path to the game directory |
| `%WindowsExePathComplete%` | `C:\[PACKAGEUID]\[ExePathRelative]` |
| `%WindowsProgramPathDoubleBackSlash%` | `C:\\[PACKAGEUID]` — for use in `.reg` file string values |
| `%WorkDirPathRelative%` | Working directory relative to ProgramPath |
| `%WorkDirPathComplete%` | Absolute working directory |

CustomVar keys are added to this map after resolution and can be referenced by `%KEY%` in subsequent subcomponents, later CustomVars, and entrypoint fields.

---

## 9. CustomVar Type System

CustomVars go through a two-layer processing pipeline:

```
Raw value (from FORCEVARS / VariableOverrides / USERSETTINGS / DEFAULT)
    ↓  Layer 1: StringVariableSubstitution  (%ScreenWidth% → "1920")
    ↓  Layer 2: TranslateCustomVarValue     ("1920" + dword → "dword:00000780")
    →  Stored in CustomVariables[KEY]       (substituted wherever %KEY% appears)
```

**Layer 1** uses `GetVariablesMap()` called per-variable inside the resolution loop. Each resolved CustomVar is immediately available to subsequent ones, so `%RESOLUTION_WIDTH%` in a later CustomVar's DEFAULT expands correctly.

**Layer 2** applies a type-specific translation:

| VARTYPE | DEFAULT format | Raw storage format | UI widget |
|---|---|---|---|
| `string` | any string | unchanged | QLineEdit |
| `number` | decimal integer | unchanged (plain string) | QSpinBox |
| `dword` | decimal integer | `dword:XXXXXXXX` (8-digit lowercase hex) | QSpinBox |
| `qword` | decimal integer | `hex(b):XX,XX,XX,XX,XX,XX,XX,XX` (little-endian) | QSpinBox |
| `bool` | `"1"`, `"true"`, `"yes"` (case-insensitive) | `"dword:00000001"` or `"dword:00000000"` | QCheckBox |
| `options` | matches one OPTIONS VALUE | unchanged (VALUES are already raw) | QComboBox |

**DEFAULT field always uses the display format**, not the raw format. For a `dword` variable that controls a resolution width, write `"DEFAULT": "1920"` (decimal), not `"DEFAULT": "dword:00000780"`. The translation is applied automatically.

**`%ScreenWidth%` as DEFAULT**: Since Layer 1 runs first, `"DEFAULT": "%ScreenWidth%"` on a `dword` variable correctly expands to the current screen width string (e.g. `"1920"`) before Layer 2 translates it to `"dword:00000780"`.

**FORCEVARS**: Entrypoints carry a `FORCEVARS` map (`{"KEY": "display_value"}`) that seeds the CustomVar resolution for that specific launch configuration. The values in FORCEVARS are Layer-1 and Layer-2 processed like any other source. This is the mechanism for wiring game-mode selection (e.g. `{"WC3_GAME_MODE": "dword:00000000"}` for Reign of Chaos) without exposing a picker to the user (`DISPLAY: false`).

**Resolution priority** (highest to lowest):
1. `VariableOverrides` — from CLI `--var KEY=VALUE` or user's picker selection in the PreLaunch window
2. `USERSETTINGS[PackageUID].VARIABLES` — persisted from a previous "Remember" selection
3. `FORCEVARS` seed on the selected entrypoint
4. `DEFAULT` from the CustomVar definition

---

## 10. Runner Types

The `TYPE` field on a runner object determines how VidyaGod constructs the launch command and initializes the environment.

### `wine`

Windows executables via Wine or Proton (umu-run).

- **Initialization**: Runs `wineboot` to create the Wine prefix at `TEMP/DEFPREFIX/`. The prefix is added as the lowest-priority VFS layer.
- **Registry**: Registry patches are written into the prefix before the VFS is mounted. Override patches are written into the live runtime after mounting.
- **Program path**: Files mounted at `RUNTIME/drive_c/[PACKAGEUID]/`. The exe is addressed as `C:\[PACKAGEUID]\[EXEPATH]` inside Wine.
- **Launch command**: `[EXECUTABLE] [EXEPATH_IN_PREFIX] [EXEARGS]`
- **DLL overrides**: Joined and set as `WINEDLLOVERRIDES`.

### `emulator`

ROM-based games where an external emulator is the runner.

- **No Wine prefix initialization.**
- **Program path**: `RUNTIME/` (files mounted at root).
- **Launch command**: `[EXECUTABLE] [RUNNER_ARGS...] [ROM_PATH] [EXEARGS]`
- **ROM path**: The `ROM` field from the entrypoint is the game file passed to the emulator.

### `native`

Linux-native binaries.

- **No Wine prefix initialization.**
- **Program path**: `RUNTIME/`.
- **Launch command**: `[EXECUTABLE] [EXEARGS]`

### `custom`

Any other runner. The game data path (`DATAPATH` from the entrypoint) is passed as the runner's primary argument.

- **No Wine prefix initialization.**
- **Program path**: `RUNTIME/`.
- **Launch command**: `[EXECUTABLE] [RUNNER_ARGS...] [DATAPATH] [EXEARGS]`
- Used for bundled interpreters (e.g. a GemRB AppImage mounted via VFSFileLayer).

---

## 11. GlobalConfig.JSON

Stored at `~/.VidyaGod/GlobalConfig.JSON`. Created automatically on first launch.

### Structure

```json
{
    "DefaultTables": { ... },
    "RUNNERS": {
        "Microsoft Windows": [
            {
                "NAME":        "wine",
                "TYPE":        "wine",
                "EXECUTABLE":  "wine",
                "ENV":         { "WINEPREFIX": "%RuntimePath%" },
                "REMOVE_ENV":  ["LD_LIBRARY_PATH"]
            },
            {
                "NAME":        "umu-proton",
                "TYPE":        "wine",
                "EXECUTABLE":  "umu-run",
                "ENV": {
                    "WINEPREFIX":  "%RuntimePath%",
                    "GAMEID":      "%UMUID%",
                    "PROTON_VERB": "waitforexitandrun"
                },
                "REMOVE_ENV": ["LD_LIBRARY_PATH"]
            }
        ],
        "SNES": [
            {
                "NAME":       "snes9x",
                "TYPE":       "emulator",
                "EXECUTABLE": "snes9x",
                "ARGS":       ["-fullscreen"],
                "ENV":        {},
                "REMOVE_ENV": []
            }
        ]
    },
    "LIBRARY": [
        {
            "PATH": "/path/to/package",
            "USERSETTINGS": {
                "PREFERRED_RUNNER":   "umu-proton",
                "PREFERRED_VARIANT_ID": "v1.31.1",
                "SKIP_LAUNCH_DIALOG": false,
                "VARIABLES": {
                    "RESOLUTION_WIDTH": "1920"
                }
            }
        }
    ]
}
```

### Runner Resolution Order

When launching, the runner is selected by priority:

1. `LIBRARY[i].USERSETTINGS.PREFERRED_RUNNER` — User's persisted choice for this package
2. First runner in `GlobalConfig.RUNNERS[Platform]` that matches by name, if none saved
3. First runner in `GlobalConfig.RUNNERS[Platform]` — default for the platform
4. Hardcoded fallback: `umu-run` / Wine — if no runners defined for the platform

Package-level `RUNNERS` in `MANIFEST.json` are checked before GlobalConfig runners.

### USERSETTINGS

Per-package preferences stored inside the library entry:

| Key | Description |
|---|---|
| `PREFERRED_RUNNER` | Runner name to pre-select in the picker |
| `PREFERRED_VARIANT_ID` | Entrypoint ID to pre-select |
| `SKIP_LAUNCH_DIALOG` | If `true`, skip the PreLaunch window and launch immediately with saved settings |
| `VARIABLES` | Saved CustomVar values (KEY → display value); used as priority-2 source in resolution |

---

## 12. Container Lifecycle

### Phase 1: Initialization (Constructor)

```
ContainerParams constructed with (PackagePath, subgame_id, component_id)
    │
    ├─ DecideComponent()
    │   Resolves which component_id to use from the subgame's DEFAULT_ENTRYPOINT_ID
    │   if component_id was not explicitly provided.
    │
    ├─ DeriveContainerParams()
    │   Fills all derived fields: PackageName, PackageUID, GameName, UMUID, Platform,
    │   runner config (RunnerName, RunnerExecutable, RunnerTypeEnum, RunnerEnv, ...),
    │   all path fields, EntrypointID (from DEFAULT_ENTRYPOINT_ID).
    │
    ├─ CreateRecipe()
    │   Walks PARENTCOMPONENT chain from component_id to root.
    │   Result: Recipe = [root, ..., component_id]  (ancestor-first)
    │
    ├─ ResolveCustomVariables()
    │   For each CustomVar in MANIFEST["CUSTOMVARS"]:
    │     1. Resolve value: VariableOverrides → USERSETTINGS → FORCEVARS → DEFAULT
    │     2. Layer 1: StringVariableSubstitution (expand %tokens%)
    │     3. Layer 2: TranslateCustomVarValue (dword/qword/bool translation)
    │     4. Store in CustomVariables[KEY]  ← available to subsequent vars
    │
    └─ BuildSubComponentsArray()
        For each ComponentID in Recipe order:
          For each SUBCOMPONENT:
            Serialize → StringVariableSubstitution → re-parse → append to SubComponentsArray
```

### Phase 2: Runtime Build

```
BuildContainerRuntime()
    │
    ├─ [Wine only] InitializeDefPrefix()
    │   Run wineboot → create TEMP/DEFPREFIX/ (Wine prefix)
    │   Add DefPrefixPath to VFSString as base (lowest-priority) layer
    │
    ├─ [Wine only] CreateFlatRegPatchJSON()
    │   Collect all RegEdit subcomponents with OVERRIDE=false
    │   Build FlatRegPatch["32"|"64"][regPath][valueName] = value
    │
    ├─ [Wine only] CreateRegPatchFiles() + MergeRegPatchFiles()
    │   Write RegPatch32.reg and RegPatch64.reg → import into Wine prefix
    │
    ├─ PreMountFilesystemComponents()
    │   For each VFSZipLayer / VFSDirLayer / VFSFileLayer in SubComponentsArray:
    │     Mount/link into numbered TEMP/[i]/ staging directory
    │     Add staging dir to VFSString (higher priority than earlier layers)
    │
    ├─ FinalizeVFSString()
    │   Prepend USERDATA=RW to VFSString (highest priority, writable layer)
    │
    ├─ MountVFS()
    │   unionfs -o cow -o uid=1000 [VFSString] [RUNTIME/]
    │
    ├─ [Wine only] ProcessDLLOverrides()
    │   Collect DLLOVERRIDE values → joined as WINEDLLOVERRIDES for Execute()
    │
    ├─ [Wine only] ProcessFileEdits()
    │   For each FileEdit (MODE=ConfigWrite): patch the config file in RUNTIME/
    │
    └─ [Wine only] ApplyOverrideRegEdits()
        Collect RegEdit subcomponents with OVERRIDE=true
        Write OverridePatch32.reg / OverridePatch64.reg to RUNTIME/drive_c/
        Import into mounted runtime (writes to USERDATA layer via COW)
        Delete temp .reg files
```

### Phase 3: Execute

```
Execute()
    │
    ├─ Resolve executable path (ExePathInPrefix for Wine; ExePathComplete for others)
    ├─ Build argument list ([RunnerArgs...] [exe] [ExeArgs] or [exe] [ExeArgs] for Wine)
    ├─ Set process environment:
    │   Remove REMOVE_ENV entries
    │   Apply RunnerEnv with %VAR% substitution
    │   [Wine] Set WINEDLLOVERRIDES from collected DLL overrides
    ├─ Set working directory (WorkDirPathComplete, falls back to RuntimePath)
    ├─ Register ActiveRunProcess (for KillGame())
    └─ Wait for process exit (indefinitely)
```

### Phase 4: Cleanup

```
Cleanup()
    ├─ fusermount -uz [each FUSE mount point in reverse order]
    ├─ Remove RUNTIME/ directory tree
    └─ Remove TEMP/ directory tree
```

`USERDATA/` is never removed — it persists game saves and user configuration changes across launches.

---

## 13. Registry System

VidyaGod applies registry patches to the Wine prefix in two distinct passes to solve the problem of persistent in-game registry changes overriding essential configuration.

### Pass 1 — DEFPREFIX pass (before VFS mount)

All `RegEdit` subcomponents with `OVERRIDE: false` (or without the `OVERRIDE` field — the default) are processed in this pass. The patches are written into the Wine prefix (`TEMP/DEFPREFIX/drive_c/RegPatch32.reg` and `RegPatch64.reg`) and imported via `winetools reg import` before any game layers are mounted.

Since `DEFPREFIX` is the base (lowest-priority) layer of the union, any file modified by the game engine during a session is copy-on-write'd into `USERDATA/`, which sits above `DEFPREFIX`. On subsequent launches, the `USERDATA` copy of `system.reg` will have the old in-game value, shadowing the `DEFPREFIX` version.

### Pass 2 — OVERRIDE pass (after VFS mount)

`RegEdit` subcomponents with `OVERRIDE: true` are processed after `MountVFS()`. The patch is written directly to `RUNTIME/drive_c/OverridePatch32.reg` and imported with `WINEPREFIX=RUNTIME/`. Since `RUNTIME/` reflects the full union and `USERDATA/` is the RW top layer, the import writes into `USERDATA/system.reg` (or `user.reg`), overwriting any previous value — including values saved by the game's own in-session registry changes.

This means OVERRIDE RegEdits win unconditionally on every launch, regardless of what the game wrote to the registry in any previous session.

**Use OVERRIDE: true when:**
- The game reads a setting from the registry at startup that must match the entrypoint's configuration (e.g. Warcraft III's `Preferred Game Version` key)
- The game's own registry-based settings switcher would otherwise override your patch

**Use OVERRIDE: false (default) when:**
- The registry key sets a static installation path or licence key that never changes
- You want the game's in-session changes to persist (e.g. user's audio or video preferences)

---

## 14. CLI Reference

```
VidyaGod [OPTIONS]
```

### Options

| Flag | Argument | Description |
|---|---|---|
| `--package <path>` | Directory path | Path to the package directory. Activates headless mode. |
| `--subgame <id>` | SUBGAMEID | Subgame to launch. If omitted with `--package`, the first subgame is used. |
| `--component <id>` | COMPONENTID | Override which component to use as LASTCOMPONENT, bypassing the entrypoint system. |
| `--variant <id>` | ENTRYPOINT_ID | Select a specific entrypoint by ID. |
| `--var KEY=VALUE` | KEY=VALUE | Set a CustomVar override. May be repeated. VALUE is in display format (e.g. `1920` for a `dword` variable, not `dword:00000780`). |

### Examples

```bash
# Launch a Windows game at its default entrypoint
VidyaGod --package "/The Vidya/[749][v1.0] Age of Empires II" --subgame aoe2_tc

# Launch with a specific entrypoint (patch level)
VidyaGod --package "/The Vidya/[749][v1.0] Age of Empires II" \
         --subgame aoe2_tc \
         --variant "Patch v1.0c"

# Launch a ROM via emulator
VidyaGod --package "/The Vidya/[299][v1.0] Super Metroid" --subgame super_metroid

# Override a CustomVar (resolution)
VidyaGod --package "/The Vidya/[5271][v1.0] NFS Most Wanted" \
         --subgame nfsmw_base \
         --var RESOLUTION_WIDTH=2560 \
         --var RESOLUTION_HEIGHT=1440

# Auto-detect (run from inside a package directory)
cd "/The Vidya/[802][v1.0] Warcraft III"
VidyaGod --subgame wc3_tft --variant v1.29.2
```

---

---

## 15. Package Editor

The Package Editor is a full-screen dialog for creating and editing package manifests. It opens directly against the `MANIFEST.json` of a chosen package directory, saving every change immediately to disk as you make it. There is no "undo" — edits are written on every field change.

### Opening the Editor

**From the main library window**: Click the edit icon on any game card, or use the "Package Editor" menu option. A directory picker appears if no package is pre-selected.

**From the PreLaunch window**: Click the "Package Editor" button at the bottom left. The editor opens directly for the current package, bypassing the directory picker.

**Directly on a package directory**: Pass the directory path to the editor programmatically. If `METADATA/MANIFEST.json` does not yet exist, the editor creates the directory structure and a blank manifest.

---

### Tab Structure

The editor presents a tab strip across the top. Tabs are built from the current manifest state and rebuilt whenever structural changes are made (adding/removing subgames, components, or subcomponents).

```
[ JSON ] [ MANIFEST ] [ CUSTOMVARS ] [ Component: base ] [ Component: patch ] ...
```

- **JSON** — Raw JSON editor and save
- **MANIFEST** — Visual editor for package fields, subgames, and entrypoints
- **CUSTOMVARS** — Manifest-level variable definitions
- **One tab per Component** — VFS layers, registry patches, and other subcomponents for that component

The **Save** button in the top toolbar writes the manifest to disk manually. Most individual field editors also auto-save on change, but using Save after a batch of edits ensures nothing is lost.

---

### JSON Tab

A raw text editor showing the full `MANIFEST.json` content. The editor validates JSON on every keystroke:

- **Valid JSON**: white background, Save JSON button enabled.
- **Invalid JSON**: red background, Save JSON button disabled.

Clicking **Save JSON** parses the text editor content, overwrites the in-memory manifest, rebuilds the entire UI from the new structure, and writes to disk. This is the escape hatch for bulk edits or for pasting in externally prepared JSON.

> **Warning**: The JSON tab reflects the manifest as it was when the tab was last built. Make structural edits (add/remove components, subgames) through the visual tabs, then use the JSON tab for fine-tuning field values.

---

### MANIFEST Tab

The MANIFEST tab has two sections stacked vertically: the package-level identity fields, and a sub-tab strip showing one tab per subgame.

#### Package-Level Fields

At the top of the MANIFEST tab, below the sub-tab strip for subgames:

| Field | Description |
|---|---|
| `PACKAGENAME` | Human-readable display name |
| `PACKAGEUID` | Unique identifier; becomes the Windows install path (`C:\[PACKAGEUID]`) |
| `PACKAGEVERSION` | Version string |

These fields auto-save when you finish editing (focus leaves the field).

#### Subgame Sub-Tabs

Each subgame has its own sub-tab. Click the sub-tab to switch between subgames. Tabs are labelled by `SUBGAMEID` (or "Subgame N" if not yet set).

**Add Subgame**: Button at the top-right of the MANIFEST tab. Creates a new subgame with null fields and switches to it.

**Remove Subgame**: Button inside each subgame tab. Removes the subgame from the manifest.

##### Cover Art

The top of each subgame tab shows a 150×225 px drop target (2:3 aspect ratio, matching SteamGridDB vertical art). Drag and drop any image file onto it to set the cover. The image is saved to `METADATA/[SUBGAMEID]_cover.[ext]` and `METADATA.COVER` is updated automatically.

##### Identity Section

| Field | Description |
|---|---|
| `SUBGAMEID` | Unique ID used to reference this subgame from the CLI and from component chains |
| `TITLE` | Display title |
| `PLATFORM` | Platform key. Must match a key in `GlobalConfig.RUNNERS` or `MANIFEST.RUNNERS`. |
| `GAMEUID` | External database ID |

**PLATFORM** is a dropdown populated from the runners defined in `GlobalConfig.JSON`. Changing the platform triggers a UI rebuild since it affects which runner-type-specific fields are shown.

##### Execution Section

| Field | Description |
|---|---|
| `DEFAULT_ENTRYPOINT_ID` | ID of the entrypoint to select by default when no user preference exists |

##### Entrypoints Section

Below the Execution section is the **Entrypoints** group. Each entrypoint is shown as a collapsible card:

**Entrypoint card fields**:

| Field | Control | Description |
|---|---|---|
| `ENTRYPOINT_ID` | QLineEdit | Unique ID within this subgame |
| `LASTCOMPONENT` | QComboBox | Dropdown of all components in the package. Select which component level the VFS is built to. |
| `EXEPATH` | QLineEdit | Executable path relative to ProgramPath. Supports `%VAR%` tokens. |
| `EXEARGS` | QLineEdit | Additional arguments. Supports `%VAR%` tokens. |
| `WORKDIR` | QLineEdit | Working directory. Supports `%VAR%` tokens. |
| `FORCEVARS` | Key/value list | Variable seeds for this entrypoint (see FORCEVARS). |

**FORCEVARS editor**: Below the main fields, a key→value list with ✕ remove buttons and a "+ Add Force Var" button. Each row has an editable key field and a value field. Used to wire entrypoint-specific CustomVar values that should not be exposed as user-facing options (e.g. setting game mode to RoC or TFT automatically based on which subgame/entrypoint is selected).

**▶ Execute button**: Runs this specific entrypoint immediately. Presents a runner picker dialog, then builds the full container runtime for this entrypoint's component chain and launches the game. Useful for testing before packaging is complete.

**✕ Remove button**: Deletes the entrypoint from the manifest.

**+ Add Entrypoint button**: Adds a new entrypoint with placeholder values at the bottom of the list.

##### Metadata Section

A form with all descriptive metadata fields (TGDBID, cover, release date, developer, publisher, multiplayer features, etc.). All fields auto-save on focus-out. See [SUBGAMES](#subgames) for the full field list.

---

### CUSTOMVARS Tab

The CUSTOMVARS tab (between MANIFEST and the first Component tab) lists all manifest-level CustomVar definitions. Each variable appears as a card.

**CustomVar card fields**:

| Field | Control | Description |
|---|---|---|
| `KEY` | QLineEdit | Variable name. Referenced as `%KEY%` throughout the manifest. Must be unique. |
| `LABEL` | QLineEdit | Human-readable label shown in the PreLaunch picker. |
| `DEFAULT` | QLineEdit | Default value in display format. May contain `%VAR%` tokens. |
| `VARTYPE` | QComboBox | Type: `string`, `number`, `dword`, `qword`, `bool`, `options`. Determines translation and UI widget. |
| `DISPLAY` | QCheckBox | If unchecked, the variable is not shown in the PreLaunch picker (resolved silently from FORCEVARS/USERSETTINGS/DEFAULT). |

When `VARTYPE` is `"options"`, the card expands to show an **OPTIONS** list:

Each option row has:
- **Label field** — what the user sees in the dropdown
- **Value field** — the raw storage value substituted for `%KEY%` (e.g. `dword:00000001`)
- **✕ button** — removes the option

**+ Add Option** button appends a new empty option row.

**✕ Remove button** (top-right of each card): deletes the CustomVar from the manifest.

**+ Add CustomVar** button at the bottom: appends a new CustomVar with `VARTYPE: "string"` and `DISPLAY: true`.

---

### Component Tabs

One tab per component in `MANIFEST.COMPONENTS`. Tabs are labelled by `COMPONENTID` (or "Component N" if not set).

**Add Component button**: Located in the top toolbar. Appends a new empty component to the manifest and opens its tab.

Each component tab has a toolbar, a name/parent section, and a scrollable subcomponent list.

#### Component Toolbar

| Button | Description |
|---|---|
| **↑ / ↓** | Move this component up or down in the manifest's COMPONENTS array. Useful for maintaining the correct declaration order (ancestors before descendants). |
| **Parent Component** | Dropdown of all earlier components in the manifest. Select `None` for a root component. Only components declared before this one are available (enforcing acyclic dependency). |
| **Run EXE** | Opens a file picker to select any executable, then runs it inside this component's full VFS runtime (built up the component's ancestor chain). Use this to run game installers or setup tools. Registry changes and file writes go to `USERDATA/`. |
| **Browse** | Opens `explorer.exe` inside this component's runtime. Use this to inspect what files are visible in the mounted VFS, verify paths, and check the Wine filesystem layout. |
| **Edit Registry** | Opens `regedit.exe` inside this component's runtime. Use this to browse the current registry state, check what keys were written by an installer, and verify registry patches. |
| **Execute Component** | Shows a picker listing all entrypoints whose `LASTCOMPONENT` is within this component's ancestor chain. Select an entrypoint and a runner, then launches the game. Useful for testing a specific configuration level. |
| **Analyze Registry** | Captures registry changes since the last session. See [The Analyze Registry Workflow](#the-analyze-registry-workflow). |
| **+ Subcomponent** | Dropdown menu for adding subcomponent entries (see below). |
| **Finalize** | Reserved for future use. |
| **Remove Component** | Deletes this component from the manifest. |

#### + Subcomponent Menu

| Option | Adds |
|---|---|
| **VFSZipLayer** | Opens a ZIP file picker; moves the file to `PACKAGEFILES/` and adds the layer. |
| **VFSDirLayer** | Opens a directory picker; moves the directory to `PACKAGEFILES/` and adds the layer. |
| **VFSFileLayer** | Opens a file picker; moves the file to `PACKAGEFILES/` and adds the layer. |
| **CustomVar** | *(Removed — CustomVars are now defined in the top-level CUSTOMVARS tab.)* |
| **RegEdit** | Adds a new empty RegEdit subcomponent. |
| **DllOverride** | Adds a new empty DllOverride subcomponent. |
| **FileEdit** | Adds a new empty FileEdit subcomponent (ConfigWrite mode). |

#### Subcomponent Cards

Each subcomponent appears as an inline card in the scrollable list. Every card has a **TYPE** label and a **✕ remove button**.

**VFSZipLayer / VFSDirLayer / VFSFileLayer** card:
- `PATH` — filename within `PACKAGEFILES/`. Edit if needed.
- `TARGET` — optional subdirectory (shown for Zip and Dir layers). Leave blank to mount at the game directory root.
- **→ ZIP / → DIR** conversion buttons (Zip/Dir only): converts the layer in-place by zipping or unzipping the content in `PACKAGEFILES/`.

**RegEdit card**:
- `REGPATH` — registry path including hive (`HKLM\...` or `HKCU\...`)
- `ARCHITECTURE` — 32 or 64
- `OVERRIDE` — checkbox. Check to apply this key after the VFS is mounted (wins over any in-game registry changes). Leave unchecked for static installation keys.
- **KEYS** section — editable key/value table:
  - Each row: editable key name field | editable value field | ✕ remove button
  - Key names are editable directly; renaming a key preserves its value.
  - Values accept plain strings, `dword:XXXXXXXX`, `hex:XX,XX,...`, and `%VAR%` tokens.
  - **+ Add Key** button appends a new empty key.

**DllOverride card**:
- `DLLOVERRIDE` — Wine override string (e.g. `d3d8=n,b`).

**FileEdit card**:
- `MODE` — currently only `ConfigWrite`
- `FILE` — path to target file relative to ProgramPath
- `KEY` — line prefix to match and replace
- `VALUE` — replacement value after the key

---

### Packaging Workflow

The typical workflow for creating a new package from a Windows game installer:

#### 1. Create the package directory

Create a directory with the naming convention `[PACKAGEUID][vVERSION] Game Name/`. Inside it, create `METADATA/` and `PACKAGEFILES/` directories, then open the Package Editor on this directory. The editor creates a blank manifest automatically.

#### 2. Set package identity

In the MANIFEST tab, fill in `PACKAGENAME`, `PACKAGEUID`, and `PACKAGEVERSION`. Add a subgame with `+ Add Subgame`, set its `SUBGAMEID`, `TITLE`, and `PLATFORM` (`Microsoft Windows` for most Windows games).

#### 3. Create the base component

Click **+ Add Component** in the toolbar. Set `COMPONENTID` (e.g. `mygame_base`) and `NAME`. Leave `PARENTCOMPONENT` as `None`. This will hold the base game installation.

#### 4. Run the installer

Click **Run EXE** on the base component. Select the game's setup executable (from anywhere on your system — it does not need to be in `PACKAGEFILES/`). The installer runs inside the component's Wine prefix. Follow the installation steps normally. The installer's file writes go to `USERDATA/` and its registry writes go to the Wine prefix (later captured by Analyze).

> **Tip**: When the installer asks for an installation directory, use `C:\[PACKAGEUID]` exactly. This matches the path VidyaGod maps the game files to inside the Wine prefix. If the installer insists on a different path, you can use the `%WindowsProgramPath%` registry variable in subsequent RegEdit subcomponents.

#### 5. Capture file changes

After the installer finishes, the installed game files are in `USERDATA/drive_c/[PACKAGEUID]/` (or wherever the installer put them). Manually copy or move these to a new directory in `PACKAGEFILES/` and add it as a **VFSDirLayer** or zip it up as a **VFSZipLayer** on the base component. Remove the files from `USERDATA/` afterwards to keep it clean.

#### 6. Capture registry changes

Click **Analyze Registry** on the base component. The editor compares the registry state before and after the install (by diffing the parent component's clean registry against the current `USERDATA/` registry). All new and changed keys are automatically added as `RegEdit` subcomponents to the base component. Review them and remove any that are not needed for the game to run.

#### 7. Clean up USERDATA

After capturing files and registry changes, delete the contents of `USERDATA/` so the next launch starts from a clean state.

#### 8. Test the base

Click **Execute Component** on the base component and pick a runner. If the game launches and works correctly, the base component is complete.

#### 9. Add patches as child components

For each patch or add-on:
1. Click **+ Add Component**, set a new `COMPONENTID`, and set `PARENTCOMPONENT` to the previous component (base or last patch).
2. Add the patch files as a VFSZipLayer or VFSDirLayer.
3. If the patch writes registry changes (version strings, etc.), add them as RegEdit subcomponents manually.

#### 10. Create entrypoints

In the subgame tab, click **+ Add Entrypoint**. Set:
- `ENTRYPOINT_ID` — a descriptive name (e.g. `"Vanilla"`, `"Patch v1.2"`)
- `LASTCOMPONENT` — the deepest component to include for this launch option
- `EXEPATH` — the game's main executable, relative to `C:\[PACKAGEUID]\` (e.g. `game.exe`)
- `DEFAULT_ENTRYPOINT_ID` — set this on the subgame to the entrypoint you want to be the default

#### 11. Set cover art

Drag a cover image (2:3 aspect ratio recommended) onto the cover drop area in the subgame tab.

#### 12. Fill in metadata

Complete the Metadata section: TGDBID, release date, developer, publisher, and multiplayer features.

---

### The Analyze Registry Workflow

**Analyze Registry** is the primary tool for capturing a game's required registry configuration without manual inspection. It compares the registry state before and after an installation or configuration step, and automatically writes the delta as `RegEdit` subcomponents.

#### How it works

When you click **Analyze Registry** on component `C` (which has parent component `P`):

1. **Builds a read-only comparator runtime** using component `P`'s full chain (or a bare Wine prefix if `P` is `None`). This represents the "before" state — the registry as it existed before `C`'s installer ran.

2. **Reads the "before" registry** — `system.reg` and `user.reg` from the comparator runtime.

3. **Tears down the comparator runtime.**

4. **Reads the "after" registry** — `system.reg` and `user.reg` from `USERDATA/`. These contain whatever the installer or game wrote during a previous **Run EXE** or **Execute Component** session.

5. **Computes the delta** — subtracts the "before" registry from the "after" registry. New keys and new/changed values appear in the delta.

6. **Merges the delta** into component `C`'s subcomponents as `RegEdit` entries — one entry per registry path, HKLM keys in a 32-bit entry, HKCU keys in a separate entry.

7. **Saves and rebuilds the UI.**

#### Typical Analyze workflow

```
1. Run EXE (installer) on component C
   → File + registry changes accumulate in USERDATA/

2. Copy installed files to PACKAGEFILES/ → add as VFSZipLayer on C
   → Delete the file changes from USERDATA/ (keep only .reg files)

3. Click Analyze Registry on C
   → RegEdit subcomponents auto-generated from USERDATA/ registry delta

4. Review the generated RegEdit entries
   → Remove noise (user preferences, unrelated installer keys, etc.)
   → Check values look correct (install paths, version strings)

5. Delete contents of USERDATA/ to reset for future sessions

6. Test: Execute Component → game should launch correctly with the captured registry
```

#### What to keep and what to remove

The delta will typically contain:

- **Installation paths** — `InstallDir`, `EXE Path`, etc. — **keep these.** They tell the game where it is.
- **License/serial keys** — `PID`, `DigitalProductID`, etc. — **keep these** if the game validates them at startup.
- **Version strings** — **keep these** if the game checks its own version.
- **User preferences** — sound volume, display mode, difficulty — **remove these.** They should live in `USERDATA/` and vary between users.
- **MRU (Most Recently Used) lists** — **remove these.** They contain session-specific data.
- **Installer metadata** — uninstall paths, update URLs — **remove these.** They reference paths that don't exist in the package.

#### Multiple components

Run Analyze Registry on each component after its installer or configuration tool has run. The parent component picker determines the "before" baseline — set it to the correct parent before clicking Analyze so the comparison baseline is accurate.

For a chain `base → patch → nocd`:
- Run installer → Analyze on `base` (parent: none)
- Run patch → Analyze on `patch` (parent: `base`)
- Apply no-cd → no registry changes expected, skip Analyze

---

*VidyaGod is developed openly. Bug reports and contributions welcome.*
