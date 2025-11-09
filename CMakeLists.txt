cmake_minimum_required(VERSION 3.5)

project(VidyaGod)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED Widgets)
find_package(nlohmann_json 3.11.3 REQUIRED)

set(TS_FILES VidyaGod_en_US.ts)

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

configure_file(resources/DefaultConfig.JSON DefaultConfig.JSON)
