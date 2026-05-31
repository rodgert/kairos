// SPDX-License-Identifier: GPL-2.0-or-later
#include "plugin_discovery.hpp"

#include <clap/entry.h>
#include <clap/factory/plugin-factory.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace kairos {

namespace {

    // Returns the binary path to dlopen for a .clap item.
    // macOS bundles are directories: MyPlugin.clap/Contents/MacOS/MyPlugin.
    // Linux .clap files are plain shared libraries.
    std::string binary_for_clap(const std::filesystem::path& p) {
#ifdef __APPLE__
        return (p / "Contents" / "MacOS" / p.stem()).string();
#else
        return p.string();
#endif
    }

    void probe_clap(const std::filesystem::path& p, plugin_registry& out) {
        const std::string binary = binary_for_clap(p);

        void* handle = ::dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            return; // not a valid CLAP binary — skip silently

        auto* sym = reinterpret_cast<const clap_plugin_entry_t*>(::dlsym(handle, "clap_entry"));
        if (!sym || !sym->init(binary.c_str())) {
            ::dlclose(handle);
            return;
        }

        auto* factory =
            static_cast<const clap_plugin_factory_t*>(sym->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (factory) {
            const uint32_t n = factory->get_plugin_count(factory);
            for (uint32_t i = 0; i < n; ++i) {
                const auto* desc = factory->get_plugin_descriptor(factory, i);
                if (!desc || !desc->id || desc->id[0] == '\0')
                    continue;
                plugin_info info;
                info.path        = binary;
                info.name        = desc->name        ? desc->name        : "";
                info.vendor      = desc->vendor      ? desc->vendor      : "";
                info.version     = desc->version     ? desc->version     : "";
                info.description = desc->description ? desc->description : "";
                out.emplace(std::string{desc->id}, std::move(info));
            }
        }

        sym->deinit();
        ::dlclose(handle);
    }

    void scan_dir(const std::filesystem::path& dir, plugin_registry& out) {
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
            if (de.path().extension() == ".clap")
                probe_clap(de.path(), out);
        }
    }

    std::vector<std::filesystem::path> platform_paths() {
        std::vector<std::filesystem::path> paths;

#ifdef __APPLE__
        if (const char* home = std::getenv("HOME"))
            paths.emplace_back(std::filesystem::path{home} / "Library/Audio/Plug-Ins/CLAP");
        paths.emplace_back("/Library/Audio/Plug-Ins/CLAP");
#else
        // Linux / other POSIX
        if (const char* home = std::getenv("HOME"))
            paths.emplace_back(std::filesystem::path{home} / ".clap");
        paths.emplace_back("/usr/lib/clap");
        paths.emplace_back("/usr/local/lib/clap");
        // CLAP_PATH: colon-separated list of additional directories.
        if (const char* env = std::getenv("CLAP_PATH")) {
            std::string s{env};
            for (std::size_t pos = 0; pos < s.size();) {
                const auto end = s.find(':', pos);
                const auto seg = (end == std::string::npos) ? s.size() : end;
                if (seg > pos)
                    paths.emplace_back(s.substr(pos, seg - pos));
                pos = (end == std::string::npos) ? s.size() : end + 1;
            }
        }
#endif

        return paths;
    }

} // namespace

plugin_registry discover_plugins(const std::vector<std::string>& extra_paths) {
    plugin_registry out;

    auto dirs = platform_paths();
    for (const auto& p : extra_paths)
        dirs.emplace_back(p);

    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            continue;
        const std::size_t before = out.size();
        scan_dir(dir, out);
        const std::size_t found = out.size() - before;
        if (found > 0)
            std::fprintf(stderr, "[discover] %s: %zu plugin(s)\n", dir.c_str(), found);
    }

    return out;
}

} // namespace kairos
