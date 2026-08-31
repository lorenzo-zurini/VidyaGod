#include "authoringsession.h"
#include "containerwrapper.h"   // ContainerWrapper + ContainerParams (full definition)
#include "commonutils.h"        // Log*

#include <algorithm>
#include <cstring>
#include <system_error>

namespace fs = std::filesystem;

AuthoringSession::AuthoringSession(const nlohmann::ordered_json &GlobalConfigJSON, const QDir &Bundle)
    : ConfigCopy(GlobalConfigJSON), BundleDir(Bundle) {}

AuthoringSession::~AuthoringSession() { End(); }

bool AuthoringSession::Begin(const NodeIndex &SrcIdx, const std::string &TargetNodeId,
                             const std::map<std::string, std::string> &Vars)
{
    Idx = SrcIdx;                                   // own a copy; Params->NodeIdx points into THIS
    TargetId = TargetNodeId;
    VarOverrides = Vars;
    // Open a BARE, platform-agnostic runtime: the node's content overlay + a writable upper, no runner, no prefix
    // (an empty runtime is fine — a fresh node has no content). Runner-driven tools (RunWindows) rebuild on demand.
    return BuildRuntime({});
}

bool AuthoringSession::RunWindows(const std::string &Exe, const std::string &RunnerId)
{
    // "Run a Windows program in a wine/proton prefix" — a per-invocation tool. (Re)build the runtime under the CHOSEN
    // runner if we're not already there (its DEFPREFIX + CONTENT_ROOT come from the runner; platform is seeded from the
    // runner's guest, independent of the package), then run the exe against the live mount.
    if (RunnerId.empty()) { LogErr("AuthoringSession::RunWindows", "No runner chosen."); return false; }
    if (!Wrapper || CurrentRunnerId != RunnerId)
        if (!BuildRuntime({ RunnerId })) return false;
    return RunExe(Exe);
}

bool AuthoringSession::BuildRuntime(const std::vector<std::string> &Chain)
{
    if (Wrapper) End();
    Params = std::make_unique<ContainerParams>(BundleDir.path().toStdString());
    Params->NodeIdx        = &Idx;
    Params->LaunchNodeId   = TargetId;
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

    Wrapper = std::make_unique<ContainerWrapper>(ConfigCopy, DummyManifest, *Params);   // ctor runs InitializeContainer
    if (!Params->AuthoringBare && Wrapper->ContainerParams.RunnerChain.empty())
    {
        LogErr("AuthoringSession::BuildRuntime", "No runner resolved for node '" + TargetId
               + "' under '" + (Chain.empty() ? std::string() : Chain.front()) + "'.");
        Wrapper.reset(); Params.reset();
        return false;
    }
    if (!Wrapper->BuildContainerRuntime())
    {
        LogErr("AuthoringSession::BuildRuntime", "Failed to build authoring runtime for '" + TargetId + "'.");
        Wrapper->Cleanup(); Wrapper.reset(); Params.reset();
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

bool AuthoringSession::RunExe(const std::string &Exe)
{
    if (!Wrapper) { LogErr("AuthoringSession::RunExe", "No live session."); return false; }
    return Wrapper->Execute(Exe);
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
    return Now.DiffToRegEdits(Baseline);
}

void AuthoringSession::End()
{
    if (Wrapper) { Wrapper->Cleanup(); Wrapper.reset(); }
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

nlohmann::ordered_json AuthoringSession::MakeDirLayer(const std::string &DirNameRelToBundle, const std::string &Target)
{
    nlohmann::ordered_json L = nlohmann::ordered_json::object();
    L["TYPE"] = "VFSDirLayer";
    L["PATH"] = DirNameRelToBundle;
    if (!Target.empty()) L["TARGET"] = Target;
    return L;
}
