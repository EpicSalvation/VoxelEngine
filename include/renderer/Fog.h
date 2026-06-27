#pragma once

#include <algorithm>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// FogParams — the renderer's distance-obscurance (atmospheric falloff) seam
// (M17, docs/ARCHITECTURE.md §9/§18).
//
// The decomposition cascade stages coarse→fine geometry at decoupled distances
// (the Asteroid belt miner refines a body's 4 m silhouette at 280 m and its 1 m
// mineable grid only within 90 m). Drawn at full clarity to the far clip, that
// transition — and the chunk-load boundary at the view-distance edge — is baldly
// visible: detail SNAPS into place rather than resolving out of the distance.
//
// Fog is the depth cue that hides the pop: geometry emerges out of murk as it
// refines. Crucially this is a POLICY, not a baked engine force — the same shape
// as gravity (L7) and authority (§15). The renderer only owns the MECHANISM
// (mix the shaded fragment toward a fog color by a distance-driven factor); the
// fog color, the near/far band, and how (or whether) they animate are supplied
// per-frame by the game — typically from a content plugin (atmospheric-mist,
// range-attenuation). Tuning the band to sit just inside each layer's decompose
// distance conceals the transition entirely.
//
// The default (density == 0) disables fog so every existing scene renders
// byte-identically: the shader's mix() collapses to the un-fogged fragment when
// the factor is 0 (see shaders/fs_voxel.sc), and fogFactor() below — the CPU
// mirror of that math — returns 0 for any distance.
// ---------------------------------------------------------------------------
struct FogParams {
    glm::vec3 color{0.0f, 0.0f, 0.0f};  // color geometry fades toward (usually the clear color)
    float     near_m  = 0.0f;           // distance (m) at which fog begins
    float     far_m   = 0.0f;           // distance (m) at which fog reaches full strength
    float     density = 0.0f;           // max fog strength in [0,1]; 0 disables fog (byte-identical)
};

// The fog blend factor at view-space distance `dist` (metres), in [0, density].
// This is the EXACT math the fragment shader runs (shaders/fs_voxel.sc): a linear
// near→far ramp clamped to [0,1] and scaled by the max density. Kept here so the
// CPU side (tests, and the supplier plugins) can reason about the same curve the
// GPU applies. With density 0 the result is 0 for every distance → no fog.
inline float fogFactor(const FogParams& f, float dist) {
    const float denom = std::max(f.far_m - f.near_m, 1e-4f);
    float t = (dist - f.near_m) / denom;
    t = std::clamp(t, 0.0f, 1.0f);
    return t * f.density;
}
