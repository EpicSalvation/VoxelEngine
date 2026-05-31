// Tests for the internal chunk save format and WorldSave directory
// (src/io/ChunkPersistence.*): palette+RLE codec round-trip, identity/format
// rejection, malformed-data safety, and on-disk save/load of dirty chunks (M5).

#include "io/ChunkPersistence.h"
#include "world/World.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

#include <filesystem>

using persistence::WorldIdentity;
using persistence::encodeChunkFile;
using persistence::decodeChunkFile;
using persistence::WorldSave;

namespace {

Voxel mat(uint8_t palette, float density, float hardness = 0.0f) {
    Voxel v;
    v.material.density       = density;
    v.material.hardness      = hardness;
    v.material.palette_index = palette;
    return v;
}

bool sameVoxel(const Voxel& a, const Voxel& b) {
    return a.material.density == b.material.density &&
           a.material.structural_strength == b.material.structural_strength &&
           a.material.thermal_conductivity == b.material.thermal_conductivity &&
           a.material.porosity == b.material.porosity &&
           a.material.hardness == b.material.hardness &&
           a.material.palette_index == b.material.palette_index;
}

bool sameGrid(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    const int n = a.size();
    const size_t count = static_cast<size_t>(n) * n * n;
    for (size_t i = 0; i < count; ++i)
        if (!sameVoxel(a.data()[i], b.data()[i])) return false;
    return true;
}

// A chunk with a mix of empty space and a few distinct materials (so the palette
// has several entries and runs of varying length).
Chunk makeMixedChunk(ChunkCoord coord, int size, double voxelSize) {
    Chunk c(coord, size, chunkmath::chunkOrigin(coord, voxelSize, size));
    for (int z = 0; z < size; ++z)
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                if (y < size / 2)           c.at(x, y, z) = mat(2, 1500.0f, 4.0f);  // stone
                else if (y == size / 2)     c.at(x, y, z) = mat(3, 600.0f, 1.0f);   // grass
                // else stays empty
                if (x == 0 && z == 0)       c.at(x, y, z) = mat(5, 999.0f, 2.5f);   // a vein
            }
    return c;
}

WorldIdentity id1() { return WorldIdentity{1.0, 8}; }

}  // namespace

TEST(ChunkCodec, RoundTripsAMixedChunk) {
    Chunk original = makeMixedChunk(ChunkCoord{1, -2, 3}, 8, 1.0);
    auto bytes = encodeChunkFile(original, id1());
    auto back  = decodeChunkFile(bytes.data(), bytes.size(), id1());

    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->coord(), original.coord());
    EXPECT_EQ(back->size(), original.size());
    EXPECT_EQ(back->origin().value, original.origin().value);
    EXPECT_TRUE(sameGrid(*back, original));
}

TEST(ChunkCodec, RoundTripsAnAllEmptyChunk) {
    Chunk original(ChunkCoord{0, 0, 0}, 8, chunkmath::chunkOrigin(ChunkCoord{0, 0, 0}, 1.0, 8));
    auto bytes = encodeChunkFile(original, id1());
    // A uniform chunk should compress to a single run (tiny file).
    auto back = decodeChunkFile(bytes.data(), bytes.size(), id1());
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(sameGrid(*back, original));
}

TEST(ChunkCodec, RejectsIdentityMismatch) {
    Chunk c = makeMixedChunk(ChunkCoord{0, 0, 0}, 8, 1.0);
    auto bytes = encodeChunkFile(c, id1());

    EXPECT_EQ(decodeChunkFile(bytes.data(), bytes.size(), WorldIdentity{2.0, 8}), nullptr);  // voxel size
    EXPECT_EQ(decodeChunkFile(bytes.data(), bytes.size(), WorldIdentity{1.0, 16}), nullptr); // chunk size
}

TEST(ChunkCodec, RejectsGarbageAndTruncation) {
    EXPECT_EQ(decodeChunkFile(nullptr, 0, id1()), nullptr);
    const uint8_t junk[5] = {'X', 'X', 'X', 'X', 1};
    EXPECT_EQ(decodeChunkFile(junk, sizeof(junk), id1()), nullptr);

    Chunk c = makeMixedChunk(ChunkCoord{0, 0, 0}, 8, 1.0);
    auto bytes = encodeChunkFile(c, id1());
    // Truncate to half — decode must fail cleanly, not crash or over-read.
    EXPECT_EQ(decodeChunkFile(bytes.data(), bytes.size() / 2, id1()), nullptr);
}

// ── On-disk WorldSave ────────────────────────────────────────────────────────

class WorldSaveTest : public ::testing::Test {
protected:
    std::filesystem::path dir;
    void SetUp() override {
        dir = std::filesystem::temp_directory_path() / "voxel_persistence_test";
        std::filesystem::remove_all(dir);
    }
    void TearDown() override { std::filesystem::remove_all(dir); }

    static LayerDef layer() {
        LayerDef d; d.name = "terrain"; d.voxel_size_m = 1.0;
        d.mode = VoxelMode::terminal; d.chunk_size_voxels = 8;
        return d;
    }
};

TEST_F(WorldSaveTest, SaveThenLoadIsByteForByteEqual) {
    WorldSave save(dir.string(), id1());

    World world(layer());
    world.loadChunk(ChunkCoord{2, 0, -1}, nullptr);
    world.setVoxel(WorldCoord(16.5, 1.5, -7.5), mat(2, 1500.0f, 4.0f));  // edit -> dirty
    world.setVoxel(WorldCoord(17.5, 2.5, -7.5), mat(5, 999.0f, 2.5f));

    ASSERT_TRUE(world.isChunkDirty(ChunkCoord{2, 0, -1}));
    EXPECT_EQ(save.saveDirtyChunks(world), 1);
    EXPECT_FALSE(world.isChunkDirty(ChunkCoord{2, 0, -1}));  // cleared after save
    EXPECT_TRUE(save.hasChunk(ChunkCoord{2, 0, -1}));

    // Reload into a fresh world and compare the grid.
    auto loaded = save.tryLoadChunk(ChunkCoord{2, 0, -1});
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(sameGrid(*loaded, *world.getChunk(ChunkCoord{2, 0, -1})));
}

TEST_F(WorldSaveTest, MissingChunkReportsAbsentAndLoadsNull) {
    WorldSave save(dir.string(), id1());
    EXPECT_FALSE(save.hasChunk(ChunkCoord{9, 9, 9}));
    EXPECT_EQ(save.tryLoadChunk(ChunkCoord{9, 9, 9}), nullptr);
}

TEST_F(WorldSaveTest, OnlyDirtyChunksAreSaved) {
    WorldSave save(dir.string(), id1());
    World world(layer());
    world.loadChunk(ChunkCoord{0, 0, 0}, nullptr);  // clean
    world.loadChunk(ChunkCoord{1, 0, 0}, nullptr);
    world.setVoxel(WorldCoord(9.5, 0.5, 0.5), mat(2, 100.0f));  // dirties {1,0,0}

    EXPECT_EQ(save.saveDirtyChunks(world), 1);
    EXPECT_TRUE(save.hasChunk(ChunkCoord{1, 0, 0}));
    EXPECT_FALSE(save.hasChunk(ChunkCoord{0, 0, 0}));
}

TEST_F(WorldSaveTest, LoadedChunkInsertsIntoWorldAndIsClean) {
    WorldSave save(dir.string(), id1());
    World a(layer());
    a.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    a.setVoxel(WorldCoord(1.5, 1.5, 1.5), mat(7, 42.0f));
    save.saveDirtyChunks(a);

    // A fresh world prefers the saved chunk over generating.
    World b(layer());
    auto disk = save.tryLoadChunk(ChunkCoord{0, 0, 0});
    ASSERT_NE(disk, nullptr);
    Chunk* inserted = b.insertChunk(std::move(disk));
    ASSERT_NE(inserted, nullptr);
    EXPECT_FALSE(b.isChunkDirty(ChunkCoord{0, 0, 0}));  // loaded edits already on disk
    EXPECT_EQ(b.getVoxel(WorldCoord(1.5, 1.5, 1.5)).material.palette_index, 7);
}
