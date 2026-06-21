// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <nomos/rt/ipc_channel.hpp>

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

// Create a connected socketpair for testing read/write round-trips.
static std::pair<int, int> make_pair() {
    int fds[2]{-1, -1};
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    return {fds[0], fds[1]};
}

TEST_CASE("ipc_channel: round-trip empty payload", "[ipc_channel]") {
    auto [a, b] = make_pair();

    auto wres = nomos::rt::ipc::write_message(a, nomos::rt::ipc::msg_session_open);
    REQUIRE(wres);

    auto rres = nomos::rt::ipc::read_message(b);
    REQUIRE(rres);
    REQUIRE(rres->type() == nomos::rt::ipc::msg_session_open);
    REQUIRE(rres->payload.empty());

    ::close(a);
    ::close(b);
}

TEST_CASE("ipc_channel: round-trip string payload", "[ipc_channel]") {
    auto [a, b] = make_pair();

    const std::string payload = "{:id :org.nomos-studio/loop :name \"loop\"}";
    auto wres = nomos::rt::ipc::write_message(a, nomos::rt::ipc::msg_register_source, payload);
    REQUIRE(wres);

    auto rres = nomos::rt::ipc::read_message(b);
    REQUIRE(rres);
    REQUIRE(rres->type() == nomos::rt::ipc::msg_register_source);

    const std::string_view got{reinterpret_cast<const char*>(rres->payload.data()),
                               rres->payload.size()};
    REQUIRE(std::string(got) == payload);

    ::close(a);
    ::close(b);
}

TEST_CASE("ipc_channel: EOF returns eof error", "[ipc_channel]") {
    auto [a, b] = make_pair();
    ::close(a); // close the write end

    auto rres = nomos::rt::ipc::read_message(b);
    REQUIRE(!rres);
    REQUIRE(rres.error() == nomos::rt::ipc::channel_error::eof);

    ::close(b);
}
