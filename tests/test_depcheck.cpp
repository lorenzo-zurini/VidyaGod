#include "vgtest.h"
#include "depcheck.h"

#include <algorithm>
#include <string>

using namespace DepCheck;

// A representative slice of real `ldconfig -p` output: libvulkan present 64+32, gnutls 64-only, giflib 32-only,
// SDL2 64+32. (64-bit tag has "x86-64"; the bare "(libc6)" and "x86-32" are 32-bit.)
static const char *kLdconfig =
    "\tlibvulkan.so.1 (libc6,x86-64) => /usr/lib/libvulkan.so.1\n"
    "\tlibvulkan.so.1 (libc6) => /usr/lib32/libvulkan.so.1\n"
    "\tlibgnutls.so.30 (libc6,x86-64) => /usr/lib/libgnutls.so.30\n"
    "\tlibgif.so.7 (libc6,x86-32) => /usr/lib32/libgif.so.7\n"
    "\tlibSDL2-2.0.so.0 (libc6,x86-64) => /usr/lib/libSDL2-2.0.so.0\n"
    "\tlibSDL2-2.0.so.0 (libc6) => /usr/lib32/libSDL2-2.0.so.0\n";

TEST(parse_ldconfig_arch_tags)
{
    const auto M = ParseLdconfig(kLdconfig);
    CHECK(M.at("libvulkan.so.1").first);          // 64
    CHECK(M.at("libvulkan.so.1").second);         // 32
    CHECK(M.at("libgnutls.so.30").first);         // 64
    CHECK(!M.at("libgnutls.so.30").second);       // no 32
    CHECK(!M.at("libgif.so.7").first);            // no 64
    CHECK(M.at("libgif.so.7").second);            // 32 (x86-32 tag)
    CHECK(M.at("libSDL2-2.0.so.0").first && M.at("libSDL2-2.0.so.0").second);
    CHECK(M.find("libnotthere.so") == M.end());
}

TEST(check_lib_ok_partial_missing)
{
    SystemState St;
    St.Libs = ParseLdconfig(kLdconfig);

    auto Lib = [](std::string so) {
        DepItem I; I.kind = Kind::Lib; I.Sonames = { std::move(so) }; return I;
    };
    CHECK(Check(Lib("libvulkan.so.1"),  St) == Status::Ok);       // 64+32
    CHECK(Check(Lib("libgnutls.so.30"), St) == Status::Partial);  // 64 only → 32-bit games may crash
    CHECK(Check(Lib("libgif.so.7"),     St) == Status::Missing);  // 32 only, no 64 → not usable
    CHECK(Check(Lib("libabsent.so.9"),  St) == Status::Missing);
}

TEST(check_binary_file_vulkan)
{
    SystemState St;
    St.Binaries.insert("fusermount3");
    St.Files.insert("vidyagodfs");
    St.VulkanIcd = true;

    DepItem Bin; Bin.kind = Kind::Binary; Bin.Token = "fusermount3";
    DepItem Missing; Missing.kind = Kind::Binary; Missing.Token = "not-a-tool";
    DepItem File; File.kind = Kind::File; File.Token = "vidyagodfs";
    DepItem Vk; Vk.kind = Kind::VulkanDriver;

    CHECK(Check(Bin, St)     == Status::Ok);
    CHECK(Check(Missing, St) == Status::Missing);
    CHECK(Check(File, St)    == Status::Ok);
    CHECK(Check(Vk, St)      == Status::Ok);
    St.VulkanIcd = false;
    CHECK(Check(Vk, St)      == Status::Missing);
}

TEST(check_sandbox_and_winfsp_read_their_probe)
{
    SystemState St;
    DepItem Sb; Sb.kind = Kind::Sandbox;
    DepItem Fsp; Fsp.kind = Kind::WinFsp;
    CHECK(Check(Sb, St)  == Status::Missing);
    CHECK(Check(Fsp, St) == Status::Missing);
    St.SandboxUsable = true;
    St.WinFsp = true;
    CHECK(Check(Sb, St)  == Status::Ok);
    CHECK(Check(Fsp, St) == Status::Ok);
}

TEST(alldeps_table_is_sane)
{
    const auto &Deps = AllDeps();
    int Sandboxes = 0, Libs = 0;
    bool HaveMountHelper = false;
    for (const auto &I : Deps)
    {
        // The sandbox is mandatory, so it is a Core item; umu-run is not used by anything any more.
        if (I.kind == Kind::Sandbox) { ++Sandboxes; CHECK(I.Cat == Category::Core); }
        if (I.kind == Kind::Lib) ++Libs;
        if (I.Id == "vidyagodfs") HaveMountHelper = true;
        CHECK(I.Token != "umu-run");
        CHECK(I.Id != "umu-run");
        // Every item names what to install on its platform's primary target.
        CHECK(!I.Pkg64.empty());
#ifndef _WIN32
        if (I.kind == Kind::Lib) CHECK(I.Pkg64.count(Distro::Arch) == 1);
#endif
    }
    CHECK(Sandboxes == 1);
    CHECK(HaveMountHelper);
#ifdef _WIN32
    CHECK(Libs == 0);   // Wine's libraries don't apply on Windows
    CHECK(std::any_of(Deps.begin(), Deps.end(), [](const DepItem &I){ return I.kind == Kind::WinFsp; }));
#else
    CHECK(Libs > 10);
    CHECK(std::any_of(Deps.begin(), Deps.end(), [](const DepItem &I){ return I.Token == "fusermount3"; }));
#endif
}

// A failed launch names what this machine lacks: the Core items only (a missing library crashes some games, it doesn't
// stop VidyaGod launching them).
TEST(missing_core_names_core_items_only)
{
    SystemState St;
    St.SandboxUsable = true;
    St.WinFsp = true;
    for (const auto &I : AllDeps())
    {
        if (I.kind == Kind::Binary) St.Binaries.insert(I.Token);
        if (I.kind == Kind::File)   St.Files.insert(I.Token);
    }
    CHECK(MissingCore(St).empty());                        // every core item present, every library absent
    St.SandboxUsable = false;
    const auto M = MissingCore(St);
    CHECK_EQ(M.size(), (size_t)1);
    const auto Sb = std::find_if(AllDeps().begin(), AllDeps().end(), [](const DepItem &I){ return I.kind == Kind::Sandbox; });
    CHECK(Sb != AllDeps().end() && M.front() == Sb->Name);
}

TEST(probe_takes_the_sandbox_verdict_it_is_given)
{
    CHECK(ProbeSystem(true).SandboxUsable);
    CHECK(!ProbeSystem(false).SandboxUsable);
}

// The Dependencies page's "Copy install command": one command for the distro's package manager, each package once;
// nothing where VidyaGod knows no package manager. Teeth: drop the de-duplication and lib32-a is named twice.
TEST(install_command_names_each_package_once)
{
    CHECK_EQ(InstallCommand({ "lib32-a", "b", "lib32-a" }, Distro::Arch), std::string("sudo pacman -S --needed lib32-a b"));
    CHECK_EQ(InstallCommand({ "libx" }, Distro::Debian), std::string("sudo apt install libx"));
    CHECK(InstallCommand({ "libx" }, Distro::Unknown).empty());
    CHECK(InstallCommand({}, Distro::Arch).empty());
}
