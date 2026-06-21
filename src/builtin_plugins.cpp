// SPDX-License-Identifier: LGPL-2.1-or-later
#include "builtin_plugins.hpp"

#include <clap/audio-buffer.h>
#include <clap/events.h>
#include <clap/ext/audio-ports.h>
#include <clap/plugin-features.h>
#include <clap/process.h>
#include <clap/version.h>

#include <algorithm>
#include <cstring>

namespace kairos {

namespace {

    // ---------------------------------------------------------------------------
    // org.nomos-studio.kairos.midi-passthrough
    //
    // Stateless plugin.  Forwards all NOTE_ON, NOTE_OFF, and raw MIDI events from
    // in_events to out_events unchanged.  Useful as a terminal node for graphs
    // where nous drives hardware synths directly via IPC note events.
    // ---------------------------------------------------------------------------

    static constexpr const char* k_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

    static const clap_plugin_descriptor_t k_passthrough_desc{
        .clap_version = CLAP_VERSION_INIT,
        .id           = k_passthrough_plugin_id,
        .name         = "MIDI Passthrough",
        .vendor       = "nomos-studio/kairos",
        .url          = "https://github.com/nomos-studio/kairos",
        .manual_url   = "",
        .support_url  = "",
        .version      = "0.1.0",
        .description  = "Forwards NOTE_ON, NOTE_OFF, and MIDI events from input to output",
        .features     = k_features,
    };

    bool pt_init(const clap_plugin_t*) noexcept {
        return true;
    }
    void pt_destroy(const clap_plugin_t*) noexcept {
    }
    bool pt_activate(const clap_plugin_t*, double, uint32_t, uint32_t) noexcept {
        return true;
    }
    void pt_deactivate(const clap_plugin_t*) noexcept {
    }
    bool pt_start_processing(const clap_plugin_t*) noexcept {
        return true;
    }
    void pt_stop_processing(const clap_plugin_t*) noexcept {
    }
    void pt_reset(const clap_plugin_t*) noexcept {
    }

    clap_process_status pt_process(const clap_plugin_t*, const clap_process_t* proc) noexcept {
        if (!proc->in_events || !proc->out_events)
            return CLAP_PROCESS_CONTINUE;

        const uint32_t n = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* hdr = proc->in_events->get(proc->in_events, i);
            if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_NOTE_OFF ||
                hdr->type == CLAP_EVENT_MIDI)
                proc->out_events->try_push(proc->out_events, hdr);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    const void* pt_get_extension(const clap_plugin_t*, const char*) noexcept {
        return nullptr;
    }
    void pt_on_main_thread(const clap_plugin_t*) noexcept {
    }

    // Single static instance — the passthrough has no per-instance state.
    static const clap_plugin_t k_passthrough_plugin{
        .desc             = &k_passthrough_desc,
        .plugin_data      = nullptr,
        .init             = pt_init,
        .destroy          = pt_destroy,
        .activate         = pt_activate,
        .deactivate       = pt_deactivate,
        .start_processing = pt_start_processing,
        .stop_processing  = pt_stop_processing,
        .reset            = pt_reset,
        .process          = pt_process,
        .get_extension    = pt_get_extension,
        .on_main_thread   = pt_on_main_thread,
    };

    // ---------------------------------------------------------------------------
    // org.nomos-studio.kairos.audio-passthrough
    //
    // Stateless plugin.  Declares one stereo input port and one stereo output
    // port.  In process(), copies each input channel to the corresponding output
    // channel unchanged.  Useful as a no-op node for debugging audio routing or
    // as a placeholder in a graph under construction.
    // ---------------------------------------------------------------------------

    static constexpr const char* k_audio_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};

    static const clap_plugin_descriptor_t k_audio_passthrough_desc{
        .clap_version = CLAP_VERSION_INIT,
        .id           = k_audio_passthrough_plugin_id,
        .name         = "Audio Passthrough",
        .vendor       = "nomos-studio/kairos",
        .url          = "https://github.com/nomos-studio/kairos",
        .manual_url   = "",
        .support_url  = "",
        .version      = "0.1.0",
        .description  = "Copies stereo audio input to output unchanged",
        .features     = k_audio_features,
    };

    bool ap_init(const clap_plugin_t*) noexcept {
        return true;
    }
    void ap_destroy(const clap_plugin_t*) noexcept {
    }
    bool ap_activate(const clap_plugin_t*, double, uint32_t, uint32_t) noexcept {
        return true;
    }
    void ap_deactivate(const clap_plugin_t*) noexcept {
    }
    bool ap_start_processing(const clap_plugin_t*) noexcept {
        return true;
    }
    void ap_stop_processing(const clap_plugin_t*) noexcept {
    }
    void ap_reset(const clap_plugin_t*) noexcept {
    }

    clap_process_status ap_process(const clap_plugin_t*, const clap_process_t* proc) noexcept {
        if (proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1)
            return CLAP_PROCESS_CONTINUE;

        const clap_audio_buffer_t& in  = proc->audio_inputs[0];
        clap_audio_buffer_t&       out = proc->audio_outputs[0];
        const uint32_t             ch  = std::min(in.channel_count, out.channel_count);
        if (!in.data32 || !out.data32)
            return CLAP_PROCESS_CONTINUE;

        for (uint32_t c = 0; c < ch; ++c)
            std::memcpy(out.data32[c], in.data32[c], proc->frames_count * sizeof(float));

        return CLAP_PROCESS_CONTINUE;
    }

    // Audio ports extension — one stereo in, one stereo out.
    static const clap_audio_port_info_t k_ap_stereo_port{
        .id            = 0,
        .name          = "Main",
        .flags         = CLAP_AUDIO_PORT_IS_MAIN,
        .channel_count = 2,
        .port_type     = CLAP_PORT_STEREO,
        .in_place_pair = CLAP_INVALID_ID,
    };

    uint32_t ap_port_count(const clap_plugin_t*, bool) noexcept {
        return 1;
    }

    bool ap_port_get(const clap_plugin_t*, uint32_t index, bool /*is_input*/,
                     clap_audio_port_info_t* info) noexcept {
        if (index != 0)
            return false;
        *info = k_ap_stereo_port;
        return true;
    }

    static const clap_plugin_audio_ports_t k_ap_audio_ports{
        .count = ap_port_count,
        .get   = ap_port_get,
    };

    const void* ap_get_extension(const clap_plugin_t*, const char* id) noexcept {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &k_ap_audio_ports;
        return nullptr;
    }
    void ap_on_main_thread(const clap_plugin_t*) noexcept {
    }

    static const clap_plugin_t k_audio_passthrough_plugin{
        .desc             = &k_audio_passthrough_desc,
        .plugin_data      = nullptr,
        .init             = ap_init,
        .destroy          = ap_destroy,
        .activate         = ap_activate,
        .deactivate       = ap_deactivate,
        .start_processing = ap_start_processing,
        .stop_processing  = ap_stop_processing,
        .reset            = ap_reset,
        .process          = ap_process,
        .get_extension    = ap_get_extension,
        .on_main_thread   = ap_on_main_thread,
    };

    // ---------------------------------------------------------------------------
    // Built-in factory
    // ---------------------------------------------------------------------------

    static const clap_plugin_descriptor_t* const k_descriptors[] = {
        &k_passthrough_desc,
        &k_audio_passthrough_desc,
    };

    static const clap_plugin_t* const k_plugins[] = {
        &k_passthrough_plugin,
        &k_audio_passthrough_plugin,
    };

    constexpr uint32_t k_builtin_count = 2;

    uint32_t factory_count(const clap_plugin_factory_t*) noexcept {
        return k_builtin_count;
    }

    const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*,
                                                       uint32_t index) noexcept {
        return (index < k_builtin_count) ? k_descriptors[index] : nullptr;
    }

    const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* /*host*/,
                                        const char* plugin_id) noexcept {
        for (uint32_t i = 0; i < k_builtin_count; ++i) {
            if (std::strcmp(plugin_id, k_descriptors[i]->id) == 0)
                return k_plugins[i];
        }
        return nullptr;
    }

    static const clap_plugin_factory_t k_builtin_factory{
        .get_plugin_count      = factory_count,
        .get_plugin_descriptor = factory_descriptor,
        .create_plugin         = factory_create,
    };

} // namespace

const clap_plugin_factory_t* get_builtin_factory() noexcept {
    return &k_builtin_factory;
}

} // namespace kairos
