// Tests for the M15 T5 scale-agnostic tiling property.
//
// The mesh builder emits a TILE-LOCAL UV scaled by face_world_size × tiling_factor
// (face_world_size == the voxel's edge length in meters). So the same material
// tiles at a fixed world density independent of voxel size: a 1 m terminal voxel
// shows one copy, an N m composite face shows N copies — not one stretched copy.
// The fragment shader's frac() turns that span into the actual repeat; here we
// pin the emitted span the shader consumes.
//
// Headless: tile rects are installed directly into the bgfx-free materialfaces
// table (the palette analog). The emitted span is read as the max tile-local UV
// over a face-block (kFaces order, six vertices per face).

#include "renderer/ChunkMeshData.h"
#include "renderer/MaterialFaces.h"
#include "world/Chunk.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

using materialfaces::Face;
using texture::AtlasTile;

namespace {

AtlasTile unitRect() {
    AtlasTile t;
    t.u0 = 0.0f; t.v0 = 0.0f; t.u1 = 0.5f; t.v1 = 0.5f;  // arbitrary sub-rect
    t.w = 16; t.h = 16;
    return t;
}

// Build one interior voxel of `palette` at `voxelSizeM` and return the max
// tile-local UV component over the top (+Y) face — the emitted repeat span.
float topFaceSpan(uint8_t palette, double voxelSizeM) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    Voxel v;
    v.material.palette_index = palette;
    v.material.density = 1.0f;
    chunk.at(1, 1, 1) = v;

    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent, voxelSizeM);

    float m = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const MeshVertex& mv = verts[static_cast<size_t>(Face::PosY) * 6 + i];
        m = std::max(m, std::max(mv.u, mv.v));
    }
    return m;
}

void bindTiled(uint8_t palette, float tiling) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    materialfaces::setTileRect("tile", unitRect());
    materialfaces::setMaterialFaces(palette, "tile", "tile", "tile", tiling);
}

}  // namespace

TEST(TextureTiling, SpanScalesByFaceWorldSizeTimesTilingFactor) {
    bindTiled(/*palette*/ 9, /*tiling*/ 2.0f);

    // face_world_size × tiling_factor copies: 1 m → 2, 4 m → 8.
    EXPECT_FLOAT_EQ(topFaceSpan(9, /*voxelSizeM*/ 1.0), 2.0f);
    EXPECT_FLOAT_EQ(topFaceSpan(9, /*voxelSizeM*/ 4.0), 8.0f);
}

TEST(TextureTiling, SameMaterialTilesIdenticallyAcrossScales) {
    // The scale-agnostic property: one authored texture at a fixed tiling density
    // yields the same per-meter copy count on a small terminal voxel and a large
    // composite face. A 1 m voxel and an 8 m voxel differ by exactly the 8× span.
    bindTiled(/*palette*/ 9, /*tiling*/ 1.0f);
    const float oneMeter   = topFaceSpan(9, 1.0);
    const float eightMeter = topFaceSpan(9, 8.0);
    EXPECT_FLOAT_EQ(oneMeter, 1.0f);
    EXPECT_FLOAT_EQ(eightMeter, 8.0f);
    EXPECT_FLOAT_EQ(eightMeter, 8.0f * oneMeter);
}

TEST(TextureTiling, FactorOneOnOneMeterFaceReproducesUntexturedSpan) {
    // tiling 1 on a 1 m face → span [0,1], the same single-copy span an untextured
    // face would imply (the regression anchor for the scale-agnostic default).
    bindTiled(/*palette*/ 9, /*tiling*/ 1.0f);
    EXPECT_FLOAT_EQ(topFaceSpan(9, 1.0), 1.0f);
}
