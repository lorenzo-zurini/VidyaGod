cmake_minimum_required(VERSION 3.25)

# Always emit compile_commands.json (and symlink it at the repo root via the rule below). Without it clangd has no
# flags for this project and reports phantom errors on every file — "QObject file not found", "no member named
# 'json' in namespace nlohmann" — which is pure noise that also hides REAL diagnostics.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

project(VidyaGod)

# `VidyaGod version`: the commit it was configured from (re-run cmake to refresh it).
execute_process(COMMAND git describe --always --dirty --tags WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE VIDYAGOD_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT VIDYAGOD_GIT_VERSION)
    set(VIDYAGOD_GIT_VERSION "dev")
endif()

# An EMPTY build type means gcc gets NO -O flag at all, and that is exactly what both dev machines were running:
# the entire app at -O0, silently, for months. Measured cost on the real catalog (2862 nodes): a launch resolve
# 1400ms -> 190ms, --validate-nodes 49.6s -> 14.8s just from turning the optimizer on. CI always passes an explicit
# -DCMAKE_BUILD_TYPE=Release so releases were never affected — which is precisely why nobody noticed locally.
# RelWithDebInfo (-O2 -g) keeps perf/gdb usable; ask for Debug explicitly when you want -O0.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type (empty would mean -O0)" FORCE)
    message(STATUS "VidyaGod: CMAKE_BUILD_TYPE was empty -> defaulting to RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The g++ >= 12 floor (std::atomic<std::shared_ptr>) is enforced in VidyaGodFS/CMakeLists.txt, beside overlay.h
# which actually needs it — the add_subdirectory(VidyaGodFS) below inherits it, and a standalone FS build gets it too.

# ---- Build hardening (applied to the VidyaGod target below) ----
# Warnings are always on; -Werror is opt-in (CI sets VIDYAGOD_WERROR=ON so local dev isn't blocked).
# VIDYAGOD_SANITIZE adds ASan+UBSan for a Debug "find real bugs" build. Qt/nlohmann come via -isystem so their
# headers don't trip our warnings.
option(VIDYAGOD_WERROR   "Treat C++ warnings as errors (CI)" OFF)
option(VIDYAGOD_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
# -Wshadow=local (not full -Wshadow): high signal (local shadows local/param) without the noisy
# ctor-param-named-like-member idiom. GCC flag (the project builds with g++).
# array-bounds/stringop-overflow/maybe-uninitialized stay WARNINGS (not errors). These are GCC's speculative,
# optimizer-driven diagnostics — GCC fires all three as false positives through system headers where -isystem
# can't suppress them (the analysis runs post-inline and gets attributed to our TU). Only the -Werror is dropped
# for these three specific names; every other warning (including the reliable definite-use -Wuninitialized, which
# is a SEPARATE diagnostic from the here-demoted speculative -Wmaybe-uninitialized) still fails the build:
#   * array-bounds/stringop-overflow: std::shared_ptr's inplace control block + nlohmann at -O2 (seen on gcc-16)
#   * maybe-uninitialized: nlohmann's iter_impl.hpp (.items()) and std::function/<regex> internals, fired by
#     gcc-12 — and MORE of them appear only under the ASan/UBSan sanitizer codegen, across many unrelated TUs
#     and in the VidyaGodFS subtree, so per-TU scoping is untenable whack-a-mole.
# The reliable -Wuninitialized (DEFINITE uses) stays an error under -Wall. Honest caveat: demoting the
# speculative -Wmaybe- leaves PATH-DEPENDENT uninitialized reads with no hard gate, and nothing here catches
# them at runtime either (that needs MemorySanitizer, which is clang-only — we build gcc, and ASan/UBSan do NOT
# flag uninitialized reads). Accepted as the lesser evil vs. an unsuppressable flood of system-header false
# positives; revisit with a valgrind-memcheck job if a real one ever slips through.
set(VG_WARNINGS -Wall -Wextra -Wshadow=local -Wnon-virtual-dtor
    -Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=maybe-uninitialized)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets Network OpenGL OpenGLWidgets Test)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)   # node-graph validation reads content zips to case-check CONTENTPATH
pkg_check_modules(LIBZSTD REQUIRED IMPORTED_TARGET libzstd) # .vgdelta generation/verification (shared with vidyagodfs)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)       # the launch window's press-a-key capture reads gamepad buttons
# XCB is Linux-only: it backs sandbox-init's X-side window cloak (src/engine/sandboxinit.cpp, itself #ifdef __linux__).
# There is no xcb on Windows/MinGW, so requiring it unconditionally broke the Windows configure — guard it.
if(NOT WIN32)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)     # sandbox-init's X-side window cloak (VIDYAGOD_CLOAK)
endif()

# Translation files
set(TS_FILES VidyaGod_en_US.ts)

# Host-OS abstraction seam: POSIX impl on Linux, Win32 impl on Windows (selected here, one or the other).
if(WIN32)
    set(VG_PLATFORM_SRC src/base/platform/platform_win.cpp)
else()
    set(VG_PLATFORM_SRC src/base/platform/platform_posix.cpp)
endif()

# The source tree is one folder per layer, lowest first: each may include only its own and the ones before it
# (tools/layers/check_layers.py holds every #include to that; ctest: layers). Every folder is on the include path, so
# a header is named bare ("packagecatalog.h"); src itself too, for the few named by their folder ("cli/cliapp.h").
set(VG_LAYERS base format net plan library engine app editor ui cli)
set(VG_SRC_DIRS ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/cli)
foreach(L ${VG_LAYERS})
    list(APPEND VG_SRC_DIRS ${CMAKE_SOURCE_DIR}/src/${L})
endforeach()

# Dear ImGui (1.92.9, vendored) — the package editor's canvas; its fonts are embedded from third_party/fonts.
# AUTOMOC off: third-party C++ with no Qt in it.
add_library(vg_imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
    src/editor/imguiprobe.cpp)
set_target_properties(vg_imgui PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
# ImGui's test-engine hooks, implemented by src/editor/imguiprobe.cpp: with VG_UI_PROBE set, the editor's canvas writes the
# items each frame lays out (labels, rectangles) so a driver can click them by name with real input.
target_compile_definitions(vg_imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
target_include_directories(vg_imgui PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/third_party/imgui/misc/cpp)
target_link_libraries(vg_imgui PUBLIC Qt6::OpenGL)

# Everything except the entry point (main.cpp) + translations. Compiled once into an OBJECT library so both the
# VidyaGod executable and the GUI test exe reuse the same objects.
set(SHARED_SOURCES
    src/ui/mainwindow.cpp

    src/base/apppaths.h
    src/base/sessions.h
    src/base/sessions.cpp
    src/base/apppaths.cpp
    src/plan/instancestore.h
    src/plan/instancestore.cpp

    # OS-abstraction seam (host-specific bits). platform_posix.cpp on Linux; platform_win.cpp on Windows.
    src/base/platform/platform.h
    src/base/platform/platform_fs.cpp
    src/base/platform/platform_screen.cpp
    ${VG_PLATFORM_SRC}

    src/ui/libraryview.h
    src/ui/libraryview.cpp
    src/ui/mainwindow.h

    src/editor/packageeditor.h
    src/editor/packageeditor.cpp

    src/editor/packageeditormodel.h
    src/editor/packageeditormodel.cpp





    src/editor/validationpanel.h
    src/editor/validationpanel.cpp

    src/editor/jsonraweditor.h
    src/editor/jsonraweditor.cpp
    src/editor/pkginsight.cpp

    src/format/filesystemoperations.h
    src/format/filesystemoperations.cpp

    src/base/jsonoperations.h
    src/base/jsonoperations.cpp

    src/plan/launchparams.h
    src/plan/launchparams.cpp

    # headless CLI mode families (P6 split of the former giant main())
    src/cli/climodes.h
    src/cli/cliipfs.cpp
    src/cli/clicontent.cpp
    src/cli/climaintain.cpp
    src/cli/cliaudit.cpp
    src/cli/clilaunch.cpp

    # the CLI app: every function and every view of the program by name (standing rule — see src/cli/command.h)
    src/cli/command.h
    src/cli/command.cpp
    src/cli/commandserver.h
    src/cli/commandserver.cpp
    src/cli/cliapp.h
    src/cli/cliapp.cpp
    src/cli/commands/cmd_friends.cpp
    src/cli/commands/cmd_catalog.cpp
    src/cli/commands/cmd_ui.cpp
    src/cli/commands/cmd_events.cpp
    src/cli/commands/cmd_packages.cpp
    src/cli/commands/cmd_engine.cpp
    src/cli/commands/cmd_app.cpp
    src/cli/commands/cmd_ipfs.cpp
    src/cli/commands/cmd_build.cpp
    src/cli/commands/cmd_editor.cpp
    src/cli/commands/cmd_author.cpp
    src/cli/commands/cmd_vfs.cpp
    src/cli/commands/cmd_launch.cpp
    src/cli/commands/cmd_appui.cpp
    src/cli/commands/cmdutil.h
    src/cli/commands/cmdutil.cpp
    src/base/eventlog.h
    src/base/eventlog.cpp

    src/format/varsubst.h
    src/format/varsubst.cpp
    src/base/keynames.cpp

    src/plan/launchresolver.h
    src/plan/launchresolver.cpp
    src/plan/launchresolver_vars.cpp
    src/plan/launchresolver_persist.cpp
    src/plan/launchresolver_exec.cpp
    src/plan/launchresolver_chain.cpp

    src/engine/sandboxlayer.h
    src/engine/sandboxlayer.cpp
    src/engine/boxlaunch.h
    src/engine/boxlaunch.cpp      # Windows: Prepare (fill the box, import the registry) and Harvest (keeps back out)
    src/engine/boxprefix.h
    src/engine/boxprefix.cpp      # Windows: the Sandboxie box as the game's prefix (plain files + .reg text, tested anywhere)
    src/engine/sandboxlayer_win.cpp   # Windows Sandboxie backend (Available/Wrap); #ifdef _WIN32, empty on Linux
    src/engine/sandboxinit.cpp

    src/ui/networktab.h
    src/ui/networktab.cpp

    src/engine/fileedits.h
    src/engine/fileedits.cpp

    src/engine/binarypatch.h
    src/engine/binarypatch.cpp

    src/engine/registrylayer.h
    src/engine/registrylayer.cpp

    src/engine/persistlayer.h
    src/engine/persistlayer.cpp

    src/engine/vfsmount.h
    src/engine/vfsmount.cpp

    src/plan/launchsources.h
    src/plan/launchsources.cpp


    src/base/procenv.h
    src/base/procenv.cpp

    src/engine/containerwrapper.h
    src/engine/containerwrapper.cpp

    src/engine/quitsignals.h
    src/engine/quitsignals.cpp

    src/engine/optionreadback.h
    src/engine/launchsession.h
    src/engine/launchsession.cpp
    src/engine/optionreadback.cpp

    src/format/manifestmodel.h
    src/plan/validate.h
    src/plan/shelf.h
    src/plan/shelf.cpp
    src/plan/installed.h
    src/plan/installed.cpp
    src/plan/validate.cpp
    src/format/manifestmodel.cpp
    src/format/fold.cpp
    src/base/cid.cpp

    src/format/nodegraph.h
    src/format/noderefs.h
    src/format/noderefs.cpp
    src/library/nodestore.h
    src/library/nodestore.cpp
    src/format/nodegraph.cpp

    src/format/nodelower.h
    src/format/nodelower.cpp
    src/format/contentfile.h
    src/format/contentfile.cpp

    src/editor/pkggraph.h
    src/editor/pkggraph.cpp
    src/library/pkgdoc.cpp
    src/editor/pkgform.h
    src/editor/pkgform.cpp
    src/editor/pkglayout.h
    src/editor/pkglayout.cpp
    src/editor/pkgcanvas.h
    src/editor/pkgcanvas.cpp
    src/editor/pkgactions.h
    src/editor/pkgactions.cpp
    src/editor/pkgcanvaspanel.h
    src/editor/pkgcanvaspanel.cpp

    src/library/packagecatalog.h
    src/library/catalog_layout.cpp
    src/library/catalog_sources.cpp
    src/library/catalog_library.cpp
    src/library/catalog_received.cpp
    src/library/catalog_versions.cpp
    src/library/catalog_index.cpp
    src/library/catalog_content.cpp
    src/plan/builtinrunners.cpp
    src/library/catalog_publish.cpp

    src/format/registrywrapper.h
    src/format/registrywrapper.cpp

    src/base/commonutils.h
    src/base/commonutils.cpp

    src/ui/prelaunchwindow.h
    src/ui/prelaunchwindow.cpp
    src/ui/keycapture.h
    src/ui/keycapture.cpp

    src/ui/launchthread.h
    src/ui/launchthread.cpp

    src/engine/authoringsession.h
    src/engine/authoringsession.cpp

    src/editor/authoringsessionmodel.h
    src/editor/authoringsessionmodel.cpp

    src/editor/deltatree.h
    src/editor/deltatree.cpp

    src/editor/authoringsessionwindow.h
    src/editor/authoringsessionwindow.cpp

    src/net/ipfswrapper.h
    src/net/ipfswrapper.cpp

    src/net/downloadqueue.h
    src/net/downloadqueue.cpp

    src/net/vgipfsapi.h

    src/plan/runnerwrapper.h
    src/plan/runnerwrapper.cpp

    src/library/covercache.h
    src/library/covercache.cpp

    src/app/appmodel.h
    src/app/appmodel.cpp
    src/app/friendsharing.cpp

    src/app/downloadmanager.h
    src/app/downloadmanager.cpp
    src/ui/downloaddialog.cpp

    src/ui/variantpicker.h
    src/ui/variantpicker.cpp

    src/app/ipfsmodel.h
    src/app/ipfsmodel.cpp

    src/ui/ipfstab.h
    src/ui/ipfstab.cpp

    src/ui/librarytab.h
    src/ui/librarytab.cpp

    src/ui/gamepicker.h
    src/ui/gamepicker.cpp

    src/ui/catalogtab.h
    src/ui/catalogtab.cpp

    src/ui/packagesview.h
    src/ui/packagesview.cpp

    src/ui/settingstab.h
    src/ui/settingstab.cpp
    src/ui/theme.h
    src/ui/theme.cpp
    src/ui/networkpage.cpp

    src/ui/runnerspage.h
    src/ui/runnerspage.cpp

    src/ui/sourcespage.h
    src/ui/sourcespage.cpp
    src/ui/a11ynames.h
    src/ui/a11ynames.cpp

    src/ui/ipfssettingspage.h
    src/ui/ipfssettingspage.cpp

    src/ui/generalpage.h
    src/ui/generalpage.cpp

    src/engine/depcheck.h
    src/engine/depcheck.cpp

    src/ui/dependenciespage.h
    src/ui/dependenciespage.cpp

    src/ui/traycontroller.h
    src/ui/traycontroller.cpp

    src/app/autostart.h
    src/app/autostart.cpp

    src/ui/pathspage.h
    src/ui/pathspage.cpp
)

# Shared object library (AUTOMOC/UIC/RCC are global, so QObject classes here get MOC'd). Linking the Qt/json/zip
# imported targets propagates their include dirs to the object compilation.
add_library(vidyagod_objs OBJECT ${SHARED_SOURCES})
# vgfs_core (from the VidyaGodFS submodule below) supplies the shared .vgdelta format and the zipscan
# ZIP64 central-directory walker — ONE compilation of each, linked everywhere, instead of the old
# per-target recompiles of vgdelta.cpp with divergent flags.
# PkgConfig::XCB exists only on Linux (guarded above); it must not be referenced on Windows.
if(NOT WIN32)
    set(VG_XCB_LIB PkgConfig::XCB)
endif()
target_link_libraries(vidyagod_objs PUBLIC
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP PkgConfig::LIBZSTD PkgConfig::SDL2 ${VG_XCB_LIB} vgfs_core vg_imgui)
if(WIN32)
    target_link_libraries(vidyagod_objs PUBLIC shell32 ole32)   # Platform::HostFolders (SHGetKnownFolderPath)
endif()
target_include_directories(vidyagod_objs PUBLIC ${VG_SRC_DIRS} VidyaGodFS/src)
target_compile_options(vidyagod_objs PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
target_compile_definitions(vidyagod_objs PRIVATE VIDYAGOD_VERSION="${VIDYAGOD_GIT_VERSION}")
if(VIDYAGOD_SANITIZE)
    target_compile_options(vidyagod_objs PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(VidyaGod src/cli/main.cpp src/cli/main.h resources/vidyagod.qrc translations/${TS_FILES})
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
# external Kubo `ipfs` CLI. It is LINKED + called from src/net/ipfswrapper.cpp in a later milestone; for now this just
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
    tests/test_nodegraph.cpp
    tests/test_nodelower.cpp
    tests/test_varsubst.cpp
    tests/test_registrywrapper.cpp
    tests/test_boxprefix.cpp
    tests/test_fileedits.cpp
    tests/test_binarypatch.cpp
    tests/test_registrylayer.cpp
    tests/test_persistlayer.cpp
    tests/test_depcheck.cpp
    tests/test_pkglayout.cpp
    tests/test_vgdelta.cpp
    tests/test_pkgdoc.cpp
    tests/test_optionreadback.cpp
    tests/test_parallel.cpp
    src/format/manifestmodel.cpp
    src/format/fold.cpp
    src/base/cid.cpp
    src/format/nodegraph.cpp   # the gigagraph's PURE transforms (freeze/topo/gather/landing checks) — no IPFS/Qt
    src/format/nodelower.cpp
    src/format/contentfile.cpp
    src/format/varsubst.cpp
    src/base/keynames.cpp
    src/format/registrywrapper.cpp
    src/engine/boxprefix.cpp
    src/engine/fileedits.cpp
    src/engine/binarypatch.cpp
    src/engine/registrylayer.cpp
    src/engine/persistlayer.cpp
    src/engine/optionreadback.cpp
    src/editor/pkggraph.cpp
    src/editor/pkglayout.cpp
    src/library/pkgdoc.cpp
    src/format/noderefs.cpp
    src/base/commonutils.cpp
    src/engine/depcheck.cpp        # Qt-free dependency detection engine (ParseLdconfig/Check)
    ${VG_PLATFORM_SRC}      # Platform::SelfExe (used by depcheck) — POSIX or Win32 impl
    src/base/platform/platform_fs.cpp   # Platform::IsLink / RemoveTree (boxprefix)
    src/plan/validate.cpp
    src/plan/launchparams.cpp)   # ContainerParams::GetVariablesMap — validator's built-in %variable% source of truth
target_include_directories(vg_tests PRIVATE ${VG_SRC_DIRS} tests VidyaGodFS/src)
if(WIN32)
    target_link_libraries(vg_tests PRIVATE shell32 ole32)
endif()
target_link_libraries(vg_tests PRIVATE nlohmann_json::nlohmann_json PkgConfig::LIBZIP PkgConfig::LIBZSTD vgfs_core)
target_compile_options(vg_tests PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
if(VIDYAGOD_SANITIZE)
    # The delta/registry/varsubst logic is exercised HERE — the sanitizer must cover the test exe too.
    target_compile_options(vg_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(vg_tests    PRIVATE -fsanitize=address,undefined)
endif()
set_target_properties(vg_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_test(NAME vg_tests COMMAND vg_tests)
# The layering (VG_LAYERS): every #include reaches its own layer or one before it.
add_test(NAME layers COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/layers/check_layers.py --src ${CMAKE_SOURCE_DIR}/src)
set_tests_properties(vg_tests PROPERTIES ENVIRONMENT "HOME=${CMAKE_BINARY_DIR}/testhome;TMPDIR=${CMAKE_BINARY_DIR}/testtmp")

# ---- GUI / integration tests (Qt Test) — drive real widgets + models + services in-process, reusing the app
# objects via vidyagod_objs, headless under the offscreen QPA platform. One exe per subsystem (add_gui_test).
function(add_gui_test name)
    add_executable(${name} tests/gui/${name}.cpp)
    target_link_libraries(${name} PRIVATE
        vidyagod_objs
        Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Test
        nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
    target_include_directories(${name} PRIVATE ${VG_SRC_DIRS} tests)   # tests/ for nodefixture.h
    target_compile_options(${name} PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
    set_target_properties(${name} PROPERTIES BUILD_RPATH "$ORIGIN")
    add_test(NAME ${name} COMMAND ${name})
    #HOME and TMPDIR are the test's own: a test that forgets to point a root at its temp dir lands in build/testhome,
    #never in the user's ~/.VidyaGod (AppPaths::DataRoot() defaults to $HOME/.VidyaGod), nor the app's session root.
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;HOME=${CMAKE_BINARY_DIR}/testhome;TMPDIR=${CMAKE_BINARY_DIR}/testtmp")
endfunction()
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/testhome ${CMAKE_BINARY_DIR}/testtmp)

# The launch-matrix fixture needs REAL deltas (a stub proves the plan, never the reconstruction), so it gets a
# tiny generator built against the same vgdelta the mount uses. POSIX only: the harness mounts and launches.
if(NOT WIN32)
    add_executable(vg_make_delta tests/fixtures/launchmatrix/make_delta.cpp)
    target_include_directories(vg_make_delta PRIVATE VidyaGodFS/src)
    target_link_libraries(vg_make_delta PRIVATE vgfs_core PkgConfig::LIBZIP PkgConfig::LIBZSTD)
endif()

# vg_fold — the generation-6 resolver in batch, gated plan-for-plan against tools/gen6/resolve.py (fold_gate.py).
add_executable(vg_fold tools/gen6/vg_fold.cpp src/format/fold.cpp src/base/cid.cpp src/format/varsubst.cpp src/base/keynames.cpp src/base/commonutils.cpp)
target_include_directories(vg_fold PRIVATE ${VG_SRC_DIRS})
target_link_libraries(vg_fold PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(vg_fold PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
set_target_properties(vg_fold PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
# The rule fixtures through both resolvers: the C++ fold must equal the reference on every rule the real library
# does not exercise yet (diamonds, moves, WHEN phases, TAKE, ANY/NOT, grafts on grafts).
add_test(NAME fold_fixtures COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen6/fold_gate.py --fixtures --vg-fold $<TARGET_FILE:vg_fold>)

# ---- Launch matrix — a synthetic package exercising every launch-engine feature, diffed against golden plans.
# Not a unit test: it runs the built binary against a generated fixture in a throwaway data dir and compares the
# resolved plan for each launchable. Touch the launch engine and this says which plans moved.
# Bound to the binaries of THIS build tree, not to a hardcoded build/VidyaGod: a second tree
# (build-asan, a Release dir) would otherwise run the matrix against a stale binary from another build and pass
# — the same stale-binary trap that makes a mutation look blind, wired into the test itself.
if(NOT WIN32)
    add_test(NAME launch_matrix COMMAND ${CMAKE_COMMAND} -E env python3
             ${CMAKE_SOURCE_DIR}/tools/launch_matrix.py
             --binary $<TARGET_FILE:VidyaGod>
             --make-delta $<TARGET_FILE:vg_make_delta>)
    #Above the harness's OWN budget (300 s per plan + 600 s per runtime node), so a hang is reported by the
    #harness — with its exit codes and golden diff — instead of ctest killing it and printing a bare Timeout.
    set_tests_properties(launch_matrix PROPERTIES TIMEOUT 2400)
endif()

# --variant names a row of a tile: alone it is refused (exit 2, a message) — not a GUI that silently launches nothing.
# A throwaway data dir so the run never touches the developer's config or instance lock.
add_test(NAME cli_variant_needs_a_tile
         COMMAND ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
                 $<TARGET_FILE:VidyaGod> --data-dir ${CMAKE_BINARY_DIR}/cli_variant_test_data --offline --variant x)
set_tests_properties(cli_variant_needs_a_tile PROPERTIES PASS_REGULAR_EXPRESSION "needs --tile" TIMEOUT 30)

# Every example in the command-line help names a command and parses against it (a stale option, a missing argument).
add_test(NAME cli_examples COMMAND $<TARGET_FILE:VidyaGod> --no-color commands --check)
# Every example of every command run against a fixture library — headless, then through an off-screen app on it: no
# crash, no hang, no raw library error reaching the user (tools/cli_sweep). HOME and the data dir are the test's own.
# POSIX only (the scripts drive process groups and the fixture's own HOME); the Windows suite runs the rest.
if(NOT WIN32)
add_test(NAME cli_sweep COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/cli_sweep/cli_sweep.py --vg $<TARGET_FILE:VidyaGod>
         --work ${CMAKE_BINARY_DIR}/cli_sweep --live --report ${CMAKE_BINARY_DIR}/cli_sweep_report.json)
set_tests_properties(cli_sweep PROPERTIES TIMEOUT 1200 RUN_SERIAL TRUE)
# Start-up, call latency and idle memory against budgets, on the fixture — warn-only for now: it prints, never fails
# (tools/perf/budgets.py; --data-dir runs it on a copy of a real library).
add_test(NAME perf_budgets COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/perf/budgets.py --vg $<TARGET_FILE:VidyaGod>
         --work ${CMAKE_BINARY_DIR}/perf_budgets)
set_tests_properties(perf_budgets PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endif()
# `cmake --build build --target screenshots`: every tab, Settings page and window shot off-screen into
# build/screenshots/<date-time>/, with build/screenshots/index.html beside the previous run (tools/screenshots).
add_custom_target(screenshots COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/screenshots/shoot.py --vg $<TARGET_FILE:VidyaGod>
                  --out ${CMAKE_BINARY_DIR}/screenshots DEPENDS VidyaGod USES_TERMINAL)
set_tests_properties(cli_examples PROPERTIES TIMEOUT 30)

add_gui_test(test_pkgcanvas)
add_gui_test(test_canvasperf)
set_tests_properties(test_canvasperf PROPERTIES RUN_SERIAL TRUE)   # it times frames: alone, or it measures the other suites
add_gui_test(test_pkgactions)
add_gui_test(test_packageeditormodel)
add_gui_test(test_tray)
add_gui_test(test_networking)
add_gui_test(test_launchresolver)
add_gui_test(test_packagecatalog)
add_gui_test(test_instancestore)
add_gui_test(test_appmodel)
add_gui_test(test_ipfsmodel)
add_gui_test(test_covercache)
add_gui_test(test_downloadqueue)
add_gui_test(test_downloadmanager)
add_gui_test(test_tabs)
add_gui_test(test_authoringsession)
add_gui_test(test_deltatree)
add_gui_test(test_diagnostics)
add_gui_test(test_launchsession)
add_gui_test(test_asyncwork)
add_gui_test(test_fileedits)
add_gui_test(test_manifestmodel)

# The standing rule: every function of every module is drivable from the CLI. A function with no command (and no
# reasoned exclusion) that is not already listed as debt fails the build's tests; paid debt must leave the baseline.
add_test(NAME cli_coverage COMMAND python3 -I ${CMAKE_SOURCE_DIR}/tools/cli_coverage/cli_coverage.py
         --binary $<TARGET_FILE:VidyaGod> --build ${CMAKE_BINARY_DIR})
add_gui_test(test_vfsmount)
add_gui_test(test_friends)
add_gui_test(test_sandbox)
add_gui_test(test_variantpicker)
add_gui_test(test_prelaunch)
add_gui_test(test_commands)
# The friends test starts the embedded node to exercise the real C ABI; keep it hermetic (no swarm/DHT). The offline
# switch must be in the environment at PROCESS START — Go caches its env at init, so a runtime setenv is invisible.
set_tests_properties(test_friends PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;VIDYAGOD_IPFS_OFFLINE=1;HOME=${CMAKE_BINARY_DIR}/testhome;TMPDIR=${CMAKE_BINARY_DIR}/testtmp")

# ---- Offscreen screenshot tool (manual): renders real widgets to PNG via QWidget::grab(). Not a ctest.
add_executable(vg_prelaunch_probe EXCLUDE_FROM_ALL tests/gui/prelaunch_probe.cpp)
target_link_libraries(vg_prelaunch_probe PRIVATE
    vidyagod_objs
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
target_include_directories(vg_prelaunch_probe PRIVATE ${VG_SRC_DIRS})
target_compile_options(vg_prelaunch_probe PRIVATE ${VG_WARNINGS})
set_target_properties(vg_prelaunch_probe PROPERTIES BUILD_RPATH "$ORIGIN")

add_executable(vg_screenshots EXCLUDE_FROM_ALL tests/gui/screenshot_tool.cpp)
target_link_libraries(vg_screenshots PRIVATE
    vidyagod_objs
    Qt6::Widgets Qt6::Network Qt6::OpenGL Qt6::OpenGLWidgets
    nlohmann_json::nlohmann_json PkgConfig::LIBZIP vgipfs)
target_include_directories(vg_screenshots PRIVATE ${VG_SRC_DIRS} tests)
target_compile_options(vg_screenshots PRIVATE ${VG_WARNINGS})
set_target_properties(vg_screenshots PROPERTIES BUILD_RPATH "$ORIGIN")
