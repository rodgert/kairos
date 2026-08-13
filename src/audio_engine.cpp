// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio_engine.hpp"

#include <kairos/clap_kairos_tap_bus.h>

#include <clap/events.h>
#include <clap/process.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace kairos {

using namespace nomos::rt;

// Target tap telemetry rate (Hz). Snapshots are throttled to roughly this on the
// audio thread. See §25 config tap-push-rate-hz (design-intent; hardcoded until
// the tap bridge is wired to the live config registry).
static constexpr double k_tap_push_hz = 30.0;

audio_engine::audio_engine(config cfg, rcu_managed<plugin_graph_manager>& graph, link_peer& link,
                           midi_event_queue& midi_out_queue, input_event_queue& ipc_in_queue,
                           input_event_queue& hw_midi_in_queue, input_event_queue& osc_in_queue,
                           event_scheduler* sched, tap_snapshot_queue* tap_out)
    : cfg_(cfg), graph_(graph), link_(link), midi_out_queue_(midi_out_queue),
      ipc_in_queue_(ipc_in_queue), hw_midi_in_queue_(hw_midi_in_queue), osc_in_queue_(osc_in_queue),
      sched_(sched), tap_out_(tap_out) {
    // Snapshot every N blocks so the tap push rate ≈ k_tap_push_hz.
    const double blocks_per_sec =
        cfg_.sample_rate / static_cast<double>(cfg_.buffer_frames > 0 ? cfg_.buffer_frames : 1);
    tap_block_divisor_ = static_cast<std::uint32_t>(std::max(1.0, blocks_per_sec / k_tap_push_hz));
}

audio_engine::~audio_engine() {
    stop();
}

bool audio_engine::start() {
    // Configure whatever graph is currently loaded (usually empty at startup).
    // Graphs loaded later via IPC are configured by the control thread before
    // being stored, so they arrive already activated.
    {
        auto g = graph_.read();
        if (g) {
            g->set_audio_config(cfg_.sample_rate, cfg_.buffer_frames, cfg_.buffer_frames);
            g->start_processing_all();
        }
    }

    audio_device_config dev_cfg{
        .device_id     = cfg_.device_id,
        .out_channels  = cfg_.out_channels,
        .in_channels   = cfg_.in_channels,
        .sample_rate   = cfg_.sample_rate,
        .buffer_frames = cfg_.buffer_frames,
    };

    if (!device_.open(dev_cfg,
                      [this](float** out, const float* const* in, uint32_t o, uint32_t i,
                             uint32_t n, double t) { on_audio_block(out, in, o, i, n, t); }))
        return false;

    return device_.start();
}

void audio_engine::stop() {
    device_.stop();
    device_.close();
    // stop_processing_all() fires from ~plugin_graph_manager() when the graph
    // is retired via call_rcu or destroyed at control_thread teardown.
}

bool audio_engine::running() const noexcept {
    return device_.is_running();
}

void audio_engine::on_audio_block(float** out_channels, const float* const* in_channels,
                                  uint32_t out_ch, uint32_t in_ch, uint32_t nframes,
                                  double /*stream_time*/) {
    const auto t0 = link_.now();

    // Advance time identity on beat transitions.
    const double beat = link_.beat_at_time(t0);
    time_.apply_if_ready(beat);

    // Tick the event scheduler before draining ipc_in_queue so that
    // beat-tagged events land in ipc_in_queue at the right block.
    if (sched_)
        sched_->tick(beat, [&](const clap_event_union& ev) { ipc_in_queue_.push(ev); });

    // Drain all input event sources.
    in_buf_.clear();
    in_buf_.drain(ipc_in_queue_);
    in_buf_.drain(hw_midi_in_queue_);
    in_buf_.drain(osc_in_queue_);

    // Build CLAP transport from Link session state.
    clap_event_transport_t transport{};
    transport.header.size     = sizeof(clap_event_transport_t);
    transport.header.time     = 0;
    transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport.header.type     = CLAP_EVENT_TRANSPORT;
    transport.header.flags    = 0;
    transport.flags =
        CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
    transport.tempo          = link_.tempo();
    transport.tempo_inc      = 0.0;
    transport.song_pos_beats = static_cast<clap_beattime>(beat * CLAP_BEATTIME_FACTOR);
    transport.bar_start      = 0;
    transport.bar_number     = 0;
    transport.tsig_num       = 4;
    transport.tsig_denom     = 4;

    clap_process_t proc{};
    proc.steady_time  = t0.count() * 1000; // µs → ns
    proc.frames_count = nframes;
    proc.transport    = &transport;
    proc.in_events    = in_buf_.input_events();
    proc.out_events   = collector_.output_events();

    {
        auto g = graph_.read();
        if (g) {
            if (in_ch > 0 && in_channels)
                g->set_hw_input_buffer(in_channels, in_ch, nframes);
            g->process_all(proc);
            if (out_ch > 0)
                g->collect_hw_output(out_channels, out_ch, nframes);

            // Tap telemetry — snapshot the tap bus (throttled) while the frame is
            // fresh (get_tap_frame is valid only until the next process()). Fixed-
            // size copy, no alloc, no strings; the names are joined off the audio
            // thread by the telemetry drain. Drops if the queue is full.
            if (tap_out_ && ++tap_block_counter_ >= tap_block_divisor_) {
                tap_block_counter_         = 0;
                auto [tap_plugin, tap_ext] = g->find_extension(CLAP_EXT_KAIROS_TAP_BUS);
                if (tap_plugin && tap_ext) {
                    const auto*  tb     = static_cast<const clap_plugin_tap_bus_t*>(tap_ext);
                    const auto*  schema = tb->get_schema(tap_plugin);
                    uint32_t     n      = 0;
                    const float* vals   = tb->get_tap_frame(tap_plugin, &n);
                    if (schema && vals && n > 0) {
                        tap_snapshot snap;
                        snap.epoch = schema->epoch;
                        snap.count = std::min<uint32_t>(n, k_max_tap_values);
                        for (uint32_t i = 0; i < snap.count; ++i)
                            snap.values[i] = vals[i];
                        tap_out_->push(snap);
                    }
                }
            }
        } else {
            if (out_ch > 0)
                for (uint32_t c = 0; c < out_ch; ++c)
                    std::memset(out_channels[c], 0, nframes * sizeof(float));
            // No graph loaded: pass note/MIDI events directly to output so
            // IPC notes reach hardware without requiring an explicit graph load.
            const clap_input_events_t*  in_ev = in_buf_.input_events();
            const clap_output_events_t* out   = collector_.output_events();
            const uint32_t              n     = in_ev->size(in_ev);
            for (uint32_t i = 0; i < n; ++i) {
                const clap_event_header_t* hdr = in_ev->get(in_ev, i);
                if (hdr)
                    out->try_push(out, hdr);
            }
        }
    }

    // Flush MIDI output events.
    if (collector_.count() > 0) {
        collector_.flush_to(midi_out_queue_);
        collector_.reset();
    }
}

} // namespace kairos
