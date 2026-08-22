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
