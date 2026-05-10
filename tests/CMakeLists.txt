# SPDX-License-Identifier: LGPL-2.1-or-later
include(Catch)

add_subdirectory(stub_plugin)

add_executable(kairos-tests
    test_time_identity.cpp
    test_ipc.cpp
    test_ipc_channel.cpp
    test_spsc_queue.cpp
    test_session.cpp
    test_plugin_instance.cpp
    test_plugin_graph.cpp
    test_builtin_plugins.cpp
)

target_link_libraries(kairos-tests
    PRIVATE
        kairos::core
        Catch2::Catch2WithMain
)

# Inject the stub plugin path as a compile-time string literal.
target_compile_definitions(kairos-tests PRIVATE
    KAIROS_STUB_PLUGIN_PATH="$<TARGET_FILE:kairos-stub-plugin>"
)

set_target_properties(kairos-tests PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
)

catch_discover_tests(kairos-tests)
