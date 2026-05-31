// Tests for AABB-vs-voxel collision resolution (src/world/VoxelCollision.cpp):
// resting on ground, no tunneling at speed, wall stops, sliding, and grounding.

#include "world/VoxelCollision.h"
#include "world/World.h"
#include "world/ChunkCoordMath.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

using voxelcollide::AABB;
using voxelcollide::MoveResult;
using voxelcollide::moveAABB;

namespace {

Voxel solid() {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = 2;
    return v;
}

LayerDef terminalLayer(double voxelSize = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name = "terrain"; d.voxel_size_m = voxelSize;
    d.mode = VoxelMode::terminal; d.chunk_size_voxels = chunkSize;
    return d;
}

void setCell(World& w, int64_t x, int64_t y, int64_t z) {
    w.setVoxel(chunkmath::voxelCenter(chunkmath::VoxelCoord{x, y, z}, w.voxelSizeM()), solid());
}

// A 32^3 world with chunk {0,0,0} resident; empty unless cells are added.
World makeWorld() {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    return w;
}

// Player-ish box: 0.6 wide, 1.8 tall.
AABB box(double x, double y, double z) {
    return AABB{WorldCoord(x, y, z), glm::dvec3(0.3, 0.9, 0.3)};
}

}  // namespace

TEST(VoxelCollision, FallsAndRestsOnFloor) {
    World w = makeWorld();
    for (int x = 0; x < 12; ++x)
        for (int z = 0; z < 12; ++z)
            setCell(w, x, 0, z);  // floor voxels y=0 span world [0,1]

    MoveResult r = moveAABB(w, box(5.0, 6.0, 5.0), glm::dvec3(0, -10, 0));
    EXPECT_TRUE(r.grounded);
    EXPECT_TRUE(r.hitY);
    // Box bottom rests on the floor top (y=1.0): center.y = 1.0 + halfY(0.9).
    EXPECT_NEAR(r.position.value.y, 1.9, 1e-4);
}

TEST(VoxelCollision, DoesNotTunnelThroughThinFloorAtSpeed) {
    World w = makeWorld();
    for (int x = 0; x < 12; ++x)
        for (int z = 0; z < 12; ++z)
            setCell(w, x, 0, z);  // single 1-thick floor layer

    // A large downward delta in one call must still stop on the floor.
    MoveResult r = moveAABB(w, box(5.0, 20.0, 5.0), glm::dvec3(0, -50, 0));
    EXPECT_TRUE(r.grounded);
    EXPECT_NEAR(r.position.value.y, 1.9, 1e-4);
}

TEST(VoxelCollision, StopsAtAWall) {
    World w = makeWorld();
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 12; ++z)
            setCell(w, 8, y, z);  // wall at x voxel 8, spans world [8,9]

    MoveResult r = moveAABB(w, box(6.0, 5.0, 5.0), glm::dvec3(3, 0, 0));
    EXPECT_TRUE(r.hitX);
    // Box max.x snaps to the wall's min face (8.0): center.x = 8.0 - halfX(0.3).
    EXPECT_NEAR(r.position.value.x, 7.7, 1e-4);
}

TEST(VoxelCollision, SlidesAlongWall) {
    World w = makeWorld();
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 12; ++z)
            setCell(w, 8, y, z);  // wall blocks +x only

    MoveResult r = moveAABB(w, box(6.0, 5.0, 5.0), glm::dvec3(3, 0, 3));
    EXPECT_TRUE(r.hitX);
    EXPECT_FALSE(r.hitZ);
    EXPECT_NEAR(r.position.value.x, 7.7, 1e-4);  // x stopped
    EXPECT_NEAR(r.position.value.z, 8.0, 1e-4);  // z proceeded fully
}

TEST(VoxelCollision, MovesFreelyInOpenSpace) {
    World w = makeWorld();  // empty chunk
    MoveResult r = moveAABB(w, box(5.0, 5.0, 5.0), glm::dvec3(2, -1, 3));
    EXPECT_FALSE(r.hitX);
    EXPECT_FALSE(r.hitY);
    EXPECT_FALSE(r.hitZ);
    EXPECT_FALSE(r.grounded);
    EXPECT_NEAR(r.position.value.x, 7.0, 1e-9);
    EXPECT_NEAR(r.position.value.y, 4.0, 1e-9);
    EXPECT_NEAR(r.position.value.z, 8.0, 1e-9);
}

TEST(VoxelCollision, RestingBoxStaysGroundedUnderTinyGravity) {
    World w = makeWorld();
    for (int x = 0; x < 12; ++x)
        for (int z = 0; z < 12; ++z)
            setCell(w, x, 0, z);

    // Box already resting on the floor; a tiny downward step (one frame of
    // gravity) keeps it grounded at the same height.
    MoveResult r = moveAABB(w, box(5.0, 1.9, 5.0), glm::dvec3(0, -0.01, 0));
    EXPECT_TRUE(r.grounded);
    EXPECT_NEAR(r.position.value.y, 1.9, 1e-4);
}

TEST(VoxelCollision, HitsCeilingMovingUp) {
    World w = makeWorld();
    for (int x = 0; x < 12; ++x)
        for (int z = 0; z < 12; ++z)
            setCell(w, x, 10, z);  // ceiling voxels y=10 span world [10,11]

    MoveResult r = moveAABB(w, box(5.0, 8.0, 5.0), glm::dvec3(0, 5, 0));
    EXPECT_TRUE(r.hitY);
    EXPECT_FALSE(r.grounded);  // up-block is not grounding
    // Box top snaps to the ceiling's min face (10.0): center.y = 10.0 - halfY.
    EXPECT_NEAR(r.position.value.y, 9.1, 1e-4);
}

TEST(VoxelCollision, NonChunkedWorldAppliesDeltaUnchanged) {
    World w(8, 8, 8);
    MoveResult r = moveAABB(w, box(1.0, 1.0, 1.0), glm::dvec3(2, 3, 4));
    EXPECT_NEAR(r.position.value.x, 3.0, 1e-9);
    EXPECT_NEAR(r.position.value.y, 4.0, 1e-9);
    EXPECT_NEAR(r.position.value.z, 5.0, 1e-9);
}
