#include "VoxelCollision.h"
#include "World.h"
#include "ChunkCoordMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace voxelcollide {
namespace {

// Is any solid voxel overlapped by the world-space box [mn, mx]? Faces that only
// touch (within eps) are excluded, so a box resting exactly on a surface is not
// counted as penetrating it.
bool anySolidInAABB(const World& world, const glm::dvec3& mn, const glm::dvec3& mx,
                    double vs, double eps) {
    const int64_t lox = static_cast<int64_t>(std::floor((mn.x + eps) / vs));
    const int64_t hix = static_cast<int64_t>(std::floor((mx.x - eps) / vs));
    const int64_t loy = static_cast<int64_t>(std::floor((mn.y + eps) / vs));
    const int64_t hiy = static_cast<int64_t>(std::floor((mx.y - eps) / vs));
    const int64_t loz = static_cast<int64_t>(std::floor((mn.z + eps) / vs));
    const int64_t hiz = static_cast<int64_t>(std::floor((mx.z - eps) / vs));
    if (hix < lox || hiy < loy || hiz < loz) return false;  // degenerate

    for (int64_t z = loz; z <= hiz; ++z)
        for (int64_t y = loy; y <= hiy; ++y)
            for (int64_t x = lox; x <= hix; ++x) {
                const WorldCoord c = chunkmath::voxelCenter(chunkmath::VoxelCoord{x, y, z}, vs);
                // Sample every layer at its own scale: the player collides with
                // composite blocks and the immutable backdrop, not just terminal
                // voxels. The iteration grid is the primary (terminal) voxel size;
                // coarser layers read solid across all the fine cells they cover.
                if (world.anySolidAt(c)) return true;
            }
    return false;
}

}  // namespace

MoveResult moveAABB(const World& world, const AABB& box, const glm::dvec3& delta) {
    MoveResult result;
    result.position = box.center;

    if (!world.isChunked()) {
        result.position = WorldCoord(box.center.value + delta);
        return result;
    }

    const double      vs   = world.voxelSizeM();
    const double      eps  = vs * 1e-4;
    const glm::dvec3& half = box.halfExtents;

    // Substep so no single step moves more than half a voxel — keeps penetration
    // within one cell, which the per-axis snap below assumes.
    const double maxComp = std::max({std::abs(delta.x), std::abs(delta.y), std::abs(delta.z)});
    int steps = std::max(1, static_cast<int>(std::ceil(maxComp / (0.5 * vs))));
    steps = std::min(steps, 256);
    const glm::dvec3 d = delta / static_cast<double>(steps);

    glm::dvec3 c = box.center.value;
    for (int s = 0; s < steps; ++s) {
        // Resolve each axis independently, applying its motion then snapping out
        // of any solid it entered, so the box slides rather than stops dead.
        for (int axis = 0; axis < 3; ++axis) {
            if (d[axis] == 0.0) continue;
            c[axis] += d[axis];

            const glm::dvec3 mn = c - half;
            const glm::dvec3 mx = c + half;
            if (!anySolidInAABB(world, mn, mx, vs, eps)) continue;

            if (d[axis] > 0.0) {
                const int64_t cell = static_cast<int64_t>(std::floor((mx[axis] - eps) / vs));
                c[axis] = static_cast<double>(cell) * vs - half[axis];  // box max -> cell min face
            } else {
                const int64_t cell = static_cast<int64_t>(std::floor((mn[axis] + eps) / vs));
                c[axis] = static_cast<double>(cell + 1) * vs + half[axis];  // box min -> cell max face
                if (axis == 1) result.grounded = true;
            }

            if (axis == 0) result.hitX = true;
            else if (axis == 1) result.hitY = true;
            else result.hitZ = true;
        }
    }

    result.position = WorldCoord(c);
    return result;
}

}  // namespace voxelcollide
