#include "vgtest.h"
#include "persistlayer.h"
#include "launchparams.h"

#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

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

// A file the game deleted inside a kept folder (a vidyagodfs marker, the file itself absent) is the user's state:
// a package default (IF_UNSAVED) written beneath it would be hidden by the marker, so it is not applied.
TEST(persistlayer_a_file_deleted_in_a_kept_folder_counts_as_the_users)
{
    auto d = PlTmp("keptdeleted");
    ContainerParams CP(d);
    CP.UserDataPath = d / "USERDATA";
    CP.KeepDirs     = { { "pfx/drive_c/9/saves", "Saves", true } };
    const fs::path K = CP.UserDataPath / "Saves";
    fs::create_directories(K / "sub");
    CHECK(!PersistLayer::HasSavedCopy(CP, "pfx/drive_c/9/saves/prefs.ini"));
    WriteFileAt(K / ".wh.prefs.ini", "");
    CHECK(PersistLayer::HasSavedCopy(CP, "pfx/drive_c/9/saves/prefs.ini"));          // deleted by the game
    WriteFileAt(K / ".wh.old", "");
    CHECK(PersistLayer::HasSavedCopy(CP, "pfx/drive_c/9/saves/old/a.cfg"));          // under a deleted folder
    WriteFileAt(K / "sub/.wh..wh..opq", "");
    CHECK(PersistLayer::HasSavedCopy(CP, "pfx/drive_c/9/saves/sub/b.cfg"));          // under a folder made again
    CHECK(!PersistLayer::HasSavedCopy(CP, "pfx/drive_c/9/saves/other.ini"));
    fs::remove_all(d);
}

// The instance folder and the session's writelayer are, or were, writable by the sandboxed game, which can leave a
// link where a kept file goes (Config -> ~/.bashrc). The launcher writes there OUTSIDE the sandbox, so a copy that
// followed the link would put the game's bytes into any file of the user's. Capture replaces the link with the file;
// the file it named is untouched. Teeth: copy_file onto the link (the victim gets the save).
TEST(persistlayer_capture_never_writes_through_a_link_in_the_store)
{
    auto d = PlTmp("cap_link");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/game/config.ini", "Config", true },
                          { "pfx/drive_c/749/player*.hki", "Hotkeys", true } };
    WriteFileAt(d / "victim/.bashrc", "the user's own\n");
    fs::create_directories(CP.UserDataPath);
    fs::create_symlink(d / "victim/.bashrc", CP.UserDataPath / "Config");      // planted by the game
    fs::create_symlink(d / "victim", CP.UserDataPath / "Hotkeys");             // a whole store folder, too
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/game/config.ini", "game bytes\n");
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/749/player1.hki", "hotkeys");
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(d / "victim/.bashrc"), std::string("the user's own\n"));
    CHECK(!fs::exists(d / "victim/player1.hki"));
    CHECK(fs::symlink_status(CP.UserDataPath / "Config").type() == fs::file_type::regular);
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Config"), std::string("game bytes\n"));
    CHECK(fs::symlink_status(CP.UserDataPath / "Hotkeys").type() == fs::file_type::directory);
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player1.hki"), std::string("hotkeys"));
    fs::remove_all(d);
}

// Seeding writes into the writelayer, which a session left in place can hold the game's links: a folder on the way
// to the kept file that is a link to the real home must not take the seed there. A link in the store is not read
// either (it is not a saved file; the store never holds one after a capture), and a link the game left at the kept
// file's runtime path is not taken for its save. Teeth: create_directories + copy_file through the folder link (the
// victim folder gets config.ini); read the stored link (the writelayer gets the linked secret); capture the runtime
// link (the store gets the secret's bytes).
TEST(persistlayer_seed_and_capture_never_follow_a_link)
{
    auto d = PlTmp("seed_link");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/game/config.ini", "Config", true },
                          { "pfx/drive_c/game/secret.ini", "Secret", true } };
    fs::create_directories(d / "victim");
    WriteFileAt(d / "outside/secret", "not the game's");
    WriteFileAt(CP.UserDataPath / "Config", "saved=1\n");
    fs::create_symlink(d / "outside/secret", CP.UserDataPath / "Secret");
    fs::create_directories(CP.WriteLayerPath / "pfx/drive_c");
    fs::create_symlink(d / "victim", CP.WriteLayerPath / "pfx/drive_c/game");
    CHECK(!PersistLayer::SeedPersistFiles(CP));                                 // the linked Secret is reported
    CHECK(!fs::exists(d / "victim/config.ini"));
    CHECK(fs::symlink_status(CP.WriteLayerPath / "pfx/drive_c/game").type() == fs::file_type::directory);
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/game/config.ini"), std::string("saved=1\n"));
    CHECK(!fs::exists(CP.WriteLayerPath / "pfx/drive_c/game/secret.ini"));

    fs::create_directories(CP.RuntimePath / "pfx/drive_c/game");
    fs::remove(CP.UserDataPath / "Secret");
    fs::create_symlink(d / "outside/secret", CP.RuntimePath / "pfx/drive_c/game/secret.ini");
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK(!fs::exists(CP.UserDataPath / "Secret"));
    fs::remove_all(d);
}

// The same for a pattern keep: a stored folder that is a link is not seeded from, and a link among the matches the
// session left is not captured. Teeth: is_directory on the store (follows: the linked secret is seeded); is_regular_file
// on the runtime entry (follows: the secret lands in the store).
TEST(persistlayer_pattern_keep_never_follows_a_link)
{
    auto d = PlTmp("pattern_link");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "pfx/drive_c/749/player*.hki", "Hotkeys", true } };
    WriteFileAt(d / "outside/player9.hki", "not the game's");
    fs::create_directories(CP.UserDataPath);
    fs::create_symlink(d / "outside", CP.UserDataPath / "Hotkeys");
    CHECK(PersistLayer::SeedPersistFiles(CP));
    CHECK(!fs::exists(CP.WriteLayerPath / "pfx/drive_c/749/player9.hki"));

    fs::remove(CP.UserDataPath / "Hotkeys");
    WriteFileAt(CP.RuntimePath / "pfx/drive_c/749/player1.hki", "one");
    fs::create_symlink(d / "outside/player9.hki", CP.RuntimePath / "pfx/drive_c/749/player9.hki");
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Hotkeys/player1.hki"), std::string("one"));
    CHECK(!fs::exists(CP.UserDataPath / "Hotkeys/player9.hki"));
    fs::remove_all(d);
}

// The guard itself: a path climbing out of its root is refused, and so is a folder on the way that is a plain file.
TEST(persistlayer_prepare_write_path_refuses_escapes)
{
    auto d = PlTmp("prepare");
    std::string Why;
    CHECK(!PersistLayer::PrepareWritePath(d / "root", "../outside.txt", Why));
    CHECK(!PersistLayer::PrepareWritePath(d / "root", "/etc/passwd", Why));
    WriteFileAt(d / "root/file", "x");
    CHECK(!PersistLayer::PrepareWritePath(d / "root", "file/inner.txt", Why));
    CHECK(PersistLayer::PrepareWritePath(d / "root", "a/b/c.txt", Why));
    CHECK(fs::is_directory(d / "root/a/b"));
    fs::remove_all(d);
}

// The writelayer sits in the session tree the game could write; one left in place (a busy mount) can have been
// swapped for a link to the real home. Seeding replaces it rather than writing the kept file through it. Teeth: anchor
// the seed at the writelayer itself (the victim folder gets pfx/.../config.ini).
TEST(persistlayer_seed_replaces_a_writelayer_that_is_a_link)
{
    auto d = PlTmp("wl_link");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "TEMP/WRITELAYER";
    CP.KeepFiles      = { { "pfx/drive_c/game/config.ini", "Config", true } };
    WriteFileAt(CP.UserDataPath / "Config", "saved=1\n");
    fs::create_directories(d / "victim");
    fs::create_directories(d / "TEMP");
    fs::create_symlink(d / "victim", CP.WriteLayerPath);
    CHECK(PersistLayer::SeedPersistFiles(CP));
    CHECK(!fs::exists(d / "victim/pfx"));
    CHECK(fs::symlink_status(CP.WriteLayerPath).type() == fs::file_type::directory);
    CHECK_EQ(ReadFileAt(CP.WriteLayerPath / "pfx/drive_c/game/config.ini"), std::string("saved=1\n"));
    fs::remove_all(d);
}

// A kept file the game never rewrote comes from a read-only package layer (the overlay shows it r-x). The stored copy
// and the next session's seeded copy must still be the user's writable file: copying the source's mode made Duke
// Nukem 3D fail to open its own DUKE3D.CFG ("Permission denied") and the next capture fail to overwrite the store.
TEST(persistlayer_kept_files_stay_writable_whatever_their_source_mode)
{
    auto d = PlTmp("ro");
    ContainerParams CP(d);
    CP.UserDataPath   = d / "USERDATA";
    CP.WriteLayerPath = d / "WRITELAYER";
    CP.RuntimePath    = d / "RUNTIME";
    CP.KeepFiles      = { { "game/DUKE3D.CFG", "Duke settings", true } };
    const fs::path Session = CP.RuntimePath / "game/DUKE3D.CFG";
    WriteFileAt(Session, "first\n");
    fs::permissions(Session, fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace);                                           // r-x as the overlay shows it
    CHECK(PersistLayer::CapturePersistFiles(CP));
    const auto Writable = [](const fs::path &P) { return (fs::status(P).permissions() & fs::perms::owner_write) != fs::perms::none; };
    CHECK(Writable(CP.UserDataPath / "Duke settings"));
    CHECK(PersistLayer::SeedPersistFiles(CP));
    const fs::path Seeded = CP.WriteLayerPath / "game/DUKE3D.CFG";
    CHECK(Writable(Seeded));
    { std::ofstream Out(Seeded, std::ios::binary | std::ios::trunc); CHECK(Out.good()); Out << "changed\n"; }
    CHECK_EQ(ReadFileAt(Seeded), std::string("changed\n"));
    // a store copy left read-only (by an earlier build) is overwritten, not refused
    fs::permissions(CP.UserDataPath / "Duke settings", fs::perms::owner_read, fs::perm_options::replace);
    fs::permissions(Session, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add);
    WriteFileAt(Session, "second\n");
    CHECK(PersistLayer::CapturePersistFiles(CP));
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Duke settings"), std::string("second\n"));
    CHECK(Writable(CP.UserDataPath / "Duke settings"));
    fs::remove_all(d);
}

#ifndef _WIN32
// A capture whose copy fails part way (a full disk, a read error on the overlay) keeps the previous save: the copy
// goes to a temporary file renamed into place only when complete. Copying over the stored file truncated it first,
// so the failure destroyed the last good save as well. A file-size limit stands in for the full disk.
TEST(persistlayer_a_failed_capture_keeps_the_previous_save)
{
    auto d = PlTmp("partial");
    ContainerParams CP(d);
    CP.UserDataPath = d / "USERDATA";
    CP.RuntimePath  = d / "RUNTIME";
    CP.KeepFiles    = { { "game/save.dat", "Save", true } };
    WriteFileAt(CP.UserDataPath / "Save", "the last good save\n");
    WriteFileAt(CP.RuntimePath / "game/save.dat", std::string(1 << 20, 'x'));          // 1 MiB, past the limit below
    struct rlimit Old {};
    getrlimit(RLIMIT_FSIZE, &Old);
    struct rlimit Small = Old; Small.rlim_cur = 4096;
    auto *OldHandler = signal(SIGXFSZ, SIG_IGN);                                        // a short write, not a kill
    setrlimit(RLIMIT_FSIZE, &Small);
    const bool Ok = PersistLayer::CapturePersistFiles(CP);
    setrlimit(RLIMIT_FSIZE, &Old);
    signal(SIGXFSZ, OldHandler);
    CHECK(!Ok);                                                                          // the failure is reported…
    CHECK_EQ(ReadFileAt(CP.UserDataPath / "Save"), std::string("the last good save\n")); // …and the old save survives
    CHECK(!fs::exists(CP.UserDataPath / "Save.vgcopy"));                                 // no half copy left behind
    fs::remove_all(d);
}
#endif
