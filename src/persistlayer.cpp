#include "persistlayer.h"
#include "commonutils.h"   // Log*

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>

namespace {
std::string LowerAscii(std::string S) { for (char &C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); return S; }
}

//Seeds each single-file persist from its durable home (UserDataPath/<Target>) into WriteLayerPath/<Path> before
//the union mounts, so the persisted file shadows the lower read-only layers. Single files are copied (not unioned
//live): a file layer can't merge, and copy mirrors the registry model. Durable name (Target) is decoupled from the
//runtime location (Path). No-op when nothing is stored yet.
bool PersistLayer::SeedPersistFiles(struct ContainerParams &ContainerParams)
{
    std::error_code ec;
    bool Ok = true;
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        if (HasWildcard(F.Path))
        {
            //A pattern (player*.hki): its durable home is a directory of the matched files, each seeded beside the
            //game under its own name. Each name seeded is recorded: capture may drop a stored file only when this
            //session put it there and the game then removed it.
            const std::filesystem::path Home = ContainerParams.UserDataPath / F.Target;
            const std::filesystem::path Pattern(F.Path);
            if (!std::filesystem::is_directory(Home, ec)) continue;                     //nothing persisted yet
            std::filesystem::directory_iterator E(Home, ec), End;
            for (; !ec && E != End; E.increment(ec))
            {
                const std::string Name = E->path().filename().string();
                std::error_code Fe;
                if (!E->is_regular_file(Fe) || !WildcardMatch(Pattern.filename().string(), Name)) continue;
                const std::filesystem::path Dst = ContainerParams.WriteLayerPath / Pattern.parent_path() / Name;
                std::filesystem::create_directories(Dst.parent_path(), Fe);
                std::filesystem::copy_file(E->path(), Dst, std::filesystem::copy_options::overwrite_existing, Fe);
                if (Fe) { LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + Name + " (" + F.Path + "): " + Fe.message()); Ok = false; continue; }
                ContainerParams.SeededPatternFiles[F.Path].insert(LowerAscii(Name));
                LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + Name + " (" + F.Path + ", from " + F.Target + ")");
            }
            if (ec) { LogWarn("PersistLayer::SeedPersistFiles", "Could not list " + Home.string() + ": " + ec.message()); Ok = false; ec.clear(); }
            continue;
        }
        const std::filesystem::path SrcFile = ContainerParams.UserDataPath  / F.Target; //durable source
        if (!std::filesystem::exists(SrcFile)) continue;                                 //nothing persisted yet
        const std::filesystem::path DstFile = ContainerParams.WriteLayerPath / F.Path;   //shadow at the runtime location
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + F.Path + ": " + ec.message()); Ok = false; }
        else    LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + F.Path + " (from " + F.Target + ")");
    }
    return Ok;
}

//Captures each single-file persist by copying RuntimePath/<Path> into its durable home UserDataPath/<Target>.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped, mirroring CapturePersistRegistry.
bool PersistLayer::CapturePersistFiles(struct ContainerParams &ContainerParams)
{
    std::error_code ec;
    bool Ok = true;
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        if (HasWildcard(F.Path))
        {
            //A pattern: every matching file the session left beside the game goes to the durable directory. A stored
            //file is dropped only when THIS session seeded it and the game then removed it (a deleted profile) — and
            //only after a listing with no error at all: a file merely missing from a partial listing, or one whose
            //seed failed, is never taken for deleted. Names compare ignoring case (the game's file system does): a
            //stored name differing only in case is replaced, never kept beside the new one.
            const std::filesystem::path Pattern(F.Path);
            const std::string Glob = Pattern.filename().string();
            const std::filesystem::path Dir = ContainerParams.RuntimePath / Pattern.parent_path();
            const std::filesystem::path Home = ContainerParams.UserDataPath / F.Target;
            if (!std::filesystem::is_directory(Dir, ec)) { ec.clear(); continue; }       //no runtime to read: keep the store
            bool Clean = true;
            std::set<std::string> Present;
            std::filesystem::directory_iterator E(Dir, ec), End;
            for (; !ec && E != End; E.increment(ec))
            {
                const std::string Name = E->path().filename().string();
                std::error_code Fe;
                const bool Regular = E->is_regular_file(Fe);
                if (Fe) { Clean = false; continue; }
                if (!Regular || !WildcardMatch(Glob, Name)) continue;
                Present.insert(LowerAscii(Name));
                std::filesystem::create_directories(Home, Fe);
                std::filesystem::copy_file(E->path(), Home / Name, std::filesystem::copy_options::overwrite_existing, Fe);
                if (Fe) { LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + Name + " (" + F.Path + "): " + Fe.message()); Ok = false; Clean = false; continue; }
                LogOut("PersistLayer::CapturePersistFiles", "Captured file " + Name + " (" + F.Path + ", to " + F.Target + ")");
                //Only once the new copy is safely stored: a stored name differing only in case goes — unless it IS the
                //new file (a case-insensitive file system), which the copy just overwrote.
                std::error_code He;
                for (std::filesystem::directory_iterator H(Home, He); !He && H != std::filesystem::directory_iterator(); H.increment(He))
                {
                    const std::string Old = H->path().filename().string();
                    std::error_code Qe;
                    if (Old != Name && LowerAscii(Old) == LowerAscii(Name) && !std::filesystem::equivalent(H->path(), Home / Name, Qe) && !Qe)
                    { std::error_code Re; std::filesystem::remove(H->path(), Re); }
                }
            }
            if (ec) { LogWarn("PersistLayer::CapturePersistFiles", "Could not list " + Dir.string() + ": " + ec.message()); Ok = false; Clean = false; ec.clear(); }
            const auto Seeded = ContainerParams.SeededPatternFiles.find(F.Path);
            if (!Clean || Seeded == ContainerParams.SeededPatternFiles.end()) continue;
            std::error_code He;
            for (std::filesystem::directory_iterator H(Home, He); !He && H != std::filesystem::directory_iterator(); H.increment(He))
            {
                const std::string Name = H->path().filename().string(), Low = LowerAscii(Name);
                if (!WildcardMatch(Glob, Name) || !Seeded->second.count(Low) || Present.count(Low)) continue;
                std::error_code Re;
                std::filesystem::remove(H->path(), Re);
                LogOut("PersistLayer::CapturePersistFiles", "Dropped stored " + Name + " (deleted this session)");
            }
            continue;
        }
        const std::filesystem::path SrcFile = ContainerParams.RuntimePath  / F.Path;   //session result in the mounted union
        if (!std::filesystem::exists(SrcFile)) continue;                                //game never created it
        const std::filesystem::path DstFile = ContainerParams.UserDataPath / F.Target; //durable home
        std::filesystem::create_directories(DstFile.parent_path(), ec);
        std::filesystem::copy_file(SrcFile, DstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + F.Path + ": " + ec.message()); Ok = false; }
        else    LogOut("PersistLayer::CapturePersistFiles", "Captured file " + F.Path + " (to " + F.Target + ")");
    }
    return Ok;
}
bool PersistLayer::HasSavedCopy(const struct ContainerParams &ContainerParams, const std::string &RuntimeRel)
{
    const auto Norm = [](std::string P) {
        std::replace(P.begin(), P.end(), '\\', '/');
        while (!P.empty() && P.front() == '/') P.erase(P.begin());
        while (!P.empty() && P.back() == '/') P.pop_back();
        return P;
    };
    const std::string Rel = Norm(RuntimeRel);
    std::error_code Ec;
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        if (Norm(F.Path) == Rel) return std::filesystem::exists(ContainerParams.UserDataPath / F.Target, Ec);
        const std::filesystem::path Pattern(Norm(F.Path)), R(Rel);
        if (HasWildcard(F.Path) && R.parent_path() == Pattern.parent_path() && WildcardMatch(Pattern.filename().string(), R.filename().string()))
            return std::filesystem::exists(ContainerParams.UserDataPath / F.Target / R.filename(), Ec);
    }
    //The most specific kept directory holding it decides.
    const PersistTarget *Best = nullptr;
    size_t BestLen = 0;
    for (const PersistTarget &D : ContainerParams.KeepDirs)
    {
        const std::string Dp = Norm(D.Path);
        if ((Dp.empty() || Rel.rfind(Dp + "/", 0) == 0) && (!Best || Dp.size() > BestLen)) { Best = &D; BestLen = Dp.size(); }
    }
    if (!Best) return false;
    const std::string Rest = BestLen == 0 ? Rel : Rel.substr(BestLen + 1);
    return std::filesystem::exists(ContainerParams.UserDataPath / Best->Target / Rest, Ec);
}
