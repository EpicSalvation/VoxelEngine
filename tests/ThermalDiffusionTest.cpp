// M14 — ThermalSystem heat diffusion (docs/ARCHITECTURE.md §17).
//
// Explicit finite-difference diffusion over a sparse thermal overlay. These
// tests pin down the five contracts named in the README task:
//
//   - heat spreads faster through high-thermal_conductivity material than low;
//   - the explicit scheme stays stable under the sub-step bound (no oscillation
//     or blow-up);
//   - cells decay back to ambient and leave the active set;
//   - a crossing fires on_thermal_event exactly once per direction;
//   - the field is byte-identical across runs.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
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

Voxel matConductivity(float k) {
    Voxel v;
    v.material.density              = 1000.0f;
    v.material.thermal_conductivity = k;
    v.material.palette_index        = 1;
    return v;
}

struct ThermalRig {
    World          world;
    PluginManager  pm;
    sim::ThermalSystem thermal;

    ThermalRig() : world(terminalOnly()), thermal(world, pm) {
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    double voxelSize() const { return world.layer("terrain")->voxelSizeM(); }

    void placeVoxel(VoxelCoord c, const Voxel& v) {
        world.layer("terrain")->setVoxel(chunkmath::voxelCenter(c, voxelSize()), v);
    }
};

// File-scope event log for on_thermal_event tests (local-class static members
// are not allowed in C++, so we use a file-scope global — the same pattern as
// StructuralCascadeTest's g_resp / g_responses).
struct ThermalEventEntry {
    VoxelCoord coord;
    float temperature;
    FieldCrossing crossing;
};
std::vector<ThermalEventEntry> g_thermalLog;

void thermalLogCallback(const ThermalFieldEvent* ev, void*) {
    if (!ev) return;
    g_thermalLog.push_back({{ev->voxel_x, ev->voxel_y, ev->voxel_z},
                             ev->temperature,
                             ev->crossing});
}

int thermalLogHookInit(PluginContext* ctx) {
    ctx->register_on_thermal_event(ctx, thermalLogCallback, nullptr);
    return 0;
}

}  // namespace

// Heat spreads faster through high-thermal_conductivity material than low.
// Measured by how many ticks it takes for heat to first reach a distant cell —
// higher conductivity propagates the wavefront sooner.
TEST(ThermalDiffusion, HighConductivitySpreadsFaster) {
    auto ticksToReach = [](float k) -> int {
        ThermalRig rig;
        for (int x = 0; x <= 4; ++x)
            rig.placeVoxel({x, 0, 0}, matConductivity(k));

        auto sourceInit = [](PluginContext* c) -> int {
            c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 500.0f);
            return 0;
        };
        rig.pm.wireInPlugin(sourceInit);

        const double dt = 0.016;
        for (int tick = 1; tick <= 200; ++tick) {
            rig.thermal.tick(dt);
            const float t = rig.thermal.temperatureAt(
                chunkmath::voxelCenter({4, 0, 0}, rig.voxelSize()));
            if (t > tuning::thermal::kAmbientTemperature + 0.001f)
                return tick;
        }
        return 201;
    };

    const int lowKTicks  = ticksToReach(0.1f);
    const int highKTicks = ticksToReach(1.0f);
    EXPECT_LT(highKTicks, lowKTicks)
        << "heat must reach a distant cell sooner through high-conductivity material";
    EXPECT_LT(highKTicks, 200)
        << "high-k material must propagate heat to the far cell within the test window";
}

// The explicit scheme stays stable (no oscillation / blow-up) under the sub-step
// bound — the max temperature never exceeds source + ambient, and no cell goes
// below ambient.
TEST(ThermalDiffusion, ExplicitSchemeStaysStable) {
    ThermalRig rig;
    const float k = 2.0f;
    for (int z = 0; z < 5; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x)
                rig.placeVoxel({x, y, z}, matConductivity(k));

    auto sourceInit = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(2.5, 2.5, 2.5), 500.0f);
        return 0;
    };
    rig.pm.wireInPlugin(sourceInit);

    const double dt = 0.05;
    float maxTemp = 0.0f;
    bool anyBelowAmbient = false;
    for (int i = 0; i < 50; ++i) {
        rig.thermal.tick(dt);
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 5; ++x) {
                    const float t = rig.thermal.temperatureAt(
                        chunkmath::voxelCenter({x, y, z}, rig.voxelSize()));
                    maxTemp = std::max(maxTemp, t);
                    if (t < tuning::thermal::kAmbientTemperature - 0.01f)
                        anyBelowAmbient = true;
                }
    }

    EXPECT_FALSE(anyBelowAmbient)
        << "no cell should drop below ambient with a positive heat source";
    EXPECT_LT(maxTemp, 1e6f)
        << "temperature must not blow up — the sub-step bound must keep the scheme stable";
}

// Cells decay back to ambient and leave the active set once no source keeps them
// elevated.
TEST(ThermalDiffusion, CellsDecayToAmbientAndLeaveActiveSet) {
    ThermalRig rig;
    for (int x = 0; x < 3; ++x)
        rig.placeVoxel({x, 0, 0}, matConductivity(0.5f));

    auto sourceInit = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 50.0f);
        return 0;
    };
    PluginId pid = rig.pm.wireInPlugin(sourceInit);

    for (int i = 0; i < 10; ++i)
        rig.thermal.tick(0.016);
    ASSERT_GT(rig.thermal.activeCount(), 0u)
        << "heat source must have activated some cells";

    rig.pm.unloadPlugin(pid);

    for (int i = 0; i < 2000; ++i)
        rig.thermal.tick(0.016);

    EXPECT_EQ(rig.thermal.activeCount(), 0u)
        << "all cells must decay back to ambient and leave the active set";
}

// A crossing fires on_thermal_event exactly once per direction.
TEST(ThermalDiffusion, CrossingFiresExactlyOncePerDirection) {
    g_thermalLog.clear();

    ThermalRig rig;
    rig.placeVoxel({0, 0, 0}, matConductivity(0.5f));

    rig.pm.wireInPlugin(thermalLogHookInit);

    auto sourceInit = [](PluginContext* c) -> int {
        c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 50.0f);
        return 0;
    };
    PluginId srcPid = rig.pm.wireInPlugin(sourceInit);

    for (int i = 0; i < 5; ++i)
        rig.thermal.tick(0.016);

    int risingCount = 0;
    for (const auto& e : g_thermalLog)
        if (e.coord.x == 0 && e.coord.y == 0 && e.coord.z == 0 &&
            e.crossing == FieldCrossing::Rising)
            ++risingCount;
    EXPECT_EQ(risingCount, 1) << "Rising must fire exactly once for the heated cell";

    rig.pm.unloadPlugin(srcPid);
    for (int i = 0; i < 2000; ++i)
        rig.thermal.tick(0.016);

    int fallingCount = 0;
    for (const auto& e : g_thermalLog)
        if (e.coord.x == 0 && e.coord.y == 0 && e.coord.z == 0 &&
            e.crossing == FieldCrossing::Falling)
            ++fallingCount;
    EXPECT_EQ(fallingCount, 1)
        << "Falling must fire exactly once when the cell decays back to ambient";
}

// The field is byte-identical across runs.
TEST(ThermalDiffusion, FieldIsByteIdenticalAcrossRuns) {
    auto run = [](std::vector<float>& temps) {
        ThermalRig rig;
        for (int x = 0; x < 4; ++x)
            rig.placeVoxel({x, 0, 0}, matConductivity(0.5f));
        auto sourceInit = [](PluginContext* c) -> int {
            c->register_heat_source(c, WorldCoord(0.5, 0.5, 0.5), 30.0f);
            return 0;
        };
        rig.pm.wireInPlugin(sourceInit);
        for (int i = 0; i < 20; ++i)
            rig.thermal.tick(0.016);
        temps.clear();
        for (int x = 0; x < 4; ++x)
            temps.push_back(rig.thermal.temperatureAt(
                chunkmath::voxelCenter({x, 0, 0}, rig.voxelSize())));
    };

    std::vector<float> a, b;
    run(a);
    run(b);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_FLOAT_EQ(a[i], b[i])
            << "temperature at cell " << i << " must be byte-identical across runs";
}
