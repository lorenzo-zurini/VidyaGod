#include "depcheck.h"

#include <array>
#include <cstdio>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace DepCheck
{

// ---- distro ----------------------------------------------------------------
Distro DistroFromOsRelease()
{
    std::ifstream F("/etc/os-release");
    std::string Line;
    auto Val = [](std::string V) {
        if (!V.empty() && (V.front() == '"' || V.front() == '\'')) V.erase(V.begin());
        if (!V.empty() && (V.back()  == '"' || V.back()  == '\'')) V.pop_back();
        return V;
    };
    std::string Id, IdLike;
    while (std::getline(F, Line))
    {
        if      (Line.rfind("ID=", 0) == 0)       Id     = Val(Line.substr(3));
        else if (Line.rfind("ID_LIKE=", 0) == 0)  IdLike = Val(Line.substr(8));
    }
    const std::string Hay = Id + " " + IdLike;
    auto Has = [&](const char *S){ return Hay.find(S) != std::string::npos; };
    if (Has("arch"))   return Distro::Arch;      // arch, manjaro, endeavouros, cachyos (ID_LIKE=arch)
    if (Has("debian") || Has("ubuntu")) return Distro::Debian;  // debian, ubuntu, mint, pop (ID_LIKE=debian/ubuntu)
    if (Has("fedora") || Has("rhel"))   return Distro::Fedora;
    if (Has("suse"))   return Distro::OpenSUSE;
    return Distro::Unknown;
}

const char *DistroLabel(Distro D)
{
    switch (D) { case Distro::Arch: return "Arch"; case Distro::Debian: return "Debian/Ubuntu";
                 case Distro::Fedora: return "Fedora"; case Distro::OpenSUSE: return "openSUSE"; default: return "your distro"; }
}
const char *CategoryLabel(Category C)
{
    switch (C) { case Category::Core: return "Core tools"; case Category::Graphics: return "Graphics / Vulkan";
                 case Category::WineLibs: return "Wine / Proton libraries"; case Category::Audio: return "Audio";
                 case Category::Media: return "Media / codecs"; default: return ""; }
}

// ---- pure: parse `ldconfig -p` --------------------------------------------
// Lines look like:  "\tlibvulkan.so.1 (libc6,x86-64) => /usr/lib/libvulkan.so.1"
//                   "\tlibvulkan.so.1 (libc6) => /usr/lib32/libvulkan.so.1"   (bare libc6 == 32-bit on multilib)
std::map<std::string, std::pair<bool, bool>> ParseLdconfig(const std::string &LdconfigOutput)
{
    std::map<std::string, std::pair<bool, bool>> Out;
    std::istringstream Ss(LdconfigOutput);
    std::string Line;
    while (std::getline(Ss, Line))
    {
        const auto Lp = Line.find(" (");
        if (Lp == std::string::npos) continue;
        // soname = last whitespace-delimited token before " (" (trim leading tab/spaces)
        auto Start = Line.find_first_not_of(" \t");
        if (Start == std::string::npos || Start >= Lp) continue;
        const std::string Soname = Line.substr(Start, Lp - Start);
        const auto Rp = Line.find(')', Lp);
        if (Rp == std::string::npos) continue;
        const std::string Tag = Line.substr(Lp + 2, Rp - (Lp + 2));   // e.g. "libc6,x86-64" / "libc6" / "libc6,x86-32"
        const bool Is64 = Tag.find("x86-64") != std::string::npos;
        const bool Is32 = Tag.find("x86-32") != std::string::npos || Tag == "libc6";
        auto &E = Out[Soname];
        E.first  = E.first  || Is64;
        E.second = E.second || Is32;
    }
    return Out;
}

// ---- pure: classify one item ----------------------------------------------
Status Check(const DepItem &Item, const SystemState &St)
{
    switch (Item.kind)
    {
    case Kind::Binary:       return St.Binaries.count(Item.Token) ? Status::Ok : Status::Missing;
    case Kind::File:         return St.Files.count(Item.Token)    ? Status::Ok : Status::Missing;
    case Kind::VulkanDriver: return St.VulkanIcd                  ? Status::Ok : Status::Missing;
    case Kind::Lib:
    {
        bool B64 = false, B32 = false;
        for (const std::string &S : Item.Sonames)
        {
            auto It = St.Libs.find(S);
            if (It == St.Libs.end()) continue;
            B64 = B64 || It->second.first;
            B32 = B32 || It->second.second;
        }
        if (B64 && B32) return Status::Ok;
        if (B64)        return Status::Partial;   // 64-bit ok, 32-bit missing → 32-bit games may crash
        return Status::Missing;
    }
    }
    return Status::Missing;
}

// ---- probe the host --------------------------------------------------------
static std::string RunLdconfig()
{
    std::string Out;
    if (FILE *P = ::popen("ldconfig -p 2>/dev/null", "r"))
    {
        std::array<char, 4096> Buf;
        size_t N;
        while ((N = ::fread(Buf.data(), 1, Buf.size(), P)) > 0) Out.append(Buf.data(), N);
        ::pclose(P);
    }
    return Out;
}

static bool OnPath(const std::string &Name)
{
    const char *Path = ::getenv("PATH");
    if (!Path) return false;
    std::istringstream Ss(Path);
    std::string Dir;
    while (std::getline(Ss, Dir, ':'))
    {
        if (Dir.empty()) continue;
        if (::access((Dir + "/" + Name).c_str(), X_OK) == 0) return true;
    }
    return false;
}

static bool HasAnyVulkanIcd()
{
    // A Vulkan driver installs an ICD manifest here; without one the loader finds no GPU driver → DXVK/VKD3D crash.
    for (const char *Dir : { "/usr/share/vulkan/icd.d", "/etc/vulkan/icd.d" })
    {
        if (DIR *D = ::opendir(Dir))
        {
            for (dirent *E; (E = ::readdir(D)); )
            {
                const std::string N = E->d_name;
                if (N.size() > 5 && N.substr(N.size() - 5) == ".json") { ::closedir(D); return true; }
            }
            ::closedir(D);
        }
    }
    return false;
}

SystemState ProbeSystem()
{
    SystemState St;
    St.Libs      = ParseLdconfig(RunLdconfig());
    St.VulkanIcd = HasAnyVulkanIcd();
    St.distro    = DistroFromOsRelease();

    // Bundled files sit next to the app binary (resolved via /proc/self/exe — no Qt/QApplication needed).
    std::error_code Ec;
    const std::filesystem::path Self = std::filesystem::read_symlink("/proc/self/exe", Ec);
    const std::filesystem::path BinDir = Ec ? std::filesystem::path() : Self.parent_path();

    for (const DepItem &I : AllDeps())
    {
        if (I.kind == Kind::Binary) { if (OnPath(I.Token)) St.Binaries.insert(I.Token); }
        else if (I.kind == Kind::File)
        {
            if (!BinDir.empty() && std::filesystem::exists(BinDir / I.Token, Ec)) St.Files.insert(I.Token);
        }
    }
    return St;
}

std::vector<std::string> MissingCritical()
{
    const SystemState St = ProbeSystem();
    std::vector<std::string> Out;
    for (const DepItem &I : AllDeps())
        if (I.Critical && Check(I, St) == Status::Missing) Out.push_back(I.Name);
    return Out;
}

std::string InstallHint(const DepItem &Item, Status St, Distro D)
{
    const auto &Table = (St == Status::Partial && !Item.Pkg32.empty()) ? Item.Pkg32 : Item.Pkg64;
    auto It = Table.find(D);
    if (It != Table.end()) return It->second;
    // Fall back to the Arch name (our primary target) so the user at least knows the package.
    It = Table.find(Distro::Arch);
    return It != Table.end() ? It->second : std::string();
}

// ---- the researched table --------------------------------------------------
// Library set = the Lutris canonical Wine/Proton dependency list (github.com/lutris/docs WineDependencies.md) +
// VidyaGod's own tools. Every Lib is probed for 64- AND 32-bit (32-bit is the big crash source for 32-bit games).
const std::vector<DepItem> &AllDeps()
{
    static const std::vector<DepItem> Deps = []{
        using D = Distro;
        std::vector<DepItem> V;
        auto Bin = [&](std::string id, std::string name, std::string tok, std::string purpose, bool crit,
                       std::map<D,std::string> pkg){
            V.push_back({std::move(id), std::move(name), std::move(purpose), Category::Core, Kind::Binary, {}, std::move(tok), std::move(pkg), {}, crit}); };
        auto Lib = [&](std::string id, std::string name, Category cat, std::vector<std::string> so, std::string purpose,
                       std::map<D,std::string> p64, std::map<D,std::string> p32){
            V.push_back({std::move(id), std::move(name), std::move(purpose), cat, Kind::Lib, std::move(so), {}, std::move(p64), std::move(p32), false}); };

        // ---- Core tools (VidyaGod itself) ----
        Bin("fusermount3", "fusermount3", "fusermount3", "Mounts/unmounts game runtimes (FUSE 3) — required.", true,
            {{D::Arch,"fuse3"},{D::Debian,"fuse3"},{D::Fedora,"fuse3"},{D::OpenSUSE,"fuse3"}});
        V.push_back({"vidyagodfs","vidyagodfs","VidyaGod's bundled runtime filesystem — should ship beside the app.",
                     Category::Core, Kind::File, {}, "vidyagodfs",
                     {{D::Arch,"reinstall VidyaGod (must sit next to the binary)"}}, {}, true});
        Bin("umu-run", "umu-run", "umu-run", "The Wine/Proton game runner (umu-launcher) — required to launch Windows games.", true,
            {{D::Arch,"umu-launcher (AUR)"},{D::Debian,"umu-launcher (github.com/Open-Wine-Components/umu-launcher)"},
             {D::Fedora,"umu-launcher"},{D::OpenSUSE,"umu-launcher"}});
        Bin("bwrap", "bubblewrap (bwrap)", "bwrap", "Sandboxes launches + the friend LAN. Optional — falls back to an unsandboxed launch.", false,
            {{D::Arch,"bubblewrap"},{D::Debian,"bubblewrap"},{D::Fedora,"bubblewrap"},{D::OpenSUSE,"bubblewrap"}});

        // ---- Graphics / Vulkan ----
        Lib("vulkan-loader","Vulkan loader (libvulkan)", Category::Graphics, {"libvulkan.so.1"},
            "The Vulkan API loader — DXVK/VKD3D (DirectX→Vulkan) need it. Missing 32-bit crashes 32-bit games instantly.",
            {{D::Arch,"vulkan-icd-loader"},{D::Debian,"libvulkan1"},{D::Fedora,"vulkan-loader"}},
            {{D::Arch,"lib32-vulkan-icd-loader"},{D::Debian,"libvulkan1:i386"},{D::Fedora,"vulkan-loader.i686"}});
        V.push_back({"vulkan-driver","Vulkan GPU driver (ICD)","A GPU Vulkan driver must be installed, else there's no renderer and games crash on launch.",
                     Category::Graphics, Kind::VulkanDriver, {}, "",
                     {{D::Arch,"vulkan-radeon / vulkan-intel / nvidia-utils (+ the lib32- variant)"},
                      {D::Debian,"mesa-vulkan-drivers (+ :i386) or the NVIDIA driver"},
                      {D::Fedora,"mesa-vulkan-drivers (+ .i686) or the NVIDIA driver"}}, {}, false});
        Lib("opengl","OpenGL loader (libGL)", Category::Graphics, {"libGL.so.1"},
            "The OpenGL loader — many older/non-Vulkan games need it. Missing 32-bit crashes 32-bit OpenGL games.",
            {{D::Arch,"libglvnd"},{D::Debian,"libgl1"},{D::Fedora,"libglvnd-glx"}},
            {{D::Arch,"lib32-libglvnd"},{D::Debian,"libgl1:i386"},{D::Fedora,"libglvnd-glx.i686"}});
        Lib("gl-driver","OpenGL GPU driver", Category::Graphics, {"libGLX_mesa.so.0","libGLX_nvidia.so.0"},
            "The actual OpenGL renderer (Mesa or NVIDIA) — without it OpenGL games have no driver and crash on launch.",
            {{D::Arch,"mesa (vulkan-radeon/intel) or nvidia-utils"},{D::Debian,"libglx-mesa0 or the NVIDIA driver"},
             {D::Fedora,"mesa-libGL or the NVIDIA driver"}},
            {{D::Arch,"lib32-mesa or lib32-nvidia-utils"},{D::Debian,"libglx-mesa0:i386"},{D::Fedora,"mesa-libGL.i686"}});

        // ---- Wine / Proton core libraries (64 + 32) ----
        Lib("gnutls","GnuTLS", Category::WineLibs, {"libgnutls.so.30"},
            "TLS/HTTPS — many game launchers and online features fail or crash without it.",
            {{D::Arch,"gnutls"},{D::Debian,"libgnutls30"},{D::Fedora,"gnutls"}},
            {{D::Arch,"lib32-gnutls"},{D::Debian,"libgnutls30:i386"},{D::Fedora,"gnutls.i686"}});
        Lib("sdl2","SDL2", Category::WineLibs, {"libSDL2-2.0.so.0"},
            "Controller/gamepad + input handling used by Wine and many games.",
            {{D::Arch,"sdl2-compat"},{D::Debian,"libsdl2-2.0-0"},{D::Fedora,"SDL2"}},
            {{D::Arch,"lib32-sdl2-compat"},{D::Debian,"libsdl2-2.0-0:i386"},{D::Fedora,"SDL2.i686"}});
        Lib("sqlite","SQLite", Category::WineLibs, {"libsqlite3.so.0"},
            "Embedded database used by launchers and games for saves/config.",
            {{D::Arch,"sqlite"},{D::Debian,"libsqlite3-0"},{D::Fedora,"sqlite-libs"}},
            {{D::Arch,"lib32-sqlite"},{D::Debian,"libsqlite3-0:i386"},{D::Fedora,"sqlite-libs.i686"}});
        Lib("gstreamer","GStreamer (base)", Category::Media, {"libgstreamer-1.0.so.0"},
            "Media playback — in-game videos/cutscenes crash without it (add gst-plugins-good/bad for more codecs).",
            {{D::Arch,"gst-plugins-base-libs"},{D::Debian,"libgstreamer1.0-0"},{D::Fedora,"gstreamer1"}},
            {{D::Arch,"lib32-gst-plugins-base-libs"},{D::Debian,"libgstreamer1.0-0:i386"},{D::Fedora,"gstreamer1.i686"}});
        Lib("libva","libva (VA-API)", Category::Graphics, {"libva.so.2"},
            "Hardware video acceleration used by some games/media.",
            {{D::Arch,"libva"},{D::Debian,"libva2"},{D::Fedora,"libva"}},
            {{D::Arch,"lib32-libva"},{D::Debian,"libva2:i386"},{D::Fedora,"libva.i686"}});
        Lib("opencl","OpenCL (ocl-icd)", Category::WineLibs, {"libOpenCL.so.1"},
            "OpenCL loader — a few games use it for physics/compute.",
            {{D::Arch,"ocl-icd"},{D::Debian,"ocl-icd-libopencl1"},{D::Fedora,"ocl-icd"}},
            {{D::Arch,"lib32-ocl-icd"},{D::Debian,"ocl-icd-libopencl1:i386"},{D::Fedora,"ocl-icd.i686"}});
        Lib("gtk3","GTK 3", Category::WineLibs, {"libgtk-3.so.0"},
            "GUI toolkit some launchers/installers render with.",
            {{D::Arch,"gtk3"},{D::Debian,"libgtk-3-0"},{D::Fedora,"gtk3"}},
            {{D::Arch,"lib32-gtk3"},{D::Debian,"libgtk-3-0:i386"},{D::Fedora,"gtk3.i686"}});
        Lib("gif","giflib", Category::WineLibs, {"libgif.so.7"},
            "GIF image decoding used by Wine's image handling.",
            {{D::Arch,"giflib"},{D::Debian,"libgif7"},{D::Fedora,"giflib"}},
            {{D::Arch,"lib32-giflib"},{D::Debian,"libgif7:i386"},{D::Fedora,"giflib.i686"}});
        Lib("freetype","FreeType", Category::WineLibs, {"libfreetype.so.6"},
            "Font rendering — missing/garbled text or crashes on font load.",
            {{D::Arch,"freetype2"},{D::Debian,"libfreetype6"},{D::Fedora,"freetype"}},
            {{D::Arch,"lib32-freetype2"},{D::Debian,"libfreetype6:i386"},{D::Fedora,"freetype.i686"}});
        Lib("xcomposite","libXcomposite", Category::WineLibs, {"libXcomposite.so.1"},
            "X Composite extension used by Wine's window compositing.",
            {{D::Arch,"libxcomposite"},{D::Debian,"libxcomposite1"},{D::Fedora,"libXcomposite"}},
            {{D::Arch,"lib32-libxcomposite"},{D::Debian,"libxcomposite1:i386"},{D::Fedora,"libXcomposite.i686"}});
        Lib("v4l","v4l-utils (libv4l2)", Category::WineLibs, {"libv4l2.so.0"},
            "Video4Linux — webcam/capture support some games probe.",
            {{D::Arch,"v4l-utils"},{D::Debian,"libv4l-0"},{D::Fedora,"libv4l"}},
            {{D::Arch,"lib32-v4l-utils"},{D::Debian,"libv4l-0:i386"},{D::Fedora,"libv4l.i686"}});
        Lib("dbus","D-Bus (libdbus-1)", Category::WineLibs, {"libdbus-1.so.3"},
            "IPC bus used by Wine and many games/launchers.",
            {{D::Arch,"dbus"},{D::Debian,"libdbus-1-3"},{D::Fedora,"dbus-libs"}},
            {{D::Arch,"lib32-dbus"},{D::Debian,"libdbus-1-3:i386"},{D::Fedora,"dbus-libs.i686"}});
        Lib("fontconfig","Fontconfig", Category::WineLibs, {"libfontconfig.so.1"},
            "Font discovery/config — garbled/missing text or crashes when a game enumerates fonts.",
            {{D::Arch,"fontconfig"},{D::Debian,"libfontconfig1"},{D::Fedora,"fontconfig"}},
            {{D::Arch,"lib32-fontconfig"},{D::Debian,"libfontconfig1:i386"},{D::Fedora,"fontconfig.i686"}});
        Lib("xinerama","libXinerama", Category::WineLibs, {"libXinerama.so.1"},
            "Multi-monitor geometry — some games crash going fullscreen without it.",
            {{D::Arch,"libxinerama"},{D::Debian,"libxinerama1"},{D::Fedora,"libXinerama"}},
            {{D::Arch,"lib32-libxinerama"},{D::Debian,"libxinerama1:i386"},{D::Fedora,"libXinerama.i686"}});
        Lib("pcsclite","PC/SC (libpcsclite)", Category::WineLibs, {"libpcsclite.so.1"},
            "Smart-card layer that some DRM / anti-cheat / online games probe on startup.",
            {{D::Arch,"pcsclite"},{D::Debian,"libpcsclite1"},{D::Fedora,"pcsc-lite-libs"}},
            {{D::Arch,"lib32-pcsclite"},{D::Debian,"libpcsclite1:i386"},{D::Fedora,"pcsc-lite-libs.i686"}});

        // ---- Audio ----
        Lib("alsa","ALSA (libasound)", Category::Audio, {"libasound.so.2"},
            "Core Linux audio — no sound / audio-init crashes without it.",
            {{D::Arch,"alsa-lib"},{D::Debian,"libasound2"},{D::Fedora,"alsa-lib"}},
            {{D::Arch,"lib32-alsa-lib"},{D::Debian,"libasound2:i386"},{D::Fedora,"alsa-lib.i686"}});
        Lib("pulse","PulseAudio (libpulse)", Category::Audio, {"libpulse.so.0"},
            "PulseAudio/PipeWire client audio — the common sound path for games.",
            {{D::Arch,"libpulse"},{D::Debian,"libpulse0"},{D::Fedora,"pulseaudio-libs"}},
            {{D::Arch,"lib32-libpulse"},{D::Debian,"libpulse0:i386"},{D::Fedora,"pulseaudio-libs.i686"}});
        Lib("openal","OpenAL (libopenal)", Category::Audio, {"libopenal.so.1"},
            "OpenAL 3D audio — a very common game audio API; without it many games fail to init sound or crash.",
            {{D::Arch,"openal"},{D::Debian,"libopenal1"},{D::Fedora,"openal-soft"}},
            {{D::Arch,"lib32-openal"},{D::Debian,"libopenal1:i386"},{D::Fedora,"openal-soft.i686"}});

        return V;
    }();
    return Deps;
}

} // namespace DepCheck
