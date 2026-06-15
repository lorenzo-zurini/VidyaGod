#include "manifestmodel.h"
#include "commonutils.h"

#include <set>

namespace ManifestModel {

// ----- VFS layer helpers -----

bool IsVfsLayer(const std::string &Type)
{
    return Type == "VFSZipLayer" || Type == "VFSDirLayer" || Type == "VFSFileLayer";
}

std::string LayerType(const nlohmann::ordered_json &Sub)
{
    return Sub.is_object() ? Sub.value("TYPE", std::string()) : std::string();
}

void ForEachVfsLayer(const nlohmann::ordered_json &Components,
                     const std::function<void(const nlohmann::ordered_json &)> &Fn)
{
    if (!Components.is_array()) return;
    for (const auto &C : Components)
    {
        if (!C.is_object() || !C.contains("SUBCOMPONENTS") || !C["SUBCOMPONENTS"].is_array()) continue;
        for (const auto &S : C["SUBCOMPONENTS"])
            if (IsVfsLayer(LayerType(S))) Fn(S);
    }
}

void LayerLocator(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath,
                  std::filesystem::path &Local, std::string &Cid)
{
    std::string PathStr = Sub.value("PATH", std::string());
    Cid.clear();
    if (Sub.contains("SOURCE") && Sub["SOURCE"].is_object())
    {
        const auto &Src = Sub["SOURCE"];
        PathStr = Src.value("PATH", PathStr);                                  // SOURCE.PATH overrides the local path
        if (Src.value("TYPE", std::string("path")) == "ipfs") Cid = Src.value("CID", std::string());
    }
    std::filesystem::path P(PathStr);
    Local = P.is_absolute() ? P : (PackagePath / P);
}

std::string ResolveLayerSource(const nlohmann::ordered_json &Sub, const std::filesystem::path &PackagePath)
{
    std::filesystem::path Local; std::string Cid;
    LayerLocator(Sub, PackagePath, Local, Cid);
    return Local.string();
}

// ----- platform -----

std::string MachinePlatform() { return "linux64"; }
std::string HostPlatform()    { return "linux64"; }

// ----- game / component / variant / module lookups -----

int FindGameIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    for (int i = 0; i < (int)MANIFESTJSON["GAMES"].size(); i++)
        if (!MANIFESTJSON["GAMES"][i]["GAMEID"].is_null() && MANIFESTJSON["GAMES"][i]["GAMEID"] == SubgameID)
            return i;
    LogErr("ManifestModel::FindGameIndex", "Subgame ID not found: " + SubgameID);
    return -1;
}

int FindComponentIndex(const nlohmann::ordered_json &MANIFESTJSON, const std::string &ComponentID)
{
    if (ComponentID.empty()) return -1;
    for (int i = 0; i < (int)MANIFESTJSON["COMPONENTS"].size(); i++)
        if (!MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"].is_null() && MANIFESTJSON["COMPONENTS"][i]["COMPONENTID"] == ComponentID)
            return i;
    LogErr("ManifestModel::FindComponentIndex", "Component ID not found: " + ComponentID);
    return -1;
}

std::vector<ModuleInfo> ParseModules(const nlohmann::ordered_json &ModulesArray)
{
    std::vector<ModuleInfo> Modules;
    if (!ModulesArray.is_array()) return Modules;
    for (const auto &M : ModulesArray)
    {
        if (!M.is_object()) continue;
        ModuleInfo Info;
        Info.Component = M.value("COMPONENT", std::string());
        if (Info.Component.empty()) continue;
        Info.Label    = M.value("LABEL", std::string());
        Info.Required = M.value("REQUIRED", true);
        Info.Default  = M.value("DEFAULT", true);
        if (M.contains("EXCLUDE") && M["EXCLUDE"].is_array())
            for (const auto &E : M["EXCLUDE"])
                if (E.is_string() && !std::string(E).empty()) Info.Exclude.push_back(std::string(E));
        Modules.push_back(std::move(Info));
    }
    return Modules;
}

std::vector<VariantInfo> GetAvailableVariants(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID)
{
    std::vector<VariantInfo> Variants;
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return Variants;
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return Variants;
    for (auto &V : Subgame["VARIANTS"])
    {
        VariantInfo Info;
        Info.VariantID     = V.value("VARIANT_ID", std::string());
        Info.Name          = V.value("NAME", Info.VariantID);
        Info.IsRecommended = V.value("RECOMMENDED", false);
        Info.HostPlatform  = V.value("HOST_PLATFORM", std::string());
        if (V.contains("GUEST_PLATFORM") && V["GUEST_PLATFORM"].is_array())   // runner variants only
            for (const auto &P : V["GUEST_PLATFORM"]) if (P.is_string()) Info.GuestPlatform.push_back(std::string(P));
        Info.Modules       = ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
        Variants.push_back(std::move(Info));
    }
    return Variants;
}

std::vector<ModuleInfo> GetVariantModules(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    int SubgameIdx = FindGameIndex(MANIFESTJSON, SubgameID);
    if (SubgameIdx == -1) return {};
    auto &Subgame = MANIFESTJSON["GAMES"][SubgameIdx];
    if (!Subgame.contains("VARIANTS") || !Subgame["VARIANTS"].is_array()) return {};
    for (auto &V : Subgame["VARIANTS"])
        if (V.value("VARIANT_ID", std::string()) == VariantID)
            return ParseModules(V.value("MODULES", nlohmann::ordered_json::array()));
    return {};
}

std::vector<std::string> ResolveEnabledModules(const std::vector<ModuleInfo> &Modules,
                                               const std::map<std::string, bool> &ModuleStates,
                                               const nlohmann::ordered_json &MANIFESTJSON)
{
    std::map<std::string, bool> InModules; // component → is a module here
    for (const auto &M : Modules) InModules[M.Component] = true;

    //All module-ancestors of a component (PARENTCOMPONENT chain entries that are themselves modules).
    auto ModuleAncestors = [&](const std::string &Component) {
        std::vector<std::string> Ancestors;
        int Idx = FindComponentIndex(MANIFESTJSON, Component);
        while (Idx != -1)
        {
            const auto &Comp = MANIFESTJSON["COMPONENTS"][Idx];
            if (!Comp.contains("PARENTCOMPONENT")) break;          // a component may have no parent (root / runner build)
            const auto &ParentField = Comp["PARENTCOMPONENT"];
            if (ParentField.is_null() || ParentField == "") break;
            std::string Parent = ParentField;
            if (InModules.count(Parent)) Ancestors.push_back(Parent);
            Idx = FindComponentIndex(MANIFESTJSON, Parent);
        }
        return Ancestors;
    };

    //Step 1: raw enable.
    std::map<std::string, bool> Enabled;
    for (const auto &M : Modules)
        Enabled[M.Component] = M.Required ? true
                             : (ModuleStates.count(M.Component) ? ModuleStates.at(M.Component) : M.Default);

    //Step 2: a REQUIRED module forces all its module-ancestors enabled (a required child keeps its parents).
    for (const auto &M : Modules)
        if (M.Required)
            for (const auto &A : ModuleAncestors(M.Component)) Enabled[A] = true;

    //Step 2.5: mutual exclusion (EXCLUDE) — symmetric; REQUIRED kept first, then each still-enabled optional in
    //declaration order is dropped if it conflicts with anything kept (first-declared of a set survives).
    std::map<std::string, std::set<std::string>> ExclAdj;
    for (const auto &M : Modules)
        for (const auto &E : M.Exclude) { ExclAdj[M.Component].insert(E); ExclAdj[E].insert(M.Component); }
    if (!ExclAdj.empty())
    {
        std::set<std::string> Kept;
        auto Conflicts = [&](const std::string &C) {
            auto it = ExclAdj.find(C);
            if (it == ExclAdj.end()) return false;
            for (const auto &K : Kept) if (it->second.count(K)) return true;
            return false;
        };
        for (const auto &M : Modules) if (Enabled[M.Component] && M.Required)  Kept.insert(M.Component);
        for (const auto &M : Modules)
        {
            if (!Enabled[M.Component] || M.Required) continue;
            if (Conflicts(M.Component)) Enabled[M.Component] = false;
            else                        Kept.insert(M.Component);
        }
    }

    //Step 3: hierarchy gate — drop any module with a disabled module-ancestor (full chain).
    std::vector<std::string> EnabledComponents;
    for (const auto &M : Modules)
    {
        if (!Enabled[M.Component]) continue;
        bool AncestorsOk = true;
        for (const auto &A : ModuleAncestors(M.Component))
            if (!Enabled[A]) { AncestorsOk = false; break; }
        if (AncestorsOk) EnabledComponents.push_back(M.Component);
    }
    return EnabledComponents;
}

std::vector<std::string> FindEndpointsForVariant(const nlohmann::ordered_json &MANIFESTJSON, const std::string &SubgameID, const std::string &VariantID)
{
    return ResolveEnabledModules(GetVariantModules(MANIFESTJSON, SubgameID, VariantID), {}, MANIFESTJSON);
}

// ----- manifest-only package predicates -----

std::vector<std::string> PackageIpfsCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("COMPONENTS")) return Cids;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!S.contains("SOURCE") || !S["SOURCE"].is_object()) return;
        const auto &Src = S["SOURCE"];
        if (Src.value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Src.value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    });
    return Cids;
}

std::vector<std::string> PackageCoverCids(const nlohmann::ordered_json &Manifest)
{
    std::vector<std::string> Cids;
    std::set<std::string> Seen;
    if (!Manifest.contains("GAMES") || !Manifest["GAMES"].is_array()) return Cids;
    auto Consider = [&](const nlohmann::ordered_json &Holder)
    {
        if (!Holder.contains("COVER") || !Holder["COVER"].is_object()) return;
        const auto &Cv = Holder["COVER"];
        if (!Cv.contains("SOURCE") || !Cv["SOURCE"].is_object() || Cv["SOURCE"].value("TYPE", std::string()) != "ipfs") return;
        const std::string Cid = Cv["SOURCE"].value("CID", std::string());
        if (!Cid.empty() && Seen.insert(Cid).second) Cids.push_back(Cid);
    };
    for (const auto &G : Manifest["GAMES"])
    {
        if (!G.is_object()) continue;
        if (G.contains("METADATA") && G["METADATA"].is_object()) Consider(G["METADATA"]);
        Consider(G);
    }
    return Cids;
}

bool PackageHydrated(const nlohmann::ordered_json &Manifest, const std::string &PackageDir)
{
    if (!Manifest.contains("COMPONENTS")) return true;
    const std::filesystem::path Pkg(PackageDir);
    bool AllPresent = true;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &S){
        if (!AllPresent) return;
        std::filesystem::path Local; std::string Cid;
        LayerLocator(S, Pkg, Local, Cid);
        std::error_code Ec;
        if (!std::filesystem::exists(Local, Ec)) AllPresent = false;            // a layer's content is missing
    });
    return AllPresent;
}

bool PackageHasContent(const nlohmann::ordered_json &Manifest)
{
    if (!Manifest.contains("COMPONENTS")) return false;
    bool Any = false;
    ForEachVfsLayer(Manifest["COMPONENTS"], [&](const nlohmann::ordered_json &){ Any = true; });
    return Any;
}

} // namespace ManifestModel
