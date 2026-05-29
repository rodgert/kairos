// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal CLAP plugin that implements the kairos/param-bus extension.
// Used by test_param_bus_host.cpp to test the host-side param-bus consumer.

#include <kairos/clap_kairos_param_bus.h>
#include <clap/clap.h>

#include <cstring>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint32_t s_epoch = 0;

static clap_kairos_param_entry_t s_entries[2] = {
    {0, "test/alpha"},
    {1, "test/beta"},
};

static clap_kairos_param_schema_t s_schema{};

// Last frame written by set_param_frame() — readable from tests.
float g_stub_param_frame[2] = {0.f, 0.f};
uint32_t g_stub_param_count  = 0;

static void rebuild_schema() {
    ++s_epoch;
    s_schema.epoch   = s_epoch;
    s_schema.count   = 2;
    s_schema.entries = s_entries;
}

// ---------------------------------------------------------------------------
// param-bus vtable
// ---------------------------------------------------------------------------

static const clap_kairos_param_schema_t* param_get_schema(const clap_plugin_t*) {
    return &s_schema;
}

static bool param_set_param_frame(const clap_plugin_t*, const float* values, uint32_t count) {
    if (count == 0) return false;
    const uint32_t n = count < 2 ? count : 2;
    for (uint32_t i = 0; i < n; ++i)
        g_stub_param_frame[i] = values[i];
    g_stub_param_count = n;
    return true;
}

static const clap_plugin_param_bus_t s_param_bus_ext{
    .get_schema      = param_get_schema,
    .set_param_frame = param_set_param_frame,
};

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

static bool plugin_init(const clap_plugin_t*) {
    return true;
}
static void plugin_destroy(const clap_plugin_t*) {
}
static bool plugin_activate(const clap_plugin_t*, double, uint32_t, uint32_t) {
    rebuild_schema();
    return true;
}
static void plugin_deactivate(const clap_plugin_t*) {
}
static bool plugin_start_processing(const clap_plugin_t*) {
    return true;
}
static void plugin_stop_processing(const clap_plugin_t*) {
}
static void plugin_reset(const clap_plugin_t*) {
    rebuild_schema();
}
static clap_process_status plugin_process(const clap_plugin_t*, const clap_process_t*) {
    return CLAP_PROCESS_SLEEP;
}
static const void* plugin_get_extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_KAIROS_PARAM_BUS) == 0)
        return &s_param_bus_ext;
    return nullptr;
}
static void plugin_on_main_thread(const clap_plugin_t*) {
}

static const char* const s_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

static const clap_plugin_descriptor_t s_desc{
    .clap_version = CLAP_VERSION_INIT,
    .id           = "org.nomos.test/stub-param-bus",
    .name         = "Stub Param-Bus",
    .vendor       = "nomos-studio",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Minimal stub plugin with param-bus extension for kairos host tests",
    .features     = s_features,
};

static const clap_plugin_t s_plugin{
    .desc             = &s_desc,
    .plugin_data      = nullptr,
    .init             = plugin_init,
    .destroy          = plugin_destroy,
    .activate         = plugin_activate,
    .deactivate       = plugin_deactivate,
    .start_processing = plugin_start_processing,
    .stop_processing  = plugin_stop_processing,
    .reset            = plugin_reset,
    .process          = plugin_process,
    .get_extension    = plugin_get_extension,
    .on_main_thread   = plugin_on_main_thread,
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

static uint32_t factory_count(const clap_plugin_factory_t*) {
    return 1;
}
static const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, uint32_t) {
    return &s_desc;
}
static const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t*,
                                           const char*) {
    s_epoch            = 0;
    g_stub_param_frame[0] = 0.f;
    g_stub_param_frame[1] = 0.f;
    g_stub_param_count    = 0;
    return &s_plugin;
}

static const clap_plugin_factory_t s_factory{
    .get_plugin_count      = factory_count,
    .get_plugin_descriptor = factory_descriptor,
    .create_plugin         = factory_create,
};

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static bool entry_init(const char*) {
    return true;
}
static void entry_deinit() {
}
static const void* entry_get_factory(const char* id) {
    if (std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &s_factory;
    return nullptr;
}

extern "C" {
CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    .clap_version = CLAP_VERSION_INIT,
    .init         = entry_init,
    .deinit       = entry_deinit,
    .get_factory  = entry_get_factory,
};
}
