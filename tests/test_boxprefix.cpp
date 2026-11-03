// BoxPrefix: the Sandboxie box as a windows-native game's prefix. The placements are what Sandboxie-Plus 1.18 does on
// Windows 11 (measured on the test VM): profile -> user/current, C:\ProgramData -> user/all, C:\Users\Public ->
// user/public, any other path -> drive/<letter>. The copies and the .reg text are plain files, tested here on any OS.
#include "vgtest.h"
#include "boxprefix.h"
#include "registrywrapper.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace {

namespace fs = std::filesystem;

const std::map<std::string, std::string> Host = {
    {"UserProfile", "C:\\Users\\dev"}, {"Public", "C:\\Users\\Public"}, {"ProgramData", "C:\\ProgramData"}};
const nlohmann::ordered_json Drives = {{"C:", "drive_c"}};

struct TempDir
{
    fs::path P;
    TempDir()
    {
        std::random_device R;
        P = fs::temp_directory_path() / ("vg_boxprefix_" + std::to_string(R()));
        fs::create_directories(P);
    }
    ~TempDir() { std::error_code Ec; fs::remove_all(P, Ec); }
};

void Put(const fs::path &F, const std::string &Text)
{
    fs::create_directories(F.parent_path());
    std::ofstream(F, std::ios::binary) << Text;
}

std::string Get(const fs::path &F)
{
    std::ifstream In(F, std::ios::binary);
    std::stringstream S; S << In.rdbuf();
    return S.str();
}

} // namespace

TEST(box_places_a_real_path_where_sandboxie_keeps_it)
{
    using BoxPrefix::BoxRelForReal;
    CHECK_EQ(BoxRelForReal("C:\\Users\\dev\\AppData\\Roaming\\Game\\a.cfg", Host), std::string("user/current/AppData/Roaming/Game/a.cfg"));
    CHECK_EQ(BoxRelForReal("c:/users/DEV/Documents", Host), std::string("user/current/Documents"));   // no case, either slash
    CHECK_EQ(BoxRelForReal("C:\\Users\\dev", Host), std::string("user/current"));
    CHECK_EQ(BoxRelForReal("C:\\ProgramData\\Vendor\\x.dat", Host), std::string("user/all/Vendor/x.dat"));
    CHECK_EQ(BoxRelForReal("C:\\Users\\Public\\Desktop\\l.lnk", Host), std::string("user/public/Desktop/l.lnk"));
    CHECK_EQ(BoxRelForReal("C:\\Windows\\SysWOW64\\dplayx.dll", Host), std::string("drive/C/Windows/SysWOW64/dplayx.dll"));
    CHECK_EQ(BoxRelForReal("C:\\Users\\devil\\x", Host), std::string("drive/C/Users/devil/x"));   // a sibling, not the profile
    CHECK_EQ(BoxRelForReal("d:\\Games\\x", Host), std::string("drive/D/Games/x"));
    CHECK_EQ(BoxRelForReal("C:\\", Host), std::string("drive/C"));
    CHECK_EQ(BoxRelForReal("relative\\x", Host), std::string());
}

TEST(box_maps_a_layout_path_to_its_real_path_and_leaves_the_game_folder_out)
{
    using BoxPrefix::RealForLayout; using BoxPrefix::InBox;
    CHECK_EQ(RealForLayout("drive_c/Users/dev/AppData/Roaming/x", Drives), std::string("C:\\Users\\dev\\AppData\\Roaming\\x"));
    CHECK_EQ(RealForLayout("drive_c", Drives), std::string("C:\\"));
    CHECK_EQ(RealForLayout("drive_cc/x", Drives), std::string());
    CHECK_EQ(RealForLayout("n64/x", Drives), std::string());
    CHECK(InBox("drive_c/windows/syswow64/a.dll", "drive_c/990001", Drives));
    CHECK(!InBox("drive_c/990001/game.exe", "drive_c/990001", Drives));
    CHECK(!InBox("drive_c/990001", "drive_c/990001/", Drives));
    CHECK(InBox("drive_c/9900011/x", "drive_c/990001", Drives));   // a sibling folder is not the game's
    CHECK(!InBox("other/x", "drive_c/990001", Drives));
}

TEST(box_materialises_everything_outside_the_game_folder)
{
    TempDir T;
    const fs::path Rt = T.P / "RUNTIME", Box = T.P / "BOX";
    Put(Rt / "drive_c/Users/dev/AppData/Roaming/Game/a.cfg", "cfg");
    Put(Rt / "drive_c/windows/syswow64/dplayx.dll", "dll");
    Put(Rt / "drive_c/ProgramData/Vendor/x.dat", "pd");
    Put(Rt / "drive_c/ProgramData/Vendor/empty.dat", "");
    Put(Rt / "drive_c/990001/game.exe", "exe");
    Put(Rt / "drive_c/990001/sub/data.bin", "data");
    Put(Rt / "__runner__/x", "runner");
    Put(Rt / "drive_c/990001/locked/x.bin", "a game folder the walk never enters");
    fs::permissions(Rt / "drive_c/990001/locked", fs::perms::none);
    Put(T.P / "elsewhere/secret.txt", "not the package's");
    fs::create_directory_symlink(T.P / "elsewhere", Rt / "drive_c/Users/dev/AppData/Roaming/link");   // never followed
    std::vector<std::string> Errors, Placed;
    CHECK(BoxPrefix::Materialise(Rt, "drive_c/990001", Drives, Host, Box, Errors, &Placed));
    CHECK(Errors.empty());
    std::sort(Placed.begin(), Placed.end());
    CHECK(Placed == (std::vector<std::string>{"drive/C/windows/syswow64/dplayx.dll", "user/all/Vendor/empty.dat",
                                              "user/all/Vendor/x.dat", "user/current/AppData/Roaming/Game/a.cfg"}));
    CHECK(!fs::exists(Box / "user/current/AppData/Roaming/link"));
    fs::permissions(Rt / "drive_c/990001/locked", fs::perms::owner_all);
    CHECK_EQ(Get(Box / "user/current/AppData/Roaming/Game/a.cfg"), std::string("cfg"));
    CHECK_EQ(Get(Box / "drive/C/windows/syswow64/dplayx.dll"), std::string("dll"));
    CHECK_EQ(Get(Box / "user/all/Vendor/x.dat"), std::string("pd"));
    CHECK(fs::exists(Box / "user/all/Vendor/empty.dat") && fs::file_size(Box / "user/all/Vendor/empty.dat") == 0);
    CHECK(!fs::exists(Box / "drive/C/990001"));
    CHECK(!fs::exists(Box / "drive/C/__runner__"));
}

TEST(box_syncs_keeps_back_deleting_only_what_the_box_was_given)
{
    TempDir T;
    const fs::path Rt = T.P / "RUNTIME", Box = T.P / "BOX";
    const std::string Saves = "drive_c/Users/dev/Documents/Game/Saves";
    const std::string BSaves = "user/current/Documents/Game/Saves";
    const std::string Roam = "drive_c/Users/dev/AppData/Roaming/Game";
    const std::string BRoam = "user/current/AppData/Roaming/Game";
    Put(Rt / Saves / "slot1.sav", "old1");
    Put(Rt / Saves / "slot4.sav", "overwritten by an empty one");
    Put(Box / BSaves / "slot4.sav", "");
    Put(Rt / Saves / "slot2.sav", "deleted by the game");
    Put(Rt / Saves / "old/dir.sav", "in a folder the game deleted");
    Put(Rt / Saves / "notcopied.sav", "its copy into the box failed: never taken for deleted");
    fs::create_directories(Rt / Saves / "empty");                                    // never in a box (files only)
    Put(Box / BSaves / "slot1.sav", "new1");
    Put(Box / BSaves / "slot3.sav", "made");
    Put(Box / BSaves / "more/s.sav", "deep");
    Put(T.P / "secret/s.txt", "outside");
    fs::create_directory_symlink(T.P / "secret", Box / BSaves / "evil");             // the game's link in its box
    Put(T.P / "victim/v.sav", "the user's file elsewhere");
    fs::create_directory_symlink(T.P / "victim", Rt / Saves / "outlink");            // the game's link in its kept folder
    Put(Box / BSaves / "outlink/new.sav", "would land in the folder the link names");
    Put(Rt / "drive_c/Users/dev/AppData/Local/Game/untouched/k.sav", "keep me");      // its box copy is missing
    Put(Box / BRoam / "settings.ini", "ini");
    Put(Rt / Roam / "gone.ini", "a kept file the game deleted");
    Put(Rt / Roam / "Gone2.ini", "kept as Gone2.ini, given to the box as GONE2.INI (Windows names are caseless)");
    Put(Box / BRoam / "player1.hki", "p1");
    Put(Box / BRoam / "player2.hki", "p2");
    Put(Rt / Roam / "player3.hki", "a profile the game deleted");
    Put(Box / BRoam / "other.txt", "not matched");
    Put(Box / BRoam / "slot1.cfg", "one character");
    Put(Box / BRoam / "slot10.cfg", "two characters: not slot?.cfg");
    Put(Box / "drive/C/990001/in-game-folder.sav", "the game folder is the mount's, never synced");
    const std::vector<std::string> Placed = {BSaves + "/slot1.sav", BSaves + "/slot2.sav", BSaves + "/slot4.sav",
        BSaves + "/old/dir.sav", BSaves + "/outlink/v.sav", BRoam + "/gone.ini", BRoam + "/GONE2.INI", BRoam + "/player3.hki",
        "user/current/AppData/Local/Game/untouched/k.sav"};
    const std::vector<PersistTarget> Dirs = {{Saves, "Saves"}, {"drive_c/Users/dev/AppData/Local/Game/untouched", "Untouched"},
                                             {"drive_c/990001/Saves", "GameSaves"}};
    const std::vector<PersistTarget> Files = {{Roam + "/settings.ini", "settings.ini"}, {Roam + "/gone.ini", "gone.ini"}, {Roam + "/Gone2.ini", "gone2"},
                                              {Roam + "/player*.hki", "players"}, {Roam + "/slot?.cfg", "slots"},
                                              {"drive_c/990001/in-game-folder.sav", "x"}};
    std::vector<std::string> Errors;
    BoxPrefix::SyncKeepsBack(Rt, "drive_c/990001", Drives, Host, Box, Dirs, Files, Placed, Errors);
    CHECK_EQ(Errors.size(), size_t(2));                              // the link on the way to v.sav, and to new.sav
    for (const std::string &E : Errors) CHECK(E.find("outlink") != std::string::npos);
    CHECK(!fs::exists(T.P / "victim/new.sav"));                                       // not written through the link
    CHECK_EQ(Get(Rt / Saves / "slot1.sav"), std::string("new1"));
    CHECK_EQ(Get(Rt / Saves / "slot3.sav"), std::string("made"));
    CHECK(fs::exists(Rt / Saves / "slot4.sav") && fs::file_size(Rt / Saves / "slot4.sav") == 0);
    CHECK_EQ(Get(Rt / Saves / "more/s.sav"), std::string("deep"));
    CHECK(!fs::exists(Rt / Saves / "slot2.sav"));
    CHECK(!fs::exists(Rt / Saves / "old/dir.sav"));
    CHECK_EQ(Get(Rt / Saves / "notcopied.sav"), std::string("its copy into the box failed: never taken for deleted"));
    CHECK(fs::is_directory(Rt / Saves / "empty"));
    CHECK(!fs::exists(Rt / Saves / "evil"));                                          // not copied through the box's link
    CHECK_EQ(Get(T.P / "victim/v.sav"), std::string("the user's file elsewhere"));    // not deleted through the folder's
    CHECK_EQ(Get(Rt / "drive_c/Users/dev/AppData/Local/Game/untouched/k.sav"), std::string("keep me"));
    CHECK_EQ(Get(Rt / Roam / "settings.ini"), std::string("ini"));
    CHECK(!fs::exists(Rt / Roam / "gone.ini"));
    CHECK(!fs::exists(Rt / Roam / "Gone2.ini"));
    CHECK_EQ(Get(Rt / Roam / "player1.hki"), std::string("p1"));
    CHECK_EQ(Get(Rt / Roam / "player2.hki"), std::string("p2"));
    CHECK(!fs::exists(Rt / Roam / "player3.hki"));
    CHECK(!fs::exists(Rt / Roam / "other.txt"));
    CHECK_EQ(Get(Rt / Roam / "slot1.cfg"), std::string("one character"));
    CHECK(!fs::exists(Rt / Roam / "slot10.cfg"));
    CHECK(!fs::exists(Rt / "drive_c/990001/in-game-folder.sav"));
}

// A box file that cannot be read leaves the kept save as it was (it once came back empty: the destination was opened,
// so truncated, before the source).
TEST(box_sync_leaves_a_save_whole_when_its_box_copy_cannot_be_read)
{
    TempDir T;
    const fs::path Rt = T.P / "RUNTIME", Box = T.P / "BOX";
    const std::string Saves = "drive_c/Users/dev/Documents/Game/Saves";
    Put(Rt / Saves / "slot.sav", "precious");
    Put(Box / "user/current/Documents/Game/Saves/slot.sav", "newer, unreadable");
    fs::permissions(Box / "user/current/Documents/Game/Saves/slot.sav", fs::perms::none);
    if (std::ifstream(Box / "user/current/Documents/Game/Saves/slot.sav")) return;   // root reads anything: nothing to test
    std::vector<std::string> Errors;
    CHECK(!BoxPrefix::SyncKeepsBack(Rt, "drive_c/990001", Drives, Host, Box, {{Saves, "Saves"}}, {}, {}, Errors));
    CHECK_EQ(Get(Rt / Saves / "slot.sav"), std::string("precious"));
    CHECK(!fs::exists(Rt / Saves / "slot.sav.vidyagod-copy"));
    fs::permissions(Box / "user/current/Documents/Game/Saves/slot.sav", fs::perms::owner_all);
}

TEST(box_remove_tree_removes_links_never_what_they_name)
{
    TempDir T;
    Put(T.P / "box/a/b/c.txt", "c");
    Put(T.P / "box/ro.txt", "read-only");
    fs::permissions(T.P / "box/ro.txt", fs::perms::owner_read);
    Put(T.P / "outside/keep.txt", "the user's");
    fs::create_directory_symlink(T.P / "outside", T.P / "box/a/junction");
    fs::create_symlink(T.P / "outside/keep.txt", T.P / "box/file-link");
    std::vector<std::string> Errors;
    CHECK(BoxPrefix::RemoveTree(T.P / "box", Errors));
    CHECK(Errors.empty());
    CHECK(!fs::exists(T.P / "box"));
    CHECK_EQ(Get(T.P / "outside/keep.txt"), std::string("the user's"));
    CHECK(BoxPrefix::RemoveTree(T.P / "box", Errors));                               // nothing there: nothing to do
    CHECK(BoxPrefix::IsLink(T.P / "outside") == false);
}

TEST(box_registry_round_trips_through_a_reg_file)
{
    RegistryWrapper RW;
    RW.SetValue("HKLM\\Software\\Wow6432Node\\Microsoft\\Microsoft Games\\Age of Empires\\2.0", "CDPath",
                RegistryWrapper::SzValue("C:\\Games\\AoE2\\"));
    RW.SetValue("HKLM\\Software\\Wow6432Node\\Microsoft\\Microsoft Games\\Age of Empires\\2.0", "",
                RegistryWrapper::SzValue("quote \" and dword:1"));
    RW.SetValue("HKCU\\Software\\Game", "Opt", RegistryWrapper::ManifestStringToValue("dword:00000005"));
    RW.SetValue("HKCU\\Software\\Game", "Blob", RegistryWrapper::ManifestStringToValue("hex:01,02,ff"));
    RW.SetValue("HKCU\\Software\\Game", "Path", RegistryWrapper::ManifestStringToValue("str(2):\"%SystemRoot%\\\\x\""));
    RW.SetValue("HKU\\.Default\\Software\\Game", "Skipped", RegistryWrapper::SzValue("x"));
    RW.SetValue("HKCU\\Software\\Game", "Lines", RegistryWrapper::SzValue("a\r\n\"b\""));
    size_t Skipped = 0;
    const std::string Reg = BoxPrefix::ToRegFile(RW, &Skipped);
    CHECK(Reg.rfind("Windows Registry Editor Version 5.00\r\n", 0) == 0);
    CHECK(Reg.find("[HKEY_LOCAL_MACHINE\\Software\\Wow6432Node\\Microsoft\\Microsoft Games\\Age of Empires\\2.0]\r\n") != std::string::npos);
    CHECK(Reg.find("\"CDPath\"=\"C:\\\\Games\\\\AoE2\\\\\"\r\n") != std::string::npos);
    CHECK(Reg.find("@=\"quote \\\" and dword:1\"\r\n") != std::string::npos);
    CHECK(Reg.find("\"Opt\"=dword:00000005\r\n") != std::string::npos);
    // REG_EXPAND_SZ as regedit spells it: hex(2): over UTF-16LE units, NUL-ended
    CHECK(Reg.find("\"Path\"=hex(2):25,00,53,00,79,00,73,00,74,00,65,00,6d,00,52,00,6f,00,6f,00,74,00,25,00,5c,00,78,00,00,00\r\n") != std::string::npos);
    // a line break cannot stand in quotes: hex(1), UTF-16LE, NUL-ended
    CHECK(Reg.find("\"Lines\"=hex(1):61,00,0d,00,0a,00,22,00,62,00,22,00,00,00\r\n") != std::string::npos);
    CHECK(Reg.find(".Default") == std::string::npos);
    CHECK_EQ(Skipped, size_t(2));   // the HKU key and its value

    RegistryWrapper Back; std::string Err;
    CHECK(BoxPrefix::FromRegFile(Reg, Back, Err));
    CHECK(Err.empty());
    const RegistryValue *Cd = Back.GetValue("HKLM\\Software\\Wow6432Node\\Microsoft\\Microsoft Games\\Age of Empires\\2.0", "CDPath");
    CHECK(Cd && Cd->Type == RegType::Sz && Cd->Str == "C:\\Games\\AoE2\\");
    const RegistryValue *Def = Back.GetValue("HKLM\\Software\\Wow6432Node\\Microsoft\\Microsoft Games\\Age of Empires\\2.0", "");
    CHECK(Def && Def->Type == RegType::Sz && Def->Str == "quote \" and dword:1");
    const RegistryValue *Opt = Back.GetValue("HKCU\\Software\\Game", "Opt");
    CHECK(Opt && Opt->Type == RegType::Dword && Opt->RawPayload == "dword:00000005");
    const RegistryValue *Blob = Back.GetValue("HKCU\\Software\\Game", "Blob");
    CHECK(Blob && Blob->Type == RegType::Hex && Blob->RawPayload == "hex:01,02,ff");
}

TEST(box_reads_what_reg_exe_exports)
{
    // reg.exe export: CRLF, a hex value wrapped over lines, a deletion, a default value.
    const std::string Text = "Windows Registry Editor Version 5.00\r\n\r\n"
                             "[HKEY_CURRENT_USER\\Software\\Microsoft\\Microsoft Games\\Age of Empires II: The Conquerors Expansion\\1.0]\r\n"
                             "@=\"\"\r\n"
                             "\"Screen Size\"=dword:00000400\r\n"
                             "\"Player Name\"=\"Lorenzo \\\"LGZ\\\"\"\r\n"
                             "\"Long\"=hex:00,01,02,03,04,05,06,07,08,09,0a,0b,0c,0d,0e,0f,10,11,12,13,14,15,16,\\\r\n"
                             "  17,18,19\r\n\r\n"
                             "[-HKEY_CURRENT_USER\\Software\\Gone]\r\n"
                             "\"Gone\"=-\r\n"
                             "\"Stray\"=\"a value under a deleted key: kept nowhere\"\r\n";
    RegistryWrapper RW; std::string Err;
    CHECK(BoxPrefix::FromRegFile(Text, RW, Err));
    const std::string K = "HKCU\\Software\\Microsoft\\Microsoft Games\\Age of Empires II: The Conquerors Expansion\\1.0";
    const RegistryValue *Size = RW.GetValue(K, "Screen Size");
    CHECK(Size && Size->RawPayload == "dword:00000400");
    const RegistryValue *Name = RW.GetValue(K, "Player Name");
    CHECK(Name && Name->Str == "Lorenzo \"LGZ\"");
    const RegistryValue *Long = RW.GetValue(K, "Long");
    CHECK(Long && Long->RawPayload == "hex:00,01,02,03,04,05,06,07,08,09,0a,0b,0c,0d,0e,0f,10,11,12,13,14,15,16,17,18,19");
    CHECK(RW.GetValue(K, "") != nullptr);
    CHECK(RW.GetKey("HKCU\\Software\\Gone") == nullptr);
    size_t Keys = 0;
    RW.ForEachKey([&](const std::string &, const RegistryKey &) { ++Keys; });
    CHECK_EQ(Keys, size_t(1));
    std::string Bad;
    CHECK(!BoxPrefix::FromRegFile("Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\X]\r\nnot a value\r\n", RW, Bad));
    CHECK(!Bad.empty());
}

TEST(box_utf16_round_trips)
{
    const std::string S = "Ā é 😀 C:\\x";
    const std::string W = BoxPrefix::Utf8ToUtf16Le(S);
    CHECK(W.size() >= 2 && static_cast<unsigned char>(W[0]) == 0xFF && static_cast<unsigned char>(W[1]) == 0xFE);
    CHECK_EQ(BoxPrefix::Utf16LeToUtf8(W), S);
}

// Before the game: every kept file in the box's part of the runtime must be in the box whole, or the launch stops (the
// game would write defaults that the capture stores over the user's). A kept folder not made yet, and the game folder,
// carry nothing. Teeth: return nothing (the missing save passes); compare existence only (the cut copy passes).
TEST(box_reports_kept_files_the_box_did_not_get_whole)
{
    TempDir T;
    const fs::path Rt = T.P / "RUNTIME", Box = T.P / "BOX";
    Put(Rt / "drive_c/Users/dev/Saved Games/X/slot1.sav", "the user's save");
    Put(Rt / "drive_c/Users/dev/AppData/Roaming/X/options.ini", "opts");
    Put(Rt / "drive_c/990001/save.dat", "in the game folder: read live from the mount");
    const std::vector<PersistTarget> Dirs = {{"drive_c/Users/dev/Saved Games/X", "Saves"}, {"drive_c/Users/dev/Nothing Yet", "Later"},
                                             {"drive_c/990001", "Game"}};
    const std::vector<PersistTarget> Files = {{"drive_c/Users/dev/AppData/Roaming/X/options.ini", "Opts"}};
    std::vector<std::string> M = BoxPrefix::KeptNotInBox(Rt, "drive_c/990001", Drives, Host, Box, Dirs, Files);
    CHECK_EQ(M.size(), (size_t)2);                                                            // neither reached the box
    Put(Box / "user/current/Saved Games/X/slot1.sav", "the user's save");
    Put(Box / "user/current/AppData/Roaming/X/options.ini", "op");                            // cut short
    M = BoxPrefix::KeptNotInBox(Rt, "drive_c/990001", Drives, Host, Box, Dirs, Files);
    CHECK_EQ(M.size(), (size_t)1);
    CHECK(M[0].find("options.ini") != std::string::npos);
    Put(Box / "user/current/AppData/Roaming/X/options.ini", "opts");
    CHECK(BoxPrefix::KeptNotInBox(Rt, "drive_c/990001", Drives, Host, Box, Dirs, Files).empty());
}
