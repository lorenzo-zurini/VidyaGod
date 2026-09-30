#include "packageeditormodel.h"
#include "nodelower.h"

#include <QThread>
#include <QTimer>
#include "apppaths.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "containerwrapper.h"   // ContainerWrapper + ContainerParams (authoring run) — pulls LaunchResolver etc.
#include "packagecatalog.h"     // BuildCatalogIndex, LibraryRootDir, EditorLayoutKey
#include "instancestore.h"

#include <QFile>
#include <QFileDialog>
#include <QMessageBox>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

//A launchable that runs NodeId: the node itself, or one that contains it.
std::string LaunchableForNode(const NodeIndex & Idx, const std::string & NodeId)
{
    const Node * N = Idx.Find(NodeId);
    if (!N) return "";
    if (N->IsVariant()) return NodeId;
    for (const auto & [Id, Nd] : Idx.Nodes)
    {
        if (!Nd.IsVariant()) continue;
        const auto Order = ManifestModel::Closure(Idx, Id);
        if (std::find(Order.begin(), Order.end(), NodeId) != Order.end()) return Id;
    }
    return "";
}

bool SameDir(const fs::path & A, const fs::path & B)
{
    std::error_code Ea, Eb;
    return fs::weakly_canonical(A, Ea) == fs::weakly_canonical(B, Eb);
}

//"[749][v1.0] Age of Empires II" → "Age of Empires II": a package's folder name without its bracketed tags.
std::string PackageName(const fs::path & Dir)
{
    std::string N = Dir.filename().string();
    while (!N.empty() && N[0] == '[')
    {
        const size_t E = N.find(']');
        if (E == std::string::npos) break;
        N = N.substr(E + 1);
        while (!N.empty() && N[0] == ' ') N.erase(0, 1);
    }
    return N.empty() ? Dir.filename().string() : N;
}

} // namespace

PackageEditorModel::PackageEditorModel(nlohmann::ordered_json * globalConfig, QWidget * dialogParent, QObject * parent)
    : QObject(parent), GlobalConfigJSON(globalConfig), DialogParent(dialogParent)
{
}

PackageEditorModel::~PackageEditorModel()
{
    if (ValThread) { ValThread->wait(); delete ValThread; }   // it holds its own snapshot; only its result is ours
    delete PackageDir;
}

QString PackageEditorModel::packagePath() const { return PackageDir ? PackageDir->path() : QString(); }

void PackageEditorModel::initPackage(const QString & preselectedPath, QWidget * dirPickerParent)
{
    const QString Chosen = preselectedPath.isEmpty()
        ? QFileDialog::getExistingDirectory(dirPickerParent, "Open a package folder")
        : preselectedPath;
    delete PackageDir;
    PackageDir = Chosen.isEmpty() ? nullptr : new QDir(Chosen);
    LoadNodes();
}

void PackageEditorModel::LoadNodes()
{
    if (PackageDir)
        for (const std::string & Odd : Doc.Load(PackageDir->path().toStdString()))
            LogWarn("PackageEditorModel", Odd);
    else Doc.Reset({});
    LoadLayout();
    Validated = false;
    ExecIndexRev = 0;
    if (ValThread) ValAgain = true;                       // a check under way is about the package that was here
    WasDirty = false;
    emit documentReloaded();
    emit dirtyChanged(false);
    emit validationChanged();
    LogSucc("PackageEditorModel", "Loaded " + std::to_string(Doc.Count()) + " node(s) from " + packagePath().toStdString());
}

// ---- this machine's layout: GlobalConfig EDITORLAYOUT[<package>] = {handle: [x, y]} ------------------------------

static std::string LayoutKeyFor(const QDir * PackageDir)
{
    if (!PackageDir) return {};
    return PackageCatalog::EditorLayoutKey(fs::path(QDir::cleanPath(PackageDir->absolutePath()).toStdString()));
}

void PackageEditorModel::LoadLayout()
{
    SavedLayout.clear();
    const std::string Key = LayoutKeyFor(PackageDir);
    if (Key.empty() || !GlobalConfigJSON) return;
    const auto Sec = GlobalConfigJSON->find("EDITORLAYOUT");
    if (Sec == GlobalConfigJSON->end() || !Sec->is_object()) return;
    const auto B = Sec->find(Key);
    if (B == Sec->end() || !B->is_object()) return;
    for (const auto & [H, P] : B->items())
    {
        //A coordinate no layout could produce is corruption, not a position: refused, so the node is laid out instead.
        if (!P.is_array() || P.size() != 2 || !P[0].is_number() || !P[1].is_number()) continue;
        const double X = P[0].get<double>(), Y = P[1].get<double>();
        if (!std::isfinite(X) || !std::isfinite(Y) || std::abs(X) > 1e7 || std::abs(Y) > 1e7) continue;
        SavedLayout[H] = PkgDoc::Pos{(float)X, (float)Y};
    }
    Doc.SetPositions(SavedLayout);
}

void PackageEditorModel::SaveLayout()
{
    const std::string Key = LayoutKeyFor(PackageDir);
    if (Key.empty() || !GlobalConfigJSON) return;
    const auto & Now = Doc.Positions();
    bool Same = Now.size() == SavedLayout.size();
    if (Same)
        for (const auto & [H, P] : Now)
        {
            const auto It = SavedLayout.find(H);
            if (It == SavedLayout.end() || It->second.X != P.X || It->second.Y != P.Y) { Same = false; break; }
        }
    if (Same) return;
    json L = json::object();
    for (const auto & [H, P] : Now) L[H] = json::array({P.X, P.Y});
    if (!GlobalConfigJSON->contains("EDITORLAYOUT") || !(*GlobalConfigJSON)["EDITORLAYOUT"].is_object())
        (*GlobalConfigJSON)["EDITORLAYOUT"] = json::object();
    if (L.empty()) (*GlobalConfigJSON)["EDITORLAYOUT"].erase(Key);
    else (*GlobalConfigJSON)["EDITORLAYOUT"][Key] = std::move(L);
    SavedLayout = Now;
    QFile Cfg(QString::fromStdString((AppPaths::DataRoot() / "GlobalConfig.JSON").string()));
    if (!JSONOps::SaveJSON(GlobalConfigJSON, &Cfg))
        LogWarn("PackageEditorModel", "could not write the canvas layout to GlobalConfig.JSON");
}

// ---- editing and saving ----------------------------------------------------------------------------------------

void PackageEditorModel::noteEdited()
{
    SaveLayout();
    if (ValContentRev != Doc.ContentRevision()) validateSoon();   // a move is not an edit worth checking
    if (Doc.Dirty() != WasDirty) { WasDirty = Doc.Dirty(); emit dirtyChanged(WasDirty); }
}

void PackageEditorModel::replaceNode(const std::string & Handle, json Node)
{
    const int I = Doc.IndexOf(Handle);
    if (I < 0) return;
    Node.erase("CID");                                    // the handle is never part of a node
    Doc.Replace(I, std::move(Node));
    Doc.Commit();
    noteEdited();
    emit nodeContentChanged();
}

bool PackageEditorModel::Save(QString * Error)
{
    if (!PackageDir) return false;
    const std::vector<fs::path> Roots = GlobalConfigJSON ? PackageCatalog::EditableRoots(*GlobalConfigJSON) : std::vector<fs::path>();
    const std::string Users = GlobalConfigJSON ? InstanceStore::Root(*GlobalConfigJSON).string() : std::string();
    const PkgDoc::SaveReport R = Doc.Save(PackageDir->path().toStdString(), Roots, Users);
    for (const std::string & L : R.Log) LogOut("PackageEditorModel", L);
    if (!R.Ok)
    {
        LogErr("PackageEditorModel", "Save failed: " + R.Error);
        if (Error) *Error = QString::fromStdString(R.Error);
        return false;
    }
    LogSucc("PackageEditorModel", "Saved " + packagePath().toStdString() + ": " + std::to_string(R.Written) + " file(s) written, "
            + std::to_string(R.Removed) + " replaced" + (R.Cascaded ? ", " + std::to_string(R.Cascaded) + " node(s) in other packages re-minted" : "")
            + (R.InstancesUpdated ? ", " + std::to_string(R.InstancesUpdated) + " instance(s) updated" : ""));
    if (R.Cascaded) LibraryCache.reset();                 // other packages changed on disk
    SaveNotes = R.Warnings;
    for (const std::string & K : R.Kept)
        SaveNotes.push_back("A deleted node was kept on disk (" + K + "): another package or an instance still names it, "
                            "and this package holds its only copy.");
    for (const std::string & W : R.Warnings) LogWarn("PackageEditorModel", W);
    //Other packages' nodes the save renamed keep their place on this machine's canvas too.
    if (GlobalConfigJSON && R.Cascaded && GlobalConfigJSON->contains("EDITORLAYOUT") && (*GlobalConfigJSON)["EDITORLAYOUT"].is_object())
        for (auto & [Pkg, Layout] : (*GlobalConfigJSON)["EDITORLAYOUT"].items())
        {
            if (!Layout.is_object()) continue;
            json Moved = json::object();
            for (auto & [H, P] : Layout.items())
            {
                const auto It = R.Renamed.find(H);
                Moved[It == R.Renamed.end() ? H : It->second] = P;
            }
            Layout = std::move(Moved);
        }
    LastRenames = Doc.TakeRenames();
    if (!LastRenames.empty()) emit handlesRenamed();
    SaveLayout();                                         // positions follow their renamed nodes
    ExecIndexRev = 0;
    if (ValThread) ValAgain = true;                       // a check under way names the nodes by their old names
    WasDirty = false;
    emit dirtyChanged(false);
    emit savedToDisk(packagePath());
    return true;
}

// ---- the library ----------------------------------------------------------------------------------------------

const NodeIndex & PackageEditorModel::LibraryIndex() const
{
    if (!LibraryCache)
    {
        LibraryCache = std::make_unique<NodeIndex>(GlobalConfigJSON ? PackageCatalog::BuildCatalogIndex(*GlobalConfigJSON) : NodeIndex());
    }
    return *LibraryCache;
}

NodeIndex PackageEditorModel::BuildExecIndex() const
{
    NodeIndex Idx;
    const fs::path Pkg = PackageDir ? fs::path(PackageDir->path().toStdString()) : fs::path();
    for (const auto & [Id, N] : LibraryIndex().Nodes)
        if (Pkg.empty() || !SameDir(N.BundleDir, Pkg)) Idx.Nodes.emplace(Id, N);   // this package: as edited, below
    for (int I = 0; I < Doc.Count(); ++I)
    {
        Node N;
        if (!ManifestModel::ParseNode(Doc.Node(I), Pkg / (Doc.Handle(I) + ".json"), Pkg, N)) continue;
        N.Cid = Doc.Handle(I);
        //Named by its handle, as everything about this package's nodes is — a draft has no CID for the file name
        //to give, and a problem the canvas cannot place on its node is one the author does not see.
        if (!N.LowerError.empty()) N.LowerError = NodeLower::CheckNode(Doc.Node(I), N.Cid, N.NodeId);
        Idx.Nodes[Doc.Handle(I)] = std::move(N);
    }
    ManifestModel::DeriveFacts(Idx);
    return Idx;
}

std::shared_ptr<const NodeIndex> PackageEditorModel::ExecSnapshot() const
{
    if (!ExecIndexCache || ExecIndexRev != Doc.ContentRevision())
    {
        ExecIndexCache = std::make_shared<const NodeIndex>(BuildExecIndex());
        ExecIndexRev = Doc.ContentRevision();
    }
    return ExecIndexCache;
}

const NodeIndex & PackageEditorModel::ExecIndex() const { return *ExecSnapshot(); }

PackageEditorModel::External PackageEditorModel::externalInfo(const std::string & Cid) const
{
    const Node * N = LibraryIndex().Find(Cid);
    if (!N) return {};
    std::string Label = N->NodeId;
    if (Label.empty()) Label = N->Meta.is_object() ? N->Meta.value("TITLE", std::string()) : std::string();
    return External{Label, PackageName(N->BundleDir), N->BundleDir.string()};
}

std::vector<PackageEditorModel::Offer> PackageEditorModel::offers() const
{
    std::vector<Offer> Out;
    const fs::path Pkg = PackageDir ? fs::path(PackageDir->path().toStdString()) : fs::path();
    for (const auto & [Id, N] : LibraryIndex().Nodes)
    {
        if (!Pkg.empty() && SameDir(N.BundleDir, Pkg)) continue;
        std::string Label = N.NodeId.empty() ? Id.substr(0, 12) : N.NodeId;
        Out.push_back(Offer{Id, Label, PackageName(N.BundleDir)});
    }
    std::sort(Out.begin(), Out.end(), [](const Offer & A, const Offer & B) {
        return A.Package != B.Package ? A.Package < B.Package : A.Label < B.Label;
    });
    return Out;
}

std::vector<std::string> PackageEditorModel::KnownPlatforms()
{
    const NodeIndex & Idx = ExecIndex();
    std::set<std::string> Seen;
    std::vector<std::string> Out;
    for (const auto & [Id, N] : Idx.Nodes)
    {
        (void)Id;
        if (!N.IsRunner()) continue;
        for (const auto & P : N.GuestPlatform) if (Seen.insert(P).second) Out.push_back(P);
    }
    for (const char * Common : {"win32", "win64", "linux64", "snes", "custom"})
        if (Seen.insert(Common).second) Out.push_back(Common);
    return Out;
}

// ---- validation ------------------------------------------------------------------------------------------------

void PackageEditorModel::Revalidate()
{
    ValResult R = Validate(ExecIndex(), bundleHandles());
    ValErrors = std::move(R.Errors);
    ValWarnings = std::move(R.Warnings);
    Validated = true;
    emit validationChanged();
}

//This package's nodes and everything they contain: the graph around them is only read.
PackageEditorModel::ValResult PackageEditorModel::Validate(const NodeIndex & Idx, const std::vector<std::string> & Handles)
{
    ValResult R;
    std::set<std::string> Scope;
    for (const std::string & H : Handles)
    {
        Scope.insert(H);
        for (const std::string & Dep : ManifestModel::Closure(Idx, H)) Scope.insert(Dep);
    }
    ManifestModel::ValidateNodeGraph(Idx, R.Errors, R.Warnings, &Scope);
    return R;
}

void PackageEditorModel::validateSoon()
{
    if (!ValTimer)
    {
        ValTimer = new QTimer(this);
        ValTimer->setSingleShot(true);
        connect(ValTimer, &QTimer::timeout, this, &PackageEditorModel::validateNow);
    }
    ValTimer->start(450);                                  // restarts: the check waits for a pause in the edits
}

void PackageEditorModel::validateNow()
{
    if (ValTimer) ValTimer->stop();
    if (ValThread) { ValAgain = true; return; }            // one at a time; the newest document is checked next
    if (!PackageDir) return;
    ValAgain = false;
    ValContentRev = Doc.ContentRevision();
    auto Idx = ExecSnapshot();
    auto Out = std::make_shared<ValResult>();
    ValOut = Out;
    ValThread = QThread::create([Idx, Handles = bundleHandles(), Out] { *Out = Validate(*Idx, Handles); });
    connect(ValThread, &QThread::finished, this, &PackageEditorModel::ValidationDone);
    ValThread->start(QThread::LowPriority);
}

void PackageEditorModel::ValidationDone()
{
    ValThread->deleteLater();
    ValThread = nullptr;
    if (ValAgain || ValContentRev != Doc.ContentRevision()) { validateNow(); return; }   // about an older document
    ValErrors = std::move(ValOut->Errors);
    ValWarnings = std::move(ValOut->Warnings);
    ValOut.reset();
    Validated = true;
    emit validationChanged();
}

std::string PackageEditorModel::handleInMessage(const std::string & M) const
{
    //Validation names a node as "node '<id>'" (its handle), sometimes followed by its label in parentheses.
    const size_t A = M.find("node '");
    if (A == std::string::npos) return {};
    const size_t B = M.find('\'', A + 6);
    if (B == std::string::npos) return {};
    const std::string Id = M.substr(A + 6, B - (A + 6));
    return Doc.IndexOf(Id) >= 0 ? Id : std::string();
}

std::map<std::string, std::vector<std::string>> PackageEditorModel::issuesByHandle() const
{
    std::map<std::string, std::vector<std::string>> Out;
    auto Add = [&](const std::string & M) {
        const std::string H = handleInMessage(M);
        if (H.empty()) return;
        std::string Text = M.substr(M.find('\'', M.find("node '") + 6) + 1);
        if (Text.rfind(": ", 0) == 0) Text = Text.substr(2);
        Out[H].push_back(Text);
    };
    for (const auto & M : ValErrors) Add(M);
    for (const auto & M : ValWarnings) Add(M);
    return Out;
}

// ---- authoring ------------------------------------------------------------------------------------------------

bool PackageEditorModel::NodeTestable(const std::string & NodeId) const
{
    return !LaunchableForNode(ExecIndex(), NodeId).empty();
}

void PackageEditorModel::RunInNode(const std::string & NodeId, const std::string & Exe)
{
    //A run is of what is on disk, so the package is saved first (a re-mint: the handle may change).
    std::string Handle = NodeId;
    if (Doc.Dirty())
    {
        QString Err;
        if (!Save(&Err)) { QMessageBox::warning(DialogParent, "Run", "Save the package first:\n" + Err); return; }
        if (const auto It = LastRenames.find(Handle); It != LastRenames.end()) Handle = It->second;
    }
    NodeIndex Idx = BuildExecIndex();
    const std::string Launch = LaunchableForNode(Idx, Handle);
    if (Launch.empty())
    {
        QMessageBox::warning(DialogParent, "Run",
            "This node is not launchable, and no launchable node in the package contains it.\n"
            "Add a node with an EXEC entry that contains this one to test it.");
        return;
    }
    ContainerParams Params(PackageDir->path().toStdString());
    Params.NodeIdx = &Idx;
    Params.LaunchNodeId = Launch;
    json Dummy = json::object();
    ContainerWrapper Container(*GlobalConfigJSON, Dummy, Params);
    if (!LaunchResolver::ResolveExecutableDefinition(Dummy, Container.ContainerParams))
    { QMessageBox::warning(DialogParent, "Run", "Could not resolve the node's entry. Check the log."); return; }
    Container.Cleanup();
    if (!Container.BuildContainerRuntime())
    { QMessageBox::critical(DialogParent, "Run", "Failed to build the runtime. Check the log."); Container.Cleanup(); return; }
    if (!Container.Execute(Exe))
        QMessageBox::warning(DialogParent, "Run", "The process exited with an error. Check the log.");
    Container.Cleanup();
}

std::vector<std::string> PackageEditorModel::bundleNodeIds() const
{
    std::vector<std::string> Out;
    for (const auto & [H, L] : Doc.Labels()) if (!L.empty()) Out.push_back(L);
    return Out;
}

std::string PackageEditorModel::createNode(json Payload, const std::vector<std::string> & Parents, const std::string & Label)
{
    json N = json::object({{"LABEL", Label}});
    json Ls = json::array();
    for (const std::string & X : Parents) if (!X.empty()) Ls.push_back(json{{"NODE", X}});
    for (const auto & [K, V] : Payload.items())
    {
        if (K != "LAYERS") { if (K != "CID") N[K] = V; continue; }
        if (V.is_array()) for (const auto & L : V) Ls.push_back(L);
    }
    N["LAYERS"] = std::move(Ls);
    const int I = Doc.Add(std::move(N));
    Doc.Commit();
    noteEdited();
    emit nodeContentChanged();
    LogSucc("PackageEditorModel", "Created node '" + (Label.empty() ? Doc.Handle(I) : Label) + "' (unsaved).");
    return Doc.Handle(I);
}
