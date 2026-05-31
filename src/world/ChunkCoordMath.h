#pragma once

#include <cmath>
#include <cstdint>

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

// ── Global voxel coordinates ──────────────────────────────────────────────
//
// A VoxelCoord names a single voxel cell across the whole layer (one unit = one
// voxel), as opposed to ChunkCoord (one unit = one chunk). It is the addressing
// unit for picking, editing, and collision: world-space picks resolve to a
// VoxelCoord, which then decomposes into the owning chunk plus a local index.
//
// 64-bit components: a global voxel index is chunk_coord * chunk_size + local,
// which overflows int32 well within the double-precision world extent this
// engine targets, even though ChunkCoord itself is 32-bit.
struct VoxelCoord {
    int64_t x = 0, y = 0, z = 0;

    bool operator==(const VoxelCoord& rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }
    bool operator!=(const VoxelCoord& rhs) const { return !(*this == rhs); }
};

// A global voxel decomposed into its owning chunk and the local index within
// that chunk. Local coordinates are always in [0, chunkSizeVoxels) and map
// directly onto Chunk::at(x, y, z).
struct LocalVoxel {
    ChunkCoord chunk;
    int        x = 0, y = 0, z = 0;
};

// Floored integer division: rounds toward negative infinity (unlike C++'s
// truncating /), so chunk/local decomposition is correct on the negative side
// of the origin. Divisor is assumed positive (a voxel/chunk count).
inline int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// World position -> the voxel cell that contains it. Floor-based, matching
// worldToChunk: a position exactly on a voxel boundary belongs to the higher
// cell, and negative positions floor toward -inf rather than truncating.
inline VoxelCoord worldToVoxel(const WorldCoord& pos, double voxelSizeM) {
    return VoxelCoord{
        static_cast<int64_t>(std::floor(pos.value.x / voxelSizeM)),
        static_cast<int64_t>(std::floor(pos.value.y / voxelSizeM)),
        static_cast<int64_t>(std::floor(pos.value.z / voxelSizeM)),
    };
}

// World-space corner (minimum x/y/z) of the given voxel cell.
inline WorldCoord voxelOrigin(const VoxelCoord& v, double voxelSizeM) {
    return WorldCoord(static_cast<double>(v.x) * voxelSizeM,
                      static_cast<double>(v.y) * voxelSizeM,
                      static_cast<double>(v.z) * voxelSizeM);
}

// World-space center of the given voxel cell.
inline WorldCoord voxelCenter(const VoxelCoord& v, double voxelSizeM) {
    const double half = voxelSizeM * 0.5;
    return WorldCoord((static_cast<double>(v.x) * voxelSizeM) + half,
                      (static_cast<double>(v.y) * voxelSizeM) + half,
                      (static_cast<double>(v.z) * voxelSizeM) + half);
}

// Decompose a global voxel coord into its owning chunk + local index. The local
// index is the Euclidean remainder, always in [0, chunkSizeVoxels).
inline LocalVoxel voxelToChunkLocal(const VoxelCoord& v, int chunkSizeVoxels) {
    const int64_t n = static_cast<int64_t>(chunkSizeVoxels);
    const int64_t cx = floorDiv(v.x, n);
    const int64_t cy = floorDiv(v.y, n);
    const int64_t cz = floorDiv(v.z, n);
    return LocalVoxel{
        ChunkCoord{static_cast<int32_t>(cx),
                   static_cast<int32_t>(cy),
                   static_cast<int32_t>(cz)},
        static_cast<int>(v.x - cx * n),
        static_cast<int>(v.y - cy * n),
        static_cast<int>(v.z - cz * n),
    };
}

// Recombine a chunk coord + local index into a global voxel coord. Inverse of
// voxelToChunkLocal (for any local in [0, chunkSizeVoxels)).
inline VoxelCoord chunkLocalToVoxel(const ChunkCoord& c, int lx, int ly, int lz,
                                    int chunkSizeVoxels) {
    const int64_t n = static_cast<int64_t>(chunkSizeVoxels);
    return VoxelCoord{
        static_cast<int64_t>(c.x) * n + lx,
        static_cast<int64_t>(c.y) * n + ly,
        static_cast<int64_t>(c.z) * n + lz,
    };
}

}  // namespace chunkmath
