// Recipe-driven decomposition (M9, docs/ARCHITECTURE.md §6).
//
// These tests pin down the recipe-driven half of decomposition that M9 adds on
// top of the M6 worker:
//   - determinism: the same (resolved recipe, macro coord, seed) yields a
//     byte-identical child grid across repeated and concurrent worker runs;
//   - the material-distribution sampler honors weights (given a uniform noise)
//     and is spatially stable for a fixed seed;
//   - per-face boundary overrides land on the correct faces, and only those;
//   - feature overlays run in declared order (an order-sensitive pair proves it);
//   - the real cave/ore feature generators (loaded from the recipe-world plugin)
//     carve the expected void fraction / replace only the targeted material;
//   - a parent seed parameter biases the child grid (single-step), measurably
//     and reproducibly.

#include "core/PluginManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionWorker.h"
#include "world/Recipe.h"
#include "world/RecipeResolve.h"
#include "world/ResolvedRecipe.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr int    kN  = 8;     // child chunk size (voxels per side)
constexpr double kVS = 1.0;   // child voxel size (m)

// A uniform [0,1) value per integer lattice cell — sampled at voxel centers
// (i+0.5), each voxel maps to a distinct cell, so material selection honors
// weights exactly over many voxels (unlike interpolated value noise, which is
// non-uniform). Used to test the SAMPLER's weight logic in isolation.
float uniformNoise(WorldCoord p, uint64_t seed, const RecipeParam*, size_t, void*) {
    auto fl = [](double v) { return static_cast<int64_t>(std::floor(v)); };
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(fl(p.value.x)) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(fl(p.value.y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(fl(p.value.z)) * 0x165667B19E3779F9ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<float>(h >> 40) / static_cast<float>(1ull << 24);  // [0,1)
}

MaterialProperties mat(uint8_t palette) {
    MaterialProperties m;
    m.palette_index = palette;
    m.density       = 1000.0f;  // non-zero so the voxel is solid, not "empty"
    return m;
}

bool sameVoxel(const Voxel& a, const Voxel& b) {
    const MaterialProperties& x = a.material;
    const MaterialProperties& y = b.material;
    return x.density == y.density && x.structural_strength == y.structural_strength &&
           x.thermal_conductivity == y.thermal_conductivity && x.porosity == y.porosity &&
           x.hardness == y.hardness && x.palette_index == y.palette_index;
}

bool sameGrid(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    const int n = a.size();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (!sameVoxel(a.at(x, y, z), b.at(x, y, z))) return false;
    return true;
}

std::vector<DecompositionResult> collect(DecompositionWorker& w, size_t n) {
    std::vector<DecompositionResult> all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (all.size() < n) {
        for (auto& r : w.drain()) all.push_back(std::move(r));
        if (all.size() < n) {
            std::this_thread::yield();
            if (std::chrono::steady_clock::now() > deadline) break;
        }
    }
    return all;
}

// A two-material interior distribution sampled by the uniform test noise.
ResolvedRecipe weightedRecipe(uint8_t a, float wa, uint8_t b, float wb) {
    ResolvedRecipe r;
    r.interior.materials = {{mat(a), wa}, {mat(b), wb}};
    r.interior.noise     = &uniformNoise;
    return r;
}

}  // namespace

// ── Determinism ──────────────────────────────────────────────────────────────

TEST(RecipeDecomposition, GenerateFromRecipeIsDeterministicAcrossCalls) {
    auto recipe = std::make_shared<const ResolvedRecipe>(weightedRecipe(1, 0.7f, 2, 0.3f));
    for (ChunkCoord c : {ChunkCoord{0, 0, 0}, ChunkCoord{3, -2, 7}, ChunkCoord{-5, 1, -9}}) {
        const chunkmath::VoxelCoord macroMin{static_cast<int64_t>(c.x) * kN,
                                             static_cast<int64_t>(c.y) * kN,
                                             static_cast<int64_t>(c.z) * kN};
        auto p = DecompositionWorker::generateChunkFromRecipe(c, kN, kVS, *recipe, macroMin, kN, 4242u);
        auto q = DecompositionWorker::generateChunkFromRecipe(c, kN, kVS, *recipe, macroMin, kN, 4242u);
        ASSERT_TRUE(p && q);
        EXPECT_TRUE(sameGrid(*p, *q));
    }
}

TEST(RecipeDecomposition, ConcurrentRecipeJobsMatchSingleThreadedReference) {
    constexpr int kJobs = 48;
    auto recipe = std::make_shared<const ResolvedRecipe>(weightedRecipe(1, 0.6f, 4, 0.4f));

    auto childChunkFor = [](int i) { return ChunkCoord{i, 0, 0}; };
    auto seedFor       = [](int i) { return voxel_seed_mix(0xABCDEFu, static_cast<uint64_t>(i)); };

    std::vector<std::unique_ptr<Chunk>> reference(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        const ChunkCoord c = childChunkFor(i);
        const chunkmath::VoxelCoord macroMin{static_cast<int64_t>(c.x) * kN, 0, 0};
        reference[i] = DecompositionWorker::generateChunkFromRecipe(
            c, kN, kVS, *recipe, macroMin, kN, seedFor(i));
    }

    DecompositionWorker worker;
    for (int i = 0; i < kJobs; ++i) {
        const ChunkCoord c = childChunkFor(i);
        DecompositionJob job;
        job.macro           = chunkmath::VoxelCoord{i, 0, 0};
        job.childChunks     = {c};
        job.childChunkSize  = kN;
        job.childVoxelSizeM = kVS;
        job.recipe          = recipe;
        job.seed            = seedFor(i);
        job.ratio           = kN;
        job.macroChildMin   = chunkmath::VoxelCoord{static_cast<int64_t>(c.x) * kN, 0, 0};
        worker.enqueue(job);
    }

    std::vector<DecompositionResult> results = collect(worker, kJobs);
    ASSERT_EQ(results.size(), static_cast<size_t>(kJobs));
    for (const DecompositionResult& r : results) {
        const int i = static_cast<int>(r.macro.x);
        ASSERT_GE(i, 0);
        ASSERT_LT(i, kJobs);
        ASSERT_EQ(r.chunks.size(), 1u);
        EXPECT_TRUE(sameGrid(*r.chunks[0], *reference[i])) << "macro " << i;
    }
}

// ── Material distribution ──────────────────────────────────────────────────────

TEST(RecipeDecomposition, DistributionHonorsWeights) {
    // 70/30 split over a large volume, sampled by the uniform test noise.
    auto recipe = weightedRecipe(1, 0.7f, 2, 0.3f);
    constexpr int kBig = 16;  // 16³ = 4096 voxels — enough to average out
    Chunk chunk(ChunkCoord{0, 0, 0}, kBig, WorldCoord(0.0, 0.0, 0.0));
    fillChildChunk(chunk, kVS, recipe, chunkmath::VoxelCoord{0, 0, 0}, kBig, 99u);

    int a = 0, b = 0;
    for (int z = 0; z < kBig; ++z)
        for (int y = 0; y < kBig; ++y)
            for (int x = 0; x < kBig; ++x) {
                const uint8_t p = chunk.at(x, y, z).material.palette_index;
                if (p == 1) ++a; else if (p == 2) ++b;
            }
    const int total = kBig * kBig * kBig;
    ASSERT_EQ(a + b, total);  // every voxel got one of the two materials
    const double fracA = static_cast<double>(a) / total;
    EXPECT_NEAR(fracA, 0.7, 0.05);  // honors the weight within sampling tolerance
}

TEST(RecipeDecomposition, DistributionIsSpatiallyStableForFixedSeed) {
    auto recipe = weightedRecipe(1, 0.5f, 2, 0.5f);
    Chunk a(ChunkCoord{2, 1, -3}, kN, chunkmath::chunkOrigin({2, 1, -3}, kVS, kN));
    Chunk b(ChunkCoord{2, 1, -3}, kN, chunkmath::chunkOrigin({2, 1, -3}, kVS, kN));
    const chunkmath::VoxelCoord m{2 * kN, 1 * kN, -3 * kN};
    fillChildChunk(a, kVS, recipe, m, kN, 7u);
    fillChildChunk(b, kVS, recipe, m, kN, 7u);
    EXPECT_TRUE(sameGrid(a, b));
}

// ── Boundary overrides ─────────────────────────────────────────────────────────

TEST(RecipeDecomposition, BoundaryOverridesLandOnCorrectFacesOnly) {
    ResolvedRecipe r;
    r.interior.materials = {{mat(1), 1.0f}};          // granite interior
    r.top.present = true;    r.top.depth = 2;    r.top.distribution.materials    = {{mat(3), 1.0f}};  // soil
    r.bottom.present = true; r.bottom.depth = 1;  r.bottom.distribution.materials = {{mat(2), 1.0f}};  // basalt
    r.side.present = true;   r.side.depth = 1;    r.side.distribution.materials   = {{mat(11), 1.0f}}; // iron

    Chunk chunk(ChunkCoord{0, 0, 0}, kN, WorldCoord(0.0, 0.0, 0.0));
    fillChildChunk(chunk, kVS, r, chunkmath::VoxelCoord{0, 0, 0}, kN, 1u);

    EXPECT_EQ(chunk.at(4, 4, 4).material.palette_index, 1u);   // interior
    EXPECT_EQ(chunk.at(4, 7, 4).material.palette_index, 3u);   // top cap (my=7 >= 8-2)
    EXPECT_EQ(chunk.at(4, 6, 4).material.palette_index, 3u);   // top cap (depth 2)
    EXPECT_EQ(chunk.at(4, 5, 4).material.palette_index, 1u);   // just below the cap: interior
    EXPECT_EQ(chunk.at(4, 0, 4).material.palette_index, 2u);   // bottom (my=0 < 1)
    EXPECT_EQ(chunk.at(0, 4, 4).material.palette_index, 11u);  // side (mx=0)
    EXPECT_EQ(chunk.at(7, 4, 4).material.palette_index, 11u);  // side (mx=7 >= 8-1)
    // Corner where side and top overlap: top wins (overlap order bottom->side->top).
    EXPECT_EQ(chunk.at(0, 7, 0).material.palette_index, 3u);
    // Corner where side and bottom overlap: side wins over bottom.
    EXPECT_EQ(chunk.at(0, 0, 0).material.palette_index, 11u);
}

// ── Feature overlay ordering ─────────────────────────────────────────────────────

namespace {
// Feature A: paint every solid voxel palette 20.
void paint20(WorldCoord, double, int n, Voxel* v, const RecipeParam*, size_t, uint64_t, void*) {
    for (int i = 0; i < n * n * n; ++i)
        if (!v[i].isEmpty()) v[i].material.palette_index = 20;
}
// Feature B: any voxel currently palette 20 becomes palette 30.
void promote20to30(WorldCoord, double, int n, Voxel* v, const RecipeParam*, size_t, uint64_t, void*) {
    for (int i = 0; i < n * n * n; ++i)
        if (v[i].material.palette_index == 20) v[i].material.palette_index = 30;
}

ResolvedRecipe orderedRecipe(FeatureGeneratorFn first, FeatureGeneratorFn second) {
    ResolvedRecipe r;
    r.interior.materials = {{mat(1), 1.0f}};
    r.features.push_back({first,  nullptr, {}, 1});
    r.features.push_back({second, nullptr, {}, 2});
    return r;
}
}  // namespace

TEST(RecipeDecomposition, FeatureOverlaysRunInDeclaredOrder) {
    // paint20 then promote20to30 => everything ends 30.
    {
        ResolvedRecipe r = orderedRecipe(&paint20, &promote20to30);
        Chunk chunk(ChunkCoord{0, 0, 0}, kN, WorldCoord(0, 0, 0));
        fillChildChunk(chunk, kVS, r, chunkmath::VoxelCoord{0, 0, 0}, kN, 1u);
        EXPECT_EQ(chunk.at(0, 0, 0).material.palette_index, 30u);
        EXPECT_EQ(chunk.at(3, 5, 6).material.palette_index, 30u);
    }
    // promote20to30 then paint20 => nothing was 20 yet, so everything ends 20.
    {
        ResolvedRecipe r = orderedRecipe(&promote20to30, &paint20);
        Chunk chunk(ChunkCoord{0, 0, 0}, kN, WorldCoord(0, 0, 0));
        fillChildChunk(chunk, kVS, r, chunkmath::VoxelCoord{0, 0, 0}, kN, 1u);
        EXPECT_EQ(chunk.at(0, 0, 0).material.palette_index, 20u);
        EXPECT_EQ(chunk.at(3, 5, 6).material.palette_index, 20u);
    }
}

// ── Real cave/ore feature generators (loaded from the recipe-world plugin) ────
#ifdef VOXEL_RECIPE_PLUGIN_PATH
namespace {

// Look up a feature generator's fn/user_data registered by the loaded plugin.
const RegisteredFeatureGenerator* feature(const PluginManager& pm, const char* id) {
    return pm.findFeatureGenerator(id);
}

// A chunk solid everywhere with the given palette.
Chunk solidChunk(uint8_t palette) {
    Chunk c(ChunkCoord{0, 0, 0}, kN, WorldCoord(0, 0, 0));
    for (int i = 0; i < kN * kN * kN; ++i) c.data()[i] = Voxel{mat(palette)};
    return c;
}

RecipeParam num(const char* key, double v) {
    RecipeParam p; p.key = key; p.kind = RecipeParamKind::Number; p.number = v; return p;
}

int countEmpty(const Chunk& c) {
    int e = 0;
    for (int i = 0; i < kN * kN * kN; ++i) if (c.at(i % kN, (i / kN) % kN, i / (kN * kN)).isEmpty()) ++e;
    return e;
}

}  // namespace

TEST(RecipeFeatures, CaveCarvesMonotoneVoidFractionAndIsDeterministic) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH), kInvalidPluginId);
    const RegisteredFeatureGenerator* cave = feature(pm, "cave");
    ASSERT_NE(cave, nullptr);

    auto carve = [&](double density) {
        Chunk c = solidChunk(1);
        RecipeParam p[2] = {num("cave_density", density), num("scale", 6.0)};
        cave->fn(c.origin(), kVS, kN, c.data(), p, 2, 31337u, cave->user_data);
        return c;
    };

    const int total = kN * kN * kN;
    EXPECT_EQ(countEmpty(carve(0.0)), 0);        // density 0 carves nothing
    EXPECT_EQ(countEmpty(carve(1.0)), total);    // density 1 carves everything
    const int v3 = countEmpty(carve(0.3));
    const int v6 = countEmpty(carve(0.6));
    EXPECT_GT(v6, v3);                            // more density => more void
    EXPECT_GT(v3, 0);
    EXPECT_LT(v6, total);

    // Determinism: same params + seed => byte-identical carve.
    EXPECT_TRUE(sameGrid(carve(0.45), carve(0.45)));
}

TEST(RecipeFeatures, OreReplacesOnlyTargetedMaterial) {
    PluginManager pm;
    ASSERT_NE(pm.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH), kInvalidPluginId);
    const RegisteredFeatureGenerator* ore = feature(pm, "ore");
    ASSERT_NE(ore, nullptr);

    // A grid that alternates granite (1) and basalt (10) by x parity.
    Chunk c(ChunkCoord{0, 0, 0}, kN, WorldCoord(0, 0, 0));
    int graniteBefore = 0, basaltBefore = 0;
    for (int z = 0; z < kN; ++z)
        for (int y = 0; y < kN; ++y)
            for (int x = 0; x < kN; ++x) {
                const uint8_t pal = (x % 2 == 0) ? 1 : 10;
                c.at(x, y, z) = Voxel{mat(pal)};
                if (pal == 1) ++graniteBefore; else ++basaltBefore;
            }

    RecipeParam p[3] = {num("ore_richness", 0.6), num("scale", 5.0), num("target_palette", 1)};
    ore->fn(c.origin(), kVS, kN, c.data(), p, 3, 555u, ore->user_data);

    int granite = 0, basalt = 0, iron = 0, other = 0;
    for (int z = 0; z < kN; ++z)
        for (int y = 0; y < kN; ++y)
            for (int x = 0; x < kN; ++x) {
                switch (c.at(x, y, z).material.palette_index) {
                    case 1:  ++granite; break;
                    case 10: ++basalt;  break;
                    case 11: ++iron;    break;
                    default: ++other;   break;
                }
            }
    EXPECT_EQ(basalt, basaltBefore);          // basalt is never touched
    EXPECT_EQ(granite + iron, graniteBefore);  // only granite turned to iron
    EXPECT_GT(iron, 0);                         // some did
    EXPECT_GT(granite, 0);                      // but not all
    EXPECT_EQ(other, 0);
}

// ── Hierarchical seed parameters (single-step) ────────────────────────────────

TEST(RecipeSeedParameters, ParentSeedBiasesChildGridDeterministically) {
    PluginManager pm;
    pm.registerBuiltinNoise();  // the recipe's interior uses the built-in "value"
    ASSERT_NE(pm.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH), kInvalidPluginId);
    const Recipe* base = pm.findRecipe("blocks");
    ASSERT_NE(base, nullptr);

    // Decompose the same macro voxel under two parent cave_density values.
    // We INJECT a "cave_density" seed parameter (the base recipe has none — its
    // cave feature is normally depth-ramped via __altitude). resolveRecipe merges
    // seed params underneath each feature's own params, and the cave feature
    // declares no cave_density of its own, so this parent value reaches the
    // feature and pins its carve density directly (plugin cave_feature honors an
    // explicit cave_density). That is the "parent seed biases the child" path.
    auto decompose = [&](double caveDensity) {
        Recipe r = *base;
        RecipeParamValue caveDensityParam;
        caveDensityParam.key    = "cave_density";
        caveDensityParam.kind   = RecipeParamKind::Number;
        caveDensityParam.number = caveDensity;
        r.seed_parameters = mergeRecipeParams(r.seed_parameters, {caveDensityParam});
        const ResolvedRecipe resolved = resolveRecipe(r, pm);
        return DecompositionWorker::generateChunkFromRecipe(
            ChunkCoord{0, 0, 0}, kN, kVS, resolved, chunkmath::VoxelCoord{0, 0, 0}, kN, 909u);
    };

    auto lowA  = decompose(0.1);
    auto lowB  = decompose(0.1);
    auto highA = decompose(0.8);
    auto highB = decompose(0.8);
    ASSERT_TRUE(lowA && lowB && highA && highB);

    // Each value regenerates identically (determinism) ...
    EXPECT_TRUE(sameGrid(*lowA, *lowB));
    EXPECT_TRUE(sameGrid(*highA, *highB));
    // ... but the two parent values produce measurably different child grids:
    // a denser cave_density carves more void.
    EXPECT_GT(countEmpty(*highA), countEmpty(*lowA));
    EXPECT_FALSE(sameGrid(*lowA, *highA));
}
#endif  // VOXEL_RECIPE_PLUGIN_PATH
