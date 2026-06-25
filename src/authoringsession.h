#ifndef AUTHORINGSESSION_H
#define AUTHORINGSESSION_H

#include <nlohmann/json.hpp>

#include <QDir>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "manifestmodel.h"     // NodeIndex
#include "registrywrapper.h"   // RegistryWrapper (baseline snapshot + DiffToRegEdits)

class ContainerWrapper;
struct ContainerParams;

// ---------------------------------------------------------------------------
// AuthoringSession — a held-open authoring runtime for one node, restoring the install→capture loop the
// pristine-by-default PERSIST redesign removed (the old loop was an implicit PersistAll side effect).
//
// It builds the container for a TARGET node's OWN CLOSURE (its PARENTS→node, treated as a synthetic launchable),
// keeps it MOUNTED (no auto-cleanup), snapshots the prefix registry as a baseline, and then lets the caller run
// executables (an installer, regedit, …) against the LIVE mount as many times as wanted. The session's WRITELAYER
// is the COW delta — exactly what those runs created/changed — which the caller captures into the bundle:
//   • files    → a VFSDirLayer (mechanism only here: copy a chosen sub-selection; the UI/user decides placement)
//   • registry → RegEdit subcomponents (baseline-vs-now diff via RegistryWrapper::DiffToRegEdits)
// End() tears the runtime down. Authoring persistence is fully independent of the package's declared Persist, so a
// finished package stays pristine.
//
// Synchronous (matches the editor's existing RunInNode, which blocks the GUI during a build/run); a wait cursor is
// the caller's job. The platform-agnostic file capture means the same session serves wine installs and, e.g., an
// emulator's generated config — wine-only affordances are gated on PrefixGenerate().
// ---------------------------------------------------------------------------
class AuthoringSession
{
public:
    AuthoringSession(const nlohmann::ordered_json &GlobalConfigJSON, const QDir &BundleDir);
    ~AuthoringSession();   // ensures End() if still live

    AuthoringSession(const AuthoringSession &) = delete;
    AuthoringSession &operator=(const AuthoringSession &) = delete;

    // Opens a BARE, platform-agnostic runtime for TargetNodeId's closure: the node's content overlay + a writable
    // upper, NO runner and NO prefix (an empty runtime is valid — a fresh node has no content). This is a capture
    // workbench; tools (RunWindows / native runs / file drops) act on it. Returns false on mount failure.
    bool Begin(const NodeIndex &Idx, const std::string &TargetNodeId,
               const std::map<std::string, std::string> &VariableOverrides = {});

    // Tool: run a Windows program (installer, editor, …) in a wine/proton prefix. (Re)builds the runtime under the
    // CHOSEN runner if needed — its DEFPREFIX + CONTENT_ROOT define the env, the platform is seeded from the runner's
    // guest, independent of the package — then runs Exe. Unlocks registry capture (PrefixGenerate() becomes true).
    bool RunWindows(const std::string &Exe, const std::string &RunnerId);

    bool Live() const { return Wrapper != nullptr; }
    bool PrefixGenerate() const;                       // a wine-prefix runtime is live → gates the wine-only tools
    std::string RunnerId() const;                      // the boundary runner of the current runtime (empty when bare)
    std::filesystem::path RuntimePath() const;         // the live mount root
    std::filesystem::path WriteLayerPath() const;      // the COW delta (capture source)
    std::string ContentRoot() const;                   // where the runner roots game content (for TARGET derivation)

    // Runs Exe (a host path, or a guest command like "regedit.exe") against the LIVE mount. Returns false on a
    // non-zero / crashed exit. The runtime stays mounted.
    bool RunExe(const std::string &Exe);

    // baseline-vs-now registry diff → a RegEdit subcomponent array (empty if not a prefix session / no changes).
    nlohmann::ordered_json CaptureRegistryDelta() const;

    void End();

    // ---- File-capture MECHANISM (static, UI-agnostic, container-free → unit-testable) ----

    // Recursively lists FILE paths under Root, relative to Root ('/'-separated), with wine/prefix churn filtered
    // out (registry hives, the wine windows/ tree, dosdevices, temp). The capture-selection candidate set.
    static std::vector<std::string> EnumerateDelta(const std::filesystem::path &Root);

    // True if a runtime-relative path is prefix bookkeeping rather than capturable content (see EnumerateDelta).
    static bool IsChurn(const std::string &Rel);

    // Copies each selected relative path (file or dir) from SrcRoot into DestRoot, re-rooting by stripping
    // StripPrefix from the front of each (so a capture under a content root lands at the right place). Creates
    // DestRoot. Returns the number of files copied.
    static int CopySelection(const std::filesystem::path &SrcRoot, const std::vector<std::string> &Rels,
                             const std::filesystem::path &DestRoot, const std::string &StripPrefix = "");

    // Builds the VFSDirLayer json for a captured dir: { TYPE:"VFSDirLayer", PATH:<DirNameRelToBundle>, TARGET:<Target> }.
    static nlohmann::ordered_json MakeDirLayer(const std::string &DirNameRelToBundle, const std::string &Target);

private:
    // (Re)builds + mounts the runtime: bare (no runner) when Chain is empty, else under the pinned runner (its
    // prefix/content-root + the platform seeded from its guest). Snapshots the registry baseline for wine runtimes.
    bool BuildRuntime(const std::vector<std::string> &Chain);

    nlohmann::ordered_json                ConfigCopy;
    nlohmann::ordered_json                DummyManifest = nlohmann::ordered_json::object();
    NodeIndex                             Idx;             // owned copy (Params->NodeIdx points into this)
    QDir                                  BundleDir;
    std::string                           TargetId;        // the node the session is built around / captures into
    std::map<std::string, std::string>    VarOverrides;    // variable overrides for the build
    std::string                           CurrentRunnerId; // the runner the current runtime is built under ("" = bare)
    std::unique_ptr<ContainerParams>      Params;
    std::unique_ptr<ContainerWrapper>     Wrapper;
    RegistryWrapper                       Baseline;
    bool                                  HasBaseline = false;
};

#endif // AUTHORINGSESSION_H
