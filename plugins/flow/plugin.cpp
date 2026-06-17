// flow plugin (M14, docs/ARCHITECTURE.md §17).
//
// The mandatory fluid-response plugin: the fluid analog of M13's crumble. The
// engine's FluidSystem only simulates a sparse fluid-amount overlay and fires
// on_fluid_event — it never calls apply_edit. This plugin is the actuator that
// turns a Rising event into a real voxel and a Falling event back into empty
// space, via the public edit path, exactly mirroring the detect/respond split.
//
// It owns the RESPONSE only, not the sources: the emitters live in the separate
// `field-sources` plugin (the demo's setup). That separation is the point —
// with the emitters elsewhere and still loaded, unloading this plugin leaves
// FluidSystem advancing its overlay and firing events while the world stays
// byte-identical (no realized geometry): the legitimate "fluid never becomes
// geometry" configuration, not a degenerate case. The FluidEvent already
// carries the palette_index resolved from the emitter's material, so the
// responder needs no material registry of its own.
//
// Build/run: auto-built into <build>/plugins/flow.{so,dll,dylib}, loaded by the
// flow-and-heat demo via PluginManager::loadPlugin (toggled at runtime).

#include "plugin_api.h"
#include "world/Voxel.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

namespace {

// Retained so on_fluid_event can call apply_edit through the context (the
// same g_ctx pattern as crumble / material-audio).
PluginContext* g_ctx = nullptr;

// Material properties for a realized fluid voxel. The palette_index that
// actually renders comes from the event (resolved from the emitter's
// material); these are the physical props the placed voxel carries. porosity
// 1.0 so a realized cell is itself fully permeable — fluid flows on through it.
MaterialProperties water_material() {
    MaterialProperties water;
    water.density              = 1000.0f;
    water.structural_strength  = 0.0f;
    water.thermal_conductivity = 0.6f;
    water.porosity             = 1.0f;
    water.hardness             = 0.0f;
    water.palette_index        = 5;
    return water;
}

void on_fluid_event(const FluidEvent* event, void* /*user_data*/) {
    if (!g_ctx || !event) return;

    if (event->crossing == FieldCrossing::Rising) {
        Voxel v;
        v.material               = water_material();
        v.material.palette_index = event->palette_index;
        g_ctx->apply_edit(g_ctx, event->position, &v);
    } else {
        const Voxel cleared = Voxel::empty();
        g_ctx->apply_edit(g_ctx, event->position, &cleared);
    }
}

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;
    ctx->register_on_fluid_event(ctx, on_fluid_event, nullptr);
    return 0;
}
