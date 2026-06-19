#pragma once

#include <glm/glm.hpp>

#include "WorldCoord.h"

class World;

// Axis-aligned bounding-box collision against solid terminal voxels (M5). The
// single place player/voxel collision math lives — double-only per the
// floating-origin rule, built on World's world-space voxel accessor.
namespace voxelcollide {

struct AABB {
    WorldCoord center;        // world-space center
    glm::dvec3 halfExtents{}; // half-size on each axis, in meters
};

struct MoveResult {
    WorldCoord position;                      // resolved center after the move
    bool       grounded = false;              // resting on a solid "below" (blocked
                                              // along the gravity vector)
    bool       hitX = false, hitY = false, hitZ = false;  // a wall was hit on that axis
};

// Move an AABB by delta through the world's solid terminal voxels, resolving
// collisions one axis at a time so the box slides along surfaces instead of
// sticking. The motion is substepped (no step larger than half a voxel) so it
// cannot tunnel through thin geometry at speed. Cells in non-resident chunks read
// as empty (no collision), and a non-chunked world applies delta unchanged.
//
// grounded is true when the move was blocked along the gravity vector this call —
// i.e. the box pressed into a surface that gravity holds it against (a floor).
// `gravity_dir` is the canonical "down" the kinematic step supplies from the
// GravityProvider (M16, L2): the default constant -Y reproduces the historical
// "blocked downward ⇒ grounded" exactly, an alternate fixed axis lets a player
// stand on a wall, a per-position radial vector lets one walk the +X face of an
// asteroid, and zero-g (the zero vector) degenerates to "no grounded concept".
// The per-axis hitX/hitY/hitZ blocking is identical regardless of which way is
// "down". Apply a small gravity delta each frame and a resting box reports
// grounded every frame.
MoveResult moveAABB(const World& world, const AABB& box, const glm::dvec3& delta,
                    const glm::dvec3& gravity_dir = glm::dvec3(0.0, -1.0, 0.0));

}  // namespace voxelcollide
