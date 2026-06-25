// water plugin — the removable plugin for the M4 plugin-driven-world demo.
//
// Registers a single material ("water") and a single feature generator that
// floods the world up to a fixed sea level: every empty voxel at or below
// kSeaLevel becomes water. Because the fill depends only on world Y, the water
// surface is always perfectly level and seamless across chunk borders.
//
// Load it and flat blue water fills the valleys; unload it and the world is
// exactly the base-terrain heightmap again. This is the visible-on-removal
// behavior M4 calls for, with a single self-contained material+placement plugin.

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

// World-space height (in voxels / metres at 1 m terminal scale) of the water
// surface. Base terrain heights span 6..28, so this floods the lower valleys
// while leaving the higher ground dry — obvious whether or not it is in use.
constexpr int kSeaLevel = 15;

void water_feature_generator(
    WorldCoord         chunk_origin,
    double             /*voxel_size_m*/,
    int                grid_size,
    Voxel*             inout_voxels,
    const RecipeParam* /*params*/,
    size_t             /*param_count*/,
    uint64_t           /*seed*/,
    void*              /*user_data*/)
{
    MaterialProperties water;
    water.density              = 1000.0f;
    water.structural_strength  = 0.0f;
    water.thermal_conductivity = 0.6f;
    water.porosity             = 1.0f;
    water.hardness             = 0.0f;
    water.palette_index        = 5;  // palette index 5 = water (blue)

    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));

    for (int z = 0; z < grid_size; ++z) {
        for (int y = 0; y < grid_size; ++y) {
            const int64_t worldY = baseY + y;
            if (worldY < 0 || worldY > kSeaLevel)
                continue;  // above the surface or below the world floor: untouched
            for (int x = 0; x < grid_size; ++x) {
                Voxel& v = inout_voxels[x + grid_size * (y + grid_size * z)];
                if (v.isEmpty())            // only fill open space; never replace terrain
                    v.material = water;
            }
        }
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    MaterialProperties water;
    water.density              = 1000.0f;
    water.structural_strength  = 0.0f;
    water.thermal_conductivity = 0.6f;
    water.porosity             = 1.0f;
    water.hardness             = 0.0f;
    water.palette_index        = 5;
    ctx->register_material(ctx, "water", water);

    ctx->register_feature_generator(ctx, "water_table", water_feature_generator, nullptr);
    return 0;
}
