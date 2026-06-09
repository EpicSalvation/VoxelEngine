// Tests for the M9 "Recipe schema and registry" work:
//   - register_recipe deep-copies a flat RecipeDesc into an owning Recipe, keyed
//     by layer name, with overwrite-by-name and per-plugin unload teardown.
//   - The noise registry: built-in ids resolve and are deterministic; a plugin
//     register_noise of the same id overrides a built-in and is gone after unload.
//   - validateRecipes(): unregistered feature/noise ids are startup errors; a
//     composite layer with no explicit recipe resolves to the default (no error);
//     an unknown material id falls back to the neutral default (no error).

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/RecipeValidation.h"
#include "world/Noise.h"
#include "world/Recipe.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

namespace {

// Minimal valid stack: a composite "blocks" decomposing to a terminal "terrain".
const char* kCompositeConfig = R"(
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)";

LayerConfig compositeConfig() { return LayerConfig::loadFromString(kCompositeConfig); }

// A no-op feature generator matching FeatureGeneratorFn — only its registration
// presence matters to validation.
void dummyFeature(WorldCoord, double, int, Voxel*,
                  const RecipeParam*, size_t, uint64_t, void*) {}

// A plugin noise that overrides the built-in "value" with a recognizable constant.
float constHalfNoise(WorldCoord, uint64_t, const RecipeParam*, size_t, void*) {
    return 0.5f;
}

// Registers a fully-populated recipe for "blocks" from LOCAL arrays (which go out
// of scope when init returns), proving register_recipe deep-copies. Also registers
// the feature generator and uses a built-in noise id so the recipe validates.
int initFullRecipe(PluginContext* ctx) {
    MaterialWeight interiorMats[2] = {{"granite", 0.7f}, {"basalt", 0.3f}};
    RecipeParam noiseParams[1];
    noiseParams[0].key = "scale"; noiseParams[0].kind = RecipeParamKind::Number; noiseParams[0].number = 16.0;

    FeatureRef feats[1];
    feats[0].generator_id = "cave"; feats[0].params = nullptr; feats[0].param_count = 0;

    MaterialWeight topMats[1] = {{"soil", 1.0f}};

    RecipeParam seed[1];
    seed[0].key = "cave_density"; seed[0].kind = RecipeParamKind::Number; seed[0].number = 0.5;

    RecipeDesc d{};
    d.interior.materials         = interiorMats;
    d.interior.material_count    = 2;
    d.interior.noise_id          = "fbm";
    d.interior.noise_params      = noiseParams;
    d.interior.noise_param_count = 1;
    d.features                   = feats;
    d.feature_count              = 1;
    d.top.present                = true;
    d.top.depth                  = 2;
    d.top.distribution.materials      = topMats;
    d.top.distribution.material_count = 1;
    d.seed_parameters            = seed;
    d.seed_parameter_count       = 1;

    ctx->register_recipe(ctx, "blocks", &d);
    ctx->register_feature_generator(ctx, "cave", &dummyFeature, nullptr);
    return 0;
}

// Registers a recipe for "blocks" twice; the second registration must overwrite.
int initOverwriteRecipe(PluginContext* ctx) {
    MaterialWeight first[1]  = {{"granite", 1.0f}};
    MaterialWeight second[1] = {{"basalt", 1.0f}};
    RecipeDesc a{};
    a.interior.materials = first;  a.interior.material_count = 1;
    ctx->register_recipe(ctx, "blocks", &a);
    RecipeDesc b{};
    b.interior.materials = second; b.interior.material_count = 1;
    ctx->register_recipe(ctx, "blocks", &b);
    return 0;
}

// Recipe naming a feature generator that nobody registers — a validation error.
int initUnregisteredFeature(PluginContext* ctx) {
    FeatureRef feats[1];
    feats[0].generator_id = "no_such_feature"; feats[0].params = nullptr; feats[0].param_count = 0;
    RecipeDesc d{};
    d.features = feats; d.feature_count = 1;
    ctx->register_recipe(ctx, "blocks", &d);
    return 0;
}

// Recipe whose interior distribution names a noise id that is not registered.
int initUnregisteredNoise(PluginContext* ctx) {
    MaterialWeight mats[1] = {{"stone", 1.0f}};
    RecipeDesc d{};
    d.interior.materials = mats; d.interior.material_count = 1;
    d.interior.noise_id  = "no_such_noise";
    ctx->register_recipe(ctx, "blocks", &d);
    return 0;
}

// Recipe referencing an unknown material id, but valid noise/features — material
// resolution is fail-soft, so this must validate cleanly.
int initUnknownMaterial(PluginContext* ctx) {
    MaterialWeight mats[1] = {{"unobtanium", 1.0f}};
    RecipeDesc d{};
    d.interior.materials = mats; d.interior.material_count = 1;
    d.interior.noise_id  = "value";  // built-in
    ctx->register_recipe(ctx, "blocks", &d);
    return 0;
}

int initOverrideValueNoise(PluginContext* ctx) {
    ctx->register_noise(ctx, "value", &constHalfNoise, nullptr);
    return 0;
}

}  // namespace

// ── Recipe registry ────────────────────────────────────────────────────────

TEST(RecipeRegistry, DeepCopiesFlatDescFromLocalArrays) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initFullRecipe);
    ASSERT_NE(id, kInvalidPluginId);

    const Recipe* r = pm.findRecipe("blocks");
    ASSERT_NE(r, nullptr);

    ASSERT_EQ(r->interior.materials.size(), 2u);
    EXPECT_EQ(r->interior.materials[0].material_id, "granite");
    EXPECT_FLOAT_EQ(r->interior.materials[0].weight, 0.7f);
    EXPECT_EQ(r->interior.materials[1].material_id, "basalt");
    EXPECT_EQ(r->interior.noise_id, "fbm");
    ASSERT_EQ(r->interior.noise_params.size(), 1u);
    EXPECT_EQ(r->interior.noise_params[0].key, "scale");
    EXPECT_DOUBLE_EQ(r->interior.noise_params[0].number, 16.0);

    ASSERT_EQ(r->features.size(), 1u);
    EXPECT_EQ(r->features[0].generator_id, "cave");

    EXPECT_TRUE(r->top.present);
    EXPECT_EQ(r->top.depth, 2);
    ASSERT_EQ(r->top.distribution.materials.size(), 1u);
    EXPECT_EQ(r->top.distribution.materials[0].material_id, "soil");
    EXPECT_FALSE(r->bottom.present);
    EXPECT_FALSE(r->side.present);

    ASSERT_EQ(r->seed_parameters.size(), 1u);
    EXPECT_EQ(r->seed_parameters[0].key, "cave_density");
    EXPECT_DOUBLE_EQ(r->seed_parameters[0].number, 0.5);
}

TEST(RecipeRegistry, UnregisteredLayerReturnsNull) {
    PluginManager pm;
    EXPECT_EQ(pm.findRecipe("blocks"), nullptr);
}

TEST(RecipeRegistry, OverwriteByLayerNameKeepsOneEntry) {
    PluginManager pm;
    ASSERT_NE(pm.wireInPlugin(initOverwriteRecipe), kInvalidPluginId);

    int count = 0;
    for (const auto& r : pm.recipes())
        if (r.layer_name == "blocks") ++count;
    EXPECT_EQ(count, 1);  // overwritten, not duplicated

    const Recipe* r = pm.findRecipe("blocks");
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(r->interior.materials.size(), 1u);
    EXPECT_EQ(r->interior.materials[0].material_id, "basalt");  // second wins
}

TEST(RecipeRegistry, UnloadTearsDownRecipe) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initFullRecipe);
    ASSERT_NE(id, kInvalidPluginId);
    ASSERT_NE(pm.findRecipe("blocks"), nullptr);

    ASSERT_TRUE(pm.unloadPlugin(id));
    EXPECT_EQ(pm.findRecipe("blocks"), nullptr);
}

// ── Noise registry ───────────────────────────────────────────────────────────

TEST(NoiseRegistry, BuiltinsResolveAndAreDeterministic) {
    PluginManager pm;
    pm.registerBuiltinNoise();

    RecipeParam params[1];
    params[0].key = "scale"; params[0].kind = RecipeParamKind::Number; params[0].number = 16.0;
    const WorldCoord p(10.0, 20.0, 30.0);

    for (const auto& b : noise::builtins()) {
        const RegisteredNoise* n = pm.resolveNoise(b.id);
        ASSERT_NE(n, nullptr) << "built-in '" << b.id << "' did not resolve";
        EXPECT_TRUE(n->isBuiltin);

        float a = n->fn(p, 7777u, params, 1, n->user_data);
        float c = n->fn(p, 7777u, params, 1, n->user_data);
        EXPECT_FLOAT_EQ(a, c) << "noise '" << b.id << "' is not deterministic";
        EXPECT_GE(a, 0.0f);
        EXPECT_LT(a, 1.0f) << "noise '" << b.id << "' left [0,1)";
    }
}

TEST(NoiseRegistry, DifferentSeedChangesValue) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    const RegisteredNoise* n = pm.resolveNoise("value");
    ASSERT_NE(n, nullptr);
    const WorldCoord p(3.0, 4.0, 5.0);
    float s1 = n->fn(p, 1u, nullptr, 0, n->user_data);
    float s2 = n->fn(p, 2u, nullptr, 0, n->user_data);
    EXPECT_NE(s1, s2);  // the seed actually threads through
}

TEST(NoiseRegistry, UnregisteredIdResolvesNull) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    EXPECT_EQ(pm.resolveNoise("definitely_not_registered"), nullptr);
}

TEST(NoiseRegistry, PluginOverridesBuiltinAndRestoresOnUnload) {
    PluginManager pm;
    pm.registerBuiltinNoise();

    const RegisteredNoise* before = pm.resolveNoise("value");
    ASSERT_NE(before, nullptr);
    EXPECT_TRUE(before->isBuiltin);

    PluginId id = pm.wireInPlugin(initOverrideValueNoise);
    ASSERT_NE(id, kInvalidPluginId);

    const RegisteredNoise* overridden = pm.resolveNoise("value");
    ASSERT_NE(overridden, nullptr);
    EXPECT_FALSE(overridden->isBuiltin);  // plugin wins over the built-in floor
    EXPECT_FLOAT_EQ(overridden->fn(WorldCoord(0.0, 0.0, 0.0), 0u, nullptr, 0, nullptr), 0.5f);

    ASSERT_TRUE(pm.unloadPlugin(id));
    const RegisteredNoise* after = pm.resolveNoise("value");
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(after->isBuiltin);  // built-in floor restored, never torn down
}

// ── Recipe validation ────────────────────────────────────────────────────────

TEST(RecipeValidation, AcceptsValidRecipe) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.wireInPlugin(initFullRecipe), kInvalidPluginId);
    EXPECT_NO_THROW(validateRecipes(compositeConfig(), pm));
}

TEST(RecipeValidation, CompositeWithNoRecipeResolvesToDefault) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    // No recipe registered for "blocks": resolves to the synthesized default.
    EXPECT_NO_THROW(validateRecipes(compositeConfig(), pm));
}

TEST(RecipeValidation, UnregisteredFeatureGeneratorIsError) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.wireInPlugin(initUnregisteredFeature), kInvalidPluginId);
    EXPECT_THROW(validateRecipes(compositeConfig(), pm), std::runtime_error);
}

TEST(RecipeValidation, UnregisteredNoiseIsError) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.wireInPlugin(initUnregisteredNoise), kInvalidPluginId);
    EXPECT_THROW(validateRecipes(compositeConfig(), pm), std::runtime_error);
}

TEST(RecipeValidation, UnknownMaterialFallsBackToNeutralDefault) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    ASSERT_NE(pm.wireInPlugin(initUnknownMaterial), kInvalidPluginId);

    // Material ids are fail-soft: validation accepts the recipe ...
    EXPECT_NO_THROW(validateRecipes(compositeConfig(), pm));
    // ... and the unknown id resolves to the neutral (zero) default.
    const MaterialProperties none = pm.material("unobtanium");
    EXPECT_FLOAT_EQ(none.density, 0.0f);
    EXPECT_FLOAT_EQ(none.hardness, 0.0f);
}

TEST(RecipeValidation, PassesAfterOwningPluginUnloads) {
    PluginManager pm;
    pm.registerBuiltinNoise();
    PluginId id = pm.wireInPlugin(initUnregisteredFeature);
    ASSERT_NE(id, kInvalidPluginId);
    EXPECT_THROW(validateRecipes(compositeConfig(), pm), std::runtime_error);

    // After the bad plugin unloads, its recipe is gone and "blocks" resolves to
    // the default again — validation passes.
    ASSERT_TRUE(pm.unloadPlugin(id));
    EXPECT_NO_THROW(validateRecipes(compositeConfig(), pm));
}
