// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

#include <clap/events.h>
#include <clap/process.h>

// Minimal empty event lists for process() calls.
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

TEST_CASE("plugin_instance: load stub plugin", "[plugin_instance]") {
    auto result = kairos::plugin_instance::load(
        KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub", kairos::kairos_host());

    REQUIRE(result);
    REQUIRE(result->current_state() == kairos::plugin_instance::state::initialized);
    REQUIRE(result->descriptor() != nullptr);
    REQUIRE(std::string(result->descriptor()->id) == "org.nomos-studio.test/stub");
}

TEST_CASE("plugin_instance: activate / deactivate", "[plugin_instance]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    auto act = inst->activate(48000.0, 32, 512);
    REQUIRE(act);
    REQUIRE(inst->current_state() == kairos::plugin_instance::state::activated);

    inst->deactivate();
    REQUIRE(inst->current_state() == kairos::plugin_instance::state::initialized);
}

TEST_CASE("plugin_instance: full lifecycle with process()", "[plugin_instance]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    REQUIRE(inst->activate(48000.0, 32, 512));
    REQUIRE(inst->start_processing());
    REQUIRE(inst->current_state() == kairos::plugin_instance::state::processing);

    const auto proc   = make_process(64);
    const auto status = inst->process(proc);
    REQUIRE(status == CLAP_PROCESS_SLEEP);

    inst->stop_processing();
    REQUIRE(inst->current_state() == kairos::plugin_instance::state::activated);

    inst->deactivate();
    REQUIRE(inst->current_state() == kairos::plugin_instance::state::initialized);
}

TEST_CASE("plugin_instance: wrong state returns error", "[plugin_instance]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    // start_processing before activate should fail
    auto sp = inst->start_processing();
    REQUIRE(!sp);
    REQUIRE(sp.error() == kairos::plugin_error::wrong_state);
}

TEST_CASE("plugin_instance: bad path returns load_failed", "[plugin_instance]") {
    auto result = kairos::plugin_instance::load(
        "/nonexistent/plugin.so", "org.nomos-studio.test/stub", kairos::kairos_host());

    REQUIRE(!result);
    REQUIRE(result.error() == kairos::plugin_error::load_failed);
}
