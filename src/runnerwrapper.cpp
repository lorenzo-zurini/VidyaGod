#include "runnerwrapper.h"
#include "ipfswrapper.h"

#include <QDir>
#include <QFileInfo>

namespace RunnerWrapper {

// The package's runner entity (RUNNERS[0]) — runner packages carry a single runner entity.
static const nlohmann::ordered_json * Entity(const nlohmann::ordered_json &RunnerPkg)
{
    if (RunnerPkg.contains("RUNNERS") && RunnerPkg["RUNNERS"].is_array() && !RunnerPkg["RUNNERS"].empty())
        return &RunnerPkg["RUNNERS"][0];
    return nullptr;
}

std::string ArtifactDir(const std::string &RunnerId, const std::string &VariantId)
{
    if (RunnerId.empty()) return std::string();
    const std::string V = VariantId.empty() ? std::string("default") : VariantId;
    return QDir::cleanPath(QDir::homePath() + "/.VidyaGod/DOWNLOADS/runners/"
                           + QString::fromStdString(RunnerId) + "/" + QString::fromStdString(V)).toStdString();
}

std::string DefPrefixArtifact(const std::string &RunnerId, const std::string &VariantId)
{
    const std::string A = ArtifactDir(RunnerId, VariantId);
    return A.empty() ? std::string() : (A + "/DEFPREFIX");
}

std::string RunnerId(const nlohmann::ordered_json &RunnerPkg)
{
    const auto *E = Entity(RunnerPkg);
    return E ? E->value("RUNNER_ID", std::string()) : std::string();
}

std::vector<std::string> VariantIds(const nlohmann::ordered_json &RunnerPkg)
{
    std::vector<std::string> Ids;
    const auto *E = Entity(RunnerPkg);
    if (E && E->contains("VARIANTS") && (*E)["VARIANTS"].is_array())
        for (const auto &V : (*E)["VARIANTS"])
            Ids.push_back(V.value("VARIANT_ID", std::string()));
    return Ids;
}

std::string DefaultVariantId(const nlohmann::ordered_json &RunnerPkg)
{
    const auto *E = Entity(RunnerPkg);
    if (!E || !E->contains("VARIANTS") || !(*E)["VARIANTS"].is_array() || (*E)["VARIANTS"].empty()) return std::string();
    for (const auto &V : (*E)["VARIANTS"])
        if (V.value("RECOMMENDED", false)) return V.value("VARIANT_ID", std::string());
    return (*E)["VARIANTS"][0].value("VARIANT_ID", std::string());
}

nlohmann::ordered_json Variant(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId)
{
    const auto *E = Entity(RunnerPkg);
    if (!E || !E->contains("VARIANTS") || !(*E)["VARIANTS"].is_array()) return nlohmann::ordered_json::object();
    const std::string Want = VariantId.empty() ? DefaultVariantId(RunnerPkg) : VariantId;
    for (const auto &V : (*E)["VARIANTS"])
        if (V.value("VARIANT_ID", std::string()) == Want) return V;
    return nlohmann::ordered_json::object();
}

bool IsWineRunner(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId)
{
    return Variant(RunnerPkg, VariantId).value("TYPE", std::string()) == "wine";
}

std::vector<std::string> BuildCids(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId)
{
    std::vector<std::string> Cids;
    const nlohmann::ordered_json V = Variant(RunnerPkg, VariantId);
    if (!V.contains("MODULES") || !V["MODULES"].is_array()) return Cids;
    // The components this variant's MODULES enable (direct references — runner builds are flat).
    std::vector<std::string> Want;
    for (const auto &M : V["MODULES"])
        if (M.is_object()) { std::string C = M.value("COMPONENT", std::string()); if (!C.empty()) Want.push_back(C); }
    if (!RunnerPkg.contains("COMPONENTS") || !RunnerPkg["COMPONENTS"].is_array()) return Cids;
    for (const auto &C : RunnerPkg["COMPONENTS"])
    {
        bool Wanted = false;
        for (const auto &W : Want) if (C.value("COMPONENTID", std::string()) == W) { Wanted = true; break; }
        if (!Wanted || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            if (S.value("TYPE", std::string()) != "VFSZipLayer") continue;
            if (!S.contains("SOURCE") || !S["SOURCE"].is_object() || S["SOURCE"].value("TYPE", std::string()) != "ipfs") continue;
            const std::string Cid = S["SOURCE"].value("CID", std::string());
            if (!Cid.empty()) Cids.push_back(Cid);
        }
    }
    return Cids;
}

bool ShipsBuild(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId)
{
    return !BuildCids(RunnerPkg, VariantId).empty();
}

bool IsInstalled(const nlohmann::ordered_json &RunnerPkg, const std::string &VariantId)
{
    if (!ShipsBuild(RunnerPkg, VariantId)) return true;                  // PATH runner — nothing to install
    for (const std::string &Cid : BuildCids(RunnerPkg, VariantId))
        if (!IpfsWrapper::IsCached(Cid)) return false;                   // build not fetched yet
    if (IsWineRunner(RunnerPkg, VariantId))
    {
        const std::string Vid = VariantId.empty() ? DefaultVariantId(RunnerPkg) : VariantId;
        QDir D(QString::fromStdString(DefPrefixArtifact(RunnerId(RunnerPkg), Vid)));
        if (!D.exists() || D.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) return false; // prefix not built
    }
    return true;
}

} // namespace RunnerWrapper
