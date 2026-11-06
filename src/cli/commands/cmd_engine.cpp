// `VidyaGod node …`, `fold …`, `vars …`, `cid …`, `graph …`, `content …` — the format engine on its own: one node as
// authored, lowered and laid out; a resolve and everything it decides; the %TOKEN% language; a node's identity; the
// node graph built, frozen and minted; and where a package keeps its content files.
#include "command.h"
#include "noderefs.h"
#include "nodestore.h"
#include "validate.h"
#include "cmdutil.h"
#include "cid.h"
#include "contentfile.h"
#include "fold.h"
#include "manifestmodel.h"
#include "nodegraph.h"
#include "nodelower.h"
#include "varsubst.h"

#include <fstream>
#include <sstream>

namespace
{
using json = nlohmann::ordered_json;
using namespace CmdUtil;
namespace fs = std::filesystem;

std::string ReadFile(const std::string &P)
{
    std::ifstream In(P, std::ios::binary);
    if (!In) throw Command::BadArgs("cannot read '" + P + "'");
    std::stringstream S;
    S << In.rdbuf();
    return S.str();
}

// NAME=VALUE pairs (comma-separated, or a JSON object) as a variable map.
std::map<std::string, std::string> Pairs(const json &A, const char *Key)
{
    std::map<std::string, std::string> Out;
    if (!A.contains(Key)) return Out;
    if (A[Key].is_object())
    {
        for (const auto &[K, V] : A[Key].items()) Out[K] = V.is_string() ? V.get<std::string>() : V.dump();
        return Out;
    }
    for (const std::string &KV : List(A, Key))
    {
        const auto Eq = KV.find('=');
        if (Eq == std::string::npos) throw Command::BadArgs(std::string("--") + Key + " takes NAME=VALUE pairs, not '" + KV + "'");
        Out[KV.substr(0, Eq)] = KV.substr(Eq + 1);
    }
    return Out;
}

// A JSON option's value (the CLI parsed it already: text, or @file).
const json &JsonArg(const json &A, const char *Key) { return A[Key]; }

// A node's JSON as authored: a node file on disk (its first node), or a node of the library.
json NodeJson(Command::Context &C, const std::string &Q, std::shared_ptr<const NodeIndex> *Keep = nullptr)
{
    std::error_code Ec;
    if (fs::is_regular_file(Q, Ec))
    {
        json J;
        if (!NodeGraph::ReadTreeJsonBounded(Q, J)) throw Command::BadArgs("'" + Q + "' is not a readable node file (too large, too deep, or not JSON)");
        return J.is_array() && !J.empty() ? J.front() : J;
    }
    const auto I = Index(C);
    if (Keep) *Keep = I;
    return NodeOf(*I, Q)->Json;
}

json PlanFacts(const Fold::Plan &P)
{
    json Ev = json::array();
    for (const auto &[K, V] : P.Events) Ev.push_back({{"event", K}, {"node", V}});
    return {{"plan", Fold::PlanToJson(P)}, {"events", Ev}, {"when_vars", P.WhenVars}, {"unsatisfied", Fold::Unsatisfied(P)},
            {"error", P.Error}};
}

Command::Arg NodeArg() { return Command::Pos("node", "a node: its CID, working handle, LABEL, or a node file's path"); }
Command::Arg VarOpt() { return Command::Opt("var", "the instance's values: NAME=VALUE[,…] or a JSON object"); }
Command::Arg BuiltinOpt() { return Command::Opt("builtin", "built-in values (%GameDir%, …): NAME=VALUE[,…]"); }
Command::Arg GraftOpt() { return Command::Opt("grafts", "grafts applied, in order (a,b,c)"); }

} // namespace

VG_COMMANDS
{
    using Command::Type;
    using Command::Needs;
    using Command::Pos;
    using Command::Opt;
    using Command::Flag;
    Command::RegisterGroup("node", "one node: its facts, its schema check, its layers lowered and laid out, its refs");
    Command::RegisterGroup("fold", "a resolve: the plan, its entries and variables, the grafts it takes, the path rules");
    Command::RegisterGroup("vars", "the %TOKEN% language: substitute, render formats, MAP tables, WHEN, EVAL");
    Command::RegisterGroup("cid", "a node's identity: canonical bytes and the CID they get");
    Command::RegisterGroup("graph", "the node graph: build an index, freeze, mint, land, and what the shelf makes of it");
    Command::RegisterGroup("content", "where a package keeps its content files (names, claims, bumped names)");

    // ---------------- node ----------------
    Command::Register({
        "node facts", "what the index derived for a node: entries, runner, faces, its tile",
        "The folded facts of one node (its own resolve with defaults): its key, whether it is a runner, a version on the "
        "shelf, presentable, the card it belongs to, every entry (lowered as the engine runs it, and the host it runs "
        "on), and the tile each of its faces shows.", { NodeArg(), Opt("entry", "one entry's lowered exec (its LABEL)") },
        { "node facts bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka", "node facts 'Patch 2.0a' --entry Play" },
        { "Node::Key", "Node::Presentable", "Node::IsRunner", "Node::IsVariant", "Node::GameKey", "Node::ExecFor", "Node::HostFor",
          "Node::EntrypointLabels", "NodeIndex::Tile" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "node"));
            json Entries = json::object();
            for (const std::string &L : N.EntrypointLabels()) Entries[L.empty() ? "(default)" : L] = {{"host", N.HostFor(L)}, {"exec", N.ExecFor(L)}};
            json Tiles = json::object();
            for (const std::string &U : N.Faces) if (const json *T = I->Tile(U)) Tiles[U] = *T;
            json Out = {{"key", N.Key()}, {"label", N.NodeId}, {"variant", N.Variant}, {"runner", N.IsRunner()}, {"version", N.IsVariant()},
                        {"presentable", N.Presentable()}, {"card", N.GameKey()}, {"uid", N.Uid}, {"family", N.PackageUid},
                        {"host", N.HostPlatform}, {"guests", N.GuestPlatform}, {"entries", Entries}, {"tiles", Tiles}};
            if (A.contains("entry")) Out["exec"] = N.ExecFor(Command::Str(A, "entry"));
            return Out;
        }, {} });

    Command::Register({
        "node check", "check a node against the schema, and its identity",
        "Whether a node is well-formed (one type key per layer, the fields each type allows, well-typed values), its "
        "refs (what it contains, and what it requires: ANY members and NOT refs), its canonical bytes' size and CID. A "
        "file is read the bounded way every library walker reads one (size cap, nesting depth).",
        { NodeArg(), Opt("max-depth", "the nesting depth allowed (default 64)", Type::Int) },
        { "node check ~/.VidyaGod/LIBRARY/VidyaGod/'[749][v1.0] Age of Empires II'/bafkrei….json", "node check 'Patch 2.0a'" },
        { "NodeLower::CheckNode", "ManifestModel::IsNodeObject", "NodeRefs::Contained", "ManifestModel::ParseNode", "Cid::Canonical",
          "Cid::OfNode", "NodeGraph::ReadTreeJsonBounded", "NodeGraph::JsonDepthWithinLimit", "NodeGraph::ParseBounded" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string Q = Command::Str(A, "node");
            const json J = NodeJson(C, Q);
            std::vector<std::string> Req;
            const auto Refs = NodeRefs::Contained(J, &Req);
            std::string Err;
            const std::string Canon = Cid::Canonical(J), Id = Cid::OfNode(J, &Err);
            json Out = {{"node_object", ManifestModel::IsNodeObject(J)}, {"problem", NodeLower::CheckNode(J, Q)}, {"contains", Refs},
                        {"requires", Req}, {"bytes", Canon.size()}, {"cid", Id.empty() ? json(Err) : json(Id)},
                        {"depth_ok", NodeGraph::JsonDepthWithinLimit(J.dump(), static_cast<int>(Command::Int(A, "max-depth", 64)))}};
            std::error_code Ec;
            if (fs::is_regular_file(Q, Ec))
            {
                Node N;
                Out["parses"] = ManifestModel::ParseNode(J, Q, fs::path(Q).parent_path(), N);
            }
            return Out;
        }, {} });

    Command::Register({
        "node lower", "a node's own layers, and one entry, in the engine's vocabulary",
        "The node's OWN layers as the engine's ops (VFSZipLayer, FileEdit, RegEdit, CustomVar, …) where they land "
        "relative to the node, and with --entry the EXEC entry of that LABEL as the exec block the engine runs.",
        { NodeArg(), Opt("entry", "an EXEC entry's LABEL ('' for the first)") }, { "node lower 'Patch 2.0a'", "node lower 'Patch 2.0a' --entry Play" },
        { "NodeLower::LowerNode", "NodeLower::LowerEntry" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string Q = Command::Str(A, "node");
            const json J = NodeJson(C, Q);
            json Out = {{"layers", NodeLower::LowerNode(J, Q)}};
            if (A.contains("entry"))
            {
                const std::string L = Command::Str(A, "entry");
                //EXEC is a list of entries (each named by its LABEL); an object of them (keyed by name) is read too.
                for (const auto &Layer : J.value("LAYERS", json::array()))
                    if (Layer.is_object() && Layer.contains("EXEC") && Layer["EXEC"].is_array())
                    {
                        for (const auto &E : Layer["EXEC"])
                            if (E.is_object() && (L.empty() || E.value("LABEL", std::string()) == L)) { Out["exec"] = NodeLower::LowerEntry(E); return Out; }
                    }
                    else if (Layer.is_object() && Layer.contains("EXEC") && Layer["EXEC"].is_object())
                        for (const auto &[K, E] : Layer["EXEC"].items())
                            if (L.empty() || K == L || (E.is_object() && E.value("LABEL", std::string()) == L)) { Out["exec"] = NodeLower::LowerEntry(E); return Out; }
                throw Command::BadArgs("no EXEC entry '" + L + "' in the node");
            }
            return Out;
        }, {} });

    Command::Register({
        "node layers", "every content layer of a node: how it mounts, where its bytes are",
        "For each of a node's own lowered layers: its type, whether it is mounted content (and as which spec kind), a "
        "runtime path (a %variable% it resolves at launch) or part of a runner's build; its path, the variables in it, "
        "its target normalised, its delta bases; where its bytes live here (the package's file name, bumped when two "
        "CIDs claim it) and its CID; whether a zip is all STOREd; and the mount-spec entry the engine builds from it.",
        { NodeArg() }, { "node layers bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka" },
        { "ManifestModel::LayerType", "ManifestModel::IsVfsLayer", "ManifestModel::VfsSpecType", "ManifestModel::IsRuntimeSourcedLayer",
          "ManifestModel::IsRunnerBuildLayer", "ManifestModel::LayerPathString", "VarSubst::PathTokens", "VarSubst::RuntimeSourced",
          "ManifestModel::NormalizeTargetPath", "ManifestModel::LayerBaseTargets", "ManifestModel::LayerLocator",
          "ManifestModel::ResolveLayerSource", "ManifestModel::ZipFullyStored", "ManifestModel::MakeVfsSpecLayer",
          "ManifestModel::HasLiveToken", "ContentFile::Path" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "node"));
            json Out = json::array();
            for (const auto &L : N.LoweredLayers())
            {
                const std::string T = ManifestModel::LayerType(L);
                json R = {{"type", T}, {"vfs", ManifestModel::IsVfsLayer(T)}};
                if (ManifestModel::IsVfsLayer(T))
                {
                    fs::path Local;
                    std::string Cid;
                    ManifestModel::LayerLocator(L, N.BundleDir, Local, Cid);
                    const std::string P = ManifestModel::LayerPathString(L), Tgt = ManifestModel::NormalizeTargetPath(L.value("TARGET", std::string()));
                    std::string FirstCompressed;
                    const bool Stored = T != "VFSZipLayer" || ManifestModel::ZipFullyStored(Local.string(), &FirstCompressed);
                    R.update({{"spec", ManifestModel::VfsSpecType(T)}, {"path", P}, {"variables", VarSubst::PathTokens(P)},
                              {"runtime", ManifestModel::IsRuntimeSourcedLayer(L)}, {"runner_build", ManifestModel::IsRunnerBuildLayer(L)},
                              {"target", Tgt}, {"bases", ManifestModel::LayerBaseTargets(L)}, {"cid", Cid}, {"local", Local.string()},
                              {"resolved", ManifestModel::ResolveLayerSource(L, N.BundleDir)},
                              {"file", ContentFile::Path(N.BundleDir, P, Cid).string()}, {"stored", Stored},
                              {"live_token", ManifestModel::HasLiveToken(P)},
                              {"mount", ManifestModel::MakeVfsSpecLayer(L, Local.string(), Tgt, ManifestModel::LayerBaseTargets(L))}});
                    if (!FirstCompressed.empty()) R["first_compressed"] = FirstCompressed;
                }
                Out.push_back(R);
            }
            return Out;
        }, {} });

    Command::Register({
        "node freeze", "a node as it would freeze: handle and POS gone, refs as CIDs",
        "Freeze one node's raw JSON against a handle→CID map (--map, a JSON object; default: the library's own handles "
        "to their CIDs), printing the frozen node and its CID — what a mint stores. --remap rewrites refs only.",
        { NodeArg(), WithSample(Opt("map", "{handle: cid} (JSON)", Type::Json), "{\"draft-1\":\"bafkrei…\"}"), Flag("remap", "only rewrite its refs through the map") },
        { "node freeze 'Patch 2.0a'", "node freeze node.json --map '{\"draft-1\":\"bafkrei…\"}' --remap" },
        { "NodeGraph::FreezeNodeJson", "NodeRefs::Remap" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::shared_ptr<const NodeIndex> I;
            json J = NodeJson(C, Command::Str(A, "node"), &I);
            std::map<std::string, std::string> M;
            if (A.contains("map")) for (const auto &[K, V] : A["map"].items()) M[K] = V.get<std::string>();
            else
            {
                if (!I) I = Index(C);
                for (const auto &[K, N] : I->Nodes) if (!N.Handle.empty() && !N.Cid.empty()) M[N.Handle] = N.Cid;
            }
            if (Command::Bool(A, "remap", false))
            {
                const bool Changed = NodeRefs::Remap(J, [&](const std::string &R) { const auto It = M.find(R); return It == M.end() ? R : It->second; });
                return json{{"changed", Changed}, {"node", J}};
            }
            std::string Dangling;
            const json F = NodeGraph::FreezeNodeJson(J, M, nullptr, &Dangling);
            std::string Err;
            return json{{"node", F}, {"cid", Cid::OfNode(F, &Err)}, {"dangling", Dangling}};
        }, {} });

    Command::Register({
        "node verify", "check node bytes a peer sent: one block, its CID, a node, canonical, no handle",
        "What a received node file goes through before anything reads it: at most one block, hashing to the CID it was "
        "asked for, a node object (depth-checked), canonical, and carrying no working-tree field (CID, POS).",
        { Pos("file", "the node file"), Opt("cid", "the CID it must be (default: its file name)") },
        { "node verify ~/.VidyaGod/CATALOG/x/bafkrei….json" }, { "NodeGraph::VerifyLanded", "NodeGraph::VerifyNodeBytes" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string F = Command::Str(A, "file");
            const std::string Want = Command::Str(A, "cid", fs::path(F).stem().string());
            std::string Err, Err2;
            json J;
            const bool Ok = NodeGraph::VerifyLanded(F, Want, &J, &Err);
            const bool BytesOk = NodeGraph::VerifyNodeBytes(ReadFile(F), Want, J, &Err2);
            return json{{"ok", Ok && BytesOk}, {"cid", Want}, {"why", Ok ? Err2 : Err}};
        }, {} });

    // ---------------- fold ----------------
    Command::Register({
        "fold plan", "resolve a node: the plan a launch lowers, its events, what it leaves unmet",
        "Fold::Resolve over the library: every applied layer in fold order (with where it came from and how it is "
        "placed), the folded registry, DLL overrides, environment, entries, KEEP, declarations; the expansion's events "
        "(moved, held, cycle, missing, an ANY unmet, a NOT hit) and the WHEN variables phase 1 settled. --lowered adds "
        "the engine ops a launch would apply; --deciding the mentions whose position chose a winner.",
        { NodeArg(), VarOpt(), BuiltinOpt(), GraftOpt(), Flag("lowered", "also the plan in the engine's ops"),
          Flag("deciding", "also the held/moved mentions that decide a winner (one resolve each)") },
        { "fold plan bafkreiank43ketmdy2jtdcnn5utluobf53avcuubisi3ajtdxrcnmyteka", "fold plan 'Patch 2.0a' --var aoe2_skip_intro=1 --lowered" },
        { "Fold::Resolve", "Fold::PlanToJson", "Fold::Unsatisfied", "Fold::DecidingMentions", "ManifestModel::LibraryOf", "NodeLower::LowerPlan" , "Fold::Library::Find" },
        Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "node"));
            const Fold::Library L = ManifestModel::LibraryOf(*I);
            const auto Inst = Pairs(A, "var"), Bi = Pairs(A, "builtin");
            const auto G = List(A, "grafts");
            const Fold::Plan P = Fold::Resolve(L, N.Key(), Inst, Bi, G);
            json Out = PlanFacts(P);
            if (Command::Bool(A, "lowered", false)) Out["lowered"] = NodeLower::LowerPlan(P);
            if (Command::Bool(A, "deciding", false))
            {
                json D = json::array();
                for (const auto &[K, V] : Fold::DecidingMentions(L, N.Key(), Inst, Bi, G, P)) D.push_back({{"event", K}, {"node", V}});
                Out["deciding"] = D;
            }
            return Out;
        }, {} });

    Command::Register({
        "fold entries", "the entries a node's resolve folds (the EXEC fold alone)",
        "The same expansion as a full resolve, folding only EXEC: what the index derives a node's entries, runner and "
        "tiles from.", { NodeArg() }, { "fold entries 'Patch 2.0a'" }, { "Fold::ResolveEntries" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Fold::Plan P = Fold::ResolveEntries(ManifestModel::LibraryOf(*I), NodeOf(*I, Command::Str(A, "node"))->Key());
            return json{{"exec", P.Exec}, {"order", P.Order}, {"error", P.Error}};
        }, {} });

    Command::Register({
        "fold vars", "the variables a launch would have: each declaration's value",
        "Resolve a node, then settle its folded declarations the way a launch does: the instance's value, else the "
        "DEFAULT, gated by WHEN, substituted against the built-ins and each other until stable.",
        { NodeArg(), VarOpt(), BuiltinOpt(), GraftOpt() }, { "fold vars 'Patch 2.0a'", "fold vars 'Patch 2.0a' --var aoe2_skip_intro=1" },
        { "Fold::ResolveVars" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*I), NodeOf(*I, Command::Str(A, "node"))->Key(), Pairs(A, "var"),
                                               Pairs(A, "builtin"), List(A, "grafts"));
            return json{{"declarations", P.Decls}, {"values", Fold::ResolveVars(P.Decls, Pairs(A, "builtin"), Pairs(A, "var"))}};
        }, {} });

    Command::Register({
        "fold grafts", "the grafts the engine offers a row, pre-ticks, and applies",
        "Fold's graft rules on one row: the grafts its resolution is offered (an ANY it meets) and pre-ticked (RECOMMENDED "
        "under the tile), then the grafts it applies for a choice (--grafts; none: a fresh instance — the pre-ticked "
        "ones, or with --every every offered one) and those dropped.",
        { NodeArg(), Opt("tile", "the tile the row is under (default: its first face)"), GraftOpt(), Flag("every", "a fresh instance takes every offered graft"),
          VarOpt() },
        { "fold grafts 'Patch 1.0e (Unofficial)'", "fold grafts 'Patch 1.0e (Unofficial)' --grafts 'UserPatch 1.5' --tile 749" },
        { "Fold::BuildGraftIndex", "Fold::OfferedGrafts", "Fold::ApplyGrafts" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Node &N = *NodeOf(*I, Command::Str(A, "node"));
            const Fold::Library L = ManifestModel::LibraryOf(*I);
            const Fold::GraftIndex G = Fold::BuildGraftIndex(L);
            const std::string Face = Command::Str(A, "tile", N.Uid);
            const auto Inst = Pairs(A, "var");
            const Fold::Offer O = Fold::OfferedGrafts(L, G, Fold::Resolve(L, N.Key(), Inst), Face);
            const auto Req = List(A, "grafts");
            std::vector<std::string> Dropped;
            const auto Applied = Fold::ApplyGrafts(L, G, N.Key(), Inst, {}, Face, A.contains("grafts") ? &Req : nullptr, &Dropped,
                                                   Command::Bool(A, "every", false));
            return json{{"offered", O.Offered}, {"ticked", O.Ticked}, {"applied", Applied}, {"dropped", Dropped}};
        }, {} });

    Command::Register({
        "fold path", "the path rules: a guest path into a layout, back to anchors, under a TARGET, through a TAKE",
        "One of Fold's path rules on a string: --drives maps a guest path into a runner's layout (its DRIVES); "
        "--anchors re-spells a value a run wrote by its most specific anchor; --under places a relative path under a "
        "NODE layer's TARGET; --take applies one TAKE to an address (nothing: not taken).",
        { Pos("path", "the path, value or address"), WithSample(Opt("drives", "{\"C:\": \"%PrefixRoot%/drive_c\", …} (JSON)", Type::Json), "{\"C:\":\"%PrefixRoot%/drive_c\"}"),
          WithSample(Opt("anchors", "{\"%ProgramFiles32%\": \"C:\\\\Program Files (x86)\", …} (JSON)", Type::Json), "{\"%ProgramFiles32%\":\"C:\\\\Program Files (x86)\"}"), Opt("under", "a TARGET prefix"),
          WithSample(Opt("take", "a TAKE (JSON)", Type::Json), "\"FILES/a\" or [\"FILES/a\",\"FILES/z\"]") },
        { "fold path 'C:/Games/x.exe' --drives '{\"C:\":\"%PrefixRoot%/drive_c\"}'", "fold path 'data/x.dat' --under 'C:/Games/AoE2'",
          "fold path 'FILES/a/b.txt' --take '[\"FILES/a\", \"FILES/z\"]'" },
        { "Fold::ToLayout", "Fold::ToAnchors", "Fold::PlaceUnder", "Fold::TakeView" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string P = Command::Str(A, "path");
            json Out = {{"path", P}};
            if (A.contains("drives")) Out["layout"] = Fold::ToLayout(P, JsonArg(A, "drives"));
            if (A.contains("anchors"))
            {
                std::map<std::string, std::string> M;
                for (const auto &[K, V] : JsonArg(A, "anchors").items()) M[K] = V.get<std::string>();
                Out["anchored"] = Fold::ToAnchors(P, M);
            }
            if (A.contains("under")) Out["placed"] = Fold::PlaceUnder(P, Command::Str(A, "under"));
            if (A.contains("take")) { const auto V = Fold::TakeView(JsonArg(A, "take"), P); Out["taken"] = V ? json(*V) : json(nullptr); }
            if (Out.size() == 1) throw Command::BadArgs("give one of --drives, --anchors, --under, --take");
            return Out;
        }, {} });

    Command::Register({
        "fold keep", "whether an address is the user's under a node's KEEP fold",
        "Resolve a node and ask its KEEP fold about an address (FILES/… or REG/…, guest coordinates): owned by the user, "
        "and the kept address a deletion there would reach. With --op, the address an EDIT op writes at --target.",
        { NodeArg(), Pos("address", "the address (FILES/<path> or REG/<key>)"), WithSample(Opt("op", "an EDIT op (JSON)", Type::Json), "{\"ConfigWrite\":{\"KEY\":\"w\"}}"), VarOpt() },
        { "fold keep 'Patch 2.0a' 'FILES/C:/Games/AoE2/SAVEGAME'", "fold keep 'Patch 2.0a' 'C:/Games/x.ini' --op '{\"ConfigWrite\":{\"KEY\":\"w\"}}'" },
        { "NodeLower::UserOwned", "NodeLower::KeptUnder", "NodeLower::EditAddress", "Fold::TypeOf" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*I), NodeOf(*I, Command::Str(A, "node"))->Key(), Pairs(A, "var"));
            std::string Addr = Command::Str(A, "address");
            json Out = json::object();
            if (A.contains("op"))
            {
                const json Op = JsonArg(A, "op");
                Out["op_type"] = Fold::TypeOf(Op);
                Addr = NodeLower::EditAddress(Op, Addr);
                Out["writes"] = Addr;
            }
            Out["address"] = Addr;
            Out["user_owned"] = NodeLower::UserOwned(P.Keep, Addr);
            Out["kept_under"] = NodeLower::KeptUnder(P.Keep, Addr);
            return Out;
        }, {} });

    // ---------------- vars ----------------
    Command::Register({
        "vars render", "substitute %TOKEN%s in a text (or every string of a JSON value)",
        "The engine's substitution: %KEY% from --var, %KEY:format% rendered, %% a literal %. Prints the result, the keys "
        "the text names and whether anything was replaced. --json treats the text as a JSON value and substitutes every "
        "string inside it.",
        { Pos("text", "the text"), VarOpt(), Flag("keep-escapes", "keep %% doubled (as while variables resolve each other)"),
          Flag("json-value", "the text is JSON: substitute each string in it") },
        { "vars render 'C:/%GameDir%/x.ini' --var GameDir=AoE2", "vars render '%w:dword%' --var w=1024",
          "vars render '{\"a\":\"%X%\"}' --json-value --var X=1" },
        { "VarSubst::StringVariableSubstitution", "VarSubst::TokenKeys", "VarSubst::SubstituteJsonValues" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::string T = Command::Str(A, "text");
            const auto V = Pairs(A, "var");
            const bool Keep = Command::Bool(A, "keep-escapes", false);
            if (Command::Bool(A, "json-value", false))
            {
                const json J = json::parse(T, nullptr, false);
                if (J.is_discarded()) throw Command::BadArgs("the text is not JSON");
                return json{{"result", VarSubst::SubstituteJsonValues(J, Keep, V)}};
            }
            const auto Keys = VarSubst::TokenKeys(T);
            const bool Any = VarSubst::StringVariableSubstitution(T, V, Keep);
            return json{{"result", T}, {"keys", Keys}, {"replaced", Any}};
        }, {} });

    Command::Register({
        "vars format", "what a %KEY:format% inserts for a value",
        "Render a value through a use-site format (dword, qword, bool, winpath, upper, lower, u8…u32be, ascii, asciiz, "
        "or map:TABLE against --var's tables), as a final substitution writes it — or escaped, with --keep-escapes.",
        { Pos("value", "the value"), Pos("format", "the format"), VarOpt(), Flag("keep-escapes", "the escaped form") },
        { "vars format 1024 dword", "vars format 10 u32le", "vars format 4 map:res --var 'res:?=1,res:=4=1920x1080'" },
        { "VarSubst::RenderValue", "VarSubst::RenderInsert", "VarSubst::MapValue" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string V = Command::Str(A, "value"), F = Command::Str(A, "format");
            const auto Vars = Pairs(A, "var");
            json Out = {{"insert", VarSubst::RenderInsert(V, F, Vars, Command::Bool(A, "keep-escapes", false))}};
            if (F.rfind("map:", 0) == 0) Out["mapped"] = VarSubst::MapValue(V, F.substr(4), Vars);
            else Out["rendered"] = VarSubst::RenderValue(V, F);
            return Out;
        }, {} });

    Command::Register({
        "vars escape", "spell a text the way the format does (% → %%), or undo it",
        "Escape: a text as the format spells it (a value read back from a game's file); --undo: a resolved value as "
        "written out.", { Pos("text", "the text"), Flag("undo", "unescape (%% → %)") }, { "vars escape '100%'", "vars escape '100%%' --undo" },
        { "VarSubst::Escape", "VarSubst::Unescape" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string T = Command::Str(A, "text");
            return json{{"result", Command::Bool(A, "undo", false) ? VarSubst::Unescape(T) : VarSubst::Escape(T)}};
        }, {} });

    Command::Register({
        "vars when", "evaluate a WHEN condition",
        "The WHEN language: %KEY% operands, \"quoted\", bare words, == !=, && || ! ( ). Prints whether it parses and "
        "what it evaluates to with --var's values (an unparseable condition is fail-open: true).",
        { Pos("condition", "the condition"), VarOpt() }, { "vars when '%widescreen% == 1 && !%dgvoodoo%' --var widescreen=1" },
        { "VarSubst::EvaluateCondition", "VarSubst::ConditionParses" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            const std::string E = Command::Str(A, "condition");
            return json{{"parses", VarSubst::ConditionParses(E)}, {"value", VarSubst::EvaluateCondition(E, Pairs(A, "var"))}};
        }, {} });

    Command::Register({
        "vars eval", "evaluate an integer expression (a declaration's EVAL)",
        "C-precedence integer arithmetic over decimal and 0x numbers, after substituting --var: what a registry bitfield "
        "built from options becomes.", { Pos("expr", "the expression"), VarOpt() },
        { "vars eval '%red%*2 | %purple%*32' --var red=1,purple=1" }, { "VarSubst::EvaluateInteger" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::string E = Command::Str(A, "expr");
            VarSubst::StringVariableSubstitution(E, Pairs(A, "var"));
            long long V = 0;
            if (!VarSubst::EvaluateInteger(E, V)) throw std::runtime_error("not an integer expression: " + E);
            return json{{"expr", E}, {"value", V}};
        }, {} });

    Command::Register({
        "vars table", "a MAP table's entries as the variable namespace holds them",
        "A declaration with MAP (from a node's folded declarations, or --decl JSON) as the names it seeds: T:? (on), "
        "T:=<entry> (each value), T:! (its ELSE).",
        { Pos("table", "the declaration's name"), Opt("node", "a node whose resolve declares it"), WithSample(Opt("decl", "the declaration (JSON)", Type::Json), "{\"MAP\":{\"4\":\"1920x1080\"},\"ELSE\":\"800x600\"}") },
        { "vars table aoe2_skip_intro --node 'Patch 2.0a'", "vars table res --decl '{\"MAP\":{\"4\":\"1920x1080\"},\"ELSE\":\"800x600\"}'" },
        { "VarSubst::TableEntries" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const std::string T = Command::Str(A, "table");
            json D;
            if (A.contains("decl")) D = JsonArg(A, "decl");
            else
            {
                const auto I = Index(C);
                const Fold::Plan P = Fold::Resolve(ManifestModel::LibraryOf(*I), NodeOf(*I, Command::Str(A, "node"))->Key());
                if (!P.Decls.contains(T)) throw Command::BadArgs("the node declares no '" + T + "'");
                D = P.Decls[T];
            }
            json Out = json::object();
            for (const auto &[K, V] : VarSubst::TableEntries(T, D)) Out[K] = V;
            return Out;
        }, {} });

    // ---------------- cid ----------------
    Command::Register({
        "cid of", "the CID of a file's bytes as one raw block, and its SHA-256",
        "What the format names bytes by: CIDv1, raw, sha2-256, base32 — for a node file's bytes, a node's canonical "
        "bytes (--node: the file parsed as a node, or a node of the library), and the raw SHA-256.",
        { Pos("file", "a file (or, with --node, a node)"), Flag("node", "the CID of its canonical node bytes") },
        { "cid of x.json", "cid of 'Patch 2.0a' --node" }, { "Cid::OfBytes", "Cid::Sha256", "Cid::OfNode", "Cid::Canonical" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            std::string Err;
            const std::string Q = Command::Str(A, "file");
            if (Command::Bool(A, "node", false))
            {
                const json J = NodeJson(C, Q);
                const std::string Canon = Cid::Canonical(J), Id = Cid::OfNode(J, &Err);
                if (Id.empty()) throw std::runtime_error(Err);
                return json{{"cid", Id}, {"bytes", Canon.size()}, {"canonical", Canon}};
            }
            const std::string B = ReadFile(Q);
            const std::string Id = Cid::OfBytes(B, &Err), H = Cid::Sha256(B);
            if (Id.empty()) throw std::runtime_error(Err);
            std::string Hex;
            static const char *Dig = "0123456789abcdef";
            for (const unsigned char Ch : H) { Hex += Dig[Ch >> 4]; Hex += Dig[Ch & 15]; }
            return json{{"cid", Id}, {"bytes", B.size()}, {"sha256", Hex}};
        }, {} });

    // ---------------- graph ----------------
    Command::Register({
        "graph index", "build a node index from folders: what it holds and refuses",
        "Scan library roots (each holding package folders) and package folders directly (--bundle) into a node index, "
        "derive its facts, and report what it holds: nodes, tiles, grafts with cards, duplicates, refused node files.",
        { Pos("roots", "library roots, comma-separated"), Opt("bundle", "package folders to scan directly (a,b)"), Flag("each", "one row per node") },
        { "graph index ~/.VidyaGod/LIBRARY", "graph index '' --bundle ~/x/'[749][v1.0] Age of Empires II'" },
        { "ManifestModel::BuildNodeIndex", "ManifestModel::ScanBundleNodes", "ManifestModel::DeriveFacts" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::vector<fs::path> Roots, Bundles;
            for (const auto &R : List(A, "roots")) Roots.emplace_back(R);
            for (const auto &B : List(A, "bundle")) Bundles.emplace_back(B);
            NodeIndex I = ManifestModel::BuildNodeIndex(Roots);
            for (const auto &B : Bundles) ManifestModel::ScanBundleNodes(B, I);
            if (!Bundles.empty()) ManifestModel::DeriveFacts(I);
            json Ref = json::array();
            for (const auto &R : I.RefusedNodes) Ref.push_back({{"file", R.File.string()}, {"handle", R.Handle}, {"why", R.Why}, {"too_large", R.TooLarge}});
            json Dup = json::object();
            for (const auto &[K, V] : I.Duplicates) { json D = json::array(); for (const auto &P : V) D.push_back(P.string()); Dup[K] = D; }
            json Out = {{"nodes", I.Nodes.size()}, {"tiles", I.Tiles.size()}, {"graft_cards", I.TileGraft}, {"duplicates", Dup}, {"refused", Ref}};
            if (Command::Bool(A, "each", false))
            {
                json Rows = json::array();
                for (const auto &[K, N] : I.Nodes) Rows.push_back({{"key", K}, {"label", N.NodeId}, {"variant", N.Variant}, {"package", N.BundleDir.filename().string()}});
                Out["each"] = Rows;
            }
            return Out;
        }, {} });

    Command::Register({
        "graph tree", "read a working tree, order it for a mint, and freeze it (nothing stored)",
        "Gather every node file under a root (recursively) as a working tree, its mint order (deps first; cycles "
        "broken), and the CID-keyed index it freezes to — what the catalog derives on load, with no block stored.",
        { Pos("root", "the folder"), Flag("untrusted", "ignore stored CID handles (as for received stubs)"),
          Flag("skip-reserved", "skip _friend_* stub folders (as a publish does)") },
        { "graph tree ~/.VidyaGod/LIBRARY/VidyaGod" },
        { "NodeGraph::GatherWorkingTree", "NodeGraph::TopoOrderForMint", "NodeGraph::FreezeToIndex", "NodeGraph::IsFetchScratchDir" }, Needs::Config, false,
        [](Command::Context &, const json &A) {
            std::map<std::string, json> Tree;
            std::map<std::string, fs::path> Dirs;
            const fs::path Root = Command::Str(A, "root");
            NodeGraph::GatherWorkingTree(Root, Tree, Dirs, Command::Bool(A, "skip-reserved", false), !Command::Bool(A, "untrusted", false));
            const std::vector<std::string> Order = NodeGraph::TopoOrderForMint(Tree);
            const NodeIndex I = NodeGraph::FreezeToIndex(Tree, Dirs);
            int Scratch = 0;
            for (const auto &[H, D] : Dirs) Scratch += NodeGraph::IsFetchScratchDir(D) ? 1 : 0;
            return json{{"nodes", Tree.size()}, {"order", Order}, {"frozen", I.Nodes.size()}, {"tiles", I.Tiles.size()}, {"in_scratch_dirs", Scratch}};
        }, {} });

    Command::Register({
        "graph mint", "mint a working tree into blocks (stored, pinned, announced)",
        "Freeze and store every node under a root, deps first: each handle's CID, and the playable roots. Blocks only "
        "— node files on disk are not rewritten (`packages stamp-cids` writes the CIDs back).",
        { Pos("root", "the folder") }, { "graph mint ~/x/'[123][v1.0] Some Game'" }, { "NodeStore::Mint" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::map<std::string, json> Tree;
            std::map<std::string, fs::path> Dirs;
            NodeGraph::GatherWorkingTree(Command::Str(A, "root"), Tree, Dirs);
            NodeStore::MintResult R;
            std::string Err;
            if (!NodeStore::Mint(Tree, R, &Err)) throw std::runtime_error(Err);
            return json{{"minted", R.HandleToCid}, {"launchables", R.Launchables}};
        }, {} });

    Command::Register({
        "graph frozen", "walk a frozen node DAG from IPFS into an index",
        "Fetch each block from the given root CIDs, following what each node contains (never a NOT, never content), "
        "and report the index it builds and the blocks that could not be fetched or are not nodes.",
        { Pos("cids", "root CIDs, comma-separated"), Flag("each", "one row per node") }, { "graph frozen bafkrei…,bafkrei…" },
        { "NodeStore::BuildFrozenIndex" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::vector<std::string> Missing;
            const NodeIndex I = NodeStore::BuildFrozenIndex(List(A, "cids"), &Missing);
            json Out = {{"nodes", I.Nodes.size()}, {"tiles", I.Tiles.size()}, {"missing", Missing}};
            if (Command::Bool(A, "each", false))
            {
                json Rows = json::array();
                for (const auto &[K, N] : I.Nodes) Rows.push_back({{"cid", K}, {"label", N.NodeId}, {"variant", N.Variant}});
                Out["each"] = Rows;
            }
            return Out;
        }, {} });

    Command::Register({
        "graph checkout", "land a launchable's closure from IPFS as a package folder",
        "Fetch a launchable node's DAG and content into \"[uid] title\" under a folder: its node files written readable "
        "(they re-freeze to the same CIDs) and every content file at its PATH — a game from a bare CID.",
        { Pos("cid", "the launchable's CID"), Pos("dest", "the folder to land it in") }, { "graph checkout bafkrei… ~/Storage/tmp/co" },
        { "NodeStore::HydratePackage" }, Needs::Config, true,
        [](Command::Context &, const json &A) {
            std::string Err;
            const std::string D = NodeStore::HydratePackage(Command::Str(A, "dest"), Command::Str(A, "cid"), &Err);
            if (D.empty()) throw std::runtime_error(Err);
            return json{{"landed", D}};
        }, {} });

    Command::Register({
        "graph within", "whether a path stays within a folder (no .. escape)",
        "The guard on every write a node can direct: does <path> resolve inside <base> (works for paths that do not "
        "exist yet).", { Pos("base", "the folder"), Pos("path", "the path") }, { "graph within ~/pkg ~/pkg/../etc/passwd" },
        { "NodeGraph::PathWithin" }, Needs::Config, false,
        [](Command::Context &, const json &A) { return json{{"within", NodeGraph::PathWithin(Command::Str(A, "base"), Command::Str(A, "path"))}}; }, {} });

    Command::Register({
        "graph shelf", "what the shelf makes of the tiles: warnings and download units",
        "The library-wide view no single node shows: tiles no variant presents and tiles named by a CID (no TITLE); "
        "with --tile, that tile's download units (what each Download row fetches, and how many versions it carries); "
        "with --graft, the versions a graft applies onto.",
        { Opt("tile", "a tile UID: its download units"), Opt("graft", "a graft: the versions it applies onto") },
        { "graph shelf", "graph shelf --tile 749", "graph shelf --graft 'UserPatch 1.5'" },
        { "Validate::Shelf", "ManifestModel::TileEndpoints", "ManifestModel::GraftBases" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            json Out = {{"warnings", Validate::Shelf(*I)}};
            if (A.contains("tile"))
            {
                const std::string U = Command::Str(A, "tile");
                std::vector<std::string> Vs;
                for (const auto &[K, N] : I->Nodes)
                    if (N.IsVariant() && std::find(N.Faces.begin(), N.Faces.end(), U) != N.Faces.end()) Vs.push_back(K);
                json Rows = json::array();
                for (const auto &E : ManifestModel::TileEndpoints(*I, Vs)) Rows.push_back({{"label", E.Label}, {"versions", E.LaunchableCount}, {"nodes", E.Ids}});
                Out["downloads"] = Rows;
            }
            if (A.contains("graft")) Out["graft_bases"] = ManifestModel::GraftBases(*I, NodeOf(*I, Command::Str(A, "graft"))->Key());
            return Out;
        }, {} });

    Command::Register({
        "graph fix-case", "canonicalise zips whose entries clash in case with a lower layer",
        "Rewrite (unpack → rename → repack STORE) the higher layers' zips so their entries take the base layer's case — "
        "the case-conflict errors the validator reports. --scope limits the rewrites to one folder.",
        { Opt("scope", "only rewrite zips under this folder") }, { "graph fix-case --scope ~/.VidyaGod/LIBRARY/VidyaGod/'[749][v1.0] Age of Empires II'" },
        { "Validate::FixCaseConflicts" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const auto I = Index(C);
            std::vector<std::string> Log;
            const fs::path S = Command::Str(A, "scope", "");
            const int N = Validate::FixCaseConflicts(*I, Log, A.contains("scope") ? &S : nullptr);
            return json{{"rewritten", N}, {"log", Log}};
        }, {} });

    Command::Register({
        "graph platform", "this machine's platform, and whether a runner serves one",
        "The platform a runner's HOST must match here; with --runner and --platform, whether that runner serves it "
        "(\"custom\" only from its own package, --package).",
        { Opt("runner", "a runner node"), Opt("platform", "the platform it should serve"), Opt("package", "the launch's package folder") },
        { "graph platform", "graph platform --runner geproton_10_34 --platform win32" },
        { "ManifestModel::MachinePlatform", "ManifestModel::ServesPlatform" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            json Out = {{"machine", ManifestModel::MachinePlatform()}};
            if (A.contains("runner"))
            {
                const auto I = Index(C);
                const fs::path P = A.contains("package") ? PackageDir(C, Command::Str(A, "package")) : fs::path();
                Out["serves"] = ManifestModel::ServesPlatform(*NodeOf(*I, Command::Str(A, "runner")), Command::Str(A, "platform"), P);
            }
            return Out;
        }, {} });

    // ---------------- content ----------------
    Command::Register({
        "content claims", "the names a package's nodes claim, and the CIDs claiming each",
        "Each content file name a package's node files name (content layers' SOURCE, tile covers) with the CIDs that "
        "claim it — a name two CIDs claim is kept at bumped names. --files: claims as they would be over just those "
        "node files.", { Pos("package", "a folder path, its name, its UID, or part of its title"), Opt("files", "node files (a,b)") },
        { "content claims 749" }, { "ContentFile::Claims", "ContentFile::ClaimsOfFiles", "ContentFile::NameLess::operator()" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            ContentFile::ClaimMap M;
            if (A.contains("files"))
            {
                std::vector<fs::path> F;
                for (const auto &X : List(A, "files")) F.emplace_back(X);
                M = ContentFile::ClaimsOfFiles(F);
            }
            else M = *ContentFile::Claims(PackageDir(C, Command::Str(A, "package")));
            json Out = json::object();
            for (const auto &[Name, Cids] : M) Out[Name] = std::vector<std::string>(Cids.begin(), Cids.end());
            return Out;
        }, {} });

    Command::Register({
        "content path", "the file a package keeps a name's bytes at",
        "Where package <package> keeps <name> holding <cid>: the name itself, or its bumped name (\"<stem> [<cid tail>]"
        "<ext>\") when its nodes claim the name for more than one CID. --also adds a hypothetical claim (planning a "
        "change).",
        { Pos("package", "a folder path, its name, its UID, or part of its title"), Pos("name", "the content file name"), Pos("cid", "its CID"),
          Opt("also", "another CID claiming the name") },
        { "content path 749 'Age of Empires II.zip' Qm…", "content path 749 dxwrapper.zip Qm… --also Qm…" },
        { "ContentFile::Path", "ContentFile::PathUnder", "ContentFile::Bumped" }, Needs::Config, false,
        [](Command::Context &C, const json &A) {
            const fs::path D = PackageDir(C, Command::Str(A, "package"));
            const std::string N = Command::Str(A, "name"), Id = Command::Str(A, "cid");
            json Out = {{"path", ContentFile::Path(D, N, Id).string()}, {"bumped_name", ContentFile::Bumped(N, Id)}};
            if (A.contains("also"))
            {
                ContentFile::ClaimMap M = *ContentFile::Claims(D);
                M[N].insert(Id);
                M[N].insert(Command::Str(A, "also"));
                Out["with_also"] = ContentFile::PathUnder(D, N, Id, M).string();
            }
            return Out;
        }, {} });
}
