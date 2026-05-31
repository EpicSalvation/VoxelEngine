#pragma once

#include <glm/glm.hpp>

#include "WorldCoord.h"
#include "ChunkCoordMath.h"

class World;

// DDA voxel picking against the chunked world (M5). The single place ray/voxel
// traversal lives — built on World's world-space voxel accessor, double-only per
// the floating-origin rule (never narrow a world position to float here).
namespace voxelcast {

struct RayHit {
    bool                  hit      = false;          // false: nothing solid within reach
    chunkmath::VoxelCoord voxel{};                   // the solid voxel struck (remove target)
    chunkmath::VoxelCoord adjacent{};                // empty cell on the entry face (place target)
    glm::ivec3            normal{0, 0, 0};           // face normal, pointing back toward the origin
    double                distance = 0.0;            // world-space distance to the struck voxel
};

// Casts a ray from origin along dir (need not be normalized) through the world's
// resident voxels, stopping at the first solid terminal voxel within maxDistanceM.
//
// Uses Amanatides & Woo grid traversal in double precision. Cells in non-resident
// chunks read as empty, so the ray passes through unstreamed regions (you cannot
// pick what is not loaded). `adjacent` is the cell the ray entered the hit voxel
// from — the cell a placed voxel goes into; for a hit it is guaranteed empty
// (it is the last cell stepped through before the solid one).
RayHit raycast(const World& world, const WorldCoord& origin,
               const glm::dvec3& dir, double maxDistanceM);

}  // namespace voxelcast
