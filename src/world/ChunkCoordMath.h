#pragma once

#include <cmath>

#include "WorldCoord.h"
#include "Chunk.h"

// Conversions between world-space positions (WorldCoord, double precision) and
// integer chunk coordinates. These are pure functions and the ONLY place chunk
// indexing math lives — keep them double-only. Per the floating-origin rule
// (docs/ARCHITECTURE.md §1), never narrow a world position to float here.
namespace chunkmath {

// World size of one chunk along an axis, in meters.
inline double chunkWorldSize(double voxelSizeM, int chunkSizeVoxels) {
    return voxelSizeM * static_cast<double>(chunkSizeVoxels);
}

// Which chunk contains the given world position. Uses floor (not truncation)
// so coordinates on the negative side of the origin map to the correct chunk.
inline ChunkCoord worldToChunk(const WorldCoord& pos, double voxelSizeM,
                               int chunkSizeVoxels) {
    const double s = chunkWorldSize(voxelSizeM, chunkSizeVoxels);
    return ChunkCoord{
        static_cast<int32_t>(std::floor(pos.value.x / s)),
        static_cast<int32_t>(std::floor(pos.value.y / s)),
        static_cast<int32_t>(std::floor(pos.value.z / s)),
    };
}

// World-space corner (minimum x/y/z) of the given chunk.
inline WorldCoord chunkOrigin(const ChunkCoord& c, double voxelSizeM,
                              int chunkSizeVoxels) {
    const double s = chunkWorldSize(voxelSizeM, chunkSizeVoxels);
    return WorldCoord(static_cast<double>(c.x) * s,
                      static_cast<double>(c.y) * s,
                      static_cast<double>(c.z) * s);
}

}  // namespace chunkmath
