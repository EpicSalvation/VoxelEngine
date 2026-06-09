#include "Noise.h"

#include <cmath>
#include <cstdint>

namespace noise {
namespace {

// Hash a 3D integer lattice corner + seed into [0,1). Pure splitmix-style mixing
// (the same finalizer the inline plugin noise and voxel_rng_next use), so it is
// deterministic and free of rand/time/global state.
double latticeValue(int64_t ix, int64_t iy, int64_t iz, uint64_t seed) {
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(ix) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(iy) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(iz) * 0x165667B19E3779F9ull;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<double>(h >> 40) / static_cast<double>(1ull << 24);  // [0,1)
}

double smootherstep(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Trilinearly-interpolated 3D value noise sampled at a world position, in [0,1).
// `frequency` is cycles per world unit (1 / feature size).
double value3(const glm::dvec3& p, uint64_t seed, double frequency) {
    const double fx = p.x * frequency;
    const double fy = p.y * frequency;
    const double fz = p.z * frequency;

    const int64_t ix = static_cast<int64_t>(std::floor(fx));
    const int64_t iy = static_cast<int64_t>(std::floor(fy));
    const int64_t iz = static_cast<int64_t>(std::floor(fz));

    const double tx = smootherstep(fx - static_cast<double>(ix));
    const double ty = smootherstep(fy - static_cast<double>(iy));
    const double tz = smootherstep(fz - static_cast<double>(iz));

    const double c000 = latticeValue(ix,     iy,     iz,     seed);
    const double c100 = latticeValue(ix + 1, iy,     iz,     seed);
    const double c010 = latticeValue(ix,     iy + 1, iz,     seed);
    const double c110 = latticeValue(ix + 1, iy + 1, iz,     seed);
    const double c001 = latticeValue(ix,     iy,     iz + 1, seed);
    const double c101 = latticeValue(ix + 1, iy,     iz + 1, seed);
    const double c011 = latticeValue(ix,     iy + 1, iz + 1, seed);
    const double c111 = latticeValue(ix + 1, iy + 1, iz + 1, seed);

    const double x00 = lerp(c000, c100, tx);
    const double x10 = lerp(c010, c110, tx);
    const double x01 = lerp(c001, c101, tx);
    const double x11 = lerp(c011, c111, tx);
    const double y0  = lerp(x00, x10, ty);
    const double y1  = lerp(x01, x11, ty);
    return lerp(y0, y1, tz);
}

// "scale" is the feature size in world units; frequency is its reciprocal. A
// non-positive scale falls back to the default so a noise call can never divide
// by zero.
double frequencyFromParams(const RecipeParam* params, size_t count) {
    const double scale = recipe_param_num(params, count, "scale", 32.0);
    return (scale > 0.0) ? (1.0 / scale) : (1.0 / 32.0);
}

}  // namespace

float value(WorldCoord pos, uint64_t seed,
            const RecipeParam* params, size_t param_count, void* /*user_data*/) {
    return static_cast<float>(value3(pos.value, seed, frequencyFromParams(params, param_count)));
}

float fbm(WorldCoord pos, uint64_t seed,
          const RecipeParam* params, size_t param_count, void* /*user_data*/) {
    int    octaves    = static_cast<int>(recipe_param_num(params, param_count, "octaves", 4.0));
    double lacunarity = recipe_param_num(params, param_count, "lacunarity", 2.0);
    double gain       = recipe_param_num(params, param_count, "gain", 0.5);
    if (octaves < 1)  octaves = 1;
    if (octaves > 16) octaves = 16;  // bound the loop; deep stacks add nothing visible

    double frequency = frequencyFromParams(params, param_count);
    double amplitude = 1.0;
    double sum       = 0.0;
    double norm      = 0.0;
    for (int o = 0; o < octaves; ++o) {
        // Decorrelate octaves by advancing the seed deterministically.
        sum  += amplitude * value3(pos.value, voxel_seed_mix(seed, static_cast<uint64_t>(o)), frequency);
        norm += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return static_cast<float>(norm > 0.0 ? sum / norm : 0.0);  // [0,1)
}

float ridged(WorldCoord pos, uint64_t seed,
             const RecipeParam* params, size_t param_count, void* /*user_data*/) {
    int    octaves    = static_cast<int>(recipe_param_num(params, param_count, "octaves", 4.0));
    double lacunarity = recipe_param_num(params, param_count, "lacunarity", 2.0);
    double gain       = recipe_param_num(params, param_count, "gain", 0.5);
    if (octaves < 1)  octaves = 1;
    if (octaves > 16) octaves = 16;

    double frequency = frequencyFromParams(params, param_count);
    double amplitude = 1.0;
    double sum       = 0.0;
    double norm      = 0.0;
    for (int o = 0; o < octaves; ++o) {
        // Fold the field into sharp ridges: 1 - |2v - 1| peaks at v == 0.5.
        double v = value3(pos.value, voxel_seed_mix(seed, static_cast<uint64_t>(o)), frequency);
        double r = 1.0 - std::fabs(2.0 * v - 1.0);
        sum  += amplitude * r;
        norm += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return static_cast<float>(norm > 0.0 ? sum / norm : 0.0);  // [0,1)
}

float worley(WorldCoord pos, uint64_t seed,
             const RecipeParam* params, size_t param_count, void* /*user_data*/) {
    // Cellular noise: distance to the nearest jittered feature point, normalized
    // to [0,1) by the cell size. One feature point per integer cell; the 27-cell
    // neighborhood is enough since a point cannot be closer from farther away.
    const double scale     = recipe_param_num(params, param_count, "scale", 32.0);
    const double cell      = (scale > 0.0) ? scale : 32.0;
    const glm::dvec3 p      = pos.value / cell;
    const int64_t cx        = static_cast<int64_t>(std::floor(p.x));
    const int64_t cy        = static_cast<int64_t>(std::floor(p.y));
    const int64_t cz        = static_cast<int64_t>(std::floor(p.z));

    double nearestSq = 3.0;  // (sqrt3)^2 — the max possible normalized distance in-cell window
    for (int64_t dz = -1; dz <= 1; ++dz)
    for (int64_t dy = -1; dy <= 1; ++dy)
    for (int64_t dx = -1; dx <= 1; ++dx) {
        const int64_t gx = cx + dx, gy = cy + dy, gz = cz + dz;
        // Three independent hashes jitter the feature point within its cell.
        const double fxp = static_cast<double>(gx) + latticeValue(gx, gy, gz, seed);
        const double fyp = static_cast<double>(gy) + latticeValue(gx, gy, gz, voxel_seed_mix(seed, 1));
        const double fzp = static_cast<double>(gz) + latticeValue(gx, gy, gz, voxel_seed_mix(seed, 2));
        const double ex = fxp - p.x, ey = fyp - p.y, ez = fzp - p.z;
        const double d2 = ex * ex + ey * ey + ez * ez;
        if (d2 < nearestSq) nearestSq = d2;
    }
    double d = std::sqrt(nearestSq);
    if (d > 1.0) d = 1.0;  // clamp into [0,1)
    return static_cast<float>(d);
}

const std::vector<BuiltinNoise>& builtins() {
    static const std::vector<BuiltinNoise> kBuiltins = {
        {"value",  &value},
        {"fbm",    &fbm},
        {"ridged", &ridged},
        {"worley", &worley},
    };
    return kBuiltins;
}

}  // namespace noise
