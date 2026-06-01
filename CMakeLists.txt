cmake_minimum_required(VERSION 3.25)

project(VidyaGod)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets Network)
find_package(nlohmann_json REQUIRED)

# Translation files
set(TS_FILES VidyaGod_en_US.ts)

# Sources
set(PROJECT_SOURCES
    src/main.cpp
    src/main.h

    src/mainwindow.cpp
    src/mainwindow.h

    src/packageeditor.h
    src/packageeditor.cpp

    src/filesystemoperations.h
    src/filesystemoperations.cpp

    src/jsonoperations.h
    src/jsonoperations.cpp

    src/containerwrapper.h
    src/containerwrapper.cpp

    src/commonutils.h
    src/commonutils.cpp

    src/prelaunchwindow.h
    src/prelaunchwindow.cpp

    translations/${TS_FILES}
)

add_executable(VidyaGod ${PROJECT_SOURCES})

target_link_libraries(VidyaGod PRIVATE Qt6::Widgets Qt6::Network)
target_link_libraries(VidyaGod PRIVATE nlohmann_json::nlohmann_json)
