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
