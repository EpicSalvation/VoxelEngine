#pragma once

// Shared API for the atmospheric-mist reference plugin (M17, distance obscurance).
//
// A small CONTENT plugin that supplies and animates the renderer's fog policy
// (FogParams). It owns no engine state and registers no engine hooks — like the
// reference input plugins it just fills this api() table at voxel_plugin_init and
// the host queries it each frame, passing the result to Renderer::setFog. The fog
// itself is a renderer-tier POLICY (see include/renderer/Fog.h): the engine owns
// the mechanism, plugins like this one own the look.
//
// "Mist/dust": a thin haze that thickens with distance so far geometry dissolves
// into the background — tuned for an open field (e.g. the Asteroid belt miner)
// where the band sits just inside the decompose distance, concealing the coarse→
// fine LOD pop. The haze BREATHES: its peak density drifts slowly over time so the
// field reads as a living medium rather than a static gradient.
//
// Pattern: header-shared api() table, mirroring kinematic_body.h / keyboard_mouse.h.

#include "renderer/Fog.h"

namespace mist {

// Base look of the haze plus the breathing animation. Defaults frame a dusty,
// background-gray field whose band (≈120–300 m) hides a typical coarse-LOD pop.
struct Config {
    FogParams base{
        /*color*/   glm::vec3(0.188f, 0.188f, 0.188f),  // ≈ the 0x303030 clear color
        /*near_m*/  120.0f,
        /*far_m*/   300.0f,
        /*density*/ 0.9f,
    };
    float drift_amplitude = 0.08f;  // peak density breathes by ±this around base.density
    float drift_hz        = 0.05f;  // breathing rate (cycles/second) — slow, ~20 s period
};

struct API {
    // Fog parameters at elapsed time `t` (seconds). Pure function of t and the
    // current config — deterministic, so two clients at the same time agree.
    FogParams (*sample)(double t) = nullptr;

    // Replace the base look / breathing. Optional; sensible defaults ship above.
    void (*configure)(const Config* cfg) = nullptr;
};

// Singleton accessor, filled by the plugin at init and read by the host — the
// same single-writer/single-reader pattern the other reference plugins use.
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace mist
