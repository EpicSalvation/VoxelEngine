// Tests for the headless chunk mesh builder (src/renderer/ChunkMeshData.{h,cpp}).
// No bgfx — verifies face culling, border behavior, and the opaque/translucent
// batch split on geometry counts.

#include "renderer/ChunkMeshData.h"
#include "world/Chunk.h"

#include <gtest/gtest.h>

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
