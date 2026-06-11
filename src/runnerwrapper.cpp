#include "runnerwrapper.h"
#include "ipfswrapper.h"

#include <QDir>
#include <QFileInfo>

namespace RunnerWrapper {

std::string ArtifactDir(const std::string &RunnerId)
{
    if (RunnerId.empty()) return std::string();
    return QDir::cleanPath(QDir::homePath() + "/.VidyaGod/DOWNLOADS/runners/" + QString::fromStdString(RunnerId)).toStdString();
}

std::string DefPrefixArtifact(const std::string &RunnerId)
{
    const std::string A = ArtifactDir(RunnerId);
    return A.empty() ? std::string() : (A + "/DEFPREFIX");
}

std::string RunnerId(const nlohmann::ordered_json &RunnerPkg)
{
    if (RunnerPkg.contains("RUNNERS") && RunnerPkg["RUNNERS"].is_array() && !RunnerPkg["RUNNERS"].empty())
        return RunnerPkg["RUNNERS"][0].value("RUNNER_ID", std::string());
    return std::string();
}

bool IsWineRunner(const nlohmann::ordered_json &RunnerPkg)
{
    if (RunnerPkg.contains("RUNNERS") && RunnerPkg["RUNNERS"].is_array() && !RunnerPkg["RUNNERS"].empty())
        return RunnerPkg["RUNNERS"][0].value("TYPE", std::string()) == "wine";
    return false;
}

std::vector<std::string> BuildCids(const nlohmann::ordered_json &RunnerPkg)
{
    std::vector<std::string> Cids;
    if (!RunnerPkg.contains("COMPONENTS") || !RunnerPkg["COMPONENTS"].is_array()) return Cids;
    for (const auto &C : RunnerPkg["COMPONENTS"])
    {
        if (!C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
        {
            if (S.value("TYPE", std::string()) != "VFSZipLayer") continue;
            if (!S.contains("SOURCE") || !S["SOURCE"].is_object()) continue;
            const auto &Src = S["SOURCE"];
            if (Src.value("TYPE", std::string()) != "ipfs") continue;
            const std::string Cid = Src.value("CID", std::string());
            if (!Cid.empty()) Cids.push_back(Cid);
        }
    }
    return Cids;
}

bool ShipsBuild(const nlohmann::ordered_json &RunnerPkg)
{
    return !BuildCids(RunnerPkg).empty();
}

bool IsInstalled(const nlohmann::ordered_json &RunnerPkg)
{
    if (!ShipsBuild(RunnerPkg)) return true;                       // PATH runner — nothing to install
    for (const std::string &Cid : BuildCids(RunnerPkg))
        if (!IpfsWrapper::IsCached(Cid)) return false;            // build not fetched yet
    if (IsWineRunner(RunnerPkg))
    {
        const QString Pfx = QString::fromStdString(DefPrefixArtifact(RunnerId(RunnerPkg)));
        QDir D(Pfx);
        if (!D.exists() || D.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) return false; // prefix not built
    }
    return true;
}

} // namespace RunnerWrapper
