#include "builtinrunners.h"
#include "cid.h"

#include <set>

namespace BuiltinRunners {

const std::vector<Runner> &All()
{
    //windows-native: a win32/win64 game on Windows runs as itself. Before it, no runner hosted win64, so a Windows
    //game on a Windows PC had no runner chain at all. Its game folder is its runtime's (MetaPackageFormat §10.4); its
    //profile and system anchors are the machine's real folders (%HostAppData%, …), whose package files its Sandboxie box
    //holds (BoxPrefix), laid out at drive_c by DRIVES.
    static const std::vector<Runner> R = {
        { "windows-native", nlohmann::ordered_json::parse(R"VG({"LABEL":"windows-native_exec","LAYERS":[{"COMMENT":"A Windows game on Windows runs as itself, in a Sandboxie box that is its prefix. Its game folder is the mounted runtime's (%GameDir% is <runtime>\\drive_c\\<uid>), served to the box as it is. Its profile and system anchors are the machine's own folders, as the game reads them (%AppData% is the real AppData): what a package places there is laid out at drive_c in the runtime by DRIVES and copied into the box before the game starts, where the boxed game finds it at the real path; what it keeps there is copied back out after.","EXEC":[{"ARGS":[],"CONTENT_ROOT":"drive_c/%PackageUID%","DRIVES":{"C:":"drive_c"},"EXE":"%Content%","GUEST":["win32","win64"],"GUEST_ROOTS":{"%AppData%":"%HostAppData%","%Documents%":"%HostDocuments%","%GameDir%":"%RuntimePath%\\drive_c\\%PackageUID%","%LocalAppData%":"%HostLocalAppData%","%ProgramData%":"%HostProgramData%","%ProgramFiles%":"%HostProgramFiles%","%ProgramFiles32%":"%HostProgramFiles32%","%SysDir32%":"%HostSysDir32%","%SysDir64%":"%HostSysDir64%","%UserProfile%":"%HostUserProfile%","%Windows%":"%HostWindows%"},"HOST":"win64","LABEL":"windows-native_exec"}]}]})VG") },
    };
    return R;
}

bool IsBuiltin(const std::string &NodeCid)
{
    static const std::set<std::string> Cids = [] {
        std::set<std::string> S;
        for (const Runner &B : All()) S.insert(Cid::OfNode(B.Node));
        return S;
    }();
    return Cids.count(NodeCid) > 0;
}

void AddTo(std::map<std::string, nlohmann::ordered_json> &Tree, std::map<std::string, std::filesystem::path> &Dirs,
           const std::filesystem::path &LibraryRoot, const std::string &Machine)
{
    for (const Runner &B : All())
    {
        bool Hosts = false;
        for (const auto &L : B.Node["LAYERS"])
            if (L.contains("EXEC")) for (const auto &E : L["EXEC"]) Hosts = Hosts || E.value("HOST", std::string()) == Machine;
        if (!Hosts) continue;
        //A synthetic key, like a node file with no stored CID: the freeze indexes it by its own CID, and an identical
        //copy on disk (or received) is the same CID, kept once — a local copy wins, as it does over a received one.
        const std::string Key = std::string("\x01builtin/") + B.Folder + "#1";
        Tree[Key] = B.Node;
        Dirs[Key] = LibraryRoot / "VidyaGodRunners" / B.Folder;
    }
}

}
