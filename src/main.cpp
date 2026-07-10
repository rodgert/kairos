// SPDX-License-Identifier: GPL-2.0-or-later

#include <kairos/control_thread.hpp>
#include <kairos/plugin_host.hpp>
#include <nomos/rt/common_args.hpp>
#include <nomos/rt/event_scheduler.hpp>
#include <nomos/rt/input_event.hpp>
#include <nomos/rt/ipc.hpp>
#include <nomos/rt/signal_handlers.hpp>
#include <nomos/rt/spsc_queue.hpp>

#include "audio_device.hpp"
#include "audio_engine.hpp"
#include "builtin_plugins.hpp"
#include "link_peer.hpp"
#include "midi_io.hpp"
#include "osc_server.hpp"
#include "plugin_discovery.hpp"
#include "process_thread.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

namespace kairos {
std::string_view version() noexcept;
}

namespace {
std::atomic<bool> g_running{true};
void              on_signal(int) noexcept {
    g_running.store(false, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char* argv[]) {
    constexpr double sample_rate = 48000.0;

    nomos::rt::common_args args;
    args.socket_path = "/tmp/kairos.sock";
    args.db_path     = "kairos.db";
    args.osc_port    = 9001;
    args.block_size  = 256;

    // kairos-specific args parsed from remainder.
    std::string              midi_virtual_port_name;
    bool                     no_discover = false;
    std::vector<std::string> extra_search_paths;
    kairos::plugin_registry  explicit_plugins;

    const auto rem = nomos::rt::parse_common_args(argc, argv, args);

    for (std::size_t i = 0; i < rem.size(); ++i) {
        const std::string_view a        = rem[i];
        const bool             has_next = (i + 1 < rem.size());

        if (a == "--virtual-midi-port" && has_next)
            midi_virtual_port_name = rem[++i];
        else if (a == "--plugin-path" && has_next)
            extra_search_paths.emplace_back(rem[++i]);
        else if (a == "--no-discover")
            no_discover = true;
        else if (a == "--plugin" && has_next) {
            std::string pair{rem[++i]};
            const auto  sep = pair.find('=');
            if (sep != std::string::npos)
                explicit_plugins.emplace(pair.substr(0, sep),
                                         kairos::plugin_info{.path = pair.substr(sep + 1)});
        } else if (a == "--help") {
            std::cout << "Usage: kairos [options]\n";
            nomos::rt::print_common_args_help(args, std::cout);
            std::cout << "  --virtual-midi-port <name>  Create a virtual MIDI output port\n"
                      << "  --plugin-path <dir>         Extra directory to scan for .clap files\n"
                      << "  --no-discover               Skip platform default CLAP scan paths\n"
                      << "  --plugin <id=path>          Register a plugin explicitly\n";
            return EXIT_SUCCESS;
        } else {
            std::cerr << "[kairos] unknown argument: " << a << "\n";
        }
    }

    if (args.version) {
        std::cout << "kairos v" << kairos::version() << "\n";
        return EXIT_SUCCESS;
    }

    if (args.list_audio_devs) {
        nomos::rt::audio_device::list_devices();
        return EXIT_SUCCESS;
    }

    nomos::rt::install_signal_handlers(on_signal);

    // Build plugin registry: builtins + discovered + explicit overrides.
    kairos::plugin_registry plugins;
    plugins[kairos::k_passthrough_plugin_id] = kairos::plugin_info{.path = "kairos:passthrough"};
    plugins[kairos::k_audio_passthrough_plugin_id] =
        kairos::plugin_info{.path = "kairos:audio-passthrough"};

    if (!no_discover) {
        auto discovered = kairos::discover_plugins(extra_search_paths);
        for (auto& [id, info] : discovered)
            plugins.emplace(id, std::move(info));
    }
    for (auto& [id, info] : explicit_plugins)
        plugins.insert_or_assign(id, std::move(info));

    std::cerr << "[kairos] plugin registry: " << plugins.size() << " plugin(s)\n";
    for (const auto& [id, info] : plugins)
        std::cerr << "  " << id << " → " << info.path << "\n";

    // Shared queues.
    nomos::rt::param_queue       param_queue;
    nomos::rt::midi_event_queue  midi_out_queue;
    nomos::rt::input_event_queue ipc_in_queue;
    nomos::rt::input_event_queue hw_midi_in_queue;
    nomos::rt::input_event_queue osc_in_queue;

    nomos::rt::event_scheduler scheduler;

    nomos::rt::link_peer link{args.bpm};
    link.enable(true);

    kairos::control_thread::config ctrl_cfg{
        .socket_path   = args.socket_path,
        .db_path       = args.db_path,
        .sched_staging = &scheduler.staging(),
        .link_peer     = &link,
        .plugins       = std::move(plugins),
        .host          = kairos::kairos_host(),
        .sample_rate   = sample_rate,
        .min_frames    = args.block_size,
        .max_frames    = args.block_size,
    };
    kairos::control_thread ctrl{ctrl_cfg, param_queue, ipc_in_queue};
    ctrl.start();

    nomos::rt::midi_io midi;
    nomos::rt::midi_io::list_ports();
    if (args.midi_port >= 0)
        midi.open_port(static_cast<unsigned int>(args.midi_port));
    else if (!args.midi_port_name.empty())
        midi.open_port_by_name(args.midi_port_name);
    else if (!midi_virtual_port_name.empty())
        midi.open_virtual_port(midi_virtual_port_name);

    midi.set_echo_callback([&ctrl](const std::vector<uint8_t>& bytes) {
        if (bytes.empty())
            return;
        const uint8_t status = bytes[0];
        const uint8_t ch     = status & 0x0Fu;
        const uint8_t b1     = bytes.size() > 1 ? bytes[1] : 0u;
        const uint8_t b2     = bytes.size() > 2 ? bytes[2] : 0u;
        std::string edn = "{:port 0 :channel " + std::to_string(static_cast<int>(ch)) + " :data [" +
                          std::to_string(static_cast<int>(status)) + " " +
                          std::to_string(static_cast<int>(b1)) + " " +
                          std::to_string(static_cast<int>(b2)) + "]}";
        ctrl.push_frame(nomos::rt::ipc::msg_midi_event, edn);
    });

    if (args.midi_in_port >= 0)
        midi.open_input_port(static_cast<unsigned int>(args.midi_in_port), hw_midi_in_queue);
    else if (!args.midi_in_port_name.empty())
        midi.open_input_port_by_name(args.midi_in_port_name, hw_midi_in_queue);

    nomos::rt::osc_server osc{args.osc_port, osc_in_queue};
    osc.start();

    std::unique_ptr<kairos::audio_engine>   audio_eng;
    std::unique_ptr<kairos::process_thread> proc_thread;

    if (!args.no_audio) {
        kairos::audio_engine::config eng_cfg{
            .sample_rate   = sample_rate,
            .buffer_frames = args.block_size,
            .out_channels  = args.audio_out_ch,
            .in_channels   = args.audio_in_ch,
            .device_id     = args.audio_device,
        };
        audio_eng = std::make_unique<kairos::audio_engine>(
            eng_cfg, ctrl.graph(), link, midi_out_queue, ipc_in_queue, hw_midi_in_queue,
            osc_in_queue, &scheduler);
        if (!audio_eng->start()) {
            std::cerr << "[kairos] audio engine failed to start, falling back to headless\n";
            audio_eng.reset();
        }
    }

    if (!audio_eng) {
        kairos::process_thread::config proc_cfg{
            .sample_rate = sample_rate,
            .block_size  = args.block_size,
        };
        proc_thread = std::make_unique<kairos::process_thread>(
            proc_cfg, ctrl.graph(), link, midi_out_queue, ipc_in_queue, hw_midi_in_queue,
            osc_in_queue, &scheduler);
        proc_thread->start();
    }

    std::thread midi_thread{[&]() {
        nomos::rt::block_signals_on_this_thread();
        while (g_running.load(std::memory_order_relaxed)) {
            if (auto batch = midi_out_queue.pop()) {
                for (uint8_t i = 0; i < batch->count; ++i) {
                    const auto& e = batch->events[i];
                    if (e.size > 0)
                        midi.send(std::vector<uint8_t>{e.data, e.data + e.size});
                }
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
        while (auto batch = midi_out_queue.pop()) {
            for (uint8_t i = 0; i < batch->count; ++i) {
                const auto& e = batch->events[i];
                if (e.size > 0)
                    midi.send(std::vector<uint8_t>{e.data, e.data + e.size});
            }
        }
    }};

    std::cerr << "[kairos] v" << kairos::version() << "\n";
    std::cerr << "[kairos] socket=" << args.socket_path << " db=" << args.db_path << "\n";
    std::cerr << "[link]   enabled, bpm=" << args.bpm << "\n";
    std::cerr << "[audio]  sample_rate=" << sample_rate << " block_size=" << args.block_size
              << "\n";
    std::cerr << "[osc]    listening on port " << args.osc_port << "\n";

    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cerr << "[kairos] shutting down\n";
    if (audio_eng)
        audio_eng->stop();
    if (proc_thread)
        proc_thread->stop();
    osc.stop();
    ctrl.stop();
    midi_thread.join();
    midi.close();
    link.enable(false);

    return EXIT_SUCCESS;
}
