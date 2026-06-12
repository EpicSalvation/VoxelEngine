// drill-world plugin — four-layer cascade world for the M10 demo.
//
// Layer stack (parent voxel size == child chunk world size for clean alignment):
//   continental  512 m composite → regional   (chunk 8 → 64 m/chunk, ratio 8)
//   regional      64 m composite → local      (chunk 8 →  8 m/chunk, ratio 8)
//   local          8 m composite → terrain    (chunk 8 →  8 m/chunk, ratio 8)
//   terrain        1 m terminal
//
// Each composite layer has a recipe that fills child voxels with material. The
// visual variety across levels: continental=dark slate, regional=gray granite,
// local=tan limestone, terrain=detailed soil-over-rock with caves.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>

#ifdef _WIN32
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

// ── Deterministic inline value noise ─────────────────────────────────────────

static uint64_t hashCoord(int64_t ix, int64_t iy, int64_t iz, uint64_t seed) {
    uint64_t h = seed ^ (static_cast<uint64_t>(ix) * 0x6c62272e07bb0142ull);
    h ^= static_cast<uint64_t>(iy) * 0x94d049bb133111ebull;
    h ^= static_cast<uint64_t>(iz) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

static float hashF(int64_t ix, int64_t iy, int64_t iz, uint64_t seed) {
    return static_cast<float>(hashCoord(ix, iy, iz, seed) >> 40) * (1.0f / 16777216.0f);
}

// Trilinear value noise [0,1).
static float valueNoise(double wx, double wy, double wz, double scale, uint64_t seed) {
    const double fx = wx / scale, fy = wy / scale, fz = wz / scale;
    const int64_t ix = static_cast<int64_t>(std::floor(fx));
    const int64_t iy = static_cast<int64_t>(std::floor(fy));
    const int64_t iz = static_cast<int64_t>(std::floor(fz));
    const float u = static_cast<float>(fx - ix), v = static_cast<float>(fy - iy),
                w = static_cast<float>(fz - iz);
    const float su = u*u*(3.0f-2.0f*u), sv = v*v*(3.0f-2.0f*v), sw = w*w*(3.0f-2.0f*w);
    auto lerp = [](float a, float b, float t){ return a + (b - a) * t; };
    return lerp(lerp(lerp(hashF(ix,iy,iz,seed),   hashF(ix+1,iy,iz,seed),   su),
                     lerp(hashF(ix,iy+1,iz,seed),  hashF(ix+1,iy+1,iz,seed), su), sv),
                lerp(lerp(hashF(ix,iy,iz+1,seed),  hashF(ix+1,iy,iz+1,seed), su),
                     lerp(hashF(ix,iy+1,iz+1,seed),hashF(ix+1,iy+1,iz+1,seed),su),sv),sw);
}

// ── Material definitions ──────────────────────────────────────────────────────

static const MaterialProperties kContinentalRock{2700.f,150.f,2.f,0.02f, 8.f,20};
static const MaterialProperties kRegionalRock   {2500.f,120.f,2.f,0.03f, 5.f,21};
static const MaterialProperties kLocalRock      {2200.f, 90.f,2.f,0.04f, 3.f,22};
static const MaterialProperties kSoil           {1400.f, 30.f,.5f,0.30f, 1.f,23};

// ── Shared surface + finite crust ─────────────────────────────────────────────
//
// ALL four layers derive solidity from this single height field so the coarse
// LOD blocks tightly superset the fine terrain (ARCHITECTURE §4 coarse-supersets-
// fine). The cascade is deliberately SHALLOW (top voxel 64 m, not 512 m): a 512 m
// top layer forced a large approach radius, and decomposing a large solid sphere
// down to 1 m is unbounded work — terrain never kept up and the player stood on
// undecomposed composite blocks. Here the surface sits just under the 64 m
// continental cell boundary (range ~[50, 58] m) so coarse blocks overshoot the
// real terrain by ≤ ~14 m, and the rock is a FINITE crust over a hollow core, so
// the fine-voxel workload inside the approach radius is bounded.
static constexpr double kCrustThickness = 16.0;  // metres of solid rock; hollow below

static double surfaceHeight(double wx, double wz) {
    return 54.0 + 4.0 * (double)valueNoise(wx, 0, wz, 256.0, 0xBEEF1234ull);
}

// A composite-layer cell [yb, yb+vs] is solid when it OVERLAPS the crust band
// (surface - thickness, surface]. Testing the cell extent (not just a point)
// guarantees a coarse cell straddling the band is solid, so it fully contains the
// finer detail beneath it (coarse-supersets-fine).
static bool cellSolid(double cellBottomY, double vs, double wx, double wz) {
    const double s = surfaceHeight(wx, wz);
    return cellBottomY < s && (cellBottomY + vs) > (s - kCrustThickness);
}

// ── Layer generators ──────────────────────────────────────────────────────────

// Continental (64 m): dark-rock slab — the coarse top of the cascade.
static void continentalGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 64.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double yb = origin.value.y + y * vs;
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                out[x + n*(y + n*z)] =
                    cellSolid(yb, vs, wx, wz) ? Voxel{kContinentalRock} : Voxel::empty();
            }
}

// Regional (16 m): gray-granite slab, same shared surface + crust.
static void regionalGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 16.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double yb = origin.value.y + y * vs;
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                out[x + n*(y + n*z)] =
                    cellSolid(yb, vs, wx, wz) ? Voxel{kRegionalRock} : Voxel::empty();
            }
}

// Local (4 m): tan-limestone slab, same shared surface + crust.
static void localGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 4.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double yb = origin.value.y + y * vs;
                const double wx = origin.value.x + (x + 0.5) * vs;
                const double wz = origin.value.z + (z + 0.5) * vs;
                out[x + n*(y + n*z)] =
                    cellSolid(yb, vs, wx, wz) ? Voxel{kLocalRock} : Voxel::empty();
            }
}

// Terrain (1 m): detailed surface — soil cap over rock, cave voids, hollow core.
// Uses the SAME surface height so the fine detail sits exactly inside the coarse
// blocks; empty above the surface AND below the crust (the drillable hollow core).
static void terrainGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 1.0;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const double wx = origin.value.x + (x + 0.5) * vs;
            const double wz = origin.value.z + (z + 0.5) * vs;
            const double height = surfaceHeight(wx, wz);
            for (int y = 0; y < n; ++y) {
                const double wy = origin.value.y + (y + 0.5) * vs;
                const int idx = x + n*(y + n*z);
                if (wy >= height) { out[idx] = Voxel::empty(); continue; }
                if (wy < height - kCrustThickness) { out[idx] = Voxel::empty(); continue; }
                const float cave = valueNoise(wx, wy, wz, 12.0, 0xCAFEBABEull);
                if (cave < 0.28f) { out[idx] = Voxel::empty(); continue; }
                const bool nearSurface = (height - wy) <= 3.0;
                out[idx] = Voxel{nearSurface ? kSoil : kLocalRock};
            }
        }
    }
}

// ── Recipe descriptors ────────────────────────────────────────────────────────

static MaterialWeight kContWeights[]  = {{"continental_rock", 1.0f}};
static MaterialWeight kRegWeights[]   = {{"regional_rock",    1.0f}};
static MaterialWeight kLocalWeights[] = {{"local_rock", 0.85f}, {"soil", 0.15f}};
static MaterialWeight kSoilWeights[]  = {{"soil", 1.0f}};

static RecipeDesc kContinentalRecipe = {
    /* interior */  {kContWeights,  1, nullptr, nullptr, 0},
    /* features */  nullptr, 0,
    /* top    */    {{kContWeights, 1, nullptr, nullptr, 0}, 1, false},
    /* bottom */    {{kContWeights, 1, nullptr, nullptr, 0}, 1, false},
    /* side   */    {{kContWeights, 1, nullptr, nullptr, 0}, 1, false},
    /* seed params */ nullptr, 0
};

static RecipeDesc kRegionalRecipe = {
    {kRegWeights, 1, nullptr, nullptr, 0},
    nullptr, 0,
    {{kRegWeights, 1, nullptr, nullptr, 0}, 1, false},
    {{kRegWeights, 1, nullptr, nullptr, 0}, 1, false},
    {{kRegWeights, 1, nullptr, nullptr, 0}, 1, false},
    nullptr, 0
};

static RecipeDesc kLocalRecipe = {
    {kLocalWeights, 2, nullptr, nullptr, 0},
    nullptr, 0,
    /* top: 2-voxel soil cap */
    {{kSoilWeights, 1, nullptr, nullptr, 0}, 2, true},
    {{kLocalWeights, 2, nullptr, nullptr, 0}, 1, false},
    {{kLocalWeights, 2, nullptr, nullptr, 0}, 1, false},
    nullptr, 0
};

// ── Plugin entry ──────────────────────────────────────────────────────────────

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_material(ctx, "continental_rock", kContinentalRock);
    ctx->register_material(ctx, "regional_rock",    kRegionalRock);
    ctx->register_material(ctx, "local_rock",       kLocalRock);
    ctx->register_material(ctx, "soil",             kSoil);

    // Install opaque, visually distinct colours for each rock type.
    // The default cycling palette maps indices 21 → the translucent water slot,
    // making regional_rock render semi-transparent; index 22 renders brownish-red.
    ctx->set_palette_color(ctx, 20, 0xff606050u); // continental_rock: dark slate
    ctx->set_palette_color(ctx, 21, 0xff909090u); // regional_rock:    granite gray
    ctx->set_palette_color(ctx, 22, 0xff78a0c0u); // local_rock:       tan limestone
    ctx->set_palette_color(ctx, 23, 0xff204060u); // soil:             dark brown

    ctx->register_layer_generator(ctx, "continental", continentalGen, nullptr);
    ctx->register_layer_generator(ctx, "regional",    regionalGen,    nullptr);
    ctx->register_layer_generator(ctx, "local",       localGen,       nullptr);
    ctx->register_layer_generator(ctx, "terrain",     terrainGen,     nullptr);

    ctx->register_recipe(ctx, "continental", &kContinentalRecipe);
    ctx->register_recipe(ctx, "regional",    &kRegionalRecipe);
    ctx->register_recipe(ctx, "local",       &kLocalRecipe);

    return 0;
}
