// ===========================================================================
// world-plugin.cpp — world-generation plugin template
//
// A plugin defines your game's CONTENT: materials and the procedural terrain
// that fills the world. The engine links zero of your symbols and you link zero
// of the engine's (ARCHITECTURE §12) — you see only include/plugin_api.h plus
// the in-tree Voxel definition a generator fills. The engine hands you a table
// of function pointers (PluginContext); you register callbacks on it.
//
// Copy this file to plugins/<yourname>/plugin.cpp. The build auto-discovers any
// plugins/<dir>/plugin.cpp and compiles it to a runtime-loadable module — no
// CMake edits needed. Then point your game's VOXEL_<NAME>_PLUGIN_PATH define at
// the artifact (CMakeLists.txt) and load it with PluginManager::loadPlugin.
//
// THE ONE RULE THAT MATTERS: generators must be PURE. A generator is a pure
// function of (world position, seed) — no rand(), no time(), no global mutable
// state, no I/O. Streamed chunks are generated and re-generated on demand as the
// player moves, so the same position MUST always yield the same voxels or the
// world will flicker and de-sync. Use the header-only deterministic RNG
// (voxel_rng_*, voxel_seed_mix) and the engine's built-in noise instead.
//
// References: plugins/base-terrain (the minimal heightmap), plugins/recipe-world
// (the full composition-recipe showcase), tutorials 02-05.
// ===========================================================================

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>

// Every runtime-loadable plugin exports its init symbol and stamps its ABI
// version. The engine reads the stamp before calling init and refuses to load a
// plugin built against a mismatched ABI (rather than crashing). These two lines
// are boilerplate — keep them exactly as-is.
#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// ---------------------------------------------------------------------------
// Palette slots. Every voxel carries a palette_index (0-255) the renderer uses
// to colour it. Name your slots so the generator and init agree. We install the
// actual ABGR colours in init via set_palette_color.
// ---------------------------------------------------------------------------
constexpr uint8_t kStoneIdx = 1;
constexpr uint8_t kDirtIdx  = 2;
constexpr uint8_t kGrassIdx = 3;

// ---------------------------------------------------------------------------
// Material factory. A material is just a filled MaterialProperties struct — no
// block-type enum, no engine changes. Simulation systems READ these fields and
// respond to them (see tutorial 03):
//   density              -> physics mass / structural load
//   structural_strength  -> collapse resistance (PropagationSystem, tutorial 13)
//   hardness             -> mining time / removal resistance
//   thermal_conductivity -> heat spread (ThermalSystem, tutorial 12)
//   porosity             -> fluid permeability (FluidSystem, tutorial 12)
//   light_emission       -> block light [0,1] (LightingSystem, tutorial 12)
//   palette_index        -> visual colour (renderer)
// ---------------------------------------------------------------------------
MaterialProperties makeMaterial(uint8_t paletteIndex, float density,
                                float strength, float hardness) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = strength;
    m.hardness            = hardness;
    m.palette_index       = paletteIndex;
    return m;
}

// ---------------------------------------------------------------------------
// Built-in "value" noise, resolved once at init. The engine ships value / fbm /
// ridged / worley noise; a plugin consumes one by id rather than hand-rolling
// its own (M16). It is pure and ignores user_data, so a bare function pointer is
// all the generator needs. (If you'd rather author your own, register_noise lets
// you add or override an id — see tutorial 04.)
// ---------------------------------------------------------------------------
NoiseFn g_valueNoise = nullptr;

constexpr uint64_t kSeed       = 0x5DEECE66Dull;  // your world's master seed
constexpr double   kFeatureM   = 48.0;            // terrain feature size, meters
constexpr int      kBaseHeight = 6;               // surface height at noise=0
constexpr int      kAmplitude  = 22;              // extra height range at noise=1

// Noise scale param (world units), passed to the value noise each sample.
const RecipeParam kNoiseParams[1] = {
    { "scale", RecipeParamKind::Number, kFeatureM, nullptr },
};

// Surface height (in voxels) at a world column (x, z). Pure function of position
// — the same column always returns the same height, so adjacent and re-streamed
// chunks line up seamlessly. The heightmap is 2D; we pin y=0 when sampling the
// 3D noise field.
int terrainHeight(int64_t worldX, int64_t worldZ) {
    const float n = g_valueNoise(
        WorldCoord(static_cast<double>(worldX), 0.0, static_cast<double>(worldZ)),
        kSeed, kNoiseParams, 1, nullptr);
    return kBaseHeight + static_cast<int>(n * kAmplitude);
}

// ---------------------------------------------------------------------------
// The layer generator. The engine calls this to fill one chunk's voxel grid
// from scratch.
//   chunk_origin — world-space corner of this chunk
//   grid_size    — voxels per side; grid is grid_size^3, row-major, x-fastest
//   out_voxels   — flat array of grid_size^3 Voxels to populate
//
// Index a voxel at local (x, y, z) as out[x + grid_size * (y + grid_size * z)].
// ---------------------------------------------------------------------------
void terrain_generator(WorldCoord chunk_origin, int grid_size,
                       Voxel* out_voxels, void* /*user_data*/) {
    const MaterialProperties stone = makeMaterial(kStoneIdx, 2700.0f, 0.9f, 0.7f);
    const MaterialProperties dirt  = makeMaterial(kDirtIdx,  1500.0f, 0.4f, 0.3f);
    const MaterialProperties grass = makeMaterial(kGrassIdx, 1200.0f, 0.3f, 0.2f);

    const int64_t baseX = static_cast<int64_t>(std::llround(chunk_origin.value.x));
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    const int64_t baseZ = static_cast<int64_t>(std::llround(chunk_origin.value.z));

    for (int z = 0; z < grid_size; ++z) {
        for (int x = 0; x < grid_size; ++x) {
            const int height = terrainHeight(baseX + x, baseZ + z);
            for (int y = 0; y < grid_size; ++y) {
                const int64_t worldY = baseY + y;
                Voxel& v = out_voxels[x + grid_size * (y + grid_size * z)];
                if (worldY < 0 || worldY > height) {
                    v = Voxel::empty();              // air above the surface / below 0
                } else if (worldY == height) {
                    v.material = grass;              // grass on top
                } else if (worldY >= height - 3) {
                    v.material = dirt;               // a few layers of dirt
                } else {
                    v.material = stone;              // stone underneath
                }
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Plugin init. Called once at load. Register everything here. Return 0 on
// success; any non-zero return aborts the load with a diagnostic.
// ---------------------------------------------------------------------------
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Resolve the built-in noise we sample in the generator. Fail loudly if the
    // id is unknown (the §6 contract: unknown id -> nullptr).
    g_valueNoise = ctx->resolve_noise(ctx, "value");
    if (!g_valueNoise) return 1;

    // Register materials by id. The id is how recipes and audio/texture bindings
    // refer to the material; the engine resolves it to a palette_index.
    ctx->register_material(ctx, "stone", makeMaterial(kStoneIdx, 2700.0f, 0.9f, 0.7f));
    ctx->register_material(ctx, "dirt",  makeMaterial(kDirtIdx,  1500.0f, 0.4f, 0.3f));
    ctx->register_material(ctx, "grass", makeMaterial(kGrassIdx, 1200.0f, 0.3f, 0.2f));

    // Install palette colours (ABGR = 0xAABBGGRR). Alpha < 0xff is translucent.
    ctx->set_palette_color(ctx, kStoneIdx, 0xff808080);  // grey
    ctx->set_palette_color(ctx, kDirtIdx,  0xff3060a0);  // brown (B=a0,G=60,R=30)
    ctx->set_palette_color(ctx, kGrassIdx, 0xff40c040);  // green

    // Register the generator for the layer named in world.yaml.
    ctx->register_layer_generator(ctx, "terrain", terrain_generator, nullptr);

    // -----------------------------------------------------------------------
    // OPTIONAL: composition recipe for a COMPOSITE layer.
    //
    // If your world.yaml has a composite layer that decomposes into a finer one,
    // attach a recipe describing how each macro voxel fills itself on approach:
    // an interior material distribution, per-face boundary overrides (e.g. a
    // grass cap), and ordered feature overlays (caves, ore). See recipe-world
    // and tutorials 04-05. Sketch:
    //
    //   MaterialWeight interior[2] = {{"stone", 0.8f}, {"dirt", 0.2f}};
    //   RecipeDesc recipe{};
    //   recipe.interior.materials      = interior;
    //   recipe.interior.material_count = 2;
    //   recipe.interior.noise_id       = "value";
    //   recipe.top.present             = true;   // grass cap on the +Y face
    //   recipe.top.depth               = 1;
    //   MaterialWeight cap[1] = {{"grass", 1.0f}};
    //   recipe.top.distribution.materials      = cap;
    //   recipe.top.distribution.material_count = 1;
    //   ctx->register_recipe(ctx, "<composite-layer-name>", &recipe);
    // -----------------------------------------------------------------------

    return 0;
}
