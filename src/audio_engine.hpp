// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "audio_device.hpp"
#include "event_collector.hpp"
#include "input_event_buffer.hpp"
#include "link_peer.hpp"
#include "tap_snapshot.hpp"

#include <kairos/plugin_graph_manager.hpp>
#include <nomos/rt/event_scheduler.hpp>
#include <nomos/rt/input_event.hpp>
#include <nomos/rt/rcu.hpp>
#include <nomos/rt/time_identity.hpp>

#include <cstdint>

namespace kairos {

// Audio-callback-driven CLAP process loop.
//
// Replaces process_thread when a real audio device is available.
// The RtAudio callback drives the block cadence, so there is no timer sleep.
// Link beat position uses captureAudioSessionState to stay locked to the
// audio device wordclock.
class audio_engine {
  public:
    struct config {
        double       sample_rate{48000.0};
        uint32_t     buffer_frames{256};
        uint32_t     out_channels{2};
        uint32_t     in_channels{0};
        unsigned int device_id{0}; // 0 = default
    };

    audio_engine(config cfg, nomos::rt::rcu_managed<plugin_graph_manager>& graph,
                 nomos::rt::link_peer& link, nomos::rt::midi_event_queue& midi_out_queue,
                 nomos::rt::input_event_queue& ipc_in_queue,
                 nomos::rt::input_event_queue& hw_midi_in_queue,
                 nomos::rt::input_event_queue& osc_in_queue,
                 nomos::rt::event_scheduler*   sched   = nullptr,
                 tap_snapshot_queue*           tap_out = nullptr);
    ~audio_engine();

    audio_engine(const audio_engine&)            = delete;
    audio_engine& operator=(const audio_engine&) = delete;

    bool start();
    void stop();
    bool running() const noexcept;

  private:
    void on_audio_block(float** out_channels, const float* const* in_channels, uint32_t out_ch,
                        uint32_t in_ch, uint32_t nframes, double stream_time);

    config                                        cfg_;
    nomos::rt::rcu_managed<plugin_graph_manager>& graph_;
    nomos::rt::link_peer&                         link_;
    nomos::rt::midi_event_queue&                  midi_out_queue_;
    nomos::rt::input_event_queue&                 ipc_in_queue_;
    nomos::rt::input_event_queue&                 hw_midi_in_queue_;
    nomos::rt::input_event_queue&                 osc_in_queue_;
    nomos::rt::event_collector                    collector_;
    nomos::rt::input_event_buffer                 in_buf_;
    nomos::rt::time_identity                      time_;
    nomos::rt::audio_device                       device_;
    nomos::rt::event_scheduler*                   sched_{nullptr};

    // Tap telemetry: snapshot the tap bus on the audio thread into tap_out_ every
    // tap_block_divisor_ blocks (≈ tap-push-rate-hz). Null = no tap draining.
    tap_snapshot_queue* tap_out_{nullptr};
    std::uint32_t       tap_block_divisor_{1}; // blocks between snapshots
    std::uint32_t       tap_block_counter_{0};
};

} // namespace kairos
