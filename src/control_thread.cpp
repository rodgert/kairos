// SPDX-License-Identifier: GPL-2.0-or-later
#include <kairos/control_thread.hpp>
#include <nomos/rt/ipc.hpp>

#include "plugin_discovery.hpp"

#include <edn/parser.hpp>

#include <string>

namespace kairos {

namespace {

std::string edn_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
}

std::string build_plugin_list_edn(const plugin_registry& reg) {
    std::string edn;
    edn += "[";
    bool first = true;
    for (const auto& [id, info] : reg) {
        if (!first) edn += " ";
        first = false;
        edn += "{:id \"";      edn += edn_escape(id);
        edn += "\" :name \"";  edn += edn_escape(info.name);
        edn += "\" :vendor \""; edn += edn_escape(info.vendor);
        edn += "\" :version \""; edn += edn_escape(info.version);
        edn += "\" :path \"";  edn += edn_escape(info.path);
        edn += "\"}";
    }
    edn += "]";
    return edn;
}

} // namespace

control_thread::control_thread(config cfg, nomos::rt::param_queue& queue,
                               nomos::rt::input_event_queue& in_queue)
    : nomos::rt::rt_control_thread(
          nomos::rt::rt_control_thread::config{
              .socket_path   = std::move(cfg.socket_path),
              .db_path       = std::move(cfg.db_path),
              .sched_staging = cfg.sched_staging,
          },
          queue, in_queue),
      kairos_cfg_(std::move(cfg)) {
}

void control_thread::dispatch_extension(int conn_fd, const nomos::rt::ipc::message& msg,
                                        std::optional<nomos::rt::session>& sess) {
    switch (msg.type()) {
    case nomos::rt::ipc::msg_graph_load: {
        if (msg.payload.empty())
            break;
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m = parsed->get<edn::map>();

        plugin_graph g;

        if (const auto* nv = m.find_kw("graph/nodes"); nv && nv->is<edn::vector>()) {
            for (const auto& item : nv->get<edn::vector>().items) {
                if (!item.is<edn::map>())
                    continue;
                const auto& nm       = item.get<edn::map>();
                const auto* id_v     = nm.find_kw("id");
                const auto* plugin_v = nm.find_kw("plugin");
                if (!id_v || !id_v->is<edn::keyword>())
                    continue;
                if (!plugin_v || !plugin_v->is<std::string>())
                    continue;
                edn::map params;
                if (const auto* pv = nm.find_kw("params"); pv && pv->is<edn::map>())
                    params = pv->get<edn::map>();
                g.nodes.push_back(
                    {id_v->get<edn::keyword>(), plugin_v->get<std::string>(), std::move(params)});
            }
        }

        if (const auto* ev = m.find_kw("graph/edges"); ev && ev->is<edn::vector>()) {
            for (const auto& item : ev->get<edn::vector>().items) {
                if (!item.is<edn::vector>())
                    continue;
                const auto& kws = item.get<edn::vector>().items;
                if (kws.size() < 4)
                    continue;
                if (!kws[0].is<edn::keyword>() || !kws[1].is<edn::keyword>() ||
                    !kws[2].is<edn::keyword>() || !kws[3].is<edn::keyword>())
                    continue;
                g.edges.push_back({kws[0].get<edn::keyword>(), kws[1].get<edn::keyword>(),
                                   kws[2].get<edn::keyword>(), kws[3].get<edn::keyword>()});
            }
        }

        if (!kairos_cfg_.host)
            break;
        {
            auto mgr = std::make_unique<plugin_graph_manager>();
            if (mgr->load(g, kairos_cfg_.plugins, kairos_cfg_.host)) {
                mgr->set_audio_config(kairos_cfg_.sample_rate, kairos_cfg_.min_frames,
                                      kairos_cfg_.max_frames);
                mgr->start_processing_all();
                graph_.store(std::move(mgr));
            }
        }
        break;
    }

    case nomos::rt::ipc::msg_graph_reset:
        graph_.store(std::make_unique<plugin_graph_manager>());
        break;

    case nomos::rt::ipc::msg_plugin_list_req: {
        std::vector<std::string> extra_paths;
        if (!msg.payload.empty()) {
            const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                        msg.payload.size()};
            auto parsed = edn::parse(text);
            if (parsed && parsed->is<edn::map>()) {
                const auto& m = parsed->get<edn::map>();
                if (const auto* pv = m.find_kw("extra-paths");
                    pv && pv->is<edn::vector>()) {
                    for (const auto& item : pv->get<edn::vector>().items)
                        if (item.is<std::string>())
                            extra_paths.push_back(item.get<std::string>());
                }
            }
        }
        const auto  reg = discover_plugins(extra_paths);
        const auto  edn = build_plugin_list_edn(reg);
        push_frame(nomos::rt::ipc::msg_plugin_list_resp, edn);
        break;
    }

    case nomos::rt::ipc::msg_wasm_hot_swap: {
        if (msg.payload.empty())
            break;
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m      = parsed->get<edn::map>();
        const auto* id_v   = m.find_kw("node-id");
        const auto* path_v = m.find_kw("wasm-path");
        if (!id_v || !id_v->is<edn::keyword>())
            break;
        if (!path_v || !path_v->is<std::string>())
            break;
        // unsafe_get() is safe here: the control thread is the sole writer of graph_,
        // so there is no concurrent store(). We must NOT hold a graph_.read() guard
        // while calling hot_swap_node — synchronize_rcu() inside would deadlock.
        auto* mgr = graph_.unsafe_get();
        if (!mgr)
            break;
        mgr->hot_swap_node(id_v->get<edn::keyword>(), path_v->get<std::string>());
        break;
    }

    default:
        break;
    }

    (void)conn_fd;
    (void)sess;
}

} // namespace kairos
