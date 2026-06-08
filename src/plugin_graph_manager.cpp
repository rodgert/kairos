// SPDX-License-Identifier: LGPL-2.1-or-later
#include <kairos/plugin_graph_manager.hpp>

#include <algorithm>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace kairos {

using namespace nomos::rt;

namespace {

    // Silence channel count ceiling: support up to 8 channels for any single port.
    constexpr uint32_t k_max_silence_channels = 8;

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::size_t plugin_graph_manager::find_node_idx(const edn::keyword& id) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].id == id)
            return i;
    }
    return std::size_t(-1);
}

uint32_t plugin_graph_manager::parse_port_index(const edn::keyword& port_kw) noexcept {
    const std::string_view name = port_kw.name;
    const auto             dash = name.rfind('-');
    if (dash == std::string_view::npos)
        return 0;
    uint32_t idx = 0;
    for (const char c : name.substr(dash + 1)) {
        if (c < '0' || c > '9')
            return 0;
        idx = idx * 10 + static_cast<uint32_t>(c - '0');
    }
    return idx;
}

// ---------------------------------------------------------------------------
// wire_audio — allocate per-node buffers and resolve graph connections
// ---------------------------------------------------------------------------

void plugin_graph_manager::wire_audio(uint32_t block_size) {
    const std::size_t n = nodes_.size();
    audio_.resize(n);

    // Silence buffer: block_size samples per channel, zero-initialised.
    silence_buf_.assign(k_max_silence_channels * block_size, 0.0f);

    // Track in-degree (number of audio edges pointing to each node) for topo sort.
    std::vector<uint32_t> in_degree(n, 0);
    // Adjacency list for topo sort: from_idx → list of to_idx.
    std::vector<std::vector<std::size_t>> adj(n);

    // Step 1: allocate output buffers for every node.
    for (std::size_t i = 0; i < n; ++i) {
        const auto& inst = nodes_[i].inst;
        auto&       na   = audio_[i];

        const uint32_t n_out = static_cast<uint32_t>(inst.out_ports().size());
        const uint32_t n_in  = static_cast<uint32_t>(inst.in_ports().size());

        na.out_channel_data.resize(n_out);
        na.out_channel_ptrs.resize(n_out);
        na.audio_outs.resize(n_out);

        for (uint32_t p = 0; p < n_out; ++p) {
            const uint32_t ch = inst.out_ports()[p].channel_count;
            na.out_channel_data[p].resize(ch);
            na.out_channel_ptrs[p].resize(ch);
            for (uint32_t c = 0; c < ch; ++c) {
                na.out_channel_data[p][c].assign(block_size, 0.0f);
                na.out_channel_ptrs[p][c] = na.out_channel_data[p][c].data();
            }
            auto& ab         = na.audio_outs[p];
            ab.data32        = na.out_channel_ptrs[p].data();
            ab.data64        = nullptr;
            ab.channel_count = ch;
            ab.latency       = 0;
            ab.constant_mask = 0;
        }

        // Default: all input channels point to silence.
        na.in_channel_ptrs.resize(n_in);
        na.audio_ins.resize(n_in);

        for (uint32_t p = 0; p < n_in; ++p) {
            const uint32_t ch = inst.in_ports()[p].channel_count;
            na.in_channel_ptrs[p].resize(ch);
            for (uint32_t c = 0; c < ch; ++c)
                na.in_channel_ptrs[p][c] = silence_buf_.data();
            auto& ab         = na.audio_ins[p];
            ab.data32        = na.in_channel_ptrs[p].data();
            ab.data64        = nullptr;
            ab.channel_count = ch;
            ab.latency       = 0;
            ab.constant_mask = 0;
        }

        // Build process template for this node.
        clap_process_t& pt     = na.proc_tmpl;
        pt                     = {};
        pt.audio_inputs_count  = n_in;
        pt.audio_outputs_count = n_out;
        pt.audio_inputs        = na.audio_ins.empty() ? nullptr : na.audio_ins.data();
        pt.audio_outputs       = na.audio_outs.empty() ? nullptr : na.audio_outs.data();
    }

    // Step 2: wire edges — connect upstream output to downstream input.
    for (const auto& edge : saved_graph_.edges) {
        // Edge to "host" marks the from_node as a hardware output provider.
        if (edge.to_node == edn::keyword{"host"}) {
            const std::size_t from_idx = find_node_idx(edge.from_node);
            if (from_idx != std::size_t(-1))
                audio_[from_idx].is_hw_output = true;
            continue;
        }

        const std::size_t from_idx = find_node_idx(edge.from_node);
        const std::size_t to_idx   = find_node_idx(edge.to_node);
        if (from_idx == std::size_t(-1) || to_idx == std::size_t(-1))
            continue;

        const uint32_t from_port = parse_port_index(edge.from_port);
        const uint32_t to_port   = parse_port_index(edge.to_port);

        auto& from_na = audio_[from_idx];
        auto& to_na   = audio_[to_idx];

        if (from_port >= from_na.out_channel_ptrs.size())
            continue;
        if (to_port >= to_na.in_channel_ptrs.size())
            continue;

        // Wire channel-by-channel; if counts differ, extra channels stay silent.
        const uint32_t ch =
            std::min(static_cast<uint32_t>(from_na.out_channel_ptrs[from_port].size()),
                     static_cast<uint32_t>(to_na.in_channel_ptrs[to_port].size()));
        for (uint32_t c = 0; c < ch; ++c)
            to_na.in_channel_ptrs[to_port][c] = from_na.out_channel_ptrs[from_port][c];

        adj[from_idx].push_back(to_idx);
        ++in_degree[to_idx];
    }

    // Step 3: Kahn's topological sort.
    topo_order_.clear();
    topo_order_.reserve(n);
    std::queue<std::size_t> q;
    for (std::size_t i = 0; i < n; ++i) {
        if (in_degree[i] == 0)
            q.push(i);
    }
    while (!q.empty()) {
        const std::size_t cur = q.front();
        q.pop();
        topo_order_.push_back(cur);
        for (const std::size_t nxt : adj[cur]) {
            if (--in_degree[nxt] == 0)
                q.push(nxt);
        }
    }
    // Cycle detection: if not all nodes were added, fall back to natural order.
    if (topo_order_.size() != n) {
        topo_order_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            topo_order_[i] = i;
    }

    // Step 4: nodes with no incoming audio edges are hw_input candidates.
    //         Only mark them if the graph actually has audio edges.
    if (!saved_graph_.edges.empty()) {
        std::vector<bool> has_audio_in(n, false);
        for (const auto& edge : saved_graph_.edges) {
            if (edge.to_node == edn::keyword{"host"})
                continue;
            const std::size_t to_idx = find_node_idx(edge.to_node);
            if (to_idx != std::size_t(-1))
                has_audio_in[to_idx] = true;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!has_audio_in[i] && !audio_[i].audio_ins.empty())
                audio_[i].is_hw_input = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

result<std::monostate, plugin_error> plugin_graph_manager::load(const plugin_graph&    graph,
                                                                const plugin_registry& registry,
                                                                const clap_host_t*     host) {
    reset();
    nodes_.reserve(graph.nodes.size());
    audio_.reserve(graph.nodes.size());

    for (const auto& node : graph.nodes) {
        auto it = registry.find(node.plugin);
        if (it == registry.end())
            return unexpected<plugin_error>{plugin_error::load_failed};

        auto inst = plugin_instance::load(it->second.path, node.plugin, host);
        if (!inst)
            return unexpected<plugin_error>{inst.error()};

        nodes_.push_back({.id = node.id, .inst = std::move(*inst)});
    }

    saved_graph_ = graph;

    if (audio_config_set_)
        wire_audio(audio_max_f_);

    return std::monostate{};
}

result<std::monostate, plugin_error>
plugin_graph_manager::activate(double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    for (auto& e : nodes_) {
        auto r = e.inst.activate(sample_rate, min_frames, max_frames);
        if (!r)
            return r;
    }
    return std::monostate{};
}

void plugin_graph_manager::set_audio_config(double sr, uint32_t min_f, uint32_t max_f) noexcept {
    audio_sr_         = sr;
    audio_min_f_      = min_f;
    audio_max_f_      = max_f;
    audio_config_set_ = true;

    if (!nodes_.empty())
        wire_audio(max_f);
}

result<std::monostate, plugin_error> plugin_graph_manager::start_processing_all() {
    for (auto& e : nodes_) {
        if (audio_config_set_ && e.inst.current_state() == plugin_instance::state::initialized) {
            auto r = e.inst.activate(audio_sr_, audio_min_f_, audio_max_f_);
            if (!r)
                return r;
        }
        if (e.inst.current_state() == plugin_instance::state::activated) {
            auto r = e.inst.start_processing();
            if (!r)
                return r;
        }
    }
    return std::monostate{};
}

void plugin_graph_manager::process_all(const clap_process_t& proc) noexcept {
    for (const std::size_t idx : topo_order_) {
        auto& e  = nodes_[idx];
        auto& na = audio_[idx];

        // Lazily promote nodes (hot-reload or post-load config).
        if (audio_config_set_ && e.inst.current_state() == plugin_instance::state::initialized)
            e.inst.activate(audio_sr_, audio_min_f_, audio_max_f_);
        if (e.inst.current_state() == plugin_instance::state::activated)
            e.inst.start_processing();
        if (e.inst.current_state() != plugin_instance::state::processing)
            continue;

        // Copy shared block fields into this node's process template.
        na.proc_tmpl.steady_time  = proc.steady_time;
        na.proc_tmpl.frames_count = proc.frames_count;
        na.proc_tmpl.transport    = proc.transport;
        na.proc_tmpl.in_events    = proc.in_events;
        na.proc_tmpl.out_events   = proc.out_events;

        e.inst.process(na.proc_tmpl);
    }

    // Headless path: if wire_audio hasn't run (no audio edges, no config yet),
    // topo_order_ may be empty while nodes_ is not.
    if (topo_order_.empty() && !nodes_.empty()) {
        for (auto& e : nodes_) {
            if (audio_config_set_ && e.inst.current_state() == plugin_instance::state::initialized)
                e.inst.activate(audio_sr_, audio_min_f_, audio_max_f_);
            if (e.inst.current_state() == plugin_instance::state::activated)
                e.inst.start_processing();
            if (e.inst.current_state() == plugin_instance::state::processing)
                e.inst.process(proc);
        }
    }
}

void plugin_graph_manager::set_hw_input_buffer(const float* const* channels, uint32_t ch_count,
                                               uint32_t /*frames*/) noexcept {
    for (std::size_t i = 0; i < audio_.size(); ++i) {
        auto& na = audio_[i];
        if (!na.is_hw_input || na.in_channel_ptrs.empty())
            continue;
        auto&          port_ptrs = na.in_channel_ptrs[0];
        const uint32_t ch        = std::min(ch_count, static_cast<uint32_t>(port_ptrs.size()));
        for (uint32_t c = 0; c < ch; ++c)
            port_ptrs[c] = const_cast<float*>(channels[c]);
    }
}

void plugin_graph_manager::collect_hw_output(float** channels, uint32_t ch_count,
                                             uint32_t frames) noexcept {
    bool first = true;
    for (std::size_t i = 0; i < audio_.size(); ++i) {
        const auto& na = audio_[i];
        if (!na.is_hw_output || na.out_channel_ptrs.empty())
            continue;
        if (na.out_channel_ptrs[0].empty())
            continue;

        const uint32_t ch =
            std::min(ch_count, static_cast<uint32_t>(na.out_channel_ptrs[0].size()));
        if (first) {
            // Copy first contributing node.
            for (uint32_t c = 0; c < ch; ++c)
                std::memcpy(channels[c], na.out_channel_ptrs[0][c], frames * sizeof(float));
            // Zero remaining hw channels if node has fewer.
            for (uint32_t c = ch; c < ch_count; ++c)
                std::memset(channels[c], 0, frames * sizeof(float));
            first = false;
        } else {
            // Accumulate additional contributing nodes.
            for (uint32_t c = 0; c < ch; ++c) {
                float*       dst = channels[c];
                const float* src = na.out_channel_ptrs[0][c];
                for (uint32_t f = 0; f < frames; ++f)
                    dst[f] += src[f];
            }
        }
    }
    // No hw_output nodes: zero the output buffers.
    if (first) {
        for (uint32_t c = 0; c < ch_count; ++c)
            std::memset(channels[c], 0, frames * sizeof(float));
    }
}

void plugin_graph_manager::stop_processing_all() noexcept {
    for (auto& e : nodes_)
        e.inst.stop_processing();
}

void plugin_graph_manager::reset() {
    nodes_.clear();
    audio_.clear();
    topo_order_.clear();
    silence_buf_.clear();
    saved_graph_ = {};
}

std::size_t plugin_graph_manager::node_count() const noexcept {
    return nodes_.size();
}

bool plugin_graph_manager::has_node(const edn::keyword& id) const noexcept {
    return find_node_idx(id) != std::size_t(-1);
}

result<std::monostate, plugin_error>
plugin_graph_manager::hot_swap_node(const edn::keyword& id, const std::string& new_wasm_path,
                                    const std::string& old_wasm_path) {
    const std::size_t idx = find_node_idx(id);
    if (idx == std::size_t(-1))
        return unexpected<plugin_error>{plugin_error::node_not_found};
    return nodes_[idx].inst.hot_swap(new_wasm_path, old_wasm_path);
}

} // namespace kairos
