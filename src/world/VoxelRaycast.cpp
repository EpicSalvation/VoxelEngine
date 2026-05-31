#include "VoxelRaycast.h"
#include "World.h"

#include <limits>

namespace voxelcast {

RayHit raycast(const World& world, const WorldCoord& origin,
               const glm::dvec3& dir, double maxDistanceM) {
    RayHit result;
    if (!world.isChunked()) return result;

    const double len = glm::length(dir);
    if (len <= 0.0) return result;
    const glm::dvec3 d  = dir / len;
    const double     vs = world.voxelSizeM();

    chunkmath::VoxelCoord cur = chunkmath::worldToVoxel(origin, vs);

    // Per-axis traversal state (Amanatides & Woo): which way each axis steps, the
    // ray parameter t at which we cross into the next voxel along that axis, and
    // how much t advances per whole voxel. t is in world meters (dir is unit), so
    // it compares directly against maxDistanceM.
    const double  o[3]    = {origin.value.x, origin.value.y, origin.value.z};
    const double  dd[3]   = {d.x, d.y, d.z};
    const int64_t cell[3] = {cur.x, cur.y, cur.z};
    int    step[3];
    double tMax[3];
    double tDelta[3];
    constexpr double kInf = std::numeric_limits<double>::infinity();

    for (int a = 0; a < 3; ++a) {
        if (dd[a] > 0.0) {
            step[a]   = 1;
            tMax[a]   = (static_cast<double>(cell[a] + 1) * vs - o[a]) / dd[a];
            tDelta[a] = vs / dd[a];
        } else if (dd[a] < 0.0) {
            step[a]   = -1;
            tMax[a]   = (static_cast<double>(cell[a]) * vs - o[a]) / dd[a];
            tDelta[a] = vs / -dd[a];
        } else {
            step[a]   = 0;
            tMax[a]   = kInf;
            tDelta[a] = kInf;
        }
    }

    glm::ivec3 normal{0, 0, 0};  // zero on the start cell (eye is inside it)
    double     t = 0.0;

    while (t <= maxDistanceM) {
        const Voxel v = world.getVoxel(chunkmath::voxelCenter(cur, vs));
        if (!v.isEmpty()) {
            result.hit      = true;
            result.voxel    = cur;
            result.normal   = normal;
            result.adjacent = chunkmath::VoxelCoord{cur.x + normal.x,
                                                    cur.y + normal.y,
                                                    cur.z + normal.z};
            result.distance = t;
            return result;
        }

        // Advance into the neighbor across the nearest axis boundary.
        int axis = 0;
        if (tMax[1] < tMax[axis]) axis = 1;
        if (tMax[2] < tMax[axis]) axis = 2;
        if (tMax[axis] == kInf) break;  // ray is axis-degenerate and found nothing

        switch (axis) {
            case 0: cur.x += step[0]; t = tMax[0]; tMax[0] += tDelta[0]; normal = {-step[0], 0, 0}; break;
            case 1: cur.y += step[1]; t = tMax[1]; tMax[1] += tDelta[1]; normal = {0, -step[1], 0}; break;
            default: cur.z += step[2]; t = tMax[2]; tMax[2] += tDelta[2]; normal = {0, 0, -step[2]}; break;
        }
    }

    return result;
}

}  // namespace voxelcast
