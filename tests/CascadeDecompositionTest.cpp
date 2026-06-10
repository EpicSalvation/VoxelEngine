// M10 cascade decomposition tests.
//
// Verifies that DecompositionManager correctly drives the full N-layer chain:
// approaching the camera drives each composite level to decompose in declared
// (coarsest-first) order; a terminal voxel deep in the chain is only resident
// after its entire ancestor chain has decomposed; each composite layer's
// DecompositionState stays consistent. Also verifies that the coarse-supersets-
// fine invariant check fires for invalid composite→composite configs.

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
void solidGen(WorldCoord /*origin*/, int n, Voxel* out, void*) {
    MaterialProperties mp;
    mp.palette_index = 1;
    mp.density = 100.0f;
    mp.hardness = 1.0f;
    for (int i = 0; i < n * n * n; ++i)
        out[i] = Voxel{mp};
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
