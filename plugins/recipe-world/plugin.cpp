// recipe-world plugin — the M9 composition-recipe showcase, driving demo 09.
//
// Where layered-world (demo 05) decomposed a macro voxel by simply re-running a
// child generator over its subvolume, recipe-world attaches a real composition
// RECIPE to its composite "blocks" layer:
//
//   - interior material distribution — granite-dominant with basalt, arranged
//     by the built-in "value" noise field
//   - top boundary override — a two-voxel SOIL cap on the macro voxel's upper
//     face, distinct from the stony interior
//   - feature overlays, in order — a CAVE-network generator that carves connected
//     voids, then an ORE-vein generator that replaces granite pockets with iron
//   - a seed parameter — "cave_density" — biasing the layer below; demo 09
//     toggles it to visibly change cave density while each value regenerates
//     identically (determinism)
//
// The two feature generators are plain pure functions of (world position, seed)
// — no rand/time/global state — registered by id so the recipe can reference
// them by name. They implement their own inline value noise: a plugin links zero
// engine symbols (ARCHITECTURE §12), so it cannot call src/world/Noise.cpp.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// Palette slots (see renderer/Palette.h).
constexpr uint8_t kGraniteIdx = 1;   // gray   — interior bulk
constexpr uint8_t kBasaltIdx  = 10;  // near-black — interior accent
constexpr uint8_t kSoilIdx    = 3;   // brown  — top boundary cap
constexpr uint8_t kIronIdx    = 11;  // blue-gray — ore veins
constexpr uint8_t kBlockIdx   = 1;   // undecomposed macro blocks render as gray stone
constexpr uint8_t kBedrockIdx = 14;  // dark red — immutable floor under the world

// Macro "blocks" voxel size (8 m) and bedrock voxel size (2 m), handed to their
// generators through user_data (the LayerGeneratorFn signature carries no voxel
// size).
const double kBlocksVoxelSizeM  = 8.0;
const double kBedrockVoxelSizeM = 2.0;

// The composite ground is a solid slab from y=0 up to this height (world m).
constexpr double kGroundTopM = 24.0;

// The immutable bedrock floor occupies world Y in [kBedrockBottomM, 0): a solid
// slab flush with the bottom of the terrain, so a player who digs or falls
// through a cave at the bottom of the slab lands on it instead of the void.
constexpr double kBedrockBottomM = -6.0;

MaterialProperties solid(uint8_t paletteIndex, float density, float hardness) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = 0.8f;
    m.hardness            = hardness;
    m.palette_index       = paletteIndex;
    return m;
}

double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// ── Inline 3D value noise (plugin-local; see file header) ────────────────────
uint64_t splitmix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double lattice(int64_t ix, int64_t iy, int64_t iz, uint64_t seed) {
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(ix) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(iy) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(iz) * 0x165667B19E3779F9ull;
    h = splitmix(h);
    return static_cast<double>(h >> 40) / static_cast<double>(1ull << 24);  // [0,1)
}

double smoother(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Trilinearly-interpolated value noise at a world position; frequency = 1/scale.
double value3(double x, double y, double z, uint64_t seed, double frequency) {
    const double fx = x * frequency, fy = y * frequency, fz = z * frequency;
    const int64_t ix = static_cast<int64_t>(std::floor(fx));
    const int64_t iy = static_cast<int64_t>(std::floor(fy));
    const int64_t iz = static_cast<int64_t>(std::floor(fz));
    const double tx = smoother(fx - ix), ty = smoother(fy - iy), tz = smoother(fz - iz);

    const double c000 = lattice(ix,     iy,     iz,     seed);
    const double c100 = lattice(ix + 1, iy,     iz,     seed);
    const double c010 = lattice(ix,     iy + 1, iz,     seed);
    const double c110 = lattice(ix + 1, iy + 1, iz,     seed);
    const double c001 = lattice(ix,     iy,     iz + 1, seed);
    const double c101 = lattice(ix + 1, iy,     iz + 1, seed);
    const double c011 = lattice(ix,     iy + 1, iz + 1, seed);
    const double c111 = lattice(ix + 1, iy + 1, iz + 1, seed);

    const double x00 = lerp(c000, c100, tx);
    const double x10 = lerp(c010, c110, tx);
    const double x01 = lerp(c001, c101, tx);
    const double x11 = lerp(c011, c111, tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);  // [0,1)
}

// ── Feature generator: cave network ──────────────────────────────────────────
// Carves connected voids out of the solid grid where a coherent 3D value-noise
// field falls below "cave_density" (0..1). Higher density => more void; 0 carves
// nothing, 1 carves everything. "scale" sets the cave feature size in meters.
// Only solid voxels are carved (empty space is left alone), so the void fraction
// is measured against the rock the recipe placed. Pure in (position, seed).
void cave_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                  const RecipeParam* params, size_t count, uint64_t seed, void*) {
    double density = recipe_param_num(params, count, "cave_density", 0.35);
    double scale   = recipe_param_num(params, count, "scale", 12.0);
    density = std::clamp(density, 0.0, 1.0);
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 12.0);

    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.isEmpty()) continue;  // only carve rock
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wy = origin.value.y + (y + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                if (value3(wx, wy, wz, seed, freq) < density)
                    v = Voxel::empty();
            }
}

// ── Feature generator: ore vein ──────────────────────────────────────────────
// Replaces pockets of a TARGET material with iron ore where a second value-noise
// field falls below "ore_richness". "target_palette" (default granite) restricts
// replacement to that material, so basalt accents and soil caps are left intact —
// the ore threads only through the granite bulk. Pure in (position, seed).
void ore_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                 const RecipeParam* params, size_t count, uint64_t seed, void*) {
    double richness = recipe_param_num(params, count, "ore_richness", 0.15);
    double scale    = recipe_param_num(params, count, "scale", 6.0);
    richness = std::clamp(richness, 0.0, 1.0);
    const auto target = static_cast<uint8_t>(
        recipe_param_num(params, count, "target_palette", kGraniteIdx));
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 6.0);
    const MaterialProperties ore = solid(kIronIdx, 5200.0f, 0.85f);

    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.isEmpty()) continue;
                if (v.material.palette_index != target) continue;  // only the targeted rock
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wy = origin.value.y + (y + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                if (value3(wx, wy, wz, seed, freq) < richness)
                    v.material = ore;
            }
}

// ── Composite "blocks" layer generator ───────────────────────────────────────
// A solid ground slab of macro voxels from y=0 to kGroundTopM. Each macro voxel
// is solid iff its vertical span overlaps the slab — undecomposed it renders as a
// gray stone block; on approach (or click) the recipe decomposes it into the
// detailed granite/basalt/soil/cave/ore interior.
void blocks_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const double vs = voxelSizeFrom(user_data);  // 8.0
    const MaterialProperties block = solid(kBlockIdx, 2400.0f, 0.6f);
    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                const double bottom = chunk_origin.value.y + y * vs;
                const double top    = bottom + vs;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                v = (top > 0.0 && bottom < kGroundTopM) ? Voxel{block} : Voxel::empty();
            }
}

// ── Immutable "bedrock" layer generator ──────────────────────────────────────
// A solid floor slab under the world, generated once and retained (immutable:
// never edited, persisted, or decomposed). It catches a player who digs or falls
// through the bottom of the terrain so they never drop into the void.
void bedrock_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const double vs = voxelSizeFrom(user_data);  // 2.0
    const MaterialProperties bedrock = solid(kBedrockIdx, 4000.0f, 1.0f);
    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                const double bottom = chunk_origin.value.y + y * vs;
                const double top    = bottom + vs;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                v = (top > kBedrockBottomM && bottom < 0.0) ? Voxel{bedrock} : Voxel::empty();
            }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_material(ctx, "granite",  solid(kGraniteIdx, 2700.0f, 0.6f));
    ctx->register_material(ctx, "basalt",   solid(kBasaltIdx,  3000.0f, 0.7f));
    ctx->register_material(ctx, "soil",     solid(kSoilIdx,    1300.0f, 0.2f));
    ctx->register_material(ctx, "iron-ore", solid(kIronIdx,    5200.0f, 0.85f));
    ctx->register_material(ctx, "bedrock",  solid(kBedrockIdx, 4000.0f, 1.0f));

    ctx->register_layer_generator(ctx, "blocks", blocks_generator,
                                  const_cast<double*>(&kBlocksVoxelSizeM));
    ctx->register_layer_generator(ctx, "bedrock", bedrock_generator,
                                  const_cast<double*>(&kBedrockVoxelSizeM));

    ctx->register_feature_generator(ctx, "cave", cave_feature, nullptr);
    ctx->register_feature_generator(ctx, "ore",  ore_feature,  nullptr);

    // ── The composition recipe for "blocks" (deep-copied at registration) ─────
    MaterialWeight interiorMats[2] = {{"granite", 0.8f}, {"basalt", 0.2f}};
    RecipeParam interiorNoise[1];
    interiorNoise[0].key = "scale"; interiorNoise[0].kind = RecipeParamKind::Number;
    interiorNoise[0].number = 8.0;

    MaterialWeight topMats[1] = {{"soil", 1.0f}};

    RecipeParam caveParams[1];
    caveParams[0].key = "scale"; caveParams[0].kind = RecipeParamKind::Number;
    caveParams[0].number = 12.0;

    RecipeParam oreParams[3];
    oreParams[0].key = "scale";         oreParams[0].kind = RecipeParamKind::Number; oreParams[0].number = 6.0;
    oreParams[1].key = "ore_richness";  oreParams[1].kind = RecipeParamKind::Number; oreParams[1].number = 0.20;
    oreParams[2].key = "target_palette"; oreParams[2].kind = RecipeParamKind::Number; oreParams[2].number = kGraniteIdx;

    FeatureRef feats[2];
    feats[0].generator_id = "cave"; feats[0].params = caveParams; feats[0].param_count = 1;
    feats[1].generator_id = "ore";  feats[1].params = oreParams;  feats[1].param_count = 3;

    // Parent seed parameter: biases the child grid. Demo 09 toggles this value.
    RecipeParam seedParams[1];
    seedParams[0].key = "cave_density"; seedParams[0].kind = RecipeParamKind::Number;
    seedParams[0].number = 0.45;

    RecipeDesc recipe{};
    recipe.interior.materials         = interiorMats;
    recipe.interior.material_count    = 2;
    recipe.interior.noise_id          = "value";
    recipe.interior.noise_params      = interiorNoise;
    recipe.interior.noise_param_count = 1;
    recipe.features                   = feats;
    recipe.feature_count              = 2;
    recipe.top.present                = true;
    recipe.top.depth                  = 2;
    recipe.top.distribution.materials      = topMats;
    recipe.top.distribution.material_count = 1;
    recipe.seed_parameters            = seedParams;
    recipe.seed_parameter_count       = 1;

    ctx->register_recipe(ctx, "blocks", &recipe);
    return 0;
}
