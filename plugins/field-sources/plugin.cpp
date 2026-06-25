// field-sources plugin (M14, docs/ARCHITECTURE.md §17).
//
// The demo's emitter setup, kept deliberately separate from the `flow`
// responder so the M14 detect/respond split is visible at runtime: this
// plugin owns the SOURCES (a fluid emitter and a heat emitter), `flow` owns
// the RESPONSE (turning a saturated fluid cell into a real voxel). Because the
// emitters live here and stay loaded, unloading `flow` leaves both fields
// still simulating — fed by these emitters — but with no fluid geometry: the
// legitimate "fluid never becomes voxels" configuration, exactly the M13
// detect-only / mandatory-response shape (ARCHITECTURE §17, §13).
//
// Heat has no response plugin at all: temperature is always a field-only
// simulation (the engine never realizes a voxel for a thermal crossing — what
// to do with heat is game policy). The heat emitter is registered here for the
// same owner-tracked, torn-down-on-unload reason the fluid emitter is.
//
// Positions are tuned to the 14-flow-and-heat scene: the fluid emitter sits
// above the left chamber, the heat emitter at the conductive/resistive floor
// seam so the asymmetric spread reads cleanly in the debug heat-map.
//
// Build/run: auto-built into <build>/plugins/field-sources.{so,dll,dylib},
// loaded once (and never unloaded) by the flow-and-heat demo.

#include "plugin_api.h"

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// The fluid material the emitter releases and the `flow` plugin realizes.
// Palette index 5 is the engine's pre-defined translucent water slot
// (src/renderer/Palette.h), so realized cells render through the existing
// translucent pass with no extra palette setup. register_fluid_source resolves
// "water" -> palette_index 5 at registration time, so the FluidEvent the flow
// plugin later receives already carries the right material to realize.
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

// Fluid emitter rate, tuned to the demo tank: fills the left chamber over a few
// seconds and seeps through the porous dam into the right chamber, without
// flooding so fast the process is invisible. The fluid model has no sink, so a
// closed tank fills completely — the seep is the transient, shown by the demo's
// fluid-field tint as the front advances ahead of the realized water.
constexpr float kFluidRate = 15.0f;

// A strong heat emitter so the plume climbs well above kAmbientTemperature
// (20 °C) and the conductivity-driven spread is obvious in the heat-map.
constexpr float kHeatRate = 80.0f;

}  // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    // Material first: register_fluid_source resolves the name to a palette_index
    // immediately, so "water" must already be registered when the source is.
    ctx->register_material(ctx, "water", water_material());

    // Fluid emitter inside the LEFT chamber (voxel ~(10, 7, 16), below the rim so
    // it rains down rather than spilling over the wall): water pools against the
    // left wall and the porous dam at x=12..13, seeps through the dam into the
    // right chamber, and pools against the impermeable right wall at x=18.
    ctx->register_fluid_source(ctx, WorldCoord(10.5, 7.5, 16.5), kFluidRate, "water");

    // Heat emitter at the iron floor (voxel ~(11, 1, 16)): the iron half (x<=12,
    // high conductivity) carries heat far and fast; the rock half (x>=13, low
    // conductivity) barely warms, so the heat-map plume is visibly asymmetric.
    ctx->register_heat_source(ctx, WorldCoord(11.5, 1.5, 16.5), kHeatRate);

    return 0;
}
