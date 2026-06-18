// M16 (L5) immutable-layer streaming tests.
//
// Immutable layers used to be generated-once-and-fully-resident; M16 brings their
// meshes into the DecompositionManager/LODManager residency cycle so they stream
// in/out under the layer's StreamingVolume + resident_chunk_budget, just like
// composite/terminal layers. They still skip dirty/persist (immutable chunks
// regenerate deterministically from seed, with no save path).
//
// Covers:
//   - StreamsInUnderVolume        — resident set is bounded by the volume, not all-resident
//   - EvictsWhenCameraLeaves      — chunks outside the (grown) volume are dropped
//   - RegeneratesIdenticallyOnReentry — re-entered chunks rebuild byte-identically; never dirty
//   - HeterogeneousBudgetsTogether — a tiny box playspace and a vast shell backdrop
//                                    stream simultaneously, each honoring its own budget
//                                    (the M16 heterogeneous-budget acceptance check)

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionManager.h"
#include "world/Layer.h"
#include "world/StreamingVolume.h"
#include "world/World.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

// Generator pure in the chunk's world-space origin, so a chunk that is evicted
// and re-entered regenerates byte-identically. palette varies with the origin so
// distinct chunks hold distinct data (a real re-entry-determinism check).
void originGen(WorldCoord origin, int n, Voxel* out, void*) {
    const uint8_t p = static_cast<uint8_t>(
        1 + (static_cast<int>(origin.value.x) + static_cast<int>(origin.value.y) +
             static_cast<int>(origin.value.z)) % 7);
    Voxel v{};  // zero-init including padding so memcmp comparisons are stable
    v.material.palette_index = p;
    v.material.density       = 100.0f;
    for (int i = 0; i < n * n * n; ++i) out[i] = v;
}

int immutablePluginInit(PluginContext* ctx) {
    for (const char* name : {"playspace", "backdrop"})
        ctx->register_layer_generator(ctx, name, originGen, nullptr);
    return 0;
}

size_t residentCount(World& w, const char* layer) {
    return w.layer(layer)->chunks().size();
}

// A big load budget so a single tick streams every chunk the volume wants;
// approach=0 since there is no composite work in these immutable-only configs.
void tickFull(DecompositionManager& mgr, const WorldCoord& cam) {
    mgr.tick(cam, /*approach=*/0.0, /*loadPerFrame=*/100000,
             /*decompPerFrame=*/0, /*applyPerFrame=*/0);
}

}  // namespace

// ── A single immutable box layer streams under its volume, not fully resident ──

TEST(ImmutableStreaming, StreamsInUnderVolumeAndEvictsWhenCameraLeaves) {
    const char* yaml = R"(
layers:
  - name: backdrop
    voxel_size_m: 1.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 1
)";
    LayerConfig cfg = LayerConfig::loadFromString(yaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(immutablePluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xABCDull, /*threads=*/1);

    // Camera at origin → a 3×3×3 box volume (radius 1) is resident. Bounded by
    // the volume — NOT "generate once, keep forever".
    tickFull(mgr, WorldCoord(2.0, 2.0, 2.0));
    EXPECT_EQ(residentCount(world, "backdrop"), 27u);

    // Move the camera far along an arbitrary axis. The old footprint is now well
    // outside the grown (load+hysteresis) volume and is evicted; a new box loads.
    tickFull(mgr, WorldCoord(2.0, 1004.0, 2.0));
    EXPECT_EQ(residentCount(world, "backdrop"), 27u);
    // None of the original near-origin chunks remain resident.
    EXPECT_EQ(world.layer("backdrop")->getChunk(ChunkCoord{0, 0, 0}), nullptr);
}

// ── Re-entry regenerates identically; immutable chunks never go dirty/persist ──

TEST(ImmutableStreaming, RegeneratesIdenticallyOnReentryAndNeverDirty) {
    const char* yaml = R"(
layers:
  - name: backdrop
    voxel_size_m: 1.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 1
)";
    LayerConfig cfg = LayerConfig::loadFromString(yaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(immutablePluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xABCDull, 1);

    // A dirty-evict callback must NEVER fire for an immutable layer (no save path).
    int dirtyEvicts = 0;
    mgr.setDirtyEvictCallback([&](const Chunk&, const std::string&) { ++dirtyEvicts; });

    const ChunkCoord probe{0, 0, 0};
    tickFull(mgr, WorldCoord(2.0, 2.0, 2.0));
    const Chunk* first = world.layer("backdrop")->getChunk(probe);
    ASSERT_NE(first, nullptr);
    EXPECT_FALSE(first->dirty());  // generation leaves a chunk clean

    const int n = first->size();
    std::vector<Voxel> snapshot(first->data(), first->data() + n * n * n);

    // Leave (evict the probe chunk) then return (regenerate it from seed).
    tickFull(mgr, WorldCoord(2.0, 1004.0, 2.0));
    ASSERT_EQ(world.layer("backdrop")->getChunk(probe), nullptr);
    tickFull(mgr, WorldCoord(2.0, 2.0, 2.0));

    const Chunk* again = world.layer("backdrop")->getChunk(probe);
    ASSERT_NE(again, nullptr);
    EXPECT_FALSE(again->dirty());
    EXPECT_EQ(std::memcmp(snapshot.data(), again->data(), snapshot.size() * sizeof(Voxel)), 0);

    EXPECT_EQ(dirtyEvicts, 0);  // immutable chunks are dropped, never persisted
}

// ── Heterogeneous budgets: tiny box playspace + vast shell backdrop, together ──

TEST(ImmutableStreaming, HeterogeneousBudgetsStreamTogether) {
    // A tiny tight playspace volume (small box, tiny budget) and a vast sparse
    // backdrop (a wide shell, larger budget) streaming SIMULTANEOUSLY within their
    // own StreamingVolume + budget — radically different scales and densities in
    // one stack (the M16 acceptance item).
    const char* yaml = R"(
layers:
  - name: playspace
    voxel_size_m: 1.0
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 2
    resident_chunk_budget: 10
  - name: backdrop
    voxel_size_m: 0.5
    mode: immutable
    chunk_size_voxels: 4
    view_distance_chunks: 5
    resident_chunk_budget: 15
    streaming_volume:
      shape: shell
      shell_thickness_chunks: 2
)";
    LayerConfig cfg = LayerConfig::loadFromString(yaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(immutablePluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xABCDull, 1);

    // Confirm the unbudgeted desired sets are both larger than the budgets, so the
    // caps are actually doing work (not trivially satisfied).
    LODManager probeLod(cfg);
    EXPECT_GT(probeLod.volumeFor("playspace").desired(ChunkCoord{0, 0, 0}).size(), 10u);
    EXPECT_GT(probeLod.volumeFor("backdrop").desired(ChunkCoord{0, 0, 0}).size(), 15u);

    tickFull(mgr, WorldCoord(2.0, 2.0, 2.0));

    // Each layer honors its OWN budget independently.
    EXPECT_EQ(residentCount(world, "playspace"), 10u);
    EXPECT_EQ(residentCount(world, "backdrop"),  15u);

    // Both are actually streaming (non-empty) at the same time — different scales,
    // different shapes, one residency cycle.
    EXPECT_GT(residentCount(world, "playspace"), 0u);
    EXPECT_GT(residentCount(world, "backdrop"),  0u);

    // The box playspace keeps its center chunk; the hollow shell does not retain
    // the chunk directly under the camera (it is inside the inner radius).
    EXPECT_NE(world.layer("playspace")->getChunk(ChunkCoord{0, 0, 0}), nullptr);
    // The shell's nearest retained chunks are out at its band, not at the center —
    // the farthest-first budget keeps the inner edge of the band.
    EXPECT_EQ(world.layer("backdrop")->getChunk(ChunkCoord{0, 0, 0}), nullptr);
}
