// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/plugin_graph_manager.hpp>
#include <kairos/rcu.hpp>
#include <kairos/rt_control_thread.hpp>

#include <clap/host.h>

#include <optional>
#include <string>

namespace kairos {

// CLAP-aware control thread.
//
// Extends rt_control_thread with the plugin graph management messages:
//   GRAPH-LOAD     → loads a plugin_graph from EDN payload
//   GRAPH-RESET    → tears down the current plugin_graph
//   WASM-HOT-SWAP  → gapless hot-swap of a WASM DSP node (KAIROS_WASM_BRIDGE only)
//
// All other message types are handled by rt_control_thread.
class control_thread : public rt_control_thread {
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
    ~control_thread() override = default;

    control_thread(const control_thread&)            = delete;
    control_thread& operator=(const control_thread&) = delete;

    rcu_managed<plugin_graph_manager>& graph() noexcept { return graph_; }

  protected:
    void dispatch_extension(int conn_fd, const ipc::message& msg,
                            std::optional<session>& sess) override;

  private:
    config                            kairos_cfg_;
    rcu_managed<plugin_graph_manager> graph_{std::make_unique<plugin_graph_manager>()};
};

} // namespace kairos
