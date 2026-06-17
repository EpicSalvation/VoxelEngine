// falling-debris plugin (M13, docs/ARCHITECTURE.md §7).
//
// The alternative mandatory structural-response plugin. Where `crumble` deletes
// an unstable macro, this one RELOCATES its mass: it clears the unstable macro's
// terminal voxels and places a block of "debris" material one macro step down,
// so the collapse looks like material falling and piling rather than vanishing.
//
// The whole point is that the SAME engine event (on_structural_event) drives a
// completely different game feel with ZERO engine change — proof the detect/
// respond split (§7) keeps all policy in the plugin. Swap crumble for this one in
// the structural-collapse demo and the cave-in becomes a rockslide.
//
// "Down" is a plugin-side policy choice (-Y), not an engine assumption: the
// support model is axis-free (§7) and generalized gravity is M16's concern. A
// debris-response plugin for a radial-gravity world would simply relocate toward
// the body's center instead.
//
// Build/run: auto-built into <build>/plugins/falling-debris.{so,dll,dylib} and
// loaded by the structural-collapse demo via PluginManager::loadPlugin.

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

PluginContext* g_ctx = nullptr;

// Palette slot the plugin claims for debris (paired with a brown colour below).
constexpr uint8_t kDebrisPalette = 12;

// The voxel a relocated macro is rebuilt from. structural_strength is low but
// above tuning::physics::kMinSupportStrength, so a settled pile bridges only a
// short span (it slumps rather than standing as a tower) — the rubble feel.
Voxel debrisVoxel() {
    Voxel v;
    v.material.density              = 1500.0f;
    v.material.structural_strength  = 0.5f;
    v.material.thermal_conductivity = 0.4f;
    v.material.porosity             = 0.3f;
    v.material.hardness             = 0.2f;
    v.material.palette_index        = kDebrisPalette;
    return v;
}

int childRatio(const StructuralEvent& ev) {
    if (ev.child_voxel_size_m <= 0.0 || ev.voxel_size_m <= 0.0) return 0;
    return static_cast<int>(std::llround(ev.voxel_size_m / ev.child_voxel_size_m));
}

// Min-corner child-voxel center of a macro whose center is `macroCenter`.
WorldCoord childOrigin(WorldCoord macroCenter, double macroSize, double childSize) {
    const double half      = macroSize * 0.5;
    const double childHalf = childSize * 0.5;
    return WorldCoord(macroCenter.value.x - half + childHalf,
                      macroCenter.value.y - half + childHalf,
                      macroCenter.value.z - half + childHalf);
}

// Fill (or clear) every terminal child of the macro centered at `macroCenter`.
void editMacro(const StructuralEvent& ev, WorldCoord macroCenter, const Voxel& v) {
    const int ratio = childRatio(ev);
    if (ratio <= 0) return;
    const double step = ev.child_voxel_size_m;
    const WorldCoord origin =
        childOrigin(macroCenter, ev.voxel_size_m, ev.child_voxel_size_m);
    for (int dz = 0; dz < ratio; ++dz)
        for (int dy = 0; dy < ratio; ++dy)
            for (int dx = 0; dx < ratio; ++dx)
                g_ctx->apply_edit(g_ctx,
                                  WorldCoord(origin.value.x + dx * step,
                                             origin.value.y + dy * step,
                                             origin.value.z + dz * step),
                                  &v);
}

void on_structural_event(const StructuralEvent* event, void* /*user_data*/) {
    if (!g_ctx || !event) return;

    // Clear the unstable macro (it "falls away" from here)...
    const Voxel empty = Voxel::empty();
    editMacro(*event, event->position, empty);

    // ...and rebuild it as debris one macro step down. If the macro below is air
    // the debris lands there and re-evaluates next frame (it may fall further and
    // settle on an anchor); if it is already solid the pile simply rests on it.
    const WorldCoord below(event->position.value.x,
                           event->position.value.y - event->voxel_size_m,
                           event->position.value.z);
    editMacro(*event, below, debrisVoxel());
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;

    // A brown so a falling rubble pile reads distinctly from the source material.
    ctx->set_palette_color(ctx, kDebrisPalette, 0xFF3A5A78);  // ABGR
    MaterialProperties debris = debrisVoxel().material;
    ctx->register_material(ctx, "debris", debris);

    ctx->register_on_structural_event(ctx, on_structural_event, nullptr);
    return 0;
}
