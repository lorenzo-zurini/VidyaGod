#include "vgtest.h"
#include "registrywrapper.h"
#include "commonutils.h"

#include <filesystem>
#include <string>
#include <vector>

using nlohmann::ordered_json;

TEST(reg_ensure_set_get_roundtrip)
{
    RegistryWrapper RW;
    RegistryValue V; V.Type = RegType::Sz; V.Str = "hello"; V.Dirty = true;
    RW.SetValue("Software\\VG\\Test", "Greeting", V);

    const RegistryValue * Got = RW.GetValue("Software\\VG\\Test", "Greeting");
    CHECK(Got != nullptr);
    if (Got) CHECK_EQ(Got->Str, std::string("hello"));
    CHECK(RW.GetValue("Software\\VG\\Test", "Missing") == nullptr);
    CHECK(RW.GetValue("Software\\VG\\Nope", "Greeting") == nullptr);
}

TEST(reg_delete_value_and_key)
{
    RegistryWrapper RW;
    RegistryValue V; V.Type = RegType::Sz; V.Str = "x"; V.Dirty = true;
    RW.SetValue("Software\\VG", "A", V);
    RW.SetValue("Software\\VG", "B", V);
    CHECK(RW.DeleteValue("Software\\VG", "A"));
    CHECK(RW.GetValue("Software\\VG", "A") == nullptr);
    CHECK(RW.GetValue("Software\\VG", "B") != nullptr);
    CHECK(RW.DeleteKey("Software\\VG"));
    CHECK(RW.GetValue("Software\\VG", "B") == nullptr);
}

TEST(reg_manifest_string_to_value)
{
    RegistryValue Sz = RegistryWrapper::ManifestStringToValue(ordered_json("plain text"));
    CHECK(Sz.Type == RegType::Sz);
    CHECK_EQ(Sz.Str, std::string("plain text"));

    RegistryValue Dw = RegistryWrapper::ManifestStringToValue(ordered_json("dword:0000003c"));
    CHECK(Dw.Type == RegType::Dword);
}

TEST(reg_apply_regedits_then_diff_roundtrips)
{
    // The "Analyze Registry" authoring flow: apply package RegEdits, diff vs a baseline, get RegEdit layers back.
    RegistryWrapper Baseline;   // empty
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"OVERRIDE", false}, {"REGPATH", "Software\\VG\\App"},
                     {"KEYVALUES", {{"Foo", "bar"}, {"Count", "dword:00000005"}}}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));

    const RegistryValue * Foo = RW.GetValue("Software\\VG\\App", "Foo");
    CHECK(Foo != nullptr);
    if (Foo) CHECK_EQ(Foo->Str, std::string("bar"));

    ordered_json Delta = RW.DiffToRegEdits(Baseline);
    CHECK(Delta.is_array());
    CHECK(!Delta.empty());
    bool Found = false;
    for (const auto & L : Delta)
        if (L.value("TYPE", std::string()) == "RegEdit"
            && L.value("REGPATH", std::string()).find("Software\\VG\\App") != std::string::npos
            && L.contains("KEYVALUES") && L["KEYVALUES"].contains("Foo"))
            Found = true;
    CHECK(Found);
}

TEST(reg_diff_empty_default_is_key_only)
{
    // A key whose only content is an empty (Default) value is the "this key exists" case (wine/installer). The diff
    // must emit it KEY-ONLY (empty KEYVALUES), not as {"":""} noise; real named values + non-empty defaults survive.
    RegistryWrapper Baseline;   // empty
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE","RegEdit"},{"OVERRIDE",false},{"REGPATH","Software\\Bethesda"},{"KEYVALUES",{{"",""}}}},
        ordered_json{{"TYPE","RegEdit"},{"OVERRIDE",false},{"REGPATH","Software\\Bethesda\\App"},
                     {"KEYVALUES",{{"",""},{"Installed Path","C:\\x"}}}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));

    ordered_json Delta = RW.DiffToRegEdits(Baseline);
    bool SawKeyOnly = false, SawApp = false;
    for (const auto & L : Delta)
    {
        const std::string P = L.value("REGPATH", std::string());
        const ordered_json KV = L.value("KEYVALUES", ordered_json::object());
        if (P.find("Software\\Bethesda\\App") != std::string::npos)
        {
            SawApp = true;
            CHECK(!KV.contains(""));                  // empty default stripped
            CHECK(KV.contains("Installed Path"));     // real named value kept
        }
        else if (P.find("Software\\Bethesda") != std::string::npos)
        {
            SawKeyOnly = true;
            CHECK(!L.contains("KEYVALUES"));          // key-only — KEYVALUES omitted entirely, not {} or {"":""}
        }
    }
    CHECK(SawKeyOnly);
    CHECK(SawApp);
}

TEST(reg_override_pass_isolation)
{
    // ApplyRegEdits(WantOverride=true) must ignore non-override RegEdits and vice versa.
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"OVERRIDE", false}, {"REGPATH", "Software\\Base"},  {"KEYVALUES", {{"K", "base"}}}},
        ordered_json{{"TYPE", "RegEdit"}, {"OVERRIDE", true},  {"REGPATH", "Software\\Over"},  {"KEYVALUES", {{"K", "over"}}}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(RW.GetValue("Software\\Base", "K") != nullptr);
    CHECK(RW.GetValue("Software\\Over", "K") == nullptr);   // override sub skipped on the base pass

    RegistryWrapper RW2;
    CHECK(RW2.ApplyRegEdits(Subs, /*WantOverride=*/true));
    CHECK(RW2.GetValue("Software\\Over", "K") != nullptr);
    CHECK(RW2.GetValue("Software\\Base", "K") == nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// The LOUD failure paths. A registry value the package declared and never got is invisible: the game just behaves
// as if the package had not said it, and the engine used to report the whole pass as successful either way.
// ---------------------------------------------------------------------------------------------------------------

namespace {
// Collect the WARN/ERR a call produces, so "did it complain?" is itself assertable.
struct Complaints
{
    std::vector<std::string> Lines;
    Complaints()  { SetLogCallback([this](LogLevel L, const std::string & C, const std::string & M){
                        if (L == LogLevel::WARN || L == LogLevel::ERR) Lines.push_back(C + "|" + M); }); }
    ~Complaints() { ClearLogCallback(); }
    bool Mentions(const std::string & Needle) const
    {
        for (const auto & L : Lines) if (L.find(Needle) != std::string::npos) return true;
        return false;
    }
};
}

// KEYVALUES that is present but not an object used to be dropped in COMPLETE silence: the key was created, every
// value the author wrote was discarded, and ApplyRegEdits still returned true.
TEST(reg_malformed_keyvalues_fails_loudly)
{
    Complaints C;
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", "Software\\Bad"}, {"KEYVALUES", "InstallDir=C:\\Games"}}
    });
    CHECK(!RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(C.Mentions("not an object"));                        // and it says WHY nothing was written
}

// null KEYVALUES is the documented key-only form (Worms 4 ships exactly this) and must stay silent, or the noise
// stops meaning anything.
TEST(reg_null_keyvalues_is_key_only_and_quiet)
{
    Complaints C;
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", "Software\\KeyOnly"}, {"KEYVALUES", nullptr}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(C.Lines.empty());
}

TEST(reg_empty_regpath_is_reported)
{
    Complaints C;
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", ""}, {"KEYVALUES", {{"V", "1"}}}}
    });
    CHECK(!RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(C.Mentions("REGPATH"));
}

// One bad edit must not stop the good ones, but must still make the pass report failure — otherwise a package with
// one typo silently loses that setting and nothing anywhere says so.
TEST(reg_one_bad_edit_does_not_hide_the_good_ones)
{
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", ""},                {"KEYVALUES", {{"V", "1"}}}},
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", "Software\\Good"}, {"KEYVALUES", {{"V", "yes"}}}}
    });
    CHECK(!RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(RW.GetValue("Software\\Good", "V") != nullptr);     // a later valid edit is still applied
}

TEST(reg_non_regedit_subcomponents_are_ignored)
{
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "FileEdit"},  {"MODE", "Overwrite"}, {"FILE", "x"}},
        ordered_json{{"TYPE", "CustomVar"}, {"KEY", "k"},          {"DEFAULT", "v"}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(RW.ApplyRegEdits(ordered_json::array(),  /*WantOverride=*/false));
    CHECK(RW.ApplyRegEdits(ordered_json::object(), /*WantOverride=*/false));
}

// ARCHITECTURE:32 redirects under Wow6432Node — that redirect is how a 32-bit game finds its own install keys, and
// getting it wrong is indistinguishable from the game never having been installed.
TEST(reg_architecture_32_redirects_under_wow6432node)
{
    RegistryWrapper RW;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"ARCHITECTURE", "32"}, {"REGPATH", "HKLM\\Software\\Acme"},
                     {"KEYVALUES", {{"V", "1"}}}}
    });
    CHECK(RW.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(RW.GetValue("HKLM\\Software\\Wow6432Node\\Acme", "V") != nullptr);
    CHECK(RW.GetValue("HKLM\\Software\\Acme", "V") == nullptr);       // and NOT also in the 64-bit view
}

// Everything above is in-memory. If the save/load round-trip loses a value, every edit above is moot — this is the
// step where an edit stops being a data structure and becomes something the game can read.
TEST(reg_values_survive_a_save_and_reload)
{
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "vg_reg_roundtrip";
    std::filesystem::remove_all(Dir);
    std::filesystem::create_directories(Dir);

    RegistryWrapper Out;
    ordered_json Subs = ordered_json::array({
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", "HKLM\\Software\\Acme\\Game"},
                     {"KEYVALUES", {{"InstallDir", "C:\\Games\\Acme"}}}},
        ordered_json{{"TYPE", "RegEdit"}, {"REGPATH", "HKCU\\Software\\Acme"}, {"KEYVALUES", {{"Nick", "Lorenzo"}}}}
    });
    CHECK(Out.ApplyRegEdits(Subs, /*WantOverride=*/false));
    CHECK(Out.SavePrefix(Dir));

    RegistryWrapper In;
    CHECK(In.LoadPrefix(Dir));
    const RegistryValue * M = In.GetValue("HKLM\\Software\\Acme\\Game", "InstallDir");
    const RegistryValue * U = In.GetValue("HKCU\\Software\\Acme", "Nick");
    CHECK(M != nullptr);
    if (M) CHECK_EQ(M->Str, std::string("C:\\Games\\Acme"));
    CHECK(U != nullptr);                                        // the two hives must not collapse into one
    if (U) CHECK_EQ(U->Str, std::string("Lorenzo"));
    std::filesystem::remove_all(Dir);
}

// The root prefix decides which hive FILE a value lands in; misrouting puts it where the game never looks.
TEST(reg_root_prefix_selects_the_hive)
{
    CHECK(RegistryWrapper::RootForPath("HKLM\\Software\\X") == HiveRoot::Machine);
    CHECK(RegistryWrapper::RootForPath("HKCU\\Software\\X") == HiveRoot::User);
}
