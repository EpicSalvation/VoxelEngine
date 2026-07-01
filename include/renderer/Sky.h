#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// SkyParams — the renderer's procedural-sky (background) seam
// (M17, docs/architecture.md §9; docs/m17-skybox-evaluation.md).
//
// The renderer's only background before this was a flat CLEAR COLOR
// (Renderer::setClearColor) plus distance fog that fades geometry toward that
// same flat color (Renderer::setFog, include/renderer/Fog.h). That is enough for
// "the void behind the world is one color", but it cannot paint a SKY: no
// horizon, no zenith→horizon gradient, no distinct space backdrop. The planned
// M19 "No Man's Voxel" demo — where the player flies up out of a world's local
// bounds into open space — needs a convincing sky/space backdrop, not a flat
// fill. That is the gap this seam closes.
//
// Like fog (§9), gravity (§18) and authority (§15), the sky is a POLICY, not a
// baked engine force. The renderer owns only the MECHANISM: it draws a
// camera-centered background whose color varies with the VIEW DIRECTION — a
// vertical gradient between a zenith color (straight "up"), a horizon color
// (level with the horizon), and a ground/nadir color (straight "down"), where
// "up" is the camera's up vector (Renderer::setCameraUp, so the gradient tracks
// local gravity on an arbitrary surface). The colors, the width of the horizon
// band, and how (or whether) they ANIMATE (a day/night cycle) are supplied by the
// game — typically from a content plugin (procedural-sky).
//
// This is deliberately a PROCEDURAL gradient, not a cubemap: it needs no texture
// asset pipeline and no new shader (the renderer draws the sky as geometry through
// the existing voxel program), covers both an atmospheric planet sky and a space
// backdrop by choice of colors, and stays a thin floor a game can build on. A
// textured cubemap and a sun/star layer are recorded follow-ons
// (docs/m17-skybox-evaluation.md), the same way the fog seam left richer suppliers
// for later.
//
// The default (enabled == false) draws NO sky, so every existing scene renders
// byte-identically — the flat clear color shows through exactly as before, and a
// renderer whose host never calls setSky() paints no background sphere.
// ---------------------------------------------------------------------------
struct SkyParams {
    bool      enabled = false;                    // false ⇒ no sky (flat clear color; byte-identical)
    glm::vec3 zenith  {0.25f, 0.50f, 0.90f};      // color looking straight "up"
    glm::vec3 horizon {0.70f, 0.80f, 0.95f};      // color at the horizon (perpendicular to up)
    glm::vec3 ground  {0.30f, 0.28f, 0.25f};      // color looking straight "down" (nadir)
    // Shapes the zenith↔horizon (and horizon↔ground) blend. The blend parameter is
    // pow(|cos angle-from-horizon|, falloff): falloff < 1 widens the zenith/ground
    // caps and tightens the horizon band; falloff > 1 does the reverse. 1 is a
    // plain linear-in-cosine gradient. Must be > 0.
    float     horizon_falloff = 0.5f;
};

// The sky color for a view direction `dir`, with `up` the world-space up the
// gradient is measured against (typically the camera up / local -gravity). This is
// the EXACT curve the renderer bakes into the background sphere's per-vertex
// colors, kept here so the CPU side (tests, and the supplier plugins) can reason
// about the same gradient the GPU shows. Neither vector need be normalized; a
// zero-length input is treated as pointing at the horizon (returns `horizon`).
inline glm::vec3 skyColor(const SkyParams& s, const glm::vec3& dir, const glm::vec3& up) {
    const float dl = glm::length(dir);
    const float ul = glm::length(up);
    if (dl < 1e-6f || ul < 1e-6f) return s.horizon;
    // c = cos(angle between the view ray and up): +1 straight up, 0 at the horizon,
    // −1 straight down.
    const float c = glm::dot(dir / dl, up / ul);
    const float falloff = s.horizon_falloff > 1e-4f ? s.horizon_falloff : 1e-4f;
    if (c >= 0.0f) {
        const float t = std::pow(std::min(c, 1.0f), falloff);   // 0 at horizon → 1 at zenith
        return glm::mix(s.horizon, s.zenith, t);
    }
    const float t = std::pow(std::min(-c, 1.0f), falloff);      // 0 at horizon → 1 at nadir
    return glm::mix(s.horizon, s.ground, t);
}
