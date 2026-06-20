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
// It is the content half of the "Asteroid belt miner" demo (demos/17): a dense
// field of minable bodies in zero ambient gravity, each a roughly-spherical crust
// of rock with worley-distributed ore veins (richer ore is hardness-costlier, per
// M8's property-driven removal). Surrounding bodies in every direction is exactly
// what the camera-centered isotropic `box` StreamingVolume (M16 L1) is for.
//
// A cascade, not one flat layer (M6). The bodies are presented as a composite
// chain — coarse `macro` (16 m) and `micro` (4 m) blocks that DECOMPOSE on
// approach into a 1 m terminal `grid` the player mines. Two solidity tests share
// one body lattice (asteroid_field.h):
//   * the COARSE generators (macro/micro) use the noise-free cube-vs-sphere
//     overlap test `asteroidfield::coarseSolid`, deliberately a conservative
//     SUPERSET of the fine field so a decomposed body never sprouts holes at a
//     coarse-cell boundary (the coarse-supersets-fine invariant, ARCHITECTURE §4;
//     the same trap drill-world's `cellSolid` avoids for a heightmap), and
//   * the FINE generator (grid) carves the detailed noisy crust + ore veins.
// Both read the SAME body centers/radii from the shared lattice, so the coarse
// blocks tightly envelope the fine asteroid they refine into.
//
// Determinism (ARCHITECTURE §4): the field is a pure function of world position
// and a fixed seed — asteroid placement is hashed from the cell index, surface
// relief and ore veins are sampled from the engine's noise registry at the world
// point. So a streamed-out chunk regenerates byte-identically.
//
// M16 (C2): surface relief and ore veins are sampled from the built-in `fbm` and
// `worley` noise resolved through ctx->resolve_noise at init — the new volumetric
// generators are the first real consumers of the resolve_noise accessor.

#include "asteroid_field.h"  // shared body lattice (also pulls in plugin_api.h)
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

using asteroidfield::Body;

// Material banding (depth below the noisy surface, in meters).
constexpr double kCrustM    = 2.5;   // outer rock shell
constexpr double kReliefFeatM = 14.0;  // fbm relief feature size
constexpr double kOreFeatM  = 9.0;   // worley vein cell size
constexpr double kOreThresh = 0.17;  // worley distance below this ⇒ ore vein

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

double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// Signed depth of world point p below the NOISY surface of the nearest covering
// asteroid: > 0 inside the body, ≤ 0 in empty space. Also returns whether that
// body is icy so the surface crust can be ice. This is the detailed (terminal)
// field — the real, minable silhouette.
double surfaceDepth(const glm::dvec3& p, bool& outIcy) {
    double bestDepth = -1.0;
    outIcy = false;

    asteroidfield::forEachBody(p, [&](const Body& b) {
        const glm::dvec3 d = p - b.center;
        const double dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist >= b.radius * (1.0 + asteroidfield::kReliefFrac))
            return;  // outside even the maximal relief bound — cheap reject

        // Lumpy crust: perturb the radius by fbm at the world point, seeded per
        // body so adjacent asteroids look different.
        const RecipeParam rp = scaleParam(kReliefFeatM);
        const float relief = g_fbm(WorldCoord(p), b.seed, &rp, 1, nullptr);
        const double effR =
            b.radius * (1.0 + asteroidfield::kReliefFrac * (relief - 0.5) * 2.0);

        const double depth = effR - dist;
        if (depth > bestDepth) {
            bestDepth = depth;
            outIcy    = b.icy;
        }
    });
    return bestDepth;
}

// FINE terminal generator (1 m `grid`): the detailed crust + worley ore veins —
// the surface the player actually mines. Richer ore is hardness-costlier (M8).
void fine_generator(WorldCoord chunk_origin, int grid_size, Voxel* out,
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
            const float w = g_worley(WorldCoord(p), asteroidfield::kSeed,
                                     &oreParam, 1, nullptr);
            v.material = (w < static_cast<float>(kOreThresh)) ? kOre : kRock;
        }
    }
}

// COARSE composite generator (macro 16 m / micro 4 m): the conservative,
// noise-free body envelope. A cell is solid when it overlaps any nearby body's
// maximal-radius sphere — a SUPERSET of the fine field, so each macro voxel fully
// contains the fine asteroid it decomposes into. Voxel size arrives through
// user_data (a pointer to a static double), as the DecompositionManager passes a
// child generator's registered user_data verbatim.
void coarse_generator(WorldCoord chunk_origin, int grid_size, Voxel* out,
                      void* user_data) {
    const double vs = voxelSizeFrom(user_data);

    for (int z = 0; z < grid_size; ++z)
    for (int y = 0; y < grid_size; ++y)
    for (int x = 0; x < grid_size; ++x) {
        const glm::dvec3 c(
            chunk_origin.value.x + (x + 0.5) * vs,
            chunk_origin.value.y + (y + 0.5) * vs,
            chunk_origin.value.z + (z + 0.5) * vs);

        bool icy = false;
        Voxel& v = out[x + grid_size * (y + grid_size * z)];
        v = asteroidfield::coarseSolid(c, vs, icy)
                ? Voxel{ icy ? kIce : kRock }
                : Voxel::empty();
    }
}

// Per-layer voxel sizes handed to the generators as user_data — the
// DecompositionManager forwards a generator's registered user_data unchanged, and
// LayerGeneratorFn carries no size of its own (the layered-world convention).
double g_macroVs = 16.0;
double g_microVs = 4.0;
double g_gridVs  = 1.0;

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

    // The decomposition cascade: macro (16 m composite) → micro (4 m composite)
    // → grid (1 m terminal). Coarse layers share the conservative envelope; the
    // terminal layer carves the detailed minable surface. Each generator receives
    // its layer's voxel size via user_data.
    ctx->register_layer_generator(ctx, "macro", coarse_generator, &g_macroVs);
    ctx->register_layer_generator(ctx, "micro", coarse_generator, &g_microVs);
    ctx->register_layer_generator(ctx, "grid",  fine_generator,   &g_gridVs);
    return 0;
}
