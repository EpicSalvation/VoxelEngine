// base-terrain plugin — built as a real shared library (.so/.dylib/.dll) and
// loaded by PluginManager at runtime. Registers the materials and the single
// terminal-layer generator that produce the heightmap world.
//
// This is the disk-loaded counterpart of src/plugins/ExamplePlugin (which is
// compiled into the engine and wired in for the M3 demo). It is a deterministic
// value-noise heightmap that is a pure function of world (x, z), so streamed
// chunks regenerate identically.
//
// M16 (C2): the heightmap is now sampled from the engine's built-in "value"
// noise, resolved through ctx->resolve_noise at init, rather than a hand-rolled
// inline copy. The noise is a general engine facility (ARCHITECTURE §6); a plugin
// consumes it by id instead of re-implementing it.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

constexpr uint64_t kSeed       = 0x5DEECE66Dull;
constexpr double   kLattice    = 48.0;  // noise feature size, in voxels
constexpr int      kBaseHeight = 6;     // height (voxels) at noise value 0
constexpr int      kAmplitude  = 22;    // additional height range at noise value 1

// The built-in "value" noise, resolved at init (M16, C2). The generator runs on
// streaming threads but only reads this pointer after init has set it, and the
// engine's value noise is pure (no per-call user_data), so a plain function
// pointer is all the generator needs.
NoiseFn g_valueNoise = nullptr;

// Feature size threaded to the value noise as its "scale" param (world units).
const RecipeParam kNoiseParams[1] = {
    { "scale", RecipeParamKind::Number, kLattice, nullptr },
};

// Surface height in voxels at a world (x, z) column. Pure function of position so
// streamed chunks regenerate identically. Samples the value field on the y = 0
// plane: the heightmap is 2D, the noise facility is 3D, so we pin the third axis.
int terrainHeight(int64_t worldX, int64_t worldZ) {
    const float n = g_valueNoise(
        WorldCoord(static_cast<double>(worldX), 0.0, static_cast<double>(worldZ)),
        kSeed, kNoiseParams, 1, nullptr);
    return kBaseHeight + static_cast<int>(n * kAmplitude);
}

void base_layer_generator(
    WorldCoord chunk_origin, int grid_size, Voxel* out_voxels, void* /*user_data*/)
{
    MaterialProperties stone;
    stone.density              = 2700.0f;
    stone.structural_strength  = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness             = 0.7f;
    stone.palette_index        = 1;

    MaterialProperties grass;
    grass.density              = 1200.0f;
    grass.structural_strength  = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness             = 0.2f;
    grass.palette_index        = 2;

    const int64_t baseX = static_cast<int64_t>(std::llround(chunk_origin.value.x));
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    const int64_t baseZ = static_cast<int64_t>(std::llround(chunk_origin.value.z));

    for (int z = 0; z < grid_size; ++z) {
        for (int x = 0; x < grid_size; ++x) {
            int height = terrainHeight(baseX + x, baseZ + z);
            for (int y = 0; y < grid_size; ++y) {
                int64_t worldY = baseY + y;
                Voxel& v = out_voxels[x + grid_size * (y + grid_size * z)];
                if (worldY < 0 || worldY > height)
                    v = Voxel::empty();
                else if (worldY == height)
                    v.material = grass;
                else
                    v.material = stone;
            }
        }
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Consume the built-in "value" noise (M16, C2). Fail loudly if the registry
    // has no such id — the §6 contract is that an unknown id resolves to null.
    g_valueNoise = ctx->resolve_noise(ctx, "value");
    if (!g_valueNoise)
        return 1;

    MaterialProperties stone;
    stone.density              = 2700.0f;
    stone.structural_strength  = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness             = 0.7f;
    stone.palette_index        = 1;
    ctx->register_material(ctx, "stone", stone);

    MaterialProperties grass;
    grass.density              = 1200.0f;
    grass.structural_strength  = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness             = 0.2f;
    grass.palette_index        = 2;
    ctx->register_material(ctx, "grass", grass);

    ctx->register_layer_generator(ctx, "terrain", base_layer_generator, nullptr);
    return 0;
}
