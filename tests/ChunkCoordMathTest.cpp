// Tests for world <-> chunk coordinate math (src/world/ChunkCoordMath.h).
// These are pure double-precision functions; no window or GPU involved.

#include "world/ChunkCoordMath.h"

#include <gtest/gtest.h>

using chunkmath::worldToChunk;
using chunkmath::chunkOrigin;

TEST(ChunkCoordMath, OriginPositionIsChunkZero) {
    ChunkCoord c = worldToChunk(WorldCoord(0.0, 0.0, 0.0), 1.0, 32);
    EXPECT_EQ(c.x, 0);
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.z, 0);
}

TEST(ChunkCoordMath, PositionsWithinFirstChunkMapToZero) {
    // With 1m voxels and 32-voxel chunks, [0,32) maps to chunk 0.
    EXPECT_EQ(worldToChunk(WorldCoord(31.9, 0.0, 0.0), 1.0, 32).x, 0);
    EXPECT_EQ(worldToChunk(WorldCoord(32.0, 0.0, 0.0), 1.0, 32).x, 1);
}

TEST(ChunkCoordMath, NegativeCoordsUseFloorNotTruncation) {
    // A small negative offset belongs to chunk -1, not 0 (which truncation gives).
    EXPECT_EQ(worldToChunk(WorldCoord(-0.5, 0.0, 0.0), 1.0, 32).x, -1);
    EXPECT_EQ(worldToChunk(WorldCoord(-32.0, 0.0, 0.0), 1.0, 32).x, -1);
    EXPECT_EQ(worldToChunk(WorldCoord(-33.0, 0.0, 0.0), 1.0, 32).x, -2);
}

TEST(ChunkCoordMath, OriginRoundTrips) {
    // chunkOrigin of a chunk maps back into that same chunk.
    for (int32_t cx : {-3, -1, 0, 2, 5}) {
        ChunkCoord c{cx, 0, 0};
        WorldCoord o = chunkOrigin(c, 1.0, 32);
        EXPECT_EQ(worldToChunk(o, 1.0, 32).x, cx);
    }
}

TEST(ChunkCoordMath, RespectsVoxelSizeAndChunkSize) {
    // 2m voxels, 16-voxel chunks -> chunk world size 32m.
    EXPECT_EQ(worldToChunk(WorldCoord(31.0, 0.0, 0.0), 2.0, 16).x, 0);
    EXPECT_EQ(worldToChunk(WorldCoord(32.0, 0.0, 0.0), 2.0, 16).x, 1);
    WorldCoord o = chunkOrigin(ChunkCoord{2, 0, 0}, 2.0, 16);
    EXPECT_DOUBLE_EQ(o.value.x, 64.0);
}

// Cross-layer voxel mapping (M6) --------------------------------------------

TEST(ChunkCoordMath, LayerRatioIsTheWholeSizeQuotient) {
    EXPECT_EQ(chunkmath::layerRatio(32.0, 1.0), 32);
    EXPECT_EQ(chunkmath::layerRatio(100.0, 1.0), 100);
    EXPECT_EQ(chunkmath::layerRatio(8.0, 2.0), 4);
    // Tolerates floating-point representation error around a whole quotient.
    EXPECT_EQ(chunkmath::layerRatio(1.0, 0.1), 10);
}

TEST(ChunkCoordMath, ChildVoxelMinIsParentTimesRatio) {
    const int64_t r = 4;
    EXPECT_EQ(chunkmath::childVoxelMin(chunkmath::VoxelCoord{0, 0, 0}, r),
              (chunkmath::VoxelCoord{0, 0, 0}));
    EXPECT_EQ(chunkmath::childVoxelMin(chunkmath::VoxelCoord{1, 2, 3}, r),
              (chunkmath::VoxelCoord{4, 8, 12}));
    EXPECT_EQ(chunkmath::childVoxelMin(chunkmath::VoxelCoord{-1, 0, 2}, r),
              (chunkmath::VoxelCoord{-4, 0, 8}));
}

TEST(ChunkCoordMath, ChildToParentInvertsAcrossTheChildBlock) {
    const int64_t r = 4;
    // Every child voxel inside a parent's ratio^3 block maps back to that parent,
    // including on the negative side of the origin.
    for (chunkmath::VoxelCoord parent : {chunkmath::VoxelCoord{0, 0, 0},
                                         chunkmath::VoxelCoord{2, -3, 5},
                                         chunkmath::VoxelCoord{-1, -1, -1}}) {
        const chunkmath::VoxelCoord base = chunkmath::childVoxelMin(parent, r);
        for (int64_t dz = 0; dz < r; ++dz)
            for (int64_t dy = 0; dy < r; ++dy)
                for (int64_t dx = 0; dx < r; ++dx) {
                    const chunkmath::VoxelCoord child{base.x + dx, base.y + dy, base.z + dz};
                    EXPECT_EQ(chunkmath::childToParentVoxel(child, r), parent);
                }
    }
}
