// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <edn/value.hpp>

namespace kairos {

// Maps stable plugin identifier (e.g. "org.cljseq.dsp/fm-voice") to .clap file path.
using plugin_registry = std::unordered_map<std::string, std::string>;

// A single plugin instance in the audio graph.
// `plugin` is the stable string identifier used by the plugin registry
// (e.g. "org.cljseq.dsp/fm-voice"); CLAP param IDs are resolved at graph load.
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
