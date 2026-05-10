// SPDX-License-Identifier: GPL-2.0-or-later
#include <kairos/control_thread.hpp>
#include <kairos/ipc.hpp>

#include <edn/builtins.hpp>
#include <edn/parser.hpp>
#include <txlog/txlog.hpp>

#include <clap/events.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace {

// Parse the canonical UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into bytes.
edn::uuid parse_uuid(const std::string& s) noexcept {
    edn::uuid u{};
    if (s.size() != 36)
        return u;
    int  bi = 0;
    auto h  = [](char c) -> uint8_t {
        return c <= '9' ? static_cast<uint8_t>(c - '0') : static_cast<uint8_t>((c | 32) - 'a' + 10);
    };
    for (std::size_t i = 0; i < 36 && bi < 16;) {
        if (s[i] == '-') {
            ++i;
            continue;
        }
        u.bytes[bi++] = static_cast<uint8_t>((h(s[i]) << 4) | h(s[i + 1]));
        i += 2;
    }
    return u;
}

} // namespace

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace kairos {

namespace {

    int make_listen_socket(const std::string& path) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        ::unlink(path.c_str()); // remove stale socket

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd, 1) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

} // namespace

control_thread::control_thread(config cfg, param_queue& queue, input_event_queue& in_queue)
    : cfg_(std::move(cfg)), queue_(queue), in_queue_(in_queue) {
}

control_thread::~control_thread() {
    stop();
}

void control_thread::start() {
    listen_fd_ = make_listen_socket(cfg_.socket_path);
    if (listen_fd_ < 0)
        return;

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&control_thread::run, this);
}

void control_thread::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(cfg_.socket_path.c_str());
    }

    if (thread_.joinable())
        thread_.join();
}

bool control_thread::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void control_thread::run() {
    std::optional<session> sess;

    while (running_.load(std::memory_order_acquire)) {
        const int conn_fd = ::accept(listen_fd_, nullptr, nullptr);
        if (conn_fd < 0)
            break;

        handle_connection(conn_fd);
        ::close(conn_fd);
    }

    if (sess)
        sess->close();
}

void control_thread::handle_connection(int conn_fd) {
    std::optional<session> sess;

    while (running_.load(std::memory_order_acquire)) {
        auto result = ipc::read_message(conn_fd);
        if (!result)
            break;
        dispatch_message(conn_fd, *result, sess);
    }
}

void control_thread::dispatch_message(int conn_fd, const ipc::message& msg,
                                      std::optional<session>& sess) {
    switch (msg.type()) {
    case ipc::msg_session_open: {
        sess = session::open(cfg_.db_path);
        break;
    }

    case ipc::msg_session_close: {
        if (sess)
            sess->close();
        sess.reset();
        break;
    }

    case ipc::msg_register_source: {
        if (!sess || msg.payload.empty())
            break;
        // Payload is EDN: {:id :some/keyword :name "..." :description "..."}
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m   = parsed->get<edn::map>();
        const auto* id  = m.find_kw("id");
        const auto* nm  = m.find_kw("name");
        const auto* dsc = m.find_kw("description");
        if (!id || !id->is<edn::keyword>())
            break;
        sess->register_source({
            .id          = id->get<edn::keyword>(),
            .name        = nm && nm->is<std::string>() ? nm->get<std::string>() : "",
            .description = dsc && dsc->is<std::string>() ? dsc->get<std::string>() : "",
        });
        break;
    }

    case ipc::msg_param_set: {
        if (msg.payload.empty())
            break;
        // Payload is EDN: {:path [...] :value <v> :time {:current {...} :pending {...}}}
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m     = parsed->get<edn::map>();
        const auto* path  = m.find_kw("path");
        const auto* value = m.find_kw("value");
        if (!path || !value)
            break;
        // time_identity reconstruction is a stub — full parse added with Link integration.
        queue_.push(param_event{.path = *path, .value = *value, .time = {}});
        break;
    }

    case ipc::msg_note_on:
    case ipc::msg_note_off: {
        if (msg.payload.empty())
            break;
        // Payload: {:port <i> :channel <i> :key <i> :velocity <f> :note-id <i>}
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m = parsed->get<edn::map>();

        auto get_i16 = [&](const char* kw, int16_t def) -> int16_t {
            const auto* v = m.find_kw(kw);
            if (v && v->is<int64_t>())
                return static_cast<int16_t>(v->get<int64_t>());
            return def;
        };
        auto get_dbl = [&](const char* kw, double def) -> double {
            const auto* v = m.find_kw(kw);
            if (v && v->is<double>())
                return v->get<double>();
            if (v && v->is<int64_t>())
                return static_cast<double>(v->get<int64_t>());
            return def;
        };

        clap_event_union ev{};
        ev.note.header.size     = sizeof(clap_event_note_t);
        ev.note.header.time     = 0;
        ev.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.note.header.type =
            (msg.type() == ipc::msg_note_on) ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        ev.note.header.flags = 0;
        ev.note.note_id      = get_i16("note-id", -1);
        ev.note.port_index   = get_i16("port", 0);
        ev.note.channel      = get_i16("channel", 0);
        ev.note.key          = get_i16("key", 60);
        ev.note.velocity     = get_dbl("velocity", 0.0);
        in_queue_.push(ev);
        break;
    }

    case ipc::msg_midi_in: {
        if (msg.payload.empty())
            break;
        // Payload: {:port <i> :data [<b0> <b1> <b2>]}
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m = parsed->get<edn::map>();

        const auto* port_v = m.find_kw("port");
        const auto* data_v = m.find_kw("data");
        if (!data_v || !data_v->is<edn::vector>())
            break;
        const auto& bytes = data_v->get<edn::vector>().items;

        clap_event_union ev{};
        ev.midi.header.size     = sizeof(clap_event_midi_t);
        ev.midi.header.time     = 0;
        ev.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.midi.header.type     = CLAP_EVENT_MIDI;
        ev.midi.header.flags    = 0;
        ev.midi.port_index =
            (port_v && port_v->is<int64_t>()) ? static_cast<uint16_t>(port_v->get<int64_t>()) : 0;
        for (std::size_t i = 0; i < 3 && i < bytes.size(); ++i) {
            if (bytes[i].is<int64_t>())
                ev.midi.data[i] = static_cast<uint8_t>(bytes[i].get<int64_t>());
        }
        in_queue_.push(ev);
        break;
    }

    case ipc::msg_tx_log: {
        if (!sess || msg.payload.empty())
            break;
        // Payload is an EDN map representing a txlog::entry.
        const std::string_view text{reinterpret_cast<const char*>(msg.payload.data()),
                                    msg.payload.size()};
        auto                   parsed = edn::parse(text);
        if (!parsed || !parsed->is<edn::map>())
            break;
        const auto& m = parsed->get<edn::map>();

        txlog::entry e;

        // :id → edn::uuid (stored as tagged{"uuid", canonical-string})
        if (const auto* v = m.find_kw("id"); v && v->is<edn::tagged>()) {
            const auto& t = v->get<edn::tagged>();
            if (t.tag == "uuid" && t.val && t.val->is<std::string>())
                e.id = parse_uuid(t.val->get<std::string>());
        }

        // :beat → double
        if (const auto* v = m.find_kw("beat"); v) {
            if (v->is<double>())
                e.beat = v->get<double>();
            else if (v->is<int64_t>())
                e.beat = static_cast<double>(v->get<int64_t>());
        }

        // :wall-ns → int64_t
        if (const auto* v = m.find_kw("wall-ns"); v && v->is<int64_t>())
            e.wall_ns = v->get<int64_t>();

        // :source → edn::keyword
        if (const auto* v = m.find_kw("source"); v && v->is<edn::keyword>())
            e.source = v->get<edn::keyword>();

        // :path → edn::value
        if (const auto* v = m.find_kw("path"); v)
            e.path = *v;

        // :before / :after / :parent → optional<edn::value>
        if (const auto* v = m.find_kw("before"); v && !v->is_nil())
            e.before = *v;
        if (const auto* v = m.find_kw("after"); v && !v->is_nil())
            e.after = *v;
        if (const auto* v = m.find_kw("parent"); v && !v->is_nil())
            e.parent = *v;

        sess->emit(e);
        break;
    }

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

        if (!cfg_.host)
            break;
        {
            auto mgr = std::make_unique<plugin_graph_manager>();
            if (mgr->load(g, cfg_.plugins, cfg_.host)) {
                mgr->set_audio_config(cfg_.sample_rate, cfg_.min_frames, cfg_.max_frames);
                mgr->start_processing_all();
                graph_.store(std::move(mgr));
            }
        }
        break;
    }

    case ipc::msg_graph_reset:
        graph_.store(std::make_unique<plugin_graph_manager>());
        break;

    default:
        break;
    }

    (void)conn_fd; // reserved for future reply messages
}

} // namespace kairos
