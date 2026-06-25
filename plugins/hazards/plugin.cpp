// hazards plugin — lava pool feature generator for the M7b arena platformer.
//
// Stamps a small cluster of lava voxels at the top surface of each non-goal
// platform (platforms 0–4), creating deadly floor patches the player must avoid.
//
// Registration: one feature generator ("hazard_pools") applied to "detail"-layer
// chunks after base generation.  No materials are registered here — the
// "hazard-lava" material (palette index 9, orange-red) is already registered by
// the arena plugin at startup.
//
// Runtime toggle (M4 live-toggle pattern): unload this plugin and evict all
// resident detail chunks.  The terraces re-decompose on approach and the freshly
// generated detail chunks omit the hazard feature, reverting the arena exactly.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// Matches arena plugin kLavaIdx.
constexpr uint8_t kLavaIdx = 9;

// Lava voxel world positions { wx, wy, wz } — bottom-left corner of each 1 m
// lava voxel. All positions are on the top-face row of each non-goal platform
// (wy = platform.y_max - 1) and inside the platform X/Z bounds.
//
// Platform y_max values (detail voxel top-row y index):
//   platform 0 (start pad, y=[10,20)): top voxels at wy=19
//   platform 1 (NW,        y=[20,30)): top voxels at wy=29
//   platform 2 (NE,        y=[30,40)): top voxels at wy=39
//   platform 3 (SE,        y=[40,50)): top voxels at wy=49
//   platform 4 (SW,        y=[50,60)): top voxels at wy=59
static const double kLavaPos[][3] = {
    // Platform 0 — central start pad (x=[180,320), z=[180,320))
    { 200.0, 19.0, 200.0 }, { 201.0, 19.0, 200.0 }, { 200.0, 19.0, 201.0 },
    // Platform 1 — NW (x=[60,180), z=[60,180))
    {  85.0, 29.0,  85.0 }, {  86.0, 29.0,  85.0 }, {  85.0, 29.0,  86.0 },
    // Platform 2 — NE (x=[320,440), z=[60,180))
    { 415.0, 39.0,  85.0 }, { 416.0, 39.0,  85.0 }, { 415.0, 39.0,  86.0 },
    // Platform 3 — SE (x=[320,440), z=[320,440))
    { 415.0, 49.0, 415.0 }, { 416.0, 49.0, 415.0 }, { 415.0, 49.0, 416.0 },
    // Platform 4 — SW (x=[60,180), z=[320,440))
    {  85.0, 59.0, 415.0 }, {  86.0, 59.0, 415.0 }, {  85.0, 59.0, 416.0 },
};

void hazard_pools_feature(WorldCoord origin, double vs, int n, Voxel* inout,
                          const RecipeParam* /*params*/, size_t /*param_count*/,
                          uint64_t /*seed*/, void* /*ud*/) {
    const double size = vs * static_cast<double>(n);
    MaterialProperties lava{};
    lava.density             = 800.0f;
    lava.structural_strength = 0.0f;
    lava.hardness            = 0.0f;
    lava.palette_index       = kLavaIdx;

    for (const auto& lp : kLavaPos) {
        if (lp[0] < origin.value.x || lp[0] >= origin.value.x + size) continue;
        if (lp[1] < origin.value.y || lp[1] >= origin.value.y + size) continue;
        if (lp[2] < origin.value.z || lp[2] >= origin.value.z + size) continue;
        const int lx = static_cast<int>((lp[0] - origin.value.x) / vs);
        const int ly = static_cast<int>((lp[1] - origin.value.y) / vs);
        const int lz = static_cast<int>((lp[2] - origin.value.z) / vs);
        if (lx >= 0 && lx < n && ly >= 0 && ly < n && lz >= 0 && lz < n)
            inout[lx + n * (ly + n * lz)] = Voxel{lava};
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    ctx->register_feature_generator(ctx, "hazard_pools", hazard_pools_feature, nullptr);
    return 0;
}
