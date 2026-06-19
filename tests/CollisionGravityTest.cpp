// Tests for M16 L2 axis-agnostic collision grounding (VoxelCollision.cpp).
//
// The swept-AABB resolution is per-axis symmetric; only the *interpretation* of
// "grounded" reads the gravity vector. So the per-axis hitX/hitY/hitZ blocking is
// identical regardless of which way is "down", while `grounded` reports resting
// on a surface the box is pressed into *along gravity* — letting a player stand
// on the +X face of an asteroid (radial up) and leaving zero-g with no grounded
// concept. The −Y default is byte-identical to the pre-M16 behavior (regression).

#include "world/VoxelCollision.h"
#include "world/World.h"
#include "world/GravityProvider.h"
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

LayerDef terminalLayer() {
    LayerDef d;
    d.name = "terrain"; d.voxel_size_m = 1.0;
    d.mode = VoxelMode::terminal; d.chunk_size_voxels = 32;
    return d;
}

void setCell(World& w, int64_t x, int64_t y, int64_t z) {
    w.setVoxel(chunkmath::voxelCenter(chunkmath::VoxelCoord{x, y, z}, w.voxelSizeM()), solid());
}

World makeWorld() {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    return w;
}

AABB box(double x, double y, double z) {
    return AABB{WorldCoord(x, y, z), glm::dvec3(0.3, 0.9, 0.3)};
}

// A floor at y voxel 0 (world span [0,1]).
void buildFloor(World& w) {
    for (int x = 0; x < 12; ++x)
        for (int z = 0; z < 12; ++z) setCell(w, x, 0, z);
}

// A wall at x voxel 0 (world span [0,1]) — the surface a -X faller rests on.
void buildWestWall(World& w) {
    for (int y = 0; y < 12; ++y)
        for (int z = 0; z < 12; ++z) setCell(w, 0, y, z);
}

}  // namespace

TEST(CollisionGravity, DefaultNegativeYIsUnchangedRegression) {
    World w = makeWorld();
    buildFloor(w);
    // Explicit -Y gravity reproduces the historical "blocked down ⇒ grounded".
    MoveResult r = moveAABB(w, box(5.0, 6.0, 5.0), glm::dvec3(0, -10, 0),
                            glm::dvec3(0, -1, 0));
    EXPECT_TRUE(r.grounded);
    EXPECT_TRUE(r.hitY);
    EXPECT_NEAR(r.position.value.y, 1.9, 1e-4);

    // Omitting the gravity arg defaults to -Y and must agree exactly.
    MoveResult d = moveAABB(w, box(5.0, 6.0, 5.0), glm::dvec3(0, -10, 0));
    EXPECT_EQ(d.grounded, r.grounded);
    EXPECT_NEAR(d.position.value.y, r.position.value.y, 1e-12);
}

TEST(CollisionGravity, GroundedAgainstAnAlternateFixedAxis) {
    World w = makeWorld();
    buildWestWall(w);

    // Gravity points -X: falling west onto the wall is "grounded" (the wall is a
    // floor in this frame). The box min.x snaps to the wall's max face (1.0).
    MoveResult r = moveAABB(w, box(5.0, 5.0, 5.0), glm::dvec3(-10, 0, 0),
                            glm::dvec3(-1, 0, 0));
    EXPECT_TRUE(r.hitX);
    EXPECT_TRUE(r.grounded);
    EXPECT_NEAR(r.position.value.x, 1.3, 1e-4);

    // The SAME collision under -Y gravity is a wall hit, not a floor: hitX still
    // fires but grounded does not — the per-axis blocking is independent of down.
    MoveResult y = moveAABB(w, box(5.0, 5.0, 5.0), glm::dvec3(-10, 0, 0),
                            glm::dvec3(0, -1, 0));
    EXPECT_TRUE(y.hitX);
    EXPECT_FALSE(y.grounded);
    EXPECT_NEAR(y.position.value.x, 1.3, 1e-4);  // identical snap
}

TEST(CollisionGravity, GroundedOnPlusXFaceUnderRadialGravity) {
    World w = makeWorld();
    buildWestWall(w);

    // A radial well far to the -X: a player near the wall reads "down" ≈ -X, so
    // resting against the +X face of the body (the wall's max face) is grounded —
    // the per-position vector the kinematic step pulls from the provider.
    const GravityProvider g = GravityProvider::radial(WorldCoord(-1000.0, 5.0, 5.0));
    const AABB player = box(5.0, 5.0, 5.0);
    const glm::dvec3 down = g.gravityAt(player.center);
    ASSERT_NEAR(down.x, -1.0, 1e-3);

    MoveResult r = moveAABB(w, player, glm::dvec3(-10, 0, 0), down);
    EXPECT_TRUE(r.hitX);
    EXPECT_TRUE(r.grounded);
    EXPECT_NEAR(r.position.value.x, 1.3, 1e-4);
}

TEST(CollisionGravity, ZeroGHasNoGroundedConcept) {
    World w = makeWorld();
    buildFloor(w);

    // Falling onto the floor still blocks (hitY) but is never grounded under
    // zero gravity — the zero vector makes the grounding product zero.
    MoveResult r = moveAABB(w, box(5.0, 6.0, 5.0), glm::dvec3(0, -10, 0),
                            GravityProvider::zeroG().gravityAt(WorldCoord(5, 6, 5)));
    EXPECT_TRUE(r.hitY);
    EXPECT_FALSE(r.grounded);
    EXPECT_NEAR(r.position.value.y, 1.9, 1e-4);  // still snaps to the surface
}

TEST(CollisionGravity, PerAxisBlockingIsIdenticalRegardlessOfDown) {
    // A +X wall hit resolves to the same position and hitX flag whether down is
    // -Y, -X, or zero — only `grounded` differs by gravity, not the blocking.
    auto runWall = [](const glm::dvec3& gravity) {
        World w = makeWorld();
        for (int y = 0; y < 12; ++y)
            for (int z = 0; z < 12; ++z) setCell(w, 8, y, z);  // wall at x voxel 8
        return moveAABB(w, box(6.0, 5.0, 5.0), glm::dvec3(3, 0, 0), gravity);
    };

    const MoveResult negY = runWall(glm::dvec3(0, -1, 0));
    const MoveResult negX = runWall(glm::dvec3(-1, 0, 0));
    const MoveResult zero = runWall(glm::dvec3(0, 0, 0));

    for (const MoveResult* r : {&negY, &negX, &zero}) {
        EXPECT_TRUE(r->hitX);
        EXPECT_FALSE(r->hitY);
        EXPECT_FALSE(r->hitZ);
        EXPECT_NEAR(r->position.value.x, 7.7, 1e-4);
    }
}
