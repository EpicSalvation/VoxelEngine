// arena plugin — five-layer arena world for the M7b platformer demo.
//
// Layer stack (in config order, largest → smallest voxel size):
//   "foundation"  500 m immutable — solid stone floor slab, Y in [-500, 0)
//   "ramparts"     20 m immutable — perimeter walls + towers, Y in [0, 100)
//   "terraces"     10 m composite → "detail" — elevated platform blocks
//   "props"         2 m immutable — decorative columns on the arena floor
//   "detail"        1 m terminal  — fine walkable surface; terraces decompose here
//
// The five-layer config exercises the validator with integer ratios 25:1, 2:1,
// 5:1, and 2:1, demonstrating immutable, composite, and terminal modes running
// concurrently in one world.
//
// All generators are pure functions of world position (no rand/time/static mutable
// state) so decomposition is deterministic (ARCHITECTURE §4): the terraces and
// detail generators agree at every boundary because they share the same
// inPlatform() predicate, and the terrace occupancy is a conservative superset of
// the detail occupancy (see comment on terraces_generator below).
//
// A feature generator ("key_spots") is registered and applied to detail layer
// chunks after base generation (the M4 feature-generator hook), stamping a
// gold-coloured marker voxel above the top face of each non-goal platform.

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

// ── Palette indices ──────────────────────────────────────────────────────────
constexpr uint8_t kStoneIdx    =  1;  // gray  — floor slab and platform body
constexpr uint8_t kGrassIdx    =  2;  // green — platform top surface
constexpr uint8_t kRampartIdx  = 10;  // dark gray — perimeter wall stone
constexpr uint8_t kPropsIdx    = 14;  // brick red — decorative column props
constexpr uint8_t kLavaIdx     =  9;  // orange-red — hazard lava (M7b Group 4)
constexpr uint8_t kGoalGoldIdx = 12;  // gold yellow — key markers and goal totem

// ── Per-layer voxel sizes passed through user_data ────────────────────────
// The LayerGeneratorFn signature carries no voxel size, so each non-1 m generator
// receives its layer's voxel size through user_data (pointer to a static double).
// The detail generator runs at the implicit 1 m scale and receives nullptr.
const double kFoundationVoxelSizeM = 500.0;
const double kRampartsVoxelSizeM   =  20.0;
const double kTerracesVoxelSizeM   =  10.0;
const double kPropsVoxelSizeM      =   2.0;

inline double voxelSizeFrom(void* user_data) {
    return user_data ? *static_cast<const double*>(user_data) : 1.0;
}

// ── Material helpers ─────────────────────────────────────────────────────────
inline MaterialProperties solid(uint8_t idx, float density) {
    MaterialProperties m;
    m.density             = density;
    m.structural_strength = 0.8f;
    m.hardness            = 0.6f;
    m.palette_index       = idx;
    return m;
}

// ── Arena geometry constants ─────────────────────────────────────────────────
// Arena footprint: X in [0, 500], Z in [0, 500].
// Floor surface at Y = 0 (top face of foundation slab).
// Players spawn near (250, 0, 80) and navigate toward the goal tower.

constexpr double kArenaMin    =   0.0;
constexpr double kArenaMax    = 500.0;
constexpr double kWallThick   =  40.0;   // perimeter wall width (m), 2 rampart voxels
constexpr double kWallHeight  = 100.0;   // wall height (m), 5 rampart voxels
constexpr double kFoundBottom = -500.0;  // foundation slab bottom Y (m)

// ── Platform zones ───────────────────────────────────────────────────────────
// Each zone is { x_min, x_max, y_min, y_max, z_min, z_max }.
// All bounds are multiples of 10 (the terrace voxel size) so the terrace and
// detail generators produce identical boundaries — no fractional edge voxels.
// Platforms ascend in a loop around the arena interior, ending at the goal tower.
struct PlatformZone { double xmin, xmax, ymin, ymax, zmin, zmax; };

constexpr PlatformZone kPlatforms[] = {
    { 180.0, 320.0, 10.0, 20.0, 180.0, 320.0 },  // 0: central start pad
    {  60.0, 180.0, 20.0, 30.0,  60.0, 180.0 },  // 1: NW low
    { 320.0, 440.0, 30.0, 40.0,  60.0, 180.0 },  // 2: NE mid-low
    { 320.0, 440.0, 40.0, 50.0, 320.0, 440.0 },  // 3: SE mid
    {  60.0, 180.0, 50.0, 60.0, 320.0, 440.0 },  // 4: SW high
    { 200.0, 300.0, 60.0, 70.0, 200.0, 300.0 },  // 5: goal tower
};

// Returns true if the world position (wx, wy, wz) — using wy as the voxel bottom —
// falls inside any platform volume.
inline bool inPlatform(double wx, double wy, double wz) {
    for (const PlatformZone& p : kPlatforms)
        if (wx >= p.xmin && wx < p.xmax &&
            wy >= p.ymin && wy < p.ymax &&
            wz >= p.zmin && wz < p.zmax)
            return true;
    return false;
}

// Returns true if the voxel whose minimum corner is at (cellX, cellY, cellZ)
// with size vs overlaps the perimeter wall zone.
inline bool inWall(double cellX, double cellY, double cellZ, double vs) {
    if (cellY + vs <= 0.0 || cellY >= kWallHeight) return false;
    bool xPerim = (cellX + vs > kArenaMin && cellX < kArenaMin + kWallThick) ||
                  (cellX + vs > kArenaMax - kWallThick && cellX < kArenaMax);
    bool zPerim = (cellZ + vs > kArenaMin && cellZ < kArenaMin + kWallThick) ||
                  (cellZ + vs > kArenaMax - kWallThick && cellZ < kArenaMax);
    return xPerim || zPerim;
}

// ── Generators ───────────────────────────────────────────────────────────────

// Foundation (500 m, immutable): solid stone slab, Y in [kFoundBottom, 0).
void foundation_generator(WorldCoord origin, int n, Voxel* out, void* ud) {
    const double vs = voxelSizeFrom(ud);
    const MaterialProperties stone = solid(kStoneIdx, 2700.0f);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double bottom = origin.value.y + y * vs;
                const double top    = bottom + vs;
                out[x + n * (y + n * z)] =
                    (top > kFoundBottom && bottom < 0.0) ? Voxel{stone} : Voxel::empty();
            }
}

// Ramparts (20 m, immutable): perimeter walls, Y in [0, kWallHeight).
void ramparts_generator(WorldCoord origin, int n, Voxel* out, void* ud) {
    const double vs = voxelSizeFrom(ud);
    const MaterialProperties wall = solid(kRampartIdx, 3000.0f);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double cx = origin.value.x + x * vs;
                const double cy = origin.value.y + y * vs;
                const double cz = origin.value.z + z * vs;
                out[x + n * (y + n * z)] =
                    inWall(cx, cy, cz, vs) ? Voxel{wall} : Voxel::empty();
            }
}

// Terraces (10 m, composite): coarse platform stand-ins.
//
// The coarse occupancy is a conservative superset of the fine detail occupancy:
// decomposition only runs on macro voxels this generator marks solid, so any
// detail voxel whose parent terrace voxel is empty is never generated, leaving
// holes in the walkable surface. Because all platform bounds (kPlatforms) are
// multiples of 10 (the terrace voxel size), inPlatform(wx, wy, wz) and
// inPlatform(wx, wy, wz) agree exactly at every voxel boundary — no edge voxels
// are missed in either direction.
void terraces_generator(WorldCoord origin, int n, Voxel* out, void* ud) {
    const double vs = voxelSizeFrom(ud);
    const MaterialProperties block = solid(kStoneIdx, 2500.0f);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double wx = origin.value.x + x * vs;
                const double wy = origin.value.y + y * vs;
                const double wz = origin.value.z + z * vs;
                out[x + n * (y + n * z)] =
                    inPlatform(wx, wy, wz) ? Voxel{block} : Voxel::empty();
            }
}

// Detail (1 m, terminal): fine walkable surface matching terraces' occupancy.
// Grass on the top face (voxel above is empty); stone for all interior faces.
void detail_generator(WorldCoord origin, int n, Voxel* out, void* /*ud*/) {
    const double vs = 1.0;
    const MaterialProperties stone = solid(kStoneIdx, 2700.0f);
    const MaterialProperties grass = solid(kGrassIdx, 1200.0f);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double wx = origin.value.x + x * vs;
                const double wy = origin.value.y + y * vs;
                const double wz = origin.value.z + z * vs;
                Voxel& v = out[x + n * (y + n * z)];
                if (!inPlatform(wx, wy, wz)) {
                    v = Voxel::empty();
                } else {
                    // Grass on the top face, stone everywhere else.
                    v.material = inPlatform(wx, wy + vs, wz) ? stone : grass;
                }
            }
}

// Props (2 m, immutable): decorative stone columns at interior landmark points.
// Column footprints (2 m × 2 m), 4 m tall (two voxels), at eight arena positions.
inline bool isColumn(double cellX, double cellY, double cellZ) {
    if (cellY < 0.0 || cellY >= 4.0) return false;
    static const int64_t kCols[][2] = {
        { 100, 100 }, { 100, 400 }, { 400, 100 }, { 400, 400 },
        { 250, 100 }, { 250, 400 }, { 100, 250 }, { 400, 250 },
    };
    const int64_t ix = static_cast<int64_t>(std::llround(cellX));
    const int64_t iz = static_cast<int64_t>(std::llround(cellZ));
    for (const auto& c : kCols)
        if (ix == c[0] && iz == c[1]) return true;
    return false;
}

void props_generator(WorldCoord origin, int n, Voxel* out, void* ud) {
    const double vs = voxelSizeFrom(ud);
    const MaterialProperties props = solid(kPropsIdx, 1800.0f);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const double cx = origin.value.x + x * vs;
                const double cy = origin.value.y + y * vs;
                const double cz = origin.value.z + z * vs;
                out[x + n * (y + n * z)] =
                    isColumn(cx, cy, cz) ? Voxel{props} : Voxel::empty();
            }
}

// ── Feature generator: key_spots ─────────────────────────────────────────────
// Stamps a single gold-coloured marker voxel just above the top face of each
// non-goal platform. Applied to detail layer chunks after base generation.
// These markers represent the key pickup locations for the M7b game objective
// (the actual collect/win logic arrives in Group 4).
void key_spots_feature(WorldCoord origin, double vs, int n, Voxel* inout, void* /*ud*/) {
    // World positions (wx, wy, wz) of the gold marker voxels.
    // wy = platform y_max (the voxel sits on top of the platform surface).
    static const double kKeys[][3] = {
        { 120.0, 30.0, 120.0 },  // NW platform top
        { 380.0, 40.0, 120.0 },  // NE platform top
        { 380.0, 50.0, 380.0 },  // SE platform top
        { 120.0, 60.0, 380.0 },  // SW platform top
    };
    const double size = vs * static_cast<double>(n);
    const MaterialProperties gold = solid(kGoalGoldIdx, 100.0f);
    for (const auto& kp : kKeys) {
        if (kp[0] < origin.value.x || kp[0] >= origin.value.x + size) continue;
        if (kp[1] < origin.value.y || kp[1] >= origin.value.y + size) continue;
        if (kp[2] < origin.value.z || kp[2] >= origin.value.z + size) continue;
        const int lx = static_cast<int>((kp[0] - origin.value.x) / vs);
        const int ly = static_cast<int>((kp[1] - origin.value.y) / vs);
        const int lz = static_cast<int>((kp[2] - origin.value.z) / vs);
        if (lx >= 0 && lx < n && ly >= 0 && ly < n && lz >= 0 && lz < n)
            inout[lx + n * (ly + n * lz)] = Voxel{gold};
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_material(ctx, "stone",       solid(kStoneIdx,    2700.0f));
    ctx->register_material(ctx, "grass",       solid(kGrassIdx,    1200.0f));
    ctx->register_material(ctx, "rampart",     solid(kRampartIdx,  3000.0f));
    ctx->register_material(ctx, "props",       solid(kPropsIdx,    1800.0f));
    ctx->register_material(ctx, "hazard-lava", solid(kLavaIdx,      800.0f));
    ctx->register_material(ctx, "goal-gold",   solid(kGoalGoldIdx,  100.0f));

    ctx->register_layer_generator(ctx, "foundation", foundation_generator,
                                  const_cast<double*>(&kFoundationVoxelSizeM));
    ctx->register_layer_generator(ctx, "ramparts",   ramparts_generator,
                                  const_cast<double*>(&kRampartsVoxelSizeM));
    ctx->register_layer_generator(ctx, "terraces",   terraces_generator,
                                  const_cast<double*>(&kTerracesVoxelSizeM));
    ctx->register_layer_generator(ctx, "props",      props_generator,
                                  const_cast<double*>(&kPropsVoxelSizeM));
    ctx->register_layer_generator(ctx, "detail",     detail_generator, nullptr);

    ctx->register_feature_generator(ctx, "key_spots", key_spots_feature, nullptr);
    return 0;
}
