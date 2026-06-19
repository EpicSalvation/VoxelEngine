// floating-playspace plugin — a VOLUMETRIC generator for the "Beyond blocks"
// demo (M16 C1).
//
// Where asteroid-field scatters many bodies, this generates ONE finite floating
// island adrift in an otherwise empty void, plus a vast, sparse immutable
// backdrop shell of distant blocks surrounding it. It is the deliberately
// non-Minecraft configuration M16 promises: the interactive layer is a small box
// playspace (the island) with empty space above, below, AND on every side — a
// shape no heightmap can produce — held inside a huge backdrop the player never
// reaches. Together with the camera-relative `StreamingVolume` (M16 L1) the
// island streams as a tight box and the backdrop as a sparse shell, each under
// its own per-layer budget (the heterogeneous-budget acceptance case, L5).
//
// Two generators ship here:
//   - "playspace" — the floating island: a domed top with grass/soil/stone bands
//     tapering to a pointed underside, footprint bounded horizontally so it is a
//     true island and not an infinite slab.
//   - "backdrop"  — a sparse spherical shell of "void rock" far out, the immutable
//     surround. Generated from the field with no save path, so it streams cheaply
//     under the volume + budget (L5) and regenerates identically from seed.
//
// Determinism (ARCHITECTURE §4): both fields are pure functions of world position
// and a fixed seed, so streamed-out chunks regenerate byte-identically. Surface
// relief and shell sparsity are sampled from the engine's noise registry (M16 C2)
// through ctx->resolve_noise rather than a hand-rolled inline copy.

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

constexpr uint64_t kSeed = 0xB3402DB10C5ull;  // "BEYOND BLOCKS"

// ── Island geometry (meters, world space) ───────────────────────────────────
// The island hangs at kCenter. Its footprint is a disc of horizontal radius
// kRadiusXZ; the top domes up to kTopThick above the center plane and the
// underside tapers to a point kRootDepth below it, vanishing at the rim — the
// classic floating-island lens. fbm relief roughens the top and the rim so the
// silhouette is irregular rather than a perfect disc.
const glm::dvec3 kCenter(0.0, 64.0, 0.0);
constexpr double kRadiusXZ   = 48.0;
constexpr double kTopThick   = 7.0;
constexpr double kRootDepth  = 40.0;
constexpr double kTopReliefM = 5.0;   // amplitude of top-surface hills
constexpr double kTopFeatM   = 22.0;  // feature size of those hills
constexpr double kRimFeatM   = 30.0;  // feature size of the rim wobble

// ── Backdrop shell (meters) ─────────────────────────────────────────────────
// A sparse spherical shell centered on the island; only cells whose value-noise
// sample clears kStarThresh are solid, so it reads as scattered distant chunks
// rather than a solid sky. Far enough out that the player never reaches it.
constexpr double kShellInner = 480.0;
constexpr double kShellOuter = 560.0;
constexpr double kStarFeatM  = 18.0;
constexpr float  kStarThresh = 0.74f;

// ── Material bands ──────────────────────────────────────────────────────────
constexpr double kGrassDepth = 1.5;  // top band painted grass
constexpr double kSoilDepth  = 5.0;  // band below grass painted soil; deeper ⇒ stone

constexpr uint8_t kGrassIdx = 40;
constexpr uint8_t kSoilIdx  = 41;
constexpr uint8_t kStoneIdx = 42;
constexpr uint8_t kVoidIdx  = 43;

NoiseFn g_fbm   = nullptr;  // top relief + rim wobble (M16 C2)
NoiseFn g_value = nullptr;  // backdrop shell sparsity

MaterialProperties make(uint8_t palette, float density, float hardness,
                        float strength) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = strength;
    m.thermal_conductivity = 1.0f;
    m.hardness            = hardness;
    m.palette_index       = palette;
    return m;
}

const MaterialProperties kGrass = make(kGrassIdx, 1200.0f, 0.20f, 0.3f);
const MaterialProperties kSoil  = make(kSoilIdx,  1500.0f, 0.30f, 0.4f);
const MaterialProperties kStone = make(kStoneIdx, 2700.0f, 0.70f, 0.9f);
const MaterialProperties kVoid  = make(kVoidIdx,  3200.0f, 0.95f, 1.0f);

RecipeParam scaleParam(double feature) {
    return { "scale", RecipeParamKind::Number, feature, nullptr };
}

double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// Island generator. Solid where the world point lies between the domed top and
// the pointed underside, inside the (noisy) rim. Material bands by depth below the
// local top surface. Pure function of world position.
void playspace_generator(WorldCoord chunk_origin, int grid_size, Voxel* out,
                         void* user_data) {
    const double vs = voxelSizeFrom(user_data);
    const RecipeParam topRp = scaleParam(kTopFeatM);
    const RecipeParam rimRp = scaleParam(kRimFeatM);

    for (int z = 0; z < grid_size; ++z)
    for (int y = 0; y < grid_size; ++y)
    for (int x = 0; x < grid_size; ++x) {
        const glm::dvec3 p(
            chunk_origin.value.x + (x + 0.5) * vs,
            chunk_origin.value.y + (y + 0.5) * vs,
            chunk_origin.value.z + (z + 0.5) * vs);
        Voxel& v = out[x + grid_size * (y + grid_size * z)];

        const glm::dvec3 q = p - kCenter;
        const double r = std::sqrt(q.x * q.x + q.z * q.z);

        // Rim: wobble the footprint radius so the edge is ragged, then reject
        // anything outside it. fbm in [0,1) maps to ±15% radius wobble.
        const float rimN = g_fbm(WorldCoord(p), kSeed, &rimRp, 1, nullptr);
        const double rim = kRadiusXZ * (0.85 + 0.30 * rimN);
        if (r >= rim) { v = Voxel::empty(); continue; }

        const double hr = r / rim;  // 0 at center, →1 at the ragged rim

        // Domed top: highest at the center, settling to the rim, plus hills.
        const float topN = g_fbm(WorldCoord(p), kSeed ^ 0x9Eull, &topRp, 1, nullptr);
        const double top = kCenter.y + kTopThick * (1.0 - hr * hr)
                         + kTopReliefM * (topN - 0.5);
        // Pointed underside: deepest at the center, tapering to nothing at the rim.
        const double bottom = kCenter.y - kRootDepth * (1.0 - hr) * (1.0 - hr);

        if (p.y > top || p.y < bottom) { v = Voxel::empty(); continue; }

        const double depthBelowTop = top - p.y;
        if (depthBelowTop < kGrassDepth)      v.material = kGrass;
        else if (depthBelowTop < kSoilDepth)  v.material = kSoil;
        else                                  v.material = kStone;
    }
}

// Backdrop generator: a sparse immutable shell. Solid only where the point falls
// in the shell band AND its value-noise sample clears the star threshold, so most
// of the shell is empty. Pure function of world position; no save path.
void backdrop_generator(WorldCoord chunk_origin, int grid_size, Voxel* out,
                        void* user_data) {
    const double vs = voxelSizeFrom(user_data);
    const RecipeParam starRp = scaleParam(kStarFeatM);

    for (int z = 0; z < grid_size; ++z)
    for (int y = 0; y < grid_size; ++y)
    for (int x = 0; x < grid_size; ++x) {
        const glm::dvec3 p(
            chunk_origin.value.x + (x + 0.5) * vs,
            chunk_origin.value.y + (y + 0.5) * vs,
            chunk_origin.value.z + (z + 0.5) * vs);
        Voxel& v = out[x + grid_size * (y + grid_size * z)];

        const glm::dvec3 q = p - kCenter;
        const double d = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        if (d < kShellInner || d >= kShellOuter) { v = Voxel::empty(); continue; }

        const float star = g_value(WorldCoord(p), kSeed, &starRp, 1, nullptr);
        v = (star > kStarThresh) ? Voxel{kVoid} : Voxel::empty();
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Consume the built-in relief / sparsity noise (M16 C2); fail loudly on an
    // unknown id per the §6 contract.
    g_fbm   = ctx->resolve_noise(ctx, "fbm");
    g_value = ctx->resolve_noise(ctx, "value");
    if (!g_fbm || !g_value)
        return 1;

    ctx->register_material(ctx, "island_grass", kGrass);
    ctx->register_material(ctx, "island_soil",  kSoil);
    ctx->register_material(ctx, "island_stone", kStone);
    ctx->register_material(ctx, "void_rock",    kVoid);

    ctx->set_palette_color(ctx, kGrassIdx, 0xff4caf50u);  // green
    ctx->set_palette_color(ctx, kSoilIdx,  0xff2a5a8bu);  // brown
    ctx->set_palette_color(ctx, kStoneIdx, 0xff808890u);  // gray
    ctx->set_palette_color(ctx, kVoidIdx,  0xff403040u);  // dim violet backdrop

    ctx->register_layer_generator(ctx, "playspace", playspace_generator, nullptr);
    ctx->register_layer_generator(ctx, "backdrop",  backdrop_generator,  nullptr);
    return 0;
}
