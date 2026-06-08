// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <kairos/clap_kairos_hot_swap.h>

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
// Hot-swap extension — types now defined in the public header.
// Aliases kept here so wasm_bridge_plugin.cpp compiles unchanged.
// ---------------------------------------------------------------------------
constexpr const char* k_kairos_ext_hot_swap = CLAP_EXT_KAIROS_HOT_SWAP; // "/2"
using kairos_hot_swap_t                     = ::clap_kairos_hot_swap_t;

} // namespace kairos
