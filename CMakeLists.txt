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

    resources/DefaultConfig.JSON
    translations/${TS_FILES}
)

add_executable(VidyaGod ${PROJECT_SOURCES})

target_link_libraries(VidyaGod PRIVATE Qt6::Widgets)
target_link_libraries(VidyaGod PRIVATE nlohmann_json::nlohmann_json)

# Copy the default config to build directory
configure_file(resources/DefaultConfig.JSON DefaultConfig.JSON COPYONLY)
