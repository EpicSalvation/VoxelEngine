// Tests for M16 L3 gravity-relative fluid flow (FluidSystem.cpp).
//
// FluidSystem's Phase-A drain / Phase-B lateral-equalize split is parameterized
// by the gravity vector from the L7 GravityProvider instead of a fixed -Y. Fluid
// drains along whatever "down" the provider supplies (pooling toward a radial
// center from multiple sides), conserves total amount, and under zero-g
// degenerates to pure 6-neighbor pressure equalization with no preferred
// direction. The default constant -Y is byte-identical to M14 (regression).

#include <gtest/gtest.h>

#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "simulation/FluidSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/GravityProvider.h"
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

Voxel wall() {  // impermeable container material
    Voxel v;
    v.material.density       = 2000.0f;
    v.material.porosity      = 0.0f;
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

struct FluidRig {
    World            world;
    PluginManager    pm;
    sim::FluidSystem fluid;

    FluidRig() : world(terminalOnly()), fluid(world, pm) {
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }
    double voxelSize() const { return world.layer("terrain")->voxelSizeM(); }
    void   placeVoxel(VoxelCoord c, const Voxel& v) {
        world.layer("terrain")->setVoxel(chunkmath::voxelCenter(c, voxelSize()), v);
    }
    float amountAt(VoxelCoord c) const {
        return fluid.amountAt(chunkmath::voxelCenter(c, voxelSize()));
    }
};

// Enclose a single Y pipe of air cells (0,y,0) for y in [0, len) with
// impermeable walls on all four lateral sides and both Y caps, so fluid only
// moves along Y.
void buildYPipe(FluidRig& rig, int len) {
    for (int y = -1; y <= len; ++y) {
        rig.placeVoxel({1, y, 0}, wall());
        rig.placeVoxel({-1, y, 0}, wall());
        rig.placeVoxel({0, y, 1}, wall());
        rig.placeVoxel({0, y, -1}, wall());
    }
    rig.placeVoxel({0, -1, 0}, wall());
    rig.placeVoxel({0, len, 0}, wall());
}

int pipeSourceInit(PluginContext* c) {
    c->register_material(c, "water", waterMat().material);
    // Source at the middle of a 7-cell pipe (y=3).
    c->register_fluid_source(c, WorldCoord(0.5, 3.5, 0.5), 50.0f, "water");
    return 0;
}

}  // namespace

// The constant -Y path is byte-identical to M14: setting the gravity provider to
// constant -Y matches the engine default exactly, cell for cell.
TEST(FluidGravity, NegativeYIsByteIdenticalRegression) {
    auto run = [](bool setExplicit, std::vector<float>& amounts) {
        FluidRig rig;
        buildYPipe(rig, 7);
        if (setExplicit)
            rig.fluid.setGravityProvider(GravityProvider::constant({0.0, -1.0, 0.0}));
        rig.pm.wireInPlugin(pipeSourceInit);
        for (int i = 0; i < 80; ++i) rig.fluid.tick(0.016);
        amounts.clear();
        for (int y = 0; y < 7; ++y) amounts.push_back(rig.amountAt({0, y, 0}));
    };

    std::vector<float> def, explicitNegY;
    run(false, def);
    run(true, explicitNegY);
    ASSERT_EQ(def.size(), explicitNegY.size());
    for (size_t i = 0; i < def.size(); ++i)
        EXPECT_FLOAT_EQ(def[i], explicitNegY[i]) << "cell y=" << i;

    // And the default sinks fluid toward the bottom of the pipe (low Y).
    EXPECT_GT(def.front(), def.back()) << "default -Y must pool fluid at the bottom";
}

// Fluid drains along an arbitrary supplied gravity vector: flip "down" to +Y and
// the same finite body of fluid pools at the TOP, the mirror of the -Y case.
// (Source is removed after a fill so the total is finite and settles cleanly.)
TEST(FluidGravity, DrainsAlongAnArbitraryGravityVector) {
    auto runWith = [](const GravityProvider& g, float& lowHalf, float& highHalf) {
        FluidRig rig;
        buildYPipe(rig, 7);
        rig.fluid.setGravityProvider(g);
        const PluginId pid = rig.pm.wireInPlugin(pipeSourceInit);
        for (int i = 0; i < 30; ++i) rig.fluid.tick(0.016);
        rig.pm.unloadPlugin(pid);                 // finite fluid from here on
        for (int i = 0; i < 250; ++i) rig.fluid.tick(0.016);
        lowHalf = highHalf = 0.0f;
        for (int y = 0; y < 3; ++y) lowHalf  += rig.amountAt({0, y, 0});
        for (int y = 4; y < 7; ++y) highHalf += rig.amountAt({0, y, 0});
    };

    float dLow, dHigh, uLow, uHigh;
    runWith(GravityProvider::constant({0.0, -1.0, 0.0}), dLow, dHigh);
    runWith(GravityProvider::constant({0.0, 1.0, 0.0}), uLow, uHigh);

    EXPECT_GT(dLow, dHigh) << "-Y must pool fluid in the lower half of the pipe";
    EXPECT_GT(uHigh, uLow) << "+Y must pool fluid in the upper half of the pipe";
}

// A radial well pools fluid toward the body's center, drawing a finite body of
// fluid to the +X end of an enclosed corridor (down = toward the distant center).
TEST(FluidGravity, RadialPoolsTowardCenterFromMultipleSides) {
    FluidRig rig;
    // An enclosed X corridor of air cells (x in [0,5)) at y=z=2, bounded by walls.
    for (int x = -1; x <= 5; ++x) {
        rig.placeVoxel({x, 1, 2}, wall());
        rig.placeVoxel({x, 3, 2}, wall());
        rig.placeVoxel({x, 2, 1}, wall());
        rig.placeVoxel({x, 2, 3}, wall());
    }
    rig.placeVoxel({-1, 2, 2}, wall());
    rig.placeVoxel({5, 2, 2}, wall());

    rig.fluid.setGravityProvider(GravityProvider::radial(WorldCoord(100.0, 2.5, 2.5)));
    auto sourceInit = [](PluginContext* c) -> int {
        c->register_material(c, "water", waterMat().material);
        c->register_fluid_source(c, WorldCoord(0.5, 2.5, 2.5), 50.0f, "water");
        return 0;
    };
    const PluginId pid = rig.pm.wireInPlugin(sourceInit);
    // Inject a small, finite body of fluid (a few cells' worth) so it slides
    // entirely to the down (+X) end rather than overflowing the short corridor.
    for (int i = 0; i < 3; ++i) rig.fluid.tick(0.016);
    rig.pm.unloadPlugin(pid);
    for (int i = 0; i < 250; ++i) rig.fluid.tick(0.016);

    // "Down" is +X here (toward the distant center), so fluid piles up against the
    // +X wall, not back at the source end.
    EXPECT_GT(rig.amountAt({4, 2, 2}), rig.amountAt({1, 2, 2}))
        << "radial gravity must pool fluid toward the center side";
}

// Under zero-g the drain phase vanishes: fluid equalizes pressure across all 6
// neighbors with no preferred direction (down ≈ up), unlike -Y where it sinks.
TEST(FluidGravity, ZeroGEqualizesWithNoPreferredDirection) {
    auto buildCube = [](FluidRig& rig) {
        // Enclosed 3×3×3 air cube (cells 0..2 each axis) with a wall shell.
        for (int x = -1; x <= 3; ++x)
            for (int y = -1; y <= 3; ++y)
                for (int z = -1; z <= 3; ++z)
                    if (x < 0 || x > 2 || y < 0 || y > 2 || z < 0 || z > 2)
                        rig.placeVoxel({x, y, z}, wall());
    };
    auto centerSource = [](PluginContext* c) -> int {
        c->register_material(c, "water", waterMat().material);
        c->register_fluid_source(c, WorldCoord(1.5, 1.5, 1.5), 30.0f, "water");
        return 0;
    };

    FluidRig zeroRig;
    buildCube(zeroRig);
    zeroRig.fluid.setGravityProvider(GravityProvider::zeroG());
    zeroRig.pm.wireInPlugin(centerSource);
    for (int i = 0; i < 120; ++i) zeroRig.fluid.tick(0.016);

    FluidRig downRig;
    buildCube(downRig);  // default -Y
    downRig.pm.wireInPlugin(centerSource);
    for (int i = 0; i < 120; ++i) downRig.fluid.tick(0.016);

    const float zDown = zeroRig.amountAt({1, 0, 1});  // below center
    const float zUp   = zeroRig.amountAt({1, 2, 1});  // above center
    const float gDown = downRig.amountAt({1, 0, 1});
    const float gUp   = downRig.amountAt({1, 2, 1});

    // -Y gravity clearly favors the down neighbor over the up neighbor.
    EXPECT_GT(gDown, gUp + 0.05f) << "under -Y fluid sinks";
    // Zero-g has no such bias: the two are essentially equal.
    EXPECT_NEAR(zDown, zUp, 0.02f) << "zero-g has no preferred direction";
}
