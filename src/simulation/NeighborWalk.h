#pragma once

#include <array>

#include "world/ChunkCoordMath.h"

// Shared deterministic 6-connected neighbor-walk primitives (M13/M14
// cleanup, docs/ARCHITECTURE.md §17). The M13 support flood
// (PropagationSystem) and the M14 field passes (FieldOverlay/ThermalSystem/
// FluidSystem) all expand a sparse active set through its axis-neighbors in
// sorted-coord order — this is the one place that shape lives, so each
// caller's output stays byte-identical without re-deriving the same ordering
// rule independently.

namespace sim {

// Strict-weak ordering on VoxelCoord (lexicographic x,y,z). Every sorted-set
// driven pass (the support flood, the dirty/carry queues, the field overlays)
// uses this exact order so detection output is byte-identical across runs
// (§4 determinism contract).
struct VoxelCoordLess {
    bool operator()(const chunkmath::VoxelCoord& a,
                    const chunkmath::VoxelCoord& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    }
};

// The 6 axis-neighbors of a voxel coord (axis-free: no privileged "down").
inline std::array<chunkmath::VoxelCoord, 6> neighbors6(chunkmath::VoxelCoord c) {
    return {chunkmath::VoxelCoord{c.x - 1, c.y, c.z}, chunkmath::VoxelCoord{c.x + 1, c.y, c.z},
            chunkmath::VoxelCoord{c.x, c.y - 1, c.z}, chunkmath::VoxelCoord{c.x, c.y + 1, c.z},
            chunkmath::VoxelCoord{c.x, c.y, c.z - 1}, chunkmath::VoxelCoord{c.x, c.y, c.z + 1}};
}

}  // namespace sim
