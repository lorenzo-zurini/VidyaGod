cmake_minimum_required(VERSION 3.25)

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
set(VG_WARNINGS -Wall -Wextra -Wshadow=local -Wnon-virtual-dtor)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets Network OpenGL OpenGLWidgets)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)   # node-graph validation reads content zips to case-check CONTENTPATH

# Translation files
set(TS_FILES VidyaGod_en_US.ts)

# Sources
set(PROJECT_SOURCES
    src/main.cpp
    src/main.h

    src/mainwindow.cpp

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

    src/varsubst.h
    src/varsubst.cpp

    src/launchresolver.h
    src/launchresolver.cpp

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

    src/processenv.h
    src/processenv.cpp

    src/containerwrapper.h
    src/containerwrapper.cpp

    src/manifestmodel.h
    src/manifestmodel.cpp

    src/packagecatalog.h
    src/packagecatalog.cpp

    src/registrywrapper.h
    src/registrywrapper.cpp

    src/commonutils.h
    src/commonutils.cpp

    src/prelaunchwindow.h
    src/prelaunchwindow.cpp

    src/launchthread.h
    src/launchthread.cpp

    src/ipfswrapper.h
    src/ipfswrapper.cpp

    src/vgipfsapi.h

    src/runnerwrapper.h
    src/runnerwrapper.cpp

    src/covercache.h
    src/covercache.cpp

    src/appmodel.h
    src/appmodel.cpp

    src/downloadmanager.h
    src/downloadmanager.cpp

    src/ipfstab.h
    src/ipfstab.cpp

    src/librarytab.h
    src/librarytab.cpp

    src/catalogtab.h
    src/catalogtab.cpp

    src/packagesview.h
    src/packagesview.cpp

    src/settingstab.h
    src/settingstab.cpp

    src/runnerspage.h
    src/runnerspage.cpp

    src/repositoriespage.h
    src/repositoriespage.cpp

    src/downloadspage.h
    src/downloadspage.cpp

    src/pathspage.h
    src/pathspage.cpp

    translations/${TS_FILES}
)

add_executable(VidyaGod ${PROJECT_SOURCES})

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
# It defines the `vidyagodfs` target (links libfuse3 + libzip). Place its binary next to VidyaGod in
# the build root so the app finds it via /proc/self/exe and the AppImage step can bundle it.
add_subdirectory(external/VidyaGodFS)
set_target_properties(vidyagodfs PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- libvgipfs: the embedded Boxo IPFS node, built from its own repo (git submodule) ----
# Builds libvgipfs.so (cgo c-shared, needs the Go toolchain) into the build root next to VidyaGod, replacing the
# external Kubo `ipfs` CLI. It is LINKED + called from src/ipfswrapper.cpp in a later milestone; for now this just
# ensures the node compiles as part of the project. Defines the imported `vgipfs` target.
add_subdirectory(external/VidyaGodIPFS)

# Link the embedded node in-process. $ORIGIN rpath so the running VidyaGod resolves libvgipfs.so beside itself
# (build root, and AppDir/usr/bin after bundling). Started in main.cpp; the IPFS call sites swap over in M4.
target_link_libraries(VidyaGod PRIVATE vgipfs)
set_target_properties(VidyaGod PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

# ---- Unit tests (ctest) — pure, Qt-free logic only, so the test exe stays light (no MOC/Widgets) ----
enable_testing()
add_executable(vg_tests
    tests/main.cpp
    tests/test_manifestmodel.cpp
    tests/test_varsubst.cpp
    src/manifestmodel.cpp
    src/varsubst.cpp
    src/commonutils.cpp)
target_include_directories(vg_tests PRIVATE src)
target_link_libraries(vg_tests PRIVATE nlohmann_json::nlohmann_json PkgConfig::LIBZIP)
target_compile_options(vg_tests PRIVATE ${VG_WARNINGS} $<$<BOOL:${VIDYAGOD_WERROR}>:-Werror>)
set_target_properties(vg_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
add_test(NAME vg_tests COMMAND vg_tests)
