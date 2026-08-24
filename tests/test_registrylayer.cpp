#include "vgtest.h"
#include "registrylayer.h"
#include "launchparams.h"

#include <filesystem>
#include <fstream>

// P7 fill-in: the RegistryLayer persist seed/capture round-trip had zero coverage (only the low-level
// RegistryWrapper was tested). Uses real temp dirs + real hive files, mirroring the fileedits tests.

namespace fs = std::filesystem;

static fs::path RlTmp(const char *tag)
{
    fs::path d = fs::temp_directory_path() / (std::string("vg_reglayer_") + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

static void WriteFileAt(const fs::path &p, const std::string &content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

static std::string ReadFileAt(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Seed: persisted KEEP hives land in the writelayer under the prefix root, shadowing DEFPREFIX.
TEST(reglayer_seed_copies_kept_hives_under_prefix_root)
{
    auto d = RlTmp("seed");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.PrefixRoot     = "pfx";
    CP.KeepRegHives   = { "user.reg" };
    WriteFileAt(CP.UserDataPath / "REGISTRY" / "user.reg", "WINE REGISTRY Version 2\n[Software]\n\"K\"=\"persisted\"\n");
    CHECK(RegistryLayer::SeedPersistRegistry(CP));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx" / "user.reg"),
             std::string("WINE REGISTRY Version 2\n[Software]\n\"K\"=\"persisted\"\n"));
    fs::remove_all(d);
}

// Seed with nothing persisted yet: clean no-op success (first launch).
TEST(reglayer_seed_nothing_persisted_is_ok)
{
    auto d = RlTmp("seednone");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.KeepRegHives   = { "user.reg" };
    CHECK(RegistryLayer::SeedPersistRegistry(CP));
    CHECK(!fs::exists(CP.WriteLayerPath / "user.reg"));
    fs::remove_all(d);
}

// Capture: the session's runtime hive is copied back into the durable store; round-trips with Seed.
TEST(reglayer_capture_roundtrips_with_seed)
{
    auto d = RlTmp("cap");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.PrefixRoot     = "";                                   // plain-wine layout: hives at the root
    CP.KeepRegHives   = { "user.reg" };
    WriteFileAt(CP.RuntimePath / "user.reg", "WINE REGISTRY Version 2\n[Game]\n\"Save\"=\"42\"\n");
    CHECK(RegistryLayer::CapturePersistRegistry(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "REGISTRY" / "user.reg"),
             std::string("WINE REGISTRY Version 2\n[Game]\n\"Save\"=\"42\"\n"));
    // …and the captured store seeds an identical next session.
    CHECK(RegistryLayer::SeedPersistRegistry(CP));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "user.reg"),
             std::string("WINE REGISTRY Version 2\n[Game]\n\"Save\"=\"42\"\n"));
    fs::remove_all(d);
}

// PersistAll short-circuits both directions (the durable RW branch already holds the files).
TEST(reglayer_persistall_is_noop)
{
    auto d = RlTmp("pall");
    ContainerParams CP(d);
    CP.PersistAll   = true;
    CP.KeepRegHives = { "user.reg" };
    CHECK(RegistryLayer::SeedPersistRegistry(CP));
    CHECK(RegistryLayer::CapturePersistRegistry(CP));
    fs::remove_all(d);
}

#include "registrywrapper.h"
#include <nlohmann/json.hpp>

// Persisted whole-hive KEEP is UNION-merged over the composed hive: the user's saved value wins per-key,
// but base RegEdits (e.g. an EULA FIRSTRUN seed) SHOW THROUGH where the user has no value — and the
// old writelayer whole-file seed (which shadowed all of that) is skipped.
TEST(reglayer_hive_store_union_merges_over_base_edits)
{
    auto d = RlTmp("union");
    ContainerParams CP(d);
    CP.PrefixGenerate  = true;
    CP.UserDataPath    = d / "USERDATA";
    CP.WriteLayerPath  = d / "WRITELAYER";
    CP.DefaultDataPath = d / "DEFAULTDATA";
    CP.DefPrefixPath   = d / "DEFPREFIX";      // empty — zero-copy prefix model
    CP.PrefixRoot      = "pfx";
    CP.KeepRegHives    = { "user.reg" };
    // base RegEdit: the package seeds FIRSTRUN (the EULA-acceptance marker)
    CP.SubComponentsArray = nlohmann::ordered_json::array();
    CP.SubComponentsArray.push_back({{"TYPE","RegEdit"},{"REGPATH","HKCU\\Software\\Seeded\\1.0"},
                                     {"ARCHITECTURE","64"},{"OVERRIDE",false},{"KEYVALUES",{{"FIRSTRUN",true}}}});
    // the persisted store: the user's own key, but NO FIRSTRUN (captured from a pre-seed session)
    {
        RegistryWrapper W;
        nlohmann::ordered_json S = nlohmann::ordered_json::array();
        S.push_back({{"TYPE","RegEdit"},{"REGPATH","HKCU\\Software\\UserOwn"},{"ARCHITECTURE","64"},
                     {"OVERRIDE",false},{"KEYVALUES",{{"UserPick","7"}}}});
        W.ApplyRegEdits(S, false);
        fs::create_directories(CP.UserDataPath / "REGISTRY");
        W.SavePrefix(CP.UserDataPath / "REGISTRY");
    }

    CHECK(RegistryLayer::BuildDefaultData(CP));
    const std::string Hive = ReadFileAt(CP.DefaultDataPath / "pfx" / "user.reg");
    CHECK(Hive.find("FIRSTRUN") != std::string::npos);    // base edit SURVIVES the persisted merge
    CHECK(Hive.find("UserPick") != std::string::npos);    // persisted user state is IN the composed hive

    // and the old whole-file writelayer seed is skipped (it would shadow the composed hive)
    CHECK(RegistryLayer::SeedPersistRegistry(CP));
    CHECK(!fs::exists(CP.WriteLayerPath / "pfx" / "user.reg"));
    fs::remove_all(d);
}

// Per-key precedence inside the union: where BOTH sides define the same value, the persisted one wins.
TEST(reglayer_union_persisted_value_wins_per_key)
{
    auto d = RlTmp("unionwin");
    ContainerParams CP(d);
    CP.PrefixGenerate  = true;
    CP.UserDataPath    = d / "USERDATA";
    CP.WriteLayerPath  = d / "WRITELAYER";
    CP.DefaultDataPath = d / "DEFAULTDATA";
    CP.DefPrefixPath   = d / "DEFPREFIX";
    CP.PrefixRoot      = "";
    CP.KeepRegHives    = { "user.reg" };
    CP.SubComponentsArray = nlohmann::ordered_json::array();
    CP.SubComponentsArray.push_back({{"TYPE","RegEdit"},{"REGPATH","HKCU\\Software\\Game"},
                                     {"ARCHITECTURE","64"},{"OVERRIDE",false},{"KEYVALUES",{{"Lang","english"}}}});
    {
        RegistryWrapper W;
        nlohmann::ordered_json S = nlohmann::ordered_json::array();
        S.push_back({{"TYPE","RegEdit"},{"REGPATH","HKCU\\Software\\Game"},{"ARCHITECTURE","64"},
                     {"OVERRIDE",false},{"KEYVALUES",{{"Lang","romana"}}}});
        W.ApplyRegEdits(S, false);
        fs::create_directories(CP.UserDataPath / "REGISTRY");
        W.SavePrefix(CP.UserDataPath / "REGISTRY");
    }
    CHECK(RegistryLayer::BuildDefaultData(CP));
    const std::string Hive = ReadFileAt(CP.DefaultDataPath / "user.reg");
    CHECK(Hive.find("romana") != std::string::npos);              // user's choice wins
    CHECK(Hive.find("\"english\"") == std::string::npos);         // base default overridden
    fs::remove_all(d);
}
