// SPDX-License-Identifier: LGPL-2.1-or-later
#include <kairos/plugin_instance.hpp>
#include <kairos/clap_kairos_hot_swap.h>
#include <kairos/clap_kairos_param_bus.h>
#include <kairos/clap_kairos_patch_bus.h>
#include <kairos/clap_kairos_tap_bus.h>

#include "builtin_plugins.hpp"
#ifdef KAIROS_WASM_BRIDGE
#include "wasm_bridge_plugin.hpp"
#endif

#include <clap/factory/plugin-factory.h>

#include <dlfcn.h>
#include <utility>

namespace kairos {

using namespace nomos::rt;

namespace {

    std::vector<clap_audio_port_info_t> query_audio_ports(const clap_plugin_t* plugin,
                                                          bool                 is_input) {
        const auto* ext = static_cast<const clap_plugin_audio_ports_t*>(
            plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        if (!ext)
            return {};
        const uint32_t                      n = ext->count(plugin, is_input);
        std::vector<clap_audio_port_info_t> ports(n);
        for (uint32_t i = 0; i < n; ++i)
            ext->get(plugin, i, is_input, &ports[i]);
        return ports;
    }

    const clap_plugin_tap_bus_t* query_tap_bus(const clap_plugin_t* plugin) {
        return static_cast<const clap_plugin_tap_bus_t*>(
            plugin->get_extension(plugin, CLAP_EXT_KAIROS_TAP_BUS));
    }

    const clap_plugin_param_bus_t* query_param_bus(const clap_plugin_t* plugin) {
        return static_cast<const clap_plugin_param_bus_t*>(
            plugin->get_extension(plugin, CLAP_EXT_KAIROS_PARAM_BUS));
    }

    const clap_plugin_patch_bus_t* query_patch_bus(const clap_plugin_t* plugin) {
        return static_cast<const clap_plugin_patch_bus_t*>(
            plugin->get_extension(plugin, CLAP_EXT_KAIROS_PATCH_BUS));
    }

    const clap_plugin_t* create_builtin(const clap_host_t* host, const std::string& plugin_id) {
#ifdef KAIROS_WASM_BRIDGE
        if (plugin_id.starts_with(k_wasm_bridge_id_prefix)) {
            const auto* factory = get_wasm_bridge_factory();
            return factory->create_plugin(factory, host, plugin_id.c_str());
        }
#endif
        const auto* factory = get_builtin_factory();
        return factory->create_plugin(factory, host, plugin_id.c_str());
    }

} // namespace

result<plugin_instance, plugin_error> plugin_instance::load(const std::string& path,
                                                            const std::string& plugin_id,
                                                            const clap_host_t* host) {
    // Built-in plugins bypass dlopen — the factory lives in-process.
    if (path.starts_with(k_builtin_path_prefix)) {
        const clap_plugin_t* plugin = create_builtin(host, plugin_id);
        if (!plugin)
            return unexpected<plugin_error>{plugin_error::create_failed};
        if (!plugin->init(plugin))
            return unexpected<plugin_error>{plugin_error::plugin_init_failed};
        plugin_instance inst;
        inst.lib_handle_     = nullptr;
        inst.entry_          = nullptr;
        inst.plugin_         = plugin;
        inst.state_          = state::initialized;
        inst.tap_bus_ext_    = query_tap_bus(plugin);
        inst.param_bus_ext_  = query_param_bus(plugin);
        inst.patch_bus_ext_  = query_patch_bus(plugin);
        inst.in_ports_       = query_audio_ports(plugin, true);
        inst.out_ports_      = query_audio_ports(plugin, false);
        return inst;
    }

    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        return unexpected<plugin_error>{plugin_error::load_failed};

    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(::dlsym(handle, "clap_entry"));
    if (!entry) {
        ::dlclose(handle);
        return unexpected<plugin_error>{plugin_error::entry_not_found};
    }

    if (!entry->init(path.c_str())) {
        ::dlclose(handle);
        return unexpected<plugin_error>{plugin_error::init_failed};
    }

    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        entry->deinit();
        ::dlclose(handle);
        return unexpected<plugin_error>{plugin_error::factory_not_found};
    }

    const clap_plugin_t* plugin = factory->create_plugin(factory, host, plugin_id.c_str());
    if (!plugin) {
        entry->deinit();
        ::dlclose(handle);
        return unexpected<plugin_error>{plugin_error::create_failed};
    }

    if (!plugin->init(plugin)) {
        plugin->destroy(plugin);
        entry->deinit();
        ::dlclose(handle);
        return unexpected<plugin_error>{plugin_error::plugin_init_failed};
    }

    plugin_instance inst;
    inst.lib_handle_     = handle;
    inst.entry_          = entry;
    inst.plugin_         = plugin;
    inst.state_          = state::initialized;
    inst.tap_bus_ext_    = query_tap_bus(plugin);
    inst.param_bus_ext_  = query_param_bus(plugin);
    inst.patch_bus_ext_  = query_patch_bus(plugin);
    inst.in_ports_       = query_audio_ports(plugin, true);
    inst.out_ports_      = query_audio_ports(plugin, false);
    return inst;
}

plugin_instance::~plugin_instance() {
    teardown();
}

plugin_instance::plugin_instance(plugin_instance&& o) noexcept
    : lib_handle_(std::exchange(o.lib_handle_, nullptr)), entry_(std::exchange(o.entry_, nullptr)),
      plugin_(std::exchange(o.plugin_, nullptr)), state_(o.state_),
      tap_bus_ext_(std::exchange(o.tap_bus_ext_, nullptr)),
      param_bus_ext_(std::exchange(o.param_bus_ext_, nullptr)),
      patch_bus_ext_(std::exchange(o.patch_bus_ext_, nullptr)),
      in_ports_(std::move(o.in_ports_)), out_ports_(std::move(o.out_ports_)) {
}

plugin_instance& plugin_instance::operator=(plugin_instance&& o) noexcept {
    if (this != &o) {
        teardown();
        lib_handle_     = std::exchange(o.lib_handle_, nullptr);
        entry_          = std::exchange(o.entry_, nullptr);
        plugin_         = std::exchange(o.plugin_, nullptr);
        state_          = o.state_;
        tap_bus_ext_    = std::exchange(o.tap_bus_ext_, nullptr);
        param_bus_ext_  = std::exchange(o.param_bus_ext_, nullptr);
        patch_bus_ext_  = std::exchange(o.patch_bus_ext_, nullptr);
        in_ports_       = std::move(o.in_ports_);
        out_ports_      = std::move(o.out_ports_);
    }
    return *this;
}

void plugin_instance::teardown() noexcept {
    if (!plugin_)
        return;
    if (state_ == state::processing)
        plugin_->stop_processing(plugin_);
    if (state_ == state::processing || state_ == state::activated)
        plugin_->deactivate(plugin_);
    plugin_->destroy(plugin_);
    plugin_ = nullptr;
    if (entry_) {
        entry_->deinit();
        entry_ = nullptr;
    }
    if (lib_handle_) {
        ::dlclose(lib_handle_);
        lib_handle_ = nullptr;
    }
}

result<std::monostate, plugin_error>
plugin_instance::activate(double sample_rate, uint32_t min_frames, uint32_t max_frames) {
    if (state_ != state::initialized)
        return unexpected<plugin_error>{plugin_error::wrong_state};
    if (!plugin_->activate(plugin_, sample_rate, min_frames, max_frames))
        return unexpected<plugin_error>{plugin_error::activate_failed};
    state_ = state::activated;
    return std::monostate{};
}

result<std::monostate, plugin_error> plugin_instance::start_processing() {
    if (state_ != state::activated)
        return unexpected<plugin_error>{plugin_error::wrong_state};
    if (!plugin_->start_processing(plugin_))
        return unexpected<plugin_error>{plugin_error::processing_failed};
    state_ = state::processing;
    return std::monostate{};
}

clap_process_status plugin_instance::process(const clap_process_t& proc) {
    return plugin_->process(plugin_, &proc);
}

void plugin_instance::stop_processing() {
    if (state_ != state::processing)
        return;
    plugin_->stop_processing(plugin_);
    state_ = state::activated;
}

void plugin_instance::deactivate() {
    if (state_ != state::activated)
        return;
    plugin_->deactivate(plugin_);
    state_ = state::initialized;
}

result<std::monostate, plugin_error> plugin_instance::hot_swap(const std::string& new_wasm_path) {
    if (state_ != state::activated && state_ != state::processing)
        return unexpected<plugin_error>{plugin_error::wrong_state};

    // Works for any plugin implementing CLAP_EXT_KAIROS_HOT_SWAP — both
    // wasm_bridge_plugin and kairos-grid's WasmGridModule slot.
    const auto* ext = static_cast<const clap_kairos_hot_swap_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_KAIROS_HOT_SWAP));
    if (!ext)
        return unexpected<plugin_error>{plugin_error::hot_swap_unsupported};

    if (!ext->request(plugin_, new_wasm_path.c_str()))
        return unexpected<plugin_error>{plugin_error::hot_swap_failed};

    return std::monostate{};
}

const clap_kairos_tap_schema_t* plugin_instance::tap_schema() const noexcept {
    if (!tap_bus_ext_ || !plugin_)
        return nullptr;
    return tap_bus_ext_->get_schema(plugin_);
}

const float* plugin_instance::tap_frame(uint32_t* out_count) const noexcept {
    if (!tap_bus_ext_ || !plugin_) {
        if (out_count)
            *out_count = 0;
        return nullptr;
    }
    return tap_bus_ext_->get_tap_frame(plugin_, out_count);
}

const clap_kairos_param_schema_t* plugin_instance::param_schema() const noexcept {
    if (!param_bus_ext_ || !plugin_)
        return nullptr;
    return param_bus_ext_->get_schema(plugin_);
}

bool plugin_instance::set_param_frame(const float* values, uint32_t count) const noexcept {
    if (!param_bus_ext_ || !plugin_)
        return false;
    return param_bus_ext_->set_param_frame(plugin_, values, count);
}

bool plugin_instance::push_patch(const char* edn_descriptor, uint32_t len) const noexcept {
    if (!patch_bus_ext_ || !plugin_)
        return false;
    return patch_bus_ext_->push_patch(plugin_, edn_descriptor, len);
}

const char* plugin_instance::get_patch() const noexcept {
    if (!patch_bus_ext_ || !plugin_)
        return nullptr;
    return patch_bus_ext_->get_patch(plugin_);
}

const clap_plugin_descriptor_t* plugin_instance::descriptor() const noexcept {
    return plugin_ ? plugin_->desc : nullptr;
}

plugin_instance::state plugin_instance::current_state() const noexcept {
    return state_;
}

} // namespace kairos
