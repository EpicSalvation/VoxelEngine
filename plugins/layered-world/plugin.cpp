// layered-world plugin — the multi-layer (M6) counterpart of base-terrain.
//
// Registers materials and one layer generator per named layer in the demo 05
// stack, so a single deterministic heightmap is sampled at three scales:
//
//   - "blocks"   (composite, 8 m) — a coarse, blocky approximation of the
//                terrain. Rendered as atomic macro voxels until the camera
//                approaches, then decomposed into the terrain layer.
//   - "terrain"  (terminal, 1 m)  — the fine heightmap the macro voxels
//                decompose into. Identical surface to base-terrain so a block
//                refines into matching detail.
//   - "backdrop" (immutable, 2 m) — a fixed bedrock slab beneath the terrain
//                that renders and collides but is never edited or decomposed.
//
// Every generator is a pure function of world position (no rand/time/order),
// which is what makes decomposition deterministic (ARCHITECTURE §4): the coarse
// and fine layers agree because they read the same height field.
//
// The LayerGeneratorFn signature carries no voxel size, so each non-1 m
// generator receives its layer's voxel size through user_data (a pointer to a
// static double). The terrain generator runs at the implicit 1 m scale.

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

namespace {

constexpr uint64_t kSeed       = 0x5DEECE66Dull;
constexpr double   kLattice    = 48.0;  // noise feature size, in meters
constexpr int      kBaseHeight = 6;     // surface height (m) at noise value 0
constexpr int      kAmplitude  = 22;    // additional height range (m) at noise value 1

// Palette slots (see renderer/Palette.h); all opaque (index 4 is the water slot).
constexpr uint8_t kStoneIdx    = 1;  // gray
constexpr uint8_t kGrassIdx    = 2;  // green
constexpr uint8_t kBlockIdx    = 5;  // amber — coarse composite blocks
constexpr uint8_t kBedrockIdx  = 7;  // purple — immutable backdrop

// Voxel sizes handed to the non-1 m generators through user_data.
const double kBlocksVoxelSizeM   = 8.0;
const double kBackdropVoxelSizeM = 2.0;

// Immutable bedrock slab occupies world Y in [kBedrockBottom, kBedrockTop).
constexpr double kBedrockBottom = -8.0;
constexpr double kBedrockTop    =  0.0;

double latticeValue(int64_t ix, int64_t iz) {
    uint64_t h = static_cast<uint64_t>(ix) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(iz) * 0xC2B2AE3D27D4EB4Full;
    h += kSeed;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<double>(h >> 40) / static_cast<double>(1ull << 24);  // [0,1)
}

double smootherstep(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

double valueNoise(double x, double z) {
    double fx = x / kLattice;
    double fz = z / kLattice;
    int64_t ix = static_cast<int64_t>(std::floor(fx));
    int64_t iz = static_cast<int64_t>(std::floor(fz));
    double tx = smootherstep(fx - static_cast<double>(ix));
    double tz = smootherstep(fz - static_cast<double>(iz));

    double v00 = latticeValue(ix,     iz);
    double v10 = latticeValue(ix + 1, iz);
    double v01 = latticeValue(ix,     iz + 1);
    double v11 = latticeValue(ix + 1, iz + 1);

    double a = v00 + (v10 - v00) * tx;
    double b = v01 + (v11 - v01) * tx;
    return a + (b - a) * tz;  // [0,1)
}

// Surface height in meters at a world (x, z) column. Pure function of position,
// shared by the coarse and fine generators so they stay consistent.
double terrainHeightM(double worldX, double worldZ) {
    return static_cast<double>(kBaseHeight) + valueNoise(worldX, worldZ) * kAmplitude;
}

MaterialProperties solid(uint8_t paletteIndex, float density) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = 0.8f;
    m.hardness            = 0.6f;
    m.palette_index       = paletteIndex;
    return m;
}

double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// Fine terminal terrain (1 m): solid stone below the surface, grass at the
// surface, empty above. The volume a decomposed macro voxel reveals.
void terrain_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const double vs = voxelSizeFrom(user_data);  // 1.0 for terrain
    const MaterialProperties stone = solid(kStoneIdx, 2700.0f);
    const MaterialProperties grass = solid(kGrassIdx, 1200.0f);

    for (int z = 0; z < grid_size; ++z)
        for (int x = 0; x < grid_size; ++x) {
            const double worldX = chunk_origin.value.x + (x + 0.5) * vs;
            const double worldZ = chunk_origin.value.z + (z + 0.5) * vs;
            const double height = terrainHeightM(worldX, worldZ);
            for (int y = 0; y < grid_size; ++y) {
                const double worldY = chunk_origin.value.y + y * vs;  // cell bottom
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                if (worldY < 0.0 || worldY >= height)
                    v = Voxel::empty();
                else if (worldY + vs >= height)
                    v.material = grass;
                else
                    v.material = stone;
            }
        }
}

// Coarse composite blocks (8 m): a macro voxel is solid when its vertical span
// overlaps the terrain anywhere in its footprint — a blocky stand-in that refines
// to terrain_generator's output when decomposed.
//
// The coarse occupancy MUST be a conservative superset of the fine occupancy:
// decomposition only runs on macro voxels this generator marks solid, so any fine
// terrain voxel whose parent macro voxel is left empty is never generated, leaving
// holes in the terrain on slopes. Sampling the surface only at the macro voxel's
// center column (as an earlier version did) misses fine columns that rise higher
// elsewhere in the 8 m footprint, so we take the MAX surface height over the same
// 1 m columns terrain_generator will fill. Footprint columns are sampled at the
// child layer's 1 m resolution and identical (x + 0.5) centering, so the bound is
// exact: a macro voxel is solid iff its decomposition would contain ≥1 voxel.
void blocks_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const double vs = voxelSizeFrom(user_data);  // 8.0
    const MaterialProperties block = solid(kBlockIdx, 2200.0f);
    const int    subs = std::max(1, static_cast<int>(std::llround(vs)));  // 1 m fine columns per edge

    for (int z = 0; z < grid_size; ++z)
        for (int x = 0; x < grid_size; ++x) {
            const double cellX0 = chunk_origin.value.x + x * vs;
            const double cellZ0 = chunk_origin.value.z + z * vs;
            double height = 0.0;
            for (int sz = 0; sz < subs; ++sz)
                for (int sx = 0; sx < subs; ++sx) {
                    const double worldX = cellX0 + sx + 0.5;
                    const double worldZ = cellZ0 + sz + 0.5;
                    height = std::max(height, terrainHeightM(worldX, worldZ));
                }
            for (int y = 0; y < grid_size; ++y) {
                const double bottom = chunk_origin.value.y + y * vs;
                const double top    = bottom + vs;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                v = (top > 0.0 && bottom < height) ? Voxel{block} : Voxel::empty();
            }
        }
}

// Immutable backdrop (2 m): a solid bedrock slab under the terrain. Generated
// once and retained — the demo never edits, persists, or decomposes it.
void backdrop_generator(WorldCoord chunk_origin, int grid_size, Voxel* out, void* user_data) {
    const double vs = voxelSizeFrom(user_data);  // 2.0
    const MaterialProperties bedrock = solid(kBedrockIdx, 3200.0f);

    for (int z = 0; z < grid_size; ++z)
        for (int y = 0; y < grid_size; ++y)
            for (int x = 0; x < grid_size; ++x) {
                const double bottom = chunk_origin.value.y + y * vs;
                const double top    = bottom + vs;
                Voxel& v = out[x + grid_size * (y + grid_size * z)];
                v = (top > kBedrockBottom && bottom < kBedrockTop)
                        ? Voxel{bedrock} : Voxel::empty();
            }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_material(ctx, "stone",   solid(kStoneIdx,   2700.0f));
    ctx->register_material(ctx, "grass",   solid(kGrassIdx,   1200.0f));
    ctx->register_material(ctx, "block",   solid(kBlockIdx,   2200.0f));
    ctx->register_material(ctx, "bedrock", solid(kBedrockIdx, 3200.0f));

    ctx->register_layer_generator(ctx, "terrain",  terrain_generator,  nullptr);
    ctx->register_layer_generator(ctx, "blocks",   blocks_generator,
                                  const_cast<double*>(&kBlocksVoxelSizeM));
    ctx->register_layer_generator(ctx, "backdrop", backdrop_generator,
                                  const_cast<double*>(&kBackdropVoxelSizeM));
    return 0;
}
