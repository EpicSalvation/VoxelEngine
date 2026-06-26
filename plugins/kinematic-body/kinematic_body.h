#pragma once

// Shared API for the kinematic-body reference plugin (M17 B1).
//
// Both the plugin (plugin.cpp) and any host/demo that uses it include this
// header. The plugin fills the global API table at init; the host calls its
// function pointers after loading the plugin to create bodies, set per-frame
// input, and query post-step state.
//
// Pattern: the asteroid-field plugin (M16) shares geometry via a header-only
// file included by both sides. This plugin extends the pattern to a runtime
// function-pointer table, since the host must interact with the plugin's body
// registry at runtime (create/destroy bodies, set input, read state).

#include "WorldCoord.h"

#include <cstdint>

namespace kinbody {

// Opaque body handle. kInvalidBody (0) denotes a failed or absent body.
using BodyId = uint32_t;
static constexpr BodyId kInvalidBody = 0;

// Configuration for a new body. All fields have sensible defaults matching
// the engine's existing demo conventions (0.6×1.8×0.6 player, 25 m/s²
// gravity, 8 m/s walk, 9 m/s jump).
struct BodyDesc {
    WorldCoord center;
    double half_x       = 0.3;    // AABB half-extent X (meters)
    double half_y       = 0.9;    // AABB half-extent Y (meters)
    double half_z       = 0.3;    // AABB half-extent Z (meters)
    double eye_offset   = 0.7;    // camera height above AABB center
    double walk_speed   = 8.0;    // horizontal movement speed (m/s)
    double gravity_accel = 25.0;  // gravity acceleration (m/s²)
    double jump_speed   = 9.0;    // initial upward velocity on jump (m/s)
    double gravity_dir_x = 0.0;   // gravity direction X (unit vector)
    double gravity_dir_y = -1.0;  // gravity direction Y
    double gravity_dir_z = 0.0;   // gravity direction Z
};

// Per-frame input for a body. The host computes the wish direction from
// camera yaw and key state and passes it here; the plugin applies walk_speed
// and integrates gravity.
struct BodyInput {
    double wish_x = 0.0;   // world-space horizontal wish direction X
    double wish_y = 0.0;   // world-space horizontal wish direction Y (usually 0)
    double wish_z = 0.0;   // world-space horizontal wish direction Z
    bool   jump   = false;  // true the frame the player presses jump
};

// Post-step body state, valid after the tick hook fires.
struct BodyState {
    WorldCoord center;                    // AABB center after collision resolve
    double vel_x = 0.0, vel_y = 0.0, vel_z = 0.0;  // current velocity (m/s)
    bool   grounded = false;              // resting on a surface along gravity
    bool   hit_x = false, hit_y = false, hit_z = false;  // per-axis wall contact
};

// Global function-pointer table filled by the plugin at init.
struct API {
    // Create a kinematic body. Returns its handle, or kInvalidBody on failure.
    BodyId (*create_body)(const BodyDesc* desc) = nullptr;

    // Destroy a body. Idempotent for kInvalidBody.
    void (*destroy_body)(BodyId id) = nullptr;

    // Set this frame's input for a body. Consumed and cleared each tick.
    void (*set_input)(BodyId id, const BodyInput* input) = nullptr;

    // Read post-step state. Returns nullptr for an invalid id.
    const BodyState* (*get_state)(BodyId id) = nullptr;

    // Update a body's gravity direction and acceleration at runtime (e.g. for
    // radial gravity, the host recomputes direction each frame).
    void (*set_gravity)(BodyId id,
                        double dir_x, double dir_y, double dir_z,
                        double accel) = nullptr;

    // Update a body's AABB center directly (e.g. teleport, respawn).
    void (*set_position)(BodyId id, WorldCoord center) = nullptr;

    // Number of live bodies.
    uint32_t (*body_count)() = nullptr;
};

// Singleton accessor. The plugin fills this during voxel_plugin_init; the
// host reads it after loading the plugin. Thread-safe for the single-writer
// (plugin init) / single-reader (host main loop) pattern the engine uses.
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace kinbody
