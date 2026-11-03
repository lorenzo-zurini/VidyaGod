#include "persistlayer.h"
#include "commonutils.h"   // Log*

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {
std::string LowerAscii(std::string S) { for (char &C : S) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C))); return S; }

namespace fs = std::filesystem;

//A link where this process is about to write goes: the link itself is removed, never what it points at.
bool RemoveLink(const fs::path &P, std::string &Why)
{
    std::error_code Ec;
    const fs::path To = fs::read_symlink(P, Ec);
    if (!fs::remove(P, Ec) || Ec) { Why = "could not remove the link " + P.string() + ": " + (Ec ? Ec.message() : "gone"); return false; }
    LogWarn("PersistLayer", "Removed a link at " + PersistLayer::Printable(P.string()) + " (to " + PersistLayer::Printable(To.string())
                            + ") — kept data is written beside the game, never through a link");
    return true;
}

//Root/Rel as a chain of real folders: each component created when missing, a link in its place replaced by a folder.
//Root is the trusted anchor (the instance folder, the writelayer), created when missing.
bool RealFolders(const fs::path &Root, const fs::path &Rel, fs::path &Out, std::string &Why)
{
    std::error_code Ec;
    if (Rel.is_absolute() || Rel.has_root_name()) { Why = "not a relative path: " + Rel.string(); return false; }
    fs::create_directories(Root, Ec);
    if (Ec) { Why = "could not create " + Root.string() + ": " + Ec.message(); return false; }
    Out = Root;
    for (const fs::path &Part : Rel)
    {
        if (Part.empty() || Part == ".") continue;
        if (Part == "..") { Why = "the path climbs out of " + Root.string() + ": " + Rel.string(); return false; }
        Out /= Part;
        const fs::file_type T = fs::symlink_status(Out, Ec).type();
        if (T == fs::file_type::directory) continue;
        if (T == fs::file_type::symlink && !RemoveLink(Out, Why)) return false;
        if (T != fs::file_type::symlink && T != fs::file_type::not_found) { Why = Out.string() + " is not a folder"; return false; }
        if (!fs::create_directory(Out, Ec) || Ec) { Why = "could not create " + Out.string() + ": " + (Ec ? Ec.message() : "it exists"); return false; }
    }
    return true;
}

//A plain file (never a link to one): the only thing seeded from or captured into a store.
bool IsPlainFile(const fs::path &P)
{
    std::error_code Ec;
    return fs::symlink_status(P, Ec).type() == fs::file_type::regular;
}

//Copies Src to Root/Rel, never writing through a link (PrepareWritePath). The copy is the user's own file: readable
//and writable by them whatever mode its source had. A file the game never rewrote is served from a read-only package
//layer (vidyagodfs shows those r-x), and copying that mode stored it read-only: the next session was seeded with a
//file the game could not open for writing (Duke Nukem 3D: "Error opening DUKE3D.CFG: Permission denied"), and the
//next capture could not overwrite the stored copy.
//The copy goes to a temporary file beside the destination and is renamed over it only once complete and writable:
//copying over the destination truncates it first, so a copy failing part way (a full disk, a read error on the
//overlay) would have destroyed the previous save too. The rename replaces a link at the destination rather than
//writing through it.
bool CopyIntoPlace(const fs::path &Src, const fs::path &Root, const fs::path &Rel, std::string &Why)
{
    if (!PersistLayer::PrepareWritePath(Root, Rel, Why)) return false;
    const fs::path Dst = Root / Rel;
    const fs::path Tmp = Dst.parent_path() / (Dst.filename().string() + ".vgcopy");
    const auto Fail = [&](const std::string &What, const std::error_code &E) {
        std::error_code Re; fs::remove(Tmp, Re);
        Why = What + ": " + E.message(); return false;
    };
    std::error_code Ec;
    fs::remove(Tmp, Ec);                                     //a leftover of an interrupted copy (a link is removed, not followed)
    if (Ec) return Fail("could not clear " + Tmp.string(), Ec);
    fs::copy_file(Src, Tmp, Ec);
    if (Ec) return Fail("could not copy to " + Tmp.string(), Ec);
    if (fs::symlink_status(Tmp, Ec).type() != fs::file_type::regular) return Fail(Tmp.string() + " is not the copy", std::make_error_code(std::errc::file_exists));
    fs::permissions(Tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, Ec);
    if (Ec) return Fail("could not make " + Tmp.string() + " writable", Ec);
#ifdef _WIN32
    //Windows refuses to rename over a read-only file (an older build stored them so); POSIX renames over any mode.
    if (fs::symlink_status(Dst, Ec).type() == fs::file_type::regular)
        fs::permissions(Dst, fs::perms::owner_write, fs::perm_options::add, Ec);
    Ec.clear();
#endif
    fs::rename(Tmp, Dst, Ec);
    if (Ec) return Fail("could not replace " + Dst.string(), Ec);
    return true;
}
}

std::string PersistLayer::Printable(std::string Name)
{
    for (char &Ch : Name)
        if (static_cast<unsigned char>(Ch) < 0x20 || Ch == 0x7f) Ch = '?';
    return Name;
}

bool PersistLayer::PrepareWritePath(const std::filesystem::path &Root, const std::filesystem::path &RelFile, std::string &Why)
{
    const fs::path Name = RelFile.filename();
    if (Name.empty() || Name == "." || Name == "..") { Why = "not a file path: " + RelFile.string(); return false; }
    fs::path Dir;
    if (!RealFolders(Root, RelFile.parent_path(), Dir, Why)) return false;
    std::error_code Ec;
    const fs::file_type T = fs::symlink_status(Dir / Name, Ec).type();
    if (T == fs::file_type::symlink) return RemoveLink(Dir / Name, Why);
    if (T == fs::file_type::regular || T == fs::file_type::not_found) return true;
    Why = (Dir / Name).string() + " is not a plain file";
    return false;
}

bool PersistLayer::PrepareWriteDir(const std::filesystem::path &Root, const std::filesystem::path &RelDir, std::string &Why)
{
    fs::path Dir;
    if (!RealFolders(Root, RelDir, Dir, Why)) return false;
    std::error_code Ec;
    std::vector<fs::path> Links, Others;
    for (fs::directory_iterator E(Dir, Ec), End; !Ec && E != End; E.increment(Ec))
    {
        std::error_code Le;
        const fs::file_type T = E->symlink_status(Le).type();
        if (T == fs::file_type::symlink) Links.push_back(E->path());
        else if (T != fs::file_type::regular && T != fs::file_type::directory) Others.push_back(E->path());
    }
    if (Ec) { Why = "could not list " + Dir.string() + ": " + Ec.message(); return false; }
    for (const fs::path &L : Links)
        if (!RemoveLink(L, Why)) return false;
    for (const fs::path &O : Others)
    {
        if (!fs::remove(O, Ec) || Ec) { Why = "could not remove " + Printable(O.string()) + ": " + (Ec ? Ec.message() : "gone"); return false; }
        LogWarn("PersistLayer", "Removed " + Printable(O.string()) + ", neither a file nor a folder");
    }
    return true;
}

//Seeds each single-file persist from its durable home (UserDataPath/<Target>) into WriteLayerPath/<Path> before
//the union mounts, so the persisted file shadows the lower read-only layers. Single files are copied (not unioned
//live): a file layer can't merge, and copy mirrors the registry model. Durable name (Target) is decoupled from the
//runtime location (Path). No-op when nothing is stored yet.
bool PersistLayer::SeedPersistFiles(struct ContainerParams &ContainerParams)
{
    std::error_code ec;
    bool Ok = true;
    //Anchored at the writelayer's parent: the writelayer itself sits in the session tree the game could write, so it
    //can be a link left by a session whose tree stayed in place (a busy mount), and is replaced like any folder.
    const std::filesystem::path WlRoot = ContainerParams.WriteLayerPath.parent_path(), WlName = ContainerParams.WriteLayerPath.filename();
    for (const PersistTarget &F : ContainerParams.KeepFiles)
    {
        if (HasWildcard(F.Path))
        {
            //A pattern (player*.hki): its durable home is a directory of the matched files, each seeded beside the
            //game under its own name. Each name seeded is recorded: capture may drop a stored file only when this
            //session put it there and the game then removed it.
            const std::filesystem::path Home = ContainerParams.UserDataPath / F.Target;
            const std::filesystem::path Pattern(F.Path);
            //Plain folders and files only: the store never holds a link (capture replaces one), so one here is left
            //unread rather than followed.
            if (std::filesystem::symlink_status(Home, ec).type() != std::filesystem::file_type::directory) { ec.clear(); continue; }   //nothing persisted yet
            std::filesystem::directory_iterator E(Home, ec), End;
            for (; !ec && E != End; E.increment(ec))
            {
                const std::string Name = E->path().filename().string();
                std::string Why;
                if (!IsPlainFile(E->path()) || !WildcardMatch(Pattern.filename().string(), Name)) continue;
                if (!CopyIntoPlace(E->path(), WlRoot, WlName / Pattern.parent_path() / Name, Why))
                { LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + Name + " (" + F.Path + "): " + Why); Ok = false; continue; }
                ContainerParams.SeededPatternFiles[F.Path].insert(LowerAscii(Name));
                LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + Name + " (" + F.Path + ", from " + F.Target + ")");
            }
            if (ec) { LogWarn("PersistLayer::SeedPersistFiles", "Could not list " + Home.string() + ": " + ec.message()); Ok = false; ec.clear(); }
            continue;
        }
        const std::filesystem::path SrcFile = ContainerParams.UserDataPath  / F.Target; //durable source
        if (!IsPlainFile(SrcFile))                                                       //nothing persisted yet, or a link: never followed
        {
            std::error_code Le;
            const std::filesystem::file_type T = std::filesystem::symlink_status(SrcFile, Le).type();
            if (T != std::filesystem::file_type::not_found)
            {
                LogWarn("PersistLayer::SeedPersistFiles", "The stored " + F.Target + " is " + (T == std::filesystem::file_type::symlink
                        ? "a link" : "not a file") + " — not seeded (" + F.Path + ")");
                Ok = false;
            }
            continue;
        }
        std::string Why;                                                                 //shadow at the runtime location
        if (!CopyIntoPlace(SrcFile, WlRoot, WlName / F.Path, Why))
        { LogWarn("PersistLayer::SeedPersistFiles", "Could not seed " + F.Path + ": " + Why); Ok = false; }
        else    LogOut("PersistLayer::SeedPersistFiles", "Seeded persisted file " + F.Path + " (from " + F.Target + ")");
    }
    return Ok;
}

//Captures each single-file persist by copying RuntimePath/<Path> into its durable home UserDataPath/<Target>.
//Runs during Cleanup BEFORE the runtime is unmounted/wiped, mirroring CapturePersistRegKeys.
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
                const std::filesystem::file_type Type = E->symlink_status(Fe).type();   //a link the game left is not its save
                if (Fe) { Clean = false; continue; }
                if (Type != std::filesystem::file_type::regular || !WildcardMatch(Glob, Name)) continue;
                Present.insert(LowerAscii(Name));
                std::string Why;
                if (!CopyIntoPlace(E->path(), ContainerParams.UserDataPath, std::filesystem::path(F.Target) / Name, Why))
                { LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + Name + " (" + F.Path + "): " + Why); Ok = false; Clean = false; continue; }
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
        if (!IsPlainFile(SrcFile))                                                      //game never created it, or left a link: not a save
        {
            if (std::error_code Le; std::filesystem::is_symlink(SrcFile, Le))
                LogWarn("PersistLayer::CapturePersistFiles", "The game left a link at " + F.Path + ", not a file — not kept");
            continue;
        }
        std::string Why;                                                                //durable home
        if (!CopyIntoPlace(SrcFile, ContainerParams.UserDataPath, F.Target, Why))
        { LogWarn("PersistLayer::CapturePersistFiles", "Could not capture " + F.Path + ": " + Why); Ok = false; }
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
    const std::filesystem::path Root = ContainerParams.UserDataPath / Best->Target;
    if (std::filesystem::exists(Root / Rest, Ec)) return true;
    //A file the game deleted inside a kept folder is the user's too: vidyagodfs leaves a `.wh.<name>` marker there
    //(or `.wh.<ancestor>`, or `.wh..wh..opq` in an ancestor made again) that hides the package's copy for good, so a
    //package default written beneath it would never be seen.
    std::filesystem::path Cur = Root;
    if (std::filesystem::exists(Cur / ".wh..wh..opq", Ec)) return true;
    for (const auto &Part : std::filesystem::path(Rest))
    {
        if (std::filesystem::exists(Cur / (".wh." + Part.string()), Ec)) return true;
        Cur /= Part;
        if (Cur != Root / Rest && std::filesystem::exists(Cur / ".wh..wh..opq", Ec)) return true;
    }
    return false;
}
