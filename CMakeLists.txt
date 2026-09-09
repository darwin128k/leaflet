cmake_minimum_required(VERSION 3.16)
project(leaflet LANGUAGES C CXX)

option(LEAFLET_LVGL "Fetch LVGL and draw the prefetch banner (click-through layered HWND)" ON)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CMAKE_IGNORE_PREFIX_PATH "C:/msys64/mingw64;C:/msys64")
FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(curl)

set(LEAFLET_SOURCES
    src/main.cpp
    src/layout.cpp
    src/log.cpp
    src/bgswitch.cpp
    src/roundframe.cpp
    src/audioextra.cpp
    src/scheme.cpp
    src/prefetch.cpp
    src/overlay.cpp
    src/vgui_bridge.cpp
    src/ui_api.cpp
    src/ui_layout.cpp
    src/ui_caps.cpp
)

# MetaHook RapidJSON (header-only) — themes / layout manifests later.
set(LEAFLET_RAPIDJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../metahook/thirdparty/rapidjson/include")
if (NOT EXISTS "${LEAFLET_RAPIDJSON_DIR}/rapidjson/document.h")
    set(LEAFLET_RAPIDJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../metaaudio/externals/MetaHookSv/thirdparty/rapidjson/include")
endif()

if (LEAFLET_LVGL)
    set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h" CACHE PATH "" FORCE)
    set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/lv_conf.h" CACHE STRING "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
    set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(lvgl
        URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.2.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(lvgl)
endif()

add_library(leaflet SHARED ${LEAFLET_SOURCES})

target_include_directories(leaflet PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${LEAFLET_RAPIDJSON_DIR}"
)
target_link_libraries(leaflet PRIVATE user32 advapi32 gdi32 ws2_32 CURL::libcurl)
target_compile_definitions(leaflet PRIVATE _CRT_SECURE_NO_WARNINGS LEAFLET_BUILD)
if (MSVC)
    target_compile_options(leaflet PRIVATE /utf-8)
endif()

if (LEAFLET_LVGL)
    target_compile_definitions(leaflet PRIVATE LEAFLET_LVGL=1)
    target_link_libraries(leaflet PRIVATE lvgl)
    if (TARGET lvgl)
        target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
        target_include_directories(lvgl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    endif()
endif()

set_target_properties(leaflet PROPERTIES OUTPUT_NAME leaflet)
