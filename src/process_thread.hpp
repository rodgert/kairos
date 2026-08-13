// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "event_collector.hpp"
#include "input_event_buffer.hpp"
#include "link_peer.hpp"
#include "tap_snapshot.hpp"

#include <kairos/plugin_graph_manager.hpp>
#include <nomos/rt/event_scheduler.hpp>
#include <nomos/rt/input_event.hpp>
#include <nomos/rt/rcu.hpp>
#include <nomos/rt/time_identity.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace kairos {

// Timer-driven CLAP process loop.
//
// Runs at block rate (block_size / sample_rate), drains three input queues
// (IPC, hardware MIDI, OSC) into an nomos::rt::input_event_buffer, calls process_all()
// on all graph nodes, translates CLAP output events to midi_out_events, and
// pushes batches into the nomos::rt::midi_event_queue for the MIDI dispatch thread.
//
// No audio I/O — plugins run in headless mode (null audio buffers).
class process_thread {
  public:
    struct config {
        double   sample_rate{48000.0};
        uint32_t block_size{64};
    };

    process_thread(config cfg, nomos::rt::rcu_managed<plugin_graph_manager>& graph,
                   nomos::rt::link_peer& link, nomos::rt::midi_event_queue& midi_out_queue,
                   nomos::rt::input_event_queue& ipc_in_queue,
                   nomos::rt::input_event_queue& hw_midi_in_queue,
                   nomos::rt::input_event_queue& osc_in_queue,
                   nomos::rt::event_scheduler*   sched   = nullptr,
                   tap_snapshot_queue*           tap_out = nullptr);
    ~process_thread();

    process_thread(const process_thread&)            = delete;
    process_thread& operator=(const process_thread&) = delete;

    void start();
    void stop();
    bool running() const noexcept;

  private:
    void run();

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

    nomos::rt::event_scheduler* sched_{nullptr};

    // Tap telemetry — see audio_engine. Null = no tap draining.
    tap_snapshot_queue* tap_out_{nullptr};
    std::uint32_t       tap_block_divisor_{1};
    std::uint32_t       tap_block_counter_{0};

    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace kairos
