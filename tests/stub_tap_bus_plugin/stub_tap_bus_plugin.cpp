// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal CLAP plugin that implements the kairos/tap-bus extension.
// Used by test_tap_bus_host.cpp to test the host-side tap-bus consumer.

#include <clap/clap.h>
#include <kairos/clap_kairos_tap_bus.h>

#include <cstring>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint32_t s_epoch = 0;

static clap_kairos_tap_entry_t s_entries[2] = {
    {0, "test/alpha"},
    {1, "test/beta"},
};

static clap_kairos_tap_schema_t s_schema{};

// Values written each process() — constant for predictable test assertions.
static constexpr float k_alpha        = 0.25f;
static constexpr float k_beta         = 0.75f;
static float           s_tap_frame[2] = {k_alpha, k_beta};

static void rebuild_schema() {
    ++s_epoch;
    s_schema.epoch   = s_epoch;
    s_schema.count   = 2;
    s_schema.entries = s_entries;
}

// ---------------------------------------------------------------------------
// tap-bus vtable
// ---------------------------------------------------------------------------

static const clap_kairos_tap_schema_t* tap_get_schema(const clap_plugin_t*) {
    return &s_schema;
}

static const float* tap_get_tap_frame(const clap_plugin_t*, uint32_t* out_count) {
    if (out_count)
        *out_count = 2;
    return s_tap_frame;
}

static const clap_plugin_tap_bus_t s_tap_bus_ext{
    .get_schema    = tap_get_schema,
    .get_tap_frame = tap_get_tap_frame,
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
    if (std::strcmp(id, CLAP_EXT_KAIROS_TAP_BUS) == 0)
        return &s_tap_bus_ext;
    return nullptr;
}
static void plugin_on_main_thread(const clap_plugin_t*) {
}

static const char* const s_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

static const clap_plugin_descriptor_t s_desc{
    .clap_version = CLAP_VERSION_INIT,
    .id           = "org.nomos.test/stub-tap-bus",
    .name         = "Stub Tap-Bus",
    .vendor       = "nomos-studio",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Minimal stub plugin with tap-bus extension for kairos host tests",
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
    s_epoch = 0;
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
