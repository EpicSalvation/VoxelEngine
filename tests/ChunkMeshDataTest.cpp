// Tests for the headless chunk mesh builder (src/renderer/ChunkMeshData.{h,cpp}).
// No bgfx — verifies face culling and border behavior on geometry counts.

#include "renderer/ChunkMeshData.h"
#include "world/Chunk.h"

#include <gtest/gtest.h>

namespace {

Voxel solid() {
    Voxel v;
    v.material.palette_index = 1;
    v.material.density       = 1.0f;
    return v;
}

constexpr int kVertsPerFace = 6;  // two triangles
constexpr int kFacesPerCube = 6;

}  // namespace

TEST(ChunkMeshData, EmptyChunkProducesNoGeometry) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    buildChunkMeshData(chunk, verts, idx);
    EXPECT_TRUE(verts.empty());
    EXPECT_TRUE(idx.empty());
}

TEST(ChunkMeshData, SingleVoxelHasAllSixFaces) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();  // interior so no border interaction
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    buildChunkMeshData(chunk, verts, idx);
    EXPECT_EQ(idx.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
}

TEST(ChunkMeshData, AdjacentVoxelsCullSharedFace) {
    Chunk chunk(ChunkCoord{0, 0, 0}, 4, WorldCoord());
    chunk.at(1, 1, 1) = solid();
    chunk.at(2, 1, 1) = solid();  // shares the face between them
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    buildChunkMeshData(chunk, verts, idx);
    // Each cube loses exactly one (interior) face: (6-1) faces * 2 cubes.
    EXPECT_EQ(idx.size(), static_cast<size_t>((kFacesPerCube - 1) * 2 * kVertsPerFace));  // 60
}

TEST(ChunkMeshData, BorderFacesAlwaysEmitted) {
    // A single voxel filling a 1³ chunk: every face is on the border and must
    // still be emitted (no cross-chunk neighbor lookup).
    Chunk chunk(ChunkCoord{0, 0, 0}, 1, WorldCoord());
    chunk.at(0, 0, 0) = solid();
    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    buildChunkMeshData(chunk, verts, idx);
    EXPECT_EQ(idx.size(), static_cast<size_t>(kFacesPerCube * kVertsPerFace));  // 36
}

TEST(ChunkMeshData, FullySolidChunkEmitsOnlyOuterShell) {
    const int n = 3;
    Chunk chunk(ChunkCoord{0, 0, 0}, n, WorldCoord());
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                chunk.at(x, y, z) = solid();

    std::vector<MeshVertex> verts;
    std::vector<uint32_t> idx;
    buildChunkMeshData(chunk, verts, idx);
    // Only the outer shell: 6 faces of n*n voxels each.
    EXPECT_EQ(idx.size(), static_cast<size_t>(kFacesPerCube * n * n * kVertsPerFace));
}
