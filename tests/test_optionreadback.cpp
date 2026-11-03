// OptionReadBack: an option the package writes into a file the user keeps is read back out of the user's saved copy,
// so a change made in the game's own settings menu becomes the option's value (spec 08 §8.8).
#include "vgtest.h"
#include "optionreadback.h"
#include "launchparams.h"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

struct Fixture
{
    fs::path Root;
    ContainerParams CP;
    Fixture() : Root(fs::temp_directory_path() / ("vg_orb_" + std::to_string(::getpid()))), CP(Root / "PKG")
    {
        fs::remove_all(Root);
        CP.UserDataPath = Root / "USERDATA";
        CP.RuntimePath  = Root / "RUNTIME";
        CP.KeepFiles = { PersistTarget{"game/settings.ini", "settings.ini"}, PersistTarget{"game/video.json", "video.json"} };
        CP.KeepDirs  = { PersistTarget{"profile", "Profile"} };
        const auto opt = [](const char *Control, json Extra = json::object()) {
            json D = { {"TYPE", "CustomVar"}, {"DEFAULT", "x"}, {"UI", {{"CONTROL", Control}}} };
            for (const auto &[K, V] : Extra.items()) D["UI"][K] = V;
            return D;
        };
        CP.VarDecls["res"]    = opt("int");
        CP.VarDecls["fps"]    = opt("int");
        CP.VarDecls["full"]   = opt("bool");
        CP.VarDecls["lang"]   = opt("enum", json{{"CHOICES", json::array({ json{{"LABEL", "English"}, {"VALUE", "en"}}, json{{"LABEL", "German"}, {"VALUE", "de"}} })}});
        CP.VarDecls["jump"]   = opt("key");
        CP.VarDecls["keys"]   = json{ {"TYPE", "CustomVar"}, {"MAP", {{"space", "space"}, {"left ctrl", "lctrl"}, {"w", "w"}}} };
        CP.VarDecls["hidden"] = json{ {"TYPE", "CustomVar"}, {"DEFAULT", "1"} };
        CP.VarDecls["cdkey"]  = opt("secret");
        CP.SubComponentsArray = json::array({
            Op("ConfigWrite", "game/settings.ini", "Resolution=", "%res%"),
            Op("JsonSet",     "game/video.json",   "/video/fps",  "%fps%"),
            Op("JsonSet",     "game/video.json",   "/video/fullscreen", "%full:bool%"),
            Op("XmlSet",      "profile/attributes.xml", "/Attributes/Attr[@name='g_language']/@value", "%lang%"),
            Op("XmlSet",      "profile/attributes.xml", "/Attributes/Attr[@name='jump']/@value", "key_%jump:map:keys%"),
            Op("ConfigWrite", "game/settings.ini", "Hidden=", "%hidden%"),
            Op("ConfigWrite", "game/settings.ini", "Key=", "%cdkey%"),
            Op("ConfigWrite", "game/settings.ini", "Both=", "%res%x%fps%") });
    }
    ~Fixture() { std::error_code Ec; fs::remove_all(Root, Ec); }
    static json Op(const char *Mode, const char *File, const char *Key, const char *Value)
    {
        return json{ {"TYPE", "FileEdit"}, {"OVERRIDE", true}, {"MODE", Mode}, {"FILE", File}, {"KEY", Key}, {"VALUE", Value} };
    }
    void Put(const fs::path &Rel, const std::string &Text)
    {
        fs::create_directories((CP.UserDataPath / Rel).parent_path());
        std::ofstream(CP.UserDataPath / Rel, std::ios::binary) << Text;
    }
};

}

namespace {
std::optional<std::string> At(const std::map<std::string, OptionReadBack::Reading> &R, const char *K)
{
    const auto It = R.find(K);
    return It == R.end() ? std::nullopt : std::optional<std::string>(It->second.Value);
}
OptionReadBack::Reading Got(const char *V) { return OptionReadBack::Reading{ V, false }; }
}

// What the saved files hold is read back, through each write's own shape: a config line, a JSON value (bool undone),
// an XML attribute (with literal text around the value, and a MAP table undone). Teeth: no prefix/suffix strip (the
// jump key reads "key_lctrl"); the bool not undone ("true"); the MAP not reversed ("lctrl").
TEST(readback_reads_each_kind_of_write)
{
    Fixture F;
    F.Put("settings.ini", "[Video]\r\nResolution=2560\r\nHidden=0\r\nKey=SECRET\r\nBoth=1x2\r\n");
    F.Put("video.json", R"({"video": {"fps": 144, "fullscreen": true}})");
    F.Put("Profile/attributes.xml", "<Attributes>\n\t<Attr name=\"g_language\" value=\"de\" />\n\t<Attr name=\"jump\" value=\"key_lctrl\" />\n</Attributes>\n");
    const auto R = OptionReadBack::Read(F.CP);
    CHECK(At(R, "res") == std::optional<std::string>("2560"));
    CHECK(At(R, "fps") == std::optional<std::string>("144"));
    CHECK(At(R, "full") == std::optional<std::string>("1"));
    CHECK(At(R, "lang") == std::optional<std::string>("de"));
    CHECK(At(R, "jump") == std::optional<std::string>("left ctrl"));
    CHECK(!R.count("hidden"));                           // no UI: not a setting
    CHECK(!R.count("cdkey"));                            // a secret stays one-way
    CHECK(!R.at("res").DefaultOnly);
}

// One-way, and nothing to read: a value outside the option's domain (an enum choice it lacks, no key name), a file
// with no saved copy, a ConfigWrite line that is not there, an option written twice that the files disagree on, a
// value the game removed (its default is the game's, not necessarily the package's), an empty file (a save cut short),
// a link where the copy should be, and a file too large to be settings. Teeth: no domain check (lang reads "fr");
// read a removal as the default; follow the link.
TEST(readback_leaves_out_what_it_cannot_read)
{
    Fixture F;
    F.Put("Profile/attributes.xml", "<Attributes><Attr name=\"g_language\" value=\"fr\" /><Attr name=\"jump\" value=\"key_nosuchkey\" /></Attributes>");
    F.Put("settings.ini", "Other=1\n");
    auto R = OptionReadBack::Read(F.CP);
    CHECK(!R.count("lang"));
    CHECK(!R.count("jump"));
    CHECK(!R.count("res"));                              // no Resolution= line: nothing was ever written there
    CHECK(!R.count("fps"));                              // video.json has no saved copy
    F.CP.SubComponentsArray.push_back(Fixture::Op("JsonSet", "game/video.json", "/other/res", "%res%"));
    F.Put("settings.ini", "Resolution=800\n");
    F.Put("video.json", R"({"other": {"res": 1024}, "video": {"fullscreen": false}})");
    R = OptionReadBack::Read(F.CP);
    CHECK(!R.count("res"));                              // 800 in one file, 1024 in the other
    CHECK(!R.count("fps"));                              // removed by the game
    CHECK(At(R, "full") == std::optional<std::string>("0"));
    F.Put("Profile/attributes.xml", "  \n");
    CHECK(!OptionReadBack::Read(F.CP).count("lang"));    // an empty file is no removal of every value in it
    const fs::path Outside = F.Root / "host-file.xml";
    std::ofstream(Outside) << "<Attributes><Attr name=\"g_language\" value=\"de\" /></Attributes>";
    fs::remove(F.CP.UserDataPath / "Profile" / "attributes.xml");
    fs::create_symlink(Outside, F.CP.UserDataPath / "Profile" / "attributes.xml");
    CHECK(!OptionReadBack::Read(F.CP).count("lang"));    // a link the game left is not followed
}

// The adopt rule (three-way): a value the game changed (the file differs from LAST_IN_FILE) is taken while the
// launcher's value is still the last known one; when the launcher's value changed too, the launcher wins; a value the
// file still holds as known leaves a window change alone; a package-default-only option follows its file. Teeth: take
// the file whenever it differs from LAST (the window's 1440 is lost); ignore LAST (the window's 1280 is lost).
TEST(readback_adopts_only_what_the_game_changed)
{
    json C = { {"VARIABLES", {{"res", "1920"}}} };
    bool Touched = false;
    auto Changed = OptionReadBack::Adopt(C, {{"res", Got("2560")}}, {{"res", "1920"}}, Touched);   // first sighting
    CHECK(Touched && Changed == std::vector<std::string>{"res"});
    CHECK_EQ(C["VARIABLES"]["res"].get<std::string>(), std::string("2560"));
    CHECK_EQ(C["LAST_IN_FILE"]["res"].get<std::string>(), std::string("2560"));
    C["VARIABLES"]["res"] = "1280";                                                                // changed in the window
    Changed = OptionReadBack::Adopt(C, {{"res", Got("2560")}}, {{"res", "1280"}}, Touched);       // the file as known
    CHECK(!Touched && Changed.empty());
    CHECK_EQ(C["VARIABLES"]["res"].get<std::string>(), std::string("1280"));
    //The review's sequence: launched at 1080 (LAST 1080), the game set 720, the window then set 1440: the launcher wins.
    C["VARIABLES"]["res"] = "1440"; C["LAST_IN_FILE"]["res"] = "1080";
    Changed = OptionReadBack::Adopt(C, {{"res", Got("720")}}, {{"res", "1440"}}, Touched);
    CHECK(Changed.empty());
    CHECK_EQ(C["VARIABLES"]["res"].get<std::string>(), std::string("1440"));
    //Launched at 1440 (LAST 1440), the game set 720, the window untouched: the game's value is taken.
    C["LAST_IN_FILE"]["res"] = "1440";
    Changed = OptionReadBack::Adopt(C, {{"res", Got("720")}}, {{"res", "1440"}}, Touched);
    CHECK(Changed == std::vector<std::string>{"res"});
    CHECK_EQ(C["VARIABLES"]["res"].get<std::string>(), std::string("720"));
    //A '%' the game wrote is stored in the format's spelling (%%), so the launch writes back exactly one.
    json P = json::object();
    Changed = OptionReadBack::Adopt(P, {{"rate", Got("50%")}}, {{"rate", "40%"}}, Touched);
    CHECK_EQ(P["VARIABLES"]["rate"].get<std::string>(), std::string("50%%"));
    //A first sighting that matches the launch's value pins nothing.
    json D = json::object();
    Changed = OptionReadBack::Adopt(D, {{"fps", Got("60")}}, {{"fps", "60"}}, Touched);
    CHECK(Changed.empty() && !D["VARIABLES"].contains("fps"));
    //A package default only: the launcher never writes it once the user has a copy, so the file decides, always.
    C["LAST_IN_FILE"]["lang"] = "en"; C["VARIABLES"]["lang"] = "de";
    Changed = OptionReadBack::Adopt(C, {{"lang", OptionReadBack::Reading{ "en", true }}}, {{"lang", "de"}}, Touched);
    CHECK(Changed == std::vector<std::string>{"lang"});
    CHECK_EQ(C["VARIABLES"]["lang"].get<std::string>(), std::string("en"));
}

// What a launch wrote is noted only where its file in the runtime holds it (an edit that failed records nothing), in a
// form Read reports (a bool DEFAULT "true" as "1"); Commit puts it in LAST_IN_FILE. Teeth: record without checking the
// runtime file (res is noted though the edit never landed); keep the raw "true".
TEST(readback_notes_only_what_was_written)
{
    Fixture F;
    F.CP.CustomVariables["res"] = "1280";
    F.CP.CustomVariables["full"] = "true";
    fs::create_directories(F.CP.RuntimePath / "game");
    std::ofstream(F.CP.RuntimePath / "game" / "video.json") << R"({"video": {"fullscreen": true}})";
    auto W = OptionReadBack::Written(F.CP);
    CHECK(!W.count("res"));                              // settings.ini was never written in the runtime
    CHECK(W.count("full") && W.at("full") == "1");
    std::ofstream(F.CP.RuntimePath / "game" / "settings.ini") << "Resolution=1280\n";
    W = OptionReadBack::Written(F.CP);
    CHECK(W.count("res") && W.at("res") == "1280");
    json C = json::object();
    CHECK(OptionReadBack::Commit(C, W));
    CHECK_EQ(C["LAST_IN_FILE"]["res"].get<std::string>(), std::string("1280"));
    CHECK(!OptionReadBack::Commit(C, W));                // nothing new
}
