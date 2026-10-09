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

// A pattern keep (AoE2's per-profile hotkeys, player*.hki beside the executable): every matching file the session left
// is captured into the durable directory (Target) under its own name, and every stored one is seeded back beside the
// game; a non-matching neighbour is left alone, and a stored file the session deleted is dropped from the store.
// Matching ignores case, as the game's file system does. Teeth: treat the pattern as a plain file (nothing captured);
// skip the drop (a deleted profile's hotkeys come back); match case-sensitively (PLAYER3.HKI is lost).
TEST(persistlayer_pattern_keep_captures_seeds_and_drops_by_name)
{
    auto d = PlTmp("pattern");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/749/player*.hki", "Hotkeys", true } };
    const fs::path Game = CP.RuntimePath / "pfx/drive_c/749";
    WriteFileAt(Game / "player1.hki", "one");
    WriteFileAt(Game / "player2.hki", "two");
    WriteFileAt(Game / "PLAYER3.HKI", "three");
    WriteFileAt(Game / "player.nfz", "profiles");                               // a neighbour the pattern does not name
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player1.hki"), std::string("one"));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player2.hki"), std::string("two"));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/PLAYER3.HKI"), std::string("three"));
    CHECK(!fs::exists(CP.UserDataPath / "Hotkeys/player.nfz"));
    CHECK(PersistLayer::SeedPersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/749/player2.hki"), std::string("two"));
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/749/PLAYER3.HKI"), std::string("three"));
    CHECK(PersistLayer::HasSavedCopy(CP, "pfx/drive_c/749/player2.hki"));
    CHECK(!PersistLayer::HasSavedCopy(CP, "pfx/drive_c/749/player9.hki"));
    fs::remove(Game / "player2.hki");                                           // the user deleted that profile
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK(!fs::exists(CP.UserDataPath / "Hotkeys/player2.hki"));
    CHECK(fs::exists(CP.UserDataPath / "Hotkeys/player1.hki"));
    fs::remove_all(d);
}

// A pattern keep never takes "missing" for "deleted" unless this session put the file there: a stored profile whose
// seed failed (here the write layer's game folder is a file, so the copy cannot land) is absent from the runtime, and
// capture must keep the only good copy. Teeth: drop every stored match absent from the runtime (the old rule).
TEST(persistlayer_pattern_capture_keeps_a_file_it_could_not_seed)
{
    auto d = PlTmp("pattern_seedfail");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/749/player*.hki", "Hotkeys", true } };
    WriteFileAt(CP.UserDataPath / "Hotkeys/player2.hki", "saved");
    WriteFileAt(CP.WriteLayerPath / "pfx/drive_c/749", "not a directory");     // the seed's copy cannot land
    CHECK(!PersistLayer::SeedPersistFiles(CP));
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/749/player1.hki", "new");         // the session ran without player2
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player2.hki"), std::string("saved"));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player1.hki"), std::string("new"));
    fs::remove_all(d);
}

// Names compare ignoring case, as the game's file system does: a game that rewrites player1.hki as PLAYER1.HKI leaves
// ONE stored copy — the new one — never both (seeded together, they would shadow each other unpredictably), and the
// seeded name counts as still present. Which spelling the stored copy keeps is the host's business (NTFS keeps the
// old one when a file is overwritten), so the test counts copies and reads the content. Teeth: compare names
// case-sensitively (the fresh save is dropped, or both kept).
TEST(persistlayer_pattern_capture_matches_names_ignoring_case)
{
    auto d = PlTmp("pattern_case");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/749/player*.hki", "Hotkeys", true } };
    WriteFileAt(CP.UserDataPath / "Hotkeys/player1.hki", "old");
    CHECK(PersistLayer::SeedPersistFiles(CP));
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/749/PLAYER1.HKI", "new");
    CHECK(PersistLayer::CapturePersistFiles(CP));
    std::vector<fs::path> Stored;
    for (const auto &E : fs::directory_iterator(CP.UserDataPath / "Hotkeys")) Stored.push_back(E.path());
    CHECK_EQ(Stored.size(), size_t(1));
    if (Stored.size() == 1) CHECK_EQ(ReadFileAt(Stored[0]), std::string("new"));
    fs::remove_all(d);
}
