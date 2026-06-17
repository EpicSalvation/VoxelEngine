// M14 — FluidSystem cellular-automaton flow (docs/ARCHITECTURE.md §17).
//
// Level/head flow gated by porosity over a sparse fluid overlay. These tests
// pin down the six contracts named in the README task:
//
//   - fluid flows through porosity == 1 and open air;
//   - is blocked by low-porosity solid;
//   - conserves total amount under the budget;
//   - carries event overflow across frames;
//   - fires rising/falling events at the right thresholds;
//   - the active set is byte-identical across runs.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "simulation/FluidSystem.h"
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

Voxel matPorosity(float p) {
    Voxel v;
    v.material.density       = 2000.0f;
    v.material.porosity      = p;
    v.material.palette_index = 1;
    return v;
}

Voxel waterMat() {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.porosity      = 1.0f;
    v.material.palette_index = 5;
    return v;
}

struct FluidEventLog {
    struct Entry {
        VoxelCoord coord;
        float amount;
        FieldCrossing crossing;
    };
    static std::vector<Entry> events;
    static void callback(const FluidEvent* ev, void*) {
        if (!ev) return;
        events.push_back({{ev->voxel_x, ev->voxel_y, ev->voxel_z},
                           ev->amount, ev->crossing});
    }
    static int init(PluginContext* c) {
        c->register_on_fluid_event(c, callback, nullptr);
        return 0;
    }
};
std::vector<FluidEventLog::Entry> FluidEventLog::events;

int waterSourceInit(PluginContext* c) {
    c->register_material(c, "water", waterMat().material);
    c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 5.0f, "water");
    return 0;
}

struct FluidRig {
    World          world;
    PluginManager  pm;
    sim::FluidSystem fluid;

    FluidRig() : world(terminalOnly()), fluid(world, pm) {
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    double voxelSize() const { return world.layer("terrain")->voxelSizeM(); }

    void placeVoxel(VoxelCoord c, const Voxel& v) {
        world.layer("terrain")->setVoxel(chunkmath::voxelCenter(c, voxelSize()), v);
    }

    float amountAt(VoxelCoord c) const {
        return fluid.amountAt(chunkmath::voxelCenter(c, voxelSize()));
    }
};

}  // namespace

// Fluid flows through porosity == 1 material and through open air (empty cells
// treated as effective porosity 1.0).
TEST(FluidFlow, FlowsThroughHighPorosityAndAir) {
    FluidRig rig;
    rig.placeVoxel({0, 0, 0}, matPorosity(1.0f));

    rig.pm.wireInPlugin(waterSourceInit);

    const double dt = 0.016;
    for (int i = 0; i < 40; ++i)
        rig.fluid.tick(dt);

    EXPECT_GT(rig.amountAt({0, 0, 0}), 0.0f)
        << "source cell must have fluid";
    bool anyNeighborHasFluid = false;
    for (auto& nc : std::vector<VoxelCoord>{{1,0,0}, {-1,0,0}, {0,0,1}, {0,0,-1}}) {
        if (rig.amountAt(nc) > 0.0f) { anyNeighborHasFluid = true; break; }
    }
    EXPECT_TRUE(anyNeighborHasFluid)
        << "fluid must spread through open air to at least one lateral neighbor";
}

// Fluid is blocked by low-porosity solid — a complete wall of porosity=0
// prevents flow in all directions. The wall is a full Y-Z plane so fluid
// cannot route around it through open air.
TEST(FluidFlow, BlockedByLowPorositySolid) {
    FluidRig rig;
    // Build an impermeable wall at x=2 spanning the full Y-Z plane of the chunk.
    for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z)
            rig.placeVoxel({2, y, z}, matPorosity(0.0f));

    // Place permeable material on the source side.
    rig.placeVoxel({0, 0, 0}, matPorosity(1.0f));
    rig.placeVoxel({1, 0, 0}, matPorosity(1.0f));
    // Permeable material on the far side.
    rig.placeVoxel({3, 0, 0}, matPorosity(1.0f));

    rig.pm.wireInPlugin(waterSourceInit);

    for (int i = 0; i < 60; ++i)
        rig.fluid.tick(0.016);

    EXPECT_GT(rig.amountAt({0, 0, 0}), 0.0f) << "source cell has fluid";
    EXPECT_FLOAT_EQ(rig.amountAt({3, 0, 0}), 0.0f)
        << "fluid must not pass through a porosity-0 wall to the cell beyond";
}

// Conserves total fluid amount: with no source, flow redistributes but the
// total across the work set does not change.
TEST(FluidFlow, ConservesTotalAmountUnderBudget) {
    FluidRig rig;
    // Fill a 3D block of permeable material to contain fluid.
    for (int x = 1; x < 5; ++x)
        for (int y = 1; y < 5; ++y)
            for (int z = 1; z < 5; ++z)
                rig.placeVoxel({x, y, z}, matPorosity(1.0f));

    // High-rate source to build up fluid quickly.
    auto sourceInit = [](PluginContext* c) -> int {
        c->register_material(c, "water", waterMat().material);
        c->register_fluid_source(c, WorldCoord(2.5, 2.5, 2.5), 200.0f, "water");
        return 0;
    };
    PluginId srcPid = rig.pm.wireInPlugin(sourceInit);

    // Build up some fluid.
    for (int i = 0; i < 20; ++i)
        rig.fluid.tick(0.016);

    // Remove the source so no new fluid enters.
    rig.pm.unloadPlugin(srcPid);

    // Measure total fluid, then tick and re-measure.
    auto totalFluid = [&]() {
        float total = 0.0f;
        for (int x = 0; x < 16; ++x)
            for (int y = 0; y < 16; ++y)
                for (int z = 0; z < 16; ++z)
                    total += rig.amountAt({x, y, z});
        return total;
    };

    const float before = totalFluid();
    ASSERT_GT(before, 0.0f) << "there must be fluid in the system";

    for (int i = 0; i < 10; ++i)
        rig.fluid.tick(0.016);

    const float after = totalFluid();
    EXPECT_NEAR(after, before, before * 0.01f)
        << "total fluid must be conserved (within 1%) when no source is active";
}

// Carries event overflow across frames: with many cells saturating, events that
// exceed the per-frame budget are deferred and eventually fire.
TEST(FluidFlow, CarriesEventOverflowAcrossFrames) {
    FluidRig rig;
    for (int x = 0; x < 8; ++x)
        rig.placeVoxel({x, 0, 0}, matPorosity(1.0f));

    FluidEventLog::events.clear();
    rig.pm.wireInPlugin(FluidEventLog::init);

    auto sourceInit = [](PluginContext* c) -> int {
        c->register_material(c, "water", waterMat().material);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 100.0f, "water");
        return 0;
    };
    rig.pm.wireInPlugin(sourceInit);

    int totalEvents = 0;
    for (int i = 0; i < 60; ++i) {
        rig.fluid.tick(0.016);
        totalEvents += rig.fluid.eventsFiredLastTick();
    }

    int risingEvents = 0;
    for (const auto& e : FluidEventLog::events)
        if (e.crossing == FieldCrossing::Rising) ++risingEvents;

    EXPECT_GT(risingEvents, 0) << "at least one Rising event must fire";
    EXPECT_EQ(static_cast<size_t>(totalEvents), FluidEventLog::events.size())
        << "total events fired must match events received by the hook";
}

// Rising/falling events fire at the right thresholds.
TEST(FluidFlow, RisingFallingEventsAtCorrectThresholds) {
    FluidRig rig;
    rig.placeVoxel({0, 0, 0}, matPorosity(1.0f));

    FluidEventLog::events.clear();
    rig.pm.wireInPlugin(FluidEventLog::init);

    // Very high rate so the cell saturates quickly despite lateral flow.
    auto sourceInit = [](PluginContext* c) -> int {
        c->register_material(c, "water", waterMat().material);
        c->register_fluid_source(c, WorldCoord(0.5, 0.5, 0.5), 200.0f, "water");
        return 0;
    };
    PluginId srcPid = rig.pm.wireInPlugin(sourceInit);

    for (int i = 0; i < 30; ++i)
        rig.fluid.tick(0.016);

    bool gotRising = false;
    for (const auto& e : FluidEventLog::events) {
        if (e.coord.x == 0 && e.coord.y == 0 && e.coord.z == 0 &&
            e.crossing == FieldCrossing::Rising) {
            gotRising = true;
            EXPECT_GE(e.amount, tuning::fluid::kSaturationThreshold)
                << "Rising must fire when amount >= kSaturationThreshold";
        }
    }
    EXPECT_TRUE(gotRising) << "a saturated cell must fire a Rising event";

    rig.pm.unloadPlugin(srcPid);
    for (int i = 0; i < 2000; ++i)
        rig.fluid.tick(0.016);

    for (const auto& e : FluidEventLog::events) {
        if (e.coord.x == 0 && e.coord.y == 0 && e.coord.z == 0 &&
            e.crossing == FieldCrossing::Falling) {
            EXPECT_LT(e.amount, tuning::fluid::kMinFluidAmount)
                << "Falling must fire when amount < kMinFluidAmount";
        }
    }
}

// The active set is byte-identical across runs.
TEST(FluidFlow, ActiveSetIsByteIdenticalAcrossRuns) {
    auto run = [](std::vector<float>& amounts) {
        FluidRig rig;
        for (int x = 0; x < 4; ++x)
            rig.placeVoxel({x, 0, 0}, matPorosity(1.0f));
        rig.pm.wireInPlugin(waterSourceInit);
        for (int i = 0; i < 20; ++i)
            rig.fluid.tick(0.016);
        amounts.clear();
        for (int x = 0; x < 4; ++x)
            amounts.push_back(rig.amountAt({x, 0, 0}));
    };

    std::vector<float> a, b;
    run(a);
    run(b);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i)
        EXPECT_FLOAT_EQ(a[i], b[i])
            << "fluid amount at cell " << i << " must be byte-identical across runs";
}
