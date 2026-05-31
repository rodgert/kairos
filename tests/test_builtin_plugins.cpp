// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <kairos/graph.hpp>
#include <kairos/plugin_graph_manager.hpp>
#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

#include <clap/events.h>
#include <clap/process.h>

#include <vector>

// -------------------------------------------------------------------------
// Minimal in-test event list helpers
// -------------------------------------------------------------------------
namespace {

struct test_in_list {
    std::vector<clap_event_note_t> events;

    static uint32_t CLAP_ABI size(const clap_input_events_t* l) noexcept {
        return static_cast<uint32_t>(reinterpret_cast<const test_in_list*>(l->ctx)->events.size());
    }
    static const clap_event_header_t* CLAP_ABI get(const clap_input_events_t* l,
                                                   uint32_t                   i) noexcept {
        const auto& ev = reinterpret_cast<const test_in_list*>(l->ctx)->events;
        return (i < ev.size()) ? &ev[i].header : nullptr;
    }

    clap_input_events_t vtable() noexcept { return {this, size, get}; }
};

struct test_out_list {
    std::vector<clap_event_note_t> events;

    static bool CLAP_ABI try_push(const clap_output_events_t* l,
                                  const clap_event_header_t*  hdr) noexcept {
        if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_NOTE_OFF) {
            reinterpret_cast<test_out_list*>(l->ctx)->events.push_back(
                *reinterpret_cast<const clap_event_note_t*>(hdr));
        }
        return true;
    }

    clap_output_events_t vtable() noexcept { return {this, try_push}; }
};

clap_event_note_t make_note(uint16_t type, int16_t key, double vel) noexcept {
    clap_event_note_t ev{};
    ev.header.size     = sizeof(clap_event_note_t);
    ev.header.time     = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type     = type;
    ev.header.flags    = 0;
    ev.note_id         = -1;
    ev.port_index      = 0;
    ev.channel         = 0;
    ev.key             = key;
    ev.velocity        = vel;
    return ev;
}

} // namespace

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

TEST_CASE("builtin passthrough: load via kairos: path", "[builtin]") {
    auto result = kairos::plugin_instance::load(
        "kairos:passthrough", "org.cljseq.kairos.midi-passthrough", kairos::kairos_host());

    REQUIRE(result);
    REQUIRE(result->current_state() == kairos::plugin_instance::state::initialized);
    REQUIRE(result->descriptor() != nullptr);
    REQUIRE(std::string{result->descriptor()->id} == "org.cljseq.kairos.midi-passthrough");
}

TEST_CASE("builtin passthrough: full lifecycle", "[builtin]") {
    auto result = kairos::plugin_instance::load(
        "kairos:passthrough", "org.cljseq.kairos.midi-passthrough", kairos::kairos_host());
    REQUIRE(result);

    REQUIRE(result->activate(48000.0, 64, 64));
    REQUIRE(result->current_state() == kairos::plugin_instance::state::activated);

    REQUIRE(result->start_processing());
    REQUIRE(result->current_state() == kairos::plugin_instance::state::processing);

    result->stop_processing();
    REQUIRE(result->current_state() == kairos::plugin_instance::state::activated);
}

TEST_CASE("builtin passthrough: forwards NOTE_ON and NOTE_OFF", "[builtin]") {
    auto inst = kairos::plugin_instance::load(
        "kairos:passthrough", "org.cljseq.kairos.midi-passthrough", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 64, 64));
    REQUIRE(inst->start_processing());

    test_in_list  in;
    test_out_list out;
    in.events.push_back(make_note(CLAP_EVENT_NOTE_ON, 60, 0.8));
    in.events.push_back(make_note(CLAP_EVENT_NOTE_OFF, 60, 0.0));

    auto in_vt  = in.vtable();
    auto out_vt = out.vtable();

    clap_process_t proc{};
    proc.frames_count = 64;
    proc.in_events    = &in_vt;
    proc.out_events   = &out_vt;
    inst->process(proc);

    REQUIRE(out.events.size() == 2);
    REQUIRE(out.events[0].header.type == CLAP_EVENT_NOTE_ON);
    REQUIRE(out.events[0].key == 60);
    REQUIRE(out.events[1].header.type == CLAP_EVENT_NOTE_OFF);
}

TEST_CASE("builtin audio-passthrough: declares stereo in and out ports", "[builtin]") {
    auto result = kairos::plugin_instance::load(
        "kairos:audio-passthrough", "org.cljseq.kairos.audio-passthrough", kairos::kairos_host());

    REQUIRE(result);
    REQUIRE(result->in_ports().size() == 1);
    REQUIRE(result->out_ports().size() == 1);
    REQUIRE(result->in_ports()[0].channel_count == 2);
    REQUIRE(result->out_ports()[0].channel_count == 2);
}

TEST_CASE("builtin audio-passthrough: copies samples through", "[builtin]") {
    auto inst = kairos::plugin_instance::load(
        "kairos:audio-passthrough", "org.cljseq.kairos.audio-passthrough", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 64, 64));
    REQUIRE(inst->start_processing());

    // Build non-interleaved stereo in/out buffers.
    constexpr uint32_t frames = 64;
    std::vector<float> in_l(frames), in_r(frames), out_l(frames, 0.0f), out_r(frames, 0.0f);
    for (uint32_t i = 0; i < frames; ++i) {
        in_l[i] = static_cast<float>(i);
        in_r[i] = static_cast<float>(i) * 0.5f;
    }

    float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};

    clap_audio_buffer_t audio_in{};
    audio_in.data32        = in_ptrs;
    audio_in.channel_count = 2;

    clap_audio_buffer_t audio_out{};
    audio_out.data32        = out_ptrs;
    audio_out.channel_count = 2;

    // Minimal in/out event lists (no events needed for audio passthrough).
    struct null_list {
        static uint32_t CLAP_ABI size(const clap_input_events_t*) noexcept { return 0; }
        static const clap_event_header_t* CLAP_ABI get(const clap_input_events_t*,
                                                       uint32_t) noexcept {
            return nullptr;
        }
        static bool CLAP_ABI try_push(const clap_output_events_t*,
                                      const clap_event_header_t*) noexcept {
            return true;
        }
    };
    clap_input_events_t  in_ev{nullptr, null_list::size, null_list::get};
    clap_output_events_t out_ev{nullptr, null_list::try_push};

    clap_process_t proc{};
    proc.frames_count        = frames;
    proc.audio_inputs_count  = 1;
    proc.audio_outputs_count = 1;
    proc.audio_inputs        = &audio_in;
    proc.audio_outputs       = &audio_out;
    proc.in_events           = &in_ev;
    proc.out_events          = &out_ev;
    inst->process(proc);

    REQUIRE(out_l == in_l);
    REQUIRE(out_r == in_r);
}

TEST_CASE("plugin_graph_manager: passthrough activates via set_audio_config", "[builtin]") {
    kairos::plugin_registry reg{{"org.cljseq.kairos.midi-passthrough",
                                  kairos::plugin_info{.path = "kairos:passthrough"}}};

    kairos::plugin_graph graph{
        .nodes = {{edn::keyword{"test/pt"}, "org.cljseq.kairos.midi-passthrough", {}}},
        .edges = {}};

    kairos::plugin_graph_manager mgr;
    REQUIRE(mgr.load(graph, reg, kairos::kairos_host()));

    mgr.set_audio_config(48000.0, 64, 64);
    REQUIRE(mgr.start_processing_all());

    // After start_processing_all, the node should be processing.
    // We verify by running a block — if the manager doesn't crash it's active.
    test_in_list  in;
    test_out_list out;
    in.events.push_back(make_note(CLAP_EVENT_NOTE_ON, 48, 1.0));
    auto in_vt  = in.vtable();
    auto out_vt = out.vtable();

    clap_process_t proc{};
    proc.frames_count = 64;
    proc.in_events    = &in_vt;
    proc.out_events   = &out_vt;
    mgr.process_all(proc);

    REQUIRE(out.events.size() == 1);
    REQUIRE(out.events[0].key == 48);
}
