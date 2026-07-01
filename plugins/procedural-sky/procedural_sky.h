#pragma once

// Shared API for the procedural-sky reference plugin (M17, standard skybox
// support; docs/m17-skybox-evaluation.md).
//
// A small CONTENT plugin that supplies (and optionally animates) the renderer's
// sky policy (SkyParams, include/renderer/Sky.h). It owns no engine state and
// registers no engine hooks — like atmospheric-mist / range-attenuation it just
// fills this api() table at voxel_plugin_init and the host queries it each frame,
// passing the result to Renderer::setSky. The sky itself is a renderer-tier
// POLICY: the engine owns the MECHANISM (a camera-centered view-direction gradient
// drawn behind the scene), plugins like this one own the look.
//
// Three canonical looks ship as pure-data PRESETS (usable directly, no ABI call):
// an atmospheric day sky, a warm dusk, and a near-uniform dark SPACE backdrop —
// the last being what the planned M19 "No Man's Voxel" demo needs when the player
// flies out of a world's bounds. A configured day↔night cycle animates the look
// over time (the canonical reason a sky is time-parameterized), defaulting to a
// static day sky so a game that just wants a fixed backdrop sets one preset.
//
// Pattern: header-shared api() table, mirroring atmospheric_mist.h / kinematic_body.h.

#include "renderer/Sky.h"

namespace psky {

// Atmospheric planet sky: blue zenith, pale horizon band, muted ground.
inline SkyParams daySky() {
    SkyParams s;
    s.enabled = true;
    s.zenith  = glm::vec3(0.25f, 0.50f, 0.90f);
    s.horizon = glm::vec3(0.70f, 0.80f, 0.95f);
    s.ground  = glm::vec3(0.32f, 0.30f, 0.27f);
    s.horizon_falloff = 0.5f;
    return s;
}

// Warm dusk: a deep-blue zenith over a sunset-orange horizon — the "night" end of
// the day/night cycle below (and a fine fixed backdrop on its own).
inline SkyParams duskSky() {
    SkyParams s;
    s.enabled = true;
    s.zenith  = glm::vec3(0.05f, 0.06f, 0.15f);
    s.horizon = glm::vec3(0.55f, 0.30f, 0.20f);
    s.ground  = glm::vec3(0.05f, 0.04f, 0.05f);
    s.horizon_falloff = 0.7f;
    return s;
}

// Space backdrop: a near-uniform dark blue-black in every direction — no bright
// horizon, no ground. This is the M19 "No Man's Voxel" look. (A textured star/
// nebula cubemap is a recorded follow-on — see docs/m17-skybox-evaluation.md.)
inline SkyParams spaceSky() {
    SkyParams s;
    s.enabled = true;
    s.zenith  = glm::vec3(0.010f, 0.010f, 0.030f);
    s.horizon = glm::vec3(0.020f, 0.020f, 0.050f);
    s.ground  = glm::vec3(0.008f, 0.008f, 0.020f);
    s.horizon_falloff = 1.0f;
    return s;
}

// Base look plus the optional day/night animation. Defaults to a static day sky.
struct Config {
    SkyParams day   = daySky();     // the look at cycle phase 0 (and when static)
    SkyParams night = duskSky();    // the look at cycle phase π (peak "night")
    float     daynight_hz = 0.0f;   // 0 ⇒ static `day`; >0 ⇒ blend day↔night over time
};

struct API {
    // Sky parameters at elapsed time `t` (seconds). Pure function of t and the
    // current config — deterministic, so two clients at the same time agree.
    SkyParams (*sample)(double t) = nullptr;

    // Replace the base looks / cycle rate. Optional; sensible defaults ship above.
    void (*configure)(const Config* cfg) = nullptr;
};

// Singleton accessor, filled by the plugin at init and read by the host — the
// same single-writer/single-reader pattern the other reference plugins use.
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace psky
