// SPDX-License-Identifier: LGPL-2.1-or-later
// Host-side tests for the kairos/param-bus consumer path.
//
// Uses two plugins:
//   KAIROS_STUB_PLUGIN_PATH           — existing stub, no param-bus extension
//   KAIROS_STUB_PARAM_BUS_PLUGIN_PATH — stub with 2-port param-bus implementation

#include <catch2/catch_test_macros.hpp>

#include <kairos/clap_kairos_param_bus.h>
#include <kairos/plugin_host.hpp>
#include <kairos/plugin_instance.hpp>

// ---------------------------------------------------------------------------
// Plugins that do not expose param-bus
// ---------------------------------------------------------------------------

TEST_CASE("param-bus host: param_schema() returns null for non-param-bus plugin", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->param_schema() == nullptr);
}

TEST_CASE("param-bus host: set_param_frame() returns false for non-param-bus plugin",
          "[param_bus]") {
    auto inst = kairos::plugin_instance::load(KAIROS_STUB_PLUGIN_PATH, "org.nomos-studio.test/stub",
                                              kairos::kairos_host());
    REQUIRE(inst);

    float val = 1.f;
    REQUIRE(inst->set_param_frame(&val, 1) == false);
}

// ---------------------------------------------------------------------------
// Plugins that do expose param-bus
// ---------------------------------------------------------------------------

TEST_CASE("param-bus host: param_schema() non-null after init (engine built in init)",
          "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    // The stub builds its schema on activate(); before that epoch == 0.
    const auto* schema = inst->param_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->epoch == 0);
}

TEST_CASE("param-bus host: param_schema() populated after activate", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    const auto* schema = inst->param_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->epoch > 0);
    REQUIRE(schema->count == 2);
    REQUIRE(schema->entries != nullptr);
    REQUIRE(std::string(schema->entries[0].name) == "test/alpha");
    REQUIRE(std::string(schema->entries[1].name) == "test/beta");
    REQUIRE(schema->entries[0].id == 0);
    REQUIRE(schema->entries[1].id == 1);

    inst->deactivate();
}

TEST_CASE("param-bus host: epoch increments on re-activate", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));
    const uint32_t epoch1 = inst->param_schema()->epoch;

    inst->deactivate();
    REQUIRE(inst->activate(48000.0, 32, 512));
    const uint32_t epoch2 = inst->param_schema()->epoch;

    REQUIRE(epoch2 > epoch1);
    inst->deactivate();
}

TEST_CASE("param-bus host: set_param_frame() returns true after activate", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    float vals[2] = {0.3f, 0.7f};
    REQUIRE(inst->set_param_frame(vals, 2) == true);

    inst->deactivate();
}

TEST_CASE("param-bus host: epoch stable across set_param_frame() calls", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    const uint32_t epoch_before = inst->param_schema()->epoch;

    float vals[2] = {0.1f, 0.9f};
    for (int i = 0; i < 4; ++i)
        REQUIRE(inst->set_param_frame(vals, 2));

    REQUIRE(inst->param_schema()->epoch == epoch_before);
    inst->deactivate();
}

TEST_CASE("param-bus host: plugin_instance move preserves param_bus_ext", "[param_bus]") {
    auto inst = kairos::plugin_instance::load(
        KAIROS_STUB_PARAM_BUS_PLUGIN_PATH, "org.nomos.test/stub-param-bus", kairos::kairos_host());
    REQUIRE(inst);
    REQUIRE(inst->activate(48000.0, 32, 512));

    kairos::plugin_instance moved  = std::move(*inst);
    const auto*             schema = moved.param_schema();
    REQUIRE(schema != nullptr);
    REQUIRE(schema->count == 2);

    moved.deactivate();
}
