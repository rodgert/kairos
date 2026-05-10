// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <kairos/ipc.hpp>

TEST_CASE("ipc: header is 8 bytes", "[ipc]") {
    static_assert(sizeof(kairos::ipc::header) == kairos::ipc::header_size);
    REQUIRE(sizeof(kairos::ipc::header) == 8);
}

TEST_CASE("ipc: message type constants are in expected ranges", "[ipc]") {
    // 0x3x session/graph block
    REQUIRE(kairos::ipc::msg_tx_log == 0x30);
    REQUIRE(kairos::ipc::msg_session_open == 0x31);
    REQUIRE(kairos::ipc::msg_session_close == 0x32);
    REQUIRE(kairos::ipc::msg_register_source == 0x33);
    REQUIRE(kairos::ipc::msg_graph_load == 0x34);
    REQUIRE(kairos::ipc::msg_graph_reset == 0x35);

    // 0x40 param/note block
    REQUIRE(kairos::ipc::msg_param_set == 0x40);
    REQUIRE(kairos::ipc::msg_note_on == 0x41);
    REQUIRE(kairos::ipc::msg_note_off == 0x42);
    REQUIRE(kairos::ipc::msg_midi_in == 0x43);
}

TEST_CASE("ipc: default-constructed header has zero payload_len", "[ipc]") {
    kairos::ipc::header h;
    REQUIRE(h.payload_len == 0);
    REQUIRE(h.type == 0);
}
