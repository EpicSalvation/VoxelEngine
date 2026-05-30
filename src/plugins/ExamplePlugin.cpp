#include "ExamplePlugin.h"
#include "../world/Voxel.h"
#include <cmath>
#include <cstdint>
#include <iostream>

// ── Deterministic value-noise heightmap (M3) ───────────────────────────────
// Terrain height is a pure function of world (x, z). It uses only integer
// hashing and interpolation — no rand(), no time(), no static state — so a
// streamed chunk regenerates identically every time it is reloaded, and
// adjacent chunks share a continuous surface (the height at a shared edge is
// computed from the same world coordinate in both chunks).
namespace {

constexpr uint64_t kSeed       = 0x5DEECE66Dull;
constexpr double   kLattice    = 48.0;  // noise feature size, in voxels
constexpr int      kBaseHeight = 6;     // height (voxels) at noise value 0
constexpr int      kAmplitude  = 22;    // additional height range at noise value 1

// Hash a lattice point to [0, 1). splitmix64-style finalizer over the mixed
// integer coordinates; deterministic and well-distributed for signed inputs.
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

// Bilinearly interpolated value noise sampled at world (x, z) in voxel units.
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

// Surface height (top solid voxel, in world voxel units) at world column (x, z).
int terrainHeight(int64_t worldX, int64_t worldZ) {
    return kBaseHeight + static_cast<int>(
        valueNoise(static_cast<double>(worldX), static_cast<double>(worldZ)) * kAmplitude);
}

}  // namespace

// Layer generator: a stone heightmap with a one-voxel grass surface. Solid
// from world y=0 up to the column height; empty above and below. chunk_origin
// is the world-space corner of the chunk; the grid is grid_size³, row-major
// with x varying fastest. Assumes 1m terminal voxels (world voxel coordinate ==
// world meter), which is the M3 single-layer configuration.
static void example_layer_generator(
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

extern "C" int voxel_plugin_init(PluginContext* ctx) {
    MaterialProperties stone;
    stone.density             = 2700.0f;
    stone.structural_strength = 0.9f;
    stone.thermal_conductivity = 2.0f;
    stone.hardness            = 0.7f;
    stone.palette_index       = 1;
    ctx->register_material(ctx, "stone", stone);

    MaterialProperties grass;
    grass.density             = 1200.0f;
    grass.structural_strength = 0.3f;
    grass.thermal_conductivity = 0.5f;
    grass.hardness            = 0.2f;
    grass.palette_index       = 2;
    ctx->register_material(ctx, "grass", grass);

    ctx->register_layer_generator(ctx, "terrain", example_layer_generator, nullptr);

    std::cout << "[ExamplePlugin] Registered materials: stone, grass. "
              << "Layer generator: terrain (value-noise heightmap).\n";
    return 0;
}
