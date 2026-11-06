#include "pkgactions.h"

#include "pkgcanvas.h"
#include "packageeditormodel.h"
#include "manifestmodel.h"
#include "prelaunchwindow.h"
#include "authoringsessionwindow.h"
#include "asyncwork.h"
#include "vgdelta.h"
#include "bytesource.h"
#include "commonutils.h"

#include <QDir>
#include <QImageReader>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <climits>
#include <cstring>
#include <sstream>
#include <iterator>
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <zip.h>
#include <filesystem>
#include <fstream>
#include <functional>

using json = nlohmann::ordered_json;

namespace {

//A type-checked string read. The editor works on RAW on-disk JSON — deliberately, since it is the tool you
//open to fix a node the format rejects — so any field may be any type, and nlohmann's value() throws on a
//mismatch AND on a non-object. These run on every action, with no handler anywhere above them.
std::string StrOf(const json & N, const char * Key)
{
    return (N.is_object() && N.contains(Key) && N[Key].is_string()) ? N[Key].get<std::string>() : std::string();
}

bool HasLayers(const json & N) { return N.is_object() && N.contains("LAYERS") && N["LAYERS"].is_array(); }

using PkgGraph::ContentIndex;
using PkgGraph::ContentType;
using PkgGraph::ContentName;
using PkgGraph::ContentTarget;

//Point the content layer at new bytes: TYPE now names NAME. Where the layer sits, its TARGET and WHEN
//stay; SOURCE and SIZE described the OLD bytes and go (publish stamps the new ones). A node without a content layer
//gets one, appended. False (nothing written) when LAYERS is there but not a list — the JSON view's to fix.
//The index of N's layer supplying the folder Dir (its DIR), -1 when none does.
int DirLayerIndex(const json & N, const std::string & Dir)
{
    if (!HasLayers(N)) return -1;
    for (size_t I = 0; I < N["LAYERS"].size(); ++I)
        if (StrOf(N["LAYERS"][I], "DIR") == Dir) return (int)I;
    return -1;
}

//A folder layer now supplies the zip packed from it: DIR becomes ZIP, the rest of the layer (TARGET, WHEN, ...) kept.
void DirToZip(json & L, const std::string & Zip)
{
    json Out = json::object();
    for (const auto & [K, V] : L.items())
        if (K == "DIR") Out["ZIP"] = Zip;
        else if (K != "SOURCE" && K != "SIZE") Out[K] = V;
    L = std::move(Out);
}

bool SetContent(json & N, const std::string & Type, const std::string & Name)
{
    if (!N.is_object() || (N.contains("LAYERS") && !N["LAYERS"].is_array())) return false;
    if (!N.contains("LAYERS")) N["LAYERS"] = json::array();
    json L = json::object();
    L[Type] = Name;
    const int I = ContentIndex(N);
    if (I < 0) { N["LAYERS"].push_back(std::move(L)); return true; }
    const json & Old = N["LAYERS"][(size_t)I];
    const std::string OldType = PkgGraph::LayerType(Old);
    for (const auto & [K, V] : Old.items())
        if (K != OldType && K != "SOURCE" && K != "SIZE") L[K] = V;
    N["LAYERS"][(size_t)I] = std::move(L);
    return true;
}

//Files ADDED to a node (dropped on it): an empty content layer takes them (a ZIP layer just added, waiting for its
//files), else a new content layer of that type goes last. Never repoints a layer that already names files.
bool AddContent(json & N, const std::string & Type, const std::string & Name)
{
    if (!N.is_object() || (N.contains("LAYERS") && !N["LAYERS"].is_array())) return false;
    if (!N.contains("LAYERS")) N["LAYERS"] = json::array();
    for (json & L : N["LAYERS"])
    {
        const std::string T = PkgGraph::LayerType(L);
        if ((T == "ZIP" || T == "DIR" || T == "FILE" || T == "DELTA") && L[T].is_string() && L[T].get<std::string>().empty())
        {
            json M = json::object();
            for (const auto & [K, V] : L.items()) M[K == T ? Type : K] = K == T ? json(Name) : V;
            L = std::move(M);
            return true;
        }
    }
    json L = PkgGraph::NewLayer(Type);
    L[Type] = Name;
    N["LAYERS"].push_back(std::move(L));
    return true;
}

//The EXEC entry a cover belongs to: the first entry carrying a TILE, else the first entry. Null without one.
//A tile's identity on an entry: its UID ("" for a tile still being filled in, which is its own).
static std::string TileUid(const json & E) { return E["TILE"].is_object() ? StrOf(E["TILE"], "UID") : std::string(); }

//The entry of the node's Tile-th distinct tile (entries sharing a UID are one tile: Quake and its mission packs' rows):
//the first entry carrying it; with none, the first entry.
json * CoverEntry(json & N, int Tile = 0)
{
    if (!HasLayers(N)) return nullptr;
    json * First = nullptr;
    std::set<std::string> Seen;
    for (auto & L : N["LAYERS"])
        if (PkgGraph::LayerType(L) == "EXEC" && L["EXEC"].is_array())
            for (auto & E : L["EXEC"])
            {
                if (!E.is_object()) continue;
                if (E.contains("TILE") && (TileUid(E).empty() || Seen.insert(TileUid(E)).second) && Tile-- == 0) return &E;
                if (!First) First = &E;
            }
    return First;
}

//The titles of the distinct tiles a node's EXEC entries carry, in order (a tile's first entry's LABEL when it has no
//TITLE here).
QStringList TileTitles(const json & N)
{
    QStringList Out;
    if (!HasLayers(N)) return Out;
    std::set<std::string> Seen;
    for (const auto & L : N["LAYERS"])
        if (PkgGraph::LayerType(L) == "EXEC" && L["EXEC"].is_array())
            for (const auto & E : L["EXEC"])
                if (E.is_object() && E.contains("TILE") && (TileUid(E).empty() || Seen.insert(TileUid(E)).second))
                {
                    const std::string T = E["TILE"].is_object() ? StrOf(E["TILE"], "TITLE") : std::string();
                    Out << QString::fromStdString(T.empty() ? StrOf(E, "LABEL") : T);
                }
    return Out;
}

//Every variable KEY the node's VARS layers declare.
std::vector<std::string> VarKeysOf(const json & N)
{
    std::vector<std::string> Keys;
    if (!HasLayers(N)) return Keys;
    for (const auto & L : N["LAYERS"])
        if (PkgGraph::LayerType(L) == "VARS" && L["VARS"].is_object())
            for (const auto & [K, V] : L["VARS"].items()) Keys.push_back(K);
    return Keys;
}


//A node's PATH resolved inside its bundle — or EMPTY if it does not stay inside it.
//
//The containment check has to live HERE, where the path is used to DELETE things, not only in the file
//picker. Two ways a legal-looking PATH escaped it:
//  * "."  — the directory picker OPENS in the bundle, so clicking OK on the bundle folder itself yields
//           relativeFilePath(Bundle) == ".", which does not start with ".." and passed the picker's guard.
//           `-> zip` then packed the bundle into itself and remove_all()'d the whole thing, reporting success.
//  * "../sibling" — the PATH box is free text (FieldKind::Text, no validation), so it can be typed directly,
//           and the deletion landed outside the bundle entirely.
//weakly_canonical resolves ".." lexically without requiring the path to exist, so a not-yet-created target
//is still checked.
std::filesystem::path ResolveInBundle(const std::filesystem::path & Bundle, const std::string & Rel)
{
    if (Rel.empty()) return {};
    //SEPARATE error codes, and BOTH are fatal. Sharing one let the second call's success clear the first's
    //failure: if canonicalising the BUNDLE failed, Root came back empty and the prefix test below passed
    //vacuously — the guard accepted anything, including absolute paths outside it. Fail CLOSED.
    std::error_code RootEc, AbsEc;
    std::filesystem::path Root = std::filesystem::weakly_canonical(Bundle, RootEc);
    std::filesystem::path Abs  = std::filesystem::weakly_canonical(Bundle / Rel, AbsEc);
    if (RootEc || AbsEc || Root.empty() || Abs.empty()) return {};

    //Normalise away a trailing separator on BOTH. "nosuch/.." canonicalises to "<bundle>/" — with a trailing
    //slash, which path comparison treats as an extra empty element, so `Abs == Root` missed it and the
    //bundle root itself slipped through as a "layer". Whether it did depended on whether the first segment
    //happened to exist, which is not a property a deletion guard may rest on.
    Root = Root.lexically_normal(); Abs = Abs.lexically_normal();
    if (Root.filename().empty()) Root = Root.parent_path();
    if (Abs.filename().empty())  Abs  = Abs.parent_path();
    if (Abs == Root) return {};                             // the bundle itself is never a layer

    for (auto I = Root.begin(), A = Abs.begin(); I != Root.end(); ++I, ++A)
        if (A == Abs.end() || *A != *I) return {};          // escapes the bundle
    return Abs;
}

} // namespace

PkgActions::PkgActions(PackageEditorModel * model, PkgCanvas * canvas, QWidget * dialogParent, QObject * parent)
    : QObject(parent), Model(model), Canvas(canvas), Parent(dialogParent)
{
    //Follow nodes through saves: an action that finishes after a save must find its node under its new name.
    if (Model) connect(Model, &PackageEditorModel::handlesRenamed, this, [this] {
        for (const auto & [Old, New] : Model->lastRenames()) Moves[Old] = New;
    });
}

PkgActions::~PkgActions() { Alive_->store(false); }   // in-flight workers must stop touching us

//Post a progress update to the GUI thread from a WORKER thread, safely.
//
//The obvious `QMetaObject::invokeMethod(this, ...)` has a race that no amount of checking can close from here:
//`this` is a QObject owned by the GUI thread, invokeMethod dereferences it to read its thread affinity, and
//~PkgActions can run between the liveness check and that dereference. The callback fires every few MiB for
//minutes on a multi-GB archive, so "narrow" is not the same as "won't happen".
//
//Post to qApp instead — it outlives every PkgActions — and re-check liveness INSIDE the lambda, which by then
//is running on the GUI thread, where the destructor cannot be concurrent with it.
//A FREE function, taking the canvas: a member would read this->Canvas ON THE WORKER THREAD, which is the same
//use-after-free window the qApp post exists to close, merely moved from invokeMethod's target to a member read.
//The worker captures the canvas pointer once, up front, alongside the liveness flag.
//
//Posted to qApp, never to a PkgActions: invokeMethod dereferences its target to read the thread affinity, and
//~PkgActions can run between a liveness check here and that dereference. qApp outlives every editor, and the
//re-check INSIDE the lambda happens on the GUI thread, where the destructor cannot be concurrent with it.
static void PostProgress(PkgCanvas *C, const std::shared_ptr<std::atomic<bool>> & Alive,
                         const std::string & NodeId, float Frac, const char * What)
{
    const QString Label = QString::fromLatin1(What);
    QMetaObject::invokeMethod(qApp, [C, Alive, NodeId, Frac, Label]() {
        if (!Alive->load()) return;                 // destroyed while this was queued — drop it
        C->setProgress(NodeId, Frac, Label);
    }, Qt::QueuedConnection);
}

void PkgActions::tell(const QString & Title, const QString & Body)
{
    if (Notify) { Notify(Title, Body); return; }
    //No parent widget means there is no window to raise a modal over — a headless run. Log instead of
    //blocking forever, so the DEFAULT path (the one that actually ships) is exercisable by a test.
    if (!Parent) { LogWarn("PkgActions", Title.toStdString() + ": " + Body.toStdString()); return; }
    QMessageBox::warning(Parent, Title, Body);
}

QString PkgActions::pick(const QString & Title, const QString & Dir, const QString & Filter, bool WantDir)
{
    if (Pick) return Pick(Title, Dir, Filter, WantDir);
    return WantDir ? QFileDialog::getExistingDirectory(Parent, Title, Dir)
                   : QFileDialog::getOpenFileName(Parent, Title, Dir, Filter);
}

int PkgActions::choose(const QString & Title, const QString & Text, const QStringList & Options, bool AsList)
{
    if (Choose) return Choose(Title, Text, Options);
    if (!Parent) { LogWarn("PkgActions", "No way to ask '" + Title.toStdString() + "' — nothing chosen."); return -1; }
    if (AsList)
    {
        bool Ok = false;
        const QString C = QInputDialog::getItem(Parent, Title, Text, Options, 0, false, &Ok);
        return Ok ? (int)Options.indexOf(C) : -1;
    }
    QMessageBox Box(Parent);
    Box.setWindowTitle(Title);
    const qsizetype Br = Text.indexOf("\n\n");          // the question, then what the options mean
    Box.setText(Br < 0 ? Text : Text.left(Br));
    if (Br >= 0) Box.setInformativeText(Text.mid(Br + 2));
    QList<QAbstractButton *> Btns;
    for (const QString & O : Options) Btns << Box.addButton(O, QMessageBox::AcceptRole);
    Box.addButton(QMessageBox::Cancel);
    Box.exec();
    return (int)Btns.indexOf(Box.clickedButton());
}

bool PkgActions::ask(const QString & Title, const QString & Body)
{
    if (Confirm) return Confirm(Title, Body);
    //Nobody to ask ⇒ REFUSE. These actions delete files; proceeding unasked is the one outcome that can
    //destroy content, so the safe default is to do nothing.
    if (!Parent) { LogWarn("PkgActions", "No way to confirm '" + Title.toStdString() + "' — refusing."); return false; }
    return QMessageBox::question(Parent, Title, Body, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
           == QMessageBox::Yes;
}

int PkgActions::indexOf(const std::string & nodeId) const
{
    return Model->doc().IndexOf(current(nodeId));
}

//A node's handle NOW: a save renames every node it re-mints, and an action started before it finishes after it.
std::string PkgActions::current(std::string Handle) const
{
    for (int Guard = 0; Guard < 64; ++Guard)
    {
        //A node of that name now is that node: a save that renamed it away can be followed by another node minted to
        //the same CID (two empty "a mod for it" nodes are byte-identical), and the old rename must not steal it.
        if (Model && Model->doc().IndexOf(Handle) >= 0) break;
        const auto It = Moves.find(Handle);
        if (It == Moves.end()) break;
        Handle = It->second;
    }
    return Handle;
}

//One edit to one node: an undo step, the canvas and the JSON panel follow. Persist: the edit describes files this
//action just changed on disk, so the package is saved with it (the node must never name a file that is gone).
void PkgActions::commitNode(int I, json N, bool Persist)
{
    if (I < 0 || I >= Model->doc().Count()) return;
    Model->doc().Replace(I, std::move(N));
    Model->doc().Commit();
    Model->notifyNodeChanged();
    if (Persist)
    {
        QString Err;
        if (!Model->Save(&Err)) tell("Save", "The files were converted, but the package could not be saved:\n\n" + Err);
    }
}

void PkgActions::perform(const QString & nodeIdQ, const QString & actionQ)
{
    const std::string NodeId = nodeIdQ.toStdString(), A = actionQ.toStdString();
    if (indexOf(NodeId) < 0) return;
    if (Canvas->isBusy(NodeId)) return;                   // one heavy action per node at a time

    if      (A == "browse")        browsePath(NodeId);
    else if (A == "bring_in")      bringIn(NodeId);
    else if (A.rfind("pick_exe:", 0) == 0) pickExe(NodeId, A.substr(9));
    else if (A.rfind("compare:", 0) == 0) compareWith(NodeId, A.substr(8));
    else if (A == "browse_cover")  browseCover(NodeId);
    else if (A == "to_zip")        convertZipDir(NodeId, /*ToZip=*/true);
    else if (A.rfind("to_zip:", 0) == 0) convertZipDir(NodeId, /*ToZip=*/true, A.substr(7));
    else if (A == "to_dir")        convertZipDir(NodeId, /*ToZip=*/false);
    else if (A == "restore")       reStore(NodeId);
    else if (A == "test_launch")   testLaunch(NodeId);
    else if (A == "capture_setup") openCapture(NodeId, 0);
    else if (A == "browse_files")  openCapture(NodeId, 1);
    else if (A == "capture_reg")   openCapture(NodeId, 2);
    else if (A == "find_usages")   findUsages(NodeId);
    else if (A == "import_reg")    importReg(NodeId);
    else if (A == "to_delta")      makeDelta(NodeId);
    else if (A == "undelta")       undelta(NodeId);
    else LogWarn("PkgActions", "No handler for action '" + A + "' on node '" + NodeId + "'.");
}

void PkgActions::cancel(const QString & nodeIdQ)
{
    //Keyed by the handle the action STARTED under: a save since may have renamed the node.
    const std::string NodeId = nodeIdQ.toStdString();
    for (auto & [K, P] : Procs) if (P && current(K) == NodeId) P->kill();
    for (auto & [K, A] : Aborts) if (A && current(K) == NodeId) A->store(true);
}

bool PkgActions::saveFirst(const QString & Title)
{
    if (!Model->isDirty()) return true;
    QString Err;
    if (Model->Save(&Err)) return true;
    tell(Title, "The package has to be saved first, and it could not be:\n\n" + Err);
    return false;
}

// ---------------------------------------------------------------------------
// Progress-reporting process runner
// ---------------------------------------------------------------------------

void PkgActions::runWithProgress(const std::string & NodeId, const QString & What, const QString & Program,
                                 const QStringList & Args, const QString & WorkDir, int Total,
                                 std::function<void(bool)> Done)
{
    QProcess * P = new QProcess(this);
    if (!WorkDir.isEmpty()) P->setWorkingDirectory(WorkDir);
    P->setProcessChannelMode(QProcess::MergedChannels);
    Procs[NodeId] = P;
    Canvas->beginAction(NodeId, What, /*cancellable=*/true);

    // zip/unzip print one line per entry, so the line count IS the progress — a real measure rather than a
    // timer pretending to be one. Both bits of state are shared_ptr, because a crash emits errorOccurred AND
    // THEN finished: the second entry must be able to read the "already done" flag, not a freed pointer.
    auto Seen     = std::make_shared<int>(0);
    auto Finished = std::make_shared<bool>(false);
    QObject::connect(P, &QProcess::readyRead, this, [this, P, NodeId, Seen, Total, Finished]() {
        if (*Finished) return;                            // output can arrive after completion
        const QByteArray Chunk = P->readAll();
        *Seen += Chunk.count('\n');
        if (Total > 0) Canvas->setProgress(current(NodeId), qMin(1.0f, (float)*Seen / (float)Total),
                                           QString("%1 / %2").arg(*Seen).arg(Total));
        else           Canvas->setProgress(current(NodeId), -1.0f, QString("%1 files").arg(*Seen));
    });
    // A failure to even START (missing zip/unzip, not executable) emits errorOccurred and NEVER finished. The
    // caller's completion handler is where rollback lives, so skipping it left the node locked to a file that
    // had already been deleted, with no message at all. Route both paths through one place.
    auto Complete = [this, P, NodeId, Seen, Finished, Done](bool Ok) {
        if (*Finished) return;
        *Finished = true;
        Procs.erase(NodeId);
        Canvas->endAction(current(NodeId));                        // paired with beginAction on EVERY path out
        P->deleteLater();
        if (Done) Done(Ok);
    };
    QObject::connect(P, &QProcess::finished, this,
                     [Complete](int Code, QProcess::ExitStatus St) {
        Complete(St == QProcess::NormalExit && Code == 0);
    });
    QObject::connect(P, &QProcess::errorOccurred, this, [this, NodeId, Complete](QProcess::ProcessError E) {
        LogErr("PkgActions", "'" + NodeId + "': the conversion tool could not be run (QProcess error "
                                 + std::to_string((int)E) + ") - is zip/unzip installed?");
        Complete(false);
    });
    P->start(Program, Args);
}

// ---------------------------------------------------------------------------
// Content: browse / zip ↔ dir / re-store
// ---------------------------------------------------------------------------

void PkgActions::browsePath(const std::string & NodeId)
{
    //Resolved BEFORE and AGAIN AFTER the modal. A json& into doc()["NODES"] held across a file dialog is a
    //dangling reference waiting to happen: a capture session finishing during it appends a node, the vector
    //reallocates, and the write below lands in freed memory. Re-look-up is the cheap, total fix.
    int I = indexOf(NodeId);
    if (I < 0) return;
    const QString Bundle = Model->packageDir() ? Model->packageDir()->path() : QDir::homePath();
    const std::string Type = [&]{ const std::string T = ContentType(Model->doc().Node(I)); return T.empty() ? std::string("ZIP") : T; }();

    const QString Picked = (Type == "DIR")
        ? pick("Pick the folder this layer supplies", Bundle, QString(), true)
        : pick("Pick the file this layer supplies", Bundle, QString());
    if (Picked.isEmpty()) return;

    // A layer's file name is relative to its bundle — content lives WITH the package. If the pick is from elsewhere,
    // say so rather than silently writing an absolute path that only resolves on this machine.
    const QString Rel = QDir(Bundle).relativeFilePath(Picked);
    if (Rel.startsWith(".."))
    {
        tell("Outside the bundle",
            "“" + QFileInfo(Picked).fileName() + "” is outside this package.\n\n"
            "A layer's file is named relative to the bundle, so it has to live here. Copy it into\n"
            + Bundle + "\nand pick it again.");
        return;
    }
    I = indexOf(NodeId);                          // re-resolved after the modal — see the note above
    if (I < 0) { tell("Browse", "That node is no longer in the package."); return; }
    json N = Model->doc().Node(I);
    if (!SetContent(N, Type, Rel.toStdString()))
    { tell("Browse", "This node's LAYERS is not a list - fix it in the JSON view first."); return; }
    commitNode(I, std::move(N), false);
}

//The way game files enter a package: from wherever they are. A folder (an installed or extracted game) is packed into
//an uncompressed zip in the package - the folder itself is left alone, it is the user's; a zip is copied in (one already
//in the package is just named). Either way the layer then supplies it.
static bool NamedByAny(const PkgDoc::Document & D, const std::string & Name);

void PkgActions::bringIn(const std::string & NodeId)
{
    if (indexOf(NodeId) < 0 || !Model->packageDir()) return;
    //A content layer waiting for its files (just added, still empty) takes them without a question; a node whose
    //files are named already is never repointed unasked.
    std::string Waiting, Named, NamedType;
    if (const int I = indexOf(NodeId); I >= 0)
    {
        const json & N = Model->doc().Node(I);
        for (const json & L : HasLayers(N) ? N["LAYERS"] : json::array())
        {
            const std::string T = PkgGraph::LayerType(L);
            if (T != "ZIP" && T != "DIR" && T != "FILE" && T != "DELTA") continue;
            const std::string V = L[T].is_string() ? L[T].get<std::string>() : std::string();
            if (V.empty()) { if (Waiting.empty()) Waiting = T; }
            else if (Named.empty()) { Named = V; NamedType = T; }
        }
    }
    auto AddOrReplace = [&](bool & Add) {
        if (!Waiting.empty() || Named.empty()) { Add = !Waiting.empty(); return true; }
        const int A = choose("Bring in files", "This node already has files (" + QString::fromStdString(Named)
                             + ").\n\nAdd these as a new layer over them, or replace them?", {"Add as a new layer", "Replace its files"}, false);
        if (A < 0) return false;
        Add = A == 0;
        return true;
    };
    //A FILE layer supplies one file: that is what is picked, not a folder or a zip.
    if (Waiting == "FILE" || (Waiting.empty() && NamedType == "FILE"))
    {
        const QString Picked = pick("Pick the file", QDir::homePath(), QString());
        if (Picked.isEmpty()) return;
        bool Add = false;
        if (!AddOrReplace(Add)) return;
        bringInFile(NodeId, Picked, Add);
        return;
    }
    const int Which = choose("Bring in files", "Where are the game's files?\n\n"
                             "A folder - an installed or extracted game - is packed into an uncompressed zip in this package.\n"
                             "A zip is copied in as it is.", {"A folder...", "A zip..."}, false);
    if (Which < 0) return;
    const bool Folder = Which == 0;
    const QString Picked = Folder ? pick("Pick the game's folder", QDir::homePath(), QString(), true)
                                  : pick("Pick the zip", QDir::homePath(), "Zip archives (*.zip)");
    if (Picked.isEmpty()) return;
    bool Add = false;
    if (!AddOrReplace(Add)) return;
    bringInPath(NodeId, Picked, Folder, Add);
}

void PkgActions::bringInFile(const std::string & NodeId, const QString & Picked, bool Add)
{
    if (indexOf(NodeId) < 0 || !Model->packageDir()) return;
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const QString Rel = QDir(QString::fromStdString(Bundle.string())).relativeFilePath(Picked);
    std::string Name = Rel.toStdString();
    if (Rel.startsWith(".."))
    {
        //Copied in under its own name; a name a node already uses is never overwritten - the next free one is taken.
        const std::filesystem::path Src = Picked.toStdString();
        Name = Src.filename().string();
        std::error_code Ec;
        for (int K = 2; std::filesystem::exists(Bundle / Name, Ec); ++K)
        {
            if (!NamedByAny(Model->doc(), Name)) { std::filesystem::remove(Bundle / Name, Ec); break; }
            Name = Src.stem().string() + "-" + std::to_string(K) + Src.extension().string();
        }
        if (!std::filesystem::copy_file(Src, Bundle / Name, Ec) || Ec)
        { tell("Bring in files", "Could not copy \"" + QString::fromStdString(Src.filename().string()) + "\" into the package."); return; }
    }
    const int I = indexOf(NodeId);
    if (I < 0) return;
    json N = Model->doc().Node(I);
    if (!(Add ? AddContent(N, "FILE", Name) : SetContent(N, "FILE", Name))) return;
    commitNode(I, std::move(N), false);
    refreshHints();
}

//A GOG install's goggame-<id><Ext> at the top of Dir, else in its app\ (newer installers keep it there: Unreal
//Tournament 2004); "" when it is not one.
static std::filesystem::path GogFile(const std::filesystem::path & Dir, const std::string & Ext)
{
    std::error_code Ec;
    for (const std::filesystem::path & D : {Dir, Dir / "app"})
        for (const auto & E : std::filesystem::directory_iterator(D, Ec))
        {
            const std::string N = E.path().filename().string();
            if (N.rfind("goggame-", 0) == 0 && E.path().extension() == Ext && E.is_regular_file(Ec)) return E.path();
        }
    return {};
}

//Whether a node of the package names this file of it (a content layer's ZIP/FILE/DIR/DELTA).
static bool NamedByAny(const PkgDoc::Document & D, const std::string & Name)
{
    for (int I = 0; I < D.Count(); ++I)
    {
        const json & N = D.Node(I);
        if (!HasLayers(N)) continue;
        for (const json & L : N["LAYERS"])
            for (const char * T : {"ZIP", "FILE", "DIR", "DELTA"})
                if (L.is_object() && L.contains(T) && L[T].is_string() && L[T].get<std::string>() == Name) return true;
    }
    return false;
}

void PkgActions::bringInPath(const std::string & NodeId, const QString & Picked, bool Folder, bool Add)
{
    if (indexOf(NodeId) < 0 || !Model->packageDir()) return;
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    auto Place = [Add](json & N, const std::string & Name) { return Add ? AddContent(N, "ZIP", Name) : SetContent(N, "ZIP", Name); };
    std::error_code Ec;
    const std::filesystem::path Src = Picked.toStdString();
    //A zip already in the package needs no copy.
    const QString Rel = QDir(QString::fromStdString(Bundle.string())).relativeFilePath(Picked);
    if (!Folder && !Rel.startsWith(".."))
    {
        const int I = indexOf(NodeId);
        if (I < 0) return;
        json N = Model->doc().Node(I);
        Place(N, Rel.toStdString());
        commitNode(I, std::move(N), false);
        refreshHints();
        autoDetect(NodeId);
        return;
    }
    //A folder holding the package would be packed into itself.
    if (Folder && !QDir(Picked).relativeFilePath(QString::fromStdString(Bundle.string())).startsWith(".."))
    {
        tell("Bring in files", "That folder holds this package. Pick the game's own folder.");
        return;
    }
    //Named after what was picked: a folder's own name (a picker may return it with a trailing slash), a zip's file name.
    const QString Base = Folder ? QDir(QDir::cleanPath(Picked)).dirName() : QFileInfo(Picked).fileName();
    if (Base.isEmpty() || Base == "." || Base == "..") { tell("Bring in files", "Pick a named folder or zip."); return; }
    std::string Name = (Folder ? Base + ".zip" : Base).toStdString();
    //An extracted GOG installer is named for its game, not the folder it was unpacked to ("x", "game"): GOG's own
    //name for it, "(GOG)" after.
    const std::filesystem::path GogInfo = Folder ? GogFile(Src, ".info") : std::filesystem::path();
    if (!GogInfo.empty())
    {
        std::ifstream In(GogInfo, std::ios::binary);
        const json Info = json::parse(In, nullptr, false);
        std::string Game = Info.is_object() ? StrOf(Info, "name") : std::string();
        for (const char * Mark : {"\u2122", "\u00ae"})                      // ™ ® - a trademark sign is no part of a file name
            for (size_t At; (At = Game.find(Mark)) != std::string::npos;) Game.erase(At, std::strlen(Mark));
        Game.erase(std::remove_if(Game.begin(), Game.end(), [](char C) { return std::strchr("<>:\"/\\|?*", C) != nullptr; }), Game.end());
        while (!Game.empty() && Game.back() == ' ') Game.pop_back();
        if (!Game.empty()) Name = Game + " (GOG).zip";
    }
    if (std::filesystem::exists(Bundle / Name, Ec))
    {
        if (NamedByAny(Model->doc(), Name))
        {
            //A node's file keeps its name: these files take the next free one.
            const std::filesystem::path P(Name);
            for (int K = 2; std::filesystem::exists(Bundle / Name, Ec); ++K)
                Name = P.stem().string() + "-" + std::to_string(K) + P.extension().string();
        }
        else
        {
            //No node's: left by a bring-in that was never saved (a discarded draft) - used as it is, or replaced.
            const int C = choose("Bring in files", "This package already has \"" + QString::fromStdString(Name) + "\", which no node uses.\n\n"
                                 "It is likely from an earlier bring-in that was never saved. Use it as it is, or replace it with what you picked.",
                                 {"Use it", "Replace it"}, false);
            if (C < 0) return;
            if (C == 0)
            {
                const int I = indexOf(NodeId);
                if (I < 0) return;
                json N = Model->doc().Node(I);
                Place(N, Name);
                commitNode(I, std::move(N), false);
                refreshHints();
                autoDetect(NodeId);
                return;
            }
            std::filesystem::remove(Bundle / Name, Ec);
            if (Ec) { tell("Bring in files", "Could not replace \"" + QString::fromStdString(Name) + "\"."); return; }
        }
    }
    const std::filesystem::path Dst = Bundle / Name, Tmp = Bundle / (Name + ".bringing.tmp");
    std::filesystem::remove(Tmp, Ec);
    auto Finish = [this, NodeId, Name, Dst, Tmp, Place, Folder, Src](bool Ok) {
        std::error_code Rc;
        if (!Ok || !std::filesystem::exists(Tmp, Rc) || std::filesystem::file_size(Tmp, Rc) == 0)
        { std::filesystem::remove(Tmp, Rc); tell("Bring in files", "It did not complete - nothing was added."); return; }
        std::filesystem::rename(Tmp, Dst, Rc);
        if (Rc) { std::filesystem::remove(Tmp, Rc); tell("Bring in files", "Could not place it in the package."); return; }
        const int J = indexOf(NodeId);
        if (J < 0) return;
        json Nj = Model->doc().Node(J);
        Place(Nj, Name);
        commitNode(J, std::move(Nj), false);
        refreshHints();
        autoDetect(NodeId);
        if (Folder) gogSteps(NodeId, Src);
    };
    if (Folder)
    {
        int Total = 0;
        for (auto It = std::filesystem::recursive_directory_iterator(Src, Ec); !Ec && It != std::filesystem::recursive_directory_iterator(); ++It)
            if (It->is_regular_file(Ec)) ++Total;
        //-0 = STORE (vidyagodfs reads entries in place and cannot inflate); -X drops extra attributes. An extracted GOG
        //installer's own folders (its redistributables, support files, slideshow, uninstaller) are not the game.
        QStringList Args{"-0", "-r", "-X", QString::fromStdString(Tmp.string()), "."};
        //What GOG's installer puts in the game folder itself is packed in at the top, over the rest, in its order: app/
        //(GOG Galaxy's icon and web cache aside), then __support/app/ (the program and configs its script sets up, as
        //Icewind Dale II's IWD2.exe and icewind2.ini) - neither kept as a folder of its own.
        //Newer installers put everything there, __support\ included (Unreal Tournament 2004).
        std::vector<std::pair<std::filesystem::path, QString>> Tops;
        bool TopHasFiles = true;
        if (!GogInfo.empty())
        {
            static const std::set<std::string> Installer = {"__redist", "__support", "tmp", "commonappdata", "app"};
            Args << "-x" << "__redist/*" << "__support/*" << "tmp/*" << "commonappdata/*";
            if (std::filesystem::is_directory(Src / "app", Ec)) { Args << "app/*"; Tops.emplace_back(Src / "app", "packing GOG's app files..."); }
            for (const std::filesystem::path & B : {Src, Src / "app"})
                if (std::filesystem::is_directory(B / "__support" / "app", Ec)) Tops.emplace_back(B / "__support" / "app", "packing GOG's support files...");
            //Everything in the installer's own folders: the first pass would pack nothing (zip fails "nothing to do").
            TopHasFiles = false;
            for (const auto & E : std::filesystem::directory_iterator(Src, Ec))
                if (!(E.is_directory(Ec) && Installer.count(E.path().filename().string()))) { TopHasFiles = true; break; }
        }
        //Each top folder after the one before it, the last one finishing the bring-in.
        auto Chain = std::make_shared<std::function<void(size_t, bool)>>();
        *Chain = [this, NodeId, Tmp, Tops, Finish, Chain](size_t I, bool Ok) {
            if (!Ok || I >= Tops.size()) { Finish(Ok); *Chain = nullptr; return; }
            std::error_code Rc;
            int N = 0;
            for (auto It = std::filesystem::recursive_directory_iterator(Tops[I].first, Rc); !Rc && It != std::filesystem::recursive_directory_iterator(); ++It)
            {
                if (It.depth() == 0 && It->is_directory(Rc) && It->path().filename() == "__support") { It.disable_recursion_pending(); continue; }
                const std::string Leaf = It->path().filename().string();
                if (It->is_regular_file(Rc) && Leaf != "webcache.zip" && !(Leaf.rfind("goggame-", 0) == 0 && It->path().extension() == ".ico")) ++N;
            }
            //Only GOG Galaxy's icon and web cache there: nothing to pack (zip would fail with "nothing to do").
            if (N == 0) { (*Chain)(I + 1, true); return; }
            runWithProgress(NodeId, Tops[I].second, ZipTool,
                            {"-0", "-r", "-X", QString::fromStdString(Tmp.string()), ".", "-x", "webcache.zip", "goggame-*.ico", "__support/*"},
                            QString::fromStdString(Tops[I].first.string()), N, [Chain, I](bool Ok2) { (*Chain)(I + 1, Ok2); });
        };
        if (!TopHasFiles) { (*Chain)(0, true); return; }
        runWithProgress(NodeId, "packing zip...", ZipTool, Args, QString::fromStdString(Src.string()), Total,
                        [Chain](bool Ok) { (*Chain)(0, Ok); });
        return;
    }
    //A compressed zip (most downloads) cannot mount - vidyagodfs reads entries in place: it comes in re-packed STORE,
    //the picked file left as it is.
    if (!ManifestModel::ZipFullyStored(Src.string()))
    {
        const std::filesystem::path Unpacked = Bundle / (Name + ".unpacking.tmp");
        std::filesystem::remove_all(Unpacked, Ec);
        std::filesystem::create_directories(Unpacked, Ec);
        runWithProgress(NodeId, "unpacking...", UnzipTool, {"-o", QString::fromStdString(Src.string()), "-d", QString::fromStdString(Unpacked.string())},
                        QString(), 0, [this, NodeId, Unpacked, Tmp, Finish](bool Ok) {
            std::error_code Rc;
            if (!Ok) { std::filesystem::remove_all(Unpacked, Rc); Finish(false); return; }
            int Total = 0;
            for (auto It = std::filesystem::recursive_directory_iterator(Unpacked, Rc); !Rc && It != std::filesystem::recursive_directory_iterator(); ++It)
                if (It->is_regular_file(Rc)) ++Total;
            runWithProgress(NodeId, "re-packing STORE...", ZipTool, {"-0", "-r", "-X", QString::fromStdString(Tmp.string()), "."},
                            QString::fromStdString(Unpacked.string()), Total, [Unpacked, Finish](bool Ok2) {
                std::error_code R2;
                std::filesystem::remove_all(Unpacked, R2);
                Finish(Ok2);
            });
        });
        return;
    }
    Canvas->beginAction(NodeId, "copying zip...", /*cancellable=*/false);
    auto Ok = std::make_shared<bool>(false);
    AsyncWork::Run(this, [Src, Tmp, Ok] { std::error_code C; *Ok = std::filesystem::copy_file(Src, Tmp, C) && !C; },
                   [this, NodeId, Ok, Finish] { Canvas->endAction(current(NodeId)); Finish(*Ok); });
}

//The programs a node's files hold, written as the anchored paths an EXEC entry names: each ZIP or DIR layer's entries
//under its TARGET ("FILES/%GameDir%" + "Game/run.exe" -> "%GameDir%/Game/run.exe"), and those of this package's nodes it
//contains - a variant names its program from the data node beneath it. Where each one lives, to read it: Zip + Entry,
//or File. A GOG install's goggame-*.info (its primary play task) is collected the same way, as Info.
struct Program { std::string Anchored; std::filesystem::path Zip; std::string Entry; std::filesystem::path File; };
static void ProgramsIn(const PkgDoc::Document & D, int I, const std::filesystem::path & Bundle, std::set<int> & Seen,
                       std::vector<Program> & Out, std::vector<Program> * Info = nullptr)
{
    if (I < 0 || !Seen.insert(I).second) return;
    const json & N = D.Node(I);
    if (!HasLayers(N)) return;
    auto Lower = [](std::string Name) { for (char & C : Name) C = (char)std::tolower((unsigned char)C); return Name; };
    auto IsProgram = [&](const std::string & Name) {
        const std::string L = Lower(Name);
        for (const char * E : {".exe", ".bat", ".cmd", ".com", ".sh", ".x86", ".x86_64", ".appimage"})
        { const size_t Ln = std::strlen(E); if (L.size() > Ln && L.compare(L.size() - Ln, Ln, E) == 0) return true; }
        return false;
    };
    auto IsInfo = [&](const std::string & Name) {
        const std::string B = Lower(Name.substr(Name.find_last_of('/') + 1));
        if (B.rfind("goggame-", 0) == 0 && B.size() > 5 && B.compare(B.size() - 5, 5, ".info") == 0) return true;
        //A Unity game's <Game>_Data/app.info: its company and product, which name where it saves.
        const std::string L = Lower(Name);
        return B == "app.info" && L.size() > 14 && L.compare(L.size() - 14, 14, "_data/app.info") == 0;
    };
    for (const json & L : N["LAYERS"])
    {
        if (!L.is_object()) continue;
        std::string Anchor = StrOf(L, "TARGET");
        if (Anchor.rfind("FILES/", 0) == 0) Anchor = Anchor.substr(6);
        if (Anchor.empty()) Anchor = "%GameDir%";
        if (L.contains("ZIP") && L["ZIP"].is_string() && !L["ZIP"].get<std::string>().empty())
        {
            const std::filesystem::path Zp = Bundle / L["ZIP"].get<std::string>();
            int Err = 0;
            if (zip_t * Za = zip_open(Zp.string().c_str(), ZIP_RDONLY, &Err))
            {
                for (zip_int64_t E = 0, Num = zip_get_num_entries(Za, 0); E < Num; ++E)
                    if (const char * Nm = zip_get_name(Za, (zip_uint64_t)E, 0))
                    {
                        if (IsProgram(Nm)) Out.push_back({Anchor + "/" + Nm, Zp, Nm, {}});
                        else if (Info && IsInfo(Nm)) Info->push_back({Anchor + "/" + Nm, Zp, Nm, {}});
                    }
                zip_close(Za);
            }
        }
        else if (L.contains("DIR") && L["DIR"].is_string() && !L["DIR"].get<std::string>().empty())
        {
            const std::filesystem::path Dir = Bundle / L["DIR"].get<std::string>();
            std::error_code Ec;
            for (auto It = std::filesystem::recursive_directory_iterator(Dir, Ec); !Ec && It != std::filesystem::recursive_directory_iterator(); ++It)
            {
                if (!It->is_regular_file(Ec)) continue;
                const std::string Rel = std::filesystem::relative(It->path(), Dir, Ec).generic_string();
                if (IsProgram(It->path().filename().string())) Out.push_back({Anchor + "/" + Rel, {}, {}, It->path()});
                else if (Info && IsInfo(Rel)) Info->push_back({Anchor + "/" + Rel, {}, {}, It->path()});
            }
        }
        else if (L.contains("NODE") && L["NODE"].is_string())
            ProgramsIn(D, D.IndexOf(L["NODE"].get<std::string>()), Bundle, Seen, Out, Info);
    }
}

//Up to Max bytes of a program's start (or all of a small file): enough to tell what it runs on.
//GOG's placeholders -> anchors; "" when the placeholder means nothing here. In a value or a file's text a Windows
//path is wanted (:winpath); in an address, the plain anchor.
static std::string GogPath(std::string P, bool Win)
{
    static const std::vector<std::pair<std::string, std::string>> Places = {
        {"{app}", "GameDir"}, {"{userdocs}", "Documents"}, {"{userappdata}", "AppData"},
        {"{localappdata}", "LocalAppData"}, {"{commonappdata}", "ProgramData"}, {"{userprofile}", "UserProfile"}};
    //Inno writes a literal "{" as "{{" (a GUID: "{{99797420-...}"): kept aside while the placeholders are read.
    static const std::string Brace = "\x01";
    for (size_t At; (At = P.find("{{")) != std::string::npos;) P.replace(At, 2, Brace);
    for (const auto & [Ph, Anchor] : Places)
        for (size_t At; (At = P.find(Ph)) != std::string::npos;)
            P.replace(At, Ph.size(), "%" + Anchor + (Win ? ":winpath" : "") + "%");
    if (P.find('{') != std::string::npos) return {};
    for (size_t At; (At = P.find(Brace)) != std::string::npos;) P.replace(At, 1, "{");
    if (!Win) std::replace(P.begin(), P.end(), '\\', '/');
    return P;
}

PkgActions::GogSteps PkgActions::GogInstallSteps(const json & Script, const std::filesystem::path & SupportDir, const std::string & Arch)
{
    GogSteps Out;
    if (!Script.is_object() || !Script.contains("actions") || !Script["actions"].is_array()) return Out;
    json Reg = json::object(), Keep = json::object();
    int RegValues = 0;
    std::vector<std::pair<std::string, json>> Inis;       // an ini file's address -> the ConfigWrites GOG sets in it
    for (const json & A : Script["actions"])
    {
        if (!A.is_object() || !A.contains("install") || !A["install"].is_object()) continue;
        //For every language, or English: the package is the English install.
        bool Lang = !A.contains("languages");
        if (A.contains("languages") && A["languages"].is_array())
            for (const json & L : A["languages"]) if (L == "*" || L == "en-US") Lang = true;
        if (!Lang) continue;
        const json & I = A["install"];
        const std::string Act = StrOf(I, "action"), Name = StrOf(A, "name");
        const json Arg = I.contains("arguments") && I["arguments"].is_object() ? I["arguments"] : json::object();
        auto Skip = [&](const std::string & Why) { Out.Skipped.push_back((Name.empty() ? Act : Name) + " - " + Why); };
        if (Act == "saveToFile")
        {
            const std::string At = GogPath(StrOf(Arg, "filename"), false), Text = GogPath(StrOf(Arg, "text"), true);
            if (At.empty() || StrOf(Arg, "text").empty()) { Skip("a file outside the places a package knows"); continue; }
            Out.Layers.push_back(json{{"COMMENT", "GOG's installer writes this file" + (Name.empty() ? std::string() : " (" + Name + ")") + "."},
                                      {"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", Text}} })}, {"TARGET", "FILES/" + At}});
            Out.Said.push_back("writes " + At.substr(At.find_last_of('/') + 1));
        }
        else if (Act == "setRegistry")
        {
            std::string Root = StrOf(Arg, "root"), Sub = StrOf(Arg, "subkey");
            if (Root == "HKEY_LOCAL_MACHINE") Root = "HKLM"; else if (Root == "HKEY_CURRENT_USER") Root = "HKCU";
            if (Sub.find("GOG.com") != std::string::npos) { Skip("GOG Galaxy's own record of the install"); continue; }
            if ((Root != "HKLM" && Root != "HKCU") || Sub.empty()) { Skip("a registry root a package does not write"); continue; }
            std::string Val = StrOf(Arg, "valueData");
            std::string Type = StrOf(Arg, "valueType");
            for (char & C : Type) C = (char)std::tolower((unsigned char)C);
            if (Type == "dword")
            {
                //Inno's "$" is hexadecimal.
                unsigned long N = 0;
                try { N = Val.rfind('$', 0) == 0 ? std::stoul(Val.substr(1), nullptr, 16) : std::stoul(Val); }
                catch (const std::exception &) { Skip("a DWORD that is no number (" + Val + ")"); continue; }
                char B[16]; std::snprintf(B, sizeof B, "dword:%08lx", N); Val = B;
            }
            else if (Type == "string") { Val = GogPath(Val, true); if (Val.empty() && !StrOf(Arg, "valueData").empty()) { Skip("a value naming a place a package does not know"); continue; } }
            else if (Type == "binary")
            {
                //GOG writes the bytes as base64 (CD keys); Wine's .reg spells them "hex:e3,00,...".
                const QByteArray Bytes = QByteArray::fromBase64(QByteArray::fromStdString(Val), QByteArray::AbortOnBase64DecodingErrors);
                if (Bytes.isEmpty() && !Val.empty()) { Skip("binary data that is not base64"); continue; }
                Val = "hex:";
                for (int i = 0; i < Bytes.size(); ++i)
                {
                    char B[4]; std::snprintf(B, sizeof B, i ? ",%02x" : "%02x", (unsigned char)Bytes[i]); Val += B;
                }
            }
            //A step with no value only clears the key before the install writes it: a fresh prefix has nothing to clear.
            else if (Type.empty() && !Arg.contains("valueName")) continue;
            else { Skip("a registry type it does not translate (" + Type + ")"); continue; }
            json * K = &Reg[Root];
            std::string Part;
            std::istringstream Ss(Sub);
            while (std::getline(Ss, Part, '\\')) if (!Part.empty()) K = &(*K)[Part];
            const std::string Vn = StrOf(Arg, "valueName");
            (*K)[Vn.empty() ? "@" : Vn] = Val;
            ++RegValues;
        }
        else if (Act == "supportData")
        {
            const std::string Src = StrOf(Arg, "source"), Dst = GogPath(StrOf(Arg, "target"), false);
            std::string T = StrOf(Arg, "type");
            for (char & C : T) C = (char)std::tolower((unsigned char)C);
            if (T != "file" || Src.rfind("{supportDir}", 0) != 0 || Dst.empty()) { Skip("not a file it can place"); continue; }
            const std::filesystem::path From = SupportDir / Src.substr(13);
            std::ifstream In(From, std::ios::binary);
            const std::string Body((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
            if (!In.good() && Body.empty()) { Skip("its file " + Src + " is not beside the game"); continue; }
            const std::string Leaf = Dst.substr(Dst.find_last_of('/') + 1), Dir = Dst.substr(0, Dst.find_last_of('/'));
            if (Body.find('\0') == std::string::npos)
                Out.Layers.push_back(json{{"COMMENT", "GOG's installer copies this default in (" + Leaf + ")."},
                                          {"EDIT", json::array({ json{{"MODE", "Overwrite"}, {"VALUE", Body}} })}, {"TARGET", "FILES/" + Dst}});
            else
            {
                Out.Copies.push_back({From, Leaf});
                Out.Layers.push_back(json{{"COMMENT", "GOG's installer copies this file in."}, {"FILE", Leaf}, {"TARGET", "FILES/" + Dir}});
            }
            Out.Said.push_back("puts " + Leaf + " in " + Dir);
        }
        else if (Act == "savePath")
        {
            std::string P = GogPath(StrOf(Arg, "savePath"), false);
            if (P.empty()) { Skip("a save folder outside the places a package knows"); continue; }
            if (P.back() != '/') P += '/';
            Keep["FILES/" + P] = true;
            Out.Said.push_back("saves in " + P);
        }
        else if (Act == "execute" || Act == "Execute") Skip("runs " + StrOf(Arg, "executable") + " - a package runs no installer");
        else if (Act == "installSDB") Skip("a Windows compatibility database - Wine applies none");
        else if (Act == "setIni")
        {
            //A line of an ini file: KEY= by its prefix (a ConfigWrite holds no section), the value's places as anchors.
            const std::string At = GogPath(StrOf(Arg, "filename"), false), Key = StrOf(Arg, "keyName"), Raw = StrOf(Arg, "keyValue");
            const std::string Val = GogPath(Raw, true);
            if (At.empty() || Key.empty()) { Skip("an ini file outside the places a package knows"); continue; }
            if (Val.empty() && !Raw.empty()) { Skip("a value naming a place a package does not know"); continue; }
            //Another machine's drive (a CD's "G:\") means nothing in a package, and the validator refuses it.
            static const std::regex Drive(R"((^|[^A-Za-z0-9])[A-Za-z]:[\\/])");
            if (std::regex_search(Val, Drive)) { Skip("a value naming a drive (" + Raw + ")"); continue; }
            auto It = std::find_if(Inis.begin(), Inis.end(), [&](const auto & F) { return F.first == At; });
            if (It == Inis.end()) It = Inis.insert(Inis.end(), {At, json::array()});
            It->second.push_back(json{{"MODE", "ConfigWrite"}, {"KEY", Key + "="}, {"VALUE", Val}});
        }
        else Skip("\"" + Act + "\" is not translated");
    }
    for (auto & [At, Ops] : Inis)
    {
        const std::string Leaf = At.substr(At.find_last_of('/') + 1);
        Out.Said.push_back("sets " + std::to_string(Ops.size()) + " line" + (Ops.size() == 1 ? "" : "s") + " of " + Leaf);
        Out.Layers.push_back(json{{"COMMENT", "What GOG's installer sets in " + Leaf + "."}, {"EDIT", std::move(Ops)}, {"TARGET", "FILES/" + At}});
    }
    if (RegValues)
    {
        //Before the seeds and the saves: a REG layer is facts, read first.
        //ARCH is the reading program's bitness; unknown, it is left for the author (the validator asks for it).
        json L = json{{"COMMENT", "The registry values GOG's installer writes."}, {"REG", Reg}};
        if (!Arch.empty()) L["ARCH"] = json::array({Arch});
        Out.Layers.insert(Out.Layers.begin(), std::move(L));
        Out.Said.insert(Out.Said.begin(), "sets " + std::to_string(RegValues) + " registry value" + (RegValues == 1 ? "" : "s"));
    }
    if (!Keep.empty()) Out.Layers.push_back(json{{"COMMENT", "Saves and settings, where GOG's installer says the game keeps them."}, {"KEEP", Keep}});
    return Out;
}

//A Unity game's app.info ("<company>\n<product>") -> the KEEP layer for where it saves: its LocalLow folder and its
//PlayerPrefs key. Null when the text is not that.
json PkgActions::UnityKeep(const std::string & AppInfo)
{
    std::istringstream In(AppInfo);
    std::string Company, Product;
    std::getline(In, Company); std::getline(In, Product);
    for (std::string * S : {&Company, &Product})
        while (!S->empty() && (S->back() == '\r' || S->back() == ' ')) S->pop_back();
    if (Company.empty() || Product.empty() || Company.find_first_of("/\\%") != std::string::npos
        || Product.find_first_of("/\\%") != std::string::npos)
        return nullptr;
    return json{{"COMMENT", "The game's saves and settings: a Unity game keeps them under its company and product (" + Company + ", " + Product + ")."},
                {"KEEP", json{{"FILES/%UserProfile%/AppData/LocalLow/" + Company + "/" + Product + "/", true},
                              {"REG/HKCU/Software/" + Company + "/" + Product, true}}}};
}

static std::string ReadHead(const Program & P, size_t Max)
{
    std::string Out;
    if (!P.File.empty())
    {
        std::ifstream In(P.File, std::ios::binary);
        Out.resize(Max);
        In.read(Out.data(), (std::streamsize)Max);
        Out.resize((size_t)In.gcount());
        return Out;
    }
    int Err = 0;
    zip_t * Za = zip_open(P.Zip.string().c_str(), ZIP_RDONLY, &Err);
    if (!Za) return Out;
    if (zip_file_t * F = zip_fopen(Za, P.Entry.c_str(), 0))
    {
        Out.resize(Max);
        const zip_int64_t Got = zip_fread(F, Out.data(), Max);
        Out.resize(Got > 0 ? (size_t)Got : 0);
        zip_fclose(F);
    }
    zip_close(Za);
    return Out;
}

std::string PkgActions::PlatformOf(const std::string & Name, const std::string & Head)
{
    std::string L = Name;
    for (char & C : L) C = (char)std::tolower((unsigned char)C);
    auto Ends = [&](const char * E) { const size_t N = std::strlen(E); return L.size() >= N && L.compare(L.size() - N, N, E) == 0; };
    if (Head.size() >= 20 && Head.compare(0, 4, "\x7f" "ELF") == 0) return Head[4] == 2 ? "linux64" : "linux32";
    if (Ends(".sh") || Ends(".x86_64") || Ends(".appimage")) return "linux64";
    if (Head.size() >= 64 && Head.compare(0, 2, "MZ") == 0)
    {
        //The PE header's offset is at 0x3C; past it, "PE\0\0" and the machine (0x14c i386, 0x8664 x64).
        const uint32_t Off = (uint8_t)Head[0x3C] | ((uint8_t)Head[0x3D] << 8) | ((uint8_t)Head[0x3E] << 16) | ((uint32_t)(uint8_t)Head[0x3F] << 24);
        if ((size_t)Off + 6 <= Head.size() && Head.compare(Off, 4, std::string("PE\0\0", 4)) == 0)
        {
            const uint16_t Machine = (uint8_t)Head[Off + 4] | ((uint8_t)Head[Off + 5] << 8);
            return Machine == 0x8664 ? "win64" : "win32";
        }
        if ((size_t)Off + 2 <= Head.size() && (Head.compare(Off, 2, "NE") == 0 || Head.compare(Off, 2, "LE") == 0)) return "win32";
        return "dos";                                     // MZ alone: a DOS program
    }
    if (Ends(".com") || Ends(".bat")) return "dos";
    return {};
}

int PkgActions::LikelyProgram(const std::vector<std::string> & Anchored)
{
    //Not the game: installers, uninstallers, crash reporters, redistributables and their setup tools.
    static const char * Not[] = {"unins", "setup", "install", "crash", "report", "vcredist", "dxsetup", "dotnet", "redist",
                                 "directx", "config", "patch", "update", "dosbox", "settings", "dos4gw"};
    std::vector<int> Keep;
    int Shallowest = INT_MAX;
    for (int I = 0; I < (int)Anchored.size(); ++I)
    {
        std::string B = Anchored[(size_t)I].substr(Anchored[(size_t)I].find_last_of('/') + 1);
        for (char & C : B) C = (char)std::tolower((unsigned char)C);
        bool Skip = false;
        for (const char * W : Not) if (B.find(W) != std::string::npos) Skip = true;
        if (Skip) continue;
        Keep.push_back(I);
    }
    //A batch file or .com beside real programs launches one of them (GTA's GTA.BAT runs its DOS build): the programs
    //are the candidates; scripts only when there is nothing else.
    auto Script = [&](int I) {
        std::string B = Anchored[(size_t)I];
        for (char & C : B) C = (char)std::tolower((unsigned char)C);
        return B.size() > 4 && (B.compare(B.size() - 4, 4, ".bat") == 0 || B.compare(B.size() - 4, 4, ".cmd") == 0 || B.compare(B.size() - 4, 4, ".com") == 0);
    };
    if (std::any_of(Keep.begin(), Keep.end(), [&](int I) { return !Script(I); }))
        Keep.erase(std::remove_if(Keep.begin(), Keep.end(), Script), Keep.end());
    for (const int I : Keep)
        Shallowest = std::min(Shallowest, (int)std::count(Anchored[(size_t)I].begin(), Anchored[(size_t)I].end(), '/'));
    //One left, or one alone at the top of the game's folder: that one. Several there: the author picks.
    int Found = -1;
    for (const int I : Keep)
        if ((int)std::count(Anchored[(size_t)I].begin(), Anchored[(size_t)I].end(), '/') == Shallowest)
        { if (Found >= 0) return -1; Found = I; }
    return Found;
}

std::vector<PkgActions::SeenFile> PkgActions::GameSees(const PkgDoc::Document & D, const std::string & Handle,
                                                       const std::filesystem::path & Bundle, std::vector<std::string> * Notes,
                                                       const OtherFn & Other)
{
    const auto Labels = D.Labels();
    std::map<std::string, OtherNode> Others;             // asked once each
    auto Outside = [&](const std::string & H) -> const OtherNode & {
        auto It = Others.find(H);
        if (It == Others.end()) It = Others.emplace(H, Other ? Other(H) : OtherNode{}).first;
        return It->second;
    };
    auto Label = [&](const std::string & H) {
        const auto It = Labels.find(H);
        return It != Labels.end() ? It->second : D.IndexOf(H) < 0 ? Outside(H).Label : std::string();
    };
    auto Find = [&](const std::string & H) -> const json * { const int I = D.IndexOf(H); return I >= 0 ? &D.Node(I) : Outside(H).Node; };
    std::map<std::string, SeenFile> Seen;                 // by path, case-folded: a Windows game sees one file per name
    auto Add = [&](const std::string & Path, const std::string & From) {
        std::string Key = Path;
        for (char & C : Key) C = (char)std::tolower((unsigned char)C);
        auto It = Seen.find(Key);
        if (It == Seen.end()) { Seen[Key] = SeenFile{Path, From, {}}; return; }
        It->second.Under.push_back(It->second.From);       // later wins: what was there is now beneath
        It->second.From = From;
        It->second.Path = Path;
    };
    for (const PkgGraph::FoldStep & St : PkgGraph::FoldOrder(Handle, Find, Label))
    {
        if (St.Outside) { if (Notes) Notes->push_back("+ the files of \"" + St.Title + "\" (another package)"); continue; }
        const json * N = Find(St.Node);
        if (!N || St.Layer < 0 || St.Layer >= (int)(*N)["LAYERS"].size()) continue;
        const std::filesystem::path Folder = D.IndexOf(St.Node) >= 0 ? Bundle : Outside(St.Node).Bundle;   // where its files are
        const json & L = (*N)["LAYERS"][(size_t)St.Layer];
        std::string Anchor = StrOf(L, "TARGET");
        if (Anchor.rfind("FILES/", 0) == 0) Anchor = Anchor.substr(6);
        if (Anchor.empty()) Anchor = "%GameDir%";
        while (!Anchor.empty() && Anchor.back() == '/') Anchor.pop_back();
        const std::string Name = StrOf(L, St.Type.c_str());
        const std::string From = (St.NodeLabel.empty() ? St.Node.substr(0, 12) : St.NodeLabel) + " > " + (Name.empty() ? St.Type : Name);
        if (Name.empty()) continue;
        if (St.Type == "ZIP")
        {
            int Err = 0;
            zip_t * Za = zip_open((Folder / Name).string().c_str(), ZIP_RDONLY, &Err);
            if (!Za) { if (Notes) Notes->push_back(Name + " is not in the package folder"); continue; }
            for (zip_int64_t E = 0, Num = zip_get_num_entries(Za, 0); E < Num; ++E)
                if (const char * Nm = zip_get_name(Za, (zip_uint64_t)E, 0); Nm && *Nm && Nm[std::strlen(Nm) - 1] != '/') Add(Anchor + "/" + Nm, From);
            zip_close(Za);
        }
        else if (St.Type == "DIR")
        {
            std::error_code Ec;
            const std::filesystem::path Dir = Folder / Name;
            for (auto It = std::filesystem::recursive_directory_iterator(Dir, Ec); !Ec && It != std::filesystem::recursive_directory_iterator(); ++It)
                if (It->is_regular_file(Ec)) Add(Anchor + "/" + std::filesystem::relative(It->path(), Dir, Ec).generic_string(), From);
        }
        else if (St.Type == "FILE") Add(Anchor + "/" + std::filesystem::path(Name).filename().generic_string(), From);
        else if (St.Type == "DELTA" && Notes) Notes->push_back("+ what the patch " + Name + " rebuilds");
    }
    std::vector<SeenFile> Out;
    for (auto & [K, F] : Seen) { (void)K; Out.push_back(std::move(F)); }
    std::sort(Out.begin(), Out.end(), [](const SeenFile & A, const SeenFile & B) { return A.Path < B.Path; });
    return Out;
}

PkgActions::VersionDiff PkgActions::Compare(const PkgDoc::Document & D, const std::string & A, const std::string & B,
                                            const std::filesystem::path & Bundle)
{
    VersionDiff R;
    const auto Labels = D.Labels();
    auto Label = [&](const std::string & H) { const auto It = Labels.find(H); return It == Labels.end() ? std::string() : It->second; };
    auto Find = [&](const std::string & H) -> const json * { const int I = D.IndexOf(H); return I < 0 ? nullptr : &D.Node(I); };
    //Layers as a multiset of "TYPE name": what one folds that the other does not.
    std::map<std::string, int> La, Lb;
    for (const auto & S : PkgGraph::FoldOrder(A, Find, Label)) ++La[S.Type + "  " + S.Title];
    for (const auto & S : PkgGraph::FoldOrder(B, Find, Label)) ++Lb[S.Type + "  " + S.Title];
    for (const auto & [K, N] : La) for (int I = Lb.count(K) ? Lb[K] : 0; I < N; ++I) R.LayersOnlyA.push_back(K);
    for (const auto & [K, N] : Lb) for (int I = La.count(K) ? La[K] : 0; I < N; ++I) R.LayersOnlyB.push_back(K);
    std::map<std::string, SeenFile> Fa, Fb;
    auto Key = [](std::string P) { for (char & C : P) C = (char)std::tolower((unsigned char)C); return P; };
    for (auto & F : GameSees(D, A, Bundle)) Fa[Key(F.Path)] = F;
    for (auto & F : GameSees(D, B, Bundle)) Fb[Key(F.Path)] = F;
    for (const auto & [K, F] : Fa)
    {
        const auto It = Fb.find(K);
        if (It == Fb.end()) R.FilesOnlyA.push_back(F.Path);
        else if (It->second.From != F.From) R.FilesDiffer.push_back(F.Path + "  (" + F.From + "  vs  " + It->second.From + ")");
    }
    for (const auto & [K, F] : Fb) if (!Fa.count(K)) R.FilesOnlyB.push_back(F.Path);
    return R;
}

void PkgActions::compareWith(const std::string & NodeId, const std::string & Other)
{
    const int I = indexOf(NodeId), J = indexOf(Other);
    if (I < 0 || J < 0 || !Model->packageDir()) return;
    const PkgDoc::Document & D = Model->doc();
    const VersionDiff R = Compare(D, D.Handle(I), D.Handle(J), Model->packageDir()->path().toStdString());
    const auto Labels = D.Labels();
    const QString Na = QString::fromStdString(Labels.at(D.Handle(I))).toHtmlEscaped(), Nb = QString::fromStdString(Labels.at(D.Handle(J))).toHtmlEscaped();
    auto Section = [](const QString & Title, const std::vector<std::string> & Items) {
        if (Items.empty()) return QString();
        QString H = "<h4>" + Title + " (" + QString::number(Items.size()) + ")</h4><ul>";
        for (size_t K = 0; K < Items.size() && K < 200; ++K) H += "<li>" + QString::fromStdString(Items[K]).toHtmlEscaped() + "</li>";
        if (Items.size() > 200) H += "<li>...</li>";
        return H + "</ul>";
    };
    QString Html = Section("Layers only in " + Na, R.LayersOnlyA) + Section("Layers only in " + Nb, R.LayersOnlyB)
                 + Section("Files only in " + Na, R.FilesOnlyA) + Section("Files only in " + Nb, R.FilesOnlyB)
                 + Section("Files both give, from different layers", R.FilesDiffer);
    if (Html.isEmpty()) Html = "<p>They fold the same layers and give the game the same files.</p>";
    const QString Title = "Compare " + Na + " with " + Nb;
    if (Notify || !Parent) { tell(Title, Html); return; }
    QMessageBox Box(QMessageBox::Information, Title, Html, QMessageBox::Close, Parent);
    Box.setTextFormat(Qt::RichText);
    Box.exec();
}

void PkgActions::autoDetect(const std::string & NodeId)
{
    if (indexOf(NodeId) < 0 || !Model->packageDir()) return;
    const PkgDoc::Document & D = Model->doc();
    //The game entry that still has no program (its Exe empty, or the "%GameDir%/" starter), in the node the files
    //came into or a version over it: another node's entries are not this file's to fill - the tiles at a package's
    //base have no program by design, and a runner brought in beside the game is not the game.
    const int Into = indexOf(NodeId);
    auto Over = [&D, Into](int I) {
        std::set<int> Seen;
        std::vector<int> Todo{I};
        while (!Todo.empty())
        {
            const int K = Todo.back();
            Todo.pop_back();
            if (K == Into) return true;
            if (K < 0 || !Seen.insert(K).second || !HasLayers(D.Node(K))) continue;
            for (const json & L : D.Node(K)["LAYERS"])
                if (L.is_object() && L.contains("NODE") && L["NODE"].is_string()) Todo.push_back(D.IndexOf(L["NODE"].get<std::string>()));
        }
        return false;
    };
    std::vector<int> Order{Into};
    for (int I = 0; I < D.Count(); ++I) if (I != Into && Over(I)) Order.push_back(I);
    int Ni = -1, Li = -1, Ei = -1;
    for (const int I : Order)
    {
        if (Ni >= 0) break;
        const json & N = D.Node(I);
        if (!HasLayers(N)) continue;
        for (int L = 0; L < (int)N["LAYERS"].size() && Ni < 0; ++L)
        {
            const json & Ly = N["LAYERS"][(size_t)L];
            if (!Ly.is_object() || !Ly.contains("EXEC") || !Ly["EXEC"].is_array()) continue;
            for (int E = 0; E < (int)Ly["EXEC"].size(); ++E)
            {
                const json & En = Ly["EXEC"][(size_t)E];
                if (!En.is_object() || En.contains("GUEST")) continue;
                const std::string Exe = StrOf(En, "EXE");
                if (Exe.empty() || Exe.back() == '/') { Ni = I; Li = L; Ei = E; break; }
            }
        }
    }
    if (Ni < 0) return;
    std::vector<Program> Progs, Infos;
    std::set<int> Seen;
    ProgramsIn(D, Ni, Model->packageDir()->path().toStdString(), Seen, Progs, &Infos);
    if (Progs.empty()) return;
    std::string Exe, Args, WorkDir, Why;
    //A GOG install says which program is the game: its primary play task.
    for (const Program & I : Infos)
    {
        const json G = json::parse(ReadHead(I, 1 << 20), nullptr, false);
        if (!G.is_object() || !G.contains("playTasks") || !G["playTasks"].is_array()) continue;
        for (const json & T : G["playTasks"])
        {
            if (!T.is_object() || !(T.value("isPrimary", false) || StrOf(T, "category") == "game") || StrOf(T, "path").empty()) continue;
            const std::string Root = I.Anchored.substr(0, I.Anchored.find_last_of('/'));
            std::string Path = StrOf(T, "path");
            std::replace(Path.begin(), Path.end(), '\\', '/');
            Exe = Root + "/" + Path;
            Args = StrOf(T, "arguments");
            std::string Wd = StrOf(T, "workingDir");
            std::replace(Wd.begin(), Wd.end(), '\\', '/');
            WorkDir = Wd.empty() ? std::string() : Root + "/" + Wd;
            Why = "its GOG play task";
            break;
        }
        if (!Exe.empty()) break;
    }
    const Program * Chosen = nullptr;
    if (!Exe.empty())
    {
        //The play task's path in the file's own case (GOG's TUNIC.exe is Tunic.exe on disk: the mount is
        //case-sensitive); a task naming no program here is no guide.
        auto Lower = [](std::string S) { for (char & C : S) C = (char)std::tolower((unsigned char)C); return S; };
        for (const Program & P : Progs) if (Lower(P.Anchored) == Lower(Exe)) Chosen = &P;
        if (!Chosen) Exe.clear(), Args.clear(), WorkDir.clear();
        else
        {
            const std::string Dir = Chosen->Anchored.substr(0, Chosen->Anchored.find_last_of('/'));
            if (Lower(WorkDir) == Lower(Dir)) WorkDir = Dir;
            Exe = Chosen->Anchored;
        }
    }
    if (Exe.empty())
    {
        std::vector<std::string> Names;
        for (const Program & P : Progs) Names.push_back(P.Anchored);
        const int K = LikelyProgram(Names);
        if (K < 0)
        {
            emit status(QString("%1 programs in the files - choose the game's: Exe > pick").arg(Progs.size()));
            return;
        }
        Chosen = &Progs[(size_t)K];
        Exe = Chosen->Anchored;
        Why = "the only likely program";
    }
    json N = D.Node(Ni);
    json & En = N["LAYERS"][(size_t)Li]["EXEC"][(size_t)Ei];
    En["EXE"] = Exe;
    if (StrOf(En, "WORKDIR").empty()) En["WORKDIR"] = WorkDir.empty() ? Exe.substr(0, Exe.find_last_of('/')) : WorkDir;
    if (!Args.empty() && !En.contains("ARGS"))
    {
        json A = json::array();
        std::istringstream Ss(Args);
        for (std::string W; Ss >> W;) A.push_back(W);
        En["ARGS"] = A;
    }
    std::string Host = StrOf(En, "HOST");
    if (Chosen && (Host.empty()))
    {
        Host = PlatformOf(Chosen->Anchored, ReadHead(*Chosen, 4096));
        if (!Host.empty()) En["HOST"] = Host;
    }
    //A Unity game saves (and keeps its settings) where its app.info says: offered as a KEEP layer, once.
    std::string Saves;
    if (Chosen)
    {
        auto Lower = [](std::string S) { for (char & C : S) C = (char)std::tolower((unsigned char)C); return S; };
        const std::string Stem = Exe.substr(0, Exe.find_last_of('.'));
        const json Keep = UnityKeep([&]() -> std::string {
            for (const Program & I : Infos) if (Lower(I.Anchored) == Lower(Stem + "_Data/app.info")) return ReadHead(I, 4096);
            return {};
        }());
        bool Kept = false;
        for (const json & L : N["LAYERS"]) if (L.is_object() && L.contains("KEEP")) Kept = true;
        if (!Keep.is_null() && !Kept) { N["LAYERS"].push_back(Keep); Saves = ", its saves kept (a Unity game)"; }
    }
    commitNode(Ni, std::move(N), false);
    emit status(QString::fromStdString("Program set to " + Exe.substr(Exe.find_last_of('/') + 1) + " (" + Why + ")"
                                       + (Host.empty() ? std::string() : ", runs on " + Host) + Saves));
}

void PkgActions::gogSteps(const std::string & NodeId, const std::filesystem::path & Folder)
{
    const std::filesystem::path Script = GogFile(Folder, ".script");
    const int I = indexOf(NodeId);
    if (Script.empty() || I < 0 || !Model->packageDir()) return;
    std::ifstream In(Script, std::ios::binary);
    const json S = json::parse(In, nullptr, false);
    //The registry's bitness is the game's: its entry's platform, now that the program is known.
    std::string Arch;
    for (int K = 0; K < Model->doc().Count() && Arch.empty(); ++K)
    {
        const json & N = Model->doc().Node(K);
        if (!HasLayers(N)) continue;
        for (const json & L : N["LAYERS"])
            if (L.is_object() && L.contains("EXEC") && L["EXEC"].is_array())
                for (const json & E : L["EXEC"])
                {
                    const std::string H = StrOf(E, "HOST");
                    if (H == "win32") Arch = "32"; else if (H == "win64") Arch = "64";
                }
    }
    const GogSteps G = GogInstallSteps(S, Script.parent_path() / "__support", Arch);   // beside the script (top or app\)
    if (G.Layers.empty()) return;
    QString Text = "GOG's installer also does this when it installs the game:\n\n";
    for (const std::string & L : G.Said) Text += QString::fromStdString("- " + L + "\n");
    if (!G.Skipped.empty())
    {
        Text += "\nLeft out (it cannot apply here):\n";
        for (const std::string & L : G.Skipped) Text += QString::fromStdString("- " + L + "\n");
    }
    if (choose("GOG's install steps", "Add GOG's install steps to this node?\n\n" + Text, {"Add them", "Not now"}, false) != 0) return;
    //A binary file it copies in ships in the package; another file of that name already there is never replaced.
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    for (const auto & [From, Name] : G.Copies)
    {
        std::error_code Ec;
        const std::filesystem::path To = Bundle / Name;
        if (std::filesystem::exists(To, Ec))
        {
            std::ifstream A(From, std::ios::binary), B(To, std::ios::binary);
            const std::string Sa((std::istreambuf_iterator<char>(A)), {}), Sb((std::istreambuf_iterator<char>(B)), {});
            if (Sa != Sb) { tell("GOG's install steps", "This package already has another \"" + QString::fromStdString(Name) + "\" - nothing was added."); return; }
        }
        else if (!std::filesystem::copy_file(From, To, Ec) || Ec)
        { tell("GOG's install steps", "Could not copy \"" + QString::fromStdString(Name) + "\" into the package - nothing was added."); return; }
    }
    const int J = indexOf(NodeId);
    if (J < 0) return;
    json N = Model->doc().Node(J);
    for (const json & L : G.Layers) N["LAYERS"].push_back(L);
    commitNode(J, std::move(N), false);
    emit status(QString("GOG's install steps added: %1 layer(s)").arg(G.Layers.size()));
}

void PkgActions::dropFiles(const QString & NodeIdQ, const QStringList & Paths)
{
    const std::string NodeId = NodeIdQ.toStdString();
    if (indexOf(NodeId) < 0 || Paths.isEmpty()) return;
    bool Heavy = false;
    QStringList Refused;
    for (const QString & P : Paths)
    {
        const QFileInfo F(P);
        const QString Ext = F.suffix().toLower();
        if (F.isDir() || Ext == "zip")
        {
            //One folder or zip at a time per node: each is a long copy with its own progress bar.
            if (Heavy || Canvas->isBusy(NodeId)) { Refused << F.fileName() + " (one folder or zip at a time)"; continue; }
            Heavy = true;
            bringInPath(NodeId, P, F.isDir(), true);
        }
        else if (Ext == "png" || Ext == "jpg" || Ext == "jpeg" || Ext == "webp") coverFrom(NodeId, P);
        else if (Ext == "reg") importRegFile(NodeId, P);
        else Refused << F.fileName();
    }
    if (!Refused.isEmpty())
        emit status("Not taken: " + Refused.join(", ") + " - drop a game's folder or zip, a cover image or a .reg export");
}

//The program a GOG install's primary play task names, as one of Found (matched without case: GOG writes TUNIC.exe for
//Tunic.exe); "" when there is none.
static std::string GogPlayTask(const std::vector<Program> & Infos, const std::vector<std::string> & Found)
{
    auto Lower = [](std::string S) { for (char & C : S) C = (char)std::tolower((unsigned char)C); return S; };
    for (const Program & I : Infos)
    {
        const json G = json::parse(ReadHead(I, 1 << 20), nullptr, false);
        if (!G.is_object() || !G.contains("playTasks") || !G["playTasks"].is_array()) continue;
        for (const json & T : G["playTasks"])
        {
            if (!T.is_object() || !(T.value("isPrimary", false) || StrOf(T, "category") == "game") || StrOf(T, "path").empty()) continue;
            std::string Path = StrOf(T, "path");
            std::replace(Path.begin(), Path.end(), '\\', '/');
            const std::string Want = Lower(I.Anchored.substr(0, I.Anchored.find_last_of('/')) + "/" + Path);
            for (const std::string & F : Found) if (Lower(F) == Want) return F;
        }
    }
    return {};
}

void PkgActions::pickExe(const std::string & NodeId, const std::string & Where)
{
    int I = indexOf(NodeId);
    if (I < 0 || !Model->packageDir()) return;
    //Where = "L<layer>/<entry>": the EXEC entry the pick is for.
    int Layer = -1, Entry = -1;
    if (std::sscanf(Where.c_str(), "L%d/%d", &Layer, &Entry) != 2 || Layer < 0 || Entry < 0) return;
    std::vector<Program> Progs, Infos;
    std::set<int> Seen;
    ProgramsIn(Model->doc(), I, Model->packageDir()->path().toStdString(), Seen, Progs, &Infos);
    std::vector<std::string> Found;
    for (const Program & P : Progs) Found.push_back(P.Anchored);
    //One program at one place, however many layers supply it (a No-CD exe over the game's): listed once.
    std::sort(Found.begin(), Found.end());
    Found.erase(std::unique(Found.begin(), Found.end()), Found.end());
    //The node's OWN programs (its files, not the game it contains): a version's program is among them. Read by
    //walking this node alone - every other node marked seen.
    std::vector<Program> OwnProgs, OwnInfos;
    {
        std::set<int> Others;
        for (int K = 0; K < Model->doc().Count(); ++K) if (K != I) Others.insert(K);
        ProgramsIn(Model->doc(), I, Model->packageDir()->path().toStdString(), Others, OwnProgs, &OwnInfos);
    }
    std::vector<std::string> Own;
    for (const Program & P : OwnProgs) Own.push_back(P.Anchored);
    std::sort(Own.begin(), Own.end());
    Own.erase(std::unique(Own.begin(), Own.end()), Own.end());
    //The likeliest first, preselected: of its own programs, the one its GOG play task names, else the only likely
    //one, else the one named like the version ("Revision.exe" for "Revision"); then the same over everything.
    auto Lower = [](std::string S) { for (char & C : S) C = (char)std::tolower((unsigned char)C); return S; };
    const json & Nn = Model->doc().Node(I);
    const std::string Names = Lower(StrOf(Nn, "LABEL") + " " + StrOf(Nn, "VARIANT"));
    auto Named = [&](const std::vector<std::string> & L) -> std::string {
        for (const std::string & F : L)
        {
            std::string B = Lower(F.substr(F.find_last_of('/') + 1));
            B = B.substr(0, B.find_last_of('.'));
            if (B.size() >= 3 && Names.find(B) != std::string::npos) return F;
        }
        return {};
    };
    //An entry for Windows (or not said yet) is for a Windows program when there is one: a disc holding a DOS build
    //and launcher scripts beside it (GTA's gtados, GTA.BAT) does not make them the likely one.
    std::string WantHost;
    {
        const json & Nd = Model->doc().Node(I);
        if (HasLayers(Nd) && Layer < (int)Nd["LAYERS"].size() && Nd["LAYERS"][(size_t)Layer].contains("EXEC")
            && Nd["LAYERS"][(size_t)Layer]["EXEC"].is_array() && Entry < (int)Nd["LAYERS"][(size_t)Layer]["EXEC"].size())
            WantHost = StrOf(Nd["LAYERS"][(size_t)Layer]["EXEC"][(size_t)Entry], "HOST");
    }
    auto WindowsOnes = [&](const std::vector<std::string> & L) {
        if (!WantHost.empty() && WantHost != "win32" && WantHost != "win64") return L;
        std::vector<std::string> W;
        for (const std::string & F : L)
            for (const Program & P : Progs)
                if (P.Anchored == F)
                {
                    const std::string H = PlatformOf(P.Anchored, ReadHead(P, 4096));
                    if (H == "win32" || H == "win64") W.push_back(F);
                    break;
                }
        return W.empty() ? L : W;
    };
    const std::vector<std::string> OwnW = WindowsOnes(Own), FoundW = WindowsOnes(Found);
    std::string First = GogPlayTask(OwnInfos, Own);
    if (First.empty()) { const int K = LikelyProgram(OwnW); if (K >= 0) First = OwnW[(size_t)K]; }
    if (First.empty()) First = Named(OwnW);
    if (First.empty() && !OwnW.empty()) First = OwnW.front();
    if (First.empty()) First = GogPlayTask(Infos, Found);
    if (First.empty()) { const int K = LikelyProgram(FoundW); if (K >= 0) First = FoundW[(size_t)K]; }
    //Its own programs first (the likeliest leading), then the rest.
    std::stable_partition(Found.begin(), Found.end(), [&](const std::string & F) { return std::find(Own.begin(), Own.end(), F) != Own.end(); });
    if (const auto It = std::find(Found.begin(), Found.end(), First); It != Found.end() && It != Found.begin())
        std::rotate(Found.begin(), It, It + 1);
    if (Found.empty())
    {
        tell("Pick the program", "This node's files hold no program yet (.exe, .bat, .sh ...).\n\n"
                                 "Add the game's files first: + layer > ZIP, then actions... > bring in files.");
        return;
    }
    QStringList Items;
    for (const auto & F : Found) Items << QString::fromStdString(F);
    const int C = choose("Pick the program", "Which program starts the game?", Items, true);
    if (C < 0 || C >= Items.size()) return;
    const QString Chosen = Items[C];
    I = indexOf(NodeId);                                  // re-resolved after the modal - see browsePath
    if (I < 0) return;
    json N = Model->doc().Node(I);
    if (!HasLayers(N) || Layer >= (int)N["LAYERS"].size() || !N["LAYERS"][Layer].contains("EXEC")) return;
    json & Ex = N["LAYERS"][Layer]["EXEC"];
    if (!Ex.is_array() || Entry >= (int)Ex.size() || !Ex[Entry].is_object()) return;
    Ex[Entry]["EXE"] = Chosen.toStdString();
    //The Host is the program's own, read from its header as bringing files in does: set when blank, and corrected
    //when it names the other Windows bitness (left from the program picked before - a 32-bit launcher, say).
    for (const Program & P : Progs)
        if (P.Anchored == Chosen.toStdString())
        {
            const std::string H = PlatformOf(P.Anchored, ReadHead(P, 4096)), Was = StrOf(Ex[Entry], "HOST");
            const bool Windows = (Was == "win32" || Was == "win64") && (H == "win32" || H == "win64");
            if (!H.empty() && (Was.empty() || (Windows && H != Was)))
            {
                Ex[Entry]["HOST"] = H;
                if (!Was.empty()) emit status(QString::fromStdString("Host changed to " + H + ": the program is " + PkgGraph::PlatformWords(H)));
            }
        }
    //A game usually runs from its own folder; a work dir left blank becomes the program's.
    if (StrOf(Ex[Entry], "WORKDIR").empty())
    {
        const std::string P = Chosen.toStdString();
        Ex[Entry]["WORKDIR"] = P.substr(0, P.find_last_of('/'));
    }
    commitNode(I, std::move(N), false);
}

void PkgActions::browseCover(const std::string & NodeId)
{
    if (indexOf(NodeId) < 0) return;
    const QString Bundle = Model->packageDir() ? Model->packageDir()->path() : QDir::homePath();
    const QString Picked = pick("Pick the cover image", Bundle, "Images (*.png *.jpg *.jpeg *.webp)");
    if (Picked.isEmpty()) return;
    coverFrom(NodeId, Picked);
}

void PkgActions::coverFrom(const std::string & NodeId, const QString & Picked)
{
    //The shelf shows covers at 600x900 (2:3): any other image is stretched or cut there, so it is used only when asked.
    const QSize Size = QImageReader(Picked).size();
    if (Size != QSize(600, 900)
        && !ask("Cover", (Size.isValid() ? QString("This image is %1x%2. Covers on the shelf are 600x900 (2:3), so it will look "
                                                   "stretched or cut there.").arg(Size.width()).arg(Size.height())
                                         : QString("This file does not read as an image."))
                         + "\n\nUse it anyway?"))
        return;
    //A node presenting several tiles (a game and its expansion) says which one the image is for.
    int Tile = 0;
    if (const int I0 = indexOf(NodeId); I0 >= 0)
        if (const QStringList Tiles = TileTitles(Model->doc().Node(I0)); Tiles.size() > 1)
        {
            Tile = choose("Cover", "Which tile is this the cover of?", Tiles, true);
            if (Tile < 0) return;
        }
    const QString Bundle = Model->packageDir() ? Model->packageDir()->path() : QDir::homePath();
    QString Rel = QDir(Bundle).relativeFilePath(Picked);
    //An image from anywhere else (a download) is copied into the package under its own name: a cover ships with it.
    if (Rel.startsWith(".."))
    {
        if (!Model->packageDir()) { tell("Cover", "Save the package first: the cover is copied into its folder."); return; }
        Rel = QFileInfo(Picked).fileName();
        const QString Dst = QDir(Bundle).filePath(Rel);
        QFile A(Picked), B(Dst);
        if (QFile::exists(Dst))
        {
            //The same image already here is used as it is; another image of that name is never replaced.
            const bool Same = A.open(QIODevice::ReadOnly) && B.open(QIODevice::ReadOnly) && A.readAll() == B.readAll();
            if (!Same) { tell("Cover", "This package already has another \"" + Rel + "\". Rename one of them first."); return; }
        }
        else if (!QFile::copy(Picked, Dst)) { tell("Cover", "Could not copy the image into the package."); return; }
    }
    const int I = indexOf(NodeId);                // re-resolved after the modal — see browsePath
    if (I < 0) { tell("Browse", "That node is no longer in the package."); return; }
    //A cover belongs to a TILE, and a tile rides an EXEC entry. Anything of the wrong shape on the way is refused,
    //never replaced: the author reaches for this button on nodes they are still fixing.
    json Nc = Model->doc().Node(I);
    json * E = CoverEntry(Nc, Tile);
    if (!E) { tell("Cover", "A cover belongs to a tile on an EXEC entry, and this node has no EXEC entry yet."); return; }
    if (!E->contains("TILE") || (*E)["TILE"].is_null()) (*E)["TILE"] = json::object({{"UID", ""}, {"TITLE", ""}});
    json & T = (*E)["TILE"];
    if (!T.is_object()) { tell("Cover", "This entry's TILE is not an object - fix it in the JSON view first."); return; }
    if (T.contains("COVER") && T["COVER"].is_string()) T["COVER"] = Rel.toStdString();   // keep the form it uses
    else if (!T.contains("COVER") || T["COVER"].is_null() || T["COVER"].is_object())
    {
        if (!T.contains("COVER") || T["COVER"].is_null()) T["COVER"] = json::object();
        T["COVER"]["FILE"] = Rel.toStdString();
        T["COVER"].erase("SOURCE"); T["COVER"].erase("SIZE");   // they described the old image
    }
    else { tell("Cover", "This tile's COVER is not an object - fix it in the JSON view first."); return; }
    commitNode(I, std::move(Nc), false);
}

void PkgActions::convertZipDir(const std::string & NodeId, bool ToZip, const std::string & Which)
{
    const int I = indexOf(NodeId);
    if (I < 0) return;
    const json N = Model->doc().Node(I);
    if (!Which.empty() && DirLayerIndex(N, Which) < 0)
    { tell("Nothing to convert", "This node has no folder layer \"" + QString::fromStdString(Which) + "\" any more."); return; }
    const std::string Path = Which.empty() ? ContentName(N) : Which;
    if (Path.empty()) { tell("Nothing to convert", "This node's content layer names no file yet."); return; }
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path Src = ResolveInBundle(Bundle, Path);
    if (Src.empty())
    { tell("Unsafe path", "This layer's file does not point inside the package folder:\n\n  " +
                          QString::fromStdString(Path) +
                          "\n\nA layer's content must live in its own bundle. Nothing was changed."); return; }
    std::error_code Ec;
    if (!std::filesystem::exists(Src, Ec))
    { tell("Not here", QString::fromStdString(Path) + " is not in the bundle."); return; }

    // Both directions DELETE the source once the new form is written. The old editor did this with no
    // confirmation at all — one misclick ate the folder — while the re-store button beside it did confirm.
    const QString Question = ToZip
        ? ("Pack “" + QString::fromStdString(Path) + "” into an uncompressed (STORE) zip?\n\n"
           "The folder is DELETED afterwards. Contents are unchanged.")
        : ("Unpack “" + QString::fromStdString(Path) + "” into a folder?\n\n"
           "The zip is DELETED afterwards. Contents are unchanged.");
    if (!ask(ToZip ? "Pack as zip" : "Unpack to folder", Question)) return;

    if (ToZip)
    {
        const std::string ZipName = Path + ".zip";
        const std::filesystem::path Zip = Bundle / ZipName;
        // `zip -r` UPDATES an existing archive rather than replacing it: packing into a name a sibling layer
        // already owns (or a partial left behind by a cancelled run) would MERGE this folder into it, and the
        // folder was then deleted. Refuse a taken name, and build a side file to swap in.
        if (std::filesystem::exists(Zip, Ec))
        {
            tell("Pack as zip",
                "\"" + QString::fromStdString(ZipName) + "\" already exists in this bundle.\n\n"
                "Packing into it would MERGE this folder into that archive. Rename or remove it first.");
            return;
        }
        const std::filesystem::path TmpZip = Bundle / (ZipName + ".packing.tmp");
        std::filesystem::remove(TmpZip, Ec);
        int Total = 0;
        for (auto It = std::filesystem::recursive_directory_iterator(Src, Ec);
             !Ec && It != std::filesystem::recursive_directory_iterator(); ++It)
            if (It->is_regular_file(Ec)) ++Total;
        // -0 = STORE: vidyagodfs reads entries at their offset in the zip and cannot inflate, so a DEFLATE zip
        // mounts to garbage. -X drops extra attributes so the archive is reproducible.
        runWithProgress(NodeId, "packing zip...", ZipTool, {"-0", "-r", "-X", QString::fromStdString(TmpZip.string()), "."},
                        QString::fromStdString(Src.string()), Total,
                        [this, NodeId, ZipName, Src, Zip, TmpZip, Which](bool Ok) {
            std::error_code Rc;
            if (!Ok || !std::filesystem::exists(TmpZip, Rc) || std::filesystem::file_size(TmpZip, Rc) == 0)
            {
                std::filesystem::remove(TmpZip, Rc);
                tell("Pack failed", "zip did not complete - the folder is untouched.");
                return;                                   // the SOURCE is still here
            }
            std::filesystem::rename(TmpZip, Zip, Rc);
            if (Rc) { std::filesystem::remove(TmpZip, Rc);
                      tell("Pack failed", "Could not place the new archive - the folder is untouched."); return; }
            std::filesystem::remove_all(Src, Rc);         // only now is the folder redundant
            const int J = indexOf(NodeId);
            if (J < 0) return;
            {
                json Nj = Model->doc().Node(J);
                if (Which.empty()) SetContent(Nj, "ZIP", ZipName);
                else if (const int K = DirLayerIndex(Nj, Which); K >= 0) DirToZip(Nj["LAYERS"][(size_t)K], ZipName);
                commitNode(J, std::move(Nj), true);
            }
            refreshHints();
        });
    }
    else
    {
        std::string DirName = Path;
        if (DirName.size() > 4 && DirName.substr(DirName.size() - 4) == ".zip") DirName.resize(DirName.size() - 4);
        else DirName += "_dir";
        const std::filesystem::path Dir = Bundle / DirName;
        // The mirror of the refusal above, and the likelier collision of the two: round-tripping this action
        // produces exactly a bundle holding both "data.zip" and "data/". Unpacking into an existing non-empty
        // folder MERGES this zip's entries into whatever layer already owns that name, and the zip is then
        // DELETED — contaminating a sibling layer and destroying this one, unrecoverably.
        //`exists && !is_empty` is not enough: is_empty on a regular FILE tests its size, so a zero-byte file
        //of that name slipped through and the failure path below then remove_all'd it.
        const bool Taken = std::filesystem::exists(Dir, Ec)
                        && (!std::filesystem::is_directory(Dir, Ec) || !std::filesystem::is_empty(Dir, Ec));
        //An empty directory we did not create is still the author's; remember whether it was already there so
        //the failure path removes only what this action made.
        const bool PreExisting = std::filesystem::exists(Dir, Ec);
        if (Taken)
        {
            tell("Unpack to folder",
                "\"" + QString::fromStdString(DirName) + "\" already exists in this bundle and is not empty.\n\n"
                "Unpacking into it would MERGE this archive into that folder. Rename or remove it first.");
            return;
        }
        std::filesystem::create_directories(Dir, Ec);
        // -o: without it unzip PROMPTS on an existing file, reads EOF from a stdin-less QProcess, extracts
        // nothing further and can still exit 0 — after which the zip was deleted.
        runWithProgress(NodeId, "unpacking...", UnzipTool, {"-o", QString::fromStdString(Src.string()), "-d", QString::fromStdString(Dir.string())},
                        QString(), 0,
                        [this, NodeId, DirName, Src, Dir, PreExisting](bool Ok) {
            std::error_code Rc;
            if (!Ok)
            {
                //"the zip is untouched" was only half true: a failed unzip can leave a partial tree in the
                //folder we just created, and saying nothing changed while leaving debris is how the NEXT run
                //hits the non-empty guard above for a reason the author cannot see. Only clean up a folder
                //THIS action created — an empty one the author made is still theirs.
                if (!PreExisting) std::filesystem::remove_all(Dir, Rc);
                tell("Unpack failed", "unzip did not complete - the zip is untouched.");
                return;
            }
            // Exit 0 with an empty destination means nothing was actually extracted; deleting the zip then
            // would destroy the layer.
            const bool Empty = std::filesystem::is_empty(Dir, Rc);
            if (Rc || Empty)   // order matters: Rc is only set by the call above
            {
                if (!PreExisting) std::filesystem::remove_all(Dir, Rc);   // only what we created
                tell("Unpack failed", "Nothing was extracted - the zip is untouched."); return;
            }
            std::filesystem::remove(Src, Rc);
            const int J = indexOf(NodeId);
            if (J < 0) return;
            { json Nj = Model->doc().Node(J); SetContent(Nj, "DIR", DirName); commitNode(J, std::move(Nj), true); }
            refreshHints();
        });
    }
}

void PkgActions::reStore(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    if (I < 0) return;
    const std::string Path = ContentName(Model->doc().Node(I));
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path Zip = ResolveInBundle(Bundle, Path);
    if (Zip.empty())
    { tell("Unsafe path", "This layer's file does not point inside the package folder. Nothing was changed."); return; }

    const QString Msg = "“" + QString::fromStdString(Path) + "” is DEFLATE-compressed.\n\n"
        "VidyaGod mounts zip layers by reading each file at its position inside the zip — it cannot decompress "
        "on the fly, so a compressed zip mounts to garbage and the game won't launch.\n\n"
        "Re-pack it uncompressed (STORE) now? Contents are unchanged; only the packing differs "
        "(the file may get larger).";
    if (!ask("Re-pack zip as STORE", Msg)) return;

    const std::filesystem::path Tmp = Bundle / (Path + ".restore.tmp");
    const std::filesystem::path NewZip = Bundle / (Path + ".restore.zip");
    std::error_code Ec; std::filesystem::remove_all(Tmp, Ec); std::filesystem::remove(NewZip, Ec);
    std::filesystem::create_directories(Tmp, Ec);
    runWithProgress(NodeId, "unpacking...", UnzipTool, {"-o", QString::fromStdString(Zip.string()), "-d", QString::fromStdString(Tmp.string())},
                    QString(), 0,
                    [this, NodeId, Zip, Tmp, NewZip](bool Ok) {
        std::error_code Rc;
        if (!Ok)
        {
            std::filesystem::remove_all(Tmp, Rc);
            tell("Re-store failed", "unzip did not complete - the zip is untouched.");
            return;                                       // the ORIGINAL is still here: nothing was destroyed
        }
        int Total = 0;
        for (auto It = std::filesystem::recursive_directory_iterator(Tmp, Rc);
             !Rc && It != std::filesystem::recursive_directory_iterator(); ++It)
            if (It->is_regular_file(Rc)) ++Total;
        // Pack to a SIDE file. The original is deleted only once a complete replacement exists on disk — a
        // cancel or an ENOSPC (a STORE re-pack is LARGER than the DEFLATE original, as the dialog says) must
        // not be able to destroy the layer's only copy.
        runWithProgress(NodeId, "re-packing STORE...", ZipTool, {"-0", "-r", "-X", QString::fromStdString(NewZip.string()), "."},
                        QString::fromStdString(Tmp.string()), Total,
                        [this, Zip, Tmp, NewZip](bool Ok2) {
            std::error_code Rc2;
            std::filesystem::remove_all(Tmp, Rc2);
            if (!Ok2 || !std::filesystem::exists(NewZip, Rc2) || std::filesystem::file_size(NewZip, Rc2) == 0)
            {
                std::filesystem::remove(NewZip, Rc2);
                tell("Re-store failed",
                                     "Re-packing did not complete - the original zip is untouched.");
                return;
            }
            std::filesystem::rename(NewZip, Zip, Rc2);    // atomic swap on the same filesystem
            if (Rc2) { tell("Re-store failed", "Could not replace the original zip."); return; }
            refreshHints();
        });
    });
}

void PkgActions::refreshHints()
{
    if (!Model->packageDir() || !Canvas) return;
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    // Probing a zip's central directory is cheap per file but adds up across a bundle, and it used to run
    // synchronously while building the UI — stalling the editor on big packages. Off-thread, then push in.
    struct Probe { std::string Id; std::string Path; };
    auto Todo = std::make_shared<std::vector<Probe>>();
    const PkgDoc::Document & D = Model->doc();
    for (int I = 0; I < D.Count(); ++I)
        if (ContentType(D.Node(I)) == "ZIP" && !ContentName(D.Node(I)).empty())
            Todo->push_back({D.Handle(I), ContentName(D.Node(I))});
    if (Todo->empty()) return;
    auto Deflated = std::make_shared<std::vector<std::string>>();
    AsyncWork::Run(this,
        [Todo, Deflated, Bundle]() {
            for (const Probe & P : *Todo)
            {
                const std::filesystem::path Z = Bundle / P.Path;
                std::error_code Ec;
                if (!std::filesystem::exists(Z, Ec)) continue;
                if (!ManifestModel::ZipFullyStored(Z.string())) Deflated->push_back(P.Id);
            }
        },
        [this, Todo, Deflated]() {
            for (const Probe & P : *Todo) Canvas->setNodeHints(current(P.Id), {});
            for (const std::string & Id : *Deflated) Canvas->setNodeHints(current(Id), {"deflate"});
        });
}

// ---------------------------------------------------------------------------
// DeclareExec: test launch
// ---------------------------------------------------------------------------

void PkgActions::testLaunch(const std::string & Handle)
{
    //Launch what is on disk, not what is half-typed: an unsaved package is saved first (a re-mint).
    if (!saveFirst("Test launch")) return;
    const std::string NodeId = current(Handle);
    // The real pre-launch dialog, on this one launchable: the runner picker, the optional-module toggles and the
    // CustomVars the player would actually see. Testing through the same door the player uses is the point —
    // a bespoke "run it" path is exactly how an editor drifts from the launcher.
    //PreLaunchWindow is MODELESS, so the index must outlive the dialog (and its launch) and must not be shared with the
    //next one. A `static NodeIndex` reassigned per click did both wrong: opening a second test launch destroyed every
    //std::map node the FIRST dialog's Node pointers referred to, and the first dialog silently began describing a
    //different graph. Each dialog gets its own index, which it and its launch keep alive.
    auto Idx = std::make_shared<const NodeIndex>(Model->BuildExecIndex());
    if (!Idx->Find(NodeId))
    { tell("Test launch", "This node isn't in the catalog index yet — save the package first."); return; }
    auto * Dlg = new PreLaunchWindow(Model->globalConfig(), Idx, {NodeId}, std::string(), Parent);   // the node's first tile
    Dlg->setAttribute(Qt::WA_DeleteOnClose);
    Dlg->show();
}

// ---------------------------------------------------------------------------
// Capture (files / registry / setup) — anchored at THIS node
// ---------------------------------------------------------------------------

void PkgActions::openCapture(const std::string & Handle, int Mode)
{
    //A capture runs on the package as saved: an unsaved package is saved first.
    if (!saveFirst("Capture")) return;
    const std::string NodeId = current(Handle);
    // The runtime is built UP TO this node and whatever gets captured becomes a NEW node parented here — which
    // is what "capture at this point in the chain" means structurally. Parented to the editor window, not to a
    // node widget: a capture writes back through the model, which reloads, which would destroy the session.
    const CaptureMode M = Mode == 1 ? CaptureMode::Files
                        : Mode == 2 ? CaptureMode::Registry
                                    : CaptureMode::Setup;
    (new AuthoringSessionWindow(Model, NodeId, M, Parent))->show();
}

// ---------------------------------------------------------------------------
// BinaryPatch: verify every EXPECT against the pristine file
// ---------------------------------------------------------------------------


void PkgActions::findUsages(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    //Every KEY this node's VARS declare — a node may carry many.
    if (I < 0) return;
    const std::vector<std::string> Keys = VarKeysOf(Model->doc().Node(I));
    if (Keys.empty()) { tell("Find usages", "This node declares no variable KEY yet."); return; }
    // A whole-document text scan for %KEY%: a var nothing consumes is dead weight, and two nodes declaring the
    // same KEY is the ambiguity the lint is about.
    QStringList Report;
    const PkgDoc::Document & D = Model->doc();
    for (const std::string & Key : Keys)
    {
        const std::string Token = "%" + Key + "%";
        QStringList Users;
        for (int K = 0; K < D.Count(); ++K)
        {
            const json & N = D.Node(K);
            const std::string H = D.Handle(K);
            if (K == I) continue;
            const std::string Disp = !StrOf(N, "LABEL").empty() ? StrOf(N, "LABEL") : H;   // show the readable name
            const std::string Dump = N.dump();
            bool SameKey = false;
            for (const std::string & Declared : VarKeysOf(N)) if (Declared == Key) SameKey = true;
            if (Dump.find(Token) != std::string::npos) Users << QString::fromStdString(Disp);
            else if (SameKey) Users << QString::fromStdString(Disp) + "  (declares the same KEY)";
        }
        Report << (Users.isEmpty() ? QString("Nothing in this bundle uses %1.").arg(QString::fromStdString(Token))
                                   : QString("%1 is used by:\n").arg(QString::fromStdString(Token)) + Users.join("\n"));
    }
    tell("Find usages", Report.join("\n\n"));
}

//Parse a .reg export into editable rows. Pure text in, rows out - no dialogs, no model - so the parser
//(the part with all the traps: UTF-16, continuation lines, deletions, escapes) is directly testable.
std::vector<PkgGraph::RegRow> PkgActions::ParseRegExport(const QString & Text, int * DeletionsOut)
{
    // A binary value is written as `hex:aa,bb,\` with the rest on CONTINUATION lines. Reading line-by-line
    // silently TRUNCATED every such value to its first line (the continuations parse as lines with no '='),
    // producing a plausible-looking but WRONG value. Rejoin them before parsing anything.
    QStringList Lines;
    for (QString Raw : Text.split(QRegularExpression("\r?\n")))
    {
        QString L = Raw.trimmed();
        if (!Lines.isEmpty() && Lines.back().endsWith('\\'))
        {
            Lines.back().chop(1);
            Lines.back() += L;
            continue;
        }
        Lines << L;
    }

    std::vector<PkgGraph::RegRow> Rows;
    QString Section;
    int Deletions = 0;
    for (const QString &Line : Lines)
    {
        if (Line.isEmpty() || Line.startsWith(';')) continue;
        if (Line.startsWith('[') && Line.endsWith(']'))
        {
            Section = Line.mid(1, Line.size() - 2);
            if (Section.startsWith('-')) { Section.clear(); ++Deletions; continue; }   // a deletion section
            //The key ITSELF, flagged: an empty NAME is the key's default value, so without the flag this row
            //shadows a real `@=` line from the same section.
            { PkgGraph::RegRow R{Section.toStdString(), "", ""}; R.KeyOnly = true; Rows.push_back(std::move(R)); }
            continue;
        }
        if (Section.isEmpty()) continue;
        const int Eq = Line.indexOf('=');
        if (Eq <= 0) continue;
        QString Name = Line.left(Eq).trimmed(), Val = Line.mid(Eq + 1).trimmed();
        // `@` in a .reg file IS the key's default value, and on disk that is the EMPTY name — 48 of them live
        // in LAVFilters, and RegistryWrapper writes an empty name back out as `@` (registrywrapper.cpp).
        // Mapping it to the literal name "@" wrote a value CALLED @ instead, which is a different thing that
        // no program reads. (The earlier comment here claimed the opposite and blamed RegRowsInto for
        // dropping empty names — that WAS true, and was the actual bug; it is fixed at the source now.)
        if (Name == "@") Name.clear();
        else if (Name.startsWith('"') && Name.endsWith('"')) Name = Name.mid(1, Name.size() - 2);
        // `"Name"=-` DELETES the value. A RegEdit layer only ever writes, so there is nothing to import — and
        // importing it as the literal string "-" would write exactly the opposite of what the file asks.
        if (Val == "-") { ++Deletions; continue; }
        if (Val.startsWith('"') && Val.endsWith('"')) Val = Val.mid(1, Val.size() - 2);
        Val.replace("\\\"", "\"");                                          // .reg escapes quotes...
        Val.replace("\\\\", "\\");                                          // ...and backslashes
        if (Name.isEmpty() && Val.isEmpty()) continue;
        Rows.push_back({Section.toStdString(), Name.toStdString(), Val.toStdString()});
    }
    if (DeletionsOut) *DeletionsOut = Deletions;
    return Rows;
}

void PkgActions::importReg(const std::string & NodeId)
{
    const QString File = pick("Import a .reg file", QDir::homePath(),
                              "Registry exports (*.reg);;All files (*)");
    if (File.isEmpty()) return;
    importRegFile(NodeId, File);
}

void PkgActions::importRegFile(const std::string & NodeId, const QString & File)
{
    std::ifstream In(File.toStdString(), std::ios::binary);
    if (!In) { tell("Import .reg", "Could not read that file."); return; }

    // A .reg export is [Key] sections followed by "Name"=value lines. regedit writes UTF-16LE by default and
    // wine writes UTF-8, so accept both rather than silently importing mojibake.
    std::string Raw((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
    QString Text = (Raw.size() >= 2 && (unsigned char)Raw[0] == 0xFF && (unsigned char)Raw[1] == 0xFE)
                       ? QString::fromUtf16(reinterpret_cast<const char16_t *>(Raw.data() + 2), (int)(Raw.size() - 2) / 2)
                       : QString::fromUtf8(Raw.c_str(), (int)Raw.size());

    int Deletions = 0;
    const std::vector<PkgGraph::RegRow> Rows = ParseRegExport(Text, &Deletions);
    if (Rows.empty())
    {
        tell("Import .reg", Deletions > 0
                 ? QString("That file only DELETES registry entries (%1). Import brings in values; write a "
                           "deletion as null in the JSON view.").arg(Deletions)
                 : QString("No keys or values found in that file."));
        return;
    }

    //Re-resolved AFTER the modal: the node can be deleted (or the document reloaded) while the file dialog is
    //open, and ["NODES"][-1] is an unchecked size_t index.
    const int I = indexOf(NodeId);
    if (I < 0) { tell("Import .reg", "That node is no longer in the package."); return; }
    json N = Model->doc().Node(I);
    //Into the node's first REG layer (a new one, both views, when it has none). A value of the wrong shape on the
    //way is REFUSED, not replaced: the author reaches for Import precisely when the node is in a state they are
    //trying to repair, and replacing a hand-edited tree would save the loss.
    if (N.contains("LAYERS") && !N["LAYERS"].is_array())
    { tell("Import .reg", "This node's LAYERS is not a list - fix it in the JSON view first."); return; }
    json * Layer = nullptr;
    if (N.contains("LAYERS"))
        for (auto & L : N["LAYERS"]) if (PkgGraph::LayerType(L) == "REG") { Layer = &L; break; }
    if (!Layer)
    {
        //BOTH views, not 32 alone: a 64-bit-only import silently landing in the 32-bit view is the silent-nothing
        //the audit exists to catch. Narrowing it afterwards is one checkbox.
        N["LAYERS"].push_back(json{{"REG", json::object()}, {"ARCH", json::array({"32", "64"})}});
        Layer = &N["LAYERS"].back();
    }
    if ((*Layer)["REG"].is_null()) (*Layer)["REG"] = json::object();
    if (!(*Layer)["REG"].is_object())
    {
        tell("Import .reg",
             "This node's REG layer is not a hive tree, so importing would replace it and lose what is there.\n\n"
             "Fix it in the JSON view first.");
        return;
    }
    json & Entry = (*Layer)["REG"];
    std::vector<PkgGraph::RegRow> Merged = PkgGraph::RegRowsOf(Entry);
    Merged.insert(Merged.end(), Rows.begin(), Rows.end());
    PkgGraph::RegRowsInto(Entry, Merged);
    commitNode(I, std::move(N), false);
    tell("Import .reg",
         QString("Imported %1 key/value row(s).").arg((int)Rows.size())
             + (Deletions > 0 ? QString("\n\nSkipped %1 DELETION(s) - write each as null in the JSON view.").arg(Deletions)
                              : QString()));
}

// ---------------------------------------------------------------------------
// Content: delta <-> full
// ---------------------------------------------------------------------------

//Reconstruct a delta node's content into a real archive next to it. DeltaByteSource composes the delta over its
//base and serves the target's bytes on the fly, so this is a straight stream-to-file — no re-diffing, and a
//chain of deltas composes into ONE flat segment map rather than nesting.
//Reconstruct a delta's content into a real archive. PURE PATHS IN, no model, no canvas, no `this` — it runs
//on a worker thread for as long as a multi-GB stream takes, and anything it touched from the GUI side could be
//freed or reallocated underneath it.
static bool ReconstructTo(const std::filesystem::path & BaseFile, const std::filesystem::path & DeltaFile,
                          const std::filesystem::path & Out, std::string & Err)
{
    std::error_code Ec;
    if (!std::filesystem::exists(BaseFile, Ec)) { Err = "the base archive is not present locally"; return false; }
    if (!std::filesystem::exists(DeltaFile, Ec)) { Err = "the delta blob is not present locally"; return false; }

    auto Base  = std::make_shared<MmapByteSource>();
    auto Delta = std::make_shared<MmapByteSource>();
    if (!Base->Map(BaseFile.string()) || !Delta->Map(DeltaFile.string())) { Err = "could not map the archives"; return false; }

    // verifyBase=true: this is an AUTHORING conversion that DELETES the only copy. A delta composed over the
    // wrong base yields a plausible archive full of garbage whose first symptom is the game not starting.
    std::string Cerr;
    auto Recon = vgdelta::DeltaByteSource::Create(Delta, Base, Cerr, /*verifyBase=*/true);
    if (!Recon) { Err = "the delta could not be composed over its base" + (Cerr.empty() ? std::string() : " (" + Cerr + ")"); return false; }

    std::ofstream O(Out, std::ios::binary);
    if (!O) { Err = "could not write the reconstructed archive"; return false; }
    std::vector<uint8_t> Buf(4u << 20);
    const uint64_t Total = Recon->size();
    for (uint64_t Off = 0; Off < Total; )
    {
        const ssize_t R = Recon->pread(Buf.data(), std::min<uint64_t>(Buf.size(), Total - Off), Off);
        if (R <= 0) { Err = "short read reconstructing the delta"; O.close(); std::filesystem::remove(Out, Ec); return false; }
        O.write((const char *)Buf.data(), R);
        Off += (uint64_t)R;
    }
    O.close();
    if (!O.good()) { Err = "could not finish writing the reconstructed archive"; std::filesystem::remove(Out, Ec); return false; }
    //Size is the one cheap end-to-end check that the stream produced the whole target.
    if (std::filesystem::file_size(Out, Ec) != Total)
    { Err = "the reconstructed archive is the wrong size"; std::filesystem::remove(Out, Ec); return false; }
    return true;
}

void PkgActions::undelta(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    if (I < 0) return;
    const std::string Path = ContentName(Model->doc().Node(I));
    if (Path.empty()) { tell("Undelta", "This node's content layer names no file yet."); return; }

    // Name the restored archive after the delta, minus the .vgdelta suffix — "delta_from__base.zip.vgdelta"
    // came from "base.zip", so the round trip lands back on a sensible name rather than a doubled extension.
    std::string ZipName = Path;
    const std::string Suffix = ".vgdelta";
    if (ZipName.size() > Suffix.size() && ZipName.compare(ZipName.size() - Suffix.size(), Suffix.size(), Suffix) == 0)
        ZipName.resize(ZipName.size() - Suffix.size());
    if (ZipName.size() < 4 || ZipName.compare(ZipName.size() - 4, 4, ".zip") != 0) ZipName += ".zip";
    const std::string Base = ZipName;
    ZipName = NodeId + "__" + Base;                      // unique in the bundle, and obviously this node's

    const QString Msg = QString("Reconstruct the full archive from \u201C%1\u201D and store it as a plain zip?\n\n"
                                "The base is verified against the delta's header before anything is written, and "
                                "the .vgdelta is removed only once the archive is complete.")
                            .arg(QString::fromStdString(Path));
    if (!ask("Undelta", Msg)) return;

    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path Out = Bundle / ZipName;
    const std::filesystem::path DeltaFile = ResolveInBundle(Bundle, Path);
    if (DeltaFile.empty())
    { tell("Unsafe path", "This layer's file does not point inside the package folder. Nothing was changed."); return; }

    // Resolve everything the reconstruction needs BEFORE going off-thread. reconstructDelta used to read the
    // model from the worker — for as long as a multi-GB stream takes — so closing the editor freed the
    // document under it, and any edit that grew the NODES array reallocated it mid-read.
    std::vector<PkgGraph::NodeRef> Refs;
    for (int K = 0; K < Model->doc().Count(); ++K) Refs.push_back({Model->doc().Handle(K), &Model->doc().Node(K)});
    const std::string BasePath = PkgGraph::DeltaBase(Refs, I);
    if (BasePath.empty())
    { tell("Undelta", "This delta has no base to reconstruct against: a contained node with a zip at its target."); return; }

    const std::filesystem::path BaseFile = ResolveInBundle(Bundle, BasePath);
    if (BaseFile.empty())
    { tell("Unsafe path", "The base layer's file does not point inside the package folder."); return; }
    Canvas->beginAction(NodeId, "reconstructing...", /*cancellable=*/false);   // a stream with no interrupt point (and every
                                                                              // return after this one ends the action)
    auto Ok  = std::make_shared<bool>(false);
    auto Err = std::make_shared<std::string>();
    auto Alive = Alive_;
    AsyncWork::Run(this,
        [BaseFile, DeltaFile, Out, Ok, Err]() { *Ok = ReconstructTo(BaseFile, DeltaFile, Out, *Err); },
        [this, NodeId, ZipName, Out, DeltaFile, Ok, Err, Alive]() {
            if (!Alive->load()) return;
            Canvas->endAction(current(NodeId));
            if (!*Ok)
            {
                tell("Undelta",
                                     "Nothing was changed - " + QString::fromStdString(*Err) + ".");
                return;
            }
            // Only now is the delta redundant: the archive exists and is the right size.
            std::error_code Rc; std::filesystem::remove(DeltaFile, Rc);
            const int J = indexOf(NodeId);
            if (J < 0) return;
            { json Nj = Model->doc().Node(J); SetContent(Nj, "ZIP", ZipName); commitNode(J, std::move(Nj), true); }
            refreshHints();
        });
}

void PkgActions::makeDelta(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    if (I < 0) return;
    const PkgDoc::Document & D = Model->doc();
    std::vector<PkgGraph::NodeRef> Refs;
    for (int K = 0; K < D.Count(); ++K) Refs.push_back({D.Handle(K), &D.Node(K)});
    const std::string TgtPath = ContentName(D.Node(I));
    const std::string BasePath = PkgGraph::DeltaBase(Refs, I);
    if (ContentType(D.Node(I)) != "ZIP" || TgtPath.empty() || BasePath.empty())
    { tell("Make delta", "This node needs a zip, and a node it contains with a zip at the same target to diff against."); return; }

    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path BaseFile = ResolveInBundle(Bundle, BasePath);
    const std::filesystem::path TgtFile   = ResolveInBundle(Bundle, TgtPath);
    if (BaseFile.empty() || TgtFile.empty())
    { tell("Unsafe path", "A layer's file does not point inside the package folder. Nothing was changed."); return; }
    std::error_code Ec;
    if (!std::filesystem::exists(BaseFile, Ec) || !std::filesystem::exists(TgtFile, Ec))
    { tell("Make delta", "Both this layer's zip and its parent's must be present locally."); return; }

    const QString Msg = QString("Store \u201C%1\u201D as a binary delta against \u201C%2\u201D?\n\n"
                                "The delta is byte-verified against the original before anything is deleted; "
                                "the full zip is then removed.")
                            .arg(QString::fromStdString(TgtPath)).arg(QString::fromStdString(BasePath));
    if (!ask("Make delta", Msg)) return;

    // Named after THIS node, not just the base: two siblings delta'd against the same base (the documented
    // fan-out shape) produced identical blob names, and the second run overwrote the first's blob after the
    // first's full zip had already been deleted — node A silently serving node B's bytes.
    std::string Blob = NodeId + "__from__" + BasePath + ".vgdelta";
    for (char &C : Blob) if (C == '/' || C == '\\') C = '_';        // a base in a subdir must not escape the bundle
    auto Abort = std::make_shared<std::atomic<bool>>(false);
    Aborts[NodeId] = Abort;
    Canvas->beginAction(NodeId, "diffing...", /*cancellable=*/true);

    auto Ok      = std::make_shared<bool>(false);
    auto ErrText = std::make_shared<std::string>();
    auto Alive   = Alive_;
    //Hoisted BEFORE the worker starts. Reading this->Canvas inside the worker body — which is where the
    //progress callbacks are constructed, minutes in — is the same use-after-free the qApp post exists to
    //close, merely moved from invokeMethod's target to a member read.
    PkgCanvas * const Cv = Canvas;
    AsyncWork::Run(this,
        [Cv, NodeId, BaseFile, TgtFile, Bundle, Blob, Abort, Ok, ErrText, Alive]() {
            // Archives are MAPPED, not read: a chain step holds base and target open at once, and eager reads
            // OOM-killed a 32GB desktop. Scratch spills beside the archives, never /tmp (a tmpfs on most
            // desktops, which would just move the memory pressure).
            auto Base = std::make_shared<MmapByteSource>();
            auto Tgt  = std::make_shared<MmapByteSource>();
            if (!Base->Map(BaseFile.string()) || !Tgt->Map(TgtFile.string())) { *ErrText = "could not map the archives"; return; }

            std::vector<uint8_t> Delta = vgdelta::GenerateDelta(
                Base->p, Base->n, Tgt->p, Tgt->n, vgdelta::DEFAULT_BLOCK, Bundle.string(),
                [Cv, NodeId, Abort, Alive](int Stage, uint64_t Done, uint64_t Total) {
                    // AsyncWork guards the COMPLETION, not the work. This callback runs on the worker thread
                    // for minutes on a multi-GB archive; closing the editor destroys PkgActions underneath it,
                    // and invokeMethod would then touch freed memory. The shared flag is cleared in ~PkgActions.
                    if (!Alive->load()) return false;
                    if (Abort->load()) return false;
                    // Three passes with independent totals -> thirds of one bar, so the number never jumps back.
                    const float F = Total ? (float)Done / (float)Total : 0.0f;
                    PostProgress(Cv, Alive, NodeId, Stage == 0 ? F * 0.5f : 0.5f + F * 0.2f,
                                 Stage == 0 ? "scanning" : "compressing");
                    return true;
                });
            if (Delta.empty()) { *ErrText = Abort->load() ? "cancelled" : "delta generation failed"; return; }

            // Byte-verify through the SAME routine the tests use before deleting anything — a delta that
            // reconstructs the wrong bytes is indistinguishable from a good one until someone tries to play.
            std::string Verr;
            if (!vgdelta::VerifyDelta(Delta, Base, *Tgt, Verr,
                    [Cv, NodeId, Abort, Alive](int, uint64_t Done, uint64_t Total) {
                        if (!Alive->load() || Abort->load()) return false;
                        const float F = Total ? (float)Done / (float)Total : 0.0f;
                        PostProgress(Cv, Alive, NodeId, 0.7f + F * 0.3f, "verifying");
                        return true;
                    }))
            { *ErrText = Abort->load() ? "cancelled" : ("verify failed: " + Verr); return; }

            const std::filesystem::path BlobPath = Bundle / Blob;
            {
                std::ofstream O(BlobPath, std::ios::binary);
                O.write((const char *)Delta.data(), (std::streamsize)Delta.size());
                O.close();                                   // flush BEFORE judging: a buffered ENOSPC on the
                if (!O.good())                               // final chunk left good() true until the close
                { *ErrText = "could not write the delta blob"; std::error_code Rc; std::filesystem::remove(BlobPath, Rc); return; }
            }
            std::error_code Sc;
            if (std::filesystem::file_size(BlobPath, Sc) != Delta.size())
            { *ErrText = "the delta blob is the wrong size on disk"; std::filesystem::remove(BlobPath, Sc); return; }
            *Ok = true;
        },
        [this, NodeId, Blob, TgtFile, Ok, ErrText]() {
            Aborts.erase(NodeId);
            Canvas->endAction(current(NodeId));
            if (!*Ok)
            {
                if (*ErrText != "cancelled")
                    tell("Make delta",
                                         "Nothing was changed - " + QString::fromStdString(*ErrText) + ".");
                return;
            }
            std::error_code Rc; std::filesystem::remove(TgtFile, Rc);
            const int J = indexOf(NodeId);
            if (J < 0) return;
            { json Nj = Model->doc().Node(J); SetContent(Nj, "DELTA", Blob); commitNode(J, std::move(Nj), true); }   // its base is the zip at the same target, beneath it
        });
}

