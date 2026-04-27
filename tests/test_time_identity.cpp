// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <kairos/time_identity.hpp>

TEST_CASE("time_identity: no pending at construction", "[time_identity]") {
    kairos::time_identity ti;
    REQUIRE(!ti.pending.has_value());
    REQUIRE(!ti.transition_ready(0.0));
    REQUIRE(!ti.apply_if_ready(0.0));
}

TEST_CASE("time_identity: pending fires at apply_at", "[time_identity]") {
    kairos::time_identity ti;
    ti.current = {120.0, 0.0};
    ti.pending = kairos::pending_transition{
        .tl       = {140.0, 4.0},
        .policy   = edn::keyword{"bar-quantize"},
        .apply_at = 4.0,
    };

    REQUIRE(!ti.transition_ready(3.99));
    REQUIRE(ti.transition_ready(4.0));
    REQUIRE(ti.transition_ready(4.01));

    bool promoted = ti.apply_if_ready(4.0);
    REQUIRE(promoted);
    REQUIRE(ti.current.bpm == Catch::Approx(140.0));
    REQUIRE(ti.current.beat == Catch::Approx(4.0));
    REQUIRE(!ti.pending.has_value());
}

TEST_CASE("time_identity: apply_if_ready is idempotent after promotion", "[time_identity]") {
    kairos::time_identity ti;
    ti.current = {120.0, 0.0};
    ti.pending = kairos::pending_transition{
        .tl       = {140.0, 4.0},
        .policy   = edn::keyword{"snap"},
        .apply_at = 4.0,
    };

    ti.apply_if_ready(4.0);
    REQUIRE(!ti.apply_if_ready(4.0)); // already promoted; no pending remains
    REQUIRE(ti.current.bpm == Catch::Approx(140.0));
}
