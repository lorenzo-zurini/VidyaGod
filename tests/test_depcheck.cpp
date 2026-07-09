#include "vgtest.h"
#include "depcheck.h"

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
    DepItem Missing; Missing.kind = Kind::Binary; Missing.Token = "umu-run";
    DepItem File; File.kind = Kind::File; File.Token = "vidyagodfs";
    DepItem Vk; Vk.kind = Kind::VulkanDriver;

    CHECK(Check(Bin, St)     == Status::Ok);
    CHECK(Check(Missing, St) == Status::Missing);
    CHECK(Check(File, St)    == Status::Ok);
    CHECK(Check(Vk, St)      == Status::Ok);
    St.VulkanIcd = false;
    CHECK(Check(Vk, St)      == Status::Missing);
}

TEST(alldeps_table_is_sane)
{
    const auto &Deps = AllDeps();
    CHECK(Deps.size() > 10);
    bool HaveVulkanLoader = false, HaveCritical = false;
    for (const auto &I : Deps)
    {
        if (I.Id == "vulkan-loader") { HaveVulkanLoader = true; CHECK(!I.Sonames.empty()); }
        if (I.Critical) HaveCritical = true;
        // Every Lib item carries at least an Arch package name for both 64 and (unless core) 32-bit.
        if (I.kind == Kind::Lib) CHECK(I.Pkg64.count(Distro::Arch) == 1);
    }
    CHECK(HaveVulkanLoader);
    CHECK(HaveCritical);
}
