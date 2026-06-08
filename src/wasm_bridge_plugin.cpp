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
// Phase 2 — module cache + CLAP params via Faust JSON metadata.
// Phase 3 — hot-swap via RCU (gapless; no stop/start cycle):
//   WasmDspState holds all wasmtime resources plus param metadata for one DSP
//   snapshot. active_dsp is an atomic pointer published by the control thread
//   using release semantics and loaded by the audio thread using acquire.
//   do_hot_swap(): build new state → store with release → synchronize_rcu
//   (blocks control thread ≤ one audio block) → free old state.
//   The audio thread holds rcu_read_lock/unlock around each access so
//   synchronize_rcu() correctly waits for any in-flight process() call.
//
// Module cache:
//   wasmtime_module_t is JIT-compiled code with no mutable state.
//   WasmModuleCache holds one wasm_engine_t and one compiled module per path.
//   Per-instance stores borrow the engine; the cache outlives all stores.

#include "wasm_bridge_plugin.hpp"

#include <nomos/rt/rcu.hpp>

#include <clap/audio-buffer.h>
#include <clap/events.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/params.h>
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

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos {

namespace {

    static constexpr uint32_t k_wasm_page_size = 65536u;

    // ---------------------------------------------------------------------------
    // Faust math import callbacks.
    //
    // Faust 2.80+ externalises standard math functions as "env._sinf" etc.
    // Each callback is a distinct static function; no void*↔function-pointer
    // casting required.
    // ---------------------------------------------------------------------------

#define FAUST_MATH_CB_1_1(name__, fn__)                                                            \
    static wasm_trap_t* name__(void*, wasmtime_caller_t*, const wasmtime_val_t* a, size_t,         \
                               wasmtime_val_t* r, size_t) noexcept {                               \
        r[0].kind   = WASMTIME_F32;                                                                \
        r[0].of.f32 = fn__(a[0].of.f32);                                                           \
        return nullptr;                                                                            \
    }
#define FAUST_MATH_CB_2_1(name__, fn__)                                                            \
    static wasm_trap_t* name__(void*, wasmtime_caller_t*, const wasmtime_val_t* a, size_t,         \
                               wasmtime_val_t* r, size_t) noexcept {                               \
        r[0].kind   = WASMTIME_F32;                                                                \
        r[0].of.f32 = fn__(a[0].of.f32, a[1].of.f32);                                              \
        return nullptr;                                                                            \
    }

    FAUST_MATH_CB_1_1(cb_sinf, std::sinf)
    FAUST_MATH_CB_1_1(cb_cosf, std::cosf)
    FAUST_MATH_CB_1_1(cb_tanf, std::tanf)
    FAUST_MATH_CB_1_1(cb_expf, std::expf)
    FAUST_MATH_CB_1_1(cb_logf, std::logf)
    FAUST_MATH_CB_1_1(cb_log10f, std::log10f)
    FAUST_MATH_CB_1_1(cb_sqrtf, std::sqrtf)
    FAUST_MATH_CB_1_1(cb_fabsf, std::fabsf)
    FAUST_MATH_CB_1_1(cb_floorf, std::floorf)
    FAUST_MATH_CB_1_1(cb_ceilf, std::ceilf)
    FAUST_MATH_CB_1_1(cb_roundf, std::roundf)
    FAUST_MATH_CB_1_1(cb_asinf, std::asinf)
    FAUST_MATH_CB_1_1(cb_acosf, std::acosf)
    FAUST_MATH_CB_1_1(cb_atanf, std::atanf)
    FAUST_MATH_CB_2_1(cb_powf, std::powf)
    FAUST_MATH_CB_2_1(cb_fmodf, std::fmodf)
    FAUST_MATH_CB_2_1(cb_atan2f, std::atan2f)
    FAUST_MATH_CB_2_1(cb_fminf, std::fminf)
    FAUST_MATH_CB_2_1(cb_fmaxf, std::fmaxf)

#undef FAUST_MATH_CB_1_1
#undef FAUST_MATH_CB_2_1

    static void linker_def_f32_f32(wasmtime_linker_t* linker, const char* name,
                                   wasmtime_func_callback_t cb) noexcept {
        wasm_functype_t* ft = wasm_functype_new_1_1(wasm_valtype_new_f32(), wasm_valtype_new_f32());
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "env", 3, name, std::strlen(name), ft, cb, nullptr, nullptr);
        wasm_functype_delete(ft);
        if (err)
            wasmtime_error_delete(err);
    }

    static void linker_def_f32_f32_f32(wasmtime_linker_t* linker, const char* name,
                                       wasmtime_func_callback_t cb) noexcept {
        wasm_functype_t*  ft = wasm_functype_new_2_1(wasm_valtype_new_f32(), wasm_valtype_new_f32(),
                                                     wasm_valtype_new_f32());
        wasmtime_error_t* err = wasmtime_linker_define_func(
            linker, "env", 3, name, std::strlen(name), ft, cb, nullptr, nullptr);
        wasm_functype_delete(ft);
        if (err)
            wasmtime_error_delete(err);
    }

    // ---------------------------------------------------------------------------
    // Module cache
    // ---------------------------------------------------------------------------

    struct WasmModuleCache {
        wasm_engine_t*                                      engine;
        wasmtime_linker_t*                                  linker;
        std::mutex                                          mtx;
        std::unordered_map<std::string, wasmtime_module_t*> modules;

        static WasmModuleCache& get() noexcept {
            static WasmModuleCache inst;
            return inst;
        }

        wasmtime_module_t* get_or_compile(const std::string& path, const uint8_t* bytes,
                                          size_t nbytes) noexcept {
            std::lock_guard<std::mutex> lock(mtx);
            auto                        it = modules.find(path);
            if (it != modules.end())
                return it->second;

            wasmtime_module_t* mod = nullptr;
            wasmtime_error_t*  err = wasmtime_module_new(engine, bytes, nbytes, &mod);
            if (err) {
                wasmtime_error_delete(err);
                return nullptr;
            }
            modules.emplace(path, mod);
            return mod;
        }

      private:
        WasmModuleCache() : engine(wasm_engine_new()), linker(wasmtime_linker_new(engine)) {
            linker_def_f32_f32(linker, "_sinf", cb_sinf);
            linker_def_f32_f32(linker, "_cosf", cb_cosf);
            linker_def_f32_f32(linker, "_tanf", cb_tanf);
            linker_def_f32_f32(linker, "_expf", cb_expf);
            linker_def_f32_f32(linker, "_logf", cb_logf);
            linker_def_f32_f32(linker, "_log10f", cb_log10f);
            linker_def_f32_f32(linker, "_sqrtf", cb_sqrtf);
            linker_def_f32_f32(linker, "_fabsf", cb_fabsf);
            linker_def_f32_f32(linker, "_floorf", cb_floorf);
            linker_def_f32_f32(linker, "_ceilf", cb_ceilf);
            linker_def_f32_f32(linker, "_roundf", cb_roundf);
            linker_def_f32_f32(linker, "_asinf", cb_asinf);
            linker_def_f32_f32(linker, "_acosf", cb_acosf);
            linker_def_f32_f32(linker, "_atanf", cb_atanf);
            linker_def_f32_f32_f32(linker, "_powf", cb_powf);
            linker_def_f32_f32_f32(linker, "_fmodf", cb_fmodf);
            linker_def_f32_f32_f32(linker, "_atan2f", cb_atan2f);
            linker_def_f32_f32_f32(linker, "_fminf", cb_fminf);
            linker_def_f32_f32_f32(linker, "_fmaxf", cb_fmaxf);
        }
        ~WasmModuleCache() {
            for (auto& [path, mod] : modules)
                wasmtime_module_delete(mod);
            wasmtime_linker_delete(linker);
            wasm_engine_delete(engine);
        }
    };

    // ---------------------------------------------------------------------------
    // Parameter metadata
    // ---------------------------------------------------------------------------

    struct ParamInfo {
        clap_id id;
        int32_t wasm_addr;
        double  min_val;
        double  max_val;
        double  default_val;
        double  step;
        char    label[CLAP_NAME_SIZE];
    };

    static std::vector<ParamInfo> load_params(const std::string& wasm_path) {
        std::string json_path = wasm_path;
        const auto  dot       = json_path.rfind('.');
        if (dot != std::string::npos)
            json_path.replace(dot, std::string::npos, ".json");
        else
            json_path += ".json";

        std::ifstream f(json_path);
        if (!f.is_open())
            return {};

        nlohmann::json root;
        try {
            f >> root;
        } catch (...) {
            return {};
        }

        std::vector<ParamInfo> params;
        clap_id                next_id = 0;

        static const char* k_leaf_types[] = {"hslider", "vslider",  "nentry",
                                             "button",  "checkbox", "knob"};

        std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& node) {
            try {
                if (!node.is_object())
                    return;
                const auto type = node.value("type", std::string{});
                for (const char* lt : k_leaf_types) {
                    if (type == lt) {
                        ParamInfo p{};
                        p.id           = next_id++;
                        p.wasm_addr    = node.value("index", 0);
                        p.min_val      = node.value("min", 0.0);
                        p.max_val      = node.value("max", 1.0);
                        p.default_val  = node.value("init", 0.0);
                        p.step         = node.value("step", 0.0);
                        const auto lbl = node.value("label", std::string{});
                        std::strncpy(p.label, lbl.c_str(), sizeof(p.label) - 1);
                        p.label[sizeof(p.label) - 1] = '\0';
                        params.push_back(p);
                        return;
                    }
                }
                if (node.contains("items")) {
                    for (const auto& item : node["items"])
                        walk(item);
                }
            } catch (...) {
            }
        };

        try {
            if (root.contains("ui")) {
                for (const auto& group : root["ui"])
                    walk(group);
            }
        } catch (...) {
        }

        return params;
    }

    // Carry over shadow values by label match; new params get Faust defaults.
    static std::vector<double> migrate_params(const std::vector<ParamInfo>& new_params,
                                              const std::vector<ParamInfo>& old_params,
                                              const std::vector<double>&    old_values) {
        std::vector<double> result(new_params.size());
        for (size_t i = 0; i < new_params.size(); ++i) {
            result[i] = new_params[i].default_val;
            for (size_t j = 0; j < old_params.size(); ++j) {
                if (std::strcmp(new_params[i].label, old_params[j].label) == 0) {
                    result[i] = std::max(new_params[i].min_val,
                                         std::min(new_params[i].max_val, old_values[j]));
                    break;
                }
            }
        }
        return result;
    }

    // ---------------------------------------------------------------------------
    // WasmDspState — all wasmtime resources + param state for one DSP snapshot.
    //
    // Lifetime:
    //   make_dsp()     — heap-allocates; caches wasmtime exports; queries I/O counts
    //   activate_dsp() — calls Faust init, applies shadow params, maps scratch memory
    //   free_dsp()     — deletes the object (store freed, module is cache-owned)
    //
    // RCU contract:
    //   Written exclusively by the control/main thread (make → activate → store).
    //   Read by the audio thread inside rcu_read_lock/unlock.
    //   param_values elements are written via std::atomic_ref<double> so audio and
    //   main threads can update/read individual values without a full pointer swap.
    // ---------------------------------------------------------------------------

    struct WasmDspState {
        wasmtime_store_t*   store{nullptr};
        wasmtime_instance_t instance{};
        wasmtime_memory_t   wasm_memory{};
        wasmtime_func_t     fn_init{};
        wasmtime_func_t     fn_compute{};
        wasmtime_func_t     fn_get_num_inputs{};
        wasmtime_func_t     fn_get_num_outputs{};
        wasmtime_func_t     fn_set_param_value{};
        bool                has_set_param{false};
        int32_t             num_inputs{0};
        int32_t             num_outputs{0};
        uint32_t            max_frames{0};
        int32_t             wasm_in_ptrs{0};
        int32_t             wasm_out_ptrs{0};
        int32_t             wasm_in_samples{0};
        int32_t             wasm_out_samples{0};
        bool                ready{false}; // true after activate_dsp

        // Param metadata and shadow values; owned by this snapshot.
        // mutable so atomic_ref<double> can be constructed from const WasmDspState*.
        std::vector<ParamInfo>      params;
        mutable std::vector<double> param_values;
    };

    // ---------------------------------------------------------------------------
    // wasmtime call helpers
    // ---------------------------------------------------------------------------

    static inline wasmtime_val_t wt_i32(int32_t v) noexcept {
        wasmtime_val_t val{};
        val.kind   = WASMTIME_I32;
        val.of.i32 = v;
        return val;
    }

    static inline wasmtime_val_t wt_f32(float v) noexcept {
        wasmtime_val_t val{};
        val.kind   = WASMTIME_F32;
        val.of.f32 = v;
        return val;
    }

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
    // WasmDspState lifecycle helpers
    // ---------------------------------------------------------------------------

    // Heap-allocate a WasmDspState, load wasm_path via the module cache, create a
    // store+instance, and cache all exported function handles.
    // Does NOT call Faust init() or set ready — that is done in activate_dsp.
    static WasmDspState* make_dsp(const std::string& wasm_path) noexcept {
        FILE* f = std::fopen(wasm_path.c_str(), "rb");
        if (!f)
            return nullptr;
        std::fseek(f, 0, SEEK_END);
        const long file_size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (file_size <= 0) {
            std::fclose(f);
            return nullptr;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
        std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);

        WasmModuleCache&   cache  = WasmModuleCache::get();
        wasmtime_module_t* module = cache.get_or_compile(wasm_path, bytes.data(), bytes.size());
        if (!module)
            return nullptr;

        auto* dsp = new (std::nothrow) WasmDspState{};
        if (!dsp)
            return nullptr;

        dsp->store = wasmtime_store_new(cache.engine, nullptr, nullptr);
        if (!dsp->store) {
            delete dsp;
            return nullptr;
        }

        wasmtime_context_t* ctx  = wasmtime_store_context(dsp->store);
        wasm_trap_t*        trap = nullptr;
        wasmtime_error_t*   err =
            wasmtime_linker_instantiate(cache.linker, ctx, module, &dsp->instance, &trap);
        if (err) {
            wasmtime_error_delete(err);
            wasmtime_store_delete(dsp->store);
            delete dsp;
            return nullptr;
        }
        if (trap) {
            wasm_trap_delete(trap);
            wasmtime_store_delete(dsp->store);
            delete dsp;
            return nullptr;
        }

        if (!get_export_func(ctx, &dsp->instance, "init", &dsp->fn_init) ||
            !get_export_func(ctx, &dsp->instance, "compute", &dsp->fn_compute) ||
            !get_export_func(ctx, &dsp->instance, "getNumInputs", &dsp->fn_get_num_inputs) ||
            !get_export_func(ctx, &dsp->instance, "getNumOutputs", &dsp->fn_get_num_outputs) ||
            !get_export_memory(ctx, &dsp->instance, "memory", &dsp->wasm_memory)) {
            wasmtime_store_delete(dsp->store);
            delete dsp;
            return nullptr;
        }

        dsp->has_set_param =
            get_export_func(ctx, &dsp->instance, "setParamValue", &dsp->fn_set_param_value);

        wasmtime_val_t dsp_arg = wt_i32(0);
        wasmtime_val_t result{};
        call_wasm(ctx, &dsp->fn_get_num_inputs, &dsp_arg, 1, &result, 1);
        dsp->num_inputs = result.of.i32;
        call_wasm(ctx, &dsp->fn_get_num_outputs, &dsp_arg, 1, &result, 1);
        dsp->num_outputs = result.of.i32;

        return dsp;
    }

    // Call Faust init, apply shadow params, grow WASM memory, write channel pointer arrays.
    // dsp.params and dsp.param_values must already be populated before this call.
    // Requires make_dsp() to have succeeded and sample_rate/max_frames to be valid.
    static bool activate_dsp(WasmDspState& dsp, double sample_rate, uint32_t max_frames) noexcept {
        wasmtime_context_t* ctx = wasmtime_store_context(dsp.store);

        wasmtime_val_t init_args[2] = {wt_i32(0), wt_i32(static_cast<int32_t>(sample_rate))};
        call_wasm(ctx, &dsp.fn_init, init_args, 2, nullptr, 0);

        // Apply shadow values so pre-activate host changes survive Faust's init reset.
        if (dsp.has_set_param) {
            for (size_t i = 0; i < dsp.params.size(); ++i) {
                const double   v       = dsp.param_values[i];
                wasmtime_val_t args[3] = {
                    wt_i32(0),
                    wt_i32(dsp.params[i].wasm_addr),
                    wt_f32(static_cast<float>(v)),
                };
                call_wasm(ctx, &dsp.fn_set_param_value, args, 3, nullptr, 0);
            }
        }

        dsp.max_frames = max_frames;

        uint64_t          prev_pages = 0;
        wasmtime_error_t* err        = wasmtime_memory_grow(ctx, &dsp.wasm_memory, 1, &prev_pages);
        if (err) {
            wasmtime_error_delete(err);
            return false;
        }

        const int32_t base  = static_cast<int32_t>(prev_pages * k_wasm_page_size);
        dsp.wasm_in_ptrs    = base;
        dsp.wasm_out_ptrs   = base + dsp.num_inputs * 4;
        dsp.wasm_in_samples = dsp.wasm_out_ptrs + dsp.num_outputs * 4;
        dsp.wasm_out_samples =
            dsp.wasm_in_samples + dsp.num_inputs * static_cast<int32_t>(max_frames) * 4;

        uint8_t* mem = wasmtime_memory_data(ctx, &dsp.wasm_memory);
        for (int32_t c = 0; c < dsp.num_inputs; ++c) {
            int32_t addr = dsp.wasm_in_samples + c * static_cast<int32_t>(max_frames) * 4;
            std::memcpy(mem + dsp.wasm_in_ptrs + c * 4, &addr, 4);
        }
        for (int32_t c = 0; c < dsp.num_outputs; ++c) {
            int32_t addr = dsp.wasm_out_samples + c * static_cast<int32_t>(max_frames) * 4;
            std::memcpy(mem + dsp.wasm_out_ptrs + c * 4, &addr, 4);
        }

        dsp.ready = true;
        return true;
    }

    // Free the wasmtime store and delete the heap-allocated WasmDspState.
    // The module is cache-owned and must not be freed here.
    static void free_dsp(WasmDspState* dsp) noexcept {
        if (!dsp)
            return;
        if (dsp->store)
            wasmtime_store_delete(dsp->store);
        delete dsp;
    }

    // ---------------------------------------------------------------------------
    // Per-instance state
    // ---------------------------------------------------------------------------

    struct WasmBridgeInstance {
        clap_plugin_t            plugin{};
        clap_plugin_descriptor_t descriptor{};
        std::string              plugin_id_str;
        std::string              wasm_path;
        const clap_host_t*       host{nullptr};

        // RCU-protected DSP state. Written by control/main thread; read by audio thread.
        // Use std::memory_order_release on stores, acquire on loads, and wrap all
        // audio-thread access in rcu_detail::read_lock/unlock so synchronize_rcu()
        // correctly waits for any in-flight process() to finish before freeing the old state.
        std::atomic<WasmDspState*> active_dsp{nullptr};

        // Saved at activate time for DSP reinit during hot-swap.
        double   sample_rate{0.0};
        uint32_t max_frames{0};

        bool instance_ready{false}; // set after wp_init
    };

    // ---------------------------------------------------------------------------
    // Parameter helpers
    // ---------------------------------------------------------------------------

    static void apply_param(WasmDspState* dsp, wasmtime_context_t* ctx, clap_id id,
                            double value) noexcept {
        if (id >= static_cast<clap_id>(dsp->params.size()))
            return;
        std::atomic_ref<double>(dsp->param_values[id]).store(value, std::memory_order_relaxed);
        if (dsp->has_set_param && ctx) {
            wasmtime_val_t args[3] = {
                wt_i32(0),
                wt_i32(dsp->params[id].wasm_addr),
                wt_f32(static_cast<float>(value)),
            };
            call_wasm(ctx, &dsp->fn_set_param_value, args, 3, nullptr, 0);
        }
    }

    static void apply_param_events(WasmDspState* dsp, wasmtime_context_t* ctx,
                                   const clap_input_events_t* in_events) noexcept {
        if (!in_events || dsp->params.empty())
            return;
        const uint32_t count = in_events->size(in_events);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* hdr = in_events->get(in_events, i);
            if (!hdr || hdr->type != CLAP_EVENT_PARAM_VALUE)
                continue;
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            apply_param(dsp, ctx, ev->param_id, ev->value);
        }
    }

    // ---------------------------------------------------------------------------
    // Hot-swap implementation — gapless via RCU
    // ---------------------------------------------------------------------------

    // Build a new DSP snapshot for new_path, activate it with current audio config,
    // then atomically install it via release store + synchronize_rcu().
    // The control thread blocks for at most one audio block (~5 ms) while
    // synchronize_rcu() drains any in-flight rcu_read_lock sections.
    // Returns true on success; on failure the old DSP is left untouched.
    static bool do_hot_swap(WasmBridgeInstance* inst, const std::string& new_path) noexcept {
        WasmDspState* new_dsp = make_dsp(new_path);
        if (!new_dsp)
            return false;

        // Load metadata and migrate shadow values before topology check so we can
        // atomically read the old values with atomic_ref (avoids data race with
        // audio thread's concurrent atomic_ref stores to the same elements).
        new_dsp->params = load_params(new_path);
        new_dsp->param_values.resize(new_dsp->params.size());

        WasmDspState* old_dsp = inst->active_dsp.load(std::memory_order_acquire);

        if (old_dsp) {
            // Reject topology changes — CLAP audio-ports contract fixed at activate.
            if (new_dsp->num_inputs != old_dsp->num_inputs ||
                new_dsp->num_outputs != old_dsp->num_outputs) {
                free_dsp(new_dsp);
                return false;
            }
            // Snapshot old shadow values with atomic reads to avoid data race.
            std::vector<double> old_values(old_dsp->params.size());
            for (size_t i = 0; i < old_values.size(); ++i)
                old_values[i] = std::atomic_ref<double>(old_dsp->param_values[i])
                                    .load(std::memory_order_relaxed);
            new_dsp->param_values = migrate_params(new_dsp->params, old_dsp->params, old_values);
        } else {
            for (size_t i = 0; i < new_dsp->params.size(); ++i)
                new_dsp->param_values[i] = new_dsp->params[i].default_val;
        }

        if (!activate_dsp(*new_dsp, inst->sample_rate, inst->max_frames)) {
            free_dsp(new_dsp);
            return false;
        }

        // Publish new DSP with release semantics; the audio thread's acquire load
        // will see all preceding writes (init + param values + ready flag).
        old_dsp = inst->active_dsp.exchange(new_dsp, std::memory_order_acq_rel);

        // Block until all rcu_read_lock sections that began before the exchange
        // have completed. After this, no thread is reading old_dsp.
        rcu_detail::synchronize();

        free_dsp(old_dsp);
        inst->wasm_path = new_path;
        return true;
    }

    // ---------------------------------------------------------------------------
    // CLAP plugin callbacks
    // ---------------------------------------------------------------------------

    static bool wp_init(const clap_plugin_t* plugin) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);

        WasmDspState* dsp = make_dsp(inst->wasm_path);
        if (!dsp)
            return false;

        dsp->params = load_params(inst->wasm_path);
        dsp->param_values.resize(dsp->params.size());
        for (size_t i = 0; i < dsp->params.size(); ++i)
            dsp->param_values[i] = dsp->params[i].default_val;

        inst->active_dsp.store(dsp, std::memory_order_release);
        inst->instance_ready = true;
        return true;
    }

    static void wp_destroy(const clap_plugin_t* plugin) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        // Drain any in-flight readers before freeing the DSP.
        rcu_detail::synchronize();
        free_dsp(inst->active_dsp.load(std::memory_order_acquire));
        delete inst;
    }

    static bool wp_activate(const clap_plugin_t* plugin, double sample_rate,
                            uint32_t /*min_frames*/, uint32_t   max_frames) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        if (!inst->instance_ready)
            return false;

        inst->sample_rate = sample_rate;
        inst->max_frames  = max_frames;

        WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        if (!dsp)
            return false;

        return activate_dsp(*dsp, sample_rate, max_frames);
    }

    static void wp_deactivate(const clap_plugin_t* plugin) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        // CLAP guarantees deactivate is called after stop_processing — no concurrent
        // audio thread access, so direct modification without a read lock is safe.
        WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        if (dsp)
            dsp->ready = false;
    }

    static bool wp_start_processing(const clap_plugin_t*) noexcept {
        return true;
    }
    static void wp_stop_processing(const clap_plugin_t*) noexcept {
    }

    static void wp_reset(const clap_plugin_t*) noexcept {
        // Hot-swap is now fully atomic via RCU (do_hot_swap in wp_hot_swap_request);
        // reset has no pending work to perform.
    }

    static clap_process_status wp_process(const clap_plugin_t*  plugin,
                                          const clap_process_t* proc) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);

        rcu_detail::read_lock();
        WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        if (!dsp || !dsp->ready) {
            rcu_detail::read_unlock();
            return CLAP_PROCESS_CONTINUE;
        }

        wasmtime_context_t* ctx    = wasmtime_store_context(dsp->store);
        uint8_t*            mem    = wasmtime_memory_data(ctx, &dsp->wasm_memory);
        const uint32_t      frames = proc->frames_count;

        apply_param_events(dsp, ctx, proc->in_events);

        if (dsp->num_inputs > 0 && proc->audio_inputs_count > 0) {
            const clap_audio_buffer_t& in_buf = proc->audio_inputs[0];
            for (int32_t c = 0; c < dsp->num_inputs; ++c) {
                float* dst = reinterpret_cast<float*>(
                    mem + dsp->wasm_in_samples + c * static_cast<int32_t>(dsp->max_frames) * 4);
                if (in_buf.data32 && c < static_cast<int32_t>(in_buf.channel_count) &&
                    in_buf.data32[c])
                    std::memcpy(dst, in_buf.data32[c], frames * sizeof(float));
                else
                    std::memset(dst, 0, frames * sizeof(float));
            }
        }

        wasmtime_val_t args[4] = {
            wt_i32(0),
            wt_i32(static_cast<int32_t>(frames)),
            wt_i32(dsp->wasm_in_ptrs),
            wt_i32(dsp->wasm_out_ptrs),
        };
        call_wasm(ctx, &dsp->fn_compute, args, 4, nullptr, 0);

        if (dsp->num_outputs > 0 && proc->audio_outputs_count > 0) {
            clap_audio_buffer_t& out_buf = proc->audio_outputs[0];
            for (int32_t c = 0; c < dsp->num_outputs; ++c) {
                const float* src = reinterpret_cast<const float*>(
                    mem + dsp->wasm_out_samples + c * static_cast<int32_t>(dsp->max_frames) * 4);
                if (out_buf.data32 && c < static_cast<int32_t>(out_buf.channel_count) &&
                    out_buf.data32[c])
                    std::memcpy(out_buf.data32[c], src, frames * sizeof(float));
            }
        }

        rcu_detail::read_unlock();
        return CLAP_PROCESS_CONTINUE;
    }

    // ---------------------------------------------------------------------------
    // CLAP params extension
    // ---------------------------------------------------------------------------

    static uint32_t wp_params_get_count(const clap_plugin_t* plugin) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        uint32_t            n   = dsp ? static_cast<uint32_t>(dsp->params.size()) : 0u;
        rcu_detail::read_unlock();
        return n;
    }

    static bool wp_params_get_info(const clap_plugin_t* plugin, uint32_t index,
                                   clap_param_info_t* out) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        bool                ok  = false;
        if (dsp && index < static_cast<uint32_t>(dsp->params.size())) {
            const ParamInfo& p = dsp->params[index];
            out->id            = p.id;
            out->flags         = CLAP_PARAM_IS_AUTOMATABLE;
            out->cookie        = nullptr;
            out->min_value     = p.min_val;
            out->max_value     = p.max_val;
            out->default_value = p.default_val;
            std::strncpy(out->name, p.label, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            std::memset(out->module, 0, sizeof(out->module));
            ok = true;
        }
        rcu_detail::read_unlock();
        return ok;
    }

    static bool wp_params_get_value(const clap_plugin_t* plugin, clap_id id,
                                    double* out_value) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        bool                ok  = false;
        if (dsp && id < static_cast<clap_id>(dsp->params.size())) {
            *out_value =
                std::atomic_ref<double>(dsp->param_values[id]).load(std::memory_order_relaxed);
            ok = true;
        }
        rcu_detail::read_unlock();
        return ok;
    }

    static bool wp_params_value_to_text(const clap_plugin_t* plugin, clap_id id, double value,
                                        char* buf, uint32_t capacity) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        bool                ok  = false;
        if (dsp && id < static_cast<clap_id>(dsp->params.size())) {
            std::snprintf(buf, capacity, "%.6g", value);
            ok = true;
        }
        rcu_detail::read_unlock();
        return ok;
    }

    static bool wp_params_text_to_value(const clap_plugin_t* plugin, clap_id id, const char* text,
                                        double* out_value) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        bool                ok  = false;
        if (dsp && id < static_cast<clap_id>(dsp->params.size())) {
            char* end  = nullptr;
            *out_value = std::strtod(text, &end);
            ok         = (end != text);
        }
        rcu_detail::read_unlock();
        return ok;
    }

    static void wp_params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in,
                                const clap_output_events_t* /*out*/) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        if (!inst->instance_ready || !in)
            return;

        rcu_detail::read_lock();
        WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        if (!dsp || dsp->params.empty()) {
            rcu_detail::read_unlock();
            return;
        }

        // Shadow values are always updated; WASM write deferred until DSP is activated.
        wasmtime_context_t* ctx   = dsp->ready ? wasmtime_store_context(dsp->store) : nullptr;
        const uint32_t      count = in->size(in);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* hdr = in->get(in, i);
            if (!hdr || hdr->type != CLAP_EVENT_PARAM_VALUE)
                continue;
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
            if (ev->param_id >= static_cast<clap_id>(dsp->params.size()))
                continue;
            std::atomic_ref<double>(dsp->param_values[ev->param_id])
                .store(ev->value, std::memory_order_relaxed);
            if (ctx && dsp->has_set_param) {
                wasmtime_val_t args[3] = {
                    wt_i32(0),
                    wt_i32(dsp->params[ev->param_id].wasm_addr),
                    wt_f32(static_cast<float>(ev->value)),
                };
                call_wasm(ctx, &dsp->fn_set_param_value, args, 3, nullptr, 0);
            }
        }

        rcu_detail::read_unlock();
    }

    static const clap_plugin_params_t k_wasm_params{
        .count         = wp_params_get_count,
        .get_info      = wp_params_get_info,
        .get_value     = wp_params_get_value,
        .value_to_text = wp_params_value_to_text,
        .text_to_value = wp_params_text_to_value,
        .flush         = wp_params_flush,
    };

    // ---------------------------------------------------------------------------
    // Audio ports extension
    // ---------------------------------------------------------------------------

    static uint32_t wp_ports_count(const clap_plugin_t* plugin, bool is_input) noexcept {
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        uint32_t            n   = 0;
        if (dsp) {
            const int32_t ch = is_input ? dsp->num_inputs : dsp->num_outputs;
            n                = ch > 0 ? 1u : 0u;
        }
        rcu_detail::read_unlock();
        return n;
    }

    static bool wp_ports_get(const clap_plugin_t* plugin, uint32_t index, bool is_input,
                             clap_audio_port_info_t* info) noexcept {
        if (index != 0)
            return false;
        const auto* inst = static_cast<const WasmBridgeInstance*>(plugin->plugin_data);
        rcu_detail::read_lock();
        const WasmDspState* dsp = inst->active_dsp.load(std::memory_order_acquire);
        bool                ok  = false;
        if (dsp) {
            const int32_t ch = is_input ? dsp->num_inputs : dsp->num_outputs;
            if (ch > 0) {
                info->id            = 0;
                info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
                info->channel_count = static_cast<uint32_t>(ch);
                info->port_type     = (ch == 1)   ? CLAP_PORT_MONO
                                      : (ch == 2) ? CLAP_PORT_STEREO
                                                  : nullptr;
                info->in_place_pair = CLAP_INVALID_ID;
                const char* name    = is_input ? "Input" : "Output";
                std::strncpy(info->name, name, sizeof(info->name) - 1);
                info->name[sizeof(info->name) - 1] = '\0';
                ok                                 = true;
            }
        }
        rcu_detail::read_unlock();
        return ok;
    }

    static const clap_plugin_audio_ports_t k_wasm_audio_ports{
        .count = wp_ports_count,
        .get   = wp_ports_get,
    };

    // ---------------------------------------------------------------------------
    // Hot-swap extension
    // ---------------------------------------------------------------------------

    static bool wp_hot_swap_request(const clap_plugin_t* plugin, const char* new_path) noexcept {
        auto* inst = static_cast<WasmBridgeInstance*>(plugin->plugin_data);
        return do_hot_swap(inst, std::string{new_path});
    }

    static const kairos_hot_swap_t k_wasm_hot_swap{
        .request = wp_hot_swap_request,
    };

    // ---------------------------------------------------------------------------
    // get_extension dispatcher
    // ---------------------------------------------------------------------------

    static const void* wp_get_extension(const clap_plugin_t*, const char* id) noexcept {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &k_wasm_audio_ports;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &k_wasm_params;
        if (std::strcmp(id, k_kairos_ext_hot_swap) == 0)
            return &k_wasm_hot_swap;
        return nullptr;
    }

    static void wp_on_main_thread(const clap_plugin_t*) noexcept {
    }

    // ---------------------------------------------------------------------------
    // Factory
    // ---------------------------------------------------------------------------

    static constexpr const char* k_bridge_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};

    static const clap_plugin_t* wb_factory_create(const clap_plugin_factory_t*,
                                                  const clap_host_t* host,
                                                  const char*        plugin_id) noexcept {
        const size_t prefix_len = std::strlen(k_wasm_bridge_id_prefix);
        if (std::strncmp(plugin_id, k_wasm_bridge_id_prefix, prefix_len) != 0)
            return nullptr;

        auto* inst = new (std::nothrow) WasmBridgeInstance{};
        if (!inst)
            return nullptr;

        inst->plugin_id_str = plugin_id;
        inst->wasm_path     = plugin_id + prefix_len;
        inst->host          = host;

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
