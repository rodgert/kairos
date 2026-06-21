// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <clap/factory/plugin-factory.h>

namespace kairos {

// Canonical plugin IDs for built-in plugins.
// Register with the matching "kairos:" path to use in a graph.
constexpr const char* k_passthrough_plugin_id       = "org.nomos-studio.kairos.midi-passthrough";
constexpr const char* k_audio_passthrough_plugin_id = "org.nomos-studio.kairos.audio-passthrough";

// Sentinel path prefix recognised by plugin_instance::load().
// Any path beginning with "kairos:" bypasses dlopen and uses get_builtin_factory().
constexpr const char* k_builtin_path_prefix = "kairos:";

// Returns the factory covering all built-in kairos plugins.
// Pointer has program lifetime.
const clap_plugin_factory_t* get_builtin_factory() noexcept;

} // namespace kairos
