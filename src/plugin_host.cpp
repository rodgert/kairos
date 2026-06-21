// SPDX-License-Identifier: LGPL-2.1-or-later
#include <kairos/plugin_host.hpp>

#include <clap/version.h>

namespace kairos {

namespace {

    const void* host_get_extension(const clap_host_t*, const char*) noexcept {
        return nullptr;
    }

    void host_request_restart(const clap_host_t*) noexcept {
    }
    void host_request_process(const clap_host_t*) noexcept {
    }
    void host_request_callback(const clap_host_t*) noexcept {
    }

    static const clap_host_t k_host{
        .clap_version     = CLAP_VERSION_INIT,
        .host_data        = nullptr,
        .name             = "kairos",
        .vendor           = "nomos-studio",
        .url              = "https://github.com/nomos-studio/kairos",
        .version          = "0.1.0",
        .get_extension    = host_get_extension,
        .request_restart  = host_request_restart,
        .request_process  = host_request_process,
        .request_callback = host_request_callback,
    };

} // namespace

const clap_host_t* kairos_host() noexcept {
    return &k_host;
}

} // namespace kairos
