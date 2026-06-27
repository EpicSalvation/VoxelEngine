// Reference range-attenuation plugin (M17, distance obscurance).
//
// Supplies and animates the renderer's distance-obscurance fog (FogParams) as a
// flickering lit radius — the "you can only see so far in here" cave/torch look.
// It rides the SAME fog mechanism as atmospheric-mist (Renderer::setFog); the
// difference is entirely policy: a near-black target color and a band equal to the
// light's reach, wavering over time the way a flame does.
//
// Pure policy: fills the shared api() table at init and registers no engine hooks.

#include "range_attenuation.h"
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

rangefog::Config g_cfg;  // ships with the defaults in range_attenuation.h

FogParams sample_impl(double t) {
    FogParams f = g_cfg.base;
    // Torch flicker: sum two out-of-phase sines (an irregular-looking beat that a
    // single sine lacks), normalized to [-1,1], then shift the lit radius — both
    // near and far — by that fraction of the amplitude. The band keeps its width,
    // so the lit region pulses outward/inward bodily, like a flame brightening.
    const double w = 2.0 * 3.14159265358979323846 * g_cfg.flicker_hz * t;
    const float  flick =
        0.6f * static_cast<float>(std::sin(w)) +
        0.4f * static_cast<float>(std::sin(w * 1.7 + 1.3));  // 0.6+0.4 = unit peak
    const float shift = g_cfg.flicker_amplitude * flick;
    f.near_m = std::max(0.0f, g_cfg.base.near_m + shift);
    f.far_m  = std::max(f.near_m + 0.1f, g_cfg.base.far_m + shift);
    return f;
}

void configure_impl(const rangefog::Config* cfg) {
    if (cfg) g_cfg = *cfg;
}

}  // namespace

// Unique entry point for compiled-in host/test builds (mirrors the kinematic-body
// plugin), so its api() inline static resolves in one address space.
VOXEL_PLUGIN_EXPORT int range_attenuation_plugin_init(PluginContext* /*ctx*/) {
    rangefog::api().sample    = sample_impl;
    rangefog::api().configure = configure_impl;
    return 0;
}

#ifndef RANGEFOG_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return range_attenuation_plugin_init(ctx);
}
#endif
