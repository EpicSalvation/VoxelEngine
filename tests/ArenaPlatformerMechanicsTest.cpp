// M7b tests — platformer mechanics (Group 3).
//
// Tests: dirty-chunk tracking after voxel edits in the detail layer, and a full
// persistence round-trip for a player-built platform.  These run headlessly —
// no window, no plugin, no renderer — so they are pure unit tests of the engine
// subsystems that the arena platformer wires together.
//
// The "detail" layer is the 1 m terminal layer the player edits with left/right
// mouse. Dirty tracking (setVoxel → isChunkDirty) and persistence (saveChunk /
// tryLoadChunk / saveDirtyChunks) are the same M5 primitives used by demo 04;
// these tests verify they compose correctly with the multi-layer arena world.

#include "core/LayerConfig.h"
#include "io/ChunkPersistence.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/Voxel.h"
#include "world/World.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Simple generator: fills with stone at Y < 0 (mimics a floor slab), empty otherwise.
void flatStoneGen(WorldCoord origin, int n, Voxel* out, void* /*ud*/) {
    MaterialProperties stone{};
    stone.density             = 2700.0f;
    stone.structural_strength = 0.8f;
    stone.hardness            = 0.6f;
    stone.palette_index       = 1;
    const Voxel stoneVoxel{stone};
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                out[x + n * (y + n * z)] =
                    (origin.value.y < 0.0) ? stoneVoxel : Voxel::empty();
}

// Create the detail-layer LayerDef (1 m terminal, chunk_size=10) matching the
// arena config used by the 07-arena-platformer demo.
LayerDef makeDetailDef() {
    LayerDef d;
    d.name                 = "detail";
    d.voxel_size_m         = 1.0;
    d.chunk_size_voxels    = 10;
    d.mode                 = VoxelMode::terminal;
    d.view_distance_chunks = 12;
    return d;
}

// A gold marker placed by the player (palette_index=12, low density).
Voxel goldMarker() {
    MaterialProperties m{};
    m.palette_index = 12;
    m.density       = 100.0f;
    return Voxel{m};
}

// Returns a unique temporary directory path (not yet created; WorldSave creates it).
std::string tempDir(const std::string& suffix) {
    return (std::filesystem::temp_directory_path() /
            ("voxelengine_test_arena_" + suffix)).string();
}

}  // namespace

// ── Group 3: Dirty tracking after voxel edits ────────────────────────────────

TEST(ArenaPlatformerMechanics, EditDetailVoxelMarksDirty) {
    // A fresh chunk generated from a pure generator is clean; writing a voxel
    // into it via world.setVoxel marks it dirty.
    LayerDef def = makeDetailDef();
    World world(def);

    const ChunkCoord cc{0, 0, 0};
    world.loadChunk(cc, flatStoneGen, nullptr);
    ASSERT_NE(world.getChunk(cc), nullptr);
    EXPECT_FALSE(world.isChunkDirty(cc));

    // Place a gold marker inside the chunk.
    const WorldCoord pos(5.5, 0.5, 5.5);  // inside chunk [0, 10)³
    world.setVoxel(pos, goldMarker());
    EXPECT_TRUE(world.isChunkDirty(cc));
}

TEST(ArenaPlatformerMechanics, BreakDetailVoxelMarksDirty) {
    // Removing a voxel (setting it to empty) also marks the chunk dirty.
    LayerDef def = makeDetailDef();
    World world(def);

    const ChunkCoord cc{0, -1, 0};  // Y < 0 → stone floor from flatStoneGen
    world.loadChunk(cc, flatStoneGen, nullptr);
    ASSERT_NE(world.getChunk(cc), nullptr);
    EXPECT_FALSE(world.isChunkDirty(cc));

    // Break one voxel from the solid chunk.
    const WorldCoord pos(3.5, -5.5, 3.5);
    world.setVoxel(pos, Voxel::empty());
    EXPECT_TRUE(world.isChunkDirty(cc));
}

TEST(ArenaPlatformerMechanics, ClearDirtyMakesChunkClean) {
    LayerDef def = makeDetailDef();
    World world(def);

    const ChunkCoord cc{0, 0, 0};
    world.loadChunk(cc, flatStoneGen, nullptr);
    world.setVoxel(WorldCoord(1.5, 1.5, 1.5), goldMarker());
    ASSERT_TRUE(world.isChunkDirty(cc));

    world.clearChunkDirty(cc);
    EXPECT_FALSE(world.isChunkDirty(cc));
}

// ── Group 3: Persistence round-trip of a player-built platform ───────────────

TEST(ArenaPlatformerMechanics, PersistenceRoundTripPreservesPlayerEdits) {
    // Generate a detail chunk, place a voxel (the "bridge"), save to disk, reload,
    // and verify the player edit survived the evict-then-restore cycle.
    const std::string dir = tempDir("roundtrip");
    LayerDef def = makeDetailDef();
    const persistence::WorldIdentity id{def.voxel_size_m, def.chunk_size_voxels};
    persistence::WorldSave save(dir, id);

    {
        World world(def);
        const ChunkCoord cc{3, 2, 1};
        world.loadChunk(cc, flatStoneGen, nullptr);

        // Place a bridge block (gold marker) at a specific world position.
        const WorldCoord editPos(33.5, 20.5, 13.5);  // inside chunk [30,40)×[20,30)×[10,20)
        world.setVoxel(editPos, goldMarker());
        ASSERT_TRUE(world.isChunkDirty(cc));

        // Save dirty chunks (as the demo does on eviction and on quit).
        const int saved = save.saveDirtyChunks(world);
        EXPECT_EQ(saved, 1);
        EXPECT_FALSE(world.isChunkDirty(cc));   // flag cleared after save
        EXPECT_TRUE(save.hasChunk(cc));
    }

    // Simulate relaunch: create a new World, prefer saved chunk over regenerating.
    {
        World world2(def);
        const ChunkCoord cc{3, 2, 1};

        auto loaded = save.tryLoadChunk(cc);
        ASSERT_NE(loaded, nullptr);
        world2.insertChunk(std::move(loaded));

        // The bridge voxel must be exactly what the player placed.
        const WorldCoord editPos(33.5, 20.5, 13.5);
        const Voxel result = world2.getVoxel(editPos);
        EXPECT_EQ(result.material.palette_index, 12);
        EXPECT_NEAR(result.material.density, 100.0f, 0.001f);

        // The loaded chunk is clean (the edits are already on disk, not pending).
        EXPECT_FALSE(world2.isChunkDirty(cc));
    }

    std::filesystem::remove_all(dir);
}

TEST(ArenaPlatformerMechanics, CleanChunkIsNotSaved) {
    // saveDirtyChunks must skip clean (generator-produced) chunks to avoid
    // bloating the save directory with regeneratable data.
    const std::string dir = tempDir("clean_not_saved");
    LayerDef def = makeDetailDef();
    const persistence::WorldIdentity id{def.voxel_size_m, def.chunk_size_voxels};
    persistence::WorldSave save(dir, id);

    World world(def);
    world.loadChunk({0, 0, 0}, flatStoneGen, nullptr);
    world.loadChunk({1, 0, 0}, flatStoneGen, nullptr);
    EXPECT_FALSE(world.isChunkDirty({0, 0, 0}));
    EXPECT_FALSE(world.isChunkDirty({1, 0, 0}));

    const int saved = save.saveDirtyChunks(world);
    EXPECT_EQ(saved, 0);
    EXPECT_FALSE(save.hasChunk({0, 0, 0}));
    EXPECT_FALSE(save.hasChunk({1, 0, 0}));

    std::filesystem::remove_all(dir);
}

TEST(ArenaPlatformerMechanics, SaveEvictReloadRestoresEdit) {
    // Simulate the save-then-evict pattern used by the demo when detail chunks
    // drift past kDetailKeepRadiusM: save the dirty chunk, clear its dirty flag,
    // unload it, then reload via insertChunk — edit must survive.
    const std::string dir = tempDir("save_evict_reload");
    LayerDef def = makeDetailDef();
    const persistence::WorldIdentity id{def.voxel_size_m, def.chunk_size_voxels};
    persistence::WorldSave save(dir, id);

    World world(def);
    const ChunkCoord cc{0, 0, 0};
    world.loadChunk(cc, flatStoneGen, nullptr);

    const WorldCoord pos(7.5, 2.5, 1.5);
    world.setVoxel(pos, goldMarker());
    ASSERT_TRUE(world.isChunkDirty(cc));

    // Save-then-evict (mirrors the demo's detail eviction loop).
    if (const Chunk* ch = world.getChunk(cc)) save.saveChunk(*ch);
    world.clearChunkDirty(cc);
    world.unloadChunk(cc);
    EXPECT_EQ(world.getChunk(cc), nullptr);

    // Reload (mirrors the demo's decomposition integration with saved-chunk check).
    auto loaded = save.tryLoadChunk(cc);
    ASSERT_NE(loaded, nullptr);
    world.insertChunk(std::move(loaded));
    ASSERT_NE(world.getChunk(cc), nullptr);

    const Voxel result = world.getVoxel(pos);
    EXPECT_EQ(result.material.palette_index, 12);
    EXPECT_NEAR(result.material.density, 100.0f, 0.001f);

    std::filesystem::remove_all(dir);
}

TEST(ArenaPlatformerMechanics, MultipleEditsInChunkAllPersist) {
    // All voxel edits within the same chunk must survive a round-trip — not just
    // the first one (verifies the RLE codec handles multiple distinct values).
    const std::string dir = tempDir("multi_edit");
    LayerDef def = makeDetailDef();
    const persistence::WorldIdentity id{def.voxel_size_m, def.chunk_size_voxels};
    persistence::WorldSave save(dir, id);

    World world(def);
    const ChunkCoord cc{0, 0, 0};
    world.loadChunk(cc, flatStoneGen, nullptr);

    // Place three different markers at distinct positions within the same chunk.
    struct Edit { WorldCoord pos; uint8_t idx; float density; };
    const Edit edits[] = {
        { WorldCoord(1.5, 1.5, 1.5), 12, 100.0f },
        { WorldCoord(5.5, 5.5, 5.5),  9, 800.0f },
        { WorldCoord(8.5, 8.5, 8.5),  2, 1200.0f },
    };
    for (const auto& e : edits) {
        MaterialProperties m{};
        m.palette_index = e.idx;
        m.density       = e.density;
        world.setVoxel(e.pos, Voxel{m});
    }
    ASSERT_TRUE(world.isChunkDirty(cc));
    save.saveDirtyChunks(world);

    // Reload into a fresh world and verify every edit.
    World world2(def);
    auto loaded = save.tryLoadChunk(cc);
    ASSERT_NE(loaded, nullptr);
    world2.insertChunk(std::move(loaded));
    for (const auto& e : edits) {
        const Voxel v = world2.getVoxel(e.pos);
        EXPECT_EQ(v.material.palette_index, e.idx)
            << "palette mismatch for edit at " << e.pos.value.x;
        EXPECT_NEAR(v.material.density, e.density, 0.001f)
            << "density mismatch for edit at " << e.pos.value.x;
    }

    std::filesystem::remove_all(dir);
}
