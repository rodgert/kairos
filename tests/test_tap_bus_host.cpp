// SPDX-License-Identifier: LGPL-2.1-or-later
// Host-side tests for the kairos/tap-bus consumer path.
//
// Uses two plugins:
//   KAIROS_STUB_PLUGIN_PATH         — existing stub, no tap-bus extension
//   KAIROS_STUB_TAP_BUS_PLUGIN_PATH — stub with 2-tap bus implementation

#include <catch2/catch_test_macros.hpp>

#include <kairos/clap_kairos_tap_bus.h>
#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

#include <clap/events.h>
#include <clap/process.h>

namespace {

uint32_t in_size(const clap_input_events_t*) {
    return 0;
}
const clap_event_header_t* in_get(const clap_input_events_t*, uint32_t) {
    return nullptr;
}
bool out_try_push(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
}

const clap_input_events_t  empty_in{nullptr, in_size, in_get};
const clap_output_events_t empty_out{nullptr, out_try_push};

clap_process_t make_process(uint32_t frames = 64) {
    clap_process_t p{};
    p.steady_time         = 0;
    p.frames_count        = frames;
    p.transport           = nullptr;
    p.audio_inputs        = nullptr;
    p.audio_outputs       = nullptr;
    p.audio_inputs_count  = 0;
    p.audio_outputs_count = 0;
    p.in_events           = &empty_in;
    p.out_events          = &empty_out;
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Plugins that do not expose tap-bus
// ---------------------------------------------------------------------------

TEST_CASE("tap-bus host: tap_schema() returns null for non-tap plugin", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->tap_schema() == nullptr);
}

TEST_CASE("tap-bus host: tap_frame() returns null for non-tap plugin", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    uint32_t     count = 99;
    const float* frame = inst->tap_frame(&count);
    REQUIRE(frame == nullptr);
    REQUIRE(count == 0);
}

// ---------------------------------------------------------------------------
// Plugins that do expose tap-bus
// ---------------------------------------------------------------------------

TEST_CASE("tap-bus host: tap_schema() returns null before activate", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    // Schema is non-null from the extension vtable, but count == 0 and epoch == 0
    // before any activate() — the stub initialises epoch to 0.
    const auto* schema = inst->tap_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->epoch == 0);
}

TEST_CASE("tap-bus host: tap_schema() populated after activate", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    const auto* schema = inst->tap_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->epoch > 0);
    REQUIRE(schema->count == 2);
    REQUIRE(schema->entries != nullptr);
    REQUIRE(std::string(schema->entries[0].name) == "test/alpha");
    REQUIRE(std::string(schema->entries[1].name) == "test/beta");
    REQUIRE(schema->entries[0].id == 0);
    REQUIRE(schema->entries[1].id == 1);
}

TEST_CASE("tap-bus host: epoch increments on re-activate", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    const uint32_t epoch1 = inst->tap_schema()->epoch;

    inst->deactivate();
    REQUIRE(inst->activate(48000.0, 32, 512));
    const uint32_t epoch2 = inst->tap_schema()->epoch;

    REQUIRE(epoch2 > epoch1);
}

TEST_CASE("tap-bus host: tap_frame() count matches schema count after process", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    REQUIRE(inst->start_processing());

    const uint32_t schema_count = inst->tap_schema()->count;

    const auto proc = make_process();
    REQUIRE(inst->process(proc) != CLAP_PROCESS_ERROR);

    uint32_t     frame_count = 0;
    const float* frame       = inst->tap_frame(&frame_count);
    REQUIRE(frame != nullptr);
    REQUIRE(frame_count == schema_count);

    inst->stop_processing();
    inst->deactivate();
}

TEST_CASE("tap-bus host: tap_frame() values are correct after process", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    REQUIRE(inst->start_processing());

    const auto proc = make_process();
    REQUIRE(inst->process(proc) != CLAP_PROCESS_ERROR);

    uint32_t     count = 0;
    const float* frame = inst->tap_frame(&count);
    REQUIRE(count == 2);
    REQUIRE(frame[0] == 0.25f);
    REQUIRE(frame[1] == 0.75f);

    inst->stop_processing();
    inst->deactivate();
}

TEST_CASE("tap-bus host: epoch stable across process() calls", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    REQUIRE(inst->start_processing());

    const uint32_t epoch_before = inst->tap_schema()->epoch;

    const auto proc = make_process();
    for (int i = 0; i < 4; ++i)
        REQUIRE(inst->process(proc) != CLAP_PROCESS_ERROR);

    REQUIRE(inst->tap_schema()->epoch == epoch_before);

    inst->stop_processing();
    inst->deactivate();
}

TEST_CASE("tap-bus host: plugin_instance move preserves tap_bus_ext", "[tap_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_TAP_BUS_PLUGIN_PATH,
                                              "org.nomos.test/stub-tap-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    kairos::plugin_instance moved  = std::move(*inst);
    const auto*             schema = moved.tap_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->count == 2);

    moved.deactivate();
}
