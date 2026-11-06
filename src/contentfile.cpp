#include "contentfile.h"
#include "nodegraph.h"
#include "varsubst.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>

namespace ContentFile
{

std::string Bumped(const std::string &Name, const std::string &Cid)
{
    const std::filesystem::path P(Name);
    const std::string Tail = Cid.size() > 8 ? Cid.substr(Cid.size() - 8) : Cid;
    return (P.parent_path() / (P.stem().string() + " [" + Tail + "]" + P.extension().string())).generic_string();
}

bool NameLess::operator()(const std::string &A, const std::string &B) const
{
    return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](unsigned char X, unsigned char Y) {
        return std::tolower(X) < std::tolower(Y);
    });
}

namespace {
void ClaimsOfNode(const nlohmann::ordered_json &J, ClaimMap &Out)
{
    if (!J.is_object() || !J.contains("LAYERS") || !J["LAYERS"].is_array()) return;
    auto Claim = [&](const nlohmann::ordered_json &Name, const nlohmann::ordered_json &Src) {
        if (Name.is_string() && Src.is_string() && !Src.get<std::string>().empty()) Out[Name.get<std::string>()].insert(Src.get<std::string>());
    };
    for (const auto &L : J["LAYERS"])
    {
        if (!L.is_object()) continue;
        for (const char *K : {"ZIP", "FILE", "DELTA"})
            if (L.contains(K)) Claim(L[K], L.contains("SOURCE") ? L["SOURCE"] : nlohmann::ordered_json());
        if (L.contains("EXEC") && L["EXEC"].is_array())
            for (const auto &E : L["EXEC"])
                if (E.is_object() && E.contains("TILE") && E["TILE"].is_object() && E["TILE"].contains("COVER")
                    && E["TILE"]["COVER"].is_object())
                {
                    const auto &C = E["TILE"]["COVER"];
                    Claim(C.contains("FILE") ? C["FILE"] : nlohmann::ordered_json(), C.contains("SOURCE") ? C["SOURCE"] : nlohmann::ordered_json());
                }
    }
}
} // namespace

ClaimMap ClaimsOfFiles(const std::vector<std::filesystem::path> &NodeFiles)
{
    ClaimMap Out;
    for (const auto &F : NodeFiles)
        if (nlohmann::ordered_json J; NodeGraph::ReadTreeJsonBounded(F, J)) ClaimsOfNode(J, Out);
    return Out;
}

std::shared_ptr<const ClaimMap> Claims(const std::filesystem::path &Dir)
{
    namespace fs = std::filesystem;
    struct Entry { fs::file_time_type When; std::string Files; std::shared_ptr<const ClaimMap> Claims; };
    static std::mutex Mu;
    static std::map<std::string, Entry> Cache;
    //The folder's time is the quick check; when it moved, the node files (name, size, time) decide — a content file
    //landing in the folder changes its time but no claim.
    std::error_code Ec;
    const fs::file_time_type When = fs::last_write_time(Dir, Ec);
    if (Ec) return std::make_shared<const ClaimMap>();
    {
        std::lock_guard<std::mutex> L(Mu);
        if (auto It = Cache.find(Dir.string()); It != Cache.end() && It->second.When == When) return It->second.Claims;
    }
    std::vector<fs::path> NodeFiles;
    std::string Files;
    for (const auto &F : fs::directory_iterator(Dir, Ec))
    {
        std::error_code Fe;
        if (!F.is_regular_file(Fe) || F.path().extension() != ".json") continue;
        NodeFiles.push_back(F.path());
    }
    std::sort(NodeFiles.begin(), NodeFiles.end());
    for (const auto &F : NodeFiles)
    {
        std::error_code Fe;
        Files += F.filename().string() + "|" + std::to_string(fs::file_size(F, Fe)) + "|"
               + std::to_string(fs::last_write_time(F, Fe).time_since_epoch().count()) + "\n";
    }
    {
        std::lock_guard<std::mutex> L(Mu);
        if (auto It = Cache.find(Dir.string()); It != Cache.end() && It->second.Files == Files)
        {
            It->second.When = When;
            return It->second.Claims;
        }
    }
    auto Shared = std::make_shared<const ClaimMap>(ClaimsOfFiles(NodeFiles));
    std::lock_guard<std::mutex> L(Mu);
    if (Cache.size() > 4096) Cache.clear();
    Cache[Dir.string()] = Entry{ When, std::move(Files), Shared };
    return Shared;
}

std::filesystem::path PathUnder(const std::filesystem::path &Dir, const std::string &Name, const std::string &Cid,
                                const ClaimMap &Claims)
{
    const std::filesystem::path P(Name);
    if (Cid.empty() || Name.empty() || P.is_absolute() || VarSubst::RuntimeSourced(Name)) return Dir / P;
    const auto It = Claims.find(Name);
    return (It != Claims.end() && It->second.size() > 1) ? Dir / Bumped(Name, Cid) : Dir / P;
}

std::filesystem::path Path(const std::filesystem::path &Dir, const std::string &Name, const std::string &Cid)
{
    const std::filesystem::path P(Name);
    if (Cid.empty() || Name.empty() || P.is_absolute() || VarSubst::RuntimeSourced(Name)) return Dir / P;
    return PathUnder(Dir, Name, Cid, *Claims(Dir));
}

}
