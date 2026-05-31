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
    bool       grounded = false;              // resting on a solid below (down-blocked)
    bool       hitX = false, hitY = false, hitZ = false;  // a wall was hit on that axis
};

// Move an AABB by delta through the world's solid terminal voxels, resolving
// collisions one axis at a time so the box slides along surfaces instead of
// sticking. The motion is substepped (no step larger than half a voxel) so it
// cannot tunnel through thin geometry at speed. Cells in non-resident chunks read
// as empty (no collision), and a non-chunked world applies delta unchanged.
//
// grounded is true when the move was blocked downward this call — apply a small
// gravity delta each frame and a resting box reports grounded every frame.
MoveResult moveAABB(const World& world, const AABB& box, const glm::dvec3& delta);

}  // namespace voxelcollide
