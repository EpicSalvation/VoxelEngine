// Reference mob plugin (M18 mega-demo) — a zombie-like wander/chase/attack AI.
//
// Built entirely on the engine's two per-frame seams: register_on_tick (called
// from Engine::update with the frame dt) and the move_aabb collision primitive
// (the same sweep-and-resolve the kinematic-body plugin uses for the player). All
// mob POLICY — the state machine, the gravity/step integration, the melee model,
// the growl/bite audio — lives here, not in the engine. The host renders the
// bodies and applies damage; the plugin never touches the renderer.
//
// AI: each mob WANDERs on a slow random heading until the player comes within
// sight range, then CHASEs (heads straight at the player, hopping one-voxel
// ledges when blocked), then ATTACKs on contact, draining the player's health on
// a cooldown. (Line-of-sight is approximated by range: the plugin ABI exposes
// move_aabb but no voxel ray, so the mob "senses" the player within a radius
// rather than tracing occlusion — a deliberate, documented simplification.)

#include "mob.h"
#include "plugin_api.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();  // no-op under VOXEL_PLUGIN_NO_ABI_STAMP (compiled-in host)

namespace {

// ── Tunables ─────────────────────────────────────────────────────────────────
constexpr double kHalfX = 0.3, kHalfY = 0.9, kHalfZ = 0.3;  // 0.6 × 1.8 × 0.6 m body
constexpr double kGravity     = 25.0;   // m/s²
constexpr double kJumpSpeed   = 7.0;    // ledge hop when blocked while grounded
constexpr double kWanderSpeed = 1.6;    // m/s idle drift
constexpr double kChaseSpeed  = 3.6;    // m/s pursuit
constexpr double kSightRange  = 16.0;   // m: begin chasing
constexpr double kAttackRange = 1.6;    // m: begin attacking
constexpr double kAttackCooldown = 1.0; // s between melee hits
constexpr float  kAttackDamage   = 12.0f;
constexpr double kRespawnY       = -8.0; // fell out of the world → back to spawn
constexpr float  kMaxHealth      = 60.0f;

struct Mob {
    mob::MobState state;
    WorldCoord    spawn;
    double        velX = 0.0, velY = 0.0, velZ = 0.0;
    bool          grounded = false;
    double        wanderHeading = 0.0;
    double        wanderTimer   = 0.0;
    double        attackTimer   = 0.0;
    mob::MobId    id = mob::kInvalidMob;
    AudioEmitterId growl = kInvalidEmitterId;
};

PluginContext*   g_ctx = nullptr;
std::vector<Mob> g_mobs;
mob::MobId       g_nextId = 1;
uint64_t         g_rng    = 0x123456789abcdefULL;

// Player AABB (host-provided each frame).
WorldCoord g_playerCenter{0.0, 0.0, 0.0};
glm::dvec3 g_playerHalf{0.3, 0.9, 0.3};

float      g_pendingDamage = 0.0f;  // melee accumulator drained by poll_attack

double rngNorm() { return static_cast<double>(voxel_rng_norm(&g_rng)); }

// ── Shared-API implementations ───────────────────────────────────────────────
void set_player_impl(WorldCoord center, double hx, double hy, double hz) {
    g_playerCenter = center;
    g_playerHalf   = glm::dvec3(hx, hy, hz);
}

mob::MobId spawn_impl(WorldCoord center) {
    Mob m;
    m.id            = g_nextId++;
    m.spawn         = center;
    m.state.center  = center;
    m.state.health  = kMaxHealth;
    m.state.state   = mob::State::Wander;
    m.wanderHeading = rngNorm() * 6.28318530718;
    m.wanderTimer   = 1.0 + rngNorm() * 2.0;
    if (g_ctx) {
        EmitterParams ep{};
        ep.sound.volume       = 0.7f;
        ep.sound.min_distance = 3.0f;
        ep.sound.max_distance = 24.0f;
        ep.loop               = true;
        m.growl = g_ctx->create_emitter(g_ctx, "zombie_growl", center, &ep);
    }
    g_mobs.push_back(m);
    return m.id;
}

uint32_t mob_count_impl() { return static_cast<uint32_t>(g_mobs.size()); }

const mob::MobState* get_mob_impl(uint32_t i) {
    if (i >= g_mobs.size()) return nullptr;
    return &g_mobs[i].state;
}

bool poll_attack_impl(float* out_damage) {
    if (g_pendingDamage <= 0.0f) return false;
    if (out_damage) *out_damage = g_pendingDamage;
    g_pendingDamage = 0.0f;
    return true;
}

void set_seed_impl(uint64_t seed) { g_rng = seed ^ 0x9E3779B97F4A7C15ULL; }

bool attack_nearest_impl(WorldCoord from, double reach, float damage) {
    double bestDist = reach + 1.0;
    Mob* bestMob = nullptr;
    for (Mob& m : g_mobs) {
        if (m.state.state == mob::State::Dead) continue;
        const double d = glm::length(m.state.center.value - from.value);
        if (d <= reach && d < bestDist) { bestDist = d; bestMob = &m; }
    }
    if (!bestMob) return false;
    bestMob->state.health -= damage;
    if (bestMob->state.health <= 0.0f) {
        bestMob->state.health = 0.0f;
        bestMob->state.state  = mob::State::Dead;
        if (bestMob->growl != kInvalidEmitterId && g_ctx)
            g_ctx->stop_emitter(g_ctx, bestMob->growl);
    }
    if (g_ctx)
        g_ctx->play_sound(g_ctx, "zombie_bite", bestMob->state.center, nullptr);
    return true;
}

// ── Per-frame tick: step every mob ───────────────────────────────────────────
void tick(double dt, void* /*user_data*/) {
    if (!g_ctx) return;

    for (Mob& m : g_mobs) {
        if (m.state.state == mob::State::Dead) continue;

        const glm::dvec3 toPlayer = g_playerCenter.value - m.state.center.value;
        const glm::dvec3 flat(toPlayer.x, 0.0, toPlayer.z);
        const double dist = glm::length(flat);

        // ── State machine ────────────────────────────────────────────────────
        glm::dvec3 wish(0.0, 0.0, 0.0);
        double speed = kWanderSpeed;
        if (dist <= kAttackRange) {
            m.state.state = mob::State::Attack;
            m.attackTimer -= dt;
            if (m.attackTimer <= 0.0) {
                m.attackTimer  = kAttackCooldown;
                g_pendingDamage += kAttackDamage;
                g_ctx->play_sound(g_ctx, "zombie_bite", m.state.center, nullptr);
            }
            if (dist > 1e-6) m.state.yaw = std::atan2(flat.x, flat.z);
        } else if (dist <= kSightRange) {
            m.state.state = mob::State::Chase;
            speed = kChaseSpeed;
            if (dist > 1e-6) {
                wish = glm::normalize(flat);
                m.state.yaw = std::atan2(wish.x, wish.z);
            }
        } else {
            m.state.state = mob::State::Wander;
            m.wanderTimer -= dt;
            if (m.wanderTimer <= 0.0) {
                m.wanderTimer   = 2.0 + rngNorm() * 2.0;
                m.wanderHeading = rngNorm() * 6.28318530718;
            }
            wish = glm::dvec3(std::sin(m.wanderHeading), 0.0, std::cos(m.wanderHeading));
            m.state.yaw = m.wanderHeading;
        }

        // ── Body integration (mirror of the kinematic-body gravity loop) ──────
        m.velY -= kGravity * dt;

        glm::dvec3 delta = wish * (speed * dt);
        delta.x += m.velX * dt;
        delta.y += m.velY * dt;
        delta.z += m.velZ * dt;

        BodyMoveResult mr = g_ctx->move_aabb(
            g_ctx, m.state.center, kHalfX, kHalfY, kHalfZ,
            delta.x, delta.y, delta.z, 0.0, -1.0, 0.0);

        m.state.center = mr.position;
        m.grounded     = mr.grounded;
        if (mr.grounded && m.velY < 0.0) m.velY = 0.0;
        if (mr.hitY && m.velY > 0.0)     m.velY = 0.0;

        // Hop one-voxel ledges so a chasing zombie can climb terraced terrain.
        if (m.grounded && (mr.hitX || mr.hitZ) && glm::length(wish) > 0.0)
            m.velY = kJumpSpeed;

        // Fell out of the world (down a deep cave): return to spawn.
        if (m.state.center.value.y < kRespawnY) {
            m.state.center = m.spawn;
            m.velX = m.velY = m.velZ = 0.0;
        }

        if (m.growl != kInvalidEmitterId)
            g_ctx->set_emitter_position(g_ctx, m.growl, m.state.center);
    }
}

void fillApi() {
    mob::api().set_player  = set_player_impl;
    mob::api().spawn       = spawn_impl;
    mob::api().mob_count   = mob_count_impl;
    mob::api().get_mob     = get_mob_impl;
    mob::api().poll_attack     = poll_attack_impl;
    mob::api().set_seed        = set_seed_impl;
    mob::api().attack_nearest  = attack_nearest_impl;
}

}  // namespace

// Unique entry point for compiled-in host builds (mega-demo links this directly).
VOXEL_PLUGIN_EXPORT int mob_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;
    fillApi();

    // Register the mob sounds so the host's preload picks them up. The WAV assets
    // are written by the host (synthesized) under assets/audio/nature/. Owner-
    // tracked + fail-soft: missing files simply leave the mob silent.
    SoundParams growl{};
    growl.volume = 0.7f; growl.min_distance = 3.0f; growl.max_distance = 24.0f;
    ctx->register_sound(ctx, "zombie_growl", "assets/audio/nature/zombie_growl.wav", growl);
    SoundParams bite{};
    bite.volume = 0.9f; bite.min_distance = 2.0f; bite.max_distance = 20.0f;
    ctx->register_sound(ctx, "zombie_bite", "assets/audio/nature/zombie_bite.wav", bite);

    ctx->register_on_tick(ctx, tick, nullptr);
    return 0;
}

// Standard disk-load entry point, suppressed when compiled into a host that
// already defines another voxel_plugin_init (the mega-demo binary).
#ifndef MOB_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return mob_plugin_init(ctx);
}
#endif
