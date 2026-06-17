// M14 — Engine-never-writes-voxel invariant for FluidSystem and ThermalSystem
// (docs/ARCHITECTURE.md §17, §13).
//
// With the flow plugin UNLOADED, FluidSystem/ThermalSystem advance their
// overlays and fire events but leave World byte-identical — proving the
// detect/respond split holds for fluid exactly as it does for M13's structural
// collapse. The engine only detects and reports; what a plugin does with
// the event is game policy.

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

// Snapshot every terminal voxel in the resident chunk so we can prove the world
// is byte-identical before/after an engine-only pass. Uses the same pattern as
// StructuralCascadeTest::snapshot.
std::vector<std::pair<float, uint8_t>> snapshot(const World& w) {
    std::vector<std::pair<float, uint8_t>> out;
    const double cvs = w.layer("terrain")->voxelSizeM();
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const Voxel v = w.getVoxel(chunkmath::voxelCenter({x, y, z}, cvs));
                out.emplace_back(v.material.density, v.material.palette_index);
            }
    return out;
}

// Event counters to verify events ARE fired even with no response plugin.
int g_fluidEvents = 0;
int g_thermalEvents = 0;

void fluidEventCounter(const FluidEvent*, void*) { ++g_fluidEvents; }
void thermalEventCounter(const ThermalFieldEvent*, void*) { ++g_thermalEvents; }

int eventCounterInit(PluginContext* ctx) {
    ctx->register_on_fluid_event(ctx, fluidEventCounter, nullptr);
    ctx->register_on_thermal_event(ctx, thermalEventCounter, nullptr);
    return 0;
}

}  // namespace

// FluidSystem advances its overlay and fires events but never writes a voxel.
TEST(FluidThermalInvariant, FluidSystemNeverWritesVoxel) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::FluidSystem fluid(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    // Place some permeable material.
    const double cvs = world.layer("terrain")->voxelSizeM();
    for (int x = 0; x < 4; ++x)
        world.layer("terrain")->setVoxel(
            chunkmath::voxelCenter({x, 0, 0}, cvs), solidMat());

    // Register a fluid source and event counter — but NO response plugin (no flow).
    g_fluidEvents = 0;
    g_thermalEvents = 0;
    pm.wireInPlugin(eventCounterInit);

    auto sourceInit = [](PluginContext* c) -> int {
        MaterialProperties w;
        w.density       = 1000.0f;
        w.porosity      = 1.0f;
        w.palette_index = 5;
        c->register_material(c, "water", w);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 200.0f, "water");
        return 0;
    };
    pm.wireInPlugin(sourceInit);

    const auto before = snapshot(world);

    // Run the fluid system for many ticks — it will fill the overlay and fire events.
    for (int i = 0; i < 60; ++i)
        fluid.tick(0.016);

    const auto after = snapshot(world);

    EXPECT_GT(g_fluidEvents, 0)
        << "FluidSystem must fire events (the overlay advances)";
    EXPECT_GT(fluid.activeCount(), 0u)
        << "the fluid overlay must have active cells";
    EXPECT_EQ(before, after)
        << "with no response plugin the engine must leave every voxel untouched "
           "— the detect/respond split holds for fluid";
}

// ThermalSystem advances its overlay and fires events but never writes a voxel.
TEST(FluidThermalInvariant, ThermalSystemNeverWritesVoxel) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::ThermalSystem thermal(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    const double cvs = world.layer("terrain")->voxelSizeM();
    for (int x = 0; x < 4; ++x)
        world.layer("terrain")->setVoxel(
            chunkmath::voxelCenter({x, 0, 0}, cvs), solidMat());

    // Register a heat source — but NO response plugin.
    auto sourceInit = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 100.0f);
        return 0;
    };
    pm.wireInPlugin(sourceInit);

    g_thermalEvents = 0;
    pm.wireInPlugin(eventCounterInit);

    const auto before = snapshot(world);

    for (int i = 0; i < 40; ++i)
        thermal.tick(0.016);

    const auto after = snapshot(world);

    EXPECT_GT(g_thermalEvents, 0)
        << "ThermalSystem must fire events (the overlay advances)";
    EXPECT_GT(thermal.activeCount(), 0u)
        << "the thermal overlay must have active cells";
    EXPECT_EQ(before, after)
        << "with no response plugin the engine must leave every voxel untouched "
           "— the detect/respond split holds for heat";
}

// Both systems together: with sources but no response plugins, the world is
// byte-identical — proving the combined detect/respond split.
TEST(FluidThermalInvariant, BothSystemsTogetherNeverWriteVoxel) {
    World         world(terminalOnly());
    PluginManager pm;
    sim::ThermalSystem thermal(world, pm);
    sim::FluidSystem   fluid(world, pm);

    world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    const double cvs = world.layer("terrain")->voxelSizeM();
    for (int x = 0; x < 6; ++x)
        world.layer("terrain")->setVoxel(
            chunkmath::voxelCenter({x, 0, 0}, cvs), solidMat());

    auto sourceInit = [](PluginContext* c) -> int {
        MaterialProperties w;
        w.density       = 1000.0f;
        w.porosity      = 1.0f;
        w.palette_index = 5;
        c->register_material(c, "water", w);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 200.0f, "water");
        c->register_heat_source(c, WorldCoord(2.5, 0.5, 0.5), 100.0f);
        return 0;
    };
    pm.wireInPlugin(sourceInit);

    const auto before = snapshot(world);

    for (int i = 0; i < 50; ++i) {
        thermal.tick(0.016);
        fluid.tick(0.016);
    }

    const auto after = snapshot(world);
    EXPECT_EQ(before, after)
        << "with no response plugin, both simulation systems must leave every "
           "voxel untouched";
}
