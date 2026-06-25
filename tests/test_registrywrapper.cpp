#include "vgtest.h"
#include "registrywrapper.h"

#include <string>

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
    RW.ApplyRegEdits(Subs, /*WantOverride=*/false);
    CHECK(RW.GetValue("Software\\Base", "K") != nullptr);
    CHECK(RW.GetValue("Software\\Over", "K") == nullptr);   // override sub skipped on the base pass
}

// (Hive file save/load round-trip is exercised by the real app via LoadPrefix on actual wine hives; the in-memory
// ops above cover the editing/diff logic the authoring tooling depends on.)
