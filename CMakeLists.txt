cmake_minimum_required(VERSION 3.16)
project(leaflet LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(leaflet SHARED
    src/main.cpp
    src/layout.cpp
    src/log.cpp
    src/bgswitch.cpp
    src/roundframe.cpp
)

target_link_libraries(leaflet PRIVATE user32 advapi32)
target_compile_definitions(leaflet PRIVATE _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(leaflet PRIVATE /utf-8)
endif()
set_target_properties(leaflet PROPERTIES OUTPUT_NAME leaflet)
