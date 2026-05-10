# SPDX-License-Identifier: LGPL-2.1-or-later
include(Catch)

add_subdirectory(stub_plugin)

# ---------------------------------------------------------------------------
# WASM bridge test: generate a phasor .wasm at configure time if faust is
# available, then inject the path as a compile-time string.
# ---------------------------------------------------------------------------

set(_kairos_test_wasm_path "")

if(KAIROS_WASM_BRIDGE)
    find_program(FAUST_EXECUTABLE faust)
    if(FAUST_EXECUTABLE)
        set(_phasor_dsp "${CMAKE_CURRENT_BINARY_DIR}/test_phasor.dsp")
        set(_phasor_wasm "${CMAKE_CURRENT_BINARY_DIR}/test_phasor.wasm")
        file(WRITE "${_phasor_dsp}"
            "import(\"stdfaust.lib\");\nprocess = os.phasor(1.0, 440.0);\n")
        execute_process(
            COMMAND "${FAUST_EXECUTABLE}" -lang wasm "${_phasor_dsp}" -o "${_phasor_wasm}"
            RESULT_VARIABLE _faust_result
            OUTPUT_QUIET ERROR_QUIET)
        if(_faust_result EQUAL 0)
            set(_kairos_test_wasm_path "${_phasor_wasm}")
            message(STATUS "WASM bridge test: generated ${_phasor_wasm}")
        else()
            message(WARNING "WASM bridge test: faust failed, bridge tests will skip")
        endif()
    else()
        message(STATUS "WASM bridge test: faust not found, bridge tests will skip")
    endif()
endif()

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------

set(_kairos_test_sources
    test_time_identity.cpp
    test_ipc.cpp
    test_ipc_channel.cpp
    test_spsc_queue.cpp
    test_session.cpp
    test_plugin_instance.cpp
    test_plugin_graph.cpp
    test_builtin_plugins.cpp
)

if(KAIROS_WASM_BRIDGE)
    list(APPEND _kairos_test_sources test_wasm_bridge.cpp)
endif()

add_executable(kairos-tests ${_kairos_test_sources})

target_link_libraries(kairos-tests
    PRIVATE
        kairos::core
        Catch2::Catch2WithMain
)

if(KAIROS_WASM_BRIDGE)
    # test_wasm_bridge.cpp includes wasm_bridge_plugin.hpp from src/
    target_include_directories(kairos-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
endif()

# Inject paths as compile-time string literals.
target_compile_definitions(kairos-tests PRIVATE
    KAIROS_STUB_PLUGIN_PATH="$<TARGET_FILE:kairos-stub-plugin>"
    KAIROS_TEST_WASM_PATH="${_kairos_test_wasm_path}"
)

set_target_properties(kairos-tests PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

catch_discover_tests(kairos-tests)
