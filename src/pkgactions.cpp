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
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>

#include <cstring>
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
    : QObject(parent), Model(model), Canvas(canvas), Parent(dialogParent) {}

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
    const auto & Ns = Model->doc()["NODES"];
    for (int I = 0; I < (int)Ns.size(); ++I)
        if (StrOf(Ns[I], "CID") == nodeId) return I;   // indexOf by the node handle (CID)
    return -1;
}

void PkgActions::perform(const QString & nodeIdQ, const QString & actionQ)
{
    const std::string NodeId = nodeIdQ.toStdString(), A = actionQ.toStdString();
    if (indexOf(NodeId) < 0) return;
    if (Canvas->isBusy(NodeId)) return;                   // one heavy action per node at a time

    if      (A == "browse")        browsePath(NodeId);
    else if (A == "browse_cover")  browseCover(NodeId);
    else if (A == "to_zip")        convertZipDir(NodeId, /*ToZip=*/true);
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
    const std::string NodeId = nodeIdQ.toStdString();
    if (auto It = Procs.find(NodeId); It != Procs.end() && It->second) It->second->kill();
    if (auto It = Aborts.find(NodeId); It != Aborts.end() && It->second) It->second->store(true);
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
        if (Total > 0) Canvas->setProgress(NodeId, qMin(1.0f, (float)*Seen / (float)Total),
                                           QString("%1 / %2").arg(*Seen).arg(Total));
        else           Canvas->setProgress(NodeId, -1.0f, QString("%1 files").arg(*Seen));
    });
    // A failure to even START (missing zip/unzip, not executable) emits errorOccurred and NEVER finished. The
    // caller's completion handler is where rollback lives, so skipping it left the node locked to a file that
    // had already been deleted, with no message at all. Route both paths through one place.
    auto Complete = [this, P, NodeId, Seen, Finished, Done](bool Ok) {
        if (*Finished) return;
        *Finished = true;
        Procs.erase(NodeId);
        Canvas->endAction(NodeId);                        // paired with beginAction on EVERY path out
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
    const std::string Form = [&]{ const std::string F = StrOf(Model->doc()["NODES"][I], "FORM"); return F.empty() ? std::string("zip") : F; }();

    const QString Picked = (Form == "dir")
        ? pick("Pick the folder this layer supplies", Bundle, QString(), true)
        : pick("Pick the file this layer supplies", Bundle, QString());
    if (Picked.isEmpty()) return;

    // A layer's PATH is relative to its bundle — content lives WITH the package. If the pick is from elsewhere,
    // say so rather than silently writing an absolute path that only resolves on this machine.
    const QString Rel = QDir(Bundle).relativeFilePath(Picked);
    if (Rel.startsWith(".."))
    {
        tell("Outside the bundle",
            "“" + QFileInfo(Picked).fileName() + "” is outside this package.\n\n"
            "A layer's PATH is relative to the bundle, so the file has to live here. Copy it into\n"
            + Bundle + "\nand pick it again.");
        return;
    }
    I = indexOf(NodeId);                          // re-resolved after the modal — see the note above
    if (I < 0) { tell("Browse", "That node is no longer in the package."); return; }
    Model->doc()["NODES"][I]["PATH"] = Rel.toStdString();
    Model->SaveNodes();
    Model->requestReload();
}

void PkgActions::browseCover(const std::string & NodeId)
{
    if (indexOf(NodeId) < 0) return;
    const QString Bundle = Model->packageDir() ? Model->packageDir()->path() : QDir::homePath();
    const QString Picked = pick("Pick the cover image", Bundle, "Images (*.png *.jpg *.jpeg *.webp)");
    if (Picked.isEmpty()) return;
    const QString Rel = QDir(Bundle).relativeFilePath(Picked);
    if (Rel.startsWith("..")) { tell("Outside the bundle", "The cover must live in the package folder."); return; }
    const int I = indexOf(NodeId);                // re-resolved after the modal — see browsePath
    if (I < 0) { tell("Browse", "That node is no longer in the package."); return; }
    json & N = Model->doc()["NODES"][I];
    if (!N.contains("COVER") || !N["COVER"].is_object()) N["COVER"] = json::object();
    N["COVER"]["PATH"] = Rel.toStdString();
    Model->SaveNodes();
    Model->requestReload();
}

void PkgActions::convertZipDir(const std::string & NodeId, bool ToZip)
{
    const int I = indexOf(NodeId);
    json & N = Model->doc()["NODES"][I];
    const std::string Path = StrOf(N, "PATH");
    if (Path.empty()) { tell("Nothing to convert", "This layer has no PATH yet."); return; }
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path Src = ResolveInBundle(Bundle, Path);
    if (Src.empty())
    { tell("Unsafe path", "This layer's PATH does not point inside the package folder:\n\n  " +
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
                        [this, NodeId, ZipName, Src, Zip, TmpZip](bool Ok) {
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
            Model->doc()["NODES"][J]["FORM"] = "zip";
            Model->doc()["NODES"][J]["PATH"] = ZipName;
            Model->SaveNodes(); Model->requestReload();
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
            Model->doc()["NODES"][J]["FORM"] = "dir";
            Model->doc()["NODES"][J]["PATH"] = DirName;
            Model->SaveNodes(); Model->requestReload();
            refreshHints();
        });
    }
}

void PkgActions::reStore(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    const std::string Path = StrOf(Model->doc()["NODES"][I], "PATH");
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path Zip = ResolveInBundle(Bundle, Path);
    if (Zip.empty())
    { tell("Unsafe path", "This layer's PATH does not point inside the package folder. Nothing was changed."); return; }

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
            Model->requestReload();
            refreshHints();
        });
    });
}

void PkgActions::refreshHints()
{
    if (!Model->packageDir() || !Canvas) return;
    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const json & Ns = Model->doc()["NODES"];
    // Probing a zip's central directory is cheap per file but adds up across a bundle, and it used to run
    // synchronously while building the UI — stalling the editor on big packages. Off-thread, then push in.
    struct Probe { std::string Id; std::string Path; };
    auto Todo = std::make_shared<std::vector<Probe>>();
    for (const auto & N : Ns)
        if (StrOf(N, "TYPE") == "Content" && StrOf(N, "FORM") == "zip"
            && !StrOf(N, "PATH").empty())
            Todo->push_back({StrOf(N, "CID"), StrOf(N, "PATH")});   // Probe.Id = handle (Running is keyed by it)
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
            for (const Probe & P : *Todo) Canvas->setNodeHints(P.Id, {});
            for (const std::string & Id : *Deflated) Canvas->setNodeHints(Id, {"deflate"});
        });
}

// ---------------------------------------------------------------------------
// DeclareExec: test launch
// ---------------------------------------------------------------------------

void PkgActions::testLaunch(const std::string & NodeId)
{
    Model->SaveNodes();                       // launch what is on disk, not what is half-typed
    // The real pre-launch dialog, on this one launchable: the runner picker, the optional-module toggles and the
    // CustomVars the player would actually see. Testing through the same door the player uses is the point —
    // a bespoke "run it" path is exactly how an editor drifts from the launcher.
    //PreLaunchWindow is MODELESS and keeps a bare `const NodeIndex *`, so the index must outlive the dialog and
    //must not be shared with the next one. A `static NodeIndex` reassigned per click did both wrong: opening a
    //second test launch destroyed every std::map node the FIRST dialog's Node pointers referred to, and the
    //first dialog silently began describing a different graph. Give each dialog its own index, owned by it.
    auto Idx = std::make_unique<NodeIndex>(Model->BuildExecIndex());
    if (!Idx->Find(NodeId))
    { tell("Test launch", "This node isn't in the catalog index yet — save the package first."); return; }
    auto * Dlg = new PreLaunchWindow(Model->globalConfig(), Idx.get(), {NodeId}, Parent);
    Dlg->setAttribute(Qt::WA_DeleteOnClose);
    //Hand ownership to the dialog: destroyed() fires before ~QObject finishes but after the dialog has stopped
    //using the index, and WA_DeleteOnClose means that is the only exit.
    NodeIndex *Raw = Idx.release();
    QObject::connect(Dlg, &QObject::destroyed, Dlg, [Raw]{ delete Raw; });
    Dlg->show();
}

// ---------------------------------------------------------------------------
// Capture (files / registry / setup) — anchored at THIS node
// ---------------------------------------------------------------------------

void PkgActions::openCapture(const std::string & NodeId, int Mode)
{
    Model->SaveNodes();
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
    const std::string Key = StrOf(Model->doc()["NODES"][I], "KEY");
    if (Key.empty()) { tell("Find usages", "This variable has no KEY yet."); return; }
    // A whole-document text scan for %KEY%: a var nothing consumes is dead weight, and two nodes declaring the
    // same KEY is the ambiguity the lint is about.
    const std::string Token = "%" + Key + "%";
    QStringList Users;
    const json & Ns = Model->doc()["NODES"];
    for (const auto & N : Ns)
    {
        const std::string H = StrOf(N, "CID");       // self-exclude by HANDLE (NodeId is a handle)
        if (H == NodeId) continue;
        const std::string Disp = !StrOf(N, "LABEL").empty() ? StrOf(N, "LABEL") : H;   // show the readable name
        const std::string Dump = N.dump();
        if (Dump.find(Token) != std::string::npos) Users << QString::fromStdString(Disp);
        else if (StrOf(N, "TYPE") == "CustomVar" && StrOf(N, "KEY") == Key)
            Users << QString::fromStdString(Disp) + "  (declares the same KEY)";
    }
    tell("Find usages",
        Users.isEmpty() ? QString("Nothing in this bundle uses %1.").arg(QString::fromStdString(Token))
                        : QString("%1 is used by:\n\n").arg(QString::fromStdString(Token)) + Users.join("\n"));
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
                 ? QString("That file only DELETES registry entries (%1). A RegEdit layer can only write, so "
                           "there is nothing to import.").arg(Deletions)
                 : QString("No keys or values found in that file."));
        return;
    }

    //Re-resolved AFTER the modal: the node can be deleted (or the document reloaded) while the file dialog is
    //open, and ["NODES"][-1] is an unchecked size_t index.
    const int I = indexOf(NodeId);
    if (I < 0) { tell("Import .reg", "That node is no longer in the package."); return; }
    json & N = Model->doc()["NODES"][I];
    //A malformed EDITS is REFUSED, not replaced. The canvas already refuses to overwrite a hand-edited
    //`"EDITS": {"HKLM": {...}}` from its "+ group" button — this path would have thrown the same registry tree
    //away on one click and saved the loss, which is the harder failure to notice because the author reaches
    //for Import precisely when the node is in a state they are trying to repair. Absent, null and empty are
    //still materialised: those are "nothing to lose", not "something of the wrong shape".
    if (N.contains("EDITS") && !N["EDITS"].is_null() && !N["EDITS"].is_array())
    {
        tell("Import .reg",
             "This node's EDITS is not a list, so importing would replace it and lose what is there.\n\n"
             "Fix it in the JSON view first.");
        return;
    }
    if (!N.contains("EDITS") || !N["EDITS"].is_array() || N["EDITS"].empty())
        //BOTH views, not 32 alone: a 64-bit-only import silently landing in the 32-bit view is the
        //silent-nothing this codebase's audit exists to catch, and the author has no way to see it.
        //Narrowing it afterwards is one checkbox; discovering it was wrong is a debugging session.
        N["EDITS"] = json::array({json::object({{"ARCHITECTURE", json::array({"32", "64"})}})});
    //...and the ENTRY, one index deeper. RegRowsOf reads nothing out of a non-object and RegRowsInto then
    //iterates it as one, so `"EDITS": ["x"]` — the exact shape drawRegEdits refuses to touch, printing
    //"(malformed entry - fix it in the JSON view)" — came back as {"": "x", "HKLM": {...}} and was saved. A
    //refusal that stops at the container and reshapes what is inside it is not a refusal.
    //Nothing to lose, so materialised — but materialised INTO THE SAME THING the container rule below writes,
    //views and all. `json::object()` here was not "the same rule": NodeLower turns a missing ARCHITECTURE into
    //a single un-redirected view, so the import would have landed in one view instead of both and a 64-bit
    //game would read nothing from a node that validates clean. That is the silent-nothing the comment under
    //the container rule cites as the reason for writing both views in the first place.
    if (N["EDITS"][0].is_null()) N["EDITS"][0] = json::object({{"ARCHITECTURE", json::array({"32", "64"})}});
    if (!N["EDITS"][0].is_object())
    {
        tell("Import .reg",
             "This node's first EDITS entry is not an object, so importing would rewrite it into one.\n\n"
             "Fix it in the JSON view first.");
        return;
    }
    json & Entry = N["EDITS"][0];
    std::vector<PkgGraph::RegRow> Merged = PkgGraph::RegRowsOf(Entry);
    Merged.insert(Merged.end(), Rows.begin(), Rows.end());
    PkgGraph::RegRowsInto(Entry, Merged);
    Model->SaveNodes(); Model->requestReload();
    tell("Import .reg",
         QString("Imported %1 key/value row(s).").arg((int)Rows.size())
             + (Deletions > 0 ? QString("\n\nSkipped %1 DELETION(s) - a RegEdit layer only writes.").arg(Deletions)
                              : QString()));
}

// ---------------------------------------------------------------------------
// Content: delta <-> full
// ---------------------------------------------------------------------------

//The Content PARENT whose bytes this node can be diffed against, with its mount target. "" if there is none —
//which is why the "-> delta" button only appears when one exists.
static bool DeltaBaseOf(const json & Nodes, int Index, std::string & BasePath, std::string & BaseTarget)
{
    const json & N = Nodes[Index];
    if (!N.contains("PARENTS") || !N["PARENTS"].is_array()) return false;
    for (const auto & P : N["PARENTS"])
    {
        if (!P.is_string()) continue;
        for (const auto & C : Nodes)
        {
            if (StrOf(C, "CID") != P.get<std::string>()) continue;   // PARENTS ref is a CID → match the handle
            if (StrOf(C, "TYPE") != "Content") continue;
            if (StrOf(C, "FORM") != "zip") continue;
            BasePath   = StrOf(C, "PATH");
            BaseTarget = C.value("TARGET", std::string());
            return !BasePath.empty();
        }
    }
    return false;
}


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
    const json & Ns = Model->doc()["NODES"];
    const std::string Path = StrOf(Ns[I], "PATH");
    if (Path.empty()) { tell("Undelta", "This layer has no PATH yet."); return; }

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
    { tell("Unsafe path", "This layer's PATH does not point inside the package folder. Nothing was changed."); return; }

    // Resolve everything the reconstruction needs BEFORE going off-thread. reconstructDelta used to read the
    // model from the worker — for as long as a multi-GB stream takes — so closing the editor freed the
    // document under it, and any edit that grew the NODES array reallocated it mid-read.
    //A MULTI-BASE delta reconstructs against the CONCATENATION of several mounted targets, which exists only
    //inside a live mount — there is no single parent archive to hand DeltaByteSource here. Say that, rather than
    //composing it over one base and reporting the resulting hash mismatch as "could not be composed".
    if (ManifestModel::LayerBaseTargets(Ns[I]).size() > 1)
    { tell("Undelta", "This delta is based on several targets at once (a concatenation), which only exists inside "
                      "a mounted runtime. Reconstructing it from the package folder is not possible."); return; }

    std::string BasePath, BaseTarget;
    if (!DeltaBaseOf(Ns, I, BasePath, BaseTarget))
    { tell("Undelta", "This delta has no Content parent to reconstruct against."); return; }

    Canvas->beginAction(NodeId, "reconstructing...", /*cancellable=*/false);   // a stream with no interrupt point
    auto Ok  = std::make_shared<bool>(false);
    auto Err = std::make_shared<std::string>();
    auto Alive = Alive_;
    const std::filesystem::path BaseFile = ResolveInBundle(Bundle, BasePath);
    if (BaseFile.empty())
    { tell("Unsafe path", "The base layer's PATH does not point inside the package folder."); return; }
    AsyncWork::Run(this,
        [BaseFile, DeltaFile, Out, Ok, Err]() { *Ok = ReconstructTo(BaseFile, DeltaFile, Out, *Err); },
        [this, NodeId, ZipName, Out, DeltaFile, Ok, Err, Alive]() {
            if (!Alive->load()) return;
            Canvas->endAction(NodeId);
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
            json & N = Model->doc()["NODES"][J];
            N["FORM"] = "zip";
            N["PATH"] = ZipName;
            N.erase("BASE_TARGETS");                      // a full archive has no byte-base
            Model->SaveNodes(); Model->requestReload();
            refreshHints();
        });
}

void PkgActions::makeDelta(const std::string & NodeId)
{
    const int I = indexOf(NodeId);
    const json & Ns = Model->doc()["NODES"];
    const std::string TgtPath   = StrOf(Ns[I], "PATH");
    const std::string TgtTarget = Ns[I].value("TARGET", std::string());
    std::string BasePath, BaseTarget;
    if (!DeltaBaseOf(Ns, I, BasePath, BaseTarget) || TgtPath.empty())
    { tell("Make delta", "This node needs a Content parent with a zip to diff against."); return; }

    const std::filesystem::path Bundle = Model->packageDir()->path().toStdString();
    const std::filesystem::path BaseFile = ResolveInBundle(Bundle, BasePath);
    const std::filesystem::path TgtFile   = ResolveInBundle(Bundle, TgtPath);
    if (BaseFile.empty() || TgtFile.empty())
    { tell("Unsafe path", "A layer's PATH does not point inside the package folder. Nothing was changed."); return; }
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
        [this, NodeId, Blob, TgtFile, BaseTarget, TgtTarget, Ok, ErrText]() {
            Aborts.erase(NodeId);
            Canvas->endAction(NodeId);
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
            json & N = Model->doc()["NODES"][J];
            N["FORM"] = "delta";
            N["PATH"] = Blob;
            // Cross-target: when the byte-base mounts at a DIFFERENT target, name it so the FS can pair them.
            //A base at the ROOT ("") is a real, ordinary base — the wine/runner-build shape. Skipping the key for it
            //left an UNDECLARED base, so the FS looks at the delta's OWN target, finds nothing, and drops the layer.
            if (BaseTarget != TgtTarget) N["BASE_TARGETS"] = json::array({BaseTarget});
            Model->SaveNodes(); Model->requestReload();
        });
}

