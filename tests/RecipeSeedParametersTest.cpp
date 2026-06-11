// Cross-step seed-parameter cascade (M10, docs/ARCHITECTURE.md §6).
//
// Verifies that a top-level seed_parameter measurably biases a leaf two or more
// levels down (grandparent → parent → leaf), each value individually deterministic
// and byte-identical across an evict → regen cycle (simulated by calling
// resolveRecipe + generateChunkFromRecipe twice with the same inputs).
//
// The inherited param set is built manually here — exactly as
// DecompositionManager::makeJob does by walking the ancestor chain — to keep the
// test free of the World/Layer/LOD stack.

#ifdef VOXEL_RECIPE_PLUGIN_PATH

#include "core/PluginManager.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/DecompositionWorker.h"
#include "world/Recipe.h"
#include "world/RecipeResolve.h"
#include "world/ResolvedRecipe.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace {

constexpr int    kN  = 8;
constexpr double kVS = 1.0;

int countEmpty(const Chunk& c) {
    const int n = c.size();
    int empty = 0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (c.at(x, y, z).isEmpty()) ++empty;
    return empty;
}

bool sameGrid(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    const int n = a.size();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const Voxel& av = a.at(x, y, z);
                const Voxel& bv = b.at(x, y, z);
                if (av.isEmpty() != bv.isEmpty()) return false;
                if (!av.isEmpty() &&
                    av.material.palette_index != bv.material.palette_index)
                    return false;
            }
    return true;
}

RecipeParamValue numParam(const char* key, double value) {
    RecipeParamValue p;
    p.key    = key;
    p.kind   = RecipeParamKind::Number;
    p.number = value;
    return p;
}

// Build a leaf recipe that has granite interior, a cave feature with only `scale`
// as a per-entry param (no `cave_density`), and no own seed_parameters.
// cave_density must therefore come entirely from the `inherited` set provided
// to resolveRecipe — i.e. from the ancestor chain.
Recipe makeLeafRecipe() {
    Recipe r;
    r.interior.materials.push_back({"granite", 1.0f});
    r.interior.noise_id = "value";

    FeatureRefValue caveRef;
    caveRef.generator_id = "cave";
    caveRef.params.push_back(numParam("scale", 6.0));
    r.features.push_back(std::move(caveRef));
    // seed_parameters intentionally empty: cave_density comes from inherited only.
    return r;
}

}  // namespace

// ── Two-level cascade: grandparent → parent → leaf ────────────────────────────

// A grandparent's seed_parameter passes through a neutral intermediate (parent
// with no own cave_density) to bias the leaf's cave feature generator, matching
// the three-level chain DecompositionManager drives: continental → regional → local.
TEST(RecipeSeedCascade, GrandparentParamBiasesLeafTwoLevelsDown) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH), kInvalidPluginId);

    const Recipe leaf = makeLeafRecipe();

    auto decomposeWith = [&](double grandparentDensity) {
        // Level 0 (grandparent): sets cave_density = grandparentDensity.
        std::vector<RecipeParamValue> gpSeed{numParam("cave_density", grandparentDensity)};
        // Level 1 (parent): no own cave_density — passes grandparent's value through.
        std::vector<RecipeParamValue> parentSeed;

        // Build inherited = merge(grandparent, parent), root → immediate parent.
        // This reproduces what DecompositionManager::makeJob does for the leaf job.
        std::vector<RecipeParamValue> inherited =
            mergeRecipeParams(gpSeed, parentSeed);

        const ResolvedRecipe resolved = resolveRecipe(leaf, pm, inherited);
        return DecompositionWorker::generateChunkFromRecipe(
            ChunkCoord{0, 0, 0}, kN, kVS, resolved,
            chunkmath::VoxelCoord{0, 0, 0}, kN, 12345u);
    };

    auto lowA  = decomposeWith(0.05);
    auto lowB  = decomposeWith(0.05);   // re-derived identically (evict → regen)
    auto highA = decomposeWith(0.90);
    auto highB = decomposeWith(0.90);

    ASSERT_NE(lowA,  nullptr);
    ASSERT_NE(lowB,  nullptr);
    ASSERT_NE(highA, nullptr);
    ASSERT_NE(highB, nullptr);

    // Determinism: same grandparent density produces byte-identical results.
    EXPECT_TRUE(sameGrid(*lowA,  *lowB));
    EXPECT_TRUE(sameGrid(*highA, *highB));

    // Grandparent constrains the leaf: higher cave_density carves more void.
    EXPECT_GT(countEmpty(*highA), countEmpty(*lowA));
    EXPECT_FALSE(sameGrid(*lowA, *highA));
}

// ── Override semantics: intermediate ancestor beats grandparent ────────────────

// When the grandparent sets cave_density=high but the intermediate parent sets
// cave_density=low (for the same key), the leaf sees low — the parent's value
// wins (later in the root→parent chain overrides earlier, per §6 precedence).
// This confirms that each level can narrow the constraint set independently.
TEST(RecipeSeedCascade, IntermediateAncestorOverridesGrandparent) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.loadPlugin(VOXEL_RECIPE_PLUGIN_PATH), kInvalidPluginId);

    const Recipe leaf = makeLeafRecipe();

    auto decompose = [&](std::vector<RecipeParamValue> inherited) {
        const ResolvedRecipe resolved = resolveRecipe(leaf, pm, inherited);
        return DecompositionWorker::generateChunkFromRecipe(
            ChunkCoord{0, 0, 0}, kN, kVS, resolved,
            chunkmath::VoxelCoord{0, 0, 0}, kN, 67890u);
    };

    // Grandparent high, parent overrides low → leaf should see low density.
    std::vector<RecipeParamValue> overrideInherited =
        mergeRecipeParams({numParam("cave_density", 0.90)},   // grandparent
                          {numParam("cave_density", 0.05)});  // parent overrides

    // Baseline: only grandparent present (high density).
    std::vector<RecipeParamValue> highInherited{numParam("cave_density", 0.90)};

    auto withOverride = decompose(overrideInherited);
    auto withHigh     = decompose(highInherited);

    ASSERT_NE(withOverride, nullptr);
    ASSERT_NE(withHigh,     nullptr);

    // After parent overrides to low, the leaf produces far fewer caves than
    // if the grandparent's high value had reached it unmodified.
    EXPECT_LT(countEmpty(*withOverride), countEmpty(*withHigh));
}

#endif  // VOXEL_RECIPE_PLUGIN_PATH
