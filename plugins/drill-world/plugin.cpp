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

// ── Layer generators ──────────────────────────────────────────────────────────

// Continental (512 m): solid dark-rock slab up to ~1024 m altitude.
static void continentalGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 512.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double cy = origin.value.y + (y + 0.5) * vs;
                const double limit = 1024.0 + 256.0 * (double)valueNoise(
                    origin.value.x + x*vs, 0, origin.value.z + z*vs, 4096.0, 0xC041C041C041ull);
                out[x + n*(y + n*z)] = (cy < limit) ? Voxel{kContinentalRock} : Voxel::empty();
            }
}

// Regional (64 m): solid gray-granite slab up to ~800 m altitude.
static void regionalGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 64.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double cy = origin.value.y + (y + 0.5) * vs;
                const double limit = 800.0 + 64.0 * (double)valueNoise(
                    origin.value.x + x*vs, 0, origin.value.z + z*vs, 512.0, 0xE6404E6404ull);
                out[x + n*(y + n*z)] = (cy < limit) ? Voxel{kRegionalRock} : Voxel::empty();
            }
}

// Local (8 m): solid tan-limestone slab up to ~700 m altitude.
static void localGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 8.0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double cy = origin.value.y + (y + 0.5) * vs;
                const double limit = 700.0 + 80.0 * (double)valueNoise(
                    origin.value.x + x*vs, 0, origin.value.z + z*vs, 64.0, 0xA0CA1A0CA1ull);
                out[x + n*(y + n*z)] = (cy < limit) ? Voxel{kLocalRock} : Voxel::empty();
            }
}

// Terrain (1 m): detailed surface — soil cap over rock, cave voids.
static void terrainGen(WorldCoord origin, int n, Voxel* out, void*) {
    const double vs = 1.0;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const double wx = origin.value.x + (x + 0.5) * vs;
            const double wz = origin.value.z + (z + 0.5) * vs;
            const double height = 640.0 + 60.0 * (double)valueNoise(wx, 0, wz, 32.0, 0xBEEF1234ull);
            for (int y = 0; y < n; ++y) {
                const double wy = origin.value.y + (y + 0.5) * vs;
                const int idx = x + n*(y + n*z);
                if (wy >= height) { out[idx] = Voxel::empty(); continue; }
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

extern "C" int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_material(ctx, "continental_rock", kContinentalRock);
    ctx->register_material(ctx, "regional_rock",    kRegionalRock);
    ctx->register_material(ctx, "local_rock",       kLocalRock);
    ctx->register_material(ctx, "soil",             kSoil);

    ctx->register_layer_generator(ctx, "continental", continentalGen, nullptr);
    ctx->register_layer_generator(ctx, "regional",    regionalGen,    nullptr);
    ctx->register_layer_generator(ctx, "local",       localGen,       nullptr);
    ctx->register_layer_generator(ctx, "terrain",     terrainGen,     nullptr);

    ctx->register_recipe(ctx, "continental", &kContinentalRecipe);
    ctx->register_recipe(ctx, "regional",    &kRegionalRecipe);
    ctx->register_recipe(ctx, "local",       &kLocalRecipe);

    return 0;
}
