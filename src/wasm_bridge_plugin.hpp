// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <clap/factory/plugin-factory.h>
#include <clap/plugin.h>

namespace kairos {

// Plugin ID prefix for the WASM bridge.
// The full plugin ID encodes the .wasm file path:
//
//   "org.cljseq.kairos.wasm-bridge:/absolute/path/to/patch.wasm"
//
// Usage in a graph (path must be "kairos:" so plugin_instance::load routes
// it through get_wasm_bridge_factory rather than dlopen):
//   path      = "kairos:"
//   plugin_id = "org.cljseq.kairos.wasm-bridge:/path/to/patch.wasm"
constexpr const char* k_wasm_bridge_id_prefix = "org.cljseq.kairos.wasm-bridge:";

// Returns the factory for WASM bridge plugin instances.
// Each call to factory->create_plugin() allocates a new per-instance
// wasmtime store for the .wasm path encoded in plugin_id.
// The compiled wasmtime_module_t is cached per path (program lifetime).
// Pointer has program lifetime.
const clap_plugin_factory_t* get_wasm_bridge_factory() noexcept;

// ---------------------------------------------------------------------------
// Hot-swap extension — kairos-specific.
//
// Usage (host side):
//   1. Get the extension: plugin->get_extension(plugin, k_kairos_ext_hot_swap)
//   2. Call request() to queue the new .wasm path.
//   3. Call stop_processing() → reset() → start_processing().
//      wp_reset() performs the actual module swap.
//
// Constraints:
//   - The new .wasm must have the same number of inputs and outputs.
//   - Parameters are migrated by label; new params use Faust defaults.
// ---------------------------------------------------------------------------
constexpr const char* k_kairos_ext_hot_swap = "org.cljseq.kairos.ext.hot-swap/1";

struct kairos_hot_swap_t {
    // Queue new_wasm_path for the next wp_reset.
    // Returns false if new_wasm_path is unreadable.
    // Called on the main thread before stop_processing.
    bool (*request)(const clap_plugin_t* plugin, const char* new_wasm_path);
};

} // namespace kairos
