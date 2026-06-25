// SPDX-License-Identifier: GPL-2.0-or-later
#include "process_thread.hpp"

#include <clap/process.h>

#include <chrono>
#include <thread>

namespace kairos {

using namespace nomos::rt;

process_thread::process_thread(config cfg, rcu_managed<plugin_graph_manager>& graph,
                               link_peer& link, midi_event_queue& midi_out_queue,
                               input_event_queue& ipc_in_queue, input_event_queue& hw_midi_in_queue,
                               input_event_queue& osc_in_queue, event_scheduler* sched)
    : cfg_(cfg), graph_(graph), link_(link), midi_out_queue_(midi_out_queue),
      ipc_in_queue_(ipc_in_queue), hw_midi_in_queue_(hw_midi_in_queue), osc_in_queue_(osc_in_queue),
      sched_(sched) {
}

process_thread::~process_thread() {
    stop();
}

void process_thread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&process_thread::run, this);
}

void process_thread::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;
    if (thread_.joinable())
        thread_.join();
}

bool process_thread::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void process_thread::run() {
    const std::chrono::microseconds block_us{
        static_cast<int64_t>(cfg_.block_size * 1'000'000.0 / cfg_.sample_rate + 0.5)};

    // Configure the initial graph (usually empty at startup).
    // Graphs loaded later via IPC are configured by the control thread.
    {
        auto g = graph_.read();
        if (g) {
            g->set_audio_config(cfg_.sample_rate, cfg_.block_size, cfg_.block_size);
            g->start_processing_all();
        }
    }

    while (running_.load(std::memory_order_acquire)) {
        const auto t0 = link_.now();

        const double beat = link_.beat_at_time(t0);
        time_.apply_if_ready(beat);

        // Tick the event scheduler before draining ipc_in_queue so that
        // beat-tagged events land in ipc_in_queue at the right block.
        if (sched_)
            sched_->tick(beat, [&](const clap_event_union& ev) { ipc_in_queue_.push(ev); });

        in_buf_.clear();
        in_buf_.drain(ipc_in_queue_);
        in_buf_.drain(hw_midi_in_queue_);
        in_buf_.drain(osc_in_queue_);

        const std::size_t in_count = in_buf_.input_events()->size(in_buf_.input_events());

        clap_process_t proc{};
        proc.steady_time         = t0.count() * 1000; // µs → ns
        proc.frames_count        = cfg_.block_size;
        proc.transport           = nullptr;
        proc.audio_inputs        = nullptr;
        proc.audio_outputs       = nullptr;
        proc.audio_inputs_count  = 0;
        proc.audio_outputs_count = 0;
        proc.in_events           = in_buf_.input_events();
        proc.out_events          = collector_.output_events();

        {
            auto g = graph_.read();
            if (g && g->node_count() > 0) {
                g->process_all(proc);
            } else if (in_count > 0) {
                // No plugin graph loaded: pass note/MIDI events directly to output.
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

        if (collector_.count() > 0) {
            collector_.flush_to(midi_out_queue_);
            collector_.reset();
        }

        const auto elapsed = link_.now() - t0;
        if (elapsed < block_us)
            std::this_thread::sleep_for(block_us - elapsed);
    }
    // stop_processing_all() fires from ~plugin_graph_manager() at teardown.
}

} // namespace kairos
