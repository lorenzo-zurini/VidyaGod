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
                             const std::map<std::string, std::string> &Vars,
                             const std::vector<std::string> &Chain)
{
    if (Wrapper) End();
    Idx = SrcIdx;                                   // own a copy; Params->NodeIdx points into THIS
    Params = std::make_unique<ContainerParams>(BundleDir.path().toStdString());
    Params->NodeIdx        = &Idx;
    Params->LaunchNodeId   = TargetNodeId;
    Params->VariableOverrides = Vars;
    if (!Chain.empty()) Params->RunnerChainIds = Chain;

    Wrapper = std::make_unique<ContainerWrapper>(ConfigCopy, DummyManifest, *Params);   // ctor runs InitializeContainer
    if (Wrapper->ContainerParams.RunnerChain.empty())
    {
        LogErr("AuthoringSession::Begin", "No runner resolved for node '" + TargetNodeId
               + "' — give it a PLATFORM.HOST or pin a runner.");
        Wrapper.reset(); Params.reset();
        return false;
    }
    if (!Wrapper->BuildContainerRuntime())
    {
        LogErr("AuthoringSession::Begin", "Failed to build authoring runtime for '" + TargetNodeId + "'.");
        Wrapper->Cleanup(); Wrapper.reset(); Params.reset();
        return false;
    }

    // Snapshot the prefix registry baseline (wine sessions only) so CaptureRegistryDelta can diff against it.
    HasBaseline = false;
    if (Wrapper->ContainerParams.PrefixGenerate)
    {
        const fs::path Hives = Wrapper->ContainerParams.PrefixRoot.empty()
            ? Wrapper->ContainerParams.RuntimePath
            : Wrapper->ContainerParams.RuntimePath / Wrapper->ContainerParams.PrefixRoot;
        Baseline = RegistryWrapper{};
        Baseline.LoadPrefix(Hives);
        HasBaseline = true;
    }
    LogSucc("AuthoringSession::Begin", "Authoring runtime live for '" + TargetNodeId + "' at "
            + Wrapper->ContainerParams.RuntimePath.string());
    return true;
}

bool AuthoringSession::PrefixGenerate() const { return Wrapper && Wrapper->ContainerParams.PrefixGenerate; }
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
    Now.LoadPrefix(Hives);
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
