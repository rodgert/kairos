// SPDX-License-Identifier: GPL-2.0-or-later

#include <kairos/control_thread.hpp>
#include <kairos/input_event.hpp>
#include <kairos/plugin_host.hpp>
#include <kairos/spsc_queue.hpp>

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
#include <csignal>
#include <cstdlib>
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
void              on_signal(int) {
    g_running.store(false, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char* argv[]) {
    std::string  socket_path     = "/tmp/kairos.sock";
    std::string  db_path         = "kairos.db";
    int          midi_port       = -1;
    int          midi_in_port    = -1;
    uint16_t     osc_port        = 9001;
    double       initial_bpm     = 120.0;
    double       sample_rate     = 48000.0;
    uint32_t     block_size      = 256;
    bool         no_discover     = false;
    bool         no_audio        = false;
    bool         list_audio_devs = false;
    unsigned int audio_device_id = 0; // 0 = default
    uint32_t     audio_out_ch    = 2;
    uint32_t     audio_in_ch     = 0;

    // Extra plugin search paths beyond the platform defaults.
    std::vector<std::string> extra_search_paths;
    // Explicit plugin registrations: id=path pairs from --plugin flags.
    kairos::plugin_registry explicit_plugins;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--socket" && i + 1 < argc)
            socket_path = argv[++i];
        else if (arg == "--db" && i + 1 < argc)
            db_path = argv[++i];
        else if (arg == "--midi-port" && i + 1 < argc)
            midi_port = std::atoi(argv[++i]);
        else if (arg == "--midi-in-port" && i + 1 < argc)
            midi_in_port = std::atoi(argv[++i]);
        else if (arg == "--osc-port" && i + 1 < argc)
            osc_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--bpm" && i + 1 < argc)
            initial_bpm = std::atof(argv[++i]);
        else if (arg == "--block-size" && i + 1 < argc)
            block_size = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--plugin-path" && i + 1 < argc)
            extra_search_paths.emplace_back(argv[++i]);
        else if (arg == "--no-discover")
            no_discover = true;
        else if (arg == "--no-audio")
            no_audio = true;
        else if (arg == "--list-audio-devices")
            list_audio_devs = true;
        else if (arg == "--audio-device" && i + 1 < argc)
            audio_device_id = static_cast<unsigned int>(std::atoi(argv[++i]));
        else if (arg == "--audio-out-ch" && i + 1 < argc)
            audio_out_ch = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--audio-in-ch" && i + 1 < argc)
            audio_in_ch = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (arg == "--plugin" && i + 1 < argc) {
            // --plugin "org.example.Synth=/path/to/synth.clap"
            std::string pair{argv[++i]};
            const auto  sep = pair.find('=');
            if (sep != std::string::npos)
                explicit_plugins.emplace(pair.substr(0, sep), pair.substr(sep + 1));
        } else if (arg == "--version") {
            std::cout << "kairos v" << kairos::version() << "\n";
            return EXIT_SUCCESS;
        } else if (arg == "--help") {
            std::cout
                << "Usage: kairos [options]\n"
                   "  --socket <path>         Unix domain socket (default: /tmp/kairos.sock)\n"
                   "  --db <path>             txlog database (default: kairos.db)\n"
                   "  --bpm <bpm>             Initial Link tempo (default: 120)\n"
                   "  --block-size <n>        CLAP block size in frames (default: 256)\n"
                   "  --midi-port <n>         MIDI output port index\n"
                   "  --midi-in-port <n>      MIDI input port index\n"
                   "  --osc-port <n>          UDP OSC listen port (default: 9001)\n"
                   "  --plugin <id=path>      Register a plugin explicitly\n"
                   "  --plugin-path <dir>     Extra directory to scan for .clap files\n"
                   "  --no-discover           Skip platform default CLAP scan paths\n"
                   "  --audio-device <id>     RtAudio device id (0=default, see "
                   "--list-audio-devices)\n"
                   "  --audio-out-ch <n>      Audio output channels (default: 2)\n"
                   "  --audio-in-ch <n>       Audio input channels (default: 0)\n"
                   "  --no-audio              Headless timer loop (skip audio device)\n"
                   "  --list-audio-devices    Print available audio devices and exit\n"
                   "  --version               Print version and exit\n";
            return EXIT_SUCCESS;
        }
    }

    if (list_audio_devs) {
        kairos::audio_device::list_devices();
        return EXIT_SUCCESS;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Build plugin registry: builtins + discovered + explicit overrides.
    kairos::plugin_registry plugins;

    // Always available — no path needed.
    plugins[kairos::k_passthrough_plugin_id]       = "kairos:passthrough";
    plugins[kairos::k_audio_passthrough_plugin_id] = "kairos:audio-passthrough";

    // Discover installed .clap files from platform default paths.
    if (!no_discover) {
        auto discovered = kairos::discover_plugins(extra_search_paths);
        for (auto& [id, path] : discovered)
            plugins.emplace(id, std::move(path)); // don't overwrite builtins
    }

    // Explicit --plugin flags win over discovery.
    for (auto& [id, path] : explicit_plugins)
        plugins.insert_or_assign(id, std::move(path));

    std::cerr << "[kairos] plugin registry: " << plugins.size() << " plugin(s)\n";
    for (const auto& [id, path] : plugins)
        std::cerr << "  " << id << " → " << path << "\n";

    // Shared queues
    kairos::param_queue       param_queue;
    kairos::midi_event_queue  midi_out_queue;
    kairos::input_event_queue ipc_in_queue;
    kairos::input_event_queue hw_midi_in_queue;
    kairos::input_event_queue osc_in_queue;

    // Control thread — IPC + session + graph management
    kairos::control_thread::config ctrl_cfg{
        .socket_path = socket_path,
        .db_path     = db_path,
        .plugins     = std::move(plugins),
        .host        = kairos::kairos_host(),
        .sample_rate = sample_rate,
        .min_frames  = block_size,
        .max_frames  = block_size,
    };
    kairos::control_thread ctrl{ctrl_cfg, param_queue, ipc_in_queue};
    ctrl.start();

    // Link peer
    kairos::link_peer link{initial_bpm};
    link.enable(true);

    // MIDI I/O
    kairos::midi_io midi;
    kairos::midi_io::list_ports();
    if (midi_port >= 0)
        midi.open_port(static_cast<unsigned int>(midi_port));
    if (midi_in_port >= 0)
        midi.open_input_port(static_cast<unsigned int>(midi_in_port), hw_midi_in_queue);

    // OSC server
    kairos::osc_server osc{osc_port, osc_in_queue};
    osc.start();

    // Process loop: audio_engine (wordclock-locked) or process_thread (headless).
    const bool use_audio = !no_audio;

    std::unique_ptr<kairos::audio_engine>   audio_eng;
    std::unique_ptr<kairos::process_thread> proc_thread;

    if (use_audio) {
        kairos::audio_engine::config eng_cfg{
            .sample_rate   = sample_rate,
            .buffer_frames = block_size,
            .out_channels  = audio_out_ch,
            .in_channels   = audio_in_ch,
            .device_id     = audio_device_id,
        };
        audio_eng =
            std::make_unique<kairos::audio_engine>(eng_cfg, ctrl.graph(), link, midi_out_queue,
                                                   ipc_in_queue, hw_midi_in_queue, osc_in_queue);
        if (!audio_eng->start()) {
            std::cerr << "[kairos] audio engine failed to start, falling back to headless\n";
            audio_eng.reset();
        }
    }

    if (!audio_eng) {
        kairos::process_thread::config proc_cfg{
            .sample_rate = sample_rate,
            .block_size  = block_size,
        };
        proc_thread =
            std::make_unique<kairos::process_thread>(proc_cfg, ctrl.graph(), link, midi_out_queue,
                                                     ipc_in_queue, hw_midi_in_queue, osc_in_queue);
        proc_thread->start();
    }

    // MIDI dispatch thread — drains midi_out_queue and sends to hardware
    std::thread midi_thread{[&]() {
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
        // Drain any remaining events before exit.
        while (auto batch = midi_out_queue.pop()) {
            for (uint8_t i = 0; i < batch->count; ++i) {
                const auto& e = batch->events[i];
                if (e.size > 0)
                    midi.send(std::vector<uint8_t>{e.data, e.data + e.size});
            }
        }
    }};

    std::cerr << "[kairos] v" << kairos::version() << "\n";
    std::cerr << "[kairos] socket=" << socket_path << " db=" << db_path << "\n";
    std::cerr << "[link]   enabled, bpm=" << initial_bpm << "\n";
    std::cerr << "[audio]  sample_rate=" << sample_rate << " block_size=" << block_size << "\n";
    std::cerr << "[osc]    listening on port " << osc_port << "\n";

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
