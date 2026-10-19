#include "vgtest.h"
#include "registrylayer.h"
#include "launchparams.h"

#include <filesystem>
#include <fstream>

// RegistryLayer's registry persistence: KEPT KEYS are captured at teardown into the instance's REGKEYS store and merged
// back into the composed hives (DEFAULTDATA) on the next launch. Real temp dirs + real hive files, like the fileedits
// tests. (Keys are the only registry persistence: a whole hive can't be kept.)

namespace fs = std::filesystem;

static fs::path RlTmp(const char *tag)
{
    fs::path d = fs::temp_directory_path() / (std::string("vg_reglayer_") + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

static std::string ReadFileAt(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#include "registrywrapper.h"
#include <nlohmann/json.hpp>

// A RegEdit subcomponent: one key and its values, as the launch engine passes base edits around.
static nlohmann::ordered_json RegEdit(const char *Key, const nlohmann::ordered_json &Values)
{
    return {{"TYPE","RegEdit"},{"REGPATH",Key},{"ARCHITECTURE","64"},{"OVERRIDE",false},{"KEYVALUES",Values}};
}

// Write a hive set (at Dir) holding the given RegEdits — a session's runtime hives, or a durable store.
static void WriteHives(const fs::path &Dir, const nlohmann::ordered_json &Edits)
{
    RegistryWrapper W;
    CHECK(W.ApplyRegEdits(Edits, false));
    fs::create_directories(Dir);
    CHECK(W.SavePrefix(Dir));
}

static ContainerParams KeyParams(const fs::path &d, const char *PrefixRoot)
{
    ContainerParams CP(d);
    CP.PrefixGenerate  = true;
    CP.UserDataPath    = d / "USERDATA";
    CP.WriteLayerPath  = d / "WRITELAYER";
    CP.DefaultDataPath = d / "DEFAULTDATA";
    CP.DefPrefixPath   = d / "DEFPREFIX";      // empty — zero-copy prefix model
    CP.RuntimePath     = d / "RUNTIME";
    CP.PrefixRoot      = PrefixRoot;
    CP.KeepRegKeys     = { "HKCU\\Software\\Game" };
    CP.SubComponentsArray = nlohmann::ordered_json::array();
    return CP;
}

// The save round trip: a kept key's session values reach the store at teardown — and ONLY the kept key (the rest of
// the session's registry is pristine-by-default) — and come back in the next session's composed hive.
TEST(reglayer_kept_key_roundtrips_through_the_store)
{
    auto d = RlTmp("keyrt");
    ContainerParams CP = KeyParams(d, "pfx");
    nlohmann::ordered_json Session = nlohmann::ordered_json::array();
    Session.push_back(RegEdit("HKCU\\Software\\Game", {{"Save","42"}}));
    Session.push_back(RegEdit("HKCU\\Software\\NotKept", {{"Junk","1"}}));
    WriteHives(CP.RuntimePath / "pfx", Session);

    CHECK(RegistryLayer::CapturePersistRegKeys(CP));
    const std::string Store = ReadFileAt(CP.UserDataPath / "REGKEYS" / "user.reg");
    CHECK(Store.find("\"Save\"=\"42\"") != std::string::npos);
    CHECK(Store.find("Junk") == std::string::npos);        // not kept → not saved

    CHECK(RegistryLayer::BuildDefaultData(CP));            // next launch
    const std::string Hive = ReadFileAt(CP.DefaultDataPath / "pfx" / "user.reg");
    CHECK(Hive.find("\"Save\"=\"42\"") != std::string::npos);
    fs::remove_all(d);
}

// A later session that never touched the kept key keeps what earlier sessions saved (a key absent from the session
// is "kept prior", never dropped) — while a session that did change it replaces the saved values.
TEST(reglayer_store_accumulates_across_sessions)
{
    auto d = RlTmp("keyacc");
    ContainerParams CP = KeyParams(d, "");
    WriteHives(CP.RuntimePath, nlohmann::ordered_json::array({ RegEdit("HKCU\\Software\\Game", {{"Save","1"}}) }));
    CHECK(RegistryLayer::CapturePersistRegKeys(CP));

    fs::remove_all(CP.RuntimePath);                        // session 2: the key is absent
    WriteHives(CP.RuntimePath, nlohmann::ordered_json::array({ RegEdit("HKCU\\Software\\Other", {{"X","1"}}) }));
    CHECK(RegistryLayer::CapturePersistRegKeys(CP));
    CHECK(ReadFileAt(CP.UserDataPath / "REGKEYS" / "user.reg").find("\"Save\"=\"1\"") != std::string::npos);

    fs::remove_all(CP.RuntimePath);                        // session 3: the user changed it
    WriteHives(CP.RuntimePath, nlohmann::ordered_json::array({ RegEdit("HKCU\\Software\\Game", {{"Save","2"}}) }));
    CHECK(RegistryLayer::CapturePersistRegKeys(CP));
    const std::string Store = ReadFileAt(CP.UserDataPath / "REGKEYS" / "user.reg");
    CHECK(Store.find("\"Save\"=\"2\"") != std::string::npos);
    CHECK(Store.find("\"Save\"=\"1\"") == std::string::npos);
    fs::remove_all(d);
}

// Inside a kept key the user's saved value wins over the package's base edit; base edits OUTSIDE kept keys (an EULA
// marker under another key) still apply.
TEST(reglayer_saved_key_wins_and_other_base_edits_apply)
{
    auto d = RlTmp("keywin");
    ContainerParams CP = KeyParams(d, "");
    CP.SubComponentsArray.push_back(RegEdit("HKCU\\Software\\Game", {{"Lang","english"}}));
    CP.SubComponentsArray.push_back(RegEdit("HKCU\\Software\\Seeded\\1.0", {{"FIRSTRUN",true}}));
    WriteHives(CP.UserDataPath / "REGKEYS", nlohmann::ordered_json::array({ RegEdit("HKCU\\Software\\Game", {{"Lang","romana"}}) }));

    CHECK(RegistryLayer::BuildDefaultData(CP));
    const std::string Hive = ReadFileAt(CP.DefaultDataPath / "user.reg");
    CHECK(Hive.find("romana") != std::string::npos);              // the user's choice wins
    CHECK(Hive.find("\"english\"") == std::string::npos);
    CHECK(Hive.find("FIRSTRUN") != std::string::npos);            // an unrelated base edit still lands
    fs::remove_all(d);
}

// Nothing saved yet (first launch) and a kept key the session never created: clean no-ops, never a crash.
TEST(reglayer_registry_persist_graceful_when_empty)
{
    auto d = RlTmp("keyempty");
    ContainerParams CP = KeyParams(d, "");
    CHECK(RegistryLayer::CapturePersistRegKeys(CP));       // no runtime hives at all → nothing captured, still OK
    CHECK(!fs::exists(CP.UserDataPath / "REGKEYS"));
    CHECK(RegistryLayer::BuildDefaultData(CP));            // no store, no edits → nothing to materialise
    fs::remove_all(d);
}
