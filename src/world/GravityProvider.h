#pragma once

#include <glm/glm.hpp>

#include "WorldCoord.h"

// ---------------------------------------------------------------------------
// GravityProvider — the engine's "down" *policy* (M16, L7; docs/ARCHITECTURE.md
// §18).
//
// Gravity is a policy the game or a plugin supplies, the same way authority and
// interest are policies (§15) — NOT a force baked into the engine. The single
// seam every axis-aware system reads is `gravityAt(WorldCoord) -> dvec3`, a
// per-position "down" vector that may vary per voxel (a radial well) and per
// frame (a moving body). The engine default is constant -Y, so every existing
// demo and test — which assume Y-up — is byte-for-byte unchanged.
//
// Consumers (the canonical "down" L2 and L3 read from):
//   * collision grounding (VoxelCollision::moveAABB) — "blocked along gravity"
//     is what makes a surface a floor, so a player can stand on the +X face of
//     an asteroid (L2).
//   * fluid flow (FluidSystem) — drain points downhill relative to this vector;
//     zero-g degenerates to pure pressure equalization (L3).
//   * face roles (materialfaces / RecipeDesc boundary) — the geometric face most
//     opposing gravity is the "up"/top face (G1/G2, via axisrole::roleOf).
//
// The provider is a small value type (copyable, no captured `this`), so it can
// be stored by value on a consumer and handed around freely. The built-in
// shapes — constant, radial, zero-g — cover the M16 demos (Beyond blocks's
// zero-g, Asteroid belt miner's per-body radial well); `custom` wraps an
// out-of-tree function pointer for anything else.
// ---------------------------------------------------------------------------
class GravityProvider {
public:
    // An out-of-tree provider: returns the gravity ("down") vector at pos.
    using Fn = glm::dvec3 (*)(WorldCoord pos, void* user_data);

    GravityProvider() = default;  // engine default: constant -Y

    // The gravity ("down") vector at pos. Magnitude is meaningful for the
    // built-in shapes only as a direction carrier — the axis-agnostic consumers
    // care about direction (and zero for zero-g), not the m/s² magnitude, which
    // each demo's kinematic step still applies itself.
    glm::dvec3 gravityAt(const WorldCoord& pos) const {
        switch (kind_) {
            case Kind::Constant:
                return dir_;
            case Kind::ZeroG:
                return glm::dvec3(0.0, 0.0, 0.0);
            case Kind::Radial: {
                const glm::dvec3 toCenter = center_.value - pos.value;
                const double len = glm::length(toCenter);
                if (len <= kEps) return glm::dvec3(0.0, 0.0, 0.0);  // at the center
                return (toCenter / len) * strength_;                 // unit·strength toward center
            }
            case Kind::Custom:
                return fn_ ? fn_(pos, user_) : glm::dvec3(0.0, -1.0, 0.0);
        }
        return glm::dvec3(0.0, -1.0, 0.0);
    }

    // Constant gravity in a fixed direction (the engine default is constant -Y).
    static GravityProvider constant(const glm::dvec3& dir) {
        GravityProvider g;
        g.kind_ = Kind::Constant;
        g.dir_  = dir;
        return g;
    }

    // A radial well: "down" is the unit vector toward `center`, scaled by
    // `strength`. At the center exactly, gravity is zero (no defined direction).
    static GravityProvider radial(const WorldCoord& center, double strength = 1.0) {
        GravityProvider g;
        g.kind_     = Kind::Radial;
        g.center_   = center;
        g.strength_ = strength;
        return g;
    }

    // No gravity anywhere — the zero vector. Collision reports no grounded state
    // and fluid equalizes pressure in all 6 directions with no preferred axis.
    static GravityProvider zeroG() {
        GravityProvider g;
        g.kind_ = Kind::ZeroG;
        return g;
    }

    // Wrap an out-of-tree function pointer + user_data.
    static GravityProvider custom(Fn fn, void* user_data) {
        GravityProvider g;
        g.kind_ = Kind::Custom;
        g.fn_   = fn;
        g.user_ = user_data;
        return g;
    }

private:
    enum class Kind { Constant, Radial, ZeroG, Custom };

    static constexpr double kEps = 1e-9;

    Kind       kind_     = Kind::Constant;
    glm::dvec3 dir_      = glm::dvec3(0.0, -1.0, 0.0);  // Constant
    WorldCoord center_   = WorldCoord();                // Radial
    double     strength_ = 1.0;                          // Radial
    Fn         fn_       = nullptr;                       // Custom
    void*      user_     = nullptr;                       // Custom
};
