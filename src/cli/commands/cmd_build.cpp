// `VidyaGod build …` — the package builder: everything the package editor does to a package, without the editor. A
// command works on the open editor's own document when the editor has that package open (what it changes shows
// there, and its undo history is the editor's); else on a build SESSION the app keeps for that package until `build
// close` (so a series of commands is one edit, with undo); run on its own, a command opens the package for that call,
// and only `--save` keeps what it did. Nothing is written until a save — and a save is a re-mint (PkgDoc).
#include "command.h"
#include "noderefs.h"
#include "cmdutil.h"
#include "mainwindow.h"
#include "packageeditor.h"
#include "packageeditormodel.h"
#include "pkgactions.h"
#include "pkgdoc.h"
#include "pkggraph.h"
#include "pkglayout.h"

#include <QDir>
#include <QFileInfo>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;
namespace fs = std::filesystem;

using CmdUtil::BuildSession;
using Session = CmdUtil::BuildSession;

Session Open(Command::Context &C, const std::string &Pkg) { return CmdUtil::OpenBuild(C, Pkg); }

// A node of the document: its handle, its index, its LABEL (unique), or a unique handle prefix.
int Ix(const PkgDoc::Document &D, const std::string &Q)
{
    if (const int I = D.IndexOf(Q); I >= 0) return I;
    if (!Q.empty() && Q.find_first_not_of("0123456789") == std::string::npos)
    {
        const int I = std::stoi(Q);
        if (I < D.Count()) return I;
    }
    int Found = -1, N = 0;
    for (int I = 0; I < D.Count(); ++I) if (D.Node(I).value("LABEL", std::string()) == Q) { Found = I; ++N; }
    if (N == 1) return Found;
    if (N > 1) throw Command::BadArgs("'" + Q + "' is the LABEL of " + std::to_string(N) + " nodes: name one by its handle");
    for (int I = 0; I < D.Count(); ++I) if (D.Handle(I).rfind(Q, 0) == 0) { Found = I; ++N; }
    if (N == 1) return Found;
    throw Command::BadArgs("the package has no node '" + Q + "' (a handle, index, or LABEL — build show lists them)");
}

std::vector<PkgGraph::NodeRef> Refs(const PkgDoc::Document &D)
{
    std::vector<PkgGraph::NodeRef> R;
    for (int I = 0; I < D.Count(); ++I) R.push_back({D.Handle(I), &D.Node(I)});
    return R;
}

// A JSON option's value (the CLI parsed it already: text, or @file).
const json &JsonArg(const json &A, const char *Key) { return A[Key]; }

// After an edit: one undo step, the editor (if open) repaints and revalidates; with --save, saved; then the state.
json Done(Session &S, const json &A, json Out = json::object())
{
    PackageEditorModel &M = *S.M;
    M.doc().Commit();
    M.notifyNodeChanged();
    if (Command::Bool(A, "save", false))
    {
        QString Err;
        if (M.saveEmptiesPackage() && !Command::Bool(A, "allow-empty", false))
            throw std::runtime_error("saving now leaves the package with no nodes: add --allow-empty if that is meant");
        if (!M.Save(&Err)) throw std::runtime_error(Err.toStdString());
        // The editor tells the app itself (its save signal); a session's save is the app's to pick up here.
        if (std::string(S.Where) == "session") MainWindow::RefreshPackage(M.packagePath());
        Out["saved"] = true;
        Out["renamed"] = M.lastRenames();
        Out["notes"] = M.saveNotes();
    }
    Out["where"] = S.Where;
    Out["dirty"] = M.isDirty();
    if (M.isDirty() && std::string(S.Where) == "this call") Out["note"] = "not saved: add --save (or run the app, whose sessions last)";
    return Out;
}

// A reference by name: its LABEL here, else the outside node's LABEL and package, else the reference itself.
PkgGraph::LabelFn Labeller(PackageEditorModel &M)
{
    return [&M, Labels = M.doc().Labels()](const std::string &R) {
        if (const auto It = Labels.find(R); It != Labels.end() && !It->second.empty()) return It->second;
        const auto X = M.externalInfo(R);
        return X.Label.empty() ? R : X.Label + " (" + X.Package + ")";
    };
}

json Facts(PackageEditorModel &M, int I)
{
    const PkgDoc::Document &D = M.doc();
    const json &N = D.Node(I);
    const auto Docs = Refs(D);
    const auto Facets = PkgGraph::CollectVarFacets(Docs);
    const PkgGraph::LabelFn Label = Labeller(M);
    json Items = json::array();
    const auto Li = PkgGraph::LayerItems(N, Facets, Label);
    for (const auto &L : Li) Items.push_back({{"layer", L.Layer}, {"type", L.Type}, {"title", L.Title}, {"section", L.Section}, {"summary", L.Summary}});
    std::vector<std::string> Hints;
    if (const auto Is = M.issuesByHandle(); Is.count(D.Handle(I))) Hints = Is.at(D.Handle(I));
    const std::string Base = PkgGraph::DeltaBase(Docs, I);
    json Acts = json::array();
    for (const auto &Ac : PkgGraph::ActionsFor(N, !Base.empty(), Hints)) Acts.push_back({{"id", Ac.Id}, {"label", Ac.Label}, {"tip", Ac.Tip}, {"long", Ac.Heavy}});
    json Entries = json::array(), Vars = json::object(), Reg = json::array(), Takes = json::array(), Faults = json::array();
    const json &Layers = N.value("LAYERS", json::array());
    for (size_t K = 0; K < Layers.size(); ++K)
    {
        const json &L = Layers[K];
        if (!L.is_object()) continue;
        if (L.contains("EXEC") && L["EXEC"].is_array())
            for (const auto &E : L["EXEC"])
                Entries.push_back({{"layer", K}, {"title", PkgGraph::EntryTitle(E)}, {"summary", PkgGraph::EntrySummary(E)}});
        if (L.contains("VARS") && L["VARS"].is_object())
            for (const auto &[Key, Decl] : L["VARS"].items()) Vars[Key] = PkgGraph::VarTitle(Key, Decl);
        if (L.contains("REG"))
        {
            json Rows = json::array();
            for (const auto &R : PkgGraph::RegRowsOf(L["REG"])) Rows.push_back({{"path", R.Path}, {"name", R.Name}, {"value", R.Value}, {"key_only", R.KeyOnly}});
            Reg.push_back({{"layer", K}, {"count", PkgGraph::CountRegRows(L["REG"])}, {"rows", Rows}});
        }
        if (L.contains("NODE") && L.contains("TAKE"))
            Takes.push_back({{"layer", K}, {"text", PkgGraph::TakeToText(L["TAKE"])}, {"fault", PkgGraph::TakeFault(&L["TAKE"])}});
        for (const auto &[Key, V] : L.items())
            if (V.is_array() || V.is_string())
                if (const int F = PkgGraph::StringListFault(&V); F >= 0 && V.is_array()) Faults.push_back({{"layer", K}, {"key", Key}, {"bad_item", F}, {"value", PkgGraph::DescribeValue(V)}});
    }
    json Refs = json::array();
    PkgDoc::ForEachRef(N, [&](const std::string &R) { Refs.push_back({{"ref", R}, {"label", Label(R)}, {"cid", PkgDoc::LooksLikeCid(R)}}); });
    return {{"index", I}, {"handle", D.Handle(I)}, {"id", PkgGraph::SafeId(D.Handle(I))}, {"file", D.File(I)}, {"kind", PkgGraph::KindOf(N)},
            {"types", PkgGraph::LayerTypes(N)}, {"describe", PkgGraph::Describe(N, Label)},
            {"content", {{"layer", PkgGraph::ContentIndex(N)}, {"type", PkgGraph::ContentType(N)}, {"name", PkgGraph::ContentName(N)},
                         {"target", PkgGraph::ContentTarget(N)}, {"delta_base", Base}}},
            {"rows", Items}, {"top_level_rows", PkgGraph::TopLevelRows(Li)}, {"actions", Acts}, {"entries", Entries}, {"variables", Vars},
            {"registry", Reg}, {"takes", Takes}, {"faults", Faults}, {"refs", Refs}, {"issues", Hints}, {"node", N}};
}

const char *KindName(PkgGraph::FieldKind K)
{
    switch (K)
    {
    case PkgGraph::FieldKind::Text: return "text";
    case PkgGraph::FieldKind::Enum: return "enum";
    case PkgGraph::FieldKind::Check: return "check";
    case PkgGraph::FieldKind::StringList: return "lines";
    case PkgGraph::FieldKind::StringListKeepEmpty: return "lines (blank kept)";
    case PkgGraph::FieldKind::KeyValue: return "key=value";
    case PkgGraph::FieldKind::ObjArray: return "entries";
    case PkgGraph::FieldKind::RegTree: return "registry";
    case PkgGraph::FieldKind::Arch: return "arch";
    case PkgGraph::FieldKind::TakeList: return "take";
    case PkgGraph::FieldKind::VarMap: return "variables";
    case PkgGraph::FieldKind::KeepMap: return "keep";
    case PkgGraph::FieldKind::Cover: return "cover";
    case PkgGraph::FieldKind::Object: return "object";
    }
    return "?";
}

json Fields(const std::vector<PkgGraph::Field> &Fs, const json *Entry)
{
    json Out = json::array();
    for (const auto &F : Fs)
    {
        json O = {{"key", F.Key}, {"label", F.Label}, {"kind", KindName(F.Kind)}, {"hint", F.Hint}};
        if (!F.Options.empty()) { json Opts = json::object(); for (const auto &[V, L] : F.Options) Opts[V] = L; O["options"] = Opts; }
        if (!F.Sub.empty()) O["fields"] = Fields(F.Sub, nullptr);
        if (*F.OnlyModes) O["only_modes"] = F.OnlyModes;
        if (Entry) O["applies"] = PkgGraph::FieldApplies(F, *Entry);
        Out.push_back(O);
    }
    return Out;
}

Command::Arg PkgArg() { return Command::Pos("package", "the package: a folder path, its name, its UID, or part of its title"); }
Command::Arg NodeArg(const char *Name = "node") { return Command::Pos(Name, "a node of the package: its handle, index, or LABEL"); }
Command::Arg SaveFlag() { return Command::Flag("save", "save afterwards (a re-mint)"); }

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("build", "the package builder: a package's nodes, layers and wires, undo, validate, save");

    // ---------------- sessions and the document ----------------
    Command::Register({
        "build open", "open a package for building (in the app: a session that lasts until `build close`)",
        "Load a package's nodes into a build session — the same document the package editor edits, with its undo "
        "history. With the editor open on the package, its document is used. Prints what was loaded.", { PkgArg() },
        { "build open 749", "build open 'Age of Empires II'" },
        { "PackageEditorModel::initPackage", "PackageEditorModel::LoadNodes", "PkgDoc::Document::Load", "PackageEditor::live", "PackageEditor::model",
          "PackageEditor::bundleDir" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            return json{{"package", S.M->packagePath().toStdString()}, {"nodes", S.M->doc().Count()}, {"where", S.Where},
                        {"unparseable", S.M->doc().Unparseable()}};
        }, {} });

    Command::Register({
        "build close", "end a build session (unsaved edits go: --discard to confirm when there are some)",
        "Drop the app's session for a package. A session with unsaved edits needs --discard.", { PkgArg(), Flag("discard", "drop unsaved edits") },
        { "build close 749", "build close 749 --discard" }, { "PackageEditorModel::isDirty" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            const std::string P = CmdUtil::Canon(PackageDir(C, Command::Str(A, "package"), false));
            auto &K = CmdUtil::BuildSessions();
            const auto It = K.find(P);
            if (It == K.end()) return json{{"closed", false}, {"why", "no session for it"}};
            if (It->second->isDirty() && !Command::Bool(A, "discard", false)) throw Command::BadArgs("the session has unsaved edits: build save, or --discard");
            K.erase(It);
            return json{{"closed", true}};
        }, {} });

    Command::Register({
        "build sessions", "the packages being built: the editor's and the app's sessions", "Each with its node count and whether it has unsaved edits.",
        {}, { "build sessions" }, {}, Needs::Model, false,
        [](Command::Context &, const json &) {
            json Out = json::array();
            if (PackageEditor *E = PackageEditor::live(); E && E->model())
                Out.push_back({{"package", E->bundleDir().toStdString()}, {"where", "editor"}, {"nodes", E->model()->doc().Count()}, {"dirty", E->model()->isDirty()}});
            for (const auto &[P, M] : CmdUtil::BuildSessions()) Out.push_back({{"package", P}, {"where", "session"}, {"nodes", M->doc().Count()}, {"dirty", M->isDirty()}});
            return Out;
        }, {} });

    Command::Register({
        "build show", "the package as the builder holds it: nodes, state, history, validation",
        "Every node (index, handle, LABEL, kind, file), the document's revisions, unsaved edits, the undo / redo steps "
        "in words, the game's ID, files no node names, the crash draft, the last validation, the last save's renames "
        "and notes, and this machine's canvas positions.", { PkgArg() }, { "build show 749", "build show 749 --json" },
        { "PkgDoc::Document::Count", "PkgDoc::Document::Handle", "PkgDoc::Document::Node", "PkgDoc::Document::File", "PkgDoc::Document::Labels",
          "PkgDoc::Document::Revision", "PkgDoc::Document::ContentRevision", "PkgDoc::Document::Dirty", "PkgDoc::Document::Pending",
          "PkgDoc::Document::CanUndo", "PkgDoc::Document::CanRedo", "PkgDoc::Document::Steps", "PkgDoc::Document::Positions",
          "PkgDoc::Document::Unparseable", "PackageEditorModel::doc", "PackageEditorModel::packagePath", "PackageEditorModel::packageDir",
          "PackageEditorModel::gameId", "PackageEditorModel::unusedFiles", "PackageEditorModel::draftTime", "PackageEditorModel::validated",
          "PackageEditorModel::validating", "PackageEditorModel::validationErrors", "PackageEditorModel::validationWarnings",
          "PackageEditorModel::lastRenames", "PackageEditorModel::saveNotes", "PackageEditorModel::isDirty", "PackageEditorModel::globalConfig",
          "PkgGraph::KindOf" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PackageEditorModel &M = *S.M;
            const PkgDoc::Document &D = M.doc();
            json Nodes = json::array();
            for (int I = 0; I < D.Count(); ++I)
                Nodes.push_back({{"index", I}, {"handle", D.Handle(I)}, {"label", D.Node(I).value("LABEL", std::string())}, {"kind", PkgGraph::KindOf(D.Node(I))},
                                 {"variant", D.Node(I).value("VARIANT", std::string())}, {"file", fs::path(D.File(I)).filename().string()}});
            json Pos = json::object();
            for (const auto &[H, P] : D.Positions()) Pos[H] = {P.X, P.Y};
            QStringList Unused = M.unusedFiles();
            json Un = json::array();
            for (const QString &U : Unused) Un.push_back(U.toStdString());
            const auto H = D.Steps();
            return json{{"package", M.packagePath().toStdString()}, {"dir_exists", M.packageDir() && M.packageDir()->exists()},
                        {"config", M.globalConfig() == C.Config}, {"where", S.Where}, {"game_id", M.gameId()}, {"nodes", Nodes},
                        {"revision", D.Revision()}, {"content_revision", D.ContentRevision()}, {"dirty", D.Dirty() && M.isDirty()},
                        {"uncommitted", D.Pending()}, {"can_undo", D.CanUndo()}, {"can_redo", D.CanRedo()}, {"done", H.Done}, {"undone", H.Undone},
                        {"unparseable", D.Unparseable()}, {"unused_files", Un}, {"draft_time", M.draftTime()},
                        {"validated", M.validated()}, {"validating", M.validating()}, {"errors", M.validationErrors()},
                        {"warnings", M.validationWarnings()}, {"last_renames", M.lastRenames()}, {"save_notes", M.saveNotes()},
                        {"labels", D.Labels()}, {"positions", Pos}};
        }, {} });

    Command::Register({
        "build node", "one node as the editor sees it: rows, entries, variables, registry, wires, actions",
        "A node's kind and one-line description, its content layer (file, type, target, the delta it could become), "
        "each layer as the node's rows show it (title, section, summary), its EXEC entries and VARS declarations "
        "titled, its registry rows, its TAKE selections as text, malformed lists, the references it makes (named), "
        "the actions the canvas offers it, and its validation issues.", { PkgArg(), NodeArg() },
        { "build node 749 'Patch 2.0a'", "build node 749 0 --json" },
        { "PkgDoc::Document::IndexOf", "PkgDoc::ForEachRef", "PkgDoc::LooksLikeCid", "PkgGraph::SafeId", "PkgGraph::LayerTypes", "PkgGraph::Describe",
          "PkgGraph::ContentIndex", "PkgGraph::ContentType", "PkgGraph::ContentName", "PkgGraph::ContentTarget", "PkgGraph::DeltaBase",
          "PkgGraph::CollectVarFacets", "PkgGraph::LayerItems", "PkgGraph::TopLevelRows", "PkgGraph::ActionsFor", "PkgGraph::EntryTitle",
          "PkgGraph::EntrySummary", "PkgGraph::VarTitle", "PkgGraph::RegRowsOf", "PkgGraph::CountRegRows", "PkgGraph::TakeToText",
          "PkgGraph::TakeFault", "PkgGraph::StringListFault", "PkgGraph::DescribeValue", "PackageEditorModel::issuesByHandle",
          "PackageEditorModel::externalInfo" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            return Facts(*S.M, Ix(S.M->doc(), Command::Str(A, "node")));
        }, {} });

    Command::Register({
        "build graph", "the package's wiring as the canvas lays it out, its fold order, its card",
        "The canvas's graph: each node (kind, content form, position — declared or computed by the layout), every "
        "wire (contains / requires / excludes, to a node here or another package's), wire crossings; with --root, "
        "the order a resolve folds that node's layers in, and the shelf card it makes; and the next step the editor "
        "suggests.", { PkgArg(), Opt("root", "a node: its fold order and card"), Flag("relayout", "compute every position afresh") },
        { "build graph 749", "build graph 749 --root 'Patch 2.0a'" },
        { "PkgGraph::Build", "PkgLayout::Compute", "PkgLayout::CountCrossings", "PkgGraph::FoldOrder", "PkgGraph::CardFor", "PkgGraph::NextStep",
          "PackageEditorModel::libraryNode" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PackageEditorModel &M = *S.M;
            const PkgDoc::Document &D = M.doc();
            PkgGraph::Graph G = PkgGraph::Build(Refs(D));
            for (auto &N : G.Nodes) if (const auto It = D.Positions().find(N.Id); It != D.Positions().end()) { N.X = It->second.X; N.Y = It->second.Y; N.HasPos = true; }
            if (Command::Bool(A, "relayout", false)) PkgLayout::Compute(G);
            json Ns = json::array(), Ls = json::array();
            for (const auto &N : G.Nodes) Ns.push_back({{"index", N.Index}, {"handle", N.Id}, {"kind", N.Type}, {"form", N.Form}, {"x", N.X}, {"y", N.Y}, {"declared", N.HasPos}});
            for (const auto &L : G.Links)
                Ls.push_back({{"child", L.ChildIndex}, {"parent", L.ParentIndex >= 0 ? json(L.ParentIndex) : json(L.ExternalId)},
                              {"as", L.Not ? "excludes" : L.Any ? "requires" : "contains"}, {"layer", L.Layer}, {"member", L.Member}});
            const auto Find = [&](const std::string &H) -> const json * {
                if (const int I = D.IndexOf(H); I >= 0) return &D.Node(I);
                const Node *N = M.libraryNode(H);
                return N ? &*N->Json : nullptr;
            };
            std::vector<const json *> All;
            for (int I = 0; I < D.Count(); ++I) All.push_back(&D.Node(I));
            json Out = {{"nodes", Ns}, {"wires", Ls}, {"external", G.Externals}, {"crossings", PkgLayout::CountCrossings(G)}, {"next_step", PkgGraph::NextStep(All)}};
            if (A.contains("root"))
            {
                const std::string R = D.Handle(Ix(D, Command::Str(A, "root")));
                json Steps = json::array();
                for (const auto &F : PkgGraph::FoldOrder(R, Find, Labeller(M)))
                    Steps.push_back({{"depth", F.Depth}, {"node", F.NodeLabel.empty() ? F.Node : F.NodeLabel}, {"layer", F.Layer}, {"type", F.Type},
                                     {"title", F.Title}, {"elsewhere", F.Outside}, {"takes", F.Take}});
                std::vector<std::string> Hs;
                for (int I = 0; I < D.Count(); ++I) Hs.push_back(D.Handle(I));
                const auto Card = PkgGraph::CardFor(R, Hs, Find);
                json Vs = json::array();
                for (const auto &[V, Ok] : Card.Versions) Vs.push_back({{"version", V}, {"ok", Ok}});
                Out["fold_order"] = Steps;
                Out["card"] = {{"title", Card.Title}, {"uid", Card.Uid}, {"cover", Card.Cover}, {"versions", Vs}};
            }
            return Out;
        }, {} });

    // ---------------- editing nodes ----------------
    Command::Register({
        "build add", "add a node: from JSON, a new game's first node, a version, a mod, or one layer",
        "A new node in the package (a draft until saved): --from as given; --recipe a new game's first node (windows, "
        "dos, console, linux, library) with --title, --uid, --platform; --version-of / --mod-of a node (a new version "
        "of it, or a mod over it); --layer TYPE a node of one empty layer of that type, built on the --on nodes (it "
        "contains them, ahead of its layer). Prints its handle.",
        { PkgArg(), WithSample(Opt("from", "the node (JSON)", Type::Json), "{\"LABEL\":\"x\",\"LAYERS\":[]}"), Opt("recipe", "a new game's first node", Type::String, false, { "windows", "dos", "console", "linux", "library" }),
          Opt("title", "(--recipe) the game's title"), Opt("uid", "(--recipe) its UID"), Opt("platform", "(--recipe) its platform"),
          Opt("version-of", "a node: a new version of it"), Opt("mod-of", "a node: a mod over it"), Opt("layer", "a one-layer node of this type"),
          Opt("on", "(--layer) the nodes it is built on: it contains them (a,b)"), Opt("label", "its LABEL"), SaveFlag() },
        { "build add 749 --layer ZIP --on 'Patch 2.0a' --label 'No-CD'", "build add 749 --version-of 'Patch 2.0a' --save",
          "build add ~/x/new --recipe windows --title 'Some Game' --uid 123" },
        { "PkgDoc::Document::Add", "PkgGraph::RecipeNode", "PkgGraph::VersionNode", "PkgGraph::ModNode", "PkgGraph::NewLayer", "PkgGraph::NewPayload",
          "PackageEditorModel::createNode", "PackageEditorModel::notifyNodeChanged", "PackageEditorModel::noteEdited", "PkgDoc::Document::Commit" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            std::string H;
            const std::string Label = Command::Str(A, "label", "");
            if (A.contains("layer"))
            {
                std::vector<std::string> Parents;
                for (const auto &P : List(A, "on")) Parents.push_back(D.Handle(Ix(D, P)));
                H = S.M->createNode(PkgGraph::NewPayload(Command::Str(A, "layer")), Parents, Label);
                if (H.empty()) throw std::runtime_error("could not make a " + Command::Str(A, "layer") + " node");
                return Done(S, A, {{"added", H}, {"layer", PkgGraph::NewLayer(Command::Str(A, "layer"))}});
            }
            json N;
            if (A.contains("from")) N = JsonArg(A, "from");
            else if (A.contains("recipe"))
            {
                static const std::map<std::string, PkgGraph::Recipe> R = {{"windows", PkgGraph::Recipe::Windows}, {"dos", PkgGraph::Recipe::Dos},
                    {"console", PkgGraph::Recipe::Console}, {"linux", PkgGraph::Recipe::Linux}, {"library", PkgGraph::Recipe::Library}};
                N = PkgGraph::RecipeNode(R.at(Command::Str(A, "recipe")), Command::Str(A, "title", ""), Command::Str(A, "uid", ""), Command::Str(A, "platform", ""));
            }
            else if (A.contains("version-of") || A.contains("mod-of"))
            {
                const bool Ver = A.contains("version-of");
                const int B = Ix(D, Command::Str(A, Ver ? "version-of" : "mod-of"));
                const std::string BL = D.Node(B).value("LABEL", std::string());
                N = Ver ? PkgGraph::VersionNode(D.Handle(B), BL) : PkgGraph::ModNode(D.Handle(B), BL);
            }
            else throw Command::BadArgs("give --from, --recipe, --version-of, --mod-of or --layer");
            if (!Label.empty()) N["LABEL"] = Label;
            const int I = D.Add(N);
            return Done(S, A, {{"added", D.Handle(I)}, {"index", I}});
        }, {} });

    Command::Register({
        "build replace", "replace a node's JSON (what the JSON panel does)",
        "The node becomes exactly --with (one undo step). Its handle stays until the save re-mints it.",
        { PkgArg(), NodeArg(), WithSample(Opt("with", "the node it becomes (JSON)", Type::Json, true), "{\"LABEL\":\"x\",\"LAYERS\":[]}"), SaveFlag() },
        { "build replace 749 'Patch 2.0a' --with @node.json" }, { "PackageEditorModel::replaceNode", "PkgDoc::Document::Replace" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            const std::string H = S.M->doc().Handle(Ix(S.M->doc(), Command::Str(A, "node")));
            S.M->replaceNode(H, JsonArg(A, "with"));
            return Done(S, A, {{"replaced", H}});
        }, {} });

    Command::Register({
        "build set", "set (or remove) one value inside a node, by JSON pointer",
        "Write --value (JSON) at a JSON pointer in the node (\"/LAYERS/2/TARGET\", \"/VARIANT\"); --remove deletes it. "
        "With --sub, write Node[<pointer key>][<sub>] the guarded way the editor's forms do (a key that holds no "
        "object is not overwritten).",
        { PkgArg(), NodeArg(), Pos("pointer", "a JSON pointer (or, with --sub, a top-level key)"), WithSample(Opt("value", "the value (JSON)", Type::Json), "\"text\", 3, [..] or {..}"),
          Flag("remove", "remove it"), Opt("sub", "the sub-key to write"), SaveFlag() },
        { "build set 749 'Patch 2.0a' /VARIANT --value '\"Patch 2.0a (No-CD)\"'", "build set 749 0 TILE --sub TITLE --value '\"AoE2\"'" },
        { "PkgDoc::Document::Replace", "PkgGraph::WriteSubKey", "PkgGraph::WritableObject" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int I = Ix(D, Command::Str(A, "node"));
            json N = D.Node(I);
            const std::string P = Command::Str(A, "pointer");
            if (A.contains("sub"))
            {
                if (!PkgGraph::WritableObject(N, P.c_str())) throw std::runtime_error("'" + P + "' holds something that is not an object");
                if (!PkgGraph::WriteSubKey(N, P.c_str(), Command::Str(A, "sub"), JsonArg(A, "value"))) throw std::runtime_error("could not write it");
            }
            else
            {
                const json::json_pointer Ptr(P);
                if (Command::Bool(A, "remove", false))
                {
                    if (!N.contains(Ptr)) throw Command::BadArgs("the node has nothing at " + P);
                    N[Ptr.parent_pointer()].erase(Ptr.back());
                }
                else N[Ptr] = JsonArg(A, "value");
            }
            D.Replace(I, N);
            return Done(S, A, {{"node", N}});
        }, {} });

    Command::Register({
        "build remove", "delete a node (and every wire to it)", "The node, every reference to it from the package's nodes, and its position.",
        { PkgArg(), NodeArg(), SaveFlag(), Flag("allow-empty", "(--save) even if the package is left with no nodes") },
        { "build remove 749 'Patch 1.0c'" }, { "PkgDoc::Document::Remove", "PackageEditorModel::saveEmptiesPackage" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int I = Ix(D, Command::Str(A, "node"));
            const std::string H = D.Handle(I);
            if (!D.Remove(I)) throw std::runtime_error("could not remove it");
            return Done(S, A, {{"removed", H}});
        }, {} });

    Command::Register({
        "build link", "wire a node into another: contains (NODE), requires (ANY), excludes (NOT)",
        "Child gets a reference to parent: by default containment (a NODE layer); --any a requirement, --not an "
        "exclusion; --at the layer to put it before. Parent may be a node of another package (its CID).",
        { PkgArg(), NodeArg("child"), Pos("parent", "a node here (handle, index, LABEL) or another package's CID"), Flag("any", "a requirement (ANY)"),
          Flag("not", "an exclusion (NOT)"), Opt("at", "before this layer", Type::Int), SaveFlag() },
        { "build link 749 'Patch 2.0a' 'The Age of Kings'", "build link 749 'UserPatch 1.5' 'Patch 2.0a' --any" }, { "PkgDoc::Document::Link" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int Ch = Ix(D, Command::Str(A, "child"));
            const std::string PQ = Command::Str(A, "parent");
            std::string P = PQ;
            if (!PkgDoc::LooksLikeCid(PQ) || D.IndexOf(PQ) >= 0) P = D.Handle(Ix(D, PQ));
            const auto K = Command::Bool(A, "not", false) ? PkgDoc::Document::RefKind::Not
                         : Command::Bool(A, "any", false) ? PkgDoc::Document::RefKind::Any : PkgDoc::Document::RefKind::Node;
            if (!D.Link(Ch, P, K, static_cast<int>(Command::Int(A, "at", -1)))) throw std::runtime_error("could not link them (already linked, or a cycle)");
            return Done(S, A, {{"child", D.Handle(Ch)}, {"parent", P}});
        }, {} });

    Command::Register({
        "build unlink", "remove a node's reference: to one node everywhere, or one reference slot",
        "--ref drops every reference the node makes to that node; --slot drops the reference in that slot (as the canvas "
        "numbers a node's wires).",
        { PkgArg(), NodeArg(), Opt("ref", "the referenced node (handle, index, LABEL, or a CID)"), Opt("slot", "the wire slot", Type::Int), SaveFlag() },
        { "build unlink 749 'Patch 2.0a' --ref 'The Conquerors'" }, { "NodeRefs::Remove", "PkgGraph::EraseRef", "NodeRefs::Names" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int I = Ix(D, Command::Str(A, "node"));
            json N = D.Node(I);
            bool Ok = false;
            json Layers = json::array();
            if (A.contains("ref"))
            {
                const std::string Q = Command::Str(A, "ref");
                const std::string R = PkgDoc::LooksLikeCid(Q) && D.IndexOf(Q) < 0 ? Q : D.Handle(Ix(D, Q));
                for (size_t K = 0; K < N.value("LAYERS", json::array()).size(); ++K) if (NodeRefs::Names(N["LAYERS"][K], R)) Layers.push_back(K);
                Ok = NodeRefs::Remove(N, R);
            }
            else if (A.contains("slot")) Ok = PkgGraph::EraseRef(N, static_cast<int>(Command::Int(A, "slot")));
            else throw Command::BadArgs("give --ref or --slot");
            if (!Ok) throw std::runtime_error("the node has no such reference");
            D.Replace(I, N);
            return Done(S, A, {{"unlinked", true}, {"layers", Layers}});
        }, {} });

    Command::Register({
        "build merge", "merge one node into another (its layers appended, its wires re-pointed)",
        "From's layers go to the end of Into, every reference to From now names Into, and From is removed. Refused when "
        "From contains Into.", { PkgArg(), NodeArg("from"), NodeArg("into"), SaveFlag() }, { "build merge 749 'Soundtrack' 'The Age of Kings'" },
        { "PkgDoc::Document::MergeInto" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            std::string Err;
            const int F = Ix(D, Command::Str(A, "from")), I = Ix(D, Command::Str(A, "into"));
            const std::string Into = D.Handle(I);
            if (!D.MergeInto(F, I, &Err)) throw std::runtime_error(Err);
            return Done(S, A, {{"into", Into}});
        }, {} });

    Command::Register({
        "build layer", "add, move, or carry a layer: within a node, to a position, or into another node",
        "--add TYPE appends an empty layer of that type (with --ref, a reference layer — NODE / ANY / NOT — naming that "
        "node, before --at); --move I --by N shifts layer I; --move I --to J puts it at J; --move I --into NODE carries "
        "it into another node (before --at; --copy keeps it here too).",
        { PkgArg(), NodeArg(), Opt("add", "a layer type to add"), Opt("ref", "(--add NODE/ANY/NOT) the node it names"), Opt("move", "the layer to move", Type::Int),
          Opt("by", "places to move by (±)", Type::Int), Opt("to", "the position to move to", Type::Int), Opt("into", "a node to carry it into"),
          Opt("at", "the position there", Type::Int), Flag("copy", "(--into) copy instead of move"), SaveFlag() },
        { "build layer 749 'Patch 2.0a' --add REG", "build layer 749 'Patch 2.0a' --move 3 --by -1", "build layer 749 0 --move 2 --into 'The Conquerors' --copy" },
        { "PkgGraph::AddLayer", "NodeRefs::Insert", "NodeRefs::Add", "PkgGraph::MoveLayer", "PkgGraph::MoveLayerTo", "PkgGraph::MoveLayerAcross" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int I = Ix(D, Command::Str(A, "node"));
            json N = D.Node(I);
            bool Ok = false;
            if (A.contains("add"))
            {
                const std::string T = Command::Str(A, "add");
                if (A.contains("ref"))
                {
                    const std::string Q = Command::Str(A, "ref");
                    const std::string R = PkgDoc::LooksLikeCid(Q) && D.IndexOf(Q) < 0 ? Q : D.Handle(Ix(D, Q));
                    Ok = A.contains("at") ? NodeRefs::Insert(N, T, R, static_cast<int>(Command::Int(A, "at"))) : (T == "NODE" ? NodeRefs::Add(N, R)
                                                                                                                  : NodeRefs::Insert(N, T, R, -1));
                }
                else Ok = PkgGraph::AddLayer(N, T);
                if (!Ok) throw std::runtime_error("could not add a " + T + " layer here");
                D.Replace(I, N);
            }
            else if (A.contains("move"))
            {
                const int L = static_cast<int>(Command::Int(A, "move"));
                if (A.contains("into"))
                {
                    const int J = Ix(D, Command::Str(A, "into"));
                    json Dst = D.Node(J);
                    Ok = PkgGraph::MoveLayerAcross(N, L, Dst, D.Handle(J), static_cast<int>(Command::Int(A, "at", -1)), Command::Bool(A, "copy", false));
                    if (!Ok) throw std::runtime_error("could not carry that layer there");
                    D.Replace(J, Dst);
                    D.Replace(I, N);
                }
                else
                {
                    Ok = A.contains("to") ? PkgGraph::MoveLayerTo(N, L, static_cast<int>(Command::Int(A, "to")))
                                          : PkgGraph::MoveLayer(N, L, static_cast<int>(Command::Int(A, "by", 0)));
                    if (!Ok) throw std::runtime_error("could not move that layer there");
                    D.Replace(I, N);
                }
            }
            else throw Command::BadArgs("give --add or --move");
            return Done(S, A, {{"layers", PkgGraph::LayerTypes(D.Node(I))}});
        }, {} });

    Command::Register({
        "build field", "edit a layer the way its form does: registry rows, a TAKE as text, the variables' visibility, keep apart",
        "--reg ROWS (JSON [{path, name, value}]) rebuilds a REG layer's hive from rows; --take TEXT sets a NODE layer's "
        "TAKE from one selection per line (\"X => Y\" renames); --vars-visible on|off shows or hides the node's "
        "variables in the launcher; --keep-apart ADDRESS gives a KEEP address its own name.",
        { PkgArg(), NodeArg(), Opt("layer", "the layer (for --reg, --take)", Type::Int), WithSample(Opt("reg", "registry rows (JSON)", Type::Json), "[{\"path\":\"HKLM\\\\Software\\\\X\",\"name\":\"V\",\"value\":\"1\"}]"), Opt("take", "the TAKE as text"),
          Opt("vars-visible", "on or off", Type::String, false, { "on", "off" }), Opt("keep-apart", "a KEEP address"), SaveFlag() },
        { "build field 749 'Patch 2.0a' --layer 2 --reg '[{\"path\":\"HKLM\\\\Software\\\\X\",\"name\":\"V\",\"value\":\"1\"}]'",
          "build field 749 'Patch 2.0a' --layer 0 --take 'FILES/a/\nFILES/b => FILES/c'" },
        { "PkgGraph::RegRowsInto", "PkgGraph::TextToTake", "PkgGraph::SetVarVisible", "PkgGraph::NameKeepApart" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            const int I = Ix(D, Command::Str(A, "node"));
            json N = D.Node(I);
            const auto LayerAt = [&]() -> json & {
                const long long L = Command::Int(A, "layer", -1);
                if (L < 0 || !N.contains("LAYERS") || static_cast<size_t>(L) >= N["LAYERS"].size()) throw Command::BadArgs("--layer: no such layer");
                return N["LAYERS"][static_cast<size_t>(L)];
            };
            if (A.contains("reg"))
            {
                std::vector<PkgGraph::RegRow> Rows;
                for (const auto &R : JsonArg(A, "reg"))
                {
                    PkgGraph::RegRow X;
                    X.Path = R.value("path", std::string());
                    X.Name = R.value("name", std::string());
                    X.Value = R.value("value", std::string());
                    X.KeyOnly = R.value("key_only", false);
                    Rows.push_back(X);
                }
                {
                    json &L = LayerAt();
                    if (!L.contains("REG")) throw Command::BadArgs("--reg: that layer is not a REG layer");
                    PkgGraph::RegRowsInto(L["REG"], Rows);
                }
            }
            if (A.contains("take")) LayerAt()["TAKE"] = PkgGraph::TextToTake(Command::Str(A, "take"));
            if (A.contains("vars-visible")) PkgGraph::SetVarVisible(N, Command::Str(A, "vars-visible") == "on");
            if (A.contains("keep-apart") && !PkgGraph::NameKeepApart(N, Command::Str(A, "keep-apart"))) throw std::runtime_error("no such KEEP address");
            D.Replace(I, N);
            return Done(S, A, {{"node", N}});
        }, {} });

    Command::Register({
        "build uid", "change a UID: one node's tiles (--node), or the whole game's ID",
        "With --node, rename UID old → new in that node's tiles, nesting and RECOMMENDED. Without, change the game's ID: "
        "every node follows, the package is saved, its folder renamed ([old] → [new]), and its saves and layout move "
        "with it.", { PkgArg(), Pos("new", "the new UID"), Opt("node", "only this node"), Opt("old", "(--node) the UID it replaces") },
        { "build uid 749 750", "build uid 749 750 --node 'UserPatch 1.5' --old 749" }, { "PackageEditorModel::changeGameId", "PkgGraph::ChangeUid" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            const std::string New = Command::Str(A, "new");
            if (A.contains("node"))
            {
                PkgDoc::Document &D = S.M->doc();
                const int I = Ix(D, Command::Str(A, "node"));
                json N = D.Node(I);
                if (!PkgGraph::ChangeUid(N, Command::Str(A, "old", S.M->gameId()), New)) throw std::runtime_error("the node names no such UID");
                D.Replace(I, N);
                return Done(S, A, {{"node", D.Handle(I)}});
            }
            QString Err;
            if (!S.M->changeGameId(New, &Err)) throw std::runtime_error(Err.toStdString());
            return json{{"game_id", S.M->gameId()}, {"package", S.M->packagePath().toStdString()}};
        }, {} });

    Command::Register({
        "build fill", "put a captured content layer where a node was waiting for its files",
        "A node whose content layer has no file yet (at the same TARGET) takes --layer in that layer's place — what a "
        "capture does.", { PkgArg(), NodeArg(), WithSample(Opt("layer", "the content layer (JSON)", Type::Json, true), "{\"ZIP\":\"AoE2.zip\",\"TARGET\":\"%GameDir%\"}"), SaveFlag() },
        { "build fill 749 'The Age of Kings' --layer '{\"ZIP\":\"AoE2.zip\",\"TARGET\":\"%GameDir%\"}'" }, { "PackageEditorModel::fillWaiting" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            const std::string H = S.M->doc().Handle(Ix(S.M->doc(), Command::Str(A, "node")));
            if (!S.M->fillWaiting(H, JsonArg(A, "layer"))) throw std::runtime_error("the node waits for no content at that target");
            return Done(S, A, {{"filled", H}});
        }, {} });

    // ---------------- positions, history, state ----------------
    Command::Register({
        "build pos", "a node's position on this machine's canvas: set, clear, or lay everything out",
        "--x/--y place a node; --clear forgets its position (the layout places it); --layout computes every node's "
        "position and keeps them; --keep writes this machine's layout to the config now (a save does it too).",
        { PkgArg(), Opt("node", "the node"), Opt("x", "x", Type::Int), Opt("y", "y", Type::Int), Flag("clear", "forget its position"),
          Flag("layout", "lay every node out afresh"), Flag("keep", "write the layout to the config") },
        { "build pos 749 --node 0 --x 100 --y 40", "build pos 749 --layout --keep" },
        { "PkgDoc::Document::SetPos", "PkgDoc::Document::ClearPos", "PkgDoc::Document::SetPositions", "PackageEditorModel::SaveLayout" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PkgDoc::Document &D = S.M->doc();
            if (A.contains("node"))
            {
                const std::string H = D.Handle(Ix(D, Command::Str(A, "node")));
                if (Command::Bool(A, "clear", false)) D.ClearPos(H);
                else D.SetPos(H, PkgDoc::Pos{static_cast<float>(Command::Int(A, "x", 0)), static_cast<float>(Command::Int(A, "y", 0))});
            }
            if (Command::Bool(A, "layout", false))
            {
                PkgGraph::Graph G = PkgGraph::Build(Refs(D));
                PkgLayout::Compute(G);
                std::map<std::string, PkgDoc::Pos> P;
                for (const auto &N : G.Nodes) P[N.Id] = PkgDoc::Pos{N.X, N.Y};
                D.SetPositions(P);
            }
            json Out = Done(S, A);
            if (Command::Bool(A, "keep", false)) { S.M->SaveLayout(); Out["kept"] = true; }
            return Out;
        }, {} });

    Command::Register({
        "build undo", "undo the last step (or several)", "Undo --steps steps (default 1); prints the history after.",
        { PkgArg(), Opt("steps", "how many", Type::Int) }, { "build undo 749", "build undo 749 --steps 3" }, { "PkgDoc::Document::Undo" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            int N = 0;
            for (long long I = 0; I < Command::Int(A, "steps", 1); ++I) N += S.M->doc().Undo() ? 1 : 0;
            S.M->notifyNodeChanged();
            const auto H = S.M->doc().Steps();
            return json{{"undone", N}, {"done", H.Done}, {"can_redo", H.Undone}};
        }, {} });

    Command::Register({
        "build redo", "redo what was undone (or several steps)", "Redo --steps steps (default 1).",
        { PkgArg(), Opt("steps", "how many", Type::Int) }, { "build redo 749" }, { "PkgDoc::Document::Redo" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            int N = 0;
            for (long long I = 0; I < Command::Int(A, "steps", 1); ++I) N += S.M->doc().Redo() ? 1 : 0;
            S.M->notifyNodeChanged();
            const auto H = S.M->doc().Steps();
            return json{{"redone", N}, {"done", H.Done}, {"can_redo", H.Undone}};
        }, {} });

    Command::Register({
        "build reset", "replace the whole document with these nodes (no files, no history)",
        "The document becomes exactly --nodes ([[handle, node], …] or [node, …]): what loading a package from elsewhere "
        "does. Clean until edited; a save writes it.", { PkgArg(), WithSample(Opt("nodes", "the nodes (JSON)", Type::Json, true), "[{\"LABEL\":\"x\",\"LAYERS\":[]}]") },
        { "build reset ~/x/new --nodes '[]'" }, { "PkgDoc::Document::Reset" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            std::vector<std::pair<std::string, json>> N;
            for (const auto &X : JsonArg(A, "nodes"))
                if (X.is_array() && X.size() == 2) N.emplace_back(X[0].get<std::string>(), X[1]);
                else N.emplace_back(std::string(), X);
            S.M->doc().Reset(std::move(N));
            S.M->notifyNodeChanged();
            return json{{"nodes", S.M->doc().Count()}};
        }, {} });

    Command::Register({
        "build save", "save the package: a re-mint that follows renamed nodes through the library",
        "Every changed node gets the CID of its bytes; every node that names it (here, in other packages, in "
        "instances) follows. Prints the renames, what was written, and what needs the author. --allow-empty saves a "
        "package left with no nodes.", { PkgArg(), Flag("allow-empty", "even if no node is left") }, { "build save 749" },
        { "PackageEditorModel::Save", "PkgDoc::Document::Save", "PkgDoc::Document::TakeRenames", "PkgDoc::RemapRefs", "PackageEditorModel::saveEmptiesPackage",
          "PkgCanvas::applyRenames", "MainWindow::RefreshPackage" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            json Args = A;
            Args["save"] = true;
            return Done(S, Args);
        }, {} });

    Command::Register({
        "build reload", "re-read the package from disk (unsaved edits go: --discard)", "", { PkgArg(), Flag("discard", "drop unsaved edits") },
        { "build reload 749 --discard" }, { "PackageEditorModel::LoadNodes" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            if (S.M->isDirty() && !Command::Bool(A, "discard", false)) throw Command::BadArgs("unsaved edits: build save, or --discard");
            S.M->LoadNodes();
            return json{{"nodes", S.M->doc().Count()}};
        }, {} });

    Command::Register({
        "build validate", "check the package as edited: errors, warnings, the node each is about, the fix offered",
        "The validator over the document (unsaved edits included) and the rest of the library; each message with the "
        "node it names and the editor's fix for it.", { PkgArg() }, { "build validate 749" },
        { "PackageEditorModel::Revalidate", "PackageEditorModel::handleInMessage", "PkgGraph::FixFor", "PackageEditorModel::validateNow",
          "PackageEditorModel::validateSoon", "PackageEditorModel::BuildExecIndex" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            PackageEditorModel &M = *S.M;
            M.Revalidate();
            const auto Rows = [&](const std::vector<std::string> &V) {
                json Out = json::array();
                for (const auto &Msg : V)
                {
                    const std::string H = M.handleInMessage(Msg);
                    const int I = H.empty() ? -1 : M.doc().IndexOf(H);
                    const auto F = PkgGraph::FixFor(Msg, I >= 0 ? &M.doc().Node(I) : nullptr);
                    Out.push_back({{"message", Msg}, {"node", H}, {"fix", F.Label}, {"action", F.Action}, {"note", F.Note}, {"waiting", F.Waiting}});
                }
                return Out;
            };
            return json{{"errors", Rows(M.validationErrors())}, {"warnings", Rows(M.validationWarnings())}, {"index_nodes", M.BuildExecIndex().Nodes.size()}};
        }, {} });

    Command::Register({
        "build unused", "files in the package folder no node names (left by a removed layer), or delete them",
        "", { PkgArg(), Flag("delete", "delete them") }, { "build unused 749", "build unused 749 --delete" },
        { "PackageEditorModel::unusedFiles", "PackageEditorModel::deleteUnusedFiles" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            if (Command::Bool(A, "delete", false)) return json{{"deleted", S.M->deleteUnusedFiles()}};
            json Out = json::array();
            for (const QString &F : S.M->unusedFiles()) Out.push_back(F.toStdString());
            return Out;
        }, {} });

    Command::Register({
        "build draft", "the crash draft of unsaved work: write it, restore it, drop it",
        "While a package has unsaved edits the editor writes them now and then to a draft; after a crash, --restore "
        "brings the draft back (one undo step).", { PkgArg(), Flag("write", "write it now"), Flag("restore", "restore it"), Flag("drop", "drop it") },
        { "build draft 749", "build draft 749 --restore" },
        { "PackageEditorModel::writeDraft", "PackageEditorModel::restoreDraft", "PackageEditorModel::dropDraft", "PackageEditorModel::draftTime" },
        Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            json Out = json::object();
            if (Command::Bool(A, "write", false)) Out["written"] = S.M->writeDraft();
            if (Command::Bool(A, "restore", false))
            {
                QString Err;
                if (!S.M->restoreDraft(&Err)) throw std::runtime_error(Err.toStdString());
                Out["restored"] = true;
            }
            if (Command::Bool(A, "drop", false)) { S.M->dropDraft(); Out["dropped"] = true; }
            Out["draft_time"] = S.M->draftTime();
            return Out;
        }, {} });

    Command::Register({
        "build library", "what the rest of the library offers this package: nodes to wire in, platforms, names",
        "Every node another package offers for wiring in (its CID, LABEL, package), the platforms known, this "
        "package's LABELs, and with --cid what one outside node is (its LABEL, package, folder, cover).",
        { PkgArg(), Opt("cid", "an outside node's CID") }, { "build library 749", "build library 749 --cid bafkrei…" },
        { "PackageEditorModel::offers", "PackageEditorModel::KnownPlatforms", "PackageEditorModel::bundleNodeIds", "PackageEditorModel::externalInfo",
          "PackageEditorModel::libraryNode" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            json Offers = json::array();
            for (const auto &O : S.M->offers()) Offers.push_back({{"cid", O.Cid}, {"label", O.Label}, {"package", O.Package}});
            json Out = {{"offers", Offers}, {"platforms", S.M->KnownPlatforms()}, {"labels", S.M->bundleNodeIds()}};
            if (A.contains("cid"))
            {
                const auto X = S.M->externalInfo(Command::Str(A, "cid"));
                Out["node"] = {{"label", X.Label}, {"package", X.Package}, {"dir", X.PackageDir}, {"cover", X.Cover},
                               {"known", S.M->libraryNode(Command::Str(A, "cid")) != nullptr}};
            }
            return Out;
        }, {} });

    Command::Register({
        "build tour", "whether the editor's first-time tour was seen here, or mark it seen", "", { PkgArg(), Flag("seen", "mark it seen") },
        { "build tour 749 --seen" }, { "PackageEditorModel::tourSeen", "PackageEditorModel::markTourSeen" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            if (Command::Bool(A, "seen", false)) S.M->markTourSeen();
            return json{{"seen", S.M->tourSeen()}};
        }, {} });

    Command::Register({
        "build types", "the layer types the builder knows: groups, help, colour, and every field of their forms",
        "Each layer type by group, with its one-line gist, its help, the colour its nodes are drawn in, and (with "
        "--type) the fields its form edits — kind, options, sub-fields, whether each applies to --entry.",
        { Opt("type", "one type: its form's fields"), WithSample(Opt("entry", "(--type) an entry (JSON): which fields apply to it", Type::Json), "{\"EXEC\":{\"HOST\":\"win32\"}}"),
          Opt("platform", "a platform token: in words") },
        { "build types", "build types --type EXEC", "build types --platform win32" },
        { "PkgGraph::TypeGroups", "PkgGraph::TypeGist", "PkgGraph::TypeHelp", "PkgGraph::TypeColour", "PkgGraph::FieldsFor", "PkgGraph::FieldApplies",
          "PkgGraph::PlatformWords", "PkgGraph::LayerType" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            if (A.contains("platform")) return json{{"platform", Command::Str(A, "platform")}, {"words", PkgGraph::PlatformWords(Command::Str(A, "platform"))}};
            if (A.contains("type"))
            {
                const std::string T = Command::Str(A, "type");
                const json E = A.contains("entry") ? JsonArg(A, "entry") : json();
                return json{{"type", T}, {"read_as", PkgGraph::LayerType(json{{T, nullptr}})}, {"gist", PkgGraph::TypeGist(T)}, {"help", PkgGraph::TypeHelp(T)},
                            {"fields", Fields(PkgGraph::FieldsFor(T), A.contains("entry") ? &E : nullptr)}};
            }
            json Out = json::array();
            for (const auto &G : PkgGraph::TypeGroups())
                for (const auto &T : G.Types)
                {
                    int R = 0, Gr = 0, B = 0;
                    PkgGraph::TypeColour(T, R, Gr, B);
                    char Hex[8];
                    std::snprintf(Hex, sizeof Hex, "#%02x%02x%02x", R & 255, Gr & 255, B & 255);
                    Out.push_back({{"group", G.Title}, {"type", T}, {"gist", PkgGraph::TypeGist(T)}, {"colour", Hex}});
                }
            return Out;
        }, {} });

    Command::Register({
        "build sees", "what the game sees when a node runs: every file, which layer gives it, what it covers",
        "Each file the node's fold provides (in fold order, later winning) with the layer it comes from and the layers "
        "it covers; the notes say what is not listed (a patch's rebuilt files).", { PkgArg(), NodeArg(), Opt("file", "only paths containing this") },
        { "build sees 749 'Patch 2.0a'", "build sees 749 'Patch 2.0a' --file empires2.exe" }, { "PkgActions::GameSees" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            const PkgDoc::Document &D = S.M->doc();
            std::vector<std::string> Notes;
            const auto Other = [&S](const std::string &Cid) {
                PkgActions::OtherNode O;
                if (const Node *N = S.M->libraryNode(Cid)) { O.Node = &*N->Json; O.Bundle = N->BundleDir; O.Label = N->NodeId; }
                return O;
            };
            const std::string Only = Command::Str(A, "file", "");
            json Files = json::array();
            for (const auto &F : PkgActions::GameSees(D, D.Handle(Ix(D, Command::Str(A, "node"))), S.M->packagePath().toStdString(), &Notes, Other))
                if (Only.empty() || F.Path.find(Only) != std::string::npos) Files.push_back({{"path", F.Path}, {"from", F.From}, {"covers", F.Under}});
            return json{{"files", Files}, {"notes", Notes}};
        }, {} });

    Command::Register({
        "build compare", "two versions side by side: the layers and files only one has, the files they give differently",
        "", { PkgArg(), NodeArg("a"), NodeArg("b") }, { "build compare 749 'Patch 1.0c' 'Patch 2.0a'" }, { "PkgActions::Compare" }, Needs::Model, false,
        [](Command::Context &C, const json &A) {
            Session S = Open(C, Command::Str(A, "package"));
            const PkgDoc::Document &D = S.M->doc();
            const auto V = PkgActions::Compare(D, D.Handle(Ix(D, Command::Str(A, "a"))), D.Handle(Ix(D, Command::Str(A, "b"))), S.M->packagePath().toStdString());
            return json{{"layers_only_a", V.LayersOnlyA}, {"layers_only_b", V.LayersOnlyB}, {"files_only_a", V.FilesOnlyA}, {"files_only_b", V.FilesOnlyB},
                        {"files_differ", V.FilesDiffer}};
        }, {} });
}
