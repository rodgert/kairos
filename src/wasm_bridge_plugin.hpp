// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <clap/factory/plugin-factory.h>

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
// wasmtime engine, store, and module for the .wasm path encoded in plugin_id.
// Pointer has program lifetime.
const clap_plugin_factory_t* get_wasm_bridge_factory() noexcept;

} // namespace kairos
