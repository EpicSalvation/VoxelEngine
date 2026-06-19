// asteroid-field plugin — the first VOLUMETRIC generator to ship in-tree (M16 C1).
//
// Every generator that shipped before this one is a Y-up heightmap: it computes a
// surface height for each (x, z) column and fills below it (base-terrain,
// layered-world, drill-world). That quietly makes the engine *look* like a
// heightmap engine even though nothing in it forbids 3D fields. This generator is
// the counter-example: a radial density field with NO privileged axis. Space is
// seeded with rocky bodies and a voxel is solid when it lies inside the
// noise-perturbed radius of a nearby asteroid — solid above, below, and to every
// side of empty space, the shape a heightmap cannot express.
//
// It is the content half of the "Asteroid belt miner" demo: a dense field of
// minable bodies in zero ambient gravity, each a roughly-spherical crust of rock
// with worley-distributed ore veins (richer ore is hardness-costlier, per M8's
// property-driven removal). Surrounding bodies in every direction is exactly what
// the camera-centered isotropic `box` StreamingVolume (M16 L1) is for.
//
// Determinism (ARCHITECTURE §4): the field is a pure function of world position
// and a fixed seed — asteroid placement is hashed from the cell index, surface
// relief and ore veins are sampled from the engine's noise registry at the world
// point. So a streamed-out chunk regenerates byte-identically, and a coarse macro
// voxel and its fine decomposition agree because both read the same field.
//
// M16 (C2): surface relief and ore veins are sampled from the built-in `fbm` and
// `worley` noise resolved through ctx->resolve_noise at init — the new volumetric
// generators are the first real consumers of the resolve_noise accessor.

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

constexpr uint64_t kSeed = 0xA57E401DF1E1Dull;  // "ASTEROID FIELD"

// Space is partitioned into cubic cells; each cell deterministically hosts at most
// one asteroid. kCellM is the mean body spacing. Body radii are capped well below
// half a cell (kRadiusMax < kCellM/2) so any asteroid covering a query point has
// its center within the point's 1-ring of cells — the 3×3×3 neighborhood scanned
// below is then exact, never missing a body that reaches in from a neighbor cell.
constexpr double kCellM      = 80.0;
constexpr double kFillChance = 0.55;  // fraction of cells that host an asteroid
constexpr double kRadiusMin  = 12.0;
constexpr double kRadiusMax  = 30.0;

// Surface relief: the body radius is modulated by ±kReliefFrac of fbm, giving a
// lumpy, non-spherical crust. kReliefFeatM is the relief feature size in meters.
constexpr double kReliefFrac  = 0.35;
constexpr double kReliefFeatM = 14.0;

// Material banding (depth below the noisy surface, in meters).
constexpr double kCrustM      = 2.5;   // outer rock shell
constexpr double kOreFeatM    = 9.0;   // worley vein cell size
constexpr double kOreThresh   = 0.17;  // worley distance below this ⇒ ore vein

// Palette slots — opaque colours installed in init so bodies don't borrow the
// default cycling palette's translucent water slot.
constexpr uint8_t kRockIdx = 30;
constexpr uint8_t kOreIdx  = 31;
constexpr uint8_t kIceIdx  = 32;

// Built-in noise resolved at init (M16 C2). Set before any generator runs; the
// engine's built-ins are pure (they ignore user_data), so plain pointers suffice.
NoiseFn g_fbm    = nullptr;
NoiseFn g_worley = nullptr;

MaterialProperties make(uint8_t palette, float density, float hardness,
                        float strength) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = strength;
    m.thermal_conductivity = 1.5f;
    m.hardness            = hardness;
    m.palette_index       = palette;
    return m;
}

const MaterialProperties kRock = make(kRockIdx, 3000.0f, 0.55f, 0.8f);
const MaterialProperties kOre  = make(kOreIdx,  5200.0f, 1.40f, 0.9f);  // richer ⇒ costlier
const MaterialProperties kIce  = make(kIceIdx,   920.0f, 0.25f, 0.4f);

// A scalar param pack the noise calls thread their feature size through.
RecipeParam scaleParam(double feature) {
    return { "scale", RecipeParamKind::Number, feature, nullptr };
}

// Deterministic per-cell seed; chains the header's splitmix mixer over the cell
// index so neighbouring cells decorrelate.
uint64_t cellSeed(int64_t cx, int64_t cy, int64_t cz) {
    uint64_t s = kSeed;
    s = voxel_seed_mix(s, static_cast<uint64_t>(cx));
    s = voxel_seed_mix(s, static_cast<uint64_t>(cy));
    s = voxel_seed_mix(s, static_cast<uint64_t>(cz));
    return s;
}

double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// Signed depth of world point p below the noisy surface of the nearest covering
// asteroid: > 0 inside the body, ≤ 0 in empty space. Also returns whether the
// covering body is icy (a deterministic per-body trait) so the surface can be ice.
// Pure function of p — this is what makes the field deterministic and seamless.
double surfaceDepth(const glm::dvec3& p, bool& outIcy) {
    const int64_t cx = static_cast<int64_t>(std::floor(p.x / kCellM));
    const int64_t cy = static_cast<int64_t>(std::floor(p.y / kCellM));
    const int64_t cz = static_cast<int64_t>(std::floor(p.z / kCellM));

    double bestDepth = -1.0;
    outIcy = false;

    for (int64_t dz = -1; dz <= 1; ++dz)
    for (int64_t dy = -1; dy <= 1; ++dy)
    for (int64_t dx = -1; dx <= 1; ++dx) {
        const int64_t gx = cx + dx, gy = cy + dy, gz = cz + dz;
        uint64_t st = cellSeed(gx, gy, gz);
        if (voxel_rng_norm(&st) >= static_cast<float>(kFillChance))
            continue;  // this cell holds no asteroid

        const double jx = voxel_rng_norm(&st);
        const double jy = voxel_rng_norm(&st);
        const double jz = voxel_rng_norm(&st);
        const double rr = voxel_rng_norm(&st);
        const bool   icy = voxel_rng_norm(&st) < 0.18f;

        const glm::dvec3 center(
            (static_cast<double>(gx) + jx) * kCellM,
            (static_cast<double>(gy) + jy) * kCellM,
            (static_cast<double>(gz) + jz) * kCellM);
        const double radius = kRadiusMin + rr * (kRadiusMax - kRadiusMin);

        const glm::dvec3 d = p - center;
        const double dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist >= radius * (1.0 + kReliefFrac))
            continue;  // outside even the maximal relief bound — cheap reject

        // Lumpy crust: perturb the radius by fbm at the world point, seeded per
        // body so adjacent asteroids look different.
        const RecipeParam rp = scaleParam(kReliefFeatM);
        const float relief = g_fbm(WorldCoord(p), st, &rp, 1, nullptr);
        const double effR = radius * (1.0 + kReliefFrac * (relief - 0.5) * 2.0);

        const double depth = effR - dist;
        if (depth > bestDepth) {
            bestDepth = depth;
            outIcy    = icy;
        }
    }
    return bestDepth;
}

// Layer generator: fills a chunk's grid from the radial density field. Voxel size
// arrives through user_data (a pointer to a static double), the layered-world
// convention; absent ⇒ the implicit 1 m terminal scale.
void asteroid_generator(WorldCoord chunk_origin, int grid_size, Voxel* out,
                        void* user_data) {
    const double vs = voxelSizeFrom(user_data);
    const RecipeParam oreParam = scaleParam(kOreFeatM);

    for (int z = 0; z < grid_size; ++z)
    for (int y = 0; y < grid_size; ++y)
    for (int x = 0; x < grid_size; ++x) {
        const glm::dvec3 p(
            chunk_origin.value.x + (x + 0.5) * vs,
            chunk_origin.value.y + (y + 0.5) * vs,
            chunk_origin.value.z + (z + 0.5) * vs);

        bool icy = false;
        const double depth = surfaceDepth(p, icy);
        Voxel& v = out[x + grid_size * (y + grid_size * z)];

        if (depth <= 0.0) {
            v = Voxel::empty();
        } else if (depth < kCrustM) {
            v.material = icy ? kIce : kRock;        // surface crust
        } else {
            const float w = g_worley(WorldCoord(p), kSeed, &oreParam, 1, nullptr);
            v.material = (w < static_cast<float>(kOreThresh)) ? kOre : kRock;
        }
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Consume the built-in relief/vein noise (M16 C2); fail loudly on an unknown
    // id per the §6 contract rather than silently mis-generating.
    g_fbm    = ctx->resolve_noise(ctx, "fbm");
    g_worley = ctx->resolve_noise(ctx, "worley");
    if (!g_fbm || !g_worley)
        return 1;

    ctx->register_material(ctx, "asteroid_rock", kRock);
    ctx->register_material(ctx, "asteroid_ore",  kOre);
    ctx->register_material(ctx, "asteroid_ice",  kIce);

    ctx->set_palette_color(ctx, kRockIdx, 0xff5a5a64u);  // dark gray rock
    ctx->set_palette_color(ctx, kOreIdx,  0xff2fb5e6u);  // amber/gold ore vein
    ctx->set_palette_color(ctx, kIceIdx,  0xffe8d8a8u);  // pale blue-white ice

    ctx->register_layer_generator(ctx, "asteroids", asteroid_generator, nullptr);
    return 0;
}
