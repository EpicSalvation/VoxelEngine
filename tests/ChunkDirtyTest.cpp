// Tests for chunk-granular dirty tracking (src/world/Chunk.h, World.cpp).
// A chunk is clean when generated and becomes dirty only on a post-generation
// world-space edit; the World exposes which chunks are dirty so edits can be
// persisted and not silently evicted (M5).

#include "world/World.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

Voxel solid(uint8_t palette = 7) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = palette;
    return v;
}

LayerDef terminalLayer(double voxelSize = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name              = "terrain";
    d.voxel_size_m      = voxelSize;
    d.mode              = VoxelMode::terminal;
    d.chunk_size_voxels = chunkSize;
    return d;
}

// A layer generator that fills the whole chunk with solid voxels via data().
void fillSolid(WorldCoord, int grid, Voxel* out, void*) {
    const int count = grid * grid * grid;
    for (int i = 0; i < count; ++i) out[i] = solid(2);
}

bool contains(const std::vector<ChunkCoord>& v, ChunkCoord c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

}  // namespace

TEST(ChunkDirty, NewlyLoadedChunkIsClean) {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    EXPECT_FALSE(w.isChunkDirty(ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(w.dirtyChunkCoords().empty());
}

TEST(ChunkDirty, GeneratedChunkStaysClean) {
    // Generation writes through data(), which must not mark the chunk dirty —
    // only player/simulation edits do.
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, &fillSolid);
    EXPECT_FALSE(w.isChunkDirty(ChunkCoord{0, 0, 0}));
}

TEST(ChunkDirty, EditMarksOwningChunkDirty) {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);

    ASSERT_TRUE(w.setVoxel(WorldCoord(3.5, 4.5, 5.5), solid()));
    EXPECT_TRUE(w.isChunkDirty(ChunkCoord{0, 0, 0}));

    auto dirty = w.dirtyChunkCoords();
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty.front(), (ChunkCoord{0, 0, 0}));
}

TEST(ChunkDirty, EditOnlyDirtiesTheTouchedChunk) {
    World w(terminalLayer(1.0, 32));
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    w.loadChunk(ChunkCoord{1, 0, 0}, nullptr);

    w.setVoxel(WorldCoord(40.5, 0.5, 0.5), solid());  // x=40 -> chunk {1,0,0}

    EXPECT_FALSE(w.isChunkDirty(ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(w.isChunkDirty(ChunkCoord{1, 0, 0}));
    auto dirty = w.dirtyChunkCoords();
    EXPECT_FALSE(contains(dirty, ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(contains(dirty, ChunkCoord{1, 0, 0}));
}

TEST(ChunkDirty, WriteToUnresidentChunkDirtiesNothing) {
    World w(terminalLayer());
    EXPECT_FALSE(w.setVoxel(WorldCoord(5.0, 5.0, 5.0), solid()));
    EXPECT_TRUE(w.dirtyChunkCoords().empty());
}

TEST(ChunkDirty, ClearChunkDirtyResetsTheFlag) {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    w.setVoxel(WorldCoord(1.5, 1.5, 1.5), solid());
    ASSERT_TRUE(w.isChunkDirty(ChunkCoord{0, 0, 0}));

    w.clearChunkDirty(ChunkCoord{0, 0, 0});
    EXPECT_FALSE(w.isChunkDirty(ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(w.dirtyChunkCoords().empty());
}

TEST(ChunkDirty, RemovingAVoxelAlsoMarksDirty) {
    // Both placing and breaking are edits; an erase-to-empty must dirty too.
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, &fillSolid);
    ASSERT_FALSE(w.isChunkDirty(ChunkCoord{0, 0, 0}));

    ASSERT_TRUE(w.setVoxel(WorldCoord(2.5, 2.5, 2.5), Voxel::empty()));
    EXPECT_TRUE(w.isChunkDirty(ChunkCoord{0, 0, 0}));
}
