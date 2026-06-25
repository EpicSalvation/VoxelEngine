// Tests for the headless chunk mesh builder (src/renderer/ChunkMeshData.{h,cpp}).
// No bgfx — verifies face culling, border behavior, and the opaque/translucent
// batch split on geometry counts.

#include "renderer/ChunkMeshData.h"
#include "renderer/MaterialFaces.h"
#include "renderer/Palette.h"
#include "world/Chunk.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

Voxel solid() {
    Voxel v;
    v.material.palette_index = 1;  // stone — opaque palette entry
    v.material.density       = 1.0f;
    return v;
}

Voxel water() {
    Voxel v;
    v.material.palette_index = 5;  // water — translucent palette entry
    v.material.density       = 1.0f;
    return v;
}

constexpr int kVertsPerFace = 6;  // two triangles
constexpr int kFacesPerCube = 6;

}  // namespace

TEST(ChunkMeshData, EmptyChunkProducesNoGeometry) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_TRUE(verts.empty());
    EXPECT_TRUE(opaque.empty());
    EXPECT_TRUE(translucent.empty());
}

TEST(ChunkMeshData, SingleVoxelHasAllSixFaces) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();  // interior so no border interaction
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_EQ(opaque.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
    EXPECT_TRUE(translucent.empty());
}

TEST(ChunkMeshData, AdjacentVoxelsCullSharedFace) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    chunk.at(2, 1, 1) = solid();  // shares the face between them
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    // Each cube loses exactly one (interior) face: (6-1) faces * 2 cubes.
    EXPECT_EQ(opaque.size(), static_cast<size_t>((kFacesPerCube - 1) * 2 * kVertsPerFace));  // 60
}

TEST(ChunkMeshData, BorderFacesAlwaysEmittedForOpaque) {
    // A single opaque voxel filling a 1³ chunk: every face is on the border and
    // must still be emitted (no cross-chunk neighbor lookup).
    Chunk chunk(ChunkCoord{0, 0, 0}, 1, WorldCoord());
    chunk.at(0, 0, 0) = solid();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_EQ(opaque.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
}

TEST(ChunkMeshData, FullySolidChunkEmitsOnlyOuterShell) {
    const int n = 3;
    Chunk chunk(ChunkCoord{0, 0, 0}, n, WorldCoord());
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                chunk.at(x, y, z) = solid();

    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    // Only the outer shell: 6 faces of n*n voxels each.
    EXPECT_EQ(opaque.size(), static_cast<size_t>(kFacesPerCube * n * n * kVertsPerFace));
}

TEST(ChunkMeshData, TranslucentVoxelGoesToTranslucentBatch) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = water();  // interior translucent voxel
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_TRUE(opaque.empty());
    EXPECT_EQ(translucent.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
}

TEST(ChunkMeshData, TranslucentFacesSkipChunkBorders) {
    // A single water voxel filling a 1³ chunk: all faces are on the border, and
    // translucent voxels do not emit border faces (water continues across seams),
    // so nothing is produced.
    Chunk chunk(ChunkCoord{0, 0, 0}, 1, WorldCoord());
    chunk.at(0, 0, 0) = water();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_TRUE(opaque.empty());
    EXPECT_TRUE(translucent.empty());
}

TEST(ChunkMeshData, WaterOnTerrainEmitsOnlyTopAndExposedSides) {
    // Terrain floor (opaque) with a water layer on top, in a 3³ chunk: the water
    // voxel at the centre column sits on terrain (its -Y face is culled) and has
    // water nowhere adjacent in-chunk, so only its non-border faces show. Verify
    // the water's down face is culled against the solid below it.
    Chunk chunk(ChunkCoord{0, 0, 0}, 3, WorldCoord());
    chunk.at(1, 0, 1) = solid();  // terrain
    chunk.at(1, 1, 1) = water();  // water directly above, interior column
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    // Water voxel: +Y neighbor is empty interior (emit), -Y is solid (cull), and
    // the four side neighbors are empty interior (emit) → 5 faces.
    EXPECT_EQ(translucent.size(), static_cast<size_t>(5 * kVertsPerFace));  // 30
}

TEST(ChunkMeshData, OpaqueFaceUnderWaterIsStillEmitted) {
    // An opaque block with water directly above keeps its top face: a translucent
    // neighbor does not occlude an opaque face, so the block shows through water
    // instead of going transparent.
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    chunk.at(1, 2, 1) = water();  // water directly above, interior column
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    // Solid keeps all six faces — its +Y toward the water is NOT culled.
    EXPECT_EQ(opaque.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
    // Water still culls its -Y against the solid below; other five faces emit.
    EXPECT_EQ(translucent.size(), static_cast<size_t>(5 * kVertsPerFace));  // 30
}

TEST(ChunkMeshData, OpaqueNeighborStillCullsOpaqueFace) {
    // Regression: opaque-opaque shared faces are still culled (only translucent
    // neighbors are treated as non-occluding).
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    chunk.at(1, 2, 1) = solid();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);
    EXPECT_EQ(opaque.size(), static_cast<size_t>((kFacesPerCube - 1) * 2 * kVertsPerFace));  // 60
}

// ── Ambient occlusion (M17, sanity-check A2) ────────────────────────────────
//
// AO is baked into the per-vertex color: a face corner is darkened by the opaque
// voxels in the 2x2 block around it in the air layer just outside the face. The
// red channel of the (uniform grey stone) color is a direct proxy for brightness.

namespace {

// Red channel of a vertex color (the AO multiplier scales all RGB equally, so one
// channel is enough to compare brightness). Stone is 0xffaaaaaa → R = 0xaa = 170.
uint32_t redOf(const MeshVertex& mv) { return mv.abgr & 0xffu; }

constexpr uint32_t kStoneR = 0xaa;  // full-bright top face (shade 1.0, AO factor 1.0)

}  // namespace

TEST(ChunkMeshData, AmbientOcclusionOpenTopFaceIsFullBright) {
    // A lone voxel has no neighbors anywhere, so every top-face corner is a fully
    // open AO level 3 (factor 1.0) and the color is unchanged from the pre-AO mesh.
    palette::resetToDefault();
    materialfaces::clearBindings();
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(2, 2, 2) = solid();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    const int base = materialfaces::Face::PosY * kVertsPerFace;  // top face block
    for (int i = 0; i < kVertsPerFace; ++i)
        EXPECT_EQ(redOf(verts[base + i]), kStoneR) << "vertex " << i;
}

TEST(ChunkMeshData, AmbientOcclusionFlatGroundIsUniform) {
    // Two coplanar voxels side by side: a coplanar neighbor sits in the SAME layer,
    // not in the air layer above, so it must not occlude the top face. The first
    // voxel's +Y block stays full-bright (no false self-AO seam between flat tiles).
    palette::resetToDefault();
    materialfaces::clearBindings();
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    chunk.at(2, 1, 1) = solid();  // coplanar neighbor to +X (shared face culled)
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    // (1,1,1) is processed first and only loses its +X face (index 5, after +Y),
    // so its +Y block is still at PosY*6.
    const int base = materialfaces::Face::PosY * kVertsPerFace;
    for (int i = 0; i < kVertsPerFace; ++i)
        EXPECT_EQ(redOf(verts[base + i]), kStoneR) << "vertex " << i;
}

TEST(ChunkMeshData, AmbientOcclusionRaisedNeighborDarkensSharedCorner) {
    // A voxel whose top face has a taller block diagonally beside it: the two top
    // corners on that side sit in a concave nook and must be darkened, while the two
    // corners on the open side stay full-bright.
    palette::resetToDefault();
    materialfaces::clearBindings();
    Chunk chunk(ChunkCoord{0, 0, 0}, 6, WorldCoord());
    chunk.at(2, 2, 2) = solid();          // the voxel under test (processed first)
    chunk.at(3, 3, 2) = solid();          // rises beside its top face, +X side

    std::vector<MeshVertex> verts;
    std::vector<uint32_t> opaque, translucent;
    buildChunkMeshData(chunk, verts, opaque, translucent);

    // The first 6 faces belong to voxel (2,2,2) (z,y,x iteration reaches it before
    // the raised neighbor), so its +Y face is the usual block.
    const int base = materialfaces::Face::PosY * kVertsPerFace;
    bool sawDark = false, sawBright = false;
    for (int i = 0; i < kVertsPerFace; ++i) {
        const MeshVertex& mv = verts[base + i];
        if (mv.x == 3.0f) {            // +X edge: beside the raised neighbor
            EXPECT_LT(redOf(mv), kStoneR) << "occluded corner should darken";
            sawDark = true;
        } else if (mv.x == 2.0f) {     // open edge
            EXPECT_EQ(redOf(mv), kStoneR) << "open corner stays full-bright";
            sawBright = true;
        }
    }
    EXPECT_TRUE(sawDark);
    EXPECT_TRUE(sawBright);
}
