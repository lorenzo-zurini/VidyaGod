#include "vgtest.h"
#include "persistlayer.h"
#include "launchparams.h"

#include <filesystem>
#include <fstream>

// PersistLayer file seed/capture had ZERO coverage — and the Path/Target split (durable name decoupled from the
// runtime location) is exactly where a wrong copy DIRECTION silently loses saves. These tests use distinct Path and
// Target so a seed/capture that confused the two would fail (with Path==Target the bug would be invisible).

namespace fs = std::filesystem;

static fs::path PlTmp(const char *tag)
{
    fs::path d = fs::temp_directory_path() / (std::string("vg_persistlayer_") + tag);
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

// Seed pulls the durable file from UserDataPath/<Target> and lays it down at WriteLayerPath/<Path> — the two are
// DIFFERENT strings, so a seed that read UserDataPath/<Path> (the old, path-keyed model) would find nothing.
TEST(persistlayer_seed_reads_target_and_writes_path)
{
    auto d = PlTmp("seed");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.KeepFiles      = { { "pfx/drive_c/game/config.ini", "Config", true } };   // Path != Target
    WriteFileAt(CP.UserDataPath / "Config", "saved=1\n");                        // durable home is Target
    CHECK(PersistLayer::SeedPersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/game/config.ini"), std::string("saved=1\n"));
    // and nothing was written at the Path-keyed durable location (proves the Target key, not the Path)
    CHECK(!fs::exists(CP.WriteLayerPath / "Config"));
    fs::remove_all(d);
}

// Capture reads the session file at RuntimePath/<Path> and writes it to the durable UserDataPath/<Target>; a full
// round-trip (capture then seed) must reproduce the session's bytes at the runtime location next launch.
TEST(persistlayer_capture_writes_target_and_roundtrips)
{
    auto d = PlTmp("cap");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/game/config.ini", "Config", true } };
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/game/config.ini", "saved=42\n");   // the session's write
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Config"), std::string("saved=42\n")); // captured to the Target home
    // …and it seeds an identical next session back at the runtime-relative Path.
    CHECK(PersistLayer::SeedPersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/game/config.ini"), std::string("saved=42\n"));
    fs::remove_all(d);
}

// Nothing declared / nothing stored → clean no-op success (first launch, or a package with no file persists).
TEST(persistlayer_empty_is_graceful)
{
    auto d = PlTmp("none");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/game/save.dat", "Save", true } };       // declared, but nothing on disk
    CHECK(PersistLayer::SeedPersistFiles(CP));       // durable source absent → skipped, still OK
    CHECK(PersistLayer::CapturePersistFiles(CP));    // session file absent → skipped, still OK
    CHECK(!fs::exists(CP.WriteLayerPath / "pfx/drive_c/game/save.dat"));
    fs::remove_all(d);
}
