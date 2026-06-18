// Tests for the M15 T4/T5 (palette_index, face) → atlas tile binding and the
// mesh builder's per-face UV emission.
//
// set_material_faces binds tiles per face (top / bottom / side); the mesh builder
// looks the binding up per kFaces direction and emits the bound tile's atlas
// sub-rect plus a tile-local UV. An unbound material falls back to the white tile
// (full-atlas rect, UV 0). And the binding keys on palette_index alone, so
// Voxel / MaterialProperties stay trivially-copyable and memcmp-stable — the
// determinism padding is untouched.
//
// Headless: materialfaces is a bgfx-free global table (the palette analog), so the
// tile rects are installed directly with setTileRect rather than through a GPU
// atlas. Faces are emitted in kFaces order (+Z,-Z,-Y,+Y,-X,+X), six vertices each,
// so a single interior voxel yields 36 vertices in six face-blocks.

#include "renderer/ChunkMeshData.h"
#include "renderer/MaterialFaces.h"
#include "world/Chunk.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <cstring>
#include <type_traits>
#include <vector>

using materialfaces::Face;
using texture::AtlasTile;

namespace {

// Distinct, recognizable atlas sub-rects so a face's emitted rect identifies the
// tile it was bound to.
AtlasTile rect(float u0, float v0, float u1, float v1) {
    AtlasTile t;
    t.u0 = u0; t.v0 = v0; t.u1 = u1; t.v1 = v1;
    t.w = 16; t.h = 16;
    return t;
}

Voxel voxelOf(uint8_t palette_index) {
    Voxel v;
    v.material.palette_index = palette_index;  // opaque by default palette
    v.material.density = 1.0f;
    return v;
}

// The rect carried by the first vertex of face-block `faceIdx` (kFaces order).
void faceRect(const std::vector<MeshVertex>& verts, int faceIdx,
              float& u0, float& v0, float& u1, float& v1) {
    const MeshVertex& mv = verts[static_cast<size_t>(faceIdx) * 6];
    u0 = mv.r0; v0 = mv.r1; u1 = mv.r2; v1 = mv.r3;
}

// Largest tile-local UV component over a face-block (its repeat span).
float faceMaxUV(const std::vector<MeshVertex>& verts, int faceIdx) {
    float m = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const MeshVertex& mv = verts[static_cast<size_t>(faceIdx) * 6 + i];
        m = std::max(m, std::max(mv.u, mv.v));
    }
    return m;
}

std::vector<MeshVertex> buildSingleVoxel(uint8_t palette_index, double voxelSizeM = 1.0) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = voxelOf(palette_index);
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent, voxelSizeM);
    return verts;
}

}  // namespace

TEST(MaterialFaceTable, BindsTilesPerFaceAndEmitsTheirRects) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();

    const AtlasTile top  = rect(0.00f, 0.00f, 0.25f, 0.25f);
    const AtlasTile bot  = rect(0.25f, 0.00f, 0.50f, 0.25f);
    const AtlasTile side = rect(0.50f, 0.00f, 0.75f, 0.25f);
    materialfaces::setTileRect("grass_top",  top);
    materialfaces::setTileRect("dirt",       bot);
    materialfaces::setTileRect("grass_side", side);

    materialfaces::setMaterialFaces(/*palette*/ 7, "grass_top", "dirt", "grass_side",
                                    /*tiling*/ 1.0f);

    std::vector<MeshVertex> verts = buildSingleVoxel(7);
    ASSERT_EQ(verts.size(), 36u);  // six faces of one interior voxel

    // Each kFaces direction carries the rect of the tile bound to it.
    struct Expect { int face; const AtlasTile* tile; };
    const Expect expects[] = {
        {Face::PosZ, &side}, {Face::NegZ, &side},  // lateral
        {Face::NegY, &bot},  {Face::PosY, &top},   // bottom / top
        {Face::NegX, &side}, {Face::PosX, &side},  // lateral
    };
    for (const Expect& e : expects) {
        float u0, v0, u1, v1;
        faceRect(verts, e.face, u0, v0, u1, v1);
        EXPECT_FLOAT_EQ(u0, e.tile->u0) << "face " << e.face;
        EXPECT_FLOAT_EQ(v0, e.tile->v0) << "face " << e.face;
        EXPECT_FLOAT_EQ(u1, e.tile->u1) << "face " << e.face;
        EXPECT_FLOAT_EQ(v1, e.tile->v1) << "face " << e.face;
    }

    // tiling 1 on a 1 m voxel → one tile copy: tile-local UV span is [0,1].
    EXPECT_FLOAT_EQ(faceMaxUV(verts, Face::PosY), 1.0f);
}

TEST(MaterialFaceTable, UnboundMaterialFallsBackToWhiteTile) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();

    // palette 1 has no binding at all.
    std::vector<MeshVertex> verts = buildSingleVoxel(1);
    ASSERT_EQ(verts.size(), 36u);
    for (const MeshVertex& mv : verts) {
        EXPECT_FLOAT_EQ(mv.u, 0.0f);
        EXPECT_FLOAT_EQ(mv.v, 0.0f);
        EXPECT_FLOAT_EQ(mv.r0, 0.0f);
        EXPECT_FLOAT_EQ(mv.r1, 0.0f);
        EXPECT_FLOAT_EQ(mv.r2, 1.0f);
        EXPECT_FLOAT_EQ(mv.r3, 1.0f);
    }
}

TEST(MaterialFaceTable, BoundButUnpackedTextureFallsBackToWhite) {
    materialfaces::clearBindings();
    materialfaces::clearTiles();
    // Bind faces to texture ids whose tiles are NOT in the atlas (no setTileRect):
    // the binding resolves to nothing, so every face renders the white tile. This
    // is the teardown end state — a texture's owner unloaded and the atlas rebuilt
    // without it, leaving the binding dangling.
    materialfaces::setMaterialFaces(5, "absent_top", "absent_bottom", "absent_side", 1.0f);

    std::vector<MeshVertex> verts = buildSingleVoxel(5);
    ASSERT_FALSE(verts.empty());
    for (const MeshVertex& mv : verts) {
        EXPECT_FLOAT_EQ(mv.r2, 1.0f);  // full-atlas rect ⇒ white
        EXPECT_FLOAT_EQ(mv.r3, 1.0f);
    }
}

TEST(MaterialFaceTable, VoxelStaysTriviallyCopyableAndMemcmpStable) {
    // The binding adds no field to Voxel/MaterialProperties — the POD discipline
    // and the memcmp-based determinism check survive the milestone.
    static_assert(std::is_trivially_copyable<MaterialProperties>::value,
                  "MaterialProperties must stay trivially copyable");
    static_assert(std::is_trivially_copyable<Voxel>::value,
                  "Voxel must stay trivially copyable");

    Voxel a = voxelOf(7);
    Voxel b = voxelOf(7);
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(Voxel)), 0);  // padding is zeroed, stable
}
