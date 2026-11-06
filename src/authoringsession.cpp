#include "varsubst.h"
#include "authoringsession.h"
#include "containerwrapper.h"   // ContainerWrapper + ContainerParams (full definition)
#include "commonutils.h"        // Log*
#include "launchresolver.h"
#include "manifestmodel.h"
#include "apppaths.h"     // AnchorRegEdits / AnchorLayouts — captures and previews speak anchors

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

AuthoringSession::AuthoringSession(const nlohmann::ordered_json &GlobalConfigJSON, const QDir &Bundle)
    : ConfigCopy(GlobalConfigJSON), BundleDir(Bundle) {}

AuthoringSession::~AuthoringSession() { End(); }

//A node still waiting for its files (a content layer with no file yet: a new package before its install) is
//authored as it is without them - the files are what the session is about to make. Only this index copy changes.
bool AuthoringSession::WithoutWaiting(NodeIndex &Idx, const std::string &Id)
{
    const auto It = Idx.Nodes.find(Id);
    if (It == Idx.Nodes.end() || !It->second.Json->contains("LAYERS") || !(*It->second.Json)["LAYERS"].is_array()) return false;
    nlohmann::ordered_json J = It->second.Json, Kept = nlohmann::ordered_json::array();
    for (const auto &L : J["LAYERS"])
    {
        bool Waiting = false;
        for (const char *T : {"ZIP", "FILE", "DIR", "DELTA"})
            if (L.is_object() && L.contains(T) && L[T].is_string() && L[T].get<std::string>().empty()) Waiting = true;
        if (!Waiting) Kept.push_back(L);
    }
    if (Kept.size() == J["LAYERS"].size()) return false;
    J["LAYERS"] = std::move(Kept);
    Node N;
    if (!ManifestModel::ParseNode(J, It->second.File, It->second.BundleDir, N)) return false;
    N.Cid = It->second.Cid; N.Handle = It->second.Handle;
    It->second = std::move(N);
    ManifestModel::DeriveFacts(Idx);
    return true;
}

bool AuthoringSession::Begin(const NodeIndex &SrcIdx, const std::string &TargetNodeId,
                             const std::map<std::string, std::string> &Vars)
{
    Idx = SrcIdx;                                   // own a copy; Params->NodeIdx points into THIS
    TargetId = TargetNodeId;
    WithoutWaiting(Idx, TargetId);
    VarOverrides = Vars;
    // Open a BARE, platform-agnostic runtime: the node's content overlay + a writable upper, no runner, no prefix
    // (an empty runtime is fine — a fresh node has no content). Runner-driven tools (RunWindows) rebuild on demand.
    return BuildRuntime({});
}

bool AuthoringSession::RunWindows(const std::string &Exe, const std::string &RunnerId, const std::vector<std::string> &Args)
{
    // "Run a Windows program in a wine/proton prefix" — a per-invocation tool. (Re)build the runtime under the CHOSEN
    // runner if we're not already there (its DEFPREFIX + CONTENT_ROOT come from the runner; platform is seeded from the
    // runner's guest, independent of the package), then run the exe against the live mount.
    if (RunnerId.empty()) { LogErr("AuthoringSession::RunWindows", "No runner chosen."); return false; }
    if (!Wrapper || CurrentRunnerId != RunnerId)
        if (!BuildRuntime({ RunnerId })) return false;
    return RunExe(Exe, Args);
}

bool AuthoringSession::BuildRuntime(const std::vector<std::string> &Chain)
{
    if (Wrapper) End();
    Params = std::make_unique<ContainerParams>(BundleDir.path().toStdString());
    Params->NodeIdx        = &Idx;
    Params->LaunchNodeId   = TargetId;
    //On disk, not in the system temp (RAM): an installer writes the whole game into this session.
    Params->TempRootOverride = AppPaths::DataRoot() / "AUTHORING";
    Params->VariableOverrides = VarOverrides;

    if (Chain.empty())
    {
        Params->AuthoringBare = true;               // runner-less content overlay + writable upper
        CurrentRunnerId.clear();
    }
    else
    {
        Params->RunnerChainIds = Chain;             // pin the chosen runner (its prefix/content-root define the env)
        // The picked runner defines the platform we're authoring under: seed it onto OUR index copy so the chain
        // resolves. The user's on-disk node stays platformless (capture is content; identity is authored separately).
        auto It = Idx.Nodes.find(TargetId);
        if (It != Idx.Nodes.end() && It->second.HostPlatform.empty())
        {
            const Node *R = Idx.Find(Chain.front());
            if (R && !R->GuestPlatform.empty()) It->second.HostPlatform = R->GuestPlatform.front();
        }
        CurrentRunnerId = Chain.front();
    }

    Wrapper = std::make_unique<ContainerWrapper>(ConfigCopy, *Params);   // ctor runs InitializeContainer
    if (!Params->AuthoringBare && Wrapper->ContainerParams.RunnerChain.empty())
    {
        LogErr("AuthoringSession::BuildRuntime", "No runner resolved for node '" + TargetId
               + "' under '" + (Chain.empty() ? std::string() : Chain.front()) + "'.");
        Wrapper.reset(); Params.reset();
        return false;
    }
    Wrapper->SetNotAGame();
    if (!Wrapper->BuildContainerRuntime())
    {
        LogErr("AuthoringSession::BuildRuntime", "Failed to build authoring runtime for '" + TargetId + "'.");
        DropRuntime(); Params.reset();
        return false;
    }

    // Snapshot the prefix registry baseline (wine runtimes only) so CaptureRegistryDelta can diff against it.
    HasBaseline = false;
    if (Wrapper->ContainerParams.PrefixGenerate)
    {
        const fs::path Hives = Wrapper->ContainerParams.PrefixRoot.empty()
            ? Wrapper->ContainerParams.RuntimePath
            : Wrapper->ContainerParams.RuntimePath / Wrapper->ContainerParams.PrefixRoot;
        Baseline = RegistryWrapper{};
        //An empty baseline is not a small error: the diff at End() then reports EVERY key already in the prefix
        //as something the author just created, and the resulting node ships thousands of bogus RegEdits.
        if (!Baseline.LoadPrefix(Hives))
            LogErr("AuthoringSession::BuildRuntime", "could not snapshot the baseline registry at " + Hives.string()
                       + " — the registry diff for this session will treat the ENTIRE existing prefix as new edits.");
        HasBaseline = true;
    }
    LogSucc("AuthoringSession::BuildRuntime", "Authoring runtime live for '" + TargetId + "' at "
            + Wrapper->ContainerParams.RuntimePath.string() + (Params->AuthoringBare ? " (bare)" : ""));
    return true;
}

bool AuthoringSession::PrefixGenerate() const { return Wrapper && Wrapper->ContainerParams.PrefixGenerate; }
std::string AuthoringSession::RunnerId() const { return Wrapper ? Wrapper->ContainerParams.RunnerID : std::string{}; }
fs::path AuthoringSession::RuntimePath() const { return Wrapper ? Wrapper->ContainerParams.RuntimePath : fs::path{}; }
fs::path AuthoringSession::WriteLayerPath() const { return Wrapper ? Wrapper->ContainerParams.WriteLayerPath : fs::path{}; }
std::string AuthoringSession::ContentRoot() const { return Wrapper ? Wrapper->ContainerParams.ContentRoot : std::string{}; }
std::vector<std::pair<std::string, std::string>> AuthoringSession::AnchorLayouts() const
{
    return Wrapper ? LaunchResolver::AnchorLayouts(Wrapper->ContainerParams) : std::vector<std::pair<std::string, std::string>>{};
}

bool AuthoringSession::RunExe(const std::string &Exe, const std::vector<std::string> &Args)
{
    if (!Wrapper) { LogErr("AuthoringSession::RunExe", "No live session."); return false; }
    return Wrapper->Execute(Exe, Args);
}

nlohmann::ordered_json AuthoringSession::CaptureRegistryDelta() const
{
    if (!Wrapper || !HasBaseline) return nlohmann::ordered_json::array();
    const fs::path Hives = Wrapper->ContainerParams.PrefixRoot.empty()
        ? Wrapper->ContainerParams.RuntimePath
        : Wrapper->ContainerParams.RuntimePath / Wrapper->ContainerParams.PrefixRoot;
    RegistryWrapper Now;
    //The mirror image: an empty "now" diffs to nothing, so the author's registry work vanishes without a word.
    if (!Now.LoadPrefix(Hives))
        LogErr("AuthoringSession::RegistryDelta", "could not read the session registry at " + Hives.string()
                   + " — the diff will be EMPTY and any registry changes made while authoring are lost.");
    //What the run wrote is in guest spelling (an installer's C:\Program Files (x86)\…); the node names places by anchor.
    //And it becomes package data, which the format reads for %tokens%: a '%' the program wrote (a REG_EXPAND_SZ's
    //%SystemRoot%, a value named %Value%) is spelled %%, before the anchors (real tokens) are spelled in.
    nlohmann::ordered_json Edits = Now.DiffToRegEdits(Baseline);
    for (auto &E : Edits)
    {
        if (!E.is_object()) continue;
        if (E.contains("REGPATH") && E["REGPATH"].is_string()) E["REGPATH"] = VarSubst::Escape(E["REGPATH"].get<std::string>());
        if (!E.contains("KEYVALUES") || !E["KEYVALUES"].is_object()) continue;
        nlohmann::ordered_json KV = nlohmann::ordered_json::object();
        for (const auto &[K, V] : E["KEYVALUES"].items())
            KV[VarSubst::Escape(K)] = V.is_string() ? nlohmann::ordered_json(VarSubst::Escape(V.get<std::string>())) : V;
        E["KEYVALUES"] = KV;
    }
    return LaunchResolver::AnchorRegEdits(Wrapper->ContainerParams, Edits);
}

void AuthoringSession::DropRuntime()
{
    if (!Wrapper) return;
    const std::filesystem::path Game = Wrapper->ContainerParams.TempPath.parent_path();
    Wrapper->Cleanup(); Wrapper.reset();
    //Cleanup takes the instance's folder; the game's folder over it is ours too, and left behind empty it piles up
    //in AUTHORING (one per game ever captured). Only when empty: another instance's session may be in it.
    std::error_code Ec;
    if (!Game.empty()) std::filesystem::remove(Game, Ec);
}

void AuthoringSession::End()
{
    DropRuntime();
    Params.reset();
    HasBaseline = false;
}

// --------------------------------------------------------------------------- file-capture mechanism (static)

bool AuthoringSession::IsChurn(const std::string &Rel)
{
    auto ends = [&](const char *S){ const size_t N = std::strlen(S); return Rel.size() >= N && Rel.compare(Rel.size() - N, N, S) == 0; };
    auto has  = [&](const char *S){ return Rel.find(S) != std::string::npos; };
    // Wine registry hives + bookkeeping.
    if (ends(".reg") || ends(".update-timestamp") || ends(".tmp")) return true;
    // The wine-managed system tree + device links — never package content.
    if (has("/windows/") || has("/Windows/") || has("/dosdevices/")) return true;
    // Scratch/temp.
    if (has("/Temp/") || has("/temp/")) return true;
    // The write layer's whiteouts (".wh.<name>" marks <name> deleted, ".wh..wh..opq" an opaque folder): how the
    // overlay records a deletion, not a file - a patcher that re-cases EULA.RTF to EULA.rtf leaves one beside it.
    const size_t Slash = Rel.find_last_of('/');
    if (Rel.compare(Slash == std::string::npos ? 0 : Slash + 1, 4, ".wh.") == 0) return true;
    return false;
}

std::vector<std::string> AuthoringSession::EnumerateDelta(const fs::path &Root)
{
    std::vector<std::string> Out;
    std::error_code ec;
    if (Root.empty() || !fs::exists(Root, ec)) return Out;
    for (auto It = fs::recursive_directory_iterator(Root, fs::directory_options::skip_permission_denied, ec);
         It != fs::recursive_directory_iterator(); It.increment(ec))
    {
        if (ec) break;
        if (!It->is_regular_file(ec)) continue;
        const std::string Rel = fs::relative(It->path(), Root, ec).generic_string();
        if (Rel.empty() || IsChurn(Rel)) continue;
        Out.push_back(Rel);
    }
    std::sort(Out.begin(), Out.end());
    return Out;
}

int AuthoringSession::CopySelection(const fs::path &SrcRoot, const std::vector<std::string> &Rels,
                                    const fs::path &DestRoot, const std::string &StripPrefix)
{
    int Count = 0;
    std::error_code ec;
    std::string Prefix = StripPrefix;
    if (!Prefix.empty() && Prefix.back() != '/') Prefix += '/';

    auto DestRel = [&](std::string Rel) -> std::string {
        if (!Prefix.empty() && Rel.rfind(Prefix, 0) == 0) return Rel.substr(Prefix.size());
        return Rel;
    };
    auto CopyOne = [&](const fs::path &Src, const std::string &Rel) {
        const std::string R = DestRel(Rel);
        if (R.empty()) return;
        const fs::path Dst = DestRoot / R;
        fs::create_directories(Dst.parent_path(), ec);
        fs::copy_file(Src, Dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++Count;
    };

    for (const std::string &Rel : Rels)
    {
        const fs::path Src = SrcRoot / Rel;
        if (fs::is_directory(Src, ec))
        {
            for (auto It = fs::recursive_directory_iterator(Src, fs::directory_options::skip_permission_denied, ec);
                 It != fs::recursive_directory_iterator(); It.increment(ec))
            {
                if (ec) break;
                if (!It->is_regular_file(ec)) continue;
                CopyOne(It->path(), fs::relative(It->path(), SrcRoot, ec).generic_string());
            }
        }
        else if (fs::is_regular_file(Src, ec))
            CopyOne(Src, Rel);
    }
    return Count;
}
