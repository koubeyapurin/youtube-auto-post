cmake_minimum_required(VERSION 3.20)
project(youtube_auto_post CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── libcurl ───────────────────────────────────────────────────────────────────
set(BUILD_CURL_EXE       OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS   ON  CACHE BOOL "" FORCE)
set(HTTP_ONLY            ON  CACHE BOOL "" FORCE)
if(WIN32)
    set(CURL_USE_SCHANNEL ON  CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL  OFF CACHE BOOL "" FORCE)
else()
    set(CURL_USE_OPENSSL  ON  CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
endif()
set(CURL_ZLIB            OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/releases/download/curl-8_7_1/curl-8.7.1.tar.gz
    URL_HASH SHA256=f91249c87f68ea00cf27c44fdfa5a78423e41e71b7d408e5901a9896d905c495
)
FetchContent_MakeAvailable(curl)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_DIR "${CMAKE_BINARY_DIR}/nlohmann"
)
FetchContent_MakeAvailable(nlohmann_json)
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json_iface INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_iface)
    target_include_directories(nlohmann_json_iface INTERFACE "${CMAKE_BINARY_DIR}")
endif()

# ── Helper macro ──────────────────────────────────────────────────────────────
macro(add_exe TARGET)
    add_executable(${TARGET} ${ARGN})
    target_link_libraries(${TARGET}
        PRIVATE
            CURL::libcurl
            nlohmann_json::nlohmann_json
    )
    find_package(Threads REQUIRED)
    target_link_libraries(${TARGET} PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(${TARGET} PRIVATE ws2_32 crypt32 wldap32)
    endif()
endmacro()

# ── Executables ───────────────────────────────────────────────────────────────
add_exe(test_gemini  src/test_gemini.cpp  src/youtube_uploader.cpp)
