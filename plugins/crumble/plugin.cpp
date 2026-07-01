// crumble plugin (M13, docs/ARCHITECTURE.md §7).
//
// NOTE: the structural-collapse feature this plugin responds to is experimental
// and likely to change (docs/ARCHITECTURE.md §7) -- proven on fixed dioramas but
// known to misbehave on large streamed surfaces.
//
// The simplest mandatory structural-response plugin: the cave-in / crumble-away
// actuator. It registers on_structural_event and, for each macro the engine
// reports as unstable, CLEARS that macro's terminal child voxels via the public
// edit path (ctx->apply_edit with an empty voxel). Those edits return through
// on_voxel_modified, so the structural pass re-dirties the parent and the NEXT
// frame finds the next ring of newly-unstable macros — the cascade is a feedback
// loop, not an in-engine recursion (§7).
//
// This is the reference for the detect/respond split: the engine only *detects*
// instability and *fires* the event; this plugin owns every write. Dropping it
// means mining never triggers a cave-in — the legitimate Minecraft-style
// configuration, not a degenerate case. The engine ships no default collapse.
//
// Build/run: this is auto-built into <build>/plugins/crumble.{so,dll,dylib} and
// loaded by the structural-collapse demo via PluginManager::loadPlugin.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// Retained so on_structural_event can call apply_edit through the context (the
// same g_ctx pattern as the chat / material-audio plugins).
PluginContext* g_ctx = nullptr;

// Number of terminal child voxels per macro edge: macro_size / child_size,
// rounded. 0/invalid when the event carries no child scale (defensive).
int childRatio(const StructuralEvent& ev) {
    if (ev.child_voxel_size_m <= 0.0 || ev.voxel_size_m <= 0.0) return 0;
    return static_cast<int>(std::llround(ev.voxel_size_m / ev.child_voxel_size_m));
}

// World-space center of the macro's first (min-corner) child voxel.
WorldCoord childOrigin(const StructuralEvent& ev) {
    const double half      = ev.voxel_size_m * 0.5;
    const double childHalf = ev.child_voxel_size_m * 0.5;
    return WorldCoord(ev.position.value.x - half + childHalf,
                      ev.position.value.y - half + childHalf,
                      ev.position.value.z - half + childHalf);
}

void on_structural_event(const StructuralEvent* event, void* /*user_data*/) {
    if (!g_ctx || !event) return;
    const int ratio = childRatio(*event);
    if (ratio <= 0) return;

    const WorldCoord origin = childOrigin(*event);
    const double step = event->child_voxel_size_m;
    const Voxel cleared = Voxel::empty();  // crumble away → empty space

    // Clear every terminal child of the unstable macro. Each apply_edit takes the
    // single edit choke point, so each cleared child re-dirties this macro; the
    // engine collapses it out of the candidate set next frame and re-evaluates the
    // neighbors — the cave-in cascade.
    for (int dz = 0; dz < ratio; ++dz)
        for (int dy = 0; dy < ratio; ++dy)
            for (int dx = 0; dx < ratio; ++dx) {
                const WorldCoord p(origin.value.x + dx * step,
                                   origin.value.y + dy * step,
                                   origin.value.z + dz * step);
                g_ctx->apply_edit(g_ctx, p, &cleared);
            }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;
    ctx->register_on_structural_event(ctx, on_structural_event, nullptr);
    return 0;
}
