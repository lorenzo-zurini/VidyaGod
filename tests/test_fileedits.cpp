#include "vgtest.h"
#include "fileedits.h"

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
    FileEdits::AppendLine("content=Morrowind.esm", cfg);
    FileEdits::AppendLine("content=Tribunal.esm", cfg);
    FileEdits::AppendLine("content=BetterClothes.esp", cfg);
    CHECK_EQ(ReadAll(cfg), std::string("content=Morrowind.esm\ncontent=Tribunal.esm\ncontent=BetterClothes.esp\n"));
    std::filesystem::remove_all(dir);
}

// Idempotent: re-appending an identical line is a no-op (so re-launching doesn't duplicate the load order).
TEST(appendline_is_idempotent)
{
    auto dir = TmpDir("idem"); auto cfg = dir / "f.cfg";
    FileEdits::AppendLine("content=A", cfg);
    FileEdits::AppendLine("content=B", cfg);
    FileEdits::AppendLine("content=A", cfg);   // dup of line 1 → skipped
    CHECK_EQ(ReadAll(cfg), std::string("content=A\ncontent=B\n"));
    std::filesystem::remove_all(dir);
}

// Appends cleanly onto a pre-shipped config that doesn't end in a newline (no fused line).
TEST(appendline_separates_unterminated_last_line)
{
    auto dir = TmpDir("nl"); auto cfg = dir / "f.cfg";
    Write(cfg, "data=\"/data/vanilla\"");          // no trailing newline
    FileEdits::AppendLine("content=Morrowind.esm", cfg);
    CHECK_EQ(ReadAll(cfg), std::string("data=\"/data/vanilla\"\ncontent=Morrowind.esm\n"));
    std::filesystem::remove_all(dir);
}
