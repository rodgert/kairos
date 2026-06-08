# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)

project(kairos
    VERSION 0.1.0
    DESCRIPTION "kairos — CLAP container runtime for nomos-studio plugins"
    HOMEPAGE_URL "https://github.com/nomos-studio/kairos"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Dependency policy
#
# - No Boost. C++20 STL covers all use cases.
# - FetchContent + pinned tag for any dependency with a build step.
# - FETCHCONTENT_UPDATES_DISCONNECTED: skip network on subsequent invocations.
# - Tests use Catch2 v3 (Boost Software License, header-friendly).
# - nomos-topology: header-only schema constants; pin to a commit SHA.
#   Set NOMOS_TOPOLOGY_SRC_DIR to a local checkout to skip the network fetch.
# ---------------------------------------------------------------------------

include(FetchContent)
include(GNUInstallDirs)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip network fetch if already populated")

# ---------------------------------------------------------------------------
# Build options
# ---------------------------------------------------------------------------

option(KAIROS_BUILD_TESTS      "Build kairos unit tests"            OFF)
option(KAIROS_BUILD_CORE       "Build the kairos CLAP plugin layer"  ON)
option(KAIROS_BUILD_EXECUTABLE "Build the kairos runtime binary"    ON)
option(KAIROS_WASM_BRIDGE      "Build the Faust WASM bridge plugin (requires network for wasmtime)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# CLAP SDK — header-only C/C++ interface.
# Declared here so FetchContent_MakeAvailable(nomos-rt) below does not
# re-fetch it; both projects pin the same tag so the first declaration wins.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.6
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(clap)

# nomos-topology — header-only EDN key constants (nomos::topology).
#   To use a local checkout: cmake -DNOMOS_TOPOLOGY_SRC_DIR=/path/to/nomos-topology ...
if(DEFINED NOMOS_TOPOLOGY_SRC_DIR)
    FetchContent_Declare(nomos_topology SOURCE_DIR "${NOMOS_TOPOLOGY_SRC_DIR}")
else()
    FetchContent_Declare(nomos_topology
        GIT_REPOSITORY https://github.com/nomos-studio/nomos-topology.git
        GIT_TAG        01eef2f14e61e62a93ada455ca2a47263a90f3c5)
endif()
FetchContent_MakeAvailable(nomos_topology)

# nomos-rt — shared C++ runtime substrate (Link, MIDI/OSC, IPC, txlog, modulators).
#   To use a local checkout: cmake -DNOMOS_RT_DIR=/path/to/nomos-rt ...
if(DEFINED NOMOS_RT_DIR)
    FetchContent_Declare(nomos-rt SOURCE_DIR "${NOMOS_RT_DIR}")
else()
    FetchContent_Declare(nomos-rt
        GIT_REPOSITORY https://github.com/nomos-studio/nomos-rt.git
        GIT_TAG        0d7ef769c94334ec34d6c0f3a7a073a03b7d98e7)
endif()
set(NOMOS_RT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nomos-rt)

# ---------------------------------------------------------------------------
# wasmtime C API — pre-built binaries, downloaded only when KAIROS_WASM_BRIDGE=ON.
#
# Platform naming follows bytecodealliance/wasmtime release convention:
#   aarch64-macos, x86_64-macos, aarch64-linux, x86_64-linux
#
# Pin: v44.0.1 — update with full SHA256 verification.
# ---------------------------------------------------------------------------

if(KAIROS_WASM_BRIDGE)
    set(_wasmtime_ver "v44.0.1")

    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_wasmtime_platform "aarch64-macos")
            set(_wasmtime_hash "SHA256=5da6d09fa6340db6d7afc8b73d189eff4973825c9a6fa5e814d2f9b1ddfd8998")
        else()
            set(_wasmtime_platform "x86_64-macos")
            set(_wasmtime_hash "SHA256=8a3877178c50a2a2a20fcb94ca86efd14ba4376698e48a509f72bc52e961ce49")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_wasmtime_platform "aarch64-linux")
            set(_wasmtime_hash "SHA256=5b4614a311c7c391fb1fc5e9c7ab3782f5a441f9e1fa0cc60fe364ff5371c010")
        else()
            set(_wasmtime_platform "x86_64-linux")
            set(_wasmtime_hash "SHA256=fe9b4bfa87724054cebcc09b83a17d5ded66a465054b77c8de0ebc166ad78d7a")
        endif()
    else()
        message(FATAL_ERROR "KAIROS_WASM_BRIDGE: unsupported platform (Windows not yet supported)")
    endif()

    set(_wasmtime_name "wasmtime-${_wasmtime_ver}-${_wasmtime_platform}-c-api")

    FetchContent_Declare(wasmtime
        URL "https://github.com/bytecodealliance/wasmtime/releases/download/${_wasmtime_ver}/${_wasmtime_name}.tar.xz"
        URL_HASH "${_wasmtime_hash}")
    FetchContent_MakeAvailable(wasmtime)

    add_library(kairos-wasmtime INTERFACE)
    target_include_directories(kairos-wasmtime INTERFACE "${wasmtime_SOURCE_DIR}/include")
    target_link_libraries(kairos-wasmtime INTERFACE
        "${wasmtime_SOURCE_DIR}/lib/libwasmtime.a")
    if(APPLE)
        target_link_libraries(kairos-wasmtime INTERFACE
            "-framework Security" "-framework CoreFoundation" "-framework SystemConfiguration")
    elseif(UNIX)
        target_link_libraries(kairos-wasmtime INTERFACE pthread dl)
    endif()

    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install    OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# kairos-core — CLAP plugin layer + process/audio engine.
#
# Depends on nomos::rt for the runtime substrate.  The executable is a thin
# main() that links kairos::core (which brings nomos::rt transitively).
# Set KAIROS_BUILD_CORE=OFF to build without the CLAP layer.
# ---------------------------------------------------------------------------

if(KAIROS_BUILD_CORE)

add_library(kairos-core STATIC
    src/version.cpp
    src/control_thread.cpp
    src/plugin_host.cpp
    src/plugin_instance.cpp
    src/plugin_graph_manager.cpp
    src/builtin_plugins.cpp
    src/process_thread.cpp
    src/audio_engine.cpp
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
        nomos::rt
        nomos::topology
    PRIVATE
        nomos::rt-exe-headers
        ${CMAKE_DL_LIBS}
)

set_target_properties(kairos-core PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

if(KAIROS_WASM_BRIDGE)
    target_sources(kairos-core PRIVATE src/wasm_bridge_plugin.cpp)
    target_compile_definitions(kairos-core PRIVATE KAIROS_WASM_BRIDGE=1)
    target_link_libraries(kairos-core PRIVATE kairos-wasmtime nlohmann_json::nlohmann_json)
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    target_compile_options(kairos-core PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    )
endif()

endif() # KAIROS_BUILD_CORE

# ---------------------------------------------------------------------------
# Executable — thin main() + plugin discovery
# ---------------------------------------------------------------------------

if(KAIROS_BUILD_EXECUTABLE AND KAIROS_BUILD_CORE)
    add_executable(kairos
        src/main.cpp
        src/plugin_discovery.cpp
    )

    target_include_directories(kairos PRIVATE src)

    target_link_libraries(kairos PRIVATE
        kairos::core
        nomos::rt-exe-headers
    )

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

if(KAIROS_BUILD_TESTS AND KAIROS_BUILD_CORE)
    enable_testing()

    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()
