// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WASM bridge plugin — embeds wasmtime and presents a Faust-compiled .wasm
// module as a CLAP plugin.
//
// Faust WASM ABI (verified from binary analysis):
//   All exported functions take an explicit i32 DSP pointer as the first arg.
//   In practice, Faust codegen always hardcodes i32.const 0 as the DSP base
//   and ignores the parameter — so the bridge always passes 0.
//
//   compute(0, count: i32, in_ptrs: i32, out_ptrs: i32) -> ()
//   init(0, sample_rate: i32) -> ()
//   getNumInputs(0) -> i32
//   getNumOutputs(0) -> i32
//   setParamValue(0, index: i32, value: f32) -> ()   // index = absolute WASM addr
//   getParamValue(0, index: i32) -> f32
//
//   Audio buffers: in_ptrs/out_ptrs are WASM memory addresses of pointer arrays.
//   Each element is an i32 WASM address of a float[] sample buffer.
//
// Phase 1 — audio generation/processing.
// Phase 2 (TODO) — CLAP params via Faust JSON metadata; hot-swap.

#include "wasm_bridge_plugin.hpp"

#include <clap/audio-buffer.h>
#include <clap/events.h>
#include <clap/ext/audio-ports.h>
#include <clap/plugin-features.h>
#include <clap/process.h>
#include <clap/version.h>

// wasmtime C headers use GNU anonymous-struct extension; suppress for this include.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <wasmtime.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace kairos {

namespace {

    static constexpr uint32_t k_wasm_page_size = 65536u;

    // ---------------------------------------------------------------------------
    // Per-instance state
    // ---------------------------------------------------------------------------

    struct WasmBridgeInstance {
        // clap_plugin_t must be the first member so that plugin_data == this.
        clap_plugin_t            plugin{};
        clap_plugin_descriptor_t descriptor{};
        std::string              plugin_id_str;
        std::string              wasm_path;

        // wasmtime resources (all owned by this struct, freed in destroy)
        wasm_engine_t*      engine{nullptr};
        wasmtime_store_t*   store{nullptr};
        wasmtime_module_t*  module{nullptr};
        wasmtime_instance_t instance{};
        wasmtime_memory_t   wasm_memory{};

        // Cached Faust WASM function handles (valid after wp_init)
        wasmtime_func_t fn_init{};
        wasmtime_func_t fn_compute{};
        wasmtime_func_t fn_get_num_inputs{};
        wasmtime_func_t fn_get_num_outputs{};

        // Channel counts (queried in wp_init, used for audio-ports extension)
        int32_t num_inputs{0};
        int32_t num_outputs{0};

        // Activation state (set in wp_activate, used in wp_process)
        uint32_t max_frames{0};

        // WASM memory addresses of scratch buffers (set in wp_activate).
        // Layout (pages grown at activate time, starting at scratch_base):
        //   [num_inputs  * 4 bytes] input channel pointer array  (i32 WASM addresses)
        //   [num_outputs * 4 bytes] output channel pointer array
        //   [num_inputs  * max_frames * 4 bytes] input float sample buffers
        //   [num_outputs * max_frames * 4 bytes] output float sample buffers
        int32_t wasm_in_ptrs{0};
        int32_t wasm_out_ptrs{0};
        int32_t wasm_in_samples{0};
        int32_t wasm_out_samples{0};

        bool wasm_ready{false};
    };

    // ---------------------------------------------------------------------------
    // wasmtime call helpers
    // ---------------------------------------------------------------------------

    // Make a wasmtime_val_t of kind i32.
    static inline wasmtime_val_t wt_i32(int32_t v) noexcept {
        wasmtime_val_t val{};
        val.kind   = WASMTIME_I32;
        val.of.i32 = v;
        return val;
    }

    // Call a WASM function; discard result, swallow errors (bridge continues).
    static void call_wasm(wasmtime_context_t* ctx, const wasmtime_func_t* fn,
                          const wasmtime_val_t* args, size_t nargs, wasmtime_val_t* results,
                          size_t nresults) noexcept {
        wasm_trap_t*      trap = nullptr;
        wasmtime_error_t* err  = wasmtime_func_call(ctx, fn, args, nargs, results, nresults, &trap);
        if (err)
            wasmtime_error_delete(err);
        if (trap)
            wasm_trap_delete(trap);
    }

    static bool get_export_func(wasmtime_context_t* ctx, const wasmtime_instance_t* inst,
                                const char* name, wasmtime_func_t* out) noexcept {
        wasmtime_extern_t ext{};
        if (!wasmtime_instance_export_get(ctx, inst, name, std::strlen(name), &ext))
            return false;
        if (ext.kind != WASMTIME_EXTERN_FUNC)
            return false;
        *out = ext.of.func;
        return true;
    }

    static bool get_export_memory(wasmtime_context_t* ctx, const wasmtime_instance_t* inst,
                                  const char* name, wasmtime_memory_t* out) noexcept {
        wasmtime_extern_t ext{};
        if (!wasmtime_instance_export_get(ctx, inst, name, std::strlen(name), &ext))
            return false;
        if (ext.kind != WASMTIME_EXTERN_MEMORY)
            return false;
        *out = ext.of.memory;
        return true;
    }

    // ---------------------------------------------------------------------------
    // CLAP plugin callbacks
    // ---------------------------------------------------------------------------

    static bool wp_init(const clap_plugin_t* plugin) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);

        // Load the .wasm file into memory.
        FILE* f = std::fopen(inst->wasm_path.c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long file_size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (file_size <= 0) {
            std::fclose(f);
            return false;
        }

        std::vector<uint8_t> wasm_bytes(static_cast<size_t>(file_size));
        std::fread(wasm_bytes.data(), 1, wasm_bytes.size(), f);
        std::fclose(f);

        inst->engine = wasm_engine_new();
        if (!inst->engine)
            return false;

        inst->store = wasmtime_store_new(inst->engine, nullptr, nullptr);
        if (!inst->store)
            return false;

        wasmtime_context_t* ctx = wasmtime_store_context(inst->store);

        wasmtime_error_t* err =
            wasmtime_module_new(inst->engine, wasm_bytes.data(), wasm_bytes.size(), &inst->module);
        if (err) {
            wasmtime_error_delete(err);
            return false;
        }

        // Faust WASM modules have zero imports — instantiate directly.
        wasm_trap_t* trap = nullptr;
        err = wasmtime_instance_new(ctx, inst->module, nullptr, 0, &inst->instance, &trap);
        if (err) {
            wasmtime_error_delete(err);
            return false;
        }
        if (trap) {
            wasm_trap_delete(trap);
            return false;
        }

        // Cache exported function handles.
        if (!get_export_func(ctx, &inst->instance, "init", &inst->fn_init))
            return false;
        if (!get_export_func(ctx, &inst->instance, "compute", &inst->fn_compute))
            return false;
        if (!get_export_func(ctx, &inst->instance, "getNumInputs", &inst->fn_get_num_inputs))
            return false;
        if (!get_export_func(ctx, &inst->instance, "getNumOutputs", &inst->fn_get_num_outputs))
            return false;
        if (!get_export_memory(ctx, &inst->instance, "memory", &inst->wasm_memory))
            return false;

        // Query channel counts now so the audio-ports extension can answer before activate().
        // DSP pointer is always 0 (Faust WASM hardcodes i32.const 0 internally).
        wasmtime_val_t dsp = wt_i32(0);
        wasmtime_val_t result{};
        call_wasm(ctx, &inst->fn_get_num_inputs, &dsp, 1, &result, 1);
        inst->num_inputs = result.of.i32;
        call_wasm(ctx, &inst->fn_get_num_outputs, &dsp, 1, &result, 1);
        inst->num_outputs = result.of.i32;

        inst->wasm_ready = true;
        return true;
    }

    static void wp_destroy(const clap_plugin_t* plugin) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        if (inst->module) {
            wasmtime_module_delete(inst->module);
            inst->module = nullptr;
        }
        if (inst->store) {
            wasmtime_store_delete(inst->store);
            inst->store = nullptr;
        }
        if (inst->engine) {
            wasm_engine_delete(inst->engine);
            inst->engine = nullptr;
        }
        delete inst;
    }

    static bool wp_activate(const clap_plugin_t* plugin, double sample_rate,
                            uint32_t /*min_frames*/, uint32_t   max_frames) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        if (!inst->wasm_ready)
            return false;

        wasmtime_context_t* ctx = wasmtime_store_context(inst->store);

        // Call Faust init(dsp=0, sample_rate).
        wasmtime_val_t init_args[2] = {wt_i32(0), wt_i32(static_cast<int32_t>(sample_rate))};
        call_wasm(ctx, &inst->fn_init, init_args, 2, nullptr, 0);

        inst->max_frames = max_frames;

        // Grow WASM memory by one page to hold audio scratch buffers.
        // Faust uses the initial page(s) for static DSP state; we grow past that.
        uint64_t          prev_pages = 0;
        wasmtime_error_t* err = wasmtime_memory_grow(ctx, &inst->wasm_memory, 1, &prev_pages);
        if (err) {
            wasmtime_error_delete(err);
            return false;
        }

        // scratch_base is the first byte of the newly grown page.
        const int32_t base = static_cast<int32_t>(prev_pages * k_wasm_page_size);

        inst->wasm_in_ptrs    = base;
        inst->wasm_out_ptrs   = base + inst->num_inputs * 4;
        inst->wasm_in_samples = inst->wasm_out_ptrs + inst->num_outputs * 4;
        inst->wasm_out_samples =
            inst->wasm_in_samples + inst->num_inputs * static_cast<int32_t>(max_frames) * 4;

        // Write channel pointer arrays into WASM memory now (they don't change
        // between process calls — only the sample data pointed to by them changes).
        uint8_t* mem = wasmtime_memory_data(ctx, &inst->wasm_memory);

        for (int32_t c = 0; c < inst->num_inputs; ++c) {
            int32_t sample_addr = inst->wasm_in_samples + c * static_cast<int32_t>(max_frames) * 4;
            std::memcpy(mem + inst->wasm_in_ptrs + c * 4, &sample_addr, 4);
        }
        for (int32_t c = 0; c < inst->num_outputs; ++c) {
            int32_t sample_addr = inst->wasm_out_samples + c * static_cast<int32_t>(max_frames) * 4;
            std::memcpy(mem + inst->wasm_out_ptrs + c * 4, &sample_addr, 4);
        }

        return true;
    }

    static void wp_deactivate(const clap_plugin_t*) noexcept {
    }
    static bool wp_start_processing(const clap_plugin_t*) noexcept {
        return true;
    }
    static void wp_stop_processing(const clap_plugin_t*) noexcept {
    }
    static void wp_reset(const clap_plugin_t*) noexcept {
    }

    static clap_process_status wp_process(const clap_plugin_t*  plugin,
                                          const clap_process_t* proc) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        if (!inst->wasm_ready || inst->max_frames == 0)
            return CLAP_PROCESS_CONTINUE;

        wasmtime_context_t* ctx    = wasmtime_store_context(inst->store);
        uint8_t*            mem    = wasmtime_memory_data(ctx, &inst->wasm_memory);
        const uint32_t      frames = proc->frames_count;

        // --- Copy CLAP input channels → WASM scratch memory ---
        // Assumes a single CLAP audio input port with num_inputs channels.
        if (inst->num_inputs > 0 && proc->audio_inputs_count > 0) {
            const clap_audio_buffer_t& in_buf = proc->audio_inputs[0];
            for (int32_t c = 0; c < inst->num_inputs; ++c) {
                float* dst = reinterpret_cast<float*>(
                    mem + inst->wasm_in_samples + c * static_cast<int32_t>(inst->max_frames) * 4);
                if (in_buf.data32 && c < static_cast<int32_t>(in_buf.channel_count) &&
                    in_buf.data32[c])
                    std::memcpy(dst, in_buf.data32[c], frames * sizeof(float));
                else
                    std::memset(dst, 0, frames * sizeof(float));
            }
        }

        // --- Call Faust compute(dsp=0, frames, in_ptrs, out_ptrs) ---
        wasmtime_val_t args[4] = {
            wt_i32(0),
            wt_i32(static_cast<int32_t>(frames)),
            wt_i32(inst->wasm_in_ptrs),
            wt_i32(inst->wasm_out_ptrs),
        };
        call_wasm(ctx, &inst->fn_compute, args, 4, nullptr, 0);

        // --- Copy WASM output scratch memory → CLAP output channels ---
        // Assumes a single CLAP audio output port with num_outputs channels.
        if (inst->num_outputs > 0 && proc->audio_outputs_count > 0) {
            clap_audio_buffer_t& out_buf = proc->audio_outputs[0];
            for (int32_t c = 0; c < inst->num_outputs; ++c) {
                const float* src = reinterpret_cast<const float*>(
                    mem + inst->wasm_out_samples + c * static_cast<int32_t>(inst->max_frames) * 4);
                if (out_buf.data32 && c < static_cast<int32_t>(out_buf.channel_count) &&
                    out_buf.data32[c])
                    std::memcpy(out_buf.data32[c], src, frames * sizeof(float));
            }
        }

        return CLAP_PROCESS_CONTINUE;
    }

    // ---------------------------------------------------------------------------
    // Audio ports extension (per-instance dispatch via plugin_data)
    // ---------------------------------------------------------------------------

    static uint32_t wp_ports_count(const clap_plugin_t* plugin, bool is_input) noexcept {
        const auto*   inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        const int32_t ch   = is_input ? inst->num_inputs : inst->num_outputs;
        return ch > 0 ? 1u : 0u;
    }

    static bool wp_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                             clap_audio_port_info_t* info) noexcept {
        if (index != 0)
            return false;
        const auto*   inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        const int32_t ch   = is_input ? inst->num_inputs : inst->num_outputs;
        if (ch <= 0)
            return false;

        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = static_cast<uint32_t>(ch);
        info->port_type     = (ch == 1) ? CLAP_PORT_MONO : (ch == 2) ? CLAP_PORT_STEREO : nullptr;
        info->in_place_pair = CLAP_INVALID_ID;

        const char* name = is_input ? "Input" : "Output";
        std::strncpy(info->name, name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        return true;
    }

    static const clap_plugin_audio_ports_t k_wasm_audio_ports{
        .count = wp_ports_count,
        .get   = wp_ports_get,
    };

    static const void* wp_get_extension(const clap_plugin_t*, const char* id) noexcept {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &k_wasm_audio_ports;
        return nullptr;
    }

    static void wp_on_main_thread(const clap_plugin_t*) noexcept {
    }

    // ---------------------------------------------------------------------------
    // Factory
    // ---------------------------------------------------------------------------

    static constexpr const char* k_bridge_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};

    static const clap_plugin_t* wb_factory_create(const clap_plugin_factory_t*, const clap_host_t*,
                                                  const char* plugin_id) noexcept {
        const size_t prefix_len = std::strlen(k_wasm_bridge_id_prefix);
        if (std::strncmp(plugin_id, k_wasm_bridge_id_prefix, prefix_len) != 0)
            return nullptr;

        auto* inst = new (std::nothrow) WasmBridgeInstance{};
        if (!inst)
            return nullptr;

        inst->plugin_id_str = plugin_id;
        inst->wasm_path     = plugin_id + prefix_len;

        inst->descriptor = {
            .clap_version = CLAP_VERSION_INIT,
            .id           = inst->plugin_id_str.c_str(),
            .name         = "WASM Bridge",
            .vendor       = "cljseq/kairos",
            .url          = "https://github.com/cljseq/kairos",
            .manual_url   = "",
            .support_url  = "",
            .version      = "0.1.0",
            .description  = "Faust WASM module loaded via wasmtime",
            .features     = k_bridge_features,
        };

        inst->plugin = {
            .desc             = &inst->descriptor,
            .plugin_data      = inst,
            .init             = wp_init,
            .destroy          = wp_destroy,
            .activate         = wp_activate,
            .deactivate       = wp_deactivate,
            .start_processing = wp_start_processing,
            .stop_processing  = wp_stop_processing,
            .reset            = wp_reset,
            .process          = wp_process,
            .get_extension    = wp_get_extension,
            .on_main_thread   = wp_on_main_thread,
        };

        return &inst->plugin;
    }

    static uint32_t wb_factory_count(const clap_plugin_factory_t*) noexcept {
        // Bridge instances are dynamically created; enumeration is not supported.
        return 0;
    }

    static const clap_plugin_descriptor_t* wb_factory_descriptor(const clap_plugin_factory_t*,
                                                                 uint32_t) noexcept {
        return nullptr;
    }

    static const clap_plugin_factory_t k_wasm_bridge_factory{
        .get_plugin_count      = wb_factory_count,
        .get_plugin_descriptor = wb_factory_descriptor,
        .create_plugin         = wb_factory_create,
    };

} // namespace

const clap_plugin_factory_t* get_wasm_bridge_factory() noexcept {
    return &k_wasm_bridge_factory;
}

} // namespace kairos
