// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/graph.hpp>
#include <kairos/plugin_instance.hpp>

#include <clap/audio-buffer.h>
#include <clap/host.h>
#include <clap/process.h>

#include <cstddef>
#include <vector>

namespace kairos {

// Owns the plugin_instance objects instantiated from a plugin_graph descriptor.
// Not thread-safe — all calls from the audio/process thread after load().
class plugin_graph_manager {
  public:
    plugin_graph_manager() = default;
    ~plugin_graph_manager() { stop_processing_all(); }

    plugin_graph_manager(plugin_graph_manager&&)            = default;
    plugin_graph_manager& operator=(plugin_graph_manager&&) = default;

    plugin_graph_manager(const plugin_graph_manager&)            = delete;
    plugin_graph_manager& operator=(const plugin_graph_manager&) = delete;

    // Instantiate all plugins in the graph.  Resets any previous graph first.
    // Fails on the first plugin that cannot be loaded.
    result<std::monostate, plugin_error>
    load(const plugin_graph& graph, const plugin_registry& registry, const clap_host_t* host);

    // Store the audio parameters used to activate nodes and wire audio buffers.
    // Must be called before start_processing_all().
    void set_audio_config(double sample_rate, uint32_t min_frames, uint32_t max_frames) noexcept;

    // Activate all loaded instances.
    result<std::monostate, plugin_error> activate(double sample_rate, uint32_t min_frames,
                                                  uint32_t max_frames);

    // Activate any initialised nodes then transition them to processing state.
    result<std::monostate, plugin_error> start_processing_all();

    // Drive one block through all processing nodes in topological order.
    // proc supplies: frames_count, steady_time, transport, in_events, out_events.
    // Audio I/O uses per-node buffers wired during set_audio_config().
    void process_all(const clap_process_t& proc) noexcept;

    // Inject hardware audio input into root-node input buffers before process_all().
    // channels[c] points to nFrames samples; ch_count may be less than the node
    // expects (extra channels receive silence).
    void set_hw_input_buffer(const float* const* channels, uint32_t ch_count,
                             uint32_t frames) noexcept;

    // Sum terminal-node output buffers into the hardware output after process_all().
    // channels[c] must be a writable buffer of nFrames samples.
    void collect_hw_output(float** channels, uint32_t ch_count, uint32_t frames) noexcept;

    // Stop processing on all nodes that are in the processing state.
    void stop_processing_all() noexcept;

    // Deactivate and destroy all instances.
    void reset();

    std::size_t node_count() const noexcept;
    bool        has_node(const edn::keyword& id) const noexcept;

    // Gapless hot-swap of the WASM module for a single node.
    // Delegates to plugin_instance::hot_swap() which uses the kairos RCU extension.
    // Returns node_not_found if no node has the given id.
    result<std::monostate, plugin_error> hot_swap_node(const edn::keyword& id,
                                                       const std::string&  new_wasm_path);

  private:
    struct node_entry {
        edn::keyword    id;
        plugin_instance inst;
    };

    // Per-node audio bookkeeping (parallel to nodes_).
    struct node_audio {
        // Owned output storage: [port][channel] → block-sized float buffer.
        std::vector<std::vector<std::vector<float>>> out_channel_data;
        // data32 backing for outputs: [port] → float* per channel.
        std::vector<std::vector<float*>> out_channel_ptrs;
        // CLAP output buffers passed to plugin.
        std::vector<clap_audio_buffer_t> audio_outs;

        // data32 backing for inputs: [port] → float* per channel (borrowed).
        std::vector<std::vector<float*>> in_channel_ptrs;
        // CLAP input buffers passed to plugin.
        std::vector<clap_audio_buffer_t> audio_ins;

        // Per-node process template — shared event fields overwritten each block.
        clap_process_t proc_tmpl{};

        bool is_hw_input{false};  // injects hardware audio into first input port
        bool is_hw_output{false}; // contributes to hardware output bus
    };

    void wire_audio(uint32_t block_size);

    std::size_t     find_node_idx(const edn::keyword& id) const noexcept;
    static uint32_t parse_port_index(const edn::keyword& port_kw) noexcept;

    std::vector<node_entry>  nodes_;
    std::vector<node_audio>  audio_;
    std::vector<std::size_t> topo_order_;  // indices into nodes_, processing order
    std::vector<float>       silence_buf_; // max_channels * block_size zeros

    plugin_graph saved_graph_;

    double   audio_sr_{48000.0};
    uint32_t audio_min_f_{64};
    uint32_t audio_max_f_{64};
    bool     audio_config_set_{false};
};

} // namespace kairos
