#include "packageeditormodel.h"
#include "apppaths.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "registrywrapper.h"
#include "containerwrapper.h"   // ContainerWrapper + ContainerParams (authoring run) — pulls LaunchResolver etc.
#include "packagecatalog.h"     // RepositoryDirs / PublishPackage

#include <QFile>
#include <QFileDialog>
#include <QMessageBox>

#include <algorithm>
#include <filesystem>
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

PackageEditorModel::PackageEditorModel(const nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent)
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
    const std::string Id = Node.is_object() ? Node.value("NODE_ID", std::string()) : std::string();
    if (!Id.empty()) return QString::fromStdString(Id) + ".json";
    if (Node.is_object() && Node.contains("__FILE__") && Node["__FILE__"].is_string()
        && !std::string(Node["__FILE__"]).empty())
        return QString::fromStdString(std::string(Node["__FILE__"]));
    return "untitled_node.json";
}

void PackageEditorModel::LoadNodes()
{
    Doc = json::object({ {"NODES", json::array()} });

    const QStringList Files = PackageDir->entryList(QStringList() << "*.json", QDir::Files, QDir::Name);
    for (const QString &FileName : Files)
    {
        nlohmann::ordered_json J;
        QFile F(PackageDir->filePath(FileName));
        if (JSONOps::LoadJSON(&F, &J) || !J.is_object()) continue;   // LoadJSON returns true on FAILURE
        if (!J.contains("NODE_ID")) continue;                        // non-node json (legacy MANIFEST.json, etc.)
        J["__FILE__"] = FileName.toStdString();
        Doc["NODES"].push_back(std::move(J));
    }

    if (Doc["NODES"].empty())
        Doc["NODES"].push_back(json::object({ {"NODE_ID", ""}, {"ROLE", "content"}, {"LAYERS", json::array()} }));

    Revalidate();
    LogSucc("PackageEditorModel", "Loaded " + std::to_string(Doc["NODES"].size()) + " node(s).");
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

void PackageEditorModel::SaveNodes()
{
    if (!PackageDir) return;
    InvalidateExecIndex();   // the bundle's nodes are changing — drop the cached catalog index
    auto &Nodes = Doc["NODES"];

    // Desired on-disk filenames for the current nodes.
    std::set<QString> Desired;
    for (auto &N : Nodes) { QString F = FileForNode(N); if (!F.isEmpty()) Desired.insert(F); }

    // Delete orphaned node files (a *.json holding a NODE_ID no longer backed by a current node — rename/delete).
    for (const QString &Existing : PackageDir->entryList(QStringList() << "*.json", QDir::Files))
    {
        if (Desired.count(Existing)) continue;
        nlohmann::ordered_json J; QFile F(PackageDir->filePath(Existing));
        if (!JSONOps::LoadJSON(&F, &J) && J.is_object() && J.contains("NODE_ID"))
            PackageDir->remove(Existing);
    }

    // Write each node to its file (stripping the editor-only tag), keeping the tag synced to where we wrote it.
    bool Ok = true;
    for (auto &N : Nodes)
    {
        QString FileName = FileForNode(N);
        if (FileName.isEmpty()) continue;
        nlohmann::ordered_json Out = N; Out.erase("__FILE__");
        N["__FILE__"] = FileName.toStdString();
        QFile F(PackageDir->filePath(FileName));
        if (!JSONOps::SaveJSON(&Out, &F)) Ok = false;
    }
    if (!Ok) LogErr("PackageEditorModel", "One or more node files failed to save.");

    emit savedToDisk(PackageDir->path());
    Revalidate();   // emits validationChanged
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
            const std::string Id = N.value("NODE_ID", std::string());
            if (Id.empty()) continue;
            Scope.insert(Id);
            for (const std::string & Dep : ManifestModel::ResolveNodeOrder(Idx, Id, {})) Scope.insert(Dep);
        }
    ManifestModel::ValidateNodeGraph(Idx, ValErrors, ValWarnings, &Scope);
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
    for (const std::string &RepoDir : PackageCatalog::RepositoryDirs(*GlobalConfigJSON))
    {
        QDir D(QString::fromStdString(RepoDir));
        if (!D.exists()) continue;
        for (const QString &Sub : D.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            ManifestModel::ScanBundleNodes(D.filePath(Sub).toStdString(), Idx);
    }
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

    Container.Cleanup();
    if (!Container.BuildContainerRuntime())
    { QMessageBox::critical(DialogParent, "Run", "Failed to build the container runtime. Check the log."); Container.Cleanup(); return; }
    if (!Container.Execute(Exe))
        QMessageBox::warning(DialogParent, "Run", "Process exited with an error. Check the log.");
    Container.Cleanup();
}

void PackageEditorModel::AnalyzeNodeRegistry(const std::string & NodeId)
{
    SaveNodes();
    NodeIndex Idx = BuildExecIndex();
    const std::string Launch = LaunchableForNode(Idx, NodeId);
    if (Launch.empty())
    { QMessageBox::warning(DialogParent, "Analyze Registry", "No launchable node includes this one — nothing to analyze against."); return; }

    const Node * LaunchNode = Idx.Find(Launch);
    const std::string PackageUID = LaunchNode ? LaunchNode->Uid : PackageDir->dirName().toStdString();
    std::filesystem::path SessionTemp = AppPaths::DataRoot() / "TEMP" / PackageUID;

    // Comparator: the launchable's baseline state in read-only mode, on isolated paths.
    ContainerParams ComparatorParams(PackageDir->path().toStdString());
    ComparatorParams.NodeIdx = &Idx;
    ComparatorParams.LaunchNodeId = Launch;
    nlohmann::ordered_json Dummy = nlohmann::ordered_json::object();
    ContainerWrapper Comparator(*GlobalConfigJSON, Dummy, ComparatorParams);
    Comparator.ContainerParams.TempPath      = SessionTemp / "COMPARATOR_TEMP";
    Comparator.ContainerParams.DefPrefixPath = SessionTemp / "COMPARATOR_TEMP" / "DEFPREFIX";
    Comparator.ContainerParams.RuntimePath   = SessionTemp / "COMPARATOR";
    Comparator.ContainerParams.ReadOnlyVFS   = true;
    Comparator.Cleanup();
    Comparator.BuildContainerRuntime();

    RegistryWrapper Baseline;
    Baseline.LoadPrefix(Comparator.ContainerParams.RuntimePath);
    Comparator.Cleanup();

    RegistryWrapper After;
    After.LoadPrefix(SessionTemp / "WRITELAYER");

    nlohmann::ordered_json Delta = After.DiffToRegEdits(Baseline);
    LogOut("PackageEditorModel::AnalyzeNodeRegistry", "Delta: " + std::to_string(Delta.size()) + " RegEdit layer(s).");

    if (!Delta.empty())
    {
        // Locate NodeId's array index in the working document.
        int ArrIdx = -1;
        for (int n = 0; n < (int)Doc["NODES"].size(); n++)
            if (Doc["NODES"][n].value("NODE_ID", std::string()) == NodeId) { ArrIdx = n; break; }
        if (ArrIdx >= 0) MergeRegistryDeltaInNode(&Delta, ArrIdx);
    }

    SaveNodes();
    emit documentReloaded();
}

void PackageEditorModel::MergeRegistryDeltaInNode(nlohmann::ordered_json * Delta, int NodeIndexInArray)
{
    if (NodeIndexInArray < 0 || NodeIndexInArray >= (int)Doc["NODES"].size()) return;
    auto &Layers = Doc["NODES"][NodeIndexInArray]["LAYERS"];
    if (!Layers.is_array()) Layers = json::array();

    for (int i = 0; i < (int)Delta->size(); i++)
    {
        auto &DeltaLayer = (*Delta)[i];
        bool Merged = false;
        for (int j = 0; j < (int)Layers.size(); j++)
        {
            auto &Existing = Layers[j];
            if (!Existing.is_object() || Existing.value("TYPE", std::string()) != "RegEdit") continue;
            if (DeltaLayer.value("REGPATH", std::string()) == Existing.value("REGPATH", std::string()))
            {
                for (auto KV : DeltaLayer["KEYVALUES"].items()) Existing["KEYVALUES"][KV.key()] = KV.value();
                Merged = true; break;
            }
        }
        if (!Merged) Layers.push_back(DeltaLayer);
    }
}
