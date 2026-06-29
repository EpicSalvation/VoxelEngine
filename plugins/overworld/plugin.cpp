// overworld plugin — the worldgen heart of the M18 mega-demo.
//
// A Minecraft-lite surface world built as a single TERMINAL layer plus
// composable feature overlays, the demo-03 (plugin-driven-world) pattern: the
// host fills each chunk with this plugin's "terrain" layer generator, then
// applies every registered feature generator in turn (caves and ore here; water
// and trees from the water/trees plugins). Nothing decomposes — the surface is a
// real rolling heightmap, not a flat slab — so the world reads as terrain at a
// glance while still showing cross-plugin composition (the recipe-world demo
// already owns the decomposition story).
//
// Determinism (the M18 seed subtask): every height, cave, and ore lookup is a
// pure function of world position and the run's world seed. The host passes the
// seed as the layer generator's user_data and as the `seed` argument to the
// feature pass, so the SAME seed regenerates the SAME world byte-for-byte
// (ARCHITECTURE §4). No rand/time/global mutable state.
//
// Materials carry real MaterialProperties (M8) so the rest of the engine reacts
// to them without a block-type table: structural_strength drives M13 collapse,
// porosity gates M14 fluid, palette_index drives color (and the M15 face-texture
// bindings set up below). Textures are bound by palette_index via
// set_material_faces; the host writes the referenced PNG tiles and builds the
// atlas, so an audio-only/headless host simply renders flat colors (fail-soft).

#include "plugin_api.h"
#include "world/Voxel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// ── Palette slots (see renderer/Palette.h) ───────────────────────────────────
constexpr uint8_t kStoneIdx   = 1;   // gray  — bulk underground
constexpr uint8_t kGrassIdx   = 2;   // green — surface cap
constexpr uint8_t kDirtIdx    = 3;   // brown — subsoil
constexpr uint8_t kLeavesIdx  = 4;   // dark green — tree canopy (trees plugin places it)
constexpr uint8_t kWaterIdx   = 5;   // blue  — owned by the water plugin; colored here
constexpr uint8_t kSandIdx    = 6;   // tan   — shoreline / lakebed
constexpr uint8_t kLogIdx     = 8;   // bark  — tree trunk (trees plugin places it)
constexpr uint8_t kIronIdx    = 11;  // blue-gray — ore veins
constexpr uint8_t kBedrockIdx = 14;  // dark  — immutable world floor

// ── World shape knobs (world metres, 1 m terminal voxels) ─────────────────────
constexpr double kBaseHeight = 22.0;  // mean surface height
constexpr double kAmplitude  = 9.0;   // +/- relief, so the surface spans ~13..31
constexpr int    kSeaLevel   = 15;    // MUST match the water plugin's sea level
constexpr int    kDirtDepth  = 3;     // grass cap then this many dirt voxels, then stone
constexpr int    kBedrockTop = 1;     // y in [0, kBedrockTop) is immutable bedrock

MaterialProperties material(uint8_t palette, float density, float strength, float hardness,
                            float porosity = 0.0f) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = strength;
    m.hardness            = hardness;
    m.porosity            = porosity;
    m.palette_index       = palette;
    return m;
}

// ── Inline value noise (plugin-local: a plugin links zero engine symbols, so it
// cannot call src/world/Noise.cpp — ARCHITECTURE §12). Identical lattice/hash to
// recipe-world so behaviour matches the rest of the engine's worldgen. ─────────
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

// Four-octave fractal value noise over the (x,z) plane, in [0,1).
double fbm2(double x, double z, uint64_t seed) {
    double sum = 0.0, norm = 0.0, amp = 0.5, freq = 1.0 / 56.0;
    for (int o = 0; o < 4; ++o) {
        sum  += amp * value3(x, 0.0, z, seed + static_cast<uint64_t>(o) * 1013u, freq);
        norm += amp;
        amp  *= 0.5;
        freq *= 2.0;
    }
    return sum / norm;  // [0,1)
}

// Surface height (integer world Y of the topmost solid voxel) at world (x,z).
// Shared by the terrain generator and the cave generator so caves stay buried.
int surfaceHeight(double wx, double wz, uint64_t seed) {
    const double h = kBaseHeight + (fbm2(wx, wz, seed) - 0.5) * 2.0 * kAmplitude;
    return static_cast<int>(std::llround(h));
}

uint64_t seedFrom(void* user_data) {
    return user_data ? *static_cast<const uint64_t*>(user_data) : 0x9E3779B97F4A7C15ull;
}

// ── Layer generator: the rolling surface world ───────────────────────────────
// Fills a terminal chunk from a heightmap: bedrock floor, a stone bulk, a few
// dirt voxels of subsoil, and a grass (or sand, near sea level) cap. Air above.
void terrain_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const uint64_t seed = seedFrom(user_data);
    const MaterialProperties grass   = material(kGrassIdx,   1500.0f, 0.35f, 0.30f);
    const MaterialProperties dirt    = material(kDirtIdx,    1300.0f, 0.30f, 0.25f);
    const MaterialProperties stone   = material(kStoneIdx,   2700.0f, 0.85f, 0.70f);
    const MaterialProperties sand    = material(kSandIdx,    1600.0f, 0.15f, 0.20f, 0.30f);
    const MaterialProperties bedrock = material(kBedrockIdx, 4000.0f, 1.00f, 1.00f);

    const int64_t baseX = static_cast<int64_t>(std::llround(chunk_origin.value.x));
    const int64_t baseY = static_cast<int64_t>(std::llround(chunk_origin.value.y));
    const int64_t baseZ = static_cast<int64_t>(std::llround(chunk_origin.value.z));

    for (int z = 0; z < grid_size; ++z)
        for (int x = 0; x < grid_size; ++x) {
            const double wx = static_cast<double>(baseX + x) + 0.5;
            const double wz = static_cast<double>(baseZ + z) + 0.5;
            const int h = surfaceHeight(wx, wz, seed);
            const bool shore = h <= kSeaLevel + 1;  // grass turns to sand at the water's edge
            for (int y = 0; y < grid_size; ++y) {
                const int64_t wy = baseY + y;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                if (wy < kBedrockTop)            v = Voxel{bedrock};
                else if (wy > h)                 v = Voxel::empty();
                else if (wy == h)                v = Voxel{shore ? sand : grass};
                else if (wy >= h - kDirtDepth)   v = Voxel{shore ? sand : dirt};
                else                             v = Voxel{stone};
            }
        }
}

// ── Feature generator: caves ─────────────────────────────────────────────────
// Carves connected voids out of the stony bulk where a coherent 3D value-noise
// field falls below a depth-ramped density: sparse just under the surface,
// heavier deep down. Caves stay buried (never within kSurfaceMargin of the
// column's surface) so the ground never reads as swiss cheese from above, and
// never break the bedrock floor. Pure in (world position, seed).
void cave_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                  const RecipeParam* params, size_t count, uint64_t seed, void*) {
    constexpr int kSurfaceMargin = 3;
    const double surfaceDensity = recipe_param_num(params, count, "cave_density_surface", 0.06);
    const double depthDensity   = recipe_param_num(params, count, "cave_density_depth", 0.12);
    const double scale          = recipe_param_num(params, count, "scale", 9.0);
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 9.0);
    const uint64_t caveSeed = voxel_seed_mix(seed, 0xCA7E5u);

    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const double wx = origin.value.x + (x + 0.5) * vs;
            const double wz = origin.value.z + (z + 0.5) * vs;
            const int surf = surfaceHeight(wx, wz, seed);
            for (int y = 0; y < n; ++y) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.isEmpty()) continue;                       // only carve solid
                if (v.material.palette_index == kBedrockIdx) continue;  // never the floor
                const double wy = origin.value.y + (y + 0.5) * vs;
                const int iy = static_cast<int>(std::floor(wy));
                if (iy > surf - kSurfaceMargin || iy < kBedrockTop) continue;  // keep caves buried
                const double depthFrac =
                    std::clamp(static_cast<double>(surf - iy) / std::max(1.0, kBaseHeight), 0.0, 1.0);
                const double density =
                    std::clamp(surfaceDensity + (depthDensity - surfaceDensity) * depthFrac, 0.0, 1.0);
                if (value3(wx, wy, wz, caveSeed, freq) < density)
                    v = Voxel::empty();
            }
        }
}

// ── Feature generator: ore veins ─────────────────────────────────────────────
// Replaces pockets of stone with iron ore where a second value-noise field is
// rich, restricted to deeper rock so ore is a reward for digging. Pure.
void ore_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                 const RecipeParam* params, size_t count, uint64_t seed, void*) {
    const double richness = std::clamp(recipe_param_num(params, count, "ore_richness", 0.14), 0.0, 1.0);
    const double scale    = recipe_param_num(params, count, "scale", 6.0);
    const double freq = (scale > 0.0) ? (1.0 / scale) : (1.0 / 6.0);
    const uint64_t oreSeed = voxel_seed_mix(seed, 0x0BEull);
    const MaterialProperties ore = material(kIronIdx, 5200.0f, 0.9f, 0.85f);

    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                Voxel& v = inout[x + n * (y + n * z)];
                if (v.material.palette_index != kStoneIdx) continue;  // only the stony bulk
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wy = origin.value.y + (y + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                if (value3(wx, wy, wz, oreSeed, freq) < richness)
                    v.material = ore;
            }
}

// Bind a material's faces to atlas tiles by texture id (M15). Fail-soft if the
// host attached no texture pipeline. one tile per world metre (tiling_factor 1).
void bindFaces(PluginContext* ctx, uint8_t palette,
               const char* top, const char* bottom, const char* side) {
    ctx->set_material_faces(ctx, palette, top, bottom, side, 1.0f);
}

}  // namespace

// Named entry point so the determinism test can compile this plugin in (sharing
// the generators) alongside other plugins without a voxel_plugin_init collision.
VOXEL_PLUGIN_EXPORT int overworld_plugin_init(PluginContext* ctx) {
    // Materials (M8). Strength/porosity are read by the M13/M14 systems directly.
    ctx->register_material(ctx, "grass",    material(kGrassIdx,   1500.0f, 0.35f, 0.30f));
    ctx->register_material(ctx, "dirt",     material(kDirtIdx,    1300.0f, 0.30f, 0.25f));
    ctx->register_material(ctx, "stone",    material(kStoneIdx,   2700.0f, 0.85f, 0.70f));
    ctx->register_material(ctx, "sand",     material(kSandIdx,    1600.0f, 0.15f, 0.20f, 0.30f));
    ctx->register_material(ctx, "log",      material(kLogIdx,      600.0f, 0.60f, 0.35f));
    ctx->register_material(ctx, "leaves",   material(kLeavesIdx,   200.0f, 0.05f, 0.10f, 0.40f));
    ctx->register_material(ctx, "iron-ore", material(kIronIdx,    5200.0f, 0.90f, 0.85f));
    ctx->register_material(ctx, "bedrock",  material(kBedrockIdx, 4000.0f, 1.00f, 1.00f));
    // Water is also registered by the water plugin (static sea flood); registering
    // it here too lets the M14 fluid spring below resolve "water" → palette_index
    // at registration regardless of plugin load order. Same palette/props, so the
    // duplicate is harmless (last write wins).
    ctx->register_material(ctx, "water",
                           material(kWaterIdx, 1000.0f, 0.0f, 0.0f, /*porosity=*/1.0f));

    // Palette colours (ABGR 0xAABBGGRR), so the world reads even with no atlas.
    ctx->set_palette_color(ctx, kStoneIdx,   0xff7a7a7a);
    ctx->set_palette_color(ctx, kGrassIdx,   0xff3fa83f);
    ctx->set_palette_color(ctx, kDirtIdx,    0xff3f5a8b);
    ctx->set_palette_color(ctx, kLeavesIdx,  0xff2f7a3a);
    ctx->set_palette_color(ctx, kWaterIdx,   0xffd08a3a);
    ctx->set_palette_color(ctx, kSandIdx,    0xff8fd6e6);
    ctx->set_palette_color(ctx, kLogIdx,     0xff2f4a6b);
    ctx->set_palette_color(ctx, kIronIdx,    0xff9a9aa8);
    ctx->set_palette_color(ctx, kBedrockIdx, 0xff202028);

    // M15 textures: register each atlas tile by id → PNG path, then bind material
    // faces to those ids by palette_index. The host writes these PNG tiles
    // (assets/textures/overworld/*.png) before building the atlas. All fail-soft:
    // with no texture pipeline attached (headless/audio-only host) both calls are
    // no-ops and the world renders the flat palette colours set above.
    const char* kTexDir = "assets/textures/overworld/";
    const char* kTiles[] = {"grass_top", "grass_side", "dirt", "stone",
                            "sand", "log_top", "log_side", "leaves"};
    for (const char* id : kTiles) {
        const std::string path = std::string(kTexDir) + id + ".png";
        ctx->register_texture(ctx, id, path.c_str());
    }
    bindFaces(ctx, kGrassIdx, "grass_top", "dirt", "grass_side");
    bindFaces(ctx, kDirtIdx,  "dirt",      "dirt", "dirt");
    bindFaces(ctx, kStoneIdx, "stone",     "stone", "stone");
    bindFaces(ctx, kSandIdx,  "sand",      "sand",  "sand");
    bindFaces(ctx, kLogIdx,   "log_top",   "log_top", "log_side");
    bindFaces(ctx, kLeavesIdx,"leaves",    "leaves", "leaves");

    ctx->register_layer_generator(ctx, "terrain", terrain_generator, nullptr);
    ctx->register_feature_generator(ctx, "cave", cave_feature, nullptr);
    ctx->register_feature_generator(ctx, "ore",  ore_feature,  nullptr);

    // M14 fluid spring: a steady source above the surface near spawn. With a
    // FluidSystem attached and the `flow` responder loaded, the engine injects
    // fluid into the field here each tick and `flow` realizes water voxels that
    // run downhill and pool — a live waterfall over the procedural terrain.
    // Fail-soft: a no-op when no FluidSystem is attached (headless host).
    ctx->register_fluid_source(ctx, WorldCoord(6.5, 33.5, 6.5), 6.0f, "water");
    return 0;
}

#ifndef OVERWORLD_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return overworld_plugin_init(ctx);
}
#endif
