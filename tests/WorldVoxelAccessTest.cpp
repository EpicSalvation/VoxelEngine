// Tests for global voxel-coordinate math (src/world/ChunkCoordMath.h) and the
// chunked World's world-space single-voxel accessors (src/world/World.cpp).
// These are the M5 foundation shared by picking, editing, and collision.

#include "world/ChunkCoordMath.h"
#include "world/World.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

using chunkmath::VoxelCoord;
using chunkmath::LocalVoxel;
using chunkmath::worldToVoxel;
using chunkmath::voxelToChunkLocal;
using chunkmath::chunkLocalToVoxel;
using chunkmath::voxelOrigin;
using chunkmath::voxelCenter;
using chunkmath::worldToChunk;

namespace {

// A distinguishable non-empty voxel (isEmpty() == false).
Voxel solid(uint8_t palette = 7) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = palette;
    return v;
}

LayerDef terminalLayer(double voxelSize = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name             = "terrain";
    d.voxel_size_m     = voxelSize;
    d.mode             = VoxelMode::terminal;
    d.chunk_size_voxels = chunkSize;
    return d;
}

}  // namespace

// ── worldToVoxel ───────────────────────────────────────────────────────────

TEST(VoxelCoordMath, WorldToVoxelFloorsLikeWorldToChunk) {
    EXPECT_EQ(worldToVoxel(WorldCoord(0.0, 0.0, 0.0), 1.0), (VoxelCoord{0, 0, 0}));
    EXPECT_EQ(worldToVoxel(WorldCoord(0.9, 1.5, 2.99), 1.0), (VoxelCoord{0, 1, 2}));
    // Boundary belongs to the higher cell.
    EXPECT_EQ(worldToVoxel(WorldCoord(2.0, 0.0, 0.0), 1.0), (VoxelCoord{2, 0, 0}));
}

TEST(VoxelCoordMath, WorldToVoxelNegativesFloorNotTruncate) {
    EXPECT_EQ(worldToVoxel(WorldCoord(-0.1, 0.0, 0.0), 1.0), (VoxelCoord{-1, 0, 0}));
    EXPECT_EQ(worldToVoxel(WorldCoord(-1.0, 0.0, 0.0), 1.0), (VoxelCoord{-1, 0, 0}));
    EXPECT_EQ(worldToVoxel(WorldCoord(-1.1, 0.0, 0.0), 1.0), (VoxelCoord{-2, 0, 0}));
}

TEST(VoxelCoordMath, WorldToVoxelRespectsVoxelSize) {
    EXPECT_EQ(worldToVoxel(WorldCoord(3.9, 0.0, 0.0), 2.0), (VoxelCoord{1, 0, 0}));
    EXPECT_EQ(worldToVoxel(WorldCoord(4.0, 0.0, 0.0), 2.0), (VoxelCoord{2, 0, 0}));
}

// ── voxelToChunkLocal / chunkLocalToVoxel ────────────────────────────────────

TEST(VoxelCoordMath, ChunkLocalDecompositionIsInRange) {
    const int n = 32;
    for (int64_t gx : {-65, -33, -32, -1, 0, 1, 31, 32, 63, 64}) {
        LocalVoxel lv = voxelToChunkLocal(VoxelCoord{gx, 0, 0}, n);
        EXPECT_GE(lv.x, 0);
        EXPECT_LT(lv.x, n);
        // Recombining must recover the original global coord.
        EXPECT_EQ(chunkLocalToVoxel(lv.chunk, lv.x, lv.y, lv.z, n),
                  (VoxelCoord{gx, 0, 0}));
    }
}

TEST(VoxelCoordMath, NegativeGlobalVoxelMapsToNegativeChunk) {
    LocalVoxel lv = voxelToChunkLocal(VoxelCoord{-1, -1, -1}, 32);
    EXPECT_EQ(lv.chunk.x, -1);
    EXPECT_EQ(lv.chunk.y, -1);
    EXPECT_EQ(lv.chunk.z, -1);
    EXPECT_EQ(lv.x, 31);
    EXPECT_EQ(lv.y, 31);
    EXPECT_EQ(lv.z, 31);
}

TEST(VoxelCoordMath, ChunkOfVoxelAgreesWithWorldToChunk) {
    // Resolving a world position to a voxel and then to its chunk must match the
    // direct world->chunk path (both are floor-based; floor of a floor agrees).
    const double vs = 1.0;
    const int    cs = 32;
    for (double x : {-100.3, -32.0, -0.5, 0.0, 5.5, 31.9, 32.0, 1000.7}) {
        WorldCoord p(x, 0.0, 0.0);
        LocalVoxel lv = voxelToChunkLocal(worldToVoxel(p, vs), cs);
        EXPECT_EQ(lv.chunk.x, worldToChunk(p, vs, cs).x) << "x=" << x;
    }
}

// ── voxelOrigin / voxelCenter ────────────────────────────────────────────────

TEST(VoxelCoordMath, VoxelOriginAndCenter) {
    WorldCoord o = voxelOrigin(VoxelCoord{2, -1, 3}, 2.0);
    EXPECT_DOUBLE_EQ(o.value.x, 4.0);
    EXPECT_DOUBLE_EQ(o.value.y, -2.0);
    EXPECT_DOUBLE_EQ(o.value.z, 6.0);
    WorldCoord c = voxelCenter(VoxelCoord{2, -1, 3}, 2.0);
    EXPECT_DOUBLE_EQ(c.value.x, 5.0);
    EXPECT_DOUBLE_EQ(c.value.y, -1.0);
    EXPECT_DOUBLE_EQ(c.value.z, 7.0);
    // A voxel's center resolves back to that same voxel.
    EXPECT_EQ(worldToVoxel(c, 2.0), (VoxelCoord{2, -1, 3}));
}

// ── World world-space accessors ──────────────────────────────────────────────

TEST(WorldVoxelAccess, ReadEmptyWhenChunkNotResident) {
    World world(terminalLayer());
    // No chunks loaded — every query is empty, no crash.
    EXPECT_TRUE(world.getVoxel(WorldCoord(5.0, 5.0, 5.0)).isEmpty());
    EXPECT_TRUE(world.getVoxel(WorldCoord(-100.0, 0.0, 0.0)).isEmpty());
}

TEST(WorldVoxelAccess, WriteToUnresidentChunkIsNoOp) {
    World world(terminalLayer());
    EXPECT_FALSE(world.setVoxel(WorldCoord(5.0, 5.0, 5.0), solid()));
    EXPECT_TRUE(world.getVoxel(WorldCoord(5.0, 5.0, 5.0)).isEmpty());
}

TEST(WorldVoxelAccess, WriteThenReadRoundTrips) {
    World world(terminalLayer());
    world.loadChunk(ChunkCoord{0, 0, 0}, nullptr);  // empty resident chunk

    WorldCoord p(3.5, 10.5, 20.5);
    EXPECT_TRUE(world.setVoxel(p, solid(9)));
    Voxel got = world.getVoxel(p);
    EXPECT_FALSE(got.isEmpty());
    EXPECT_EQ(got.material.palette_index, 9);

    // A neighboring cell in the same chunk is unaffected.
    EXPECT_TRUE(world.getVoxel(WorldCoord(4.5, 10.5, 20.5)).isEmpty());
}

TEST(WorldVoxelAccess, ResolvesAcrossChunksAndNegativeSpace) {
    World world(terminalLayer(1.0, 32));
    world.loadChunk(ChunkCoord{-1, -1, -1}, nullptr);

    // (-1,-1,-1) world maps to local (31,31,31) of chunk (-1,-1,-1).
    WorldCoord p(-0.5, -0.5, -0.5);
    EXPECT_TRUE(world.setVoxel(p, solid(3)));
    EXPECT_EQ(world.getVoxel(p).material.palette_index, 3);

    // The adjacent chunk (0,0,0) is not resident, so a query just across the
    // seam reads empty rather than aliasing into the loaded chunk.
    EXPECT_TRUE(world.getVoxel(WorldCoord(0.5, 0.5, 0.5)).isEmpty());
}

TEST(WorldVoxelAccess, FiniteGridWorldReturnsEmptyAndRejectsWrite) {
    World world(8, 8, 8);  // non-chunked
    EXPECT_TRUE(world.getVoxel(WorldCoord(1.0, 1.0, 1.0)).isEmpty());
    EXPECT_FALSE(world.setVoxel(WorldCoord(1.0, 1.0, 1.0), solid()));
}
