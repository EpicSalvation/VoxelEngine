#pragma once

#include <cmath>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// cameraBasis — build an orthonormal camera frame from pitch/yaw/roll that is
// interpreted in a frame whose "up" is an arbitrary world-up vector (M17,
// surface-normal camera orientation; docs/ARCHITECTURE.md §9/§18).
//
// The historical renderer baked a fixed +Y up: forward = (cosP·sinY, sinP,
// cosP·cosY), up = forward × right with right = (cosY, 0, -sinY). Once "down"
// can point any direction (M16 L7's gravity provider), the camera should be able
// to align its up-axis to the local surface normal (the -gravity up) so a player
// standing on the +X face of a body sees a level horizon.
//
// The construction is: build the canonical +Y-up forward/right/up from pitch/yaw
// (and roll about the forward axis), then rotate that whole frame by the rotation
// that maps +Y onto `worldUp`. When worldUp is +Y and roll is 0 the rotation is
// the identity and the canonical formulas are returned UNCHANGED — so the default
// basis is byte-for-byte the pre-M17 one and every existing scene renders
// identically.
// ---------------------------------------------------------------------------
struct CameraBasis {
    glm::dvec3 forward;
    glm::dvec3 right;
    glm::dvec3 up;
};

namespace camerabasis_detail {

// Rotate `v` by the rotation that maps +Y onto the unit vector `u`. Identity
// when u == +Y (so callers that never leave Y-up are unaffected); a 180° flip
// about X for the antipode (u == -Y); Rodrigues' rotation otherwise.
inline glm::dvec3 rotateFromYTo(const glm::dvec3& v, const glm::dvec3& u) {
    const double d = u.y;  // dot(+Y, u)
    if (d >= 1.0 - 1e-12) return v;                                  // already +Y
    if (d <= -1.0 + 1e-12) return glm::dvec3(v.x, -v.y, -v.z);       // antipodal
    // axis = normalize(+Y × u); +Y × u = (u.z, 0, -u.x).
    const glm::dvec3 axis = glm::normalize(glm::dvec3(u.z, 0.0, -u.x));
    const double c = d;                       // cos(angle) = dot(+Y, u)
    const double s = std::sqrt(1.0 - d * d);  // sin(angle), angle in [0, π]
    return v * c + glm::cross(axis, v) * s + axis * (glm::dot(axis, v) * (1.0 - c));
}

}  // namespace camerabasis_detail

// pitch/yaw/roll in radians; worldUp need not be normalized. See header comment
// for the byte-identical default guarantee (worldUp == +Y, roll == 0).
inline CameraBasis cameraBasis(double pitch, double yaw, double roll,
                               const glm::dvec3& worldUp = glm::dvec3(0.0, 1.0, 0.0)) {
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw),   sy = std::sin(yaw);

    glm::dvec3 forward(cp * sy, sp, cp * cy);
    glm::dvec3 right(cy, 0.0, -sy);
    glm::dvec3 up = glm::cross(forward, right);

    // Roll about the forward axis (rotates right/up; skipped — and so bit-exact —
    // at roll == 0, the only value the in-tree demos ever pass).
    if (roll != 0.0) {
        const double cr = std::cos(roll), sr = std::sin(roll);
        const glm::dvec3 r2 = right * cr + up * sr;
        const glm::dvec3 u2 = up * cr - right * sr;
        right = r2;
        up = u2;
    }

    // Rotate the canonical frame so +Y maps onto worldUp. The identity short-circuit
    // inside rotateFromYTo keeps the default (+Y) basis bit-identical.
    const glm::dvec3 u = glm::normalize(worldUp);
    forward = camerabasis_detail::rotateFromYTo(forward, u);
    right   = camerabasis_detail::rotateFromYTo(right, u);
    up      = camerabasis_detail::rotateFromYTo(up, u);

    return {forward, right, up};
}

// Rotate the unit vector `current` toward the unit vector `target` by at most
// `maxRadians`, returning a unit vector. Pure and stateless — the *rate* and the
// per-frame state are the game's to own (this is the slerp step a game runs each
// frame to ANIMATE a camera-up change instead of snapping it, e.g. when a player
// lands on a surface or crosses between gravity bodies). When the remaining angle
// is within the step it returns `target` exactly, so the animation settles
// cleanly; the ~180° case (no unique rotation axis) falls back to a deterministic
// perpendicular axis. A non-positive step leaves `current` unchanged.
//
// Typical use (mirrors cameraBasis's caller): feed the result to BOTH cameraBasis
// (for the look/raycast direction) and Renderer::setCameraUp, so the rendered view
// and the host's picking stay locked together through the animation.
inline glm::dvec3 rotateUpToward(const glm::dvec3& current, const glm::dvec3& target,
                                 double maxRadians) {
    const glm::dvec3 c = glm::normalize(current);
    const glm::dvec3 t = glm::normalize(target);
    if (maxRadians <= 0.0) return c;

    const double d   = glm::clamp(glm::dot(c, t), -1.0, 1.0);
    const double ang = std::acos(d);
    if (ang <= maxRadians || ang < 1e-9) return t;  // arrived (or already aligned)

    glm::dvec3 axis = glm::cross(c, t);
    if (glm::length(axis) < 1e-9) {
        // Antipodal: any axis perpendicular to c works; pick one deterministically.
        axis = glm::cross(c, glm::dvec3(1.0, 0.0, 0.0));
        if (glm::length(axis) < 1e-9) axis = glm::cross(c, glm::dvec3(0.0, 0.0, 1.0));
    }
    axis = glm::normalize(axis);

    // Rodrigues rotation of c about axis by maxRadians.
    const double cs = std::cos(maxRadians), sn = std::sin(maxRadians);
    const glm::dvec3 r = c * cs + glm::cross(axis, c) * sn
                       + axis * (glm::dot(axis, c) * (1.0 - cs));
    return glm::normalize(r);
}
