// SPDX-License-Identifier: GPL-2.0-or-later
#include <kairos/control_thread.hpp>
#include <kairos/ipc.hpp>

#include <edn/parser.hpp>

namespace kairos {

control_thread::control_thread(config cfg, param_queue& queue, input_event_queue& in_queue)
    : rt_control_thread(
          rt_control_thread::config{
              .socket_path = std::move(cfg.socket_path),
              .db_path     = std::move(cfg.db_path),
          },
          queue, in_queue),
      kairos_cfg_(std::move(cfg)) {
}

void control_thread::dispatch_extension(int conn_fd, const ipc::message& msg,
                                        std::optional<session>& sess) {
    switch (msg.type()) {
    case ipc::msg_graph_load: {
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

    case ipc::msg_graph_reset:
        graph_.store(std::make_unique<plugin_graph_manager>());
        break;

    case ipc::msg_wasm_hot_swap: {
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
