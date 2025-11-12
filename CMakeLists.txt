cmake_minimum_required(VERSION 3.25)

project(VidyaGod)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(nlohmann_json REQUIRED)

# Translation files
set(TS_FILES VidyaGod_en_US.ts)

# Sources
set(PROJECT_SOURCES
    src/main.cpp

    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui

    src/packageeditor.h
    src/packageeditor.cpp
    src/packageeditor.ui

    src/filesystemoperations.h
    src/filesystemoperations.cpp

    src/jsonoperations.h
    src/jsonoperations.cpp

    src/runner.h
    src/runner.cpp

    src/AppRun

    resources/DefaultConfig.JSON

    translations/${TS_FILES}
)

add_executable(VidyaGod ${PROJECT_SOURCES})

target_link_libraries(VidyaGod PRIVATE Qt6::Widgets)
target_link_libraries(VidyaGod PRIVATE nlohmann_json::nlohmann_json)

# Copy the default config to build directory
configure_file(resources/DefaultConfig.JSON DefaultConfig.JSON COPYONLY)

# ---------------------------
# AppDir setup
# ---------------------------
set(APPDIR ${CMAKE_BINARY_DIR}/AppDir)
set(APPDIR_USR ${APPDIR}/usr)
set(APPDIR_BIN ${APPDIR_USR}/bin)
set(APPDIR_RES ${APPDIR_USR}/resources)
set(APPDIR_TRANS ${APPDIR_USR}/translations)
set(APPDIR_PLUGINS ${APPDIR_USR}/plugins)
set(APPDIR_LIB ${APPDIR_USR}/lib)

# Create AppDir directories
file(MAKE_DIRECTORY ${APPDIR_BIN} ${APPDIR_RES} ${APPDIR_TRANS} ${APPDIR_LIB} ${APPDIR_PLUGINS})

# ---------------------------
# Post-build commands
# ---------------------------
add_custom_command(TARGET VidyaGod POST_BUILD
    # Copy executable
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:VidyaGod> ${APPDIR_BIN}/

    # Copy resources and translations
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/resources ${APPDIR_RES}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/translations ${APPDIR_TRANS}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/resources/VidyaGod.desktop ${APPDIR}/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/resources/VidyaGod.png ${APPDIR}/

    # Copy default.json
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/resources/DefaultConfig.JSON ${APPDIR_BIN}


    # Copy AppRun into AppDir root
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/AppRun ${CMAKE_BINARY_DIR}/AppDir/AppRun

    # Bundle nlohmann_json headers
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/json/include/nlohmann
            ${APPDIR_USR}/include/nlohmann
)

# ---------------------------
# Bundle required Qt libraries and plugins
# ---------------------------
find_program(LDD_EXECUTABLE ldd REQUIRED)

if(LDD_EXECUTABLE)
    add_custom_command(TARGET VidyaGod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Copying Qt libraries..."
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APPDIR_LIB}

        # Copy the minimal set of Qt6 libraries (adjust as needed)
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6Core.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6Gui.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6Widgets.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6DBus.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6Network.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6Concurrent.so.6 ${APPDIR_LIB}/
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/libQt6OpenGL.so.6 ${APPDIR_LIB}/

        # Copy minimal Qt plugins needed
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APPDIR_PLUGINS}/platforms
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/qt6/plugins/platforms/libqxcb.so ${APPDIR_PLUGINS}/platforms/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APPDIR_PLUGINS}/imageformats
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/qt6/plugins/imageformats/libqjpeg.so ${APPDIR_PLUGINS}/imageformats/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APPDIR_PLUGINS}/sqldrivers
        COMMAND ${CMAKE_COMMAND} -E copy /usr/lib/qt6/plugins/sqldrivers/libqsqlite.so ${APPDIR_PLUGINS}/sqldrivers/

        # Fix RPATH so executable finds libraries inside AppImage
        COMMAND patchelf --set-rpath "\$ORIGIN/../lib" ${APPDIR_BIN}/VidyaGod
    )
endif()

# ---------------------------
# AppImage
# ---------------------------
find_program(APPIMAGETOOL_EXECUTABLE appimagetool)
if(APPIMAGETOOL_EXECUTABLE)
    add_custom_command(TARGET VidyaGod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Creating AppImage..."
        COMMAND ${CMAKE_COMMAND} -E env ARCH=x86_64 ${APPIMAGETOOL_EXECUTABLE} ${APPDIR}
        COMMENT "Building AppImage"
    )
else()
    message(WARNING "appimagetool not found. AppImage will not be created automatically.")
endif()
