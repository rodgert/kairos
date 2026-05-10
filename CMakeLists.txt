# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(kairos
    VERSION 0.1.0
    DESCRIPTION "kairos — CLAP container runtime for cljseq-generated plugins"
    HOMEPAGE_URL "https://github.com/cljseq/kairos"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Dependency policy (mirrors cljseq native convention)
#
# - No Boost. C++20 STL covers all use cases.
# - FetchContent + pinned tag for any dependency with a build step.
# - FETCHCONTENT_UPDATES_DISCONNECTED: skip network on subsequent invocations.
# - Tests use Catch2 v3 (Boost Software License, header-friendly).
# - SQLite3 via find_package — available on all target platforms.
# ---------------------------------------------------------------------------

include(FetchContent)
include(GNUInstallDirs)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip network fetch if already populated")

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

option(KAIROS_BUILD_TESTS      "Build kairos unit tests"           OFF)
option(KAIROS_BUILD_EXECUTABLE "Build the kairos runtime binary"   ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# SQLite3 — system package on all target platforms.
find_package(SQLite3 REQUIRED)

# txlog-cpp — pulls in edn-cpp transitively.
#   To use a local checkout: cmake -DTXLOG_CPP_DIR=/path/to/txlog-cpp ...
if(DEFINED TXLOG_CPP_DIR)
    FetchContent_Declare(txlog-cpp SOURCE_DIR "${TXLOG_CPP_DIR}")
else()
    FetchContent_Declare(txlog-cpp
        GIT_REPOSITORY https://github.com/cljseq/txlog-cpp.git
        GIT_TAG        v0.1.0
        GIT_SHALLOW    TRUE)
endif()
set(TXLOG_CPP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(TXLOG_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(txlog-cpp)

# CLAP SDK — header-only C/C++ interface.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.6
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(clap)

# ---------------------------------------------------------------------------
# liburcu — vendored from third_party/liburcu/ (LGPL-2.1-or-later).
#
# Flavor selection:
#   macOS  → urcu-bp (bulletproof; no thread registration, no sys_membarrier)
#   Linux  → urcu-mb (memory barrier; sys_membarrier upgrade path deferred)
#
# config.h is copied from the appropriate platform file at configure time.
# ---------------------------------------------------------------------------

set(URCU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/liburcu")
set(URCU_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/urcu-include")

# Select and materialise config.h for the target platform.
# Start with urcu-bp on all platforms: no thread registration, no
# sys_membarrier dependency, works in the audio callback without ceremony.
# Linux upgrade path: switch URCU_FLAVOR_SRC to urcu.c (urcu-mb) once
# we are ready to adopt the thread-registration discipline.
if(APPLE)
    set(URCU_CONFIG_SRC "${URCU_ROOT}/config/config-darwin.h")
else()
    set(URCU_CONFIG_SRC "${URCU_ROOT}/config/config-linux.h")
endif()
set(URCU_FLAVOR_SRC "${URCU_ROOT}/src/urcu-bp.c")

configure_file("${URCU_CONFIG_SRC}"
               "${URCU_GENERATED_INCLUDE}/urcu/config.h" COPYONLY)

add_library(urcu-flavor STATIC
    "${URCU_FLAVOR_SRC}"
    "${URCU_ROOT}/src/compat_futex.c"
    "${URCU_ROOT}/src/urcu-call-rcu-impl.h"
)

add_library(urcu::flavor ALIAS urcu-flavor)

target_include_directories(urcu-flavor
    PUBLIC
        "${URCU_ROOT}/include"
        "${URCU_GENERATED_INCLUDE}"
    PRIVATE
        "${URCU_ROOT}/src"
)

# Macro defines intentionally omitted: urcu-bp.c and urcu.c define
# _LGPL_SOURCE and URCU_NO_COMPAT_IDENTIFIERS themselves at the top of the file.

set_target_properties(urcu-flavor PROPERTIES
    C_STANDARD   11
    C_STANDARD_REQUIRED ON
)

# RtMidi — cross-platform MIDI I/O (MIT).
# Pin: 6.0.0 — matches cljseq-sidecar.
FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG        6.0.0
    GIT_SHALLOW    TRUE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtmidi)

# RtAudio — cross-platform audio I/O (MIT).
# Pin: 6.0.1 — same family as RtMidi, matches cljseq native convention.
FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG        6.0.1
    GIT_SHALLOW    TRUE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_TARGETNAME_UNINSTALL rtaudio_uninstall CACHE STRING "" FORCE)
FetchContent_MakeAvailable(rtaudio)

# Ableton Link — beat-sync peer protocol.
# Use FetchContent_Populate + include() rather than FetchContent_MakeAvailable:
# AbletonLinkConfig.cmake creates an IMPORTED INTERFACE target; if added via
# add_subdirectory (what MakeAvailable does) that target has subdirectory scope
# and is invisible to this CMakeLists. Including the config directly puts the
# target in the top-level scope where target_link_libraries can find it.
#   To use a local checkout: cmake -DLINK_DIR=/path/to/link ...
if(DEFINED LINK_DIR)
    FetchContent_Declare(abl-link SOURCE_DIR "${LINK_DIR}")
else()
    FetchContent_Declare(abl-link
        GIT_REPOSITORY https://github.com/Ableton/link.git
        GIT_TAG        Link-3.1.1
        GIT_SHALLOW    TRUE)
endif()
FetchContent_GetProperties(abl-link)
if(NOT abl-link_POPULATED)
    FetchContent_Populate(abl-link)
endif()
include(${abl-link_SOURCE_DIR}/AbletonLinkConfig.cmake)

# ---------------------------------------------------------------------------
# kairos-core — static library; all testable logic lives here.
# The kairos executable is a thin wrapper that calls kairos_core::run().
# ---------------------------------------------------------------------------

add_library(kairos-core STATIC
    src/version.cpp
    src/ipc_channel.cpp
    src/session.cpp
    src/control_thread.cpp
    src/plugin_host.cpp
    src/plugin_instance.cpp
    src/plugin_graph_manager.cpp
    src/builtin_plugins.cpp
)

add_library(kairos::core ALIAS kairos-core)

target_include_directories(kairos-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        src
)

target_link_libraries(kairos-core
    PUBLIC
        txlog::txlog
        clap
        urcu::flavor
    PRIVATE
        ${CMAKE_DL_LIBS}
    # Ableton::Link is GPL-2.0-or-later and is linked only into the executable,
    # keeping kairos-core (and its LGPL-2.1-or-later headers) free of GPL exposure.
)

set_target_properties(kairos-core PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

# ---------------------------------------------------------------------------
# Compiler warnings (opt-in; does not affect consumers)
# ---------------------------------------------------------------------------

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-core PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

if(KAIROS_BUILD_EXECUTABLE)
    add_executable(kairos
        src/main.cpp
        src/link_peer.cpp
        src/midi_io.cpp
        src/event_collector.cpp
        src/input_event_buffer.cpp
        src/osc_server.cpp
        src/process_thread.cpp
        src/plugin_discovery.cpp
        src/audio_device.cpp
        src/audio_engine.cpp
    )

    target_include_directories(kairos PRIVATE src)

    target_link_libraries(kairos PRIVATE kairos::core Ableton::Link rtmidi rtaudio)

    set_target_properties(kairos PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS        OFF
    )

    if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
        target_compile_options(kairos PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(KAIROS_BUILD_TESTS)
    enable_testing()

    # Catch2 v3 — BSL-1.0, header-friendly, no extra dependencies.
    # Pin: v3.7.1 — update deliberately and document the reason.
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()
