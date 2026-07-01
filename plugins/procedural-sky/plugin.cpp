// Reference procedural-sky plugin (M17, standard skybox support).
//
// Supplies (and optionally animates) the renderer's sky policy (SkyParams) as a
// view-direction gradient. All POLICY lives here — the engine owns only the sky
// mechanism (Renderer::setSky, a camera-centered gradient sphere). The host
// queries api().sample each frame and forwards the result to the renderer;
// nothing crosses the engine boundary but a plain POD.
//
// This plugin registers NO engine hooks (no PluginContext use beyond init), the
// same shape as the atmospheric-mist / range-attenuation fog suppliers: it just
// fills the shared api() table.

#include "procedural_sky.h"
#include "plugin_api.h"

#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();  // no-op in compiled-in host builds (VOXEL_PLUGIN_NO_ABI_STAMP)

namespace {

psky::Config g_cfg;  // ships with the defaults in procedural_sky.h (a static day sky)

// Linear blend between two looks. `enabled` follows `a` (both looks are enabled);
// the three colors and the horizon falloff interpolate.
SkyParams blend(const SkyParams& a, const SkyParams& b, float t) {
    SkyParams s;
    s.enabled         = a.enabled;
    s.zenith          = glm::mix(a.zenith,  b.zenith,  t);
    s.horizon         = glm::mix(a.horizon, b.horizon, t);
    s.ground          = glm::mix(a.ground,  b.ground,  t);
    s.horizon_falloff = a.horizon_falloff + (b.horizon_falloff - a.horizon_falloff) * t;
    return s;
}

SkyParams sample_impl(double t) {
    if (g_cfg.daynight_hz <= 0.0f) return g_cfg.day;   // static: no animation
    // Day/night cycle: blend day↔night by a raised cosine in [0,1] so the look
    // eases into peak night at phase π and back, rather than snapping. The band
    // shape and colors both drift; the sky reads as a slow time-of-day sweep.
    const double phase = 2.0 * 3.14159265358979323846 * g_cfg.daynight_hz * t;
    const float  k = 0.5f * (1.0f - static_cast<float>(std::cos(phase)));  // 0 → 1 → 0
    return blend(g_cfg.day, g_cfg.night, k);
}

void configure_impl(const psky::Config* cfg) {
    if (cfg) g_cfg = *cfg;
}

}  // namespace

// Unique entry point for compiled-in host/test builds (mirrors the other reference
// plugins), so its api() inline static resolves in one address space.
VOXEL_PLUGIN_EXPORT int procedural_sky_plugin_init(PluginContext* /*ctx*/) {
    psky::api().sample    = sample_impl;
    psky::api().configure = configure_impl;
    return 0;
}

#ifndef PSKY_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return procedural_sky_plugin_init(ctx);
}
#endif
