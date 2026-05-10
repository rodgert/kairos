// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests for the WASM bridge plugin (KAIROS_WASM_BRIDGE=ON only).
//
// KAIROS_TEST_WASM_PATH is a compile-time string injected by CMake.
// It points to a Faust-generated .wasm file (phasor with one output).
// If faust was not available at configure time, the path is "" and all
// tests skip themselves.

#include <catch2/catch_test_macros.hpp>

#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

#include "wasm_bridge_plugin.hpp"

#include <clap/events.h>
#include <clap/ext/params.h>
#include <clap/process.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

uint32_t evt_in_size(const clap_input_events_t*) {
    return 0;
}
const clap_event_header_t* evt_in_get(const clap_input_events_t*, uint32_t) {
    return nullptr;
}
bool evt_out_push(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
}
const clap_input_events_t  empty_in{nullptr, evt_in_size, evt_in_get};
const clap_output_events_t empty_out{nullptr, evt_out_push};

clap_process_t make_process(clap_audio_buffer_t* outs, uint32_t nouts, uint32_t frames = 256) {
    clap_process_t p{};
    p.steady_time         = 0;
    p.frames_count        = frames;
    p.transport           = nullptr;
    p.audio_inputs        = nullptr;
    p.audio_outputs       = outs;
    p.audio_inputs_count  = 0;
    p.audio_outputs_count = nouts;
    p.in_events           = &empty_in;
    p.out_events          = &empty_out;
    return p;
}

std::string wasm_plugin_id(const std::string& path) {
    return std::string(kairos::k_wasm_bridge_id_prefix) + path;
}

} // namespace

TEST_CASE("wasm_bridge: skip if no test wasm", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set — install faust and reconfigure");
    SUCCEED();
}

TEST_CASE("wasm_bridge: load and init phasor", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto result =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(result);
    REQUIRE(result->current_state() == kairos::plugin_instance::state::initialized);
    REQUIRE(result->descriptor() != nullptr);
    REQUIRE(std::string(result->descriptor()->id) == wasm_plugin_id(wasm_path));

    // Phasor: 0 inputs, 1 output
    CHECK(result->in_ports().empty());
    REQUIRE(result->out_ports().size() == 1);
    CHECK(result->out_ports()[0].channel_count == 1u);
}

TEST_CASE("wasm_bridge: activate and produce audio", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto inst =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(inst);

    REQUIRE(inst->activate(48000.0, 32, 256));
    REQUIRE(inst->start_processing());

    // Allocate a mono output buffer
    static constexpr uint32_t k_frames = 256;
    std::vector<float>        ch0(k_frames, 0.0f);
    float*                    ch0_ptr = ch0.data();

    clap_audio_buffer_t out_buf{};
    out_buf.data32        = &ch0_ptr;
    out_buf.channel_count = 1;
    out_buf.latency       = 0;
    out_buf.constant_mask = 0;

    auto       proc   = make_process(&out_buf, 1, k_frames);
    const auto status = inst->process(proc);
    REQUIRE(status == CLAP_PROCESS_CONTINUE);

    // A phasor generates non-silence; at least some samples must be non-zero.
    bool has_signal = false;
    for (float v : ch0) {
        if (v != 0.0f) {
            has_signal = true;
            break;
        }
    }
    REQUIRE(has_signal);

    inst->stop_processing();
    inst->deactivate();
}

TEST_CASE("wasm_bridge: full process cycle — two blocks", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto inst =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    REQUIRE(inst->start_processing());

    static constexpr uint32_t k_frames = 128;
    std::vector<float>        ch0(k_frames, 0.0f);
    float*                    ch0_ptr = ch0.data();
    clap_audio_buffer_t       out_buf{};
    out_buf.data32        = &ch0_ptr;
    out_buf.channel_count = 1;

    // First block
    auto proc1 = make_process(&out_buf, 1, k_frames);
    REQUIRE(inst->process(proc1) == CLAP_PROCESS_CONTINUE);
    const float first_sample = ch0[0];

    // Second block — phasor state advances, sample should differ from block start
    auto proc2 = make_process(&out_buf, 1, k_frames);
    REQUIRE(inst->process(proc2) == CLAP_PROCESS_CONTINUE);
    // The second block's first sample should not be the same as first block's
    // unless phasor cycles exactly — extremely unlikely at 440 Hz / 48 kHz.
    CHECK(ch0[0] != first_sample);

    inst->stop_processing();
    inst->deactivate();
}

TEST_CASE("wasm_bridge: param discovery", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto result =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(result);

    const clap_plugin_t* plugin = result->raw();
    REQUIRE(plugin != nullptr);

    const auto* params_ext =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params_ext != nullptr);

    // test_phasor.dsp: freq = hslider("freq", 440, 20, 20000, 1)
    REQUIRE(params_ext->count(plugin) == 1u);

    clap_param_info_t info{};
    REQUIRE(params_ext->get_info(plugin, 0, &info));
    CHECK(info.id == 0u);
    CHECK(std::string(info.name) == "freq");
    CHECK(info.min_value == 20.0);
    CHECK(info.max_value == 20000.0);
    CHECK(info.default_value == 440.0);
    CHECK((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0u);
}

TEST_CASE("wasm_bridge: param get_value returns default", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto result =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(result);

    const clap_plugin_t* plugin = result->raw();
    const auto*          params_ext =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params_ext != nullptr);

    double value = -1.0;
    REQUIRE(params_ext->get_value(plugin, 0, &value));
    CHECK(value == 440.0);
}

TEST_CASE("wasm_bridge: params_flush applies param event", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto result =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(result);

    const clap_plugin_t* plugin = result->raw();
    const auto*          params_ext =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    REQUIRE(params_ext != nullptr);

    // Synthetic CLAP_EVENT_PARAM_VALUE: param 0 (freq) → 880.0
    clap_event_param_value_t ev{};
    ev.header.size     = sizeof(ev);
    ev.header.time     = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type     = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags    = 0;
    ev.param_id        = 0;
    ev.cookie          = nullptr;
    ev.note_id         = -1;
    ev.port_index      = -1;
    ev.channel         = -1;
    ev.key             = -1;
    ev.value           = 880.0;

    clap_input_events_t flush_in{};
    flush_in.ctx  = &ev;
    flush_in.size = [](const clap_input_events_t*) -> uint32_t { return 1u; };
    flush_in.get  = [](const clap_input_events_t* q, uint32_t i) -> const clap_event_header_t* {
        return i == 0 ? &static_cast<const clap_event_param_value_t*>(q->ctx)->header : nullptr;
    };

    params_ext->flush(plugin, &flush_in, nullptr);

    double value = -1.0;
    REQUIRE(params_ext->get_value(plugin, 0, &value));
    CHECK(value == 880.0);
}

TEST_CASE("wasm_bridge: hot-swap reinitialises DSP to t=0", "[wasm_bridge]") {
    const std::string wasm_path{KAIROS_TEST_WASM_PATH};
    if (wasm_path.empty())
        SKIP("KAIROS_TEST_WASM_PATH not set");

    auto inst =
        kairos::plugin_instance::load("kairos:", wasm_plugin_id(wasm_path), kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 256));
    REQUIRE(inst->start_processing());

    static constexpr uint32_t k_frames = 256;
    std::vector<float>        ch0(k_frames, 0.0f);
    float*                    ch0_ptr = ch0.data();
    clap_audio_buffer_t       out_buf{};
    out_buf.data32        = &ch0_ptr;
    out_buf.channel_count = 1;

    // Block 1: phasor at t=0; first sample == 0.
    auto proc1 = make_process(&out_buf, 1, k_frames);
    REQUIRE(inst->process(proc1) == CLAP_PROCESS_CONTINUE);
    const float first_block_s0 = ch0[0];
    CHECK(first_block_s0 == 0.0f);

    // Block 2: phasor has advanced; first sample != 0.
    auto proc2 = make_process(&out_buf, 1, k_frames);
    REQUIRE(inst->process(proc2) == CLAP_PROCESS_CONTINUE);
    const float second_block_s0 = ch0[0];
    CHECK(second_block_s0 != 0.0f);

    // Hot-swap to the same .wasm — DSP reinitialises to t=0.
    auto swap = inst->hot_swap(wasm_path);
    REQUIRE(swap);

    // Block 3: phasor restarted; first sample == 0 again.
    std::fill(ch0.begin(), ch0.end(), -1.0f);
    auto proc3 = make_process(&out_buf, 1, k_frames);
    REQUIRE(inst->process(proc3) == CLAP_PROCESS_CONTINUE);
    CHECK(ch0[0] == first_block_s0); // same as block 1's first sample (0.0)

    inst->stop_processing();
    inst->deactivate();
}
