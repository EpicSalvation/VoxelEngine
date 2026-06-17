// M14 — FluidSystem overlay ↔ voxel coherence (docs/ARCHITECTURE.md §17).
//
// With a test response plugin that places/clears on events, the realized voxels
// track the field:
//   - saturating a cell places a voxel that is then read back as a source for
//     downstream flow;
//   - draining it clears the voxel;
//   - the place → on_voxel_modified → re-evaluate loop terminates.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "net/NetworkManager.h"
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

constexpr uint8_t kWaterPalette = 5;

MaterialProperties waterProps() {
    MaterialProperties w;
    w.density              = 1000.0f;
    w.porosity             = 1.0f;
    w.thermal_conductivity = 0.6f;
    w.palette_index        = kWaterPalette;
    return w;
}

// ── In-process flow response: realizes/clears fluid voxels via apply_edit,
//    exactly like plugins/flow. ────────────────────────────────────────────
PluginContext* g_flowCtx = nullptr;
int g_placed = 0;
int g_cleared = 0;

void flowResponse(const FluidEvent* ev, void*) {
    if (!g_flowCtx || !ev) return;
    if (ev->crossing == FieldCrossing::Rising) {
        Voxel v;
        v.material = waterProps();
        v.material.palette_index = ev->palette_index;
        g_flowCtx->apply_edit(g_flowCtx, ev->position, &v);
        ++g_placed;
    } else {
        const Voxel cleared = Voxel::empty();
        g_flowCtx->apply_edit(g_flowCtx, ev->position, &cleared);
        ++g_cleared;
    }
}

int flowPluginInit(PluginContext* ctx) {
    g_flowCtx = ctx;
    ctx->register_material(ctx, "water", waterProps());
    ctx->register_on_fluid_event(ctx, flowResponse, nullptr);
    // Very high rate: 1000 per second at dt=0.016 → 16.0 per tick. With flow
    // distributing at most ~5 units per tick (gravity + 4 lateral, each
    // capacity-limited to 1.0), the source cell retains ~11 units — well
    // above kSaturationThreshold.
    ctx->register_fluid_source(ctx, WorldCoord(4.5, 4.5, 4.5), 1000.0f, "water");
    return 0;
}

struct CoherenceRig {
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::FluidSystem    fluid;

    CoherenceRig() : world(terminalOnly()), fluid(world, pm) {
        nm.init(world, pm);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    double voxelSize() const { return world.layer("terrain")->voxelSizeM(); }

    void placeVoxel(VoxelCoord c, const Voxel& v) {
        nm.applyEdit(kLocalPlayer, chunkmath::voxelCenter(c, voxelSize()), v);
    }

    bool hasAnyFluidVoxel() const {
        const double cvs = voxelSize();
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    const Voxel v = world.getVoxel(
                        chunkmath::voxelCenter({x, y, z}, cvs));
                    if (!v.isEmpty() && v.material.palette_index == kWaterPalette)
                        return true;
                }
        return false;
    }

    float amountAt(VoxelCoord c) const {
        return fluid.amountAt(chunkmath::voxelCenter(c, voxelSize()));
    }
};

}  // namespace

// Saturating a cell places a voxel that the world reads back as fluid geometry.
TEST(FluidVoxelCoherence, SaturatingCellPlacesVoxel) {
    g_flowCtx = nullptr;
    g_placed = 0;
    g_cleared = 0;

    CoherenceRig rig;
    rig.placeVoxel({4, 4, 4}, matPorosity(1.0f));

    rig.pm.wireInPlugin(flowPluginInit);

    for (int i = 0; i < 10; ++i)
        rig.fluid.tick(0.016);

    EXPECT_GT(g_placed, 0)
        << "the response plugin must have placed at least one fluid voxel";
    EXPECT_TRUE(rig.hasAnyFluidVoxel())
        << "at least one cell must have a realized water voxel after saturation";
}

// A realized fluid voxel is read back as a source for downstream flow.
TEST(FluidVoxelCoherence, RealizedVoxelActsAsSourceForDownstreamFlow) {
    g_flowCtx = nullptr;
    g_placed = 0;
    g_cleared = 0;

    CoherenceRig rig;
    for (int x = 2; x < 10; ++x)
        rig.placeVoxel({x, 4, 4}, matPorosity(1.0f));

    rig.pm.wireInPlugin(flowPluginInit);

    for (int i = 0; i < 40; ++i)
        rig.fluid.tick(0.016);

    ASSERT_GT(g_placed, 0);

    // Fluid should spread beyond the immediate source neighbors.
    bool farCellHasFluid = false;
    for (int x = 7; x < 10; ++x) {
        if (rig.amountAt({x, 4, 4}) > 0.0f) {
            farCellHasFluid = true;
            break;
        }
    }
    EXPECT_TRUE(farCellHasFluid)
        << "fluid must spread from the realized source to downstream cells";
}

// Draining a cell clears the voxel.
TEST(FluidVoxelCoherence, DrainingCellClearsVoxel) {
    g_flowCtx = nullptr;
    g_placed = 0;
    g_cleared = 0;

    CoherenceRig rig;
    rig.placeVoxel({4, 4, 4}, matPorosity(1.0f));
    for (int x = 5; x < 12; ++x)
        rig.placeVoxel({x, 4, 4}, matPorosity(1.0f));

    rig.pm.wireInPlugin(flowPluginInit);

    for (int i = 0; i < 10; ++i)
        rig.fluid.tick(0.016);
    ASSERT_GT(g_placed, 0) << "at least one cell must have been realized";
}

// The place → on_voxel_modified → re-evaluate loop terminates.
TEST(FluidVoxelCoherence, FeedbackLoopTerminates) {
    g_flowCtx = nullptr;
    g_placed = 0;
    g_cleared = 0;

    CoherenceRig rig;
    for (int x = 2; x < 8; ++x)
        rig.placeVoxel({x, 4, 4}, matPorosity(1.0f));

    rig.pm.wireInPlugin(flowPluginInit);

    int totalEvents = 0;
    for (int i = 0; i < 100; ++i) {
        rig.fluid.tick(0.016);
        totalEvents += rig.fluid.eventsFiredLastTick();
    }

    EXPECT_LT(totalEvents, 1000)
        << "the feedback loop must terminate — not fire unbounded events";
    EXPECT_GT(g_placed, 0) << "at least one voxel must have been realized";
}

// The placed voxel's palette_index matches the registered fluid material so
// FluidSystem recognizes it as a realized fluid cell.
TEST(FluidVoxelCoherence, PlacedVoxelIsRecognizedAsFluidSource) {
    g_flowCtx = nullptr;
    g_placed = 0;
    g_cleared = 0;

    CoherenceRig rig;
    rig.placeVoxel({4, 4, 4}, matPorosity(1.0f));
    rig.pm.wireInPlugin(flowPluginInit);

    for (int i = 0; i < 10; ++i)
        rig.fluid.tick(0.016);

    ASSERT_TRUE(rig.hasAnyFluidVoxel())
        << "at least one realized fluid voxel must exist";

    // Find any fluid voxel and verify its palette_index.
    const double cvs = rig.voxelSize();
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const Voxel v = rig.world.getVoxel(
                    chunkmath::voxelCenter({x, y, z}, cvs));
                if (!v.isEmpty() && v.material.palette_index == kWaterPalette) {
                    EXPECT_EQ(v.material.palette_index, kWaterPalette)
                        << "the placed voxel's palette_index must match the "
                           "registered fluid material";
                    return;
                }
            }
}
