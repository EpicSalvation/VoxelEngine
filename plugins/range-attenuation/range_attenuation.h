#pragma once

// Shared API for the range-attenuation reference plugin (M17, distance obscurance).
//
// The "basic lighting/attenuation" companion to atmospheric-mist: instead of a
// dust haze that fades to the sky color, this models a LIGHT that reaches only so
// far — beyond its radius the world falls off into darkness, as inside a cave lit
// by a torch or headlamp. It is expressed through the SAME renderer fog mechanism
// (FogParams), because "visibility falls off with range, toward a color" is
// exactly what fog computes — here the color is near-black and the band is the
// lit radius. The light FLICKERS: the visible radius wavers over time the way a
// real flame does, so the dark edge breathes in and out.
//
// Like atmospheric-mist it is pure policy — fills this api() table at init,
// registers no engine hooks — and the host queries sample() each frame and
// forwards the result to Renderer::setFog.

#include "renderer/Fog.h"

namespace rangefog {

// The lit-range look plus its flicker. Defaults model a modest torch: visible to
// ~6 m, fully dark by ~22 m, fading toward near-black.
struct Config {
    FogParams base{
        /*color*/   glm::vec3(0.02f, 0.02f, 0.03f),  // cave darkness (a hair cool, not pure black)
        /*near_m*/  6.0f,    // fully lit within this radius
        /*far_m*/   22.0f,   // darkness by here
        /*density*/ 1.0f,    // beyond far_m the world is the dark color
    };
    float flicker_amplitude = 2.5f;   // the lit radius (near_m & far_m) wavers by ±this (m)
    float flicker_hz        = 7.0f;   // base flame rate (cycles/second)
};

struct API {
    // Fog parameters at elapsed time `t` (seconds). Pure function of t and config.
    FogParams (*sample)(double t) = nullptr;

    // Replace the lit-range look / flicker. Optional; defaults ship above.
    void (*configure)(const Config* cfg) = nullptr;
};

inline API& api() {
    static API instance;
    return instance;
}

}  // namespace rangefog
