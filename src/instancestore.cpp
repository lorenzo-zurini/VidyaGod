#include "instancestore.h"
#include "apppaths.h"
#include "commonutils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <utility>
#ifdef _WIN32
#include <process.h>
#define VG_GETPID _getpid
#else
#include <unistd.h>
#define VG_GETPID getpid
#endif

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace InstanceStore
{

// UTC ISO-8601 ("2026-09-16T12:34:56Z"). Zero-padded UTC, so a lexicographic string compare == chronological
// order — which is what List sorts on and Active picks the max of. Also human-readable for a "last played" label.
static std::string NowIso()
{
    const std::time_t T = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm Tm{};
#if defined(_WIN32)
    gmtime_s(&Tm, &T);
#else
    gmtime_r(&T, &Tm);
#endif
    char Buf[32];
    std::strftime(Buf, sizeof(Buf), "%Y-%m-%dT%H:%M:%SZ", &Tm);
    return Buf;
}

std::filesystem::path Root(const nlohmann::ordered_json &Cfg)
{
    if (Cfg.contains("Settings") && Cfg["Settings"].is_object())
    {
        const auto &S = Cfg["Settings"];
        if (S.contains("Paths") && S["Paths"].is_object() && S["Paths"].contains("UserDataRoot")
            && S["Paths"]["UserDataRoot"].is_string() && !std::string(S["Paths"]["UserDataRoot"]).empty())
            return fs::path(std::string(S["Paths"]["UserDataRoot"]));
    }
    return AppPaths::DataRoot() / "USERDATA";
}

std::string SanitizeUid(const std::string &PackageUID)
{
    if (PackageUID.empty() || PackageUID == "." || PackageUID == "..") return "_";
    std::string Out;
    Out.reserve(PackageUID.size());
    for (const unsigned char C : PackageUID)
        Out.push_back((std::isalnum(C) || C == '.' || C == '_' || C == '-') ? static_cast<char>(C) : '_');
    return (Out == "." || Out == "..") ? std::string("_") : Out;   // can't end up as a traversal segment
}

std::filesystem::path PackageDir(const nlohmann::ordered_json &Cfg, const std::string &PackageUID)
{
    return Root(Cfg) / SanitizeUid(PackageUID);   // sanitized: a peer-authored UID can never escape <root>
}
std::filesystem::path InstanceDir(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance)
{
    return PackageDir(Cfg, PackageUID) / Instance;
}
std::filesystem::path ConfigPath(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance)
{
    return InstanceDir(Cfg, PackageUID, Instance) / ConfigFile;
}

bool ValidName(const std::string &Name)
{
    if (Name.empty() || Name.size() > 64) return false;
    if (Name == "." || Name == "..") return false;
    for (const unsigned char C : Name)
        if (C == '/' || C == '\\' || C < 0x20 || C == 0x7f) return false;
    return true;
}

std::vector<std::string> List(const nlohmann::ordered_json &Cfg, const std::string &PackageUID)
{
    std::vector<std::pair<std::string, std::string>> Found;   // (name, LASTRUN)
    std::error_code Ec;
    const fs::path Pd = PackageDir(Cfg, PackageUID);
    for (auto It = fs::directory_iterator(Pd, Ec); !Ec && It != fs::directory_iterator(); It.increment(Ec))
    {
        std::error_code Dc;
        if (!It->is_directory(Dc)) continue;
        const std::string Name = It->path().filename().string();
        if (!ValidName(Name)) continue;                       // ignore a stray non-instance entry
        // Explicit name → ReadConfig does not re-enter Active(), so no recursion.
        const json C = ReadConfig(Cfg, PackageUID, Name);
        Found.emplace_back(Name, C.value("LASTRUN", std::string()));
    }
    // Newest LASTRUN first; an empty/absent LASTRUN (never launched) sorts last; name breaks ties for determinism.
    std::sort(Found.begin(), Found.end(), [](const auto &A, const auto &B) {
        if (A.second != B.second) return A.second > B.second;
        return A.first < B.first;
    });
    std::vector<std::string> Out;
    Out.reserve(Found.size());
    for (auto &P : Found) Out.push_back(std::move(P.first));
    return Out;
}

std::string Active(const nlohmann::ordered_json &Cfg, const std::string &PackageUID)
{
    const std::vector<std::string> L = List(Cfg, PackageUID);
    if (!L.empty()) return L.front();
    std::string Err;                                          // none yet → materialise DefaultInstance
    if (!Create(Cfg, PackageUID, DefaultInstance, &Err))
        LogWarn("InstanceStore::Active", "could not auto-create DefaultInstance for " + PackageUID + " (" + Err + ")");
    return DefaultInstance;
}

std::string ResolveActive(const nlohmann::ordered_json &Cfg, const std::string &PackageUID)
{
    const std::vector<std::string> L = List(Cfg, PackageUID);
    return L.empty() ? std::string(DefaultInstance) : L.front();
}

nlohmann::ordered_json ReadConfig(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance)
{
    std::string Inst = Instance;
    if (Inst.empty())
    {
        // Empty ⇒ the active instance, but a READ must never CREATE one (a passive display read of a never-launched
        // game would otherwise litter DefaultInstance dirs). Peek the newest without auto-creating; none ⇒ {}.
        const std::vector<std::string> L = List(Cfg, PackageUID);
        if (L.empty()) return json::object();
        Inst = L.front();
    }
    std::ifstream In(ConfigPath(Cfg, PackageUID, Inst));
    if (!In) return json::object();                            // no config yet — quietly empty (never launched / display read)
    json J = json::parse(In, nullptr, /*allow_exceptions=*/false);
    if (J.is_discarded())                                      // file EXISTS but is corrupt: say so — a silent {} looks like a settings reset
        LogWarn("InstanceStore::ReadConfig", "instance config for " + PackageUID + "/" + Inst
                + " did not parse — treating as empty (settings will look reset). The file is NOT overwritten until a new write.");
    return J.is_object() ? json(std::move(J)) : json::object();
}

bool WriteConfig(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance,
                 const nlohmann::ordered_json &Data, std::string *Error)
{
    const std::string Inst = Instance.empty() ? Active(Cfg, PackageUID) : Instance;
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; LogErr("InstanceStore::WriteConfig", M); return false; };
    const fs::path Dir = InstanceDir(Cfg, PackageUID, Inst);
    std::error_code Ec;
    fs::create_directories(Dir, Ec);
    if (Ec) return Fail("could not create " + Dir.string() + ": " + Ec.message());
    const fs::path Path = Dir / ConfigFile;
    // UNIQUE temp per (pid, sequence): a fixed "instance.json.tmp" let two concurrent writers (GUI + a CLI launch,
    // or the migration racing the app) clobber each other's partial write and publish garbage on rename.
    static std::atomic<unsigned long long> TmpSeq{0};
    const fs::path Tmp = Dir / (std::string(ConfigFile) + "." + std::to_string(VG_GETPID())
                               + "." + std::to_string(TmpSeq.fetch_add(1)) + ".tmp");
    {
        std::ofstream Out(Tmp, std::ios::trunc);
        if (!Out) return Fail("could not open " + Tmp.string());
        Out << Data.dump(2);
        if (!Out.good()) { fs::remove(Tmp, Ec); return Fail("write failed for " + Tmp.string() + " (disk full?)"); }
    }
    fs::rename(Tmp, Path, Ec);
    if (Ec) { fs::remove(Tmp, Ec); return Fail("could not replace " + Path.string() + ": " + Ec.message()); }
    return true;
}

bool Create(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Name, std::string *Error)
{
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; return false; };
    if (!ValidName(Name)) return Fail("invalid instance name: '" + Name + "'");
    std::error_code Ec;
    if (fs::exists(InstanceDir(Cfg, PackageUID, Name), Ec)) return Fail("instance already exists: " + Name);
    json C = json::object();
    C["LASTRUN"] = NowIso();                                  // a new instance is the freshest → becomes active
    return WriteConfig(Cfg, PackageUID, Name, C, Error);
}

bool Rename(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &From, const std::string &To, std::string *Error)
{
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; return false; };
    if (!ValidName(To)) return Fail("invalid instance name: '" + To + "'");
    if (From == To) return true;
    std::error_code Ec;
    if (!fs::exists(InstanceDir(Cfg, PackageUID, From), Ec)) return Fail("no such instance: " + From);
    if (fs::exists(InstanceDir(Cfg, PackageUID, To), Ec))    return Fail("instance already exists: " + To);
    fs::rename(InstanceDir(Cfg, PackageUID, From), InstanceDir(Cfg, PackageUID, To), Ec);
    return Ec ? Fail("rename failed: " + Ec.message()) : true;
}

bool Delete(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Name, std::string *Error)
{
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; return false; };
    if (!ValidName(Name)) return Fail("invalid instance name: '" + Name + "'");   // never rm-rf a path we didn't validate
    std::error_code Ec;
    const fs::path Dir = InstanceDir(Cfg, PackageUID, Name);
    if (!fs::exists(Dir, Ec)) return true;                    // already gone
    fs::remove_all(Dir, Ec);
    return Ec ? Fail("delete failed: " + Ec.message()) : true;
}

bool Clone(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &From, const std::string &To, std::string *Error)
{
    auto Fail = [&](const std::string &M) { if (Error) *Error = M; return false; };
    if (!ValidName(To)) return Fail("invalid instance name: '" + To + "'");
    std::error_code Ec;
    if (!fs::exists(InstanceDir(Cfg, PackageUID, From), Ec)) return Fail("no such instance: " + From);
    if (fs::exists(InstanceDir(Cfg, PackageUID, To), Ec))    return Fail("instance already exists: " + To);
    fs::copy(InstanceDir(Cfg, PackageUID, From), InstanceDir(Cfg, PackageUID, To),
             fs::copy_options::recursive | fs::copy_options::copy_symlinks, Ec);
    if (Ec) return Fail("clone failed: " + Ec.message());
    TouchLastRun(Cfg, PackageUID, To);                       // the clone is the freshest → becomes active
    return true;
}

void TouchLastRun(const nlohmann::ordered_json &Cfg, const std::string &PackageUID, const std::string &Instance)
{
    const std::string Inst = Instance.empty() ? Active(Cfg, PackageUID) : Instance;
    json C = ReadConfig(Cfg, PackageUID, Inst);
    C["LASTRUN"] = NowIso();
    std::string Err;
    if (!WriteConfig(Cfg, PackageUID, Inst, C, &Err))
        LogWarn("InstanceStore::TouchLastRun", "could not stamp LASTRUN for " + PackageUID + "/" + Inst + " (" + Err + ")");
}

} // namespace InstanceStore
