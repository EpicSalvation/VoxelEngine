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

// Builds a triangle mesh for one chunk into out_vertices, with face indices
// split into two batches that share the vertex array:
//   - out_opaque_indices      — faces of opaque voxels (alpha == 0xff)
//   - out_translucent_indices — faces of translucent voxels (alpha < 0xff, e.g.
//                               water); drawn by the renderer in a blended pass.
//
// Positions are in chunk-local space (voxel (0,0,0) at local origin), so the
// renderer can place the whole chunk with a single floating-origin model
// transform of the chunk's world origin. Only faces adjacent to an empty voxel
// are emitted; interior faces are culled. Indices are 32-bit because a dense
// chunk can exceed 65535 vertices.
//
// Border behavior differs by translucency. Opaque faces on the chunk border are
// always emitted (no cross-chunk neighbor lookup; coincident at seams but hidden
// because they are opaque and back-to-back). Translucent faces on the border are
// *not* emitted: water continues across the seam, and emitting both sides would
// double-blend into a visible grid of walls. The trade-off is a missing edge
// only where a translucent body genuinely ends exactly on a chunk boundary.
void buildChunkMeshData(const Chunk& chunk,
                        std::vector<MeshVertex>& out_vertices,
                        std::vector<uint32_t>&   out_opaque_indices,
                        std::vector<uint32_t>&   out_translucent_indices);
