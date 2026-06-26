// Reference kinematic-body plugin (M17 B1).
//
// Provides a reusable multi-body kinematic system — body registry, gravity
// integration, ground detection, and sweep-and-resolve stepping for N AABB
// bodies — built on the engine's VoxelCollision::moveAABB primitive (exposed
// through the plugin ABI as ctx->move_aabb).
//
// All POLICY lives here, not in the engine core. The engine adds only the
// per-frame tick hook (register_on_tick) and the collision primitive
// (move_aabb); everything else — the body registry, the gravity loop, the
// velocity model — is plugin-owned and swappable.

#include "kinematic_body.h"
#include "plugin_api.h"

#include <glm/glm.hpp>

#include <unordered_map>
#include <cmath>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

// ---------------------------------------------------------------------------
// Internal body representation
// ---------------------------------------------------------------------------

namespace {

struct Body {
    kinbody::BodyId    id;
    kinbody::BodyState state;
    kinbody::BodyInput input;

    double half_x, half_y, half_z;
    double eye_offset;
    double walk_speed;
    double gravity_accel;
    double jump_speed;
    double gravity_dir_x, gravity_dir_y, gravity_dir_z;
};

PluginContext*                               g_ctx      = nullptr;
kinbody::BodyId                              g_nextId   = 1;
std::unordered_map<kinbody::BodyId, Body>    g_bodies;

// ---------------------------------------------------------------------------
// API implementations
// ---------------------------------------------------------------------------

kinbody::BodyId create_body_impl(const kinbody::BodyDesc* desc) {
    if (!desc) return kinbody::kInvalidBody;
    kinbody::BodyId id = g_nextId++;

    Body b{};
    b.id            = id;
    b.half_x        = desc->half_x;
    b.half_y        = desc->half_y;
    b.half_z        = desc->half_z;
    b.eye_offset    = desc->eye_offset;
    b.walk_speed    = desc->walk_speed;
    b.gravity_accel = desc->gravity_accel;
    b.jump_speed    = desc->jump_speed;
    b.gravity_dir_x = desc->gravity_dir_x;
    b.gravity_dir_y = desc->gravity_dir_y;
    b.gravity_dir_z = desc->gravity_dir_z;

    b.state.center  = desc->center;
    b.state.vel_x   = 0.0;
    b.state.vel_y   = 0.0;
    b.state.vel_z   = 0.0;
    b.state.grounded = false;

    b.input = {};

    g_bodies[id] = b;
    return id;
}

void destroy_body_impl(kinbody::BodyId id) {
    g_bodies.erase(id);
}

void set_input_impl(kinbody::BodyId id, const kinbody::BodyInput* input) {
    auto it = g_bodies.find(id);
    if (it == g_bodies.end() || !input) return;
    it->second.input = *input;
}

const kinbody::BodyState* get_state_impl(kinbody::BodyId id) {
    auto it = g_bodies.find(id);
    if (it == g_bodies.end()) return nullptr;
    return &it->second.state;
}

void set_gravity_impl(kinbody::BodyId id,
                      double dir_x, double dir_y, double dir_z,
                      double accel) {
    auto it = g_bodies.find(id);
    if (it == g_bodies.end()) return;
    it->second.gravity_dir_x = dir_x;
    it->second.gravity_dir_y = dir_y;
    it->second.gravity_dir_z = dir_z;
    it->second.gravity_accel = accel;
}

void set_position_impl(kinbody::BodyId id, WorldCoord center) {
    auto it = g_bodies.find(id);
    if (it == g_bodies.end()) return;
    it->second.state.center = center;
    it->second.state.vel_x  = 0.0;
    it->second.state.vel_y  = 0.0;
    it->second.state.vel_z  = 0.0;
}

uint32_t body_count_impl() {
    return static_cast<uint32_t>(g_bodies.size());
}

// ---------------------------------------------------------------------------
// Per-frame tick — the heart of the kinematic system
// ---------------------------------------------------------------------------

void tick(double dt, void* /*user_data*/) {
    if (!g_ctx) return;

    for (auto& [id, body] : g_bodies) {
        const glm::dvec3 gravDir(body.gravity_dir_x,
                                  body.gravity_dir_y,
                                  body.gravity_dir_z);
        const double gravLen = glm::length(gravDir);

        // Gravity integration: accelerate velocity along the gravity direction.
        if (gravLen > 1e-9) {
            const glm::dvec3 gravUnit = gravDir / gravLen;
            const double gravDelta = body.gravity_accel * dt;
            body.state.vel_x += gravUnit.x * gravDelta;
            body.state.vel_y += gravUnit.y * gravDelta;
            body.state.vel_z += gravUnit.z * gravDelta;
        }

        // Jump: if grounded and jump requested, apply jump velocity opposite
        // to gravity direction.
        if (body.input.jump && body.state.grounded && gravLen > 1e-9) {
            const glm::dvec3 gravUnit = gravDir / gravLen;
            body.state.vel_x -= gravUnit.x * body.jump_speed;
            body.state.vel_y -= gravUnit.y * body.jump_speed;
            body.state.vel_z -= gravUnit.z * body.jump_speed;
        }

        // Build the frame delta: wish direction (horizontal movement) + velocity.
        glm::dvec3 wish(body.input.wish_x, body.input.wish_y, body.input.wish_z);
        if (glm::length(wish) > 1e-9)
            wish = glm::normalize(wish);

        glm::dvec3 delta = wish * (body.walk_speed * dt);
        delta.x += body.state.vel_x * dt;
        delta.y += body.state.vel_y * dt;
        delta.z += body.state.vel_z * dt;

        // Sweep-and-resolve via the engine's collision primitive.
        BodyMoveResult mr = g_ctx->move_aabb(
            g_ctx,
            body.state.center,
            body.half_x, body.half_y, body.half_z,
            delta.x, delta.y, delta.z,
            body.gravity_dir_x, body.gravity_dir_y, body.gravity_dir_z
        );

        body.state.center  = mr.position;
        body.state.grounded = mr.grounded;
        body.state.hit_x   = mr.hitX;
        body.state.hit_y   = mr.hitY;
        body.state.hit_z   = mr.hitZ;

        // Zero velocity components along blocked axes.
        if (gravLen > 1e-9) {
            const glm::dvec3 gravUnit = gravDir / gravLen;
            const glm::dvec3 vel(body.state.vel_x, body.state.vel_y, body.state.vel_z);
            const double velAlongGrav = glm::dot(vel, gravUnit);

            // On ground: kill velocity component along gravity direction
            if (mr.grounded && velAlongGrav > 0.0) {
                body.state.vel_x -= gravUnit.x * velAlongGrav;
                body.state.vel_y -= gravUnit.y * velAlongGrav;
                body.state.vel_z -= gravUnit.z * velAlongGrav;
            }
            // Hit ceiling (moving against gravity and blocked): kill that component
            if (!mr.grounded && velAlongGrav < 0.0) {
                const bool hitAxis = (std::abs(gravUnit.x) > 0.5 && mr.hitX) ||
                                     (std::abs(gravUnit.y) > 0.5 && mr.hitY) ||
                                     (std::abs(gravUnit.z) > 0.5 && mr.hitZ);
                if (hitAxis) {
                    body.state.vel_x -= gravUnit.x * velAlongGrav;
                    body.state.vel_y -= gravUnit.y * velAlongGrav;
                    body.state.vel_z -= gravUnit.z * velAlongGrav;
                }
            }
        }

        // Clear per-frame input.
        body.input = {};
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;

    // Fill the shared API table so the host can interact with the body registry.
    kinbody::api().create_body  = create_body_impl;
    kinbody::api().destroy_body = destroy_body_impl;
    kinbody::api().set_input    = set_input_impl;
    kinbody::api().get_state    = get_state_impl;
    kinbody::api().set_gravity  = set_gravity_impl;
    kinbody::api().set_position = set_position_impl;
    kinbody::api().body_count   = body_count_impl;

    // Register the per-frame tick hook.
    ctx->register_on_tick(ctx, tick, nullptr);

    return 0;
}
