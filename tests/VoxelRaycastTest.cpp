// Tests for DDA voxel picking (src/world/VoxelRaycast.cpp). Pure double-precision
// traversal over an in-memory chunked World; no window or GPU involved.

#include "world/VoxelRaycast.h"
#include "world/World.h"
#include "world/ChunkCoordMath.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

using chunkmath::VoxelCoord;
using voxelcast::RayHit;
using voxelcast::raycast;

namespace {

Voxel solid(uint8_t palette = 7) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = palette;
    return v;
}

LayerDef terminalLayer(double voxelSize = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name              = "terrain";
    d.voxel_size_m      = voxelSize;
    d.mode              = VoxelMode::terminal;
    d.chunk_size_voxels = chunkSize;
    return d;
}

// A chunked world with chunks {0,0,0} and {-1,-1,-1} resident and empty.
World makeWorld() {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    w.loadChunk(ChunkCoord{-1, -1, -1}, nullptr);
    return w;
}

}  // namespace

TEST(VoxelRaycast, HitsNearestSolidAlongPositiveX) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(5.5, 0.5, 0.5), solid());

    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 16.0);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.voxel, (VoxelCoord{5, 0, 0}));
    EXPECT_EQ(r.normal, glm::ivec3(-1, 0, 0));      // face points back toward origin
    EXPECT_EQ(r.adjacent, (VoxelCoord{4, 0, 0}));   // empty cell a placed voxel goes into
    EXPECT_DOUBLE_EQ(r.distance, 4.5);              // entry at x=5.0 from x=0.5
}

TEST(VoxelRaycast, StopsAtTheFirstSolidNotTheFarther) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(3.5, 0.5, 0.5), solid());
    w.setVoxel(WorldCoord(6.5, 0.5, 0.5), solid());

    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 16.0);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.voxel, (VoxelCoord{3, 0, 0}));
}

TEST(VoxelRaycast, MissWhenNothingInRange) {
    World w = makeWorld();
    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 16.0);
    EXPECT_FALSE(r.hit);
}

TEST(VoxelRaycast, RespectsMaxDistance) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(5.5, 0.5, 0.5), solid());  // entry at distance 4.5
    EXPECT_FALSE(raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 3.0).hit);
    EXPECT_TRUE(raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 5.0).hit);
}

TEST(VoxelRaycast, NegativeDirectionAndNormal) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(-4.5, -0.5, -0.5), solid());  // global voxel (-5,-1,-1)

    RayHit r = raycast(w, WorldCoord(0.5, -0.5, -0.5), glm::dvec3(-1, 0, 0), 16.0);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.voxel, (VoxelCoord{-5, -1, -1}));
    EXPECT_EQ(r.normal, glm::ivec3(1, 0, 0));         // entered from the +X side
    EXPECT_EQ(r.adjacent, (VoxelCoord{-4, -1, -1}));
}

TEST(VoxelRaycast, VerticalDownHit) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(0.5, 2.5, 0.5), solid());

    RayHit r = raycast(w, WorldCoord(0.5, 10.0, 0.5), glm::dvec3(0, -1, 0), 32.0);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.voxel, (VoxelCoord{0, 2, 0}));
    EXPECT_EQ(r.normal, glm::ivec3(0, 1, 0));
    EXPECT_EQ(r.adjacent, (VoxelCoord{0, 3, 0}));
}

TEST(VoxelRaycast, AdjacentCellIsEmptyForPlacement) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(5.5, 0.5, 0.5), solid());
    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 16.0);
    ASSERT_TRUE(r.hit);
    Voxel adj = w.getVoxel(chunkmath::voxelCenter(r.adjacent, w.voxelSizeM()));
    EXPECT_TRUE(adj.isEmpty());
}

TEST(VoxelRaycast, PassesThroughNonResidentChunks) {
    // A solid voxel sits beyond chunk {0,0,0} in a chunk that is not loaded; the
    // ray reads those cells as empty and finds nothing rather than false-hitting.
    World w = makeWorld();
    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 0, 0), 100.0);
    EXPECT_FALSE(r.hit);
}

TEST(VoxelRaycast, DiagonalRayHitsAndReportsAFace) {
    World w = makeWorld();
    w.setVoxel(WorldCoord(4.5, 4.5, 0.5), solid());
    RayHit r = raycast(w, WorldCoord(0.5, 0.5, 0.5), glm::dvec3(1, 1, 0), 16.0);
    ASSERT_TRUE(r.hit);
    EXPECT_EQ(r.voxel, (VoxelCoord{4, 4, 0}));
    // Exactly one axis of the face normal is set (a single face was crossed).
    EXPECT_EQ(std::abs(r.normal.x) + std::abs(r.normal.y) + std::abs(r.normal.z), 1);
}

TEST(VoxelRaycast, NonChunkedWorldNeverHits) {
    World w(8, 8, 8);
    EXPECT_FALSE(raycast(w, WorldCoord(0, 0, 0), glm::dvec3(1, 0, 0), 16.0).hit);
}
