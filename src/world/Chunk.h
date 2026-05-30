#pragma once

#include <cstdint>
#include <vector>

#include "WorldCoord.h"
#include "Voxel.h"

// Integer coordinate of a chunk in the chunk grid. One unit = one chunk side.
// World position is recovered via ChunkCoordMath::chunkOrigin.
struct ChunkCoord {
    int32_t x = 0, y = 0, z = 0;

    bool operator==(const ChunkCoord& rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }
    bool operator!=(const ChunkCoord& rhs) const { return !(*this == rhs); }
};

// Hash for using ChunkCoord as an unordered_map key. Mixes the three 32-bit
// components into a 64-bit hash (no allocation, no ordering dependence).
struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const noexcept {
        uint64_t h = static_cast<uint32_t>(c.x);
        h = h * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(c.y);
        h = h * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(c.z);
        h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

// A fixed-size cubic block of voxels — the unit of world streaming (M3).
// Voxels are stored flat, row-major with x varying fastest, matching the
// LayerGeneratorFn contract in plugin_api.h so a generator can fill data()
// directly.
class Chunk {
public:
    Chunk(ChunkCoord coord, int sizeVoxels, WorldCoord origin)
        : coord_(coord),
          size_(sizeVoxels),
          origin_(origin),
          voxels_(static_cast<size_t>(sizeVoxels) * sizeVoxels * sizeVoxels,
                  Voxel::empty()) {}

    const Voxel& at(int x, int y, int z) const { return voxels_[idx(x, y, z)]; }
    Voxel&       at(int x, int y, int z)       { return voxels_[idx(x, y, z)]; }

    Voxel*       data()       { return voxels_.data(); }
    const Voxel* data() const { return voxels_.data(); }

    int        size()   const { return size_; }    // voxels per side
    ChunkCoord coord()  const { return coord_; }
    WorldCoord origin() const { return origin_; }   // world-space corner of the chunk

private:
    ChunkCoord         coord_;
    int                size_;
    WorldCoord         origin_;
    std::vector<Voxel> voxels_;

    int idx(int x, int y, int z) const { return x + size_ * (y + size_ * z); }
};
