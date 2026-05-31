// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/plugin_graph_manager.hpp>
#include <nomos/rt/rcu.hpp>
#include <nomos/rt/rt_control_thread.hpp>

#include <clap/host.h>

#include <optional>
#include <string>

namespace nomos::rt { class link_peer; }

namespace kairos {

// CLAP-aware control thread.
//
// Extends nomos::rt::rt_control_thread with the plugin graph management messages:
//   GRAPH-LOAD     → loads a plugin_graph from EDN payload
//   GRAPH-RESET    → tears down the current plugin_graph
//   WASM-HOT-SWAP  → gapless hot-swap of a WASM DSP node (KAIROS_WASM_BRIDGE only)
//
// All other message types are handled by nomos::rt::rt_control_thread.
class control_thread : public nomos::rt::rt_control_thread {
  public:
    struct config {
        std::string                     socket_path;
        std::string                     db_path;
        nomos::rt::sched_staging_queue* sched_staging{nullptr};
        nomos::rt::link_peer*           link_peer{nullptr};
        plugin_registry                 plugins;
        const clap_host_t*              host{nullptr};
        double                          sample_rate{48000.0};
        uint32_t                        min_frames{64};
        uint32_t                        max_frames{256};
    };

    explicit control_thread(config cfg, nomos::rt::param_queue& queue,
                            nomos::rt::input_event_queue& in_queue);
    ~control_thread() override = default;

    control_thread(const control_thread&)            = delete;
    control_thread& operator=(const control_thread&) = delete;

    nomos::rt::rcu_managed<plugin_graph_manager>& graph() noexcept { return graph_; }

  protected:
    void dispatch_extension(int conn_fd, const nomos::rt::ipc::message& msg,
                            std::optional<nomos::rt::session>& sess) override;

  private:
    config                                       kairos_cfg_;
    nomos::rt::rcu_managed<plugin_graph_manager> graph_{std::make_unique<plugin_graph_manager>()};
};

} // namespace kairos
