cmake_minimum_required(VERSION 3.5)

project(VidyaGod)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED Widgets)
find_package(Qt6 REQUIRED LinguistTools)
find_package(Qt6 REQUIRED Sql)
find_package(nlohmann_json 3.11.3 REQUIRED)

set(TS_FILES VidyaGod_en_US.ts)

set(PROJECT_SOURCES
        main.cpp

        mainwindow.cpp
        mainwindow.h
        mainwindow.ui

        packageeditor.h
        packageeditor.cpp
        packageeditor.ui

        filesystemoperations.h
        filesystemoperations.cpp

        jsonoperations.h
        jsonoperations.cpp

        runner.h
        runner.cpp

        DefaultConfig.JSON

        ${TS_FILES}
)



add_executable(VidyaGod ${PROJECT_SOURCES})

target_link_libraries(VidyaGod PRIVATE Qt6::Widgets)
target_link_libraries(VidyaGod PRIVATE Qt6::Sql)
target_link_libraries(VidyaGod PRIVATE nlohmann_json::nlohmann_json)

configure_file(DefaultConfig.JSON DefaultConfig.JSON)
