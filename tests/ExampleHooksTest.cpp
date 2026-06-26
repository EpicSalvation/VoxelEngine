// Verifies the example-hooks plugin (plugins/example-hooks) registers — and that
// its callbacks fire through — every hook the other example plugins don't already
// demonstrate, so the example-plugin suite covers all major hook types (M17).
//
// The plugin is compiled into the test binary (see CMakeLists) and wired in via
// PluginManager::wireInPlugin, the same approach KinematicBodyTest uses, so the
// plugin's example_hooks::observed() singleton resolves in one address space.

#include "core/PluginManager.h"
#include "example_hooks.h"
#include "plugin_api.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>

#include <cstdlib>

// The plugin's compiled-in entry point (its generic voxel_plugin_init is
// suppressed under EXAMPLE_HOOKS_COMPILED_IN).
extern "C" int example_hooks_plugin_init(PluginContext* ctx);

namespace {

class ExampleHooks : public ::testing::Test {
protected:
    void SetUp() override { example_hooks::observed() = {}; }  // clean slate per test
};

}  // namespace

// Every otherwise-unexampled hook lands in its PluginManager registry under the
// plugin's ownership.
TEST_F(ExampleHooks, RegistersEveryDemonstratedHook) {
    PluginManager pm;
    const PluginId id = pm.wireInPlugin(example_hooks_plugin_init);
    ASSERT_NE(id, kInvalidPluginId);

    // Generation: a custom noise field, resolvable by id and owned by this plugin.
    const RegisteredNoise* noise = pm.resolveNoise("example_ridges");
    ASSERT_NE(noise, nullptr);
    EXPECT_EQ(noise->owner, id);

    // Field-event observers + a point light source.
    ASSERT_EQ(pm.thermalEventHooks().size(),  1u);
    ASSERT_EQ(pm.lightingEventHooks().size(), 1u);
    ASSERT_EQ(pm.lightSources().size(),       1u);
    EXPECT_FLOAT_EQ(pm.lightSources()[0].brightness, 1.0f);

    // Chunk lifecycle observers, keyed to a layer.
    ASSERT_EQ(pm.chunkCreatedHooks().size(), 1u);
    ASSERT_EQ(pm.chunkEvictedHooks().size(), 1u);
    EXPECT_EQ(pm.chunkCreatedHooks()[0].layer_name, "terrain");
    EXPECT_EQ(pm.chunkEvictedHooks()[0].layer_name, "terrain");

    // IO export handler for the custom extension.
    bool foundExporter = false;
    for (const auto& e : pm.exporters())
        if (e.extension == ".ehv") foundExporter = true;
    EXPECT_TRUE(foundExporter);

    // Networking interest filter.
    ASSERT_EQ(pm.interestFilters().size(), 1u);

    // Unloading tears every one of those registrations back down (the §8 owner
    // teardown contract) — no dangling callbacks left behind.
    ASSERT_TRUE(pm.unloadPlugin(id));
    EXPECT_EQ(pm.resolveNoise("example_ridges"), nullptr);
    EXPECT_TRUE(pm.thermalEventHooks().empty());
    EXPECT_TRUE(pm.lightingEventHooks().empty());
    EXPECT_TRUE(pm.lightSources().empty());
    EXPECT_TRUE(pm.chunkCreatedHooks().empty());
    EXPECT_TRUE(pm.chunkEvictedHooks().empty());
    EXPECT_TRUE(pm.interestFilters().empty());
    bool exporterStillThere = false;
    for (const auto& e : pm.exporters())
        if (e.extension == ".ehv") exporterStillThere = true;
    EXPECT_FALSE(exporterStillThere);
}

// Invoking each registered callback the way its engine subsystem would proves the
// signatures and the user_data threading are correct end to end.
TEST_F(ExampleHooks, CallbacksRunWhenFired) {
    PluginManager pm;
    const PluginId id = pm.wireInPlugin(example_hooks_plugin_init);
    ASSERT_NE(id, kInvalidPluginId);

    // Noise is pure (same inputs → same output) and normalized to [0,1].
    const RegisteredNoise* noise = pm.resolveNoise("example_ridges");
    ASSERT_NE(noise, nullptr);
    const float a = noise->fn(WorldCoord(10.0, 0.0, 20.0), 1234, nullptr, 0, noise->user_data);
    const float b = noise->fn(WorldCoord(10.0, 0.0, 20.0), 1234, nullptr, 0, noise->user_data);
    EXPECT_EQ(a, b);
    EXPECT_GE(a, 0.0f);
    EXPECT_LE(a, 1.0f);

    // Thermal / lighting observers record into the shared Observed state via user_data.
    ThermalFieldEvent te{};
    te.temperature = 80.0f;
    pm.thermalEventHooks()[0].fn(&te, pm.thermalEventHooks()[0].user_data);
    EXPECT_EQ(example_hooks::observed().thermalEvents, 1);
    EXPECT_FLOAT_EQ(example_hooks::observed().lastTemperature, 80.0f);

    LightingEvent le{};
    le.brightness = 0.7f;
    pm.lightingEventHooks()[0].fn(&le, pm.lightingEventHooks()[0].user_data);
    EXPECT_EQ(example_hooks::observed().lightingEvents, 1);
    EXPECT_FLOAT_EQ(example_hooks::observed().lastBrightness, 0.7f);

    // Chunk lifecycle.
    pm.chunkCreatedHooks()[0].fn(WorldCoord(0.0, 0.0, 0.0), pm.chunkCreatedHooks()[0].user_data);
    pm.chunkEvictedHooks()[0].fn(WorldCoord(0.0, 0.0, 0.0), pm.chunkEvictedHooks()[0].user_data);
    EXPECT_EQ(example_hooks::observed().chunksCreated, 1);
    EXPECT_EQ(example_hooks::observed().chunksEvicted, 1);

    // Interest filter forwards (returns true) and records the check.
    const auto& filter = pm.interestFilters()[0];
    EXPECT_TRUE(filter.fn(7, WorldCoord(1.0, 2.0, 3.0), filter.user_data));
    EXPECT_EQ(example_hooks::observed().interestChecks, 1);

    // Exporter writes the custom ".ehv" format; the engine owns the freeing.
    ExporterFn exporter = nullptr;
    void*      exporterUser = nullptr;
    for (const auto& e : pm.exporters())
        if (e.extension == ".ehv") { exporter = e.fn; exporterUser = e.user_data; }
    ASSERT_NE(exporter, nullptr);

    Voxel grid[8]{};  // a 2³ region
    grid[0].material.palette_index = 5;
    uint8_t* out = nullptr;
    size_t   outSize = 0;
    const int rc = exporter(grid, 2, WorldCoord(0.0, 0.0, 0.0), &out, &outSize, exporterUser);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(outSize, 8u + 8u);   // 8-byte header + 2³ palette bytes
    EXPECT_EQ(out[0], 'E');
    EXPECT_EQ(out[4], 2);          // grid size, little-endian
    EXPECT_EQ(out[8], 5);          // first voxel's palette_index
    std::free(out);
}
