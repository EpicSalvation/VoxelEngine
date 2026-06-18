#pragma once

#include <cstdint>
#include <vector>

#include "world/Chunk.h"

// Plain interleaved vertex for chunk meshes: position + packed ABGR color +
// tile-local atlas UV (u,v) + the bound tile's atlas sub-rect (r0..r3).
// Deliberately bgfx-free (no VertexLayout member) so the mesh builder can be
// unit-tested headlessly. Its memory layout matches BgfxRenderer's VoxelVertex,
// so the same bgfx::VertexLayout describes both when uploading.
//
// Texturing model (M15 T1/T2/T5): (u,v) is a TILE-LOCAL coordinate scaled by
// face_world_size × tiling_factor, so a tile REPEATS across a large face rather
// than stretching (a 1 m face → [0,1], an 8 m face → [0,8]). (r0,r1,r2,r3) is the
// tile's atlas sub-rect (u0,v0,u1,v1); the fragment shader wraps the in-tile
// coordinate into it with frac() — hardware REPEAT cannot wrap a sub-rectangle of
// an atlas. The per-face color stays as a shade/translucency modulate on top of
// the sampled texel.
//
// Until content binds per-face tiles (T4/T5), the builder emits (u,v)=(0,0) and
// the full-atlas sub-rect (0,0,1,1); with the renderer's 1×1 white atlas that
// samples white, so the per-face color renders unchanged — colored worlds are
// byte-identical to the pre-texture pipeline.
struct MeshVertex {
    float    x, y, z;
    uint32_t abgr;
    float    u, v;              // tile-local atlas UV (repeat space)
    float    r0, r1, r2, r3;    // atlas sub-rect of the bound tile: u0,v0,u1,v1
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
// voxel_size_m is the edge length of one voxel in meters (the layer's world
// scale). It scales the emitted tile-local UV span so textures tile at a fixed
// world density regardless of voxel size (T5): a face's UV span is
// voxel_size_m × tiling_factor. The default 1.0 reproduces the per-voxel-equals-
// one-tile behavior used by the existing single-scale callers and tests.
void buildChunkMeshData(const Chunk& chunk,
                        std::vector<MeshVertex>& out_vertices,
                        std::vector<uint32_t>&   out_opaque_indices,
                        std::vector<uint32_t>&   out_translucent_indices,
                        double                   voxel_size_m = 1.0);
