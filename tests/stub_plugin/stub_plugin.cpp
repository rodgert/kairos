// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Minimal CLAP plugin for kairos integration tests.
// Does nothing in process(); exercises the full lifecycle path.

#include <clap/clap.h>

#include <cstring>

static bool stub_init(const clap_plugin_t*) {
    return true;
}
static void stub_destroy(const clap_plugin_t*) {
}
static bool stub_activate(const clap_plugin_t*, double, uint32_t, uint32_t) {
    return true;
}
static void stub_deactivate(const clap_plugin_t*) {
}
static bool stub_start_processing(const clap_plugin_t*) {
    return true;
}
static void stub_stop_processing(const clap_plugin_t*) {
}
static void stub_reset(const clap_plugin_t*) {
}
static const void* stub_get_extension(const clap_plugin_t*, const char*) {
    return nullptr;
}
static void stub_on_main_thread(const clap_plugin_t*) {
}

static clap_process_status stub_process(const clap_plugin_t*, const clap_process_t*) {
    return CLAP_PROCESS_SLEEP;
}

static const char* const stub_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr};

static const clap_plugin_descriptor_t stub_desc{
    .clap_version = CLAP_VERSION_INIT,
    .id           = "org.nomos-studio.test/stub",
    .name         = "Stub",
    .vendor       = "nomos-studio",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Minimal stub plugin for kairos tests",
    .features     = stub_features,
};

static const clap_plugin_t stub_plugin_inst{
    .desc             = &stub_desc,
    .plugin_data      = nullptr,
    .init             = stub_init,
    .destroy          = stub_destroy,
    .activate         = stub_activate,
    .deactivate       = stub_deactivate,
    .start_processing = stub_start_processing,
    .stop_processing  = stub_stop_processing,
    .reset            = stub_reset,
    .process          = stub_process,
    .get_extension    = stub_get_extension,
    .on_main_thread   = stub_on_main_thread,
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

static uint32_t factory_count(const clap_plugin_factory_t*) {
    return 1;
}

static const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, uint32_t) {
    return &stub_desc;
}

static const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t*,
                                           const char*) {
    return &stub_plugin_inst;
}

static const clap_plugin_factory_t stub_factory{
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
        return &stub_factory;
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
