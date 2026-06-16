// flow plugin (M14, docs/ARCHITECTURE.md §17).
//
// The mandatory fluid-response plugin: the fluid analog of M13's crumble. The
// engine's FluidSystem only simulates a sparse fluid-amount overlay and fires
// on_fluid_event — it never calls apply_edit. This plugin is the actuator that
// turns a Rising event into a real voxel and a Falling event back into empty
// space, via the public edit path, exactly mirroring the detect/respond split.
//
// It also registers the fluid material ("water") and a single demo emitter via
// register_fluid_source, so a host need only load this plugin to see fluid
// simulate, realize as geometry, and flow. Dropping it leaves FluidSystem
// still advancing its overlay and firing events, but the world stays
// byte-identical — the legitimate "fluid never becomes geometry" configuration,
// not a degenerate case.
//
// Build/run: auto-built into <build>/plugins/flow.{so,dll,dylib}, loaded by the
// flow-and-heat demo via PluginManager::loadPlugin.

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

// The fluid material this plugin realizes. Palette index 5 is the engine's
// pre-defined translucent water slot (src/renderer/Palette.h), so realized
// cells render through the existing fluid-surface translucent pass with no
// extra palette setup.
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

// A modest demo emitter: saturates a cell (kSaturationThreshold == 1.0, see
// Tuning.h) in well under a second, slow enough that flow visibly spreads
// rather than instantly filling its basin.
constexpr float kEmitterRate = 2.0f;

void on_fluid_event(const FluidEvent* event, void* /*user_data*/) {
    if (!g_ctx || !event) return;

    if (event->crossing == FieldCrossing::Rising) {
        Voxel v;
        v.material              = water_material();
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

    ctx->register_material(ctx, "water", water_material());
    ctx->register_on_fluid_event(ctx, on_fluid_event, nullptr);

    // The demo's single emitter. register_fluid_source resolves "water" to
    // its palette_index now, at registration time (the register_material_sound
    // pattern), so the FluidEvent this plugin later receives already carries
    // the right material to realize.
    ctx->register_fluid_source(ctx, WorldCoord(0.0, 12.0, 0.0), kEmitterRate, "water");

    return 0;
}
