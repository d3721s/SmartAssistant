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

option(SMARTASSISTANT_ENABLE_SHERPA_ONNX
    "Build WakeupManager and ASRManager sherpa-onnx support"
    ON
)

set(SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS OFF CACHE BOOL
    "Build offline sherpa-onnx C++ API TTS support; forced OFF because online TTS is used"
    FORCE
)

option(SMARTASSISTANT_ENABLE_ROS2
    "Build WheelchairController and ArmController with ROS2 cmd_vel publishing"
    ON
)

set(SHERPA_ONNX_ROOT "" CACHE PATH
    "sherpa-onnx install prefix containing include/ and lib/"
)

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

set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://gitcode.com/gh_mirrors/ix/IXWebSocket.git
    GIT_TAG v11.4.5
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/Middlewares/ixwebsocket"
    BINARY_DIR "${CMAKE_BINARY_DIR}/Middlewares/ixwebsocket"
)

FetchContent_MakeAvailable(tomlplusplus quill ixwebsocket)

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

pkg_check_modules(WEBRTC_APM REQUIRED IMPORTED_TARGET webrtc-audio-processing)
pkg_check_modules(SOXR       REQUIRED IMPORTED_TARGET soxr)
pkg_check_modules(UDEV       REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(PULSE      REQUIRED IMPORTED_TARGET libpulse)

if(SMARTASSISTANT_ENABLE_SHERPA_ONNX)
    set(_SHERPA_ONNX_HINTS
        /usr/local
        /usr
    )

    if(SHERPA_ONNX_ROOT)
        list(PREPEND _SHERPA_ONNX_HINTS "${SHERPA_ONNX_ROOT}")
    endif()

    if(DEFINED ENV{SHERPA_ONNX_ROOT} AND NOT "$ENV{SHERPA_ONNX_ROOT}" STREQUAL "")
        list(PREPEND _SHERPA_ONNX_HINTS "$ENV{SHERPA_ONNX_ROOT}")
    endif()

    find_path(SHERPA_ONNX_INCLUDE_DIR
        NAMES sherpa-onnx/c-api/c-api.h
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES include
    )

    find_library(SHERPA_ONNX_C_API_LIB
        NAMES sherpa-onnx-c-api
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
    )

    find_library(SHERPA_ONNX_CXX_API_LIB
        NAMES sherpa-onnx-cxx-api
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
    )

    find_library(SHERPA_ONNX_RUNTIME_LIB
        NAMES onnxruntime
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
    )

    set(_SHERPA_ONNX_MISSING "")
    if(NOT SHERPA_ONNX_INCLUDE_DIR)
        list(APPEND _SHERPA_ONNX_MISSING "include/sherpa-onnx/c-api/c-api.h")
    elseif(SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS AND NOT EXISTS "${SHERPA_ONNX_INCLUDE_DIR}/sherpa-onnx/c-api/cxx-api.h")
        list(APPEND _SHERPA_ONNX_MISSING "include/sherpa-onnx/c-api/cxx-api.h")
    endif()
    if(NOT SHERPA_ONNX_C_API_LIB)
        list(APPEND _SHERPA_ONNX_MISSING "lib/libsherpa-onnx-c-api.so")
    endif()
    if(SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS AND NOT SHERPA_ONNX_CXX_API_LIB)
        list(APPEND _SHERPA_ONNX_MISSING "lib/libsherpa-onnx-cxx-api.so")
    endif()
    if(NOT SHERPA_ONNX_RUNTIME_LIB)
        list(APPEND _SHERPA_ONNX_MISSING "lib/libonnxruntime.so")
    endif()

    if(_SHERPA_ONNX_MISSING)
        list(JOIN _SHERPA_ONNX_MISSING ", " _SHERPA_ONNX_MISSING_TEXT)
        message(FATAL_ERROR
            "sherpa-onnx not found; missing: ${_SHERPA_ONNX_MISSING_TEXT}. "
            "Install the official linux-x64 shared tarball into /usr/local, "
            "or configure with -DSHERPA_ONNX_ROOT=/path/to/sherpa-onnx. "
            "You can also disable these targets with "
            "-DSMARTASSISTANT_ENABLE_SHERPA_ONNX=OFF.")
    endif()
endif()

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

add_library(SmartAssistantE2EChat
    SmartAssistant/e2echat.cpp
)

target_include_directories(SmartAssistantE2EChat
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantE2EChat
    PUBLIC
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
        ixwebsocket
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
)

add_library(SmartAssistantTTSManager
    SmartAssistant/TTSManager.cpp
)

target_include_directories(SmartAssistantTTSManager
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantTTSManager
    PUBLIC
        SmartAssistantAudioManager
        SmartAssistantNetworkManager
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
        ixwebsocket
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS)
    if(NOT SMARTASSISTANT_ENABLE_SHERPA_ONNX)
        message(FATAL_ERROR
            "SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS requires "
            "SMARTASSISTANT_ENABLE_SHERPA_ONNX=ON.")
    endif()

    target_sources(SmartAssistantTTSManager
        PRIVATE
            SmartAssistant/SherpaOnnxTtsBridge.cpp
    )

    set_source_files_properties(
        SmartAssistant/SherpaOnnxTtsBridge.cpp
        PROPERTIES
            COMPILE_DEFINITIONS "_GLIBCXX_USE_CXX11_ABI=0"
    )

    target_include_directories(SmartAssistantTTSManager
        PUBLIC
            ${SHERPA_ONNX_INCLUDE_DIR}
    )

    target_compile_definitions(SmartAssistantTTSManager
        PRIVATE
            SMARTASSISTANT_ENABLE_SHERPA_ONNX_TTS=1
    )

    target_link_libraries(SmartAssistantTTSManager
        PRIVATE
            ${SHERPA_ONNX_CXX_API_LIB}
            ${SHERPA_ONNX_C_API_LIB}
            ${SHERPA_ONNX_RUNTIME_LIB}
    )
endif()

# ----------------------------------------------------------------------------
# WakeupManager — KWS + SV based on sherpa-onnx
# ----------------------------------------------------------------------------
if(SMARTASSISTANT_ENABLE_SHERPA_ONNX)
    set(_SHERPA_ONNX_HINTS
        /usr/local
        /usr
    )

    if(SHERPA_ONNX_ROOT)
        list(PREPEND _SHERPA_ONNX_HINTS "${SHERPA_ONNX_ROOT}")
    endif()

    if(DEFINED ENV{SHERPA_ONNX_ROOT} AND NOT "$ENV{SHERPA_ONNX_ROOT}" STREQUAL "")
        list(PREPEND _SHERPA_ONNX_HINTS "$ENV{SHERPA_ONNX_ROOT}")
    endif()

    find_path(SHERPA_ONNX_INCLUDE_DIR
        NAMES sherpa-onnx/c-api/c-api.h
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES include
    )

    find_library(SHERPA_ONNX_C_API_LIB
        NAMES sherpa-onnx-c-api
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
    )

    find_library(SHERPA_ONNX_RUNTIME_LIB
        NAMES onnxruntime
        HINTS ${_SHERPA_ONNX_HINTS}
        PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
    )

    set(_SHERPA_ONNX_MISSING "")
    if(NOT SHERPA_ONNX_INCLUDE_DIR)
        list(APPEND _SHERPA_ONNX_MISSING "include/sherpa-onnx/c-api/c-api.h")
    endif()
    if(NOT SHERPA_ONNX_C_API_LIB)
        list(APPEND _SHERPA_ONNX_MISSING "lib/libsherpa-onnx-c-api.so")
    endif()
    if(NOT SHERPA_ONNX_RUNTIME_LIB)
        list(APPEND _SHERPA_ONNX_MISSING "lib/libonnxruntime.so")
    endif()

    if(_SHERPA_ONNX_MISSING)
        list(JOIN _SHERPA_ONNX_MISSING ", " _SHERPA_ONNX_MISSING_TEXT)
        message(FATAL_ERROR
            "sherpa-onnx not found; missing: ${_SHERPA_ONNX_MISSING_TEXT}. "
            "Install the official linux-x64 shared tarball into /usr/local, "
            "or configure with -DSHERPA_ONNX_ROOT=/path/to/sherpa-onnx. "
            "You can also disable these targets with "
            "-DSMARTASSISTANT_ENABLE_SHERPA_ONNX=OFF.")
    endif()

    add_library(SmartAssistantWakeupManager
        SmartAssistant/WakeupManager.cpp
    )

    target_include_directories(SmartAssistantWakeupManager
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
            ${SHERPA_ONNX_INCLUDE_DIR}
    )

    target_link_libraries(SmartAssistantWakeupManager
        PUBLIC
            SmartAssistantAudioManager
            SmartAssistantTTSManager
            tomlplusplus::tomlplusplus
            quill::quill
        PRIVATE
            Threads::Threads
            ${SHERPA_ONNX_C_API_LIB}
            ${SHERPA_ONNX_RUNTIME_LIB}
    )

    # ------------------------------------------------------------------------
    # ASRManager — sherpa-onnx streaming (offline) + Doubao WebSocket (online)
    # ------------------------------------------------------------------------
    add_library(SmartAssistantASRManager
        SmartAssistant/ASRManager.cpp
    )

    target_include_directories(SmartAssistantASRManager
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
            ${SHERPA_ONNX_INCLUDE_DIR}
    )

    target_link_libraries(SmartAssistantASRManager
        PUBLIC
            SmartAssistantAudioManager
            SmartAssistantNetworkManager
            tomlplusplus::tomlplusplus
            quill::quill
        PRIVATE
            Threads::Threads
            ${SHERPA_ONNX_C_API_LIB}
            ${SHERPA_ONNX_RUNTIME_LIB}
            ixwebsocket
            ZLIB::ZLIB
            OpenSSL::SSL
            OpenSSL::Crypto
    )
endif()

# ----------------------------------------------------------------------------
# MusicPlayer — local MP3 playback via mpg123 process
# ----------------------------------------------------------------------------
add_library(SmartAssistantMusicPlayer
    SmartAssistant/MusicPlayer.cpp
)

target_include_directories(SmartAssistantMusicPlayer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantMusicPlayer
    PUBLIC
        SmartAssistantAudioManager
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
)

# ----------------------------------------------------------------------------
# SystemVolumeController — PulseAudio default sink volume/mute control
# ----------------------------------------------------------------------------
add_library(SmartAssistantSystemVolumeController
    SmartAssistant/SystemVolumeController.cpp
)

target_include_directories(SmartAssistantSystemVolumeController
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantSystemVolumeController
    PUBLIC
        PkgConfig::PULSE
)

# ----------------------------------------------------------------------------
# IntentManager — offline rule engine + online HTTP NLU client
# ----------------------------------------------------------------------------
add_library(SmartAssistantIntentManager
    SmartAssistant/IntentManager.cpp
)

target_include_directories(SmartAssistantIntentManager
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantIntentManager
    PUBLIC
        SmartAssistantNetworkManager
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
        ixwebsocket
        ZLIB::ZLIB
        OpenSSL::SSL
        OpenSSL::Crypto
)

# ----------------------------------------------------------------------------
# NavigationController — stub (no ROS2). Logs incoming destinations and
# returns a fake monotonic command id; future Nav2 / planner integration will
# replace navigateTo() internals without changing the public API.
# ----------------------------------------------------------------------------
add_library(SmartAssistantNavigationController
    SmartAssistant/NavigationController.cpp
)

target_include_directories(SmartAssistantNavigationController
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantNavigationController
    PUBLIC
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
)

# ----------------------------------------------------------------------------
# WheelchairController / ArmController / GripperController ROS2 clients.
# ----------------------------------------------------------------------------
if(SMARTASSISTANT_ENABLE_ROS2)
    find_package(rclcpp QUIET)
    find_package(geometry_msgs QUIET)
    find_package(std_msgs QUIET)

    if(rclcpp_FOUND AND geometry_msgs_FOUND AND std_msgs_FOUND)
        message(STATUS "ROS2 found: building SmartAssistantWheelchairController, SmartAssistantArmController, and SmartAssistantGripperController")

        add_library(SmartAssistantWheelchairController
            SmartAssistant/WheelchairController.cpp
        )

        target_include_directories(SmartAssistantWheelchairController
            PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
            PRIVATE
                ${rclcpp_INCLUDE_DIRS}
                ${geometry_msgs_INCLUDE_DIRS}
        )

        target_link_libraries(SmartAssistantWheelchairController
            PUBLIC
                tomlplusplus::tomlplusplus
                quill::quill
            PRIVATE
                Threads::Threads
                ${rclcpp_LIBRARIES}
                ${geometry_msgs_LIBRARIES}
        )

        add_library(SmartAssistantArmController
            SmartAssistant/ArmController.cpp
        )

        target_include_directories(SmartAssistantArmController
            PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
            PRIVATE
                ${rclcpp_INCLUDE_DIRS}
                ${geometry_msgs_INCLUDE_DIRS}
        )

        target_link_libraries(SmartAssistantArmController
            PUBLIC
                tomlplusplus::tomlplusplus
                quill::quill
            PRIVATE
                Threads::Threads
                ${rclcpp_LIBRARIES}
                ${geometry_msgs_LIBRARIES}
        )

        add_library(SmartAssistantGripperController
            SmartAssistant/GripperController.cpp
        )

        target_include_directories(SmartAssistantGripperController
            PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
            PRIVATE
                ${rclcpp_INCLUDE_DIRS}
                ${std_msgs_INCLUDE_DIRS}
        )

        target_link_libraries(SmartAssistantGripperController
            PUBLIC
                tomlplusplus::tomlplusplus
                quill::quill
            PRIVATE
                Threads::Threads
                ${rclcpp_LIBRARIES}
                ${std_msgs_LIBRARIES}
        )

        # NavigationController gains a real ROS2 backend when ROS2 is present:
        # it publishes the destination name (std_msgs/String) to nav_bridge and
        # std_msgs/Empty to /nav_clear.  Without ROS2 it stays a log-only stub.
        target_compile_definitions(SmartAssistantNavigationController
            PRIVATE NAVIGATION_HAS_ROS2)
        target_include_directories(SmartAssistantNavigationController
            PRIVATE
                ${rclcpp_INCLUDE_DIRS}
                ${std_msgs_INCLUDE_DIRS}
        )
        target_link_libraries(SmartAssistantNavigationController
            PRIVATE
                ${rclcpp_LIBRARIES}
                ${std_msgs_LIBRARIES}
        )
    else()
        message(STATUS
            "ROS2 (rclcpp / geometry_msgs / std_msgs) not found — "
            "SmartAssistantWheelchairController, SmartAssistantArmController, and SmartAssistantGripperController will NOT be built. "
            "Source /opt/ros/<distro>/setup.bash before configuring, "
            "or pass -DSMARTASSISTANT_ENABLE_ROS2=OFF to silence this message.")
    endif()
endif()

# ----------------------------------------------------------------------------
# Orchestrator — drives Wake → Doubao TTS greeting → E2E session → skill
# dispatch → ChatTTSText reply.  Replaces the legacy ASR + Intent + TTS
# main loop wholesale; all ASR / NLU / Chat / TTS work runs inside the
# Doubao realtime session.  Skill controllers are linked transitively
# only when the host has ROS2; the orchestrator handles missing controllers
# gracefully (the matching intents become acknowledged-but-no-op).
# Placed AFTER the ROS2 block so the if(TARGET ...) checks see the
# wheelchair / arm / gripper libraries when they were built.
# ----------------------------------------------------------------------------
add_library(SmartAssistantOrchestrator
    SmartAssistant/Orchestrator.cpp
)

target_include_directories(SmartAssistantOrchestrator
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SmartAssistant"
)

target_link_libraries(SmartAssistantOrchestrator
    PUBLIC
        SmartAssistantAudioManager
        SmartAssistantE2EChat
        SmartAssistantTTSManager
        SmartAssistantIntentManager
        SmartAssistantMusicPlayer
        SmartAssistantNavigationController
        SmartAssistantSystemVolumeController
        tomlplusplus::tomlplusplus
        quill::quill
    PRIVATE
        Threads::Threads
)

if(TARGET SmartAssistantWakeupManager)
    target_link_libraries(SmartAssistantOrchestrator
        PUBLIC SmartAssistantWakeupManager)
endif()

if(TARGET SmartAssistantWheelchairController)
    target_link_libraries(SmartAssistantOrchestrator
        PUBLIC SmartAssistantWheelchairController)
endif()
if(TARGET SmartAssistantArmController)
    target_link_libraries(SmartAssistantOrchestrator
        PUBLIC SmartAssistantArmController)
endif()
if(TARGET SmartAssistantGripperController)
    target_link_libraries(SmartAssistantOrchestrator
        PUBLIC SmartAssistantGripperController)
endif()

if(BUILD_TESTING)
    add_subdirectory(Test)
endif()
