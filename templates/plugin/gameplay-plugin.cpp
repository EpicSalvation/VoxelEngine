// ===========================================================================
// gameplay-plugin.cpp — gameplay / hooks plugin template
//
// Where world-plugin.cpp defines what the world IS, a gameplay plugin defines
// how it BEHAVES: it reacts to events the engine reports and pushes changes back
// through the public edit path. This template wires up the four hooks most games
// reach for, each with a no-op-by-default body you fill in:
//
//   * on_tick ............ per-frame logic (timers, AI, spawners)
//   * on_voxel_modified .. react to a block being broken / placed (audio, score)
//   * on_structural_event  respond to a collapsing structure (the engine detects;
//                          you decide what falls — clear it, spawn debris, etc.)
//   * apply_edit ......... the one public path for writing voxels from a plugin
//
// It also shows the audio registration pattern (register_sound +
// register_material_sound) so broken/placed blocks make material-appropriate
// sounds with zero extra glue.
//
// The detect/respond split (ARCHITECTURE §7) is the key idea: the engine never
// decides what a collapse or a fluid crossing should DO — it reports the event
// and a plugin owns the response via apply_edit. That keeps game policy out of
// the engine core. See plugins/crumble and plugins/falling-debris for two real
// structural responses, plugins/material-audio for the audio pattern, and
// tutorials 09-13.
//
// Copy to plugins/<yourname>/plugin.cpp (auto-discovered by the build).
// ===========================================================================

#include "plugin_api.h"
#include "world/Voxel.h"

#include <cstdint>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// The plugin may retain the ctx pointer past init — the engine keeps each
// plugin's PluginContext alive until unload — so callbacks can call back into
// the engine (apply_edit, play_sound, ...) using it. Stash it here at init.
PluginContext* g_ctx = nullptr;

// ---------------------------------------------------------------------------
// Per-frame tick. Fires once per frame from Engine::update with the elapsed dt
// (seconds). Use it for anything time-driven: cooldown timers, simple AI,
// periodic spawns. Keep it cheap — it runs every frame.
// ---------------------------------------------------------------------------
void on_tick(double dt, void* /*user_data*/) {
    (void)dt;
    // e.g. accumulate a timer and spawn something every N seconds, advance an
    // animation, or step a simple state machine.
}

// ---------------------------------------------------------------------------
// Voxel-modified hook. Fires after any terminal voxel changes — player edit or
// simulation. `source` is kLocalPlayer for local edits, or the remote peer id
// in a networked game (ignore it for single-player). Distinguish break vs place
// by inspecting old/new emptiness.
// ---------------------------------------------------------------------------
void on_voxel_modified(WorldCoord position, const Voxel* old_voxel,
                       const Voxel* new_voxel, PlayerId /*source*/, void* /*ud*/) {
    const bool wasSolid = old_voxel && !old_voxel->isEmpty();
    const bool nowSolid = new_voxel && !new_voxel->isEmpty();

    if (wasSolid && !nowSolid) {
        // BREAK: play the broken material's break sound at its position. The
        // engine resolved material->palette_index when the binding was
        // registered, so we look up by the palette_index the voxel carried.
        if (g_ctx)
            g_ctx->play_material_sound(g_ctx, AudioEvent::Break,
                                       old_voxel->material.palette_index, position);
        // ... award score, drop an item, trigger a quest, etc.
    } else if (!wasSolid && nowSolid) {
        // PLACE: play the placed material's place sound.
        if (g_ctx)
            g_ctx->play_material_sound(g_ctx, AudioEvent::Place,
                                       new_voxel->material.palette_index, position);
    }
}

// ---------------------------------------------------------------------------
// Structural-event response. The engine's PropagationSystem detects when a
// composite macro voxel can no longer reach support and reports it here; WE
// decide the consequence. The simplest response (this one) clears the unstable
// macro's child voxels via apply_edit — they vanish, "collapsing" the structure.
// Because apply_edit routes through on_voxel_modified, clearing these cells
// re-dirties the parent and the cascade closes itself with no engine-side
// collapse routine (§7). For a richer effect, relocate the material downward
// instead (see plugins/falling-debris).
//
// To enumerate the macro's child cells, walk its volume in child-voxel steps:
// the event carries the macro's center, size, and child voxel size.
// ---------------------------------------------------------------------------
void on_structural_event(const StructuralEvent* e, void* /*user_data*/) {
    if (!g_ctx || !e) return;

    const double child = e->child_voxel_size_m;
    if (child <= 0.0) return;
    const int n = static_cast<int>(e->voxel_size_m / child);  // children per side
    const double half = e->voxel_size_m * 0.5;
    const Voxel empty = Voxel::empty();

    // Clear every child cell of the unstable macro.
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const WorldCoord cell(
                    e->position.value.x - half + (x + 0.5) * child,
                    e->position.value.y - half + (y + 0.5) * child,
                    e->position.value.z - half + (z + 0.5) * child);
                g_ctx->apply_edit(g_ctx, cell, &empty);
            }
}

}  // namespace

// ---------------------------------------------------------------------------
// Plugin init. Register the hooks you actually use and delete the rest.
// ---------------------------------------------------------------------------
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;

    // --- Audio: register sounds, then bind them to (material, event) pairs ---
    // Paths resolve under the assets/ directory convention (see tutorial 10).
    // Registration is fail-soft: a no-op in a headless / audio-less host.
    SoundParams sfx;                       // defaults: inverse attenuation, vol 1
    sfx.max_distance = 40.0f;
    ctx->register_sound(ctx, "break-stone", "assets/audio/break_stone.wav", sfx);
    ctx->register_sound(ctx, "place-stone", "assets/audio/place_stone.wav", sfx);

    // Bind material id + event -> sound id. The material ids here must match
    // those a world plugin registered (e.g. "stone" from world-plugin.cpp).
    ctx->register_material_sound(ctx, "stone", AudioEvent::Break, "break-stone");
    ctx->register_material_sound(ctx, "stone", AudioEvent::Place, "place-stone");

    // --- Hooks ---
    ctx->register_on_tick(ctx, on_tick, nullptr);
    ctx->register_on_voxel_modified(ctx, on_voxel_modified, nullptr);
    ctx->register_on_structural_event(ctx, on_structural_event, nullptr);

    // Other hooks available on ctx (register the ones your game needs):
    //   register_on_fluid_event / register_fluid_source  — fluids (tutorial 12)
    //   register_on_thermal_event / register_heat_source — heat   (tutorial 12)
    //   register_on_lighting_event / register_light_source — light (tutorial 12)
    //   register_on_player_joined / _left, register_on_network_message,
    //     send_network_message, register_authority_policy — multiplayer (tut 11)
    //   create_emitter / set_emitter_position / stop_emitter — looping audio
    //   register_importer / register_exporter — custom asset formats (tutorial 06)

    return 0;
}
