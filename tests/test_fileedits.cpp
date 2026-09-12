#include "vgtest.h"
#include "fileedits.h"
#include "launchparams.h"   // ContainerParams — ProcessFileEdits takes one

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
std::filesystem::path TmpDir(const char * tag)
{
    auto d = std::filesystem::temp_directory_path() / ("vgfe_" + std::string(tag) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
std::string ReadAll(const std::filesystem::path & p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void Write(const std::filesystem::path & p, const std::string & s) { std::ofstream(p, std::ios::binary) << s; }
}

// AppendLine creates the file and appends each line in call order — the load-order primitive (openmw.cfg content=).
TEST(appendline_creates_and_appends_in_order)
{
    auto dir = TmpDir("order"); auto cfg = dir / "openmw.cfg";
    CHECK(FileEdits::AppendLine("content=Morrowind.esm", cfg));
    CHECK(FileEdits::AppendLine("content=Tribunal.esm", cfg));
    CHECK(FileEdits::AppendLine("content=BetterClothes.esp", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("content=Morrowind.esm\ncontent=Tribunal.esm\ncontent=BetterClothes.esp\n"));
    std::filesystem::remove_all(dir);
}

// Idempotent: re-appending an identical line is a no-op (so re-launching doesn't duplicate the load order).
TEST(appendline_is_idempotent)
{
    auto dir = TmpDir("idem"); auto cfg = dir / "f.cfg";
    CHECK(FileEdits::AppendLine("content=A", cfg));
    CHECK(FileEdits::AppendLine("content=B", cfg));
    CHECK(FileEdits::AppendLine("content=A", cfg));  // dup of line 1 → skipped
    CHECK_EQ(ReadAll(cfg), std::string("content=A\ncontent=B\n"));
    std::filesystem::remove_all(dir);
}

// Appends cleanly onto a pre-shipped config that doesn't end in a newline (no fused line).
TEST(appendline_separates_unterminated_last_line)
{
    auto dir = TmpDir("nl"); auto cfg = dir / "f.cfg";
    Write(cfg, "data=\"/data/vanilla\"");          // no trailing newline
    CHECK(FileEdits::AppendLine("content=Morrowind.esm", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("data=\"/data/vanilla\"\ncontent=Morrowind.esm\n"));
    std::filesystem::remove_all(dir);
}

// ---- ConfigWrite (P7 fill-in: only AppendLine was covered) ----

// Prefix-matched line replacement: any line starting with Key becomes Key+Value; others untouched.
TEST(configwrite_replaces_prefixed_line)
{
    auto dir = TmpDir("cw"); auto cfg = dir / "settings.ini";
    Write(cfg, "Resolution=640x480\nFullscreen=1\nVolume=8\n");
    CHECK(FileEdits::ConfigWrite("Resolution=", "1920x1080", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("Resolution=1920x1080\nFullscreen=1\nVolume=8\n"));
    std::filesystem::remove_all(dir);
}

// A missing file fails loudly (false) instead of silently creating garbage.
TEST(configwrite_missing_file_fails)
{
    auto dir = TmpDir("cwm");
    CHECK(!FileEdits::ConfigWrite("K=", "v", dir / "nope.ini"));
    std::filesystem::remove_all(dir);
}

// Multiple lines sharing the prefix ALL get replaced (documented prefix semantics).
TEST(configwrite_replaces_every_prefixed_line)
{
    auto dir = TmpDir("cwa"); auto cfg = dir / "multi.ini";
    Write(cfg, "content=A\ncontent=B\nother=x\n");
    CHECK(FileEdits::ConfigWrite("content=", "Z", cfg));
    CHECK_EQ(ReadAll(cfg), std::string("content=Z\ncontent=Z\nother=x\n"));
    std::filesystem::remove_all(dir);
}

// ---- FileOverwrite ----

// Whole-content write, creating parents (the WC3 .w3k key-file path).
TEST(fileoverwrite_creates_parents_and_replaces)
{
    auto dir = TmpDir("fo"); auto f = dir / "sub" / "deep" / "roc.w3k";
    CHECK(FileEdits::FileOverwrite("ABCD-1234", f));
    CHECK_EQ(ReadAll(f), std::string("ABCD-1234"));
    CHECK(FileEdits::FileOverwrite("NEW", f));      // second write REPLACES (trunc), not appends
    CHECK_EQ(ReadAll(f), std::string("NEW"));
    std::filesystem::remove_all(dir);
}

//A package arrives from a peer by CID, so a FileEdit's FILE is UNTRUSTED INPUT. Neither stripping a leading
//separator nor is_absolute() stops a "../" climb, and following one is an arbitrary file write outside the
//runtime, at launch, from data someone else authored.
TEST(a_fileedit_path_that_climbs_out_of_its_base_is_refused)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_escape";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base);
    const std::filesystem::path Outside = Root / "outside.txt";

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","../outside.txt"},
                               {"MODE","Overwrite"},{"VALUE","escaped\n"}}});

    const bool Ok = FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base);
    CHECK(!Ok);                                             // reported as a FAILED edit, not quietly skipped
    CHECK(!std::filesystem::exists(Outside));               // and nothing was written outside the base
    std::filesystem::remove_all(Root);
}

//...while an ordinary nested path still works, so the guard is not simply refusing everything.
TEST(a_fileedit_path_inside_its_base_still_applies)
{
    const std::filesystem::path Root = std::filesystem::temp_directory_path() / "vg_fe_ok";
    std::filesystem::remove_all(Root);
    const std::filesystem::path Base = Root / "base";
    std::filesystem::create_directories(Base);

    ContainerParams CP(Root / "PKG");
    CP.SubComponentsArray = nlohmann::ordered_json::array({
        nlohmann::ordered_json{{"TYPE","FileEdit"},{"FILE","deep/nested/ok.txt"},
                               {"MODE","Overwrite"},{"VALUE","fine\n"}}});

    CHECK(FileEdits::ProcessFileEdits(CP, /*OverridePass=*/false, Base));
    CHECK(std::filesystem::exists(Base / "deep" / "nested" / "ok.txt"));
    std::filesystem::remove_all(Root);
}
