// Reference atmospheric-mist plugin (M17, distance obscurance).
//
// Supplies and animates the renderer's distance-obscurance fog (FogParams) as a
// drifting dust haze. All POLICY lives here — the engine owns only the fog
// mechanism in the chunk shader (Renderer::setFog). The host queries api().sample
// each frame and forwards the result to the renderer; nothing crosses the engine
// boundary but a plain POD.
//
// This plugin registers NO engine hooks (no PluginContext use beyond init), the
// same shape as the reference input plugins: it just fills the shared api() table.

#include "atmospheric_mist.h"
#include "plugin_api.h"

#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();  // no-op in compiled-in host builds (VOXEL_PLUGIN_NO_ABI_STAMP)

namespace {

mist::Config g_cfg;  // ships with the defaults in atmospheric_mist.h

FogParams sample_impl(double t) {
    FogParams f = g_cfg.base;
    // Breathe the peak density slowly (a sine in [-1,1] scaled by the amplitude),
    // clamped so the haze never goes negative or fully opaque. The band stays put;
    // only the strength drifts, so geometry seems to fade in and out of the dust.
    const double phase = 2.0 * 3.14159265358979323846 * g_cfg.drift_hz * t;
    const float  wobble = g_cfg.drift_amplitude * static_cast<float>(std::sin(phase));
    f.density = std::clamp(g_cfg.base.density + wobble, 0.0f, 1.0f);
    return f;
}

void configure_impl(const mist::Config* cfg) {
    if (cfg) g_cfg = *cfg;
}

}  // namespace

// Unique entry point for compiled-in host/test builds (mirrors the kinematic-body
// plugin), so its api() inline static resolves in one address space.
VOXEL_PLUGIN_EXPORT int atmospheric_mist_plugin_init(PluginContext* /*ctx*/) {
    mist::api().sample    = sample_impl;
    mist::api().configure = configure_impl;
    return 0;
}

#ifndef MIST_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return atmospheric_mist_plugin_init(ctx);
}
#endif
