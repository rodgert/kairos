// SPDX-License-Identifier: LGPL-2.1-or-later
#include <catch2/catch_test_macros.hpp>

#include <kairos/graph.hpp>
#include <kairos/plugin_graph_manager.hpp>
#include <kairos/plugin_host.hpp>

TEST_CASE("plugin_graph_manager: load single node", "[plugin_graph]") {
    kairos::plugin_registry reg{
        {"org.nomos-studio.test/stub", kairos::plugin_info{.path = KAIROS_STUB_PLUGIN_PATH}}};

    kairos::plugin_graph graph{
        .nodes = {{edn::keyword{"test/node-1"}, "org.nomos-studio.test/stub", {}}}, .edges = {}};

    kairos::plugin_graph_manager mgr;
    auto                         result = mgr.load(graph, reg, kairos::kairos_host());

    REQUIRE(result);
    REQUIRE(mgr.node_count() == 1);
    REQUIRE(mgr.has_node(edn::keyword{"test/node-1"}));
}

TEST_CASE("plugin_graph_manager: unknown plugin returns load_failed", "[plugin_graph]") {
    kairos::plugin_registry reg{};

    kairos::plugin_graph graph{
        .nodes = {{edn::keyword{"test/node-1"}, "org.nomos-studio.dsp/unknown", {}}}, .edges = {}};

    kairos::plugin_graph_manager mgr;
    auto                         result = mgr.load(graph, reg, kairos::kairos_host());

    REQUIRE(!result);
    REQUIRE(result.error() == kairos::plugin_error::load_failed);
}

TEST_CASE("plugin_graph_manager: multi-node load and activate", "[plugin_graph]") {
    kairos::plugin_registry reg{
        {"org.nomos-studio.test/stub", kairos::plugin_info{.path = KAIROS_STUB_PLUGIN_PATH}}};

    kairos::plugin_graph graph{
        .nodes = {{edn::keyword{"test/voice"}, "org.nomos-studio.test/stub", {}},
                  {edn::keyword{"test/fx"}, "org.nomos-studio.test/stub", {}}},
        .edges = {{edn::keyword{"test/voice"}, edn::keyword{"audio/out-0"}, edn::keyword{"test/fx"},
                   edn::keyword{"audio/in-0"}}}};

    kairos::plugin_graph_manager mgr;
    REQUIRE(mgr.load(graph, reg, kairos::kairos_host()));
    REQUIRE(mgr.node_count() == 2);
    REQUIRE(mgr.has_node(edn::keyword{"test/voice"}));
    REQUIRE(mgr.has_node(edn::keyword{"test/fx"}));

    REQUIRE(mgr.activate(48000.0, 32, 512));
}

TEST_CASE("plugin_graph_manager: reset clears all nodes", "[plugin_graph]") {
    kairos::plugin_registry reg{
        {"org.nomos-studio.test/stub", kairos::plugin_info{.path = KAIROS_STUB_PLUGIN_PATH}}};

    kairos::plugin_graph graph{
        .nodes = {{edn::keyword{"test/node-1"}, "org.nomos-studio.test/stub", {}}}, .edges = {}};

    kairos::plugin_graph_manager mgr;
    REQUIRE(mgr.load(graph, reg, kairos::kairos_host()));
    mgr.reset();
    REQUIRE(mgr.node_count() == 0);
    REQUIRE(!mgr.has_node(edn::keyword{"test/node-1"}));
}

TEST_CASE("plugin_graph_manager: reload replaces previous graph", "[plugin_graph]") {
    kairos::plugin_registry reg{
        {"org.nomos-studio.test/stub", kairos::plugin_info{.path = KAIROS_STUB_PLUGIN_PATH}}};

    kairos::plugin_graph g1{.nodes = {{edn::keyword{"test/a"}, "org.nomos-studio.test/stub", {}},
                                      {edn::keyword{"test/b"}, "org.nomos-studio.test/stub", {}}},
                            .edges = {}};

    kairos::plugin_graph g2{.nodes = {{edn::keyword{"test/c"}, "org.nomos-studio.test/stub", {}}},
                            .edges = {}};

    kairos::plugin_graph_manager mgr;
    REQUIRE(mgr.load(g1, reg, kairos::kairos_host()));
    REQUIRE(mgr.node_count() == 2);

    REQUIRE(mgr.load(g2, reg, kairos::kairos_host()));
    REQUIRE(mgr.node_count() == 1);
    REQUIRE(!mgr.has_node(edn::keyword{"test/a"}));
    REQUIRE(mgr.has_node(edn::keyword{"test/c"}));
}
