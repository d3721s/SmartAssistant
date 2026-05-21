cmake_minimum_required(VERSION 3.22)

project(SmartVoiceAssistant
    VERSION 0.1.0
    DESCRIPTION "Smart voice assistant audio manager skeleton"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CTest)

set(FETCHCONTENT_BASE_DIR
    "${CMAKE_BINARY_DIR}/Middlewares/fetchcontent"
    CACHE PATH "FetchContent working directory" FORCE
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/Middlewares")

set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TOMLPLUSPLUS_BUILD_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://gitcode.com/gh_mirrors/to/tomlplusplus.git
    GIT_TAG v3.4.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/Middlewares/tomlplusplus"
    BINARY_DIR "${CMAKE_BINARY_DIR}/Middlewares/tomlplusplus"
)

set(QUILL_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    quill
    GIT_REPOSITORY https://gitcode.com/GitHub_Trending/quill4/quill.git
    GIT_TAG v11.1.0
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/Middlewares/quill"
    BINARY_DIR "${CMAKE_BINARY_DIR}/Middlewares/quill"
)

FetchContent_MakeAvailable(tomlplusplus quill)

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing)
pkg_check_modules(SOXR       REQUIRED IMPORTED_TARGET soxr)
pkg_check_modules(UDEV       REQUIRED IMPORTED_TARGET libudev)

add_library(SmartAssistantAudioManager
    SmartAssistant/AudioManager.cpp
)

target_include_directories(SmartAssistantAudioManager
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
    PRIVATE
        ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(SmartAssistantAudioManager
    PUBLIC
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
        ${ALSA_LIBRARIES}
        PkgConfig::WEBRTC_APM
        PkgConfig::SOXR
        PkgConfig::UDEV
)

add_library(SmartAssistantNetworkManager
    SmartAssistant/NetworkManager.cpp
)

target_include_directories(SmartAssistantNetworkManager
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantNetworkManager
    PUBLIC
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
)

if(BUILD_TESTING)
    add_subdirectory(Test)
endif()
