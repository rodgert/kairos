// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <nomos/rt/session.hpp>

TEST_CASE("session: open in-memory database", "[session]") {
    auto sess = nomos::rt::session::open(":memory:");
    REQUIRE(sess.has_value());
    REQUIRE(sess->is_open());
}

TEST_CASE("session: close marks session as closed", "[session]") {
    auto sess = nomos::rt::session::open(":memory:");
    REQUIRE(sess->is_open());
    sess->close();
    REQUIRE(!sess->is_open());
}

TEST_CASE("session: register_source is idempotent", "[session]") {
    auto sess = nomos::rt::session::open(":memory:");
    REQUIRE(sess.has_value());

    nomos::rt::source_info src{
        .id          = edn::keyword{"org.nomos-studio/loop"},
        .name        = "loop",
        .description = "nous loop sequencer",
    };

    REQUIRE_NOTHROW(sess->register_source(src));
    REQUIRE_NOTHROW(sess->register_source(src)); // idempotent
}

TEST_CASE("session: emit writes an entry", "[session]") {
    auto sess = nomos::rt::session::open(":memory:");
    REQUIRE(sess.has_value());

    sess->register_source({
        .id   = edn::keyword{"org.nomos-studio/loop"},
        .name = "loop",
    });

    txlog::entry e{};
    e.beat    = 1.0;
    e.wall_ns = 0;
    e.source  = edn::keyword{"org.nomos-studio/loop"};
    e.path    = edn::vector{{edn::keyword{"test/param"}}};
    e.after   = edn::value{42.0};

    REQUIRE_NOTHROW(sess->emit(e));

    auto entries = sess->log().read_all();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].beat == 1.0);
}
