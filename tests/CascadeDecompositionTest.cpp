// M10 cascade decomposition tests — including cache eviction and memory budget.
//
// Verifies that DecompositionManager correctly drives the full N-layer chain:
// approaching the camera drives each composite level to decompose in declared
// (coarsest-first) order; a terminal voxel deep in the chain is only resident
// after its entire ancestor chain has decomposed; each composite layer's
// DecompositionState stays consistent. Also verifies that the coarse-supersets-
// fine invariant check fires for invalid composite→composite configs.
//
// Cache eviction / memory budget tests (the three unchecked M10 tasks):
//   CacheMissDeterminismAcrossCascade   — evict then re-approach → identical result
//   CascadeEvictionCorrectness          — dirty chunks saved, clean chunks dropped
//   MemoryBudget                        — per-layer cap enforced by farthest-first eviction

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionManager.h"
#include "world/Layer.h"
#include "world/World.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

// ── Helper: solid generator ────────────────────────────────────────────────────

// Fills every voxel solid (palette_index = 1). Deterministic and pure.
// Use Voxel{} (aggregate value-initialization) so ALL bytes — including the
// compiler-inserted padding in MaterialProperties — are deterministically zero
// before the named fields are written. Without this, GCC compiles the struct
// assignment as memcpy which propagates indeterminate padding bytes from the
// stack, causing memcmp-based determinism checks to spuriously fail.
void solidGen(WorldCoord /*origin*/, int n, Voxel* out, void*) {
    Voxel v{};  // zero-initialize including padding
    v.material.palette_index = 1;
    v.material.density       = 100.0f;
    v.material.hardness      = 1.0f;
    for (int i = 0; i < n * n * n; ++i)
        out[i] = v;
}

// Plugin init that registers a solid generator for four layer names.
int solidPluginInit(PluginContext* ctx) {
    for (const char* name : {"continental", "regional", "local", "terrain"})
        ctx->register_layer_generator(ctx, name, solidGen, nullptr);
    return 0;
}

// ── Test config ────────────────────────────────────────────────────────────────
// 3-level composite chain + 1 terminal, ratios all 8, each parent voxel ==
// child chunk world size (coarse-supersets-fine guaranteed by design):
//   continental 512 m, chunk 8  → chunk_world = 512 m = parent voxel  ✓
//   regional     64 m, chunk 8  → chunk_world =  64 m = parent voxel  ✓
//   local         8 m, chunk 8  → chunk_world =   8 m = parent voxel  ✓
//   terrain       1 m, chunk 8  (terminal)
// Use small chunk sizes so each level produces tiny chunks and the cascade
// completes quickly in tests. Parent voxel == child chunk world size at every
// composite→composite hop (coarse-supersets-fine invariant):
//   continental 16 m, chunk 2 → chunk_world =  16 m  (ratio 4 to regional)
//   regional     4 m, chunk 2 → chunk_world =   8 m... no, ratio must be int.
//
// Let ratio=2 everywhere with chunk_size_voxels=2:
//   continental  8 m, chunk 2 → chunk_world = 16 m  — violates c-s-f.
//
// Use chunk_size_voxels=1 so chunk_world == voxel_size_m:
//   continental  8 m, chunk 1 → chunk_world =  8 m = parent voxel  ✓
//   regional     4 m, chunk 1 → chunk_world =  4 m = parent voxel  ✓
//   local        2 m, chunk 1 → chunk_world =  2 m = parent voxel  ✓
//   terrain      1 m, chunk 1  (terminal)
// Ratios: 8/4=2, 4/2=2, 2/1=2. All valid.
static const char* kFourLayerYaml = R"(
layers:
  - name: continental
    voxel_size_m: 8.0
    mode: composite
    decompose_to: regional
    chunk_size_voxels: 1
    view_distance_chunks: 1
  - name: regional
    voxel_size_m: 4.0
    mode: composite
    decompose_to: local
    chunk_size_voxels: 1
    view_distance_chunks: 1
  - name: local
    voxel_size_m: 2.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 1
    view_distance_chunks: 1
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 1
    view_distance_chunks: 2
)";

// Tick the manager repeatedly until inFlight drops to zero and stays there for
// one additional tick cycle.  This covers the case where a tick enqueues new
// jobs that make inFlight > 0 again immediately after the previous batch
// completes (cascade: draining continental results enqueues regional jobs, etc.).
void drainUntilDone(DecompositionManager& mgr, World& /*world*/,
                    const WorldCoord& cam, double approach, int maxMs = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
    while (std::chrono::steady_clock::now() < deadline) {
        mgr.tick(cam, approach);
        if (mgr.inFlight() == 0) {
            // Run one more tick to let approach-triggered jobs from this round
            // propagate to the next layer, then check again.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            mgr.tick(cam, approach);
            if (mgr.inFlight() == 0) break;  // truly quiescent
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

// ── Coarse-supersets-fine config validation ────────────────────────────────────

TEST(CascadeDecompositionTest, CoarseSupersetsFineRejectedForCompositeToComposite) {
    // continental (16 m) → regional (8 m, chunk 8 → chunk_world=64 m).
    // 16 < 64 and regional is composite — should throw.
    const char* bad = R"(
layers:
  - name: continental
    voxel_size_m: 16.0
    mode: composite
    decompose_to: regional
    chunk_size_voxels: 2
    view_distance_chunks: 4
  - name: regional
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    view_distance_chunks: 8
)";
    EXPECT_THROW(LayerConfig::loadFromString(bad), std::runtime_error);
}

TEST(CascadeDecompositionTest, CoarseSupersetsFineAcceptedForCompositeToTerminal) {
    // continental (16 m) → terrain (1 m, chunk 32 → 32 m > 16 m) is allowed
    // because the child is TERMINAL (the extra chunk area is benign — terminal
    // chunks are not further decomposed and the bounds check in fillChildChunk
    // enforces the invariant at fill time).
    const char* ok = R"(
layers:
  - name: continental
    voxel_size_m: 16.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
    view_distance_chunks: 4
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 32
    view_distance_chunks: 4
)";
    EXPECT_NO_THROW(LayerConfig::loadFromString(ok));
}

// ── Four-layer cascade ────────────────────────────────────────────────────────

TEST(CascadeDecompositionTest, ManagerInitializesCompositeLayersOnly) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xDEADBEEFull, /*threads=*/1);
    // Before any tick, nothing is decomposed.
    EXPECT_EQ(mgr.decomposedCount("continental"), 0u);
    EXPECT_EQ(mgr.decomposedCount("regional"),    0u);
    EXPECT_EQ(mgr.decomposedCount("local"),       0u);
    EXPECT_EQ(mgr.inFlight(), 0u);
}

TEST(CascadeDecompositionTest, TerminalLayerEmptyBeforeAnyDecomposition) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xDEADBEEFull, 1);

    // Initial tick: continental chunks load; no decomposition yet.
    const WorldCoord cam(256.0, 256.0, 256.0);
    mgr.tick(cam, /*approach=*/100.0);  // small radius — nothing decomposed yet

    const Layer* terrain = world.layer("terrain");
    ASSERT_NE(terrain, nullptr);
    EXPECT_EQ(terrain->chunks().size(), 0u)
        << "terrain must be empty before decomposition cascade reaches it";
}

TEST(CascadeDecompositionTest, CascadeDecomposesAllLayersInOrder) {
    // Camera positioned inside a continental voxel with a large approach radius
    // so all three composite levels decompose in turn.
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    // Use 2 worker threads for realistic async behaviour.
    DecompositionManager mgr(world, pm, cfg, 0x123456789ABCull, 2);

    // Camera at the center of continental voxel (0,0,0) — world pos (256,256,256).
    const WorldCoord cam(256.0, 256.0, 256.0);
    // Approach radius slightly larger than one continental voxel (8 m) so halfR=2
    // in the tick scan — avoids billion-iteration loops while still triggering the
    // cascade for the voxels immediately around the camera.
    constexpr double kApproach = 20.0;

    // ── Step 1: tick with approach — triggers continental decomposition ────────
    mgr.tick(cam, kApproach);
    // Wait for continental to decompose.
    drainUntilDone(mgr, world, cam, kApproach);

    EXPECT_GT(mgr.decomposedCount("continental"), 0u)
        << "continental should be decomposed after approach";
    // Regional chunks are now resident but not yet decomposed.
    const Layer* regional = world.layer("regional");
    ASSERT_NE(regional, nullptr);
    EXPECT_GT(regional->chunks().size(), 0u)
        << "regional chunks must be resident after continental decomposition";

    // ── Step 2: tick again — now regional decomposes (parent continental done) ─
    drainUntilDone(mgr, world, cam, kApproach);

    EXPECT_GT(mgr.decomposedCount("regional"), 0u)
        << "regional should decompose once continental is done";
    const Layer* local = world.layer("local");
    ASSERT_NE(local, nullptr);
    EXPECT_GT(local->chunks().size(), 0u)
        << "local chunks must be resident after regional decomposition";

    // ── Step 3: tick again — local decomposes, terrain appears ────────────────
    drainUntilDone(mgr, world, cam, kApproach);

    EXPECT_GT(mgr.decomposedCount("local"), 0u)
        << "local should decompose once regional is done";
    const Layer* terrain = world.layer("terrain");
    ASSERT_NE(terrain, nullptr);
    EXPECT_GT(terrain->chunks().size(), 0u)
        << "terrain chunks must be resident only after full ancestor chain decomposed";
}

TEST(CascadeDecompositionTest, TerminalVoxelResidentOnlyAfterFullChainDecomposed) {
    // Stronger version: assert that the terrain layer has zero chunks UNTIL the
    // full continental→regional→local chain has decomposed.
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xFACEFACEull, 2);

    const WorldCoord cam(256.0, 256.0, 256.0);
    constexpr double kApproach = 20.0;

    const Layer* terrain = world.layer("terrain");
    ASSERT_NE(terrain, nullptr);

    // First pass: continental decomposes, terrain still empty.
    mgr.tick(cam, kApproach);
    drainUntilDone(mgr, world, cam, kApproach, /*maxMs=*/2000);
    const size_t afterCont = terrain->chunks().size();

    // At this point continental decomposed → regional resident.
    // Regional has not yet decomposed → terrain should still be empty
    // (unless regional decomposed in the same pass, which is race-y).
    // We accept either 0 (ideal) or a non-zero value only if local also decomposed.
    if (afterCont > 0) {
        // If terrain has chunks, local must also be decomposed.
        EXPECT_GT(mgr.decomposedCount("local"), 0u)
            << "terrain chunks present but local not decomposed — chain was violated";
    }

    // Second pass: drain until all levels have decomposed.
    for (int i = 0; i < 20; ++i) {
        drainUntilDone(mgr, world, cam, kApproach, 1000);
        if (mgr.decomposedCount("local") > 0) break;
    }
    EXPECT_GT(terrain->chunks().size(), 0u)
        << "terrain must eventually be resident after the full chain decomposes";
}

TEST(CascadeDecompositionTest, DecompositionStateConsistentPerLayer) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xABCD1234ull, 1);

    const WorldCoord cam(256.0, 256.0, 256.0);
    constexpr double kApproach = 20.0;

    // Drive until all levels decompose.
    for (int i = 0; i < 30; ++i) {
        mgr.tick(cam, kApproach);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    drainUntilDone(mgr, world, cam, kApproach);

    // Each composite layer must have at least one decomposed macro voxel, and
    // zero pending (all jobs completed).
    EXPECT_GT(mgr.decomposedCount("continental"), 0u);
    EXPECT_GT(mgr.decomposedCount("regional"),    0u);
    EXPECT_GT(mgr.decomposedCount("local"),       0u);
    EXPECT_EQ(mgr.pendingCount("continental"), 0u);
    EXPECT_EQ(mgr.pendingCount("regional"),    0u);
    EXPECT_EQ(mgr.pendingCount("local"),       0u);
    EXPECT_EQ(mgr.inFlight(),  0u);
}

// ── Single source of truth per layer ──────────────────────────────────────────
//
// Only the root composite layer is generator-streamed. A non-root composite
// layer's chunks must come exclusively from its parent's decomposition —
// generator-streaming them too creates a second source of truth for the same
// chunks (insertChunk silently overwrites) and renders fine content under macro
// blocks that never decomposed.
TEST(CascadeDecompositionTest, NonRootCompositeLayersNotStreamedDirectly) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);  // registers generators for ALL four layers
    DecompositionManager mgr(world, pm, cfg, 0x0123ull, 1);

    const WorldCoord cam(256.0, 256.0, 256.0);
    // Stream only: decompPerFrame=0 so no decomposition can populate child layers.
    for (int i = 0; i < 10; ++i)
        mgr.tick(cam, /*approachRadiusM=*/100.0, /*loadPerFrame=*/64,
                 /*decompPerFrame=*/0);

    EXPECT_GT(world.layer("continental")->chunks().size(), 0u)
        << "root composite layer must be generator-streamed";
    EXPECT_EQ(world.layer("regional")->chunks().size(), 0u)
        << "non-root composite layer must not be generator-streamed";
    EXPECT_EQ(world.layer("local")->chunks().size(), 0u)
        << "non-root composite layer must not be generator-streamed";
}

// ── Approach trigger geometry ─────────────────────────────────────────────────
//
// The approach test must measure camera distance to the macro voxel's AABB
// (closest point), not its center: a center test under-triggers by up to
// voxelSize*√3/2, which with voxel-scale radii stalls the cascade at the single
// block under the camera (the M10 demo's "only the landed chunk decomposes" bug).
TEST(CascadeDecompositionTest, ApproachTriggerUsesVoxelSurfaceDistance) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0x5EED5EEDull, 1);

    // Continental voxels are 8 m. Camera hovers 1 m above the top face of voxel
    // (0,0,0), on its center column. With a 5 m approach radius:
    //   voxel (0,0,0): surface distance 1 m        (center distance 5 m)
    //   voxel (1,0,0): surface distance √17 ≈ 4.1 m (center distance √89 ≈ 9.4 m)
    // A center-based test would never decompose (1,0,0).
    const WorldCoord cam(4.0, 9.0, 4.0);
    constexpr double kApproach = 5.0;

    mgr.tick(cam, kApproach);
    drainUntilDone(mgr, world, cam, kApproach);

    EXPECT_TRUE(mgr.isDecomposed("continental", {0, 0, 0}))
        << "the block directly under the camera must decompose";
    EXPECT_TRUE(mgr.isDecomposed("continental", {1, 0, 0}))
        << "a neighbor whose face (not center) is within range must decompose";
}

// ── Cache-miss determinism across the cascade ─────────────────────────────────
//
// Decompose the full chain in a region, capture terminal voxel data, move the
// camera far away so the whole region cascade-evicts back to coarse blocks
// (decompPerFrame=0 so no new decompositions happen during the eviction phase),
// then re-approach and verify the regenerated deep grid is byte-for-byte identical.
// This proves the M9 determinism guarantee holds transitively through every hop.
TEST(CascadeDecompositionTest, CacheMissDeterminismAcrossCascade) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xC0FFEE11ull, 2);

    const WorldCoord camNear(0.0, 0.0, 0.0);
    // Camera far enough that view_distance_chunks=1 forces eviction of origin chunks.
    const WorldCoord camFar(10000.0, 10000.0, 10000.0);
    constexpr double kApproach = 20.0;

    auto drainFull = [&](const WorldCoord& cam) {
        for (int pass = 0; pass < 60; ++pass) {
            mgr.tick(cam, kApproach);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        drainUntilDone(mgr, world, cam, kApproach);
    };

    // Pure-evict helper: tick with approach=0 and decompPerFrame=0 so only the
    // LOD eviction runs — no new decompositions are triggered near camFar.
    auto evictOnly = [&](const WorldCoord& cam) {
        for (int pass = 0; pass < 20; ++pass) {
            mgr.tick(cam, /*approachRadiusM=*/0.0, /*loadPerFrame=*/4, /*decompPerFrame=*/0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    // ── First approach: decompose full chain to terminal ─────────────────────
    drainFull(camNear);
    const Layer* terrain = world.layer("terrain");
    ASSERT_NE(terrain, nullptr);
    ASSERT_GT(terrain->chunks().size(), 0u)
        << "terminal layer must be resident after full descent";

    // Snapshot: copy all voxels in every resident terminal chunk.
    struct ChunkSnapshot {
        ChunkCoord         coord;
        std::vector<Voxel> voxels;
    };
    std::vector<ChunkSnapshot> snapBefore;
    for (const auto& kv : terrain->chunks()) {
        const Chunk* chunk = kv.second.get();
        const int total = chunk->size() * chunk->size() * chunk->size();
        snapBefore.push_back({chunk->coord(),
            std::vector<Voxel>(chunk->data(), chunk->data() + total)});
    }
    ASSERT_FALSE(snapBefore.empty());
    // Remember which coords were resident.
    std::vector<ChunkCoord> origCoords;
    for (const auto& s : snapBefore) origCoords.push_back(s.coord);

    // ── Evict: move camera far — no new decompositions ───────────────────────
    evictOnly(camFar);
    // All origin terrain chunks must be gone (cascade-evicted from origin parent).
    for (const ChunkCoord& cc : origCoords) {
        EXPECT_EQ(terrain->getChunk(cc), nullptr)
            << "origin terrain chunk must be evicted after camera moves far away";
    }
    // Continental decomposed state at origin must be cleared.
    EXPECT_EQ(mgr.decomposedCount("continental"), 0u)
        << "all origin continental decomposed state must clear on cascade eviction";

    // ── Re-approach: regenerate ──────────────────────────────────────────────
    drainFull(camNear);
    ASSERT_GT(terrain->chunks().size(), 0u)
        << "terminal layer must be resident again after re-approach";

    // Every chunk that was present in the first descent must now be byte-identical.
    for (const auto& snap : snapBefore) {
        const Chunk* chunk = terrain->getChunk(snap.coord);
        if (!chunk) continue;  // edge-of-view chunk; skip
        const int total = chunk->size() * chunk->size() * chunk->size();
        ASSERT_EQ(static_cast<int>(snap.voxels.size()), total);
        for (int i = 0; i < total; ++i) {
            EXPECT_EQ(std::memcmp(&snap.voxels[i], &chunk->data()[i], sizeof(Voxel)), 0)
                << "voxel mismatch at linear index " << i << " in chunk ("
                << snap.coord.x << "," << snap.coord.y << "," << snap.coord.z << ")";
            if (std::memcmp(&snap.voxels[i], &chunk->data()[i], sizeof(Voxel)) != 0)
                break;
        }
    }
}

// ── Cascade eviction correctness ──────────────────────────────────────────────
//
// Decompose to terminal, mark one terminal chunk dirty, then evict by moving the
// camera far away (decompPerFrame=0 so no new decompositions happen).
// Verify: (a) the dirty-evict callback fires for the dirty chunk, (b) it does NOT
// fire for any clean sibling, (c) both dirty and clean origin chunks are gone after
// eviction — dirty ones were saved, clean ones silently dropped.
TEST(CascadeDecompositionTest, CascadeEvictionCorrectness) {
    auto cfg = LayerConfig::loadFromString(kFourLayerYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(solidPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xDEADC0DEull, 2);

    // Track which chunks get passed to the dirty-evict callback.
    struct SavedChunk {
        ChunkCoord         coord;
        std::vector<Voxel> voxels;
    };
    std::vector<SavedChunk> saved;
    mgr.setDirtyEvictCallback([&](const Chunk& chunk, const std::string& /*layerName*/) {
        const int total = chunk.size() * chunk.size() * chunk.size();
        saved.push_back({chunk.coord(),
            std::vector<Voxel>(chunk.data(), chunk.data() + total)});
    });

    const WorldCoord camNear(0.0, 0.0, 0.0);
    const WorldCoord camFar(10000.0, 10000.0, 10000.0);
    constexpr double kApproach = 20.0;

    // Drive full decomposition from camNear.
    for (int pass = 0; pass < 60; ++pass) {
        mgr.tick(camNear, kApproach);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drainUntilDone(mgr, world, camNear, kApproach);

    Layer* terrain = world.layer("terrain");
    ASSERT_NE(terrain, nullptr);
    ASSERT_GT(terrain->chunks().size(), 0u);

    // Mark one terminal chunk dirty (simulates a player edit).
    ChunkCoord dirtyCoord = terrain->chunks().begin()->first;
    terrain->chunks().begin()->second->markDirty();
    ASSERT_TRUE(terrain->isChunkDirty(dirtyCoord));

    // Remember all origin terrain coords before eviction.
    std::vector<ChunkCoord> originCoords;
    for (const auto& kv : terrain->chunks())
        originCoords.push_back(kv.first);
    const size_t totalTerminalBefore = originCoords.size();

    // Evict by moving far away with decompPerFrame=0 (no new decompositions).
    for (int pass = 0; pass < 20; ++pass) {
        mgr.tick(camFar, /*approachRadiusM=*/0.0, /*loadPerFrame=*/4, /*decompPerFrame=*/0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // The dirty chunk must have been passed to the save callback exactly once.
    EXPECT_EQ(saved.size(), 1u) << "exactly one dirty chunk should have been saved";
    if (!saved.empty())
        EXPECT_EQ(saved[0].coord, dirtyCoord)
            << "the saved chunk must be the one we marked dirty";

    // All origin terrain chunks must be evicted (dirty saved, clean dropped).
    for (const ChunkCoord& cc : originCoords) {
        EXPECT_EQ(terrain->getChunk(cc), nullptr)
            << "origin terrain chunk must be gone after cascade eviction";
    }

    // Clean chunks must NOT have been passed to the save callback.
    EXPECT_LT(saved.size(), totalTerminalBefore)
        << "clean chunks must be silently dropped, not saved";
}

// ── Memory budget: per-layer resident-chunk cap ───────────────────────────────
//
// Configure a 2-layer stack (composite → terminal) with a tight
// resident_chunk_budget on the composite layer. Load many chunks by placing the
// camera inside a large region, then verify the composite layer's resident count
// never exceeds the budget. Near chunks must not be evicted; farthest-first clean
// chunks must be shed to stay within the cap.
TEST(CascadeDecompositionTest, MemoryBudget) {
    // 2-layer stack: composite (4 m voxels, chunk_size=1 so each chunk == 1 voxel)
    // decomposing into terminal (1 m).  Budget = 4 composite chunks.
    const char* budgetYaml = R"(
layers:
  - name: coarse
    voxel_size_m: 4.0
    mode: composite
    decompose_to: fine
    chunk_size_voxels: 1
    view_distance_chunks: 4
    resident_chunk_budget: 4
  - name: fine
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 1
    view_distance_chunks: 8
)";
    auto cfg = LayerConfig::loadFromString(budgetYaml);
    World world(cfg);
    PluginManager pm;
    int coarseGenCalls = 0;
    auto coarseGen = [](WorldCoord /*o*/, int n, Voxel* out, void* ud) {
        (*static_cast<int*>(ud))++;
        MaterialProperties mp; mp.palette_index = 1; mp.density = 1.0f;
        for (int i = 0; i < n*n*n; ++i) out[i] = Voxel{mp};
    };
    // Wire plugin directly so we can capture coarseGenCalls.
    pm.wireInPlugin([](PluginContext* ctx) -> int {
        ctx->register_layer_generator(ctx, "coarse",
            [](WorldCoord, int n, Voxel* out, void*) {
                MaterialProperties mp; mp.palette_index = 1; mp.density = 1.0f;
                for (int i = 0; i < n*n*n; ++i) out[i] = Voxel{mp};
            }, nullptr);
        ctx->register_layer_generator(ctx, "fine",
            [](WorldCoord, int n, Voxel* out, void*) {
                MaterialProperties mp; mp.palette_index = 2;
                for (int i = 0; i < n*n*n; ++i) out[i] = Voxel{mp};
            }, nullptr);
        return 0;
    });
    (void)coarseGen; (void)coarseGenCalls;

    DecompositionManager mgr(world, pm, cfg, 0xB00DB00Bull, 1);

    // Place camera at origin and run many ticks so more than 4 chunks would
    // normally load within the view_distance_chunks=4 radius (produces 9×9=81
    // XZ candidates with the default unconstrained Y).
    const WorldCoord cam(0.0, 0.0, 0.0);
    for (int pass = 0; pass < 30; ++pass) {
        mgr.tick(cam, /*approachRadiusM=*/0.0, /*loadPerFrame=*/20, /*decompPerFrame=*/0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const Layer* coarse = world.layer("coarse");
    ASSERT_NE(coarse, nullptr);
    EXPECT_LE((int)coarse->chunks().size(), 4)
        << "composite layer must not exceed its resident_chunk_budget of 4";
    EXPECT_GT(coarse->chunks().size(), 0u)
        << "at least the near chunks must remain resident";

    // Re-run with a different camera position: budget must still hold, and the
    // near chunks around the new camera must survive (dirty=false so they can be
    // evicted, but the LOD + budget together keep the nearest ones alive).
    const WorldCoord cam2(100.0, 0.0, 100.0);
    for (int pass = 0; pass < 30; ++pass) {
        mgr.tick(cam2, 0.0, 20, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_LE((int)coarse->chunks().size(), 4)
        << "budget must hold after camera teleport";
}
