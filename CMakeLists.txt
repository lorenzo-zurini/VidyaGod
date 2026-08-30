cmake_minimum_required(VERSION 3.25)

# Always emit compile_commands.json (and symlink it at the repo root via the rule below). Without it clangd has no
# flags for this project and reports phantom errors on every file — "QObject file not found", "no member named
# 'json' in namespace nlohmann" — which is pure noise that also hides REAL diagnostics.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

project(VidyaGod)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Build hardening (applied to the VidyaGod target below) ----
# Warnings are always on; -Werror is opt-in (CI sets VIDYAGOD_WERROR=ON so local dev isn't blocked).
# VIDYAGOD_SANITIZE adds ASan+UBSan for a Debug "find real bugs" build. Qt/nlohmann come via -isystem so their
# headers don't trip our warnings.
option(VIDYAGOD_WERROR   "Treat C++ warnings as errors (CI)" OFF)
option(VIDYAGOD_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
# -Wshadow=local (not full -Wshadow): high signal (local shadows local/param) without the noisy
# ctor-param-named-like-member idiom. GCC flag (the project builds with g++).
# array-bounds/stringop-overflow stay WARNINGS (not errors): newer GCC (observed on 16) fires known
# false positives through std::shared_ptr's inplace control block + nlohmann at -O2, anchored in
# system headers where -isystem can't suppress them.
set(VG_WARNINGS -Wall -Wextra -Wshadow=local -Wnon-virtual-dtor
    -Wno-error=array-bounds -Wno-error=stringop-overflow)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets Network OpenGL OpenGLWidgets Test)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)   # node-graph validation reads content zips to case-check CONTENTPATH
pkg_check_modules(LIBZSTD REQUIRED IMPORTED_TARGET libzstd) # .vgdelta generation/verification (shared with vidyagodfs)

# Translation files
set(TS_FILES VidyaGod_en_US.ts)

# Host-OS abstraction seam: POSIX impl on Linux, Win32 impl on Windows (selected here, one or the other).
if(WIN32)
    set(VG_PLATFORM_SRC src/platform/platform_win.cpp)
else()
    set(VG_PLATFORM_SRC src/platform/platform_posix.cpp)
endif()

# Everything except the entry point (main.cpp) + translations. Compiled once into an OBJECT library so both the
# VidyaGod executable and the GUI test exe reuse the same objects.
set(SHARED_SOURCES
    src/mainwindow.cpp

    src/apppaths.h
    src/apppaths.cpp

    # OS-abstraction seam (host-specific bits). platform_posix.cpp on Linux; platform_win.cpp on Windows.
    src/platform/platform.h
    ${VG_PLATFORM_SRC}

    src/libraryview.h
    src/libraryview.cpp
    src/mainwindow.h

    src/packageeditor.h
    src/packageeditor.cpp

    src/packageeditormodel.h
    src/packageeditormodel.cpp

    src/nodegraphview.h
    src/nodegraphview.cpp

    src/nodeeditor.h
    src/nodeeditor.cpp

    src/nodesection.h
    src/nodesection.cpp

    src/nodesections.h
    src/nodesections.cpp

    src/validationpanel.h
    src/validationpanel.cpp

    src/jsonraweditor.h
    src/jsonraweditor.cpp

    src/filesystemoperations.h
    src/filesystemoperations.cpp

    src/jsonoperations.h
    src/jsonoperations.cpp

    src/launchparams.h
    src/launchparams.cpp

    # headless CLI mode families (P6 split of the former giant main())
    src/cli/climodes.h
    src/cli/cliipfs.cpp
    src/cli/clicontent.cpp
    src/cli/climaintain.cpp
    src/cli/clilaunch.cpp

    src/varsubst.h
    src/varsubst.cpp

    src/launchresolver.h
    src/launchresolver.cpp
    src/launchresolver_vars.cpp
    src/launchresolver_persist.cpp
    src/launchresolver_exec.cpp
    src/launchresolver_chain.cpp

    src/sandboxlayer.h
    src/sandboxlayer.cpp
    src/sandboxlayer_win.cpp   # Windows Sandboxie backend (Available/Wrap); #ifdef _WIN32, empty on Linux
    src/sandboxinit.cpp

    src/friendstab.h
    src/friendstab.cpp

    src/fileedits.h
    src/fileedits.cpp

    src/registrylayer.h
    src/registrylayer.cpp

    src/persistlayer.h
    src/persistlayer.cpp

    src/vfsmount.h
    src/vfsmount.cpp

    src/launchsources.h
    src/launchsources.cpp

    src/runnerinstall.h
    src/runnerinstall.cpp

    src/procenv.h
    src/procenv.cpp

    src/containerwrapper.h
    src/containerwrapper.cpp

    src/manifestmodel.h
    src/manifestmodel.cpp

    src/packagecatalog.h
    src/packagecatalog.cpp
    src/packagecatalog_publish.cpp

    src/registrywrapper.h
    src/registrywrapper.cpp

    src/commonutils.h
    src/commonutils.cpp

    src/prelaunchwindow.h
    src/prelaunchwindow.cpp

    src/launchthread.h
    src/launchthread.cpp

    src/authoringsession.h
    src/authoringsession.cpp

    src/authoringsessionmodel.h
    src/authoringsessionmodel.cpp

    src/deltatree.h
    src/deltatree.cpp

    src/authoringsessionwindow.h
    src/authoringsessionwindow.cpp

    src/ipfswrapper.h
    src/ipfswrapper.cpp

    src/downloadqueue.h
    src/downloadqueue.cpp

    src/vgipfsapi.h

    src/runnerwrapper.h
    src/runnerwrapper.cpp

    src/covercache.h
    src/covercache.cpp

    src/appmodel.h
    src/appmodel.cpp

    src/downloadmanager.h
    src/downloadmanager.cpp

    src/variantpicker.h
    src/variantpicker.cpp

    src/ipfsmodel.h
    src/ipfsmodel.cpp

    src/ipfstab.h
    src/ipfstab.cpp

    src/librarytab.h
    src/librarytab.cpp

    src/gamepicker.h
    src/gamepicker.cpp

    src/catalogtab.h
    src/catalogtab.cpp

    src/packagesview.h
    src/packagesview.cpp

    src/settingstab.h
    src/settingstab.cpp

    src/runnerspage.h
    src/runnerspage.cpp

    src/sourcespage.h
    src/sourcespage.cpp
    src/a11ynames.h
    src/a11ynames.cpp

    src/ipfssettingspage.h
    src/ipfssettingspage.cpp

    src/generalpage.h
    src/generalpage.cpp

    src/depcheck.h
    src/depcheck.cpp

    src/dependenciespage.h
    src/dependenciespage.cpp

    src/traycontroller.h
    src/traycontroller.cpp

    src/autostart.h
    src/autostart.cpp

    src/pathspage.h
    src/pathspage.cpp
)

# Shared object library (AUTOMOC/UIC/RCC are global, so QObject classes here get MOC'd). Linking the Qt/json/zip
# imported targets propagates their include dirs to the object compilation.
add_library(vidyagod_objs OBJECT ${SHARED_SOURCES})
# vgfs_core (from the VidyaGodFS submodule below) supplies the shared .vgdelta format and the zipscan
# ZIP64 central-directory walker — ONE compilation of each, linked everywhere, instead of the old
# per-target recompiles of vgdelta.cpp with divergent flags.
target_link_libraries(vidyagod_objs PUBLIC
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP PkgConfig::LIBZSTD vgfs_core)
target_include_directories(vidyagod_objs PUBLIC src VidyaGodFS/src)   # src: the cli/ TUs include engine headers bare
target_compile_options(vidyagod_objs PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
if(VIDYAGOD_SANITIZE)
    target_compile_options(vidyagod_objs PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(VidyaGod src/main.cpp src/main.h resources/vidyagod.qrc translations/${TS_FILES})
target_link_libraries(VidyaGod PRIVATE vidyagod_objs)
target_link_libraries(VidyaGod PRIVATE Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets)
target_link_libraries(VidyaGod PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(VidyaGod PRIVATE PkgConfig::LIBZIP)

# Warnings (+ optional -Werror) and optional sanitizers on our own code.
target_compile_options(VidyaGod PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
if(VIDYAGOD_SANITIZE)
    target_compile_options(VidyaGod PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(VidyaGod    PRIVATE -fsanitize=address,undefined)
endif()

# ---- vidyagodfs: the custom FUSE filesystem, built from its own repo (git submodule) ----
# It defines the `vidyagodfs` target (links libfuse3 + libzip) plus the vgfs_core static lib the app
# links for vgdelta/zipscan. Place its binary next to VidyaGod in the build root so the app finds it
# via /proc/self/exe and the AppImage step can bundle it. The parent's hardening options drive the
# submodule's (CMP0077: these pre-sets win over the subproject's option() defaults) so -Werror and
# the sanitizers gate the FUSE code exactly like the app code.
set(VGFS_WERROR   ${VIDYAGOD_WERROR})
set(VGFS_SANITIZE ${VIDYAGOD_SANITIZE})
add_subdirectory(VidyaGodFS)
set_target_properties(vidyagodfs PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- libvgipfs: the embedded Boxo IPFS node, built from its own repo (git submodule) ----
# Builds libvgipfs.so (cgo c-shared, needs the Go toolchain) into the build root next to VidyaGod, replacing the
# external Kubo `ipfs` CLI. It is LINKED + called from src/ipfswrapper.cpp in a later milestone; for now this just
# ensures the node compiles as part of the project. Defines the imported `vgipfs` target.
add_subdirectory(VidyaGodIPFS)

# Link the embedded node in-process. $ORIGIN rpath so the running VidyaGod resolves libvgipfs.so beside itself
# (build root, and AppDir/usr/bin after bundling). Started in main.cpp; the IPFS call sites swap over in M4.
target_link_libraries(VidyaGod PRIVATE vgipfs)
set_target_properties(VidyaGod PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

# ---- Unit tests (ctest) — pure, Qt-free logic (no MOC/Widgets), so the exe stays light ----
enable_testing()
add_executable(vg_tests
    tests/main.cpp
    tests/test_manifestmodel.cpp
    tests/test_varsubst.cpp
    tests/test_registrywrapper.cpp
    tests/test_fileedits.cpp
    tests/test_registrylayer.cpp
    tests/test_depcheck.cpp
    tests/test_vgdelta.cpp
    src/manifestmodel.cpp
    src/varsubst.cpp
    src/registrywrapper.cpp
    src/fileedits.cpp
    src/registrylayer.cpp
    src/persistlayer.cpp
    src/commonutils.cpp
    src/depcheck.cpp        # Qt-free dependency detection engine (ParseLdconfig/Check)
    ${VG_PLATFORM_SRC}      # Platform::SelfExe (used by depcheck) — POSIX or Win32 impl
    src/launchparams.cpp)   # ContainerParams::GetVariablesMap — validator's built-in %variable% source of truth
target_include_directories(vg_tests PRIVATE src VidyaGodFS/src)
target_link_libraries(vg_tests PRIVATE nlohmann_json::nlohmann_json PkgConfig::LIBZIP PkgConfig::LIBZSTD vgfs_core)
target_compile_options(vg_tests PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
if(VIDYAGOD_SANITIZE)
    # The delta/registry/varsubst logic is exercised HERE — the sanitizer must cover the test exe too.
    target_compile_options(vg_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(vg_tests    PRIVATE -fsanitize=address,undefined)
endif()
set_target_properties(vg_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_test(NAME vg_tests COMMAND vg_tests)

# ---- GUI / integration tests (Qt Test) — drive real widgets + models + services in-process, reusing the app
# objects via vidyagod_objs, headless under the offscreen QPA platform. One exe per subsystem (add_gui_test).
function(add_gui_test name)
    add_executable(${name} tests/gui/${name}.cpp)
    target_link_libraries(${name} PRIVATE
        vidyagod_objs
        Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Test
        nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
    target_include_directories(${name} PRIVATE src)
    target_compile_options(${name} PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
    set_target_properties(${name} PROPERTIES BUILD_RPATH "$ORIGIN")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_gui_test(test_packageeditor_gui)
add_gui_test(test_packageeditormodel)
add_gui_test(test_tray)
add_gui_test(test_networking)
add_gui_test(test_launchresolver)
add_gui_test(test_packagecatalog)
add_gui_test(test_appmodel)
add_gui_test(test_ipfsmodel)
add_gui_test(test_tabs)
add_gui_test(test_authoringsession)
add_gui_test(test_deltatree)
add_gui_test(test_friends)
add_gui_test(test_sandbox)
add_gui_test(test_variantpicker)
# The friends test starts the embedded node to exercise the real C ABI; keep it hermetic (no swarm/DHT). The offline
# switch must be in the environment at PROCESS START — Go caches its env at init, so a runtime setenv is invisible.
set_tests_properties(test_friends PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VIDYAGOD_IPFS_OFFLINE=1")

# ---- Offscreen screenshot tool (manual): renders real widgets to PNG via QWidget::grab(). Not a ctest.
add_executable(vg_prelaunch_probe EXCLUDE_FROM_ALL tests/gui/prelaunch_probe.cpp)
target_link_libraries(vg_prelaunch_probe PRIVATE
    vidyagod_objs
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
target_include_directories(vg_prelaunch_probe PRIVATE src)
target_compile_options(vg_prelaunch_probe PRIVATE ${VG_WARNINGS})
set_target_properties(vg_prelaunch_probe PROPERTIES BUILD_RPATH "$ORIGIN")

add_executable(vg_screenshots EXCLUDE_FROM_ALL tests/gui/screenshot_tool.cpp)
target_link_libraries(vg_screenshots PRIVATE
    vidyagod_objs
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
target_include_directories(vg_screenshots PRIVATE src)
target_compile_options(vg_screenshots PRIVATE ${VG_WARNINGS})
set_target_properties(vg_screenshots PROPERTIES BUILD_RPATH "$ORIGIN")
