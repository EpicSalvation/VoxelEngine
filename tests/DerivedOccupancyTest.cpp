// Engine-derived coarse occupancy (M18.5, docs/proposals/recipe-occupancy.md).
//
// When a ROOT composite layer registers an occupancy-bearing recipe but no layer
// generator, DecompositionManager synthesizes the layer's coarse occupancy from
// the recipe's own carve field (no hand-written generator that must stay in sync).
// These tests verify:
//   - a macro is solid iff the carve field is solid anywhere in its footprint
//     (below/straddling a heightmap surface present; fully above absent) — the
//     §4 coarse-supersets-fine invariant, engine-guaranteed;
//   - the atomic macro carries the recipe's dominant interior material;
//   - a layer WITH a registered generator keeps it (the derivation is opt-in).

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "world/DecompositionManager.h"
#include "world/Layer.h"
#include "world/World.h"

#include <gtest/gtest.h>

namespace {

// A heightmap "surface" carve field: solid (1.0) below world y = 10, empty above.
// Pure in position; ignores the seed (a continuous surface across macros).
float testSurface(WorldCoord p, uint64_t, const RecipeParam*, size_t, void*) {
    return p.value.y < 10.0 ? 1.0f : 0.0f;
}

MaterialProperties stoneMat() {
    MaterialProperties m{};
    m.density = 2700.0f;
    m.structural_strength = 0.8f;
    m.hardness = 0.7f;
    m.palette_index = 1;
    return m;
}

// Registers the carve noise, a stone material, and an occupancy recipe for the
// "blocks" composite layer — but NO layer generator, so the manager derives the
// coarse occupancy.
int occupancyPluginInit(PluginContext* ctx) {
    ctx->register_noise(ctx, "test_surface", testSurface, nullptr);
    ctx->register_material(ctx, "stone", stoneMat());

    MaterialWeight interior[1] = {{"stone", 1.0f}};
    RecipeDesc r{};
    r.interior.materials      = interior;
    r.interior.material_count = 1;
    r.occupancy.present   = true;
    r.occupancy.noise_id  = "test_surface";
    r.occupancy.threshold = 0.5f;
    ctx->register_recipe(ctx, "blocks", &r);
    return 0;
}

// A generator that fills every macro with a sentinel material (palette 5), to
// prove a registered generator wins over the derived occupancy.
void sentinelGen(WorldCoord /*origin*/, int n, Voxel* out, void*) {
    Voxel v{};
    v.material.palette_index = 5;
    v.material.density       = 100.0f;
    for (int i = 0; i < n * n * n; ++i) out[i] = v;
}

int generatorPluginInit(PluginContext* ctx) {
    const int rc = occupancyPluginInit(ctx);          // same recipe + noise + material
    ctx->register_layer_generator(ctx, "blocks", sentinelGen, nullptr);  // ...plus a generator
    return rc;
}

// blocks (composite 4 m, chunk 1) → terrain (terminal 1 m, chunk 4). ratio 4;
// parent voxel 4 m == terrain chunk world (1 m × 4) — coarse-supersets-fine OK.
const char* kYaml = R"(
layers:
  - name: blocks
    voxel_size_m: 4.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 1
    view_distance_chunks: 4
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 4
    view_distance_chunks: 2
)";

// Load every desired root-composite chunk with NO decomposition (decompPerFrame
// 0), so the derived occupancy stays intact in the composite layer's voxels.
void loadAll(DecompositionManager& mgr, const WorldCoord& cam) {
    for (int i = 0; i < 3; ++i)
        mgr.tick(cam, /*approach=*/1000.0, /*load=*/1000000, /*decomp=*/0, /*apply=*/0);
}

}  // namespace

TEST(DerivedOccupancy, RootLayerOccupancyTracksCarveSurface) {
    auto cfg = LayerConfig::loadFromString(kYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(occupancyPluginInit);
    DecompositionManager mgr(world, pm, cfg, 0xABCDEFull, /*threads=*/1);

    // Camera above the surface (its own macro is empty, so nothing decomposes even
    // if approach fired); decompPerFrame 0 keeps every macro atomic regardless.
    loadAll(mgr, WorldCoord(2.0, 18.0, 2.0));

    const Layer* blocks = world.layer("blocks");
    ASSERT_NE(blocks, nullptr);

    // Macro [4,8): entirely below y=10 → present (solid stone).
    const Voxel below = blocks->getVoxel(WorldCoord(2.0, 6.0, 2.0));
    EXPECT_FALSE(below.isEmpty());
    EXPECT_EQ(below.material.palette_index, 1u);  // dominant interior material

    // Macro [8,12): straddles the surface (child cells at 8.5/9.5 are below) →
    // present, the conservative superset (decomposition would yield solid cells).
    EXPECT_FALSE(blocks->getVoxel(WorldCoord(2.0, 10.0, 2.0)).isEmpty());

    // Macro [12,16): entirely above y=10 → absent (empty).
    EXPECT_TRUE(blocks->getVoxel(WorldCoord(2.0, 14.0, 2.0)).isEmpty());
}

TEST(DerivedOccupancy, RegisteredGeneratorWinsOverDerivation) {
    auto cfg = LayerConfig::loadFromString(kYaml);
    World world(cfg);
    PluginManager pm;
    pm.wireInPlugin(generatorPluginInit);  // recipe has occupancy AND a generator
    DecompositionManager mgr(world, pm, cfg, 0xABCDEFull, /*threads=*/1);

    loadAll(mgr, WorldCoord(2.0, 18.0, 2.0));

    const Layer* blocks = world.layer("blocks");
    ASSERT_NE(blocks, nullptr);

    // The registered generator fills every macro with the sentinel material —
    // including ABOVE the carve surface, where derived occupancy would be empty.
    const Voxel above = blocks->getVoxel(WorldCoord(2.0, 14.0, 2.0));
    EXPECT_FALSE(above.isEmpty());
    EXPECT_EQ(above.material.palette_index, 5u);  // generator output, not derivation
}
