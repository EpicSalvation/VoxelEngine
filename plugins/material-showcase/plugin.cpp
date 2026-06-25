// material-showcase plugin — the M8 "material matters" world.
//
// Registers a stack of materials whose only meaningful difference is their
// `hardness` (with deliberately different `density`/`structural_strength` so the
// HUD readout varies too), and a single terminal-layer generator that lays them
// down as flat horizontal strata. Digging straight down passes through soft
// topsoil, progressively harder rock, and finally an indestructible bedrock
// floor that no tool can clear.
//
// This is the reference pattern for per-material indestructibility (M8): a
// material with the sentinel `hardness < 0` is refused by the removal tool
// (sim::RemovalModel / RemovalAccumulator) without any block-type branch — the
// removal path reads the voxel's own `hardness` by value (ARCHITECTURE.md §5).
// It is distinct from whole-layer `VoxelMode::immutable`: bedrock lives in the
// ordinary editable terminal layer; it just can't be removed.
//
// Generation is a pure function of world Y only (no rand/time/unordered
// iteration), so streamed chunks regenerate identically.

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

// Indestructibility sentinel — any negative hardness (matches
// sim::kIndestructible in src/simulation/RemovalModel.h). Kept as a literal so
// the plugin stays self-contained and links zero engine symbols.
constexpr float kIndestructible = -1.0f;

// The strata, top-of-world to bottom. Each is a distinct material whose hardness
// climbs so the player feels each take longer to mine, ending in indestructible
// bedrock. palette indices map to the visual base palette (src/renderer/Palette.h).
MaterialProperties makeMaterial(float density, float structural,
                                float hardness, uint8_t palette) {
    MaterialProperties m{};
    m.density              = density;
    m.structural_strength  = structural;
    m.thermal_conductivity = 1.0f;
    m.porosity             = 0.0f;
    m.hardness             = hardness;
    m.palette_index        = palette;
    return m;
}

MaterialProperties grass()   { return makeMaterial(1200.0f, 0.3f, 0.2f, 2);  }  // green
MaterialProperties dirt()    { return makeMaterial(1500.0f, 0.4f, 0.4f, 3);  }  // brown
MaterialProperties stone()   { return makeMaterial(2700.0f, 0.9f, 0.7f, 1);  }  // gray
MaterialProperties iron()    { return makeMaterial(7800.0f, 1.2f, 1.5f, 11); }  // blue-gray
MaterialProperties diamond() { return makeMaterial(3500.0f, 2.0f, 3.0f, 13); }  // cyan
MaterialProperties bedrock() { return makeMaterial(5000.0f, 9.9f, kIndestructible, 10); }  // near-black

// Material for a given world Y. Flat strata that climb in hardness as you dig
// down, ending in an indestructible bedrock floor. The whole stack lives in
// world y [0, 31] — one 32-voxel chunk row (chunk-Y 0) — so it streams under the
// demo's absolute vertical band of chunk-Y 0 (LODManager::desiredChunks). Air
// fills the rest of that chunk above the surface; y < 0 (chunk-Y -1) is never
// streamed and the indestructible bedrock at y=0..1 caps the dig there.
Voxel strataAt(int64_t worldY) {
    if (worldY >= 24)  return Voxel::empty();  // air (y 24..31)
    if (worldY == 23)  return Voxel{grass()};   // topsoil
    if (worldY >= 20)  return Voxel{dirt()};     // y 20..22
    if (worldY >= 14)  return Voxel{stone()};    // y 14..19
    if (worldY >= 8)   return Voxel{iron()};     // y 8..13
    if (worldY >= 2)   return Voxel{diamond()};  // y 2..7
    return Voxel{bedrock()};                     // y 0..1 — indestructible floor
}

void strata_generator(WorldCoord chunk_origin, int grid_size, Voxel* out_voxels,
                      void* /*user_data*/) {
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    for (int z = 0; z < grid_size; ++z) {
        for (int y = 0; y < grid_size; ++y) {
            const Voxel v = strataAt(baseY + y);
            for (int x = 0; x < grid_size; ++x)
                out_voxels[x + grid_size * (y + grid_size * z)] = v;
        }
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Register all strata materials by id so the build menu and HUD can resolve
    // them through PluginManager::material() (tooling lookup, ARCHITECTURE §5).
    ctx->register_material(ctx, "grass",   grass());
    ctx->register_material(ctx, "dirt",    dirt());
    ctx->register_material(ctx, "stone",   stone());
    ctx->register_material(ctx, "iron",    iron());
    ctx->register_material(ctx, "diamond", diamond());
    ctx->register_material(ctx, "bedrock", bedrock());  // hardness < 0: indestructible

    ctx->register_layer_generator(ctx, "terrain", strata_generator, nullptr);
    return 0;
}
