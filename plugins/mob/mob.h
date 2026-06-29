#pragma once

// Shared API for the mob reference plugin (M18 mega-demo).
//
// Same pattern as the kinematic-body plugin (kinematic_body.h): the plugin owns
// a registry of simulated bodies and steps them from its register_on_tick hook;
// the host shares state through this header-only function-pointer table. The host
// pushes the player position in each frame (set_player), the plugin runs the
// wander/chase/attack AI and the body physics (via the engine's move_aabb ABI),
// and the host reads back each mob's pose to RENDER it (the engine has no entity
// system, so the front-end draws the body voxels itself) and drains melee hits
// (poll_attack) into the player's health.

#include "WorldCoord.h"

#include <cstdint>

namespace mob {

using MobId = uint32_t;
static constexpr MobId kInvalidMob = 0;

// AI state, surfaced so the host can tint/animate the rendered body per state.
enum class State : uint8_t { Wander, Chase, Attack, Dead };

// Post-step pose of one mob, valid after the tick hook fires.
struct MobState {
    WorldCoord center;          // AABB center (a ~1.8 m tall body)
    double     yaw    = 0.0;    // facing, radians (atan2 of travel/target dir)
    float      health = 0.0f;
    State      state  = State::Wander;
};

// Global function-pointer table filled by the plugin at init.
struct API {
    // Host → plugin: the player's AABB this frame (chase/attack target).
    void (*set_player)(WorldCoord center, double half_x, double half_y, double half_z) = nullptr;

    // Spawn a mob at a world position. Returns its id, or kInvalidMob on failure.
    MobId (*spawn)(WorldCoord center) = nullptr;

    // Live mob count and indexed read of post-step pose (nullptr if out of range).
    uint32_t          (*mob_count)()          = nullptr;
    const MobState*   (*get_mob)(uint32_t i)  = nullptr;

    // Drain one pending melee hit on the player. Returns true and writes the
    // damage amount if a mob landed an attack since the last call; else false.
    bool (*poll_attack)(float* out_damage) = nullptr;

    // Seed the plugin's AI RNG so a run's mob behaviour is reproducible.
    void (*set_seed)(uint64_t seed) = nullptr;

    // Host → plugin: deal damage to the nearest mob within reach of a world
    // position (the player's eye). Returns true if a mob was hit.
    bool (*attack_nearest)(WorldCoord from, double reach, float damage) = nullptr;
};

// Singleton accessor. Plugin fills it during init; host reads it after wiring in.
inline API& api() {
    static API instance;
    return instance;
}

}  // namespace mob
