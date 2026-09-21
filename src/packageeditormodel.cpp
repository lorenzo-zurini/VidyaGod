#include "packageeditormodel.h"
#include "apppaths.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "registrywrapper.h"
#include "containerwrapper.h"   // ContainerWrapper + ContainerParams (authoring run) — pulls LaunchResolver etc.
#include "packagecatalog.h"     // BuildCatalogIndex / PublishPackage

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

//The launchable node to run when authoring `nodeId`: the node itself if launchable, else the first launchable in
//the index whose resolved closure includes it (so its layers are in the recipe). "" if none.
static std::string LaunchableForNode(const NodeIndex & Idx, const std::string & nodeId)
{
    const Node * N = Idx.Find(nodeId);
    if (!N) return "";
    if (N->IsLaunchable()) return nodeId;
    for (const auto & [Id, Node] : Idx.Nodes)
    {
        if (!Node.IsLaunchable()) continue;
        const auto Order = ManifestModel::ResolveNodeOrder(Idx, Id, {});
        if (std::find(Order.begin(), Order.end(), nodeId) != Order.end()) return Id;
    }
    return "";
}

PackageEditorModel::PackageEditorModel(nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent)
    : QObject(parent), GlobalConfigJSON(globalConfig), DialogParent(dialogParent)
{
    Doc = json::object({ {"NODES", json::array()} });
}

PackageEditorModel::~PackageEditorModel() { delete PackageDir; }

// ============================================================================
// Bundle open + node I/O (one file per node)
// ============================================================================

void PackageEditorModel::initPackage(const QString & preselectedPath, QWidget * dirPickerParent)
{
    QString ChosenPath = preselectedPath.isEmpty()
        ? QFileDialog::getExistingDirectory(dirPickerParent, "Select bundle directory...")
        : preselectedPath;
    delete PackageDir;
    PackageDir = new QDir(ChosenPath);
    LoadNodes();
}

QString PackageEditorModel::FileForNode(const nlohmann::ordered_json & Node) const
{
    //Prefer <NODE_ID>.json so a rename re-files the node (SaveNodes then cleans the stale file).
    std::string Id = (Node.is_object() && Node.contains("LABEL") && Node["LABEL"].is_string()) ? Node["LABEL"].get<std::string>() : std::string();
    //A node id becomes a FILENAME here, so a '/' or a ".." would write outside the bundle. Ids are authored
    //freely on the canvas; sanitise rather than trust.
    for (char &C : Id) if (C == '/' || C == '\\' || C == ':') C = '_';
    if (Id == "." || Id == "..") Id = "node";
    if (!Id.empty()) return QString::fromStdString(Id) + ".json";
    if (Node.is_object() && Node.contains("__FILE__") && Node["__FILE__"].is_string()
        && !std::string(Node["__FILE__"]).empty())
        return QString::fromStdString(std::string(Node["__FILE__"]));
    return "untitled_node.json";
}

void PackageEditorModel::LoadNodes()
{
    Doc = json::object({ {"NODES", json::array()} });
    Carried.clear();

    const QStringList Files = PackageDir->entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    for (const QString &FileName : Files)
    {
        nlohmann::ordered_json J;
        QFile F(PackageDir->filePath(FileName));
        if (JSONOps::LoadJSON(&F, &J)) continue;                     // LoadJSON returns true on FAILURE
        //A file holds ONE node or an ARRAY of them — grouping nodes into files is pure presentation, so the
        //editor reads either and (see SaveNodes) writes back the grouping it found.
        if (J.is_object() && J.contains("TYPE"))
        {
            J["__FILE__"] = FileName.toStdString();
            Doc["NODES"].push_back(std::move(J));
        }
        else if (J.is_array())
        {
            //An entry we do not recognise as a node is CARRIED, not dropped: SaveNodes rewrites the whole file
            //from what was loaded, so skipping an element here would erase it from disk the next time anything
            //in that file is touched.
            nlohmann::ordered_json Strays = nlohmann::ordered_json::array();
            for (auto &N : J)
            {
                if (N.is_object() && N.contains("TYPE"))
                {
                    N["__FILE__"] = FileName.toStdString();
                    Doc["NODES"].push_back(std::move(N));
                }
                else Strays.push_back(N);
            }
            if (!Strays.empty())
            {
                Carried[FileName.toStdString()] = std::move(Strays);
                LogWarn("PackageEditorModel", "Carrying " + std::to_string(Carried[FileName.toStdString()].size())
                        + " unrecognised entr(ies) in " + FileName.toStdString() + " through untouched.");
            }
        }
    }

    if (Doc["NODES"].empty())
        Doc["NODES"].push_back(json::object({ {"LABEL", ""}, {"TYPE", "Group"} }));   // pure composition until given a payload

    Validated = false; emit validationChanged();   // validation is on-demand ("Check Package Validity"); don't auto-run on load
    LoadLayout();
    LogSucc("PackageEditorModel", "Loaded " + std::to_string(Doc["NODES"].size()) + " node(s).");
}

//The canvas layout sidecar.
//
//NOT in the node files: a Meta-CID is minted add-by-reference and IN PLACE over those very files, so anything
//stored there is in the CID and no later stage can strip it — every drag would republish the package. (A
//node's POS is the exception, and deliberately so: it is written once at PUBLISH, not on every mouse-up.)
//
//NOT under USERDATA either, which was the first attempt: a `Persist KEEP %RuntimePath%` node makes
//<bundle>/USERDATA the writable TOP branch of the union (vfsmount.cpp), so its whole contents appear at the
//game's runtime root — the layout would ship into the game's own directory, visible to mod loaders and
//data-file scanners, and durably.
//
//NOT a sidecar in the bundle root either, which is what this used to be. It was invisible to publishing (both
//paths are text-only-JSON), but a bundle is still the wrong home for per-machine state: it travels with the
//folder whenever the author copies, moves or backs it up, so one person's canvas arrangement rides along into
//everyone else's copy.
//
//So: the tool's own per-user configuration, which is what per-machine state is.
//The GlobalConfig key for a bundle's local positions. Delegates to PackageCatalog::EditorLayoutKey so the
//editor (which writes it) and publishing (which reads it) cannot drift: they already did once, and a mismatch
//is silent — the lookup misses and the author's arrangement is discarded at publish.
static std::string LayoutKeyFor(const QDir *PackageDir)
{
    if (!PackageDir) return {};
    return PackageCatalog::EditorLayoutKey(
        std::filesystem::path(QDir::cleanPath(PackageDir->absolutePath()).toStdString()));
}

void PackageEditorModel::LoadLayout()
{
    //THIS MACHINE's positions only. A node's published default lives in the node itself (POS) and is applied by
    //PkgGraph::Build; this object is the local override laid on top, so dragging a box is a preference on this
    //computer and never an edit to the package.
    Layout = nlohmann::ordered_json::object();
    const std::string Key = LayoutKeyFor(PackageDir);
    if (Key.empty() || !GlobalConfigJSON) return;
    const auto SecIt = GlobalConfigJSON->find("EDITORLAYOUT");
    if (SecIt == GlobalConfigJSON->end() || !SecIt->is_object()) return;
    const auto BundleIt = SecIt->find(Key);
    if (BundleIt == SecIt->end() || !BundleIt->is_object()) return;
    Layout = *BundleIt;
}

void PackageEditorModel::SaveLayout()
{
    const std::string Key = LayoutKeyFor(PackageDir);
    if (Key.empty() || !GlobalConfigJSON || !Layout.is_object()) return;
    //An empty override is not worth a stanza: once every node carries POS, the common case is a bundle nobody
    //has dragged, and writing "{}" for each would grow GlobalConfig by one entry per bundle ever opened.
    if (Layout.empty())
    {
        auto SecIt = GlobalConfigJSON->find("EDITORLAYOUT");
        if (SecIt != GlobalConfigJSON->end() && SecIt->is_object()) SecIt->erase(Key);
        return;
    }
    if (!GlobalConfigJSON->contains("EDITORLAYOUT") || !(*GlobalConfigJSON)["EDITORLAYOUT"].is_object())
        (*GlobalConfigJSON)["EDITORLAYOUT"] = nlohmann::ordered_json::object();
    (*GlobalConfigJSON)["EDITORLAYOUT"][Key] = Layout;
    //...and reach DISK. The sidecar this replaced was written on every mouse-up; GlobalConfig is otherwise
    //only flushed by MainWindow::closeEvent, so a crash, a kill, or any exit that skips closeEvent would lose
    //the whole session's arranging — a strictly worse guarantee than the file it replaced.
    //SaveJSON returns TRUE ON SUCCESS (every other call site reads it that way). Inverted, this logged "could
    //not flush" after every successful save and said nothing when the write actually failed.
    QFile Cfg(QString::fromStdString((AppPaths::DataRoot() / "GlobalConfig.JSON").string()));
    if (!JSONOps::SaveJSON(GlobalConfigJSON, &Cfg))
        LogWarn("PackageEditorModel", "could not flush the canvas layout to GlobalConfig.JSON");
}

void PackageEditorModel::replaceNodeJson(int nodeIndex, nlohmann::ordered_json node)
{
    if (nodeIndex < 0 || nodeIndex >= (int)Doc["NODES"].size()) return;
    // Preserve the editor-only provenance tag so a raw-JSON save doesn't re-file the node.
    if (Doc["NODES"][nodeIndex].contains("__FILE__")) node["__FILE__"] = Doc["NODES"][nodeIndex]["__FILE__"];
    Doc["NODES"][nodeIndex] = std::move(node);
    SaveNodes();
    emit documentReloaded();
}

void PackageEditorModel::updateNodeLive(int nodeIndex, nlohmann::ordered_json node)
{
    if (nodeIndex < 0 || nodeIndex >= (int)Doc["NODES"].size()) return;
    if (Doc["NODES"][nodeIndex].contains("__FILE__")) node["__FILE__"] = Doc["NODES"][nodeIndex]["__FILE__"];
    Doc["NODES"][nodeIndex] = std::move(node);
    SaveNodes();                 // persist + revalidate (SaveNodes emits savedToDisk + validationChanged)
    emit nodeContentChanged();   // canvas repaints from the doc; NO documentReloaded → the JSON editor survives
}

void PackageEditorModel::SaveNodes()
{
    if (!PackageDir) return;
    InvalidateExecIndex();   // the bundle's nodes are changing — drop the cached catalog index
    auto &Nodes = Doc["NODES"];

    // Which file each node belongs to. A node whose __FILE__ is SHARED with others stays in that file (the
    // author grouped them; file grouping carries no semantics, so the editor must not silently re-shuffle it).
    // A node that owns its file keeps the old behaviour — <NODE_ID>.json, so a rename re-files it.
    std::map<std::string, int> Occupants;
    for (auto &N : Nodes)
        if (N.contains("__FILE__") && N["__FILE__"].is_string()) ++Occupants[N["__FILE__"].get<std::string>()];
    auto TargetFile = [&](nlohmann::ordered_json &N) -> QString {
        if (N.contains("__FILE__") && N["__FILE__"].is_string())
        {
            const std::string F = N["__FILE__"].get<std::string>();
            //Keep the file when it is SHARED, and also when it carries entries we did not parse as nodes —
            //re-filing the node away from it would orphan the file, and the orphan sweep would then delete
            //the strays with it.
            if (Occupants[F] > 1 || Carried.count(F)) return QString::fromStdString(F);
        }
        return FileForNode(N);
    };

    // Group the nodes by destination file, preserving document order within each.
    std::map<QString, std::vector<nlohmann::ordered_json *>> ByFile;
    std::vector<QString> FileOrder;
    for (auto &N : Nodes)
    {
        const QString F = TargetFile(N);
        if (F.isEmpty()) continue;
        if (ByFile.find(F) == ByFile.end()) FileOrder.push_back(F);
        ByFile[F].push_back(&N);
    }

    // Write each file: one node → an object, several → an array (the form it was read in).
    // WRITES COME FIRST. Deleting orphans up front meant a failed write (full disk, read-only bundle) left the
    // node existing only in memory, with the old file already gone and nothing but a log line to say so.
    bool Ok = true;
    for (const QString &FileName : FileOrder)
    {
        auto &Group = ByFile[FileName];
        nlohmann::ordered_json Out;
        if (Group.size() == 1) { Out = *Group.front(); Out.erase("__FILE__"); }
        else
        {
            Out = nlohmann::ordered_json::array();
            for (auto *N : Group) { nlohmann::ordered_json C = *N; C.erase("__FILE__"); Out.push_back(std::move(C)); }
        }
        //Re-append anything this file held that we did not parse as a node, so a save never erases it.
        if (auto Cit = Carried.find(FileName.toStdString()); Cit != Carried.end())
        {
            if (!Out.is_array()) { nlohmann::ordered_json A = nlohmann::ordered_json::array(); A.push_back(Out); Out = std::move(A); }
            for (const auto &X : Cit->second) Out.push_back(X);
        }
        for (auto *N : Group) (*N)["__FILE__"] = FileName.toStdString();
        QFile F(PackageDir->filePath(FileName));
        if (!JSONOps::SaveJSON(&Out, &F)) Ok = false;
    }
    if (!Ok)
    {
        // Nothing is swept when a write failed: the stale files on disk are now the only copy of whatever did
        // not get written, so removing them would turn a failed save into data loss.
        LogErr("PackageEditorModel", "One or more node files failed to save — leaving every existing file in "
                                     "place. Fix the problem (disk full? read-only bundle?) and save again.");
        emit savedToDisk(PackageDir->path());
        Validated = false; emit validationChanged();
        return;
    }

    // Only now sweep the files that are genuinely orphaned (a *.json holding nodes no current node claims).
    for (const QString &Existing : PackageDir->entryList(QStringList() << "*.json", QDir::Files))
    {
        if (ByFile.count(Existing)) continue;
        nlohmann::ordered_json J; QFile F(PackageDir->filePath(Existing));
        if (JSONOps::LoadJSON(&F, &J)) continue;
        const bool IsNodeFile = (J.is_object() && J.contains("TYPE"))
                             || (J.is_array() && !J.empty() && J[0].is_object() && J[0].contains("TYPE"));
        if (IsNodeFile) PackageDir->remove(Existing);
    }

    emit savedToDisk(PackageDir->path());
    Validated = false; emit validationChanged();   // edits invalidate the last check; re-run on demand via the button
}

// ============================================================================
// Validation
// ============================================================================

const NodeIndex & PackageEditorModel::ExecIndex() const
{
    if (!ExecIndexValid) { ExecIndexCache = BuildExecIndex(); ExecIndexValid = true; }
    return ExecIndexCache;
}

void PackageEditorModel::Revalidate()
{
    ValErrors.clear(); ValWarnings.clear();
    const NodeIndex & Idx = ExecIndex();
    // Scope validation to THIS bundle's own nodes + their PARENTS closures, so unrelated packages' legitimate issues
    // (e.g. another game's cross-layer case collisions) never surface while editing — mirrors LibraryGameCard::play.
    std::set<std::string> Scope;
    if (Doc.contains("NODES") && Doc["NODES"].is_array())
        for (const auto & N : Doc["NODES"])
        {
            const std::string Id = (N.contains("LABEL") && N["LABEL"].is_string()) ? N["LABEL"].get<std::string>() : std::string();
            if (Id.empty()) continue;
            Scope.insert(Id);
            for (const std::string & Dep : ManifestModel::ResolveNodeOrder(Idx, Id, {})) Scope.insert(Dep);
        }
    ManifestModel::ValidateNodeGraph(Idx, ValErrors, ValWarnings, &Scope);
    Validated = true;
    emit validationChanged();
}

// ============================================================================
// Catalog queries (PARENTS picker / platform suggestions / exec index)
// ============================================================================

NodeIndex PackageEditorModel::BuildExecIndex() const
{
    NodeIndex Idx;
    if (PackageDir)
        ManifestModel::ScanBundleNodes(PackageDir->path().toStdString(), Idx);   // this bundle wins (first-seen)
    // Merge in the rest of the catalog (CID package sources + local bundles); std::map::emplace keeps this bundle's nodes.
    NodeIndex Cat = PackageCatalog::BuildCatalogIndex(*GlobalConfigJSON);
    for (auto &[Id, N] : Cat.Nodes) Idx.Nodes.emplace(Id, N);
    ManifestModel::LinkGames(Idx);   // link variants to their game nodes (graph-edge grouping)
    return Idx;
}

std::vector<std::string> PackageEditorModel::KnownNodeIds()
{
    const NodeIndex & Idx = ExecIndex();
    std::vector<std::string> Out;
    for (const auto &[Id, N] : Idx.Nodes) { (void)N; Out.push_back(Id); }
    std::sort(Out.begin(), Out.end());
    return Out;   // (std::map already sorted, but keep explicit)
}

std::vector<std::string> PackageEditorModel::KnownPlatforms()
{
    const NodeIndex & Idx = ExecIndex();
    std::set<std::string> Seen;
    std::vector<std::string> Out;
    for (const auto &[Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        for (const auto &P : N.GuestPlatform) if (Seen.insert(P).second) Out.push_back(P);
    }
    for (const char *Common : {"win32", "win64", "linux64", "snes", "custom"})
        if (Seen.insert(Common).second) Out.push_back(Common);
    return Out;
}

// ============================================================================
// Authoring execute (native node engine)
// ============================================================================

bool PackageEditorModel::NodeTestable(const std::string & NodeId) const
{
    return !LaunchableForNode(ExecIndex(), NodeId).empty();
}

void PackageEditorModel::RunInNode(const std::string & NodeId, const std::string & Exe)
{
    SaveNodes();
    NodeIndex Idx = BuildExecIndex();
    const std::string Launch = LaunchableForNode(Idx, NodeId);
    if (Launch.empty())
    {
        QMessageBox::warning(DialogParent, "Run",
            "This node isn't launchable and no launchable node in the bundle includes it.\n"
            "Add a launchable node (with this one as a parent) to test it.");
        return;
    }

    ContainerParams Params(PackageDir->path().toStdString());
    Params.NodeIdx = &Idx;
    Params.LaunchNodeId = Launch;
    nlohmann::ordered_json Dummy = nlohmann::ordered_json::object();
    ContainerWrapper Container(*GlobalConfigJSON, Dummy, Params);

    // Resolve the exec (CONTENTPATH → %ContentPath%/%Content%) — same step the real launch path runs (launchthread/
    // main). Without it ExePathRelative stays empty, so a wine runner's "C:\<UID>\%ContentPath%" arg becomes the bare
    // content dir and wine opens it in its file explorer instead of running the game.
    if (!LaunchResolver::ResolveExecutableDefinition(Dummy, Container.ContainerParams))
    { QMessageBox::warning(DialogParent, "Run", "Could not resolve the node's exec (CONTENTPATH). Check the log."); return; }

    Container.Cleanup();
    if (!Container.BuildContainerRuntime())
    { QMessageBox::critical(DialogParent, "Run", "Failed to build the container runtime. Check the log."); Container.Cleanup(); return; }
    if (!Container.Execute(Exe))
        QMessageBox::warning(DialogParent, "Run", "Process exited with an error. Check the log.");
    Container.Cleanup();
}


std::string PackageEditorModel::createNode(nlohmann::ordered_json Payload,
                                           const std::vector<std::string> & Parents,
                                           const std::string & IdHint)
{
    auto Exists = [this](const std::string & Id) {
        for (const auto & N : Doc["NODES"]) if (N.contains("LABEL") && N["LABEL"].is_string() && N["LABEL"].get<std::string>() == Id) return true;
        return false;
    };
    std::string Base = IdHint.empty() ? std::string("node") : IdHint;
    for (char & C : Base) if (!std::isalnum((unsigned char)C) && C != '_') C = '_';
    std::string Id = Base;
    for (int K = 2; Exists(Id); ++K) Id = Base + "_" + std::to_string(K);

    nlohmann::ordered_json N = nlohmann::ordered_json::object({{"LABEL", Id}});
    nlohmann::ordered_json P = nlohmann::ordered_json::array();
    for (const std::string & X : Parents) if (!X.empty()) P.push_back(X);
    N["PARENTS"] = std::move(P);
    for (const auto & [K, V] : Payload.items()) N[K] = V;
    Doc["NODES"].push_back(std::move(N));
    SaveNodes();
    emit documentReloaded();
    LogSucc("PackageEditorModel", "Created node '" + Id + "'.");
    return Id;
}

QString PackageEditorModel::packagePath() const
{
    return PackageDir ? PackageDir->path() : QString();
}

std::vector<std::string> PackageEditorModel::bundleNodeIds() const
{
    std::vector<std::string> Out;
    if (Doc.contains("NODES") && Doc["NODES"].is_array())
        for (const auto & N : Doc["NODES"])
        {
            const std::string Id = (N.contains("LABEL") && N["LABEL"].is_string()) ? N["LABEL"].get<std::string>() : std::string();
            if (!Id.empty()) Out.push_back(Id);
        }
    return Out;
}



