// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/clap_kairos_param_bus.h>
#include <kairos/clap_kairos_patch_bus.h>
#include <kairos/clap_kairos_tap_bus.h>

#include <nomos/rt/result.hpp>

#include <clap/entry.h>
#include <clap/ext/audio-ports.h>
#include <clap/plugin.h>
#include <clap/process.h>

#include <string>
#include <vector>

namespace kairos {

enum class plugin_error {
    load_failed,          // dlopen() returned null
    entry_not_found,      // clap_plugin_entry symbol missing from binary
    init_failed,          // entry->init() returned false
    factory_not_found,    // no CLAP_PLUGIN_FACTORY_ID factory in binary
    create_failed,        // factory->create_plugin() returned null
    plugin_init_failed,   // plugin->init() returned false
    activate_failed,      // plugin->activate() returned false
    processing_failed,    // plugin->start_processing() returned false
    wrong_state,          // operation not valid in current state
    hot_swap_unsupported, // plugin does not expose kairos hot-swap extension
    hot_swap_failed, // extension request() returned false (e.g. path unreadable or topology change)
    node_not_found,  // no node with the given keyword id exists in the graph
};

// RAII wrapper around the full CLAP plugin lifecycle.
//
// State machine:
//   load()            → initialized
//   activate()        → activated
//   start_processing()→ processing
//   process()           (only valid in processing state)
//   stop_processing() → activated
//   deactivate()      → initialized
//   ~plugin_instance()  calls destroy(), deinit(), dlclose()
class plugin_instance {
  public:
    enum class state { initialized, activated, processing };

    // Load a CLAP plugin binary at `path`, find the plugin with `plugin_id`,
    // and call plugin->init().  On macOS the path must point directly to the
    // Mach-O binary (bundle unwrapping is handled by the caller for now).
    static nomos::rt::result<plugin_instance, plugin_error>
    load(const std::string& path, const std::string& plugin_id, const clap_host_t* host);

    ~plugin_instance();
    plugin_instance(plugin_instance&&) noexcept;
    plugin_instance& operator=(plugin_instance&&) noexcept;
    plugin_instance(const plugin_instance&)            = delete;
    plugin_instance& operator=(const plugin_instance&) = delete;

    nomos::rt::result<std::monostate, plugin_error>
    activate(double sample_rate, uint32_t min_frames, uint32_t max_frames);

    nomos::rt::result<std::monostate, plugin_error> start_processing();

    clap_process_status process(const clap_process_t& proc);

    void stop_processing();
    void deactivate();

    // Hot-swap the underlying .wasm for bridge plugins (gapless — no stop/start cycle).
    // Delegates directly to the kairos hot-swap extension request(), which performs a
    // full RCU swap on the audio thread's DSP pointer without interrupting processing.
    // Returns hot_swap_unsupported if the plugin lacks the kairos hot-swap extension.
    // Returns hot_swap_failed if the new path is unreadable or has a different topology.
    // Only valid in activated or processing state.
    // old_wasm_path identifies which WASM slot to replace (empty = first slot).
    nomos::rt::result<std::monostate, plugin_error> hot_swap(const std::string& new_wasm_path,
                                                             const std::string& old_wasm_path = {});

    // Tap-bus — kairos/tap-bus custom CLAP extension.
    // Returns nullptr if the plugin does not expose the extension.
    // tap_schema() is valid after activate() and until the next activate() or reset().
    // tap_frame() is valid on the audio thread immediately after process().
    const clap_kairos_tap_schema_t* tap_schema() const noexcept;
    const float*                    tap_frame(uint32_t* out_count) const noexcept;

    // Param-bus — kairos/param-bus custom CLAP extension.
    // Returns nullptr if the plugin does not expose the extension.
    // param_schema() is valid after activate() and until the next activate() or reset().
    // set_param_frame() must be called on the audio thread before each process().
    const clap_kairos_param_schema_t* param_schema() const noexcept;
    bool set_param_frame(const float* values, uint32_t count) const noexcept;

    // Patch-bus — kairos/patch-bus custom CLAP extension.
    // Returns false if the plugin does not expose the extension or the descriptor is invalid.
    // push_patch() must be called from the main thread; the engine swap occurs at the next
    // process() boundary.  get_patch() returns the last accepted descriptor (main thread only).
    bool        push_patch(const char* edn_descriptor, uint32_t len) const noexcept;
    const char* get_patch() const noexcept;

    const clap_plugin_descriptor_t* descriptor() const noexcept;
    state                           current_state() const noexcept;

    const std::vector<clap_audio_port_info_t>& in_ports() const noexcept { return in_ports_; }
    const std::vector<clap_audio_port_info_t>& out_ports() const noexcept { return out_ports_; }
    const clap_plugin_t*                       raw() const noexcept { return plugin_; }

  private:
    plugin_instance() = default;
    void teardown() noexcept;

    void*                      lib_handle_{nullptr};
    const clap_plugin_entry_t* entry_{nullptr};
    const clap_plugin_t*       plugin_{nullptr};
    state                      state_{state::initialized};

    const clap_plugin_tap_bus_t*   tap_bus_ext_{nullptr};
    const clap_plugin_param_bus_t* param_bus_ext_{nullptr};
    const clap_plugin_patch_bus_t* patch_bus_ext_{nullptr};

    std::vector<clap_audio_port_info_t> in_ports_;
    std::vector<clap_audio_port_info_t> out_ports_;
};

} // namespace kairos
