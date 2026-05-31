// base-terrain plugin — built as a real shared library (.so/.dylib/.dll) and
// loaded by PluginManager at runtime. Registers the materials and the single
// terminal-layer generator that produce the heightmap world. With only this
// plugin loaded the output is identical to the M3 streaming-terrain demo.
//
// This is the disk-loaded counterpart of src/plugins/ExamplePlugin (which is
// compiled into the engine and wired in for the M3 demo). The generation math
// is intentionally the same: a deterministic value-noise heightmap that is a
// pure function of world (x, z), so streamed chunks regenerate identically.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

constexpr uint64_t kSeed       = 0x5DEECE66Dull;
constexpr double   kLattice    = 48.0;  // noise feature size, in voxels
constexpr int      kBaseHeight = 6;     // height (voxels) at noise value 0
constexpr int      kAmplitude  = 22;    // additional height range at noise value 1

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

int terrainHeight(int64_t worldX, int64_t worldZ) {
    return kBaseHeight + static_cast<int>(
        valueNoise(static_cast<double>(worldX), static_cast<double>(worldZ)) * kAmplitude);
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
