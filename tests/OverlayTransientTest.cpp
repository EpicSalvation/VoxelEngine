// M14 — Overlay-is-transient + teardown invariant (docs/ARCHITECTURE.md §17).
//
// The fluid and thermal overlays are transient working state, not persisted in
// the §9 chunk format. These tests confirm:
//
//   - save/load re-derives the fields from re-registered emitters + existing
//     fluid voxels with no §9 chunk-format change;
//   - registered sources and event hooks are gone after the owning plugin unloads
//     (the M4 teardown contract).

#include <gtest/gtest.h>

#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "simulation/FluidSystem.h"
#include "simulation/ThermalSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

LayerConfig terminalOnly() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 16
)");
}

Voxel solidMat() {
    Voxel v;
    v.material.density              = 2000.0f;
    v.material.thermal_conductivity = 0.5f;
    v.material.porosity             = 1.0f;
    v.material.palette_index        = 1;
    return v;
}

MaterialProperties waterProps() {
    MaterialProperties w;
    w.density       = 1000.0f;
    w.porosity      = 1.0f;
    w.palette_index = 5;
    return w;
}

int g_hookCalls = 0;
void hookCounter(const FluidEvent*, void*) { ++g_hookCalls; }
void thermalHookCounter(const ThermalFieldEvent*, void*) { ++g_hookCalls; }

}  // namespace

// Registered sources are gone after the owning plugin unloads — the M4 teardown
// contract applied to the fluid/thermal source registries.
TEST(OverlayTransient, SourcesTornDownOnPluginUnload) {
    PluginManager pm;

    auto sourceInit = [](PluginContext* c) -> int {
        MaterialProperties w;
        w.density       = 1000.0f;
        w.porosity      = 1.0f;
        w.palette_index = 5;
        c->register_material(c, "water", w);
        c->register_heat_source(c, WorldCoord(1.0, 1.0, 1.0), 50.0f);
        c->register_fluid_source(c, WorldCoord(2.0, 2.0, 2.0), 10.0f, "water");
        return 0;
    };

    PluginId pid = pm.wireInPlugin(sourceInit);
    ASSERT_NE(pid, kInvalidPluginId);
    EXPECT_EQ(pm.heatSources().size(), 1u);
    EXPECT_EQ(pm.fluidSources().size(), 1u);

    pm.unloadPlugin(pid);
    EXPECT_EQ(pm.heatSources().size(), 0u)
        << "heat sources must be torn down when the owning plugin unloads";
    EXPECT_EQ(pm.fluidSources().size(), 0u)
        << "fluid sources must be torn down when the owning plugin unloads";
}

// Registered event hooks are gone after the owning plugin unloads.
TEST(OverlayTransient, EventHooksTornDownOnPluginUnload) {
    PluginManager pm;

    auto hookInit = [](PluginContext* c) -> int {
        c->register_on_fluid_event(c, hookCounter, nullptr);
        c->register_on_thermal_event(c, thermalHookCounter, nullptr);
        return 0;
    };

    PluginId pid = pm.wireInPlugin(hookInit);
    ASSERT_NE(pid, kInvalidPluginId);
    EXPECT_EQ(pm.fluidEventHooks().size(), 1u);
    EXPECT_EQ(pm.thermalEventHooks().size(), 1u);

    pm.unloadPlugin(pid);
    EXPECT_EQ(pm.fluidEventHooks().size(), 0u)
        << "fluid event hooks must be torn down when the owning plugin unloads";
    EXPECT_EQ(pm.thermalEventHooks().size(), 0u)
        << "thermal event hooks must be torn down when the owning plugin unloads";
}

// Multiple plugins: unloading one tears down only its own registrations, leaving
// the other's intact (the per-owner teardown contract).
TEST(OverlayTransient, PerOwnerTeardownIsSelective) {
    PluginManager pm;

    auto initA = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(0.0, 0.0, 0.0), 10.0f);
        return 0;
    };
    auto initB = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(5.0, 5.0, 5.0), 20.0f);
        return 0;
    };

    PluginId a = pm.wireInPlugin(initA);
    PluginId b = pm.wireInPlugin(initB);
    ASSERT_EQ(pm.heatSources().size(), 2u);

    pm.unloadPlugin(a);
    EXPECT_EQ(pm.heatSources().size(), 1u)
        << "unloading plugin A must remove only A's heat source";
    EXPECT_FLOAT_EQ(pm.heatSources()[0].rate, 20.0f)
        << "plugin B's source must survive A's unload";

    pm.unloadPlugin(b);
    EXPECT_EQ(pm.heatSources().size(), 0u);
}

// The overlay is transient: after evicting and re-loading a chunk, the fluid
// system re-derives its state from the existing voxels via seedChunk.
TEST(OverlayTransient, FluidOverlayReDerivesOnChunkReload) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::FluidSystem fluid(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    // Register a fluid material so the system can recognize fluid voxels.
    auto matInit = [](PluginContext* c) -> int {
        MaterialProperties w;
        w.density       = 1000.0f;
        w.porosity      = 1.0f;
        w.palette_index = 5;
        c->register_material(c, "water", w);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 10.0f, "water");
        return 0;
    };
    pm.wireInPlugin(matInit);

    const double cvs = world.layer("terrain")->voxelSizeM();

    // Place a fluid voxel directly (simulating a "saved" fluid voxel that
    // persists through the §9 chunk format as a normal voxel).
    Voxel waterVoxel;
    waterVoxel.material = waterProps();
    world.layer("terrain")->setVoxel(
        chunkmath::voxelCenter({3, 3, 3}, cvs), waterVoxel);

    // Seed the chunk — the fluid system should recognize the water voxel and
    // seed its overlay with kSaturationThreshold.
    fluid.seedChunk(ChunkCoord{0, 0, 0});

    const float amount = fluid.amountAt(
        chunkmath::voxelCenter({3, 3, 3}, cvs));
    EXPECT_GE(amount, tuning::fluid::kSaturationThreshold)
        << "seedChunk must recognize existing fluid voxels and seed the overlay "
           "at kSaturationThreshold — the overlay re-derives from voxels, not "
           "from serialized field data";
}

// Drop and re-seed: evicting a chunk clears the overlay cells, and re-seeding
// re-derives them — the round-trip is lossless from the voxels' perspective.
TEST(OverlayTransient, DropAndReseedRoundTrip) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::FluidSystem fluid(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    auto matInit = [](PluginContext* c) -> int {
        MaterialProperties w;
        w.density       = 1000.0f;
        w.porosity      = 1.0f;
        w.palette_index = 5;
        c->register_material(c, "water", w);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 10.0f, "water");
        return 0;
    };
    pm.wireInPlugin(matInit);

    const double cvs = world.layer("terrain")->voxelSizeM();
    Voxel waterVoxel;
    waterVoxel.material = waterProps();
    world.layer("terrain")->setVoxel(
        chunkmath::voxelCenter({5, 5, 5}, cvs), waterVoxel);

    fluid.seedChunk(ChunkCoord{0, 0, 0});
    ASSERT_GT(fluid.activeCount(), 0u);

    // Drop the chunk — all overlay cells inside it must be cleared.
    fluid.dropChunk(ChunkCoord{0, 0, 0});
    EXPECT_FLOAT_EQ(fluid.amountAt(chunkmath::voxelCenter({5, 5, 5}, cvs)), 0.0f)
        << "dropChunk must clear the overlay cell";

    // Re-seed from the still-resident voxels.
    fluid.seedChunk(ChunkCoord{0, 0, 0});
    EXPECT_GE(fluid.amountAt(chunkmath::voxelCenter({5, 5, 5}, cvs)),
              tuning::fluid::kSaturationThreshold)
        << "re-seeding must re-derive the overlay from the surviving voxel";
}

// ThermalSystem has no seedChunk (heat re-derives from emitters, not voxels),
// but it must still produce a live field after re-registration. Unloading and
// re-wiring a heat-source plugin results in the thermal system producing heat
// again on subsequent ticks.
TEST(OverlayTransient, ThermalReDerivesFromReRegisteredEmitters) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::ThermalSystem thermal(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    const double cvs = world.layer("terrain")->voxelSizeM();
    world.layer("terrain")->setVoxel(
        chunkmath::voxelCenter({0, 0, 0}, cvs), solidMat());

    auto sourceInit = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 100.0f);
        return 0;
    };

    PluginId pid = pm.wireInPlugin(sourceInit);
    for (int i = 0; i < 10; ++i)
        thermal.tick(0.016);
    ASSERT_GT(thermal.activeCount(), 0u);

    // Unload the source — heat decays.
    pm.unloadPlugin(pid);
    for (int i = 0; i < 2000; ++i)
        thermal.tick(0.016);

    // Re-register the same source.
    pm.wireInPlugin(sourceInit);
    for (int i = 0; i < 10; ++i)
        thermal.tick(0.016);

    EXPECT_GT(thermal.activeCount(), 0u)
        << "re-registering a heat source must resume the thermal field — the "
           "overlay re-derives from emitters, not from persisted field data";
    EXPECT_GT(thermal.temperatureAt(chunkmath::voxelCenter({0, 0, 0}, cvs)),
              tuning::thermal::kAmbientTemperature)
        << "temperature must be above ambient after re-registering the source";
}
