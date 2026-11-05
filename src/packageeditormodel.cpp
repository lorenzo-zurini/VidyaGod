#include "packageeditormodel.h"
#include "nodelower.h"
#include "contentfile.h"   // Bumped / Claims — the names two CIDs give one file

#include <QThread>
#include <QTimer>
#include "apppaths.h"
#include "commonutils.h"
#include "jsonoperations.h"
#include "packagecatalog.h"
#include "pkggraph.h"     // BuildCatalogIndex, LibraryRootDir, EditorLayoutKey
#include "instancestore.h"

#include <QDateTime>
#include <QFile>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

bool SameDir(const fs::path & A, const fs::path & B)
{
    std::error_code Ea, Eb;
    return fs::weakly_canonical(A, Ea) == fs::weakly_canonical(B, Eb);
}

std::string PackageName(const fs::path & Dir) { return PackageCatalog::FolderTitle(Dir); }

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

void PackageEditorModel::initPackage(const QString & path, QWidget *)
{
    //No path: no package - the editor shows its start (New package / Open package), it does not push a picker.
    delete PackageDir;
    PackageDir = path.isEmpty() ? nullptr : new QDir(path);
    LoadNodes();
}

void PackageEditorModel::LoadNodes()
{
    LibraryCache.reset();                                 // the package left may have been saved since: read again
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

static fs::path DraftPath(const QDir * PackageDir)
{
    const std::string K = LayoutKeyFor(PackageDir);
    if (K.empty()) return {};
    std::string Safe = K;
    for (char & C : Safe) if (!std::isalnum((unsigned char)C) && C != '-' && C != '_') C = '_';
    return AppPaths::DataRoot() / "EDITORDRAFTS" / (Safe + ".json");
}

bool PackageEditorModel::writeDraft()
{
    const fs::path P = DraftPath(PackageDir);
    if (P.empty() || !Doc.Dirty()) return false;
    nlohmann::ordered_json J = {{"PACKAGE", PackageDir->absolutePath().toStdString()}, {"WRITTEN", QDateTime::currentMSecsSinceEpoch()},
                                {"NODES", nlohmann::ordered_json::array()}, {"POS", nlohmann::ordered_json::object()}};
    for (int I = 0; I < Doc.Count(); ++I) J["NODES"].push_back({{"HANDLE", Doc.Handle(I)}, {"NODE", Doc.Node(I)}});
    for (const auto & [H, Ps] : Doc.Positions()) J["POS"][H] = {Ps.X, Ps.Y};
    std::error_code Ec;
    fs::create_directories(P.parent_path(), Ec);
    const fs::path Tmp = P.string() + ".tmp";
    {
        std::ofstream O(Tmp, std::ios::binary | std::ios::trunc);
        if (!O) return false;
        O << J.dump();
        if (!O) return false;
    }
    fs::rename(Tmp, P, Ec);
    return !Ec;
}

qint64 PackageEditorModel::draftTime() const
{
    const fs::path P = DraftPath(PackageDir);
    if (P.empty()) return 0;
    std::ifstream In(P, std::ios::binary);
    const auto J = nlohmann::ordered_json::parse(In, nullptr, false);
    return J.is_object() && J.contains("WRITTEN") && J["WRITTEN"].is_number() ? J["WRITTEN"].get<qint64>() : 0;
}

bool PackageEditorModel::restoreDraft(QString * Error)
{
    const fs::path P = DraftPath(PackageDir);
    std::ifstream In(P, std::ios::binary);
    const auto J = nlohmann::ordered_json::parse(In, nullptr, false);
    if (!J.is_object() || !J.contains("NODES") || !J["NODES"].is_array())
    { if (Error) *Error = "The draft could not be read."; return false; }
    std::set<std::string> Kept;
    for (const auto & E : J["NODES"])
    {
        if (!E.is_object() || !E.contains("HANDLE") || !E["HANDLE"].is_string() || !E.contains("NODE")) continue;
        const std::string H = E["HANDLE"].get<std::string>();
        Kept.insert(H);
        const int I = Doc.IndexOf(H);
        if (I < 0) Doc.Add(E["NODE"], H);
        else if (Doc.Node(I) != E["NODE"]) Doc.Replace(I, E["NODE"]);
    }
    for (int I = Doc.Count() - 1; I >= 0; --I) if (!Kept.count(Doc.Handle(I))) Doc.Remove(I);
    if (J.contains("POS") && J["POS"].is_object())
        for (const auto & [H, V] : J["POS"].items())
            if (V.is_array() && V.size() == 2 && V[0].is_number() && V[1].is_number()) Doc.SetPos(H, {V[0].get<float>(), V[1].get<float>()});
    Doc.Commit();
    noteEdited();
    emit documentReloaded();
    return true;
}

void PackageEditorModel::dropDraft()
{
    const fs::path P = DraftPath(PackageDir);
    std::error_code Ec;
    if (!P.empty()) fs::remove(P, Ec);
}

std::string PackageEditorModel::gameId() const
{
    if (!PackageDir) return {};
    const std::string N = QDir(PackageDir->absolutePath()).dirName().toStdString();
    const size_t E = N.find(']');
    return N.empty() || N[0] != '[' || E == std::string::npos ? std::string() : N.substr(1, E - 1);
}

bool PackageEditorModel::changeGameId(const std::string & New, QString * Error)
{
    auto Fail = [&](const QString & W) { if (Error) *Error = W; return false; };
    const std::string Old = gameId();
    if (!PackageDir || Old.empty()) return Fail("This package's folder is not named by a game ID ([<id>] ...).");
    if (New.empty() || !std::all_of(New.begin(), New.end(), [](unsigned char C) { return std::isdigit(C); }))
        return Fail("A game ID is a number (its TheGamesDB ID, or its release date as YYYYMMDD and a serial).");
    if (New == Old) return true;
    const fs::path Dir = QDir::cleanPath(PackageDir->absolutePath()).toStdString();
    const std::string Name = Dir.filename().string();
    const fs::path To = Dir.parent_path() / ("[" + New + "]" + Name.substr(Name.find(']') + 1));
    std::error_code Ec;
    if (fs::exists(To, Ec)) return Fail(QString::fromStdString("There is already a package at " + To.string() + "."));
    for (const auto & E : fs::directory_iterator(Dir.parent_path(), Ec))
        if (E.path().filename().string().rfind("[" + New + "]", 0) == 0)
            return Fail(QString::fromStdString("Another package already has the ID " + New + ": " + E.path().filename().string()));
    //The nodes first, saved where they are: a failed save leaves the folder as it was.
    for (int I = 0; I < Doc.Count(); ++I)
    {
        nlohmann::ordered_json N = Doc.Node(I);
        if (PkgGraph::ChangeUid(N, Old, New)) Doc.Replace(I, std::move(N));
    }
    Doc.Commit();
    noteEdited();
    QString E;
    if (!Save(&E)) return Fail("The package could not be saved:\n\n" + E);
    fs::rename(Dir, To, Ec);
    if (Ec) return Fail(QString::fromStdString("The tiles now say " + New + ", but the folder could not be renamed: " + Ec.message()));
    //This machine's layout follows the folder; so do the saves kept under the old ID, when the new one has none.
    if (GlobalConfigJSON && GlobalConfigJSON->contains("EDITORLAYOUT") && (*GlobalConfigJSON)["EDITORLAYOUT"].is_object())
    {
        auto & L = (*GlobalConfigJSON)["EDITORLAYOUT"];
        const std::string Ko = PackageCatalog::EditorLayoutKey(Dir), Kn = PackageCatalog::EditorLayoutKey(To);
        if (L.contains(Ko)) { L[Kn] = L[Ko]; L.erase(Ko); }
    }
    if (GlobalConfigJSON)
    {
        const fs::path Users = InstanceStore::Root(*GlobalConfigJSON);
        if (fs::exists(Users / Old, Ec) && !fs::exists(Users / New, Ec)) fs::rename(Users / Old, Users / New, Ec);
        WriteConfig();
    }
    initPackage(QString::fromStdString(To.string()), nullptr);
    return true;
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
    WriteConfig();
}

bool PackageEditorModel::tourSeen() const
{
    if (!GlobalConfigJSON) return true;
    const json & C = *GlobalConfigJSON;
    return C.contains("Settings") && C["Settings"].is_object() && C["Settings"].contains("EditorTourSeen")
        && C["Settings"]["EditorTourSeen"].is_boolean() && C["Settings"]["EditorTourSeen"].get<bool>();
}

void PackageEditorModel::markTourSeen()
{
    if (!GlobalConfigJSON) return;
    if (!GlobalConfigJSON->contains("Settings") || !(*GlobalConfigJSON)["Settings"].is_object()) (*GlobalConfigJSON)["Settings"] = json::object();
    (*GlobalConfigJSON)["Settings"]["EditorTourSeen"] = true;
    WriteConfig();
}

void PackageEditorModel::WriteConfig()
{
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

QStringList PackageEditorModel::unusedFiles() const
{
    if (!PackageDir) return {};
    //A file is unused only when NOTHING names it: not the package as edited here, and not its node files as saved (a
    //discarded edit must not leave a saved node naming a deleted file). And a name two CIDs claim lives at each one's
    //BUMPED name (ContentFile): those files are named too, by the name and the SOURCE together.
    std::set<std::string> Named;
    const auto Top = [](const std::string & N) { return N.substr(0, N.find('/')); };   // its top folder or file
    std::function<void(const nlohmann::ordered_json &)> Walk = [&](const nlohmann::ordered_json & J) {
        if (J.is_object())
            for (const auto & [K, V] : J.items())
            {
                if ((K == "ZIP" || K == "FILE" || K == "DELTA" || K == "DIR") && V.is_string() && !V.get<std::string>().empty())
                {
                    Named.insert(Top(V.get<std::string>()));
                    if (J.contains("SOURCE") && J["SOURCE"].is_string() && !J["SOURCE"].get<std::string>().empty())
                        Named.insert(Top(ContentFile::Bumped(V.get<std::string>(), J["SOURCE"].get<std::string>())));
                }
                Walk(V);
            }
        else if (J.is_array()) for (const auto & V : J) Walk(V);
    };
    for (int I = 0; I < Doc.Count(); ++I) Walk(Doc.Node(I));
    for (const QFileInfo & F : PackageDir->entryInfoList({"*.json"}, QDir::Files))
    {
        std::ifstream In(F.filePath().toStdString(), std::ios::binary);
        const nlohmann::ordered_json J = nlohmann::ordered_json::parse(In, nullptr, /*allow_exceptions=*/false);
        if (J.is_object() && J.contains("LAYERS")) Walk(J);
    }
    if (const auto C = ContentFile::Claims(PackageDir->path().toStdString()))
        for (const auto & [Name, Cids] : *C)
        {
            Named.insert(Top(Name));
            for (const std::string & Cid : Cids) Named.insert(Top(ContentFile::Bumped(Name, Cid)));
        }
    QStringList Out;
    for (const QFileInfo & F : PackageDir->entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        const QString N = F.fileName();
        if (N.startsWith('.') || (N.startsWith("baf") && N.endsWith(".json")) || N.endsWith(".tmp") || N.contains(".tmp-")) continue;
        if (!Named.count(N.toStdString())) Out << N;
    }
    return Out;
}

int PackageEditorModel::deleteUnusedFiles()
{
    int N = 0;
    for (const QString & F : unusedFiles())
    {
        const QString P = PackageDir->filePath(F);
        if (QFileInfo(P).isDir() ? QDir(P).removeRecursively() : QFile::remove(P)) ++N;
    }
    return N;
}

bool PackageEditorModel::saveEmptiesPackage() const
{
    if (!PackageDir || Doc.Count() > 0) return false;
    return !PackageDir->entryList({"baf*.json"}, QDir::Files).isEmpty();
}

bool PackageEditorModel::Save(QString * Error)
{
    if (!PackageDir) return false;
    //A received stub (CATALOG) is a friend's bytes, browsable, never ours to rewrite.
    if (GlobalConfigJSON)
    {
        const fs::path Cat = fs::weakly_canonical(PackageCatalog::CatalogRootDir(*GlobalConfigJSON));
        const fs::path Here = fs::weakly_canonical(PackageDir->path().toStdString());
        if (std::mismatch(Cat.begin(), Cat.end(), Here.begin(), Here.end()).first == Cat.end())
        {
            if (Error) *Error = "This package was received from a friend (" + packagePath() + "): it is theirs, and is never rewritten here.";
            return false;
        }
    }
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
    if (R.Cascaded) WriteConfig();
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

const Node * PackageEditorModel::libraryNode(const std::string & Cid) const { return LibraryIndex().Find(Cid); }

PackageEditorModel::External PackageEditorModel::externalInfo(const std::string & Cid) const
{
    const Node * N = LibraryIndex().Find(Cid);
    if (!N) return {};
    std::string Label = N->NodeId;
    if (Label.empty()) Label = N->Meta.is_object() ? N->Meta.value("TITLE", std::string()) : std::string();
    //Its tile's cover, when the node presents one (a game's chip shows the game).
    std::string Cover;
    if (N->Meta.is_object() && N->Meta.contains("COVER") && N->Meta["COVER"].is_object())
    {
        const std::string P = N->Meta["COVER"].value("PATH", std::string());
        std::error_code Ec;
        if (!P.empty() && fs::exists(N->BundleDir / P, Ec)) Cover = (N->BundleDir / P).string();
    }
    return External{Label, PackageName(N->BundleDir), N->BundleDir.string(), Cover};
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

//A file in the package folder no node names (a layer removed since) is left behind by every save; say so.
void PackageEditorModel::noteUnusedFiles()
{
    for (const QString & F : unusedFiles()) ValWarnings.push_back("package file '" + F.toStdString() + "' is used by no node");
}

void PackageEditorModel::Revalidate()
{
    ValResult R = Validate(ExecIndex(), bundleHandles());
    ValErrors = std::move(R.Errors);
    ValWarnings = std::move(R.Warnings);
    noteUnusedFiles();
    Validated = true;
    emit validationChanged();
}

//This package's nodes and everything they contain: the graph around them is only read. Some checks look across the
//whole library (runners disagreeing where %GameDir% is): a message naming a node outside that scope - another
//package's, a runner's - is nothing this author can act on here, and counted as this package's it sent them looking
//for marked nodes that do not exist. A node this package contains stays in scope: its problem is this package's too.
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
    auto Foreign = [&Scope](const std::string & M) {
        const size_t A = M.find("node '");
        if (A == std::string::npos) return false;          // about no node in particular: it stays
        const size_t B = M.find('\'', A + 6);
        return B != std::string::npos && !Scope.count(M.substr(A + 6, B - (A + 6)));
    };
    for (auto * V : {&R.Errors, &R.Warnings}) V->erase(std::remove_if(V->begin(), V->end(), Foreign), V->end());
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
    noteUnusedFiles();
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
        //On the node itself, the node's name says nothing: " (Label): rest" -> "rest".
        if (Text.rfind(" (", 0) == 0)
            if (const size_t E = Text.find("): "); E != std::string::npos) Text = Text.substr(E + 1);
        if (Text.rfind(": ", 0) == 0) Text = Text.substr(2);
        Out[H].push_back(Text);
    };
    for (const auto & M : ValErrors) Add(M);
    for (const auto & M : ValWarnings) Add(M);
    return Out;
}

// ---- authoring ------------------------------------------------------------------------------------------------

std::vector<std::string> PackageEditorModel::bundleNodeIds() const
{
    std::vector<std::string> Out;
    for (const auto & [H, L] : Doc.Labels()) if (!L.empty()) Out.push_back(L);
    return Out;
}

bool PackageEditorModel::fillWaiting(const std::string & Handle, const json & Layer)
{
    const int I = Doc.IndexOf(Handle);
    if (I < 0 || !Layer.is_object()) return false;
    json N = Doc.Node(I);
    if (!N.is_object() || !N.contains("LAYERS") || !N["LAYERS"].is_array()) return false;
    auto Target = [](const json & L) { return L.contains("TARGET") && L["TARGET"].is_string() ? L["TARGET"].get<std::string>() : std::string("FILES"); };
    for (json & L : N["LAYERS"])
        for (const char * T : {"ZIP", "FILE", "DIR", "DELTA"})
            if (L.is_object() && L.contains(T) && L[T].is_string() && L[T].get<std::string>().empty() && Target(L) == Target(Layer))
            {
                json Filled = Layer;
                for (const auto & [K, V] : L.items())             // its COMMENT, WHEN ... stay
                    if (K != T && !Filled.contains(K)) Filled[K] = V;
                L = std::move(Filled);
                Doc.Replace(I, std::move(N));
                Doc.Commit();
                noteEdited();
                emit nodeContentChanged();
                return true;
            }
    return false;
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
