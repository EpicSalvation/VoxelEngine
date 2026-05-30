#pragma once

#include <cstdint>
#include <vector>

#include "world/Chunk.h"

// Plain interleaved vertex for chunk meshes: position + packed ABGR color.
// Deliberately bgfx-free (no VertexLayout member) so the mesh builder can be
// unit-tested headlessly. Its memory layout matches BgfxRenderer's VoxelVertex,
// so the same bgfx::VertexLayout describes both when uploading.
struct MeshVertex {
    float    x, y, z;
    uint32_t abgr;
};

// Builds a triangle mesh for one chunk into out_vertices / out_indices.
//
// Positions are in chunk-local space (voxel (0,0,0) at local origin), so the
// renderer can place the whole chunk with a single floating-origin model
// transform of the chunk's world origin. Only faces adjacent to an empty voxel
// are emitted; interior faces are culled. Faces on the chunk border are always
// emitted (no cross-chunk neighbor lookup in M3) — coincident at seams but not
// a visual artifact. Indices are 32-bit because a dense chunk can exceed 65535
// vertices.
void buildChunkMeshData(const Chunk& chunk,
                        std::vector<MeshVertex>& out_vertices,
                        std::vector<uint32_t>&   out_indices);
