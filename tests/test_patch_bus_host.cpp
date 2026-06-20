// SPDX-License-Identifier: LGPL-2.1-or-later
// Host-side tests for the kairos/patch-bus consumer path.
//
// Uses two plugins:
//   KAIROS_STUB_PLUGIN_PATH               — existing stub, no patch-bus extension
//   KAIROS_STUB_PATCH_BUS_PLUGIN_PATH     — stub with patch-bus implementation

#include <catch2/catch_test_macros.hpp>

#include <kairos/clap_kairos_patch_bus.h>
#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

// ---------------------------------------------------------------------------
// Plugins that do not expose patch-bus
// ---------------------------------------------------------------------------

TEST_CASE("patch-bus host: push_patch() returns false for non-patch-bus plugin", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    const char* edn = "{:modules [] :cables []}";
    REQUIRE(inst->push_patch(edn, static_cast<uint32_t>(std::strlen(edn))) == false);
}

TEST_CASE("patch-bus host: get_patch() returns null for non-patch-bus plugin", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->get_patch() == nullptr);
}

// ---------------------------------------------------------------------------
// Plugins that do expose patch-bus
// ---------------------------------------------------------------------------

TEST_CASE("patch-bus host: get_patch() returns null before any push", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PATCH_BUS_PLUGIN_PATH, "org.nomos.test/stub-patch-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->get_patch() == nullptr);
}

TEST_CASE("patch-bus host: push_patch() returns true for non-null descriptor", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PATCH_BUS_PLUGIN_PATH, "org.nomos.test/stub-patch-bus", kairos::kairos_host());
    REQUIRE(inst);

    const char* edn = "{:modules [{:type \"env\"}] :cables []}";
    REQUIRE(inst->push_patch(edn, static_cast<uint32_t>(std::strlen(edn))) == true);
}

TEST_CASE("patch-bus host: get_patch() returns stored descriptor after push", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PATCH_BUS_PLUGIN_PATH, "org.nomos.test/stub-patch-bus", kairos::kairos_host());
    REQUIRE(inst);

    const char*    edn = "{:modules [{:type \"env\"} {:type \"audio-out\"}] :cables []}";
    const uint32_t len = static_cast<uint32_t>(std::strlen(edn));
    REQUIRE(inst->push_patch(edn, len));

    const char* stored = inst->get_patch();
    REQUIRE(stored != nullptr);
    REQUIRE(std::string(stored) == std::string(edn));
}

TEST_CASE("patch-bus host: push_patch() returns false for null descriptor", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PATCH_BUS_PLUGIN_PATH, "org.nomos.test/stub-patch-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->push_patch(nullptr, 0) == false);
    REQUIRE(inst->push_patch("", 0) == false);
}

TEST_CASE("patch-bus host: plugin_instance move preserves patch_bus_ext", "[patch_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PATCH_BUS_PLUGIN_PATH, "org.nomos.test/stub-patch-bus", kairos::kairos_host());
    REQUIRE(inst);

    const char* edn = "{:modules [] :cables []}";
    REQUIRE(inst->push_patch(edn, static_cast<uint32_t>(std::strlen(edn))));

    kairos::plugin_instance moved = std::move(*inst);
    REQUIRE(moved.get_patch() != nullptr);
    REQUIRE(std::string(moved.get_patch()) == std::string(edn));
}
