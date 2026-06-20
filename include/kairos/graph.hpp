// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <edn/value.hpp>

namespace kairos {

// Metadata for a discovered CLAP plugin.
struct plugin_info {
    std::string path;        // absolute binary path inside the .clap bundle
    std::string name;        // human-readable plugin name
    std::string vendor;      // vendor / manufacturer string
    std::string version;     // version string (e.g. "1.2.0")
    std::string description; // short description
};

// Maps stable plugin identifier (e.g. "org.nomos-studio.dsp/fm-voice") to plugin metadata.
using plugin_registry = std::unordered_map<std::string, plugin_info>;

// A single plugin instance in the audio graph.
// `plugin` is the stable string identifier used by the plugin registry
// (e.g. "org.nomos-studio.dsp/fm-voice"); CLAP param IDs are resolved at graph load.
struct plugin_node {
    edn::keyword id;     // e.g. :synth/voice-1
    std::string  plugin; // plugin registry key
    edn::map     params; // initial parameter values: keyword → value
};

// A directed audio routing edge.
// `to_node == edn::keyword{"host"}` is the sentinel for the container audio output.
struct audio_edge {
    edn::keyword from_node;
    edn::keyword from_port;
    edn::keyword to_node;
    edn::keyword to_port;
};

struct plugin_graph {
    std::vector<plugin_node> nodes;
    std::vector<audio_edge>  edges;
};

} // namespace kairos
