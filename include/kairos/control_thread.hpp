// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/input_event.hpp>
#include <kairos/ipc_channel.hpp>
#include <kairos/param_event.hpp>
#include <kairos/plugin_graph_manager.hpp>
#include <kairos/rcu.hpp>
#include <kairos/session.hpp>
#include <kairos/spsc_queue.hpp>

#include <clap/host.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace kairos {

// Capacity of the param event queue shared with the audio thread.
// Must be a power of two.  512 slots is generous for block-rate param updates.
constexpr std::size_t param_queue_capacity = 512;

using param_queue = spsc_queue<param_event, param_queue_capacity>;

// The control thread:
//   - listens on a Unix domain socket at `socket_path`
//   - accepts one connection at a time from cljseq
//   - reads IPC frames and dispatches on message type:
//       SESSION-OPEN        → opens the txlog session
//       SESSION-CLOSE       → closes the txlog session
//       REGISTER-SOURCE     → forwards to session::register_source()
//       GRAPH-LOAD          → loads a plugin_graph from EDN payload
//       GRAPH-RESET         → tears down the current plugin_graph
//       PARAM-SET           → pushes a param_event onto `param_queue`
//       TX-LOG              → deserialises EDN entry and calls session::emit()
//   - runs until stop() is called
//
// The caller owns the param_queue and passes a reference; the audio thread
// consumes from the same queue.
class control_thread {
  public:
    struct config {
        std::string        socket_path; // Unix domain socket path
        std::string        db_path;     // txlog database path
        plugin_registry    plugins;     // plugin_id → .clap file path
        const clap_host_t* host{nullptr};
        double             sample_rate{48000.0};
        uint32_t           min_frames{64};
        uint32_t           max_frames{256};
    };

    explicit control_thread(config cfg, param_queue& queue, input_event_queue& in_queue);
    ~control_thread();

    control_thread(const control_thread&)            = delete;
    control_thread& operator=(const control_thread&) = delete;

    void start();
    void stop();
    bool running() const noexcept;

    rcu_managed<plugin_graph_manager>& graph() noexcept { return graph_; }

  private:
    void run();
    void handle_connection(int conn_fd);
    void dispatch_message(int conn_fd, const ipc::message& msg, std::optional<session>& sess);

    config                            cfg_;
    param_queue&                      queue_;
    input_event_queue&                in_queue_;
    rcu_managed<plugin_graph_manager> graph_{std::make_unique<plugin_graph_manager>()};

    int               listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace kairos
