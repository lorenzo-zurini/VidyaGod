#include "builtinrunners.h"

namespace BuiltinRunners {

const std::vector<Runner> &All()
{
    //windows-native: a win32/win64 game on Windows runs as itself. Before it, no runner hosted win64, so a Windows
    //game on a Windows PC had no runner chain at all.
    static const std::vector<Runner> R = {
        { "windows-native", nlohmann::ordered_json::parse(R"VG({"LABEL":"windows-native_exec","LAYERS":[{"COMMENT":"A Windows game on Windows runs as itself: the game's exe, started from the mounted game folder. The guest's C: is laid out inside the mount (drive_c), the way a Wine prefix lays it out, so every package places its files the same way on both hosts.","EXEC":[{"ARGS":[],"CONTENT_ROOT":"drive_c/%PackageUID%","DRIVES":{"C:":"drive_c"},"EXE":"%Content%","GUEST":["win32","win64"],"GUEST_ROOTS":{"%AppData%":"C:\\users\\steamuser\\AppData\\Roaming","%Documents%":"C:\\users\\steamuser\\Documents","%GameDir%":"C:\\%PackageUID%","%LocalAppData%":"C:\\users\\steamuser\\AppData\\Local","%ProgramData%":"C:\\ProgramData","%ProgramFiles%":"C:\\Program Files","%ProgramFiles32%":"C:\\Program Files (x86)","%SysDir32%":"C:\\windows\\syswow64","%SysDir64%":"C:\\windows\\system32","%UserProfile%":"C:\\users\\steamuser","%Windows%":"C:\\windows"},"HOST":"win64","LABEL":"windows-native_exec"}]}]})VG") },
    };
    return R;
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
