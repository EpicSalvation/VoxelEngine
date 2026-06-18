// Tests for the M15 T1/T5 vertex format: MeshVertex carries a tile-local atlas UV
// (TexCoord0) plus the bound tile's atlas sub-rect (TexCoord1, u0,v0,u1,v1).
// MeshVertex (headless, src/renderer/ChunkMeshData.h) and the GPU VoxelVertex
// (src/renderer/BgfxRenderer.h) must stay byte-compatible because ChunkMesh
// uploads MeshVertex memory through VoxelVertex::layout. VoxelVertex pulls in
// bgfx headers (PRIVATE to the engine, not on the test include path), so this
// asserts MeshVertex's own field offsets against the exact layout
// VoxelVertex::initLayout describes — Position(3×float) @0, Color0(4×uint8) @12,
// TexCoord0(2×float) @16, TexCoord1(4×float) @24, stride 40 — the contract both
// sides share.
//
// It also pins the white-tile regression: an untextured (unbound) material emits
// UV (0,0) over the full-atlas sub-rect (0,0,1,1) on every vertex, so sampling the
// 1×1 white atlas is a no-op and colored worlds render byte-identically to the
// pre-texture pipeline.

#include "renderer/ChunkMeshData.h"
#include "world/Chunk.h"

#include <gtest/gtest.h>
#include <cstddef>

namespace {

Voxel solid() {
    Voxel v;
    v.material.palette_index = 1;  // stone — opaque palette entry
    v.material.density       = 1.0f;
    return v;
}

}  // namespace

TEST(VertexLayout, MeshVertexMatchesGpuLayoutOffsets) {
    // These offsets MUST match VoxelVertex::initLayout in BgfxRenderer.cpp.
    EXPECT_EQ(offsetof(MeshVertex, x),    0u);   // Position float3
    EXPECT_EQ(offsetof(MeshVertex, abgr), 12u);  // Color0 uint8x4
    EXPECT_EQ(offsetof(MeshVertex, u),    16u);  // TexCoord0 float2
    EXPECT_EQ(offsetof(MeshVertex, v),    20u);
    EXPECT_EQ(offsetof(MeshVertex, r0),   24u);  // TexCoord1 float4 (atlas sub-rect)
    EXPECT_EQ(offsetof(MeshVertex, r1),   28u);
    EXPECT_EQ(offsetof(MeshVertex, r2),   32u);
    EXPECT_EQ(offsetof(MeshVertex, r3),   36u);
    EXPECT_EQ(sizeof(MeshVertex),         40u);  // stride
}

TEST(VertexLayout, UntexturedMeshEmitsZeroUVsAndFullAtlasRect) {
    // Until per-face tiles are bound, every emitted vertex addresses the atlas at
    // (0,0) over the full-atlas sub-rect (0,0,1,1). With the renderer's default
    // 1×1 white tile that sample returns white, so the per-face color passes
    // through unmodulated (the byte-identical path).
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    ASSERT_FALSE(verts.empty());
    for (const MeshVertex& mv : verts) {
        EXPECT_FLOAT_EQ(mv.u, 0.0f);
        EXPECT_FLOAT_EQ(mv.v, 0.0f);
        EXPECT_FLOAT_EQ(mv.r0, 0.0f);
        EXPECT_FLOAT_EQ(mv.r1, 0.0f);
        EXPECT_FLOAT_EQ(mv.r2, 1.0f);
        EXPECT_FLOAT_EQ(mv.r3, 1.0f);
    }
}
