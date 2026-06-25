// material-audio plugin (M12, ARCHITECTURE §16).
//
// Default break/place audio for voxel edits — the audio sibling of the M11
// chat plugin. Dropping this plugin removes all default break/place audio
// with no engine change.
//
// Responsibilities:
//   1. Registers example sound assets and per-material bindings (Break, Place,
//      Footstep) — the canonical reference for authoring material-driven audio.
//   2. Registers on_voxel_modified and fires play_material_sound for Break or
//      Place at each committed edit's WorldCoord.
//
// Sound paths are relative to the working directory (the same convention as
// "voxelsave/"). Missing WAVs are handled fail-soft at play time; run
// validateAudio with policy=Error (debug default) during development to surface
// missing assets (ARCHITECTURE §16).
//
// Footstep audio: front-ends fire play_material_sound(Footstep, palette, pos)
// directly via Engine::audioManager() — the engine supplies the lookup-and-play
// helper; the caller owns the cadence (ARCHITECTURE §16).

#include "plugin_api.h"
#include "world/Voxel.h"

#include <string>

#ifdef _WIN32
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif
VOXEL_PLUGIN_ABI_STAMP();

namespace {

// Stored during init so the on_voxel_modified callback can call play_material_sound
// through the PluginContext — the same pattern as the chat plugin.
PluginContext* g_ctx = nullptr;

// ---------------------------------------------------------------------------
// on_voxel_modified — the triggering mechanism
// ---------------------------------------------------------------------------

void on_voxel_modified(WorldCoord         pos,
                        const Voxel*       old_voxel,
                        const Voxel*       new_voxel,
                        PlayerId           /*source*/,
                        void*              /*user_data*/) {
    if (!g_ctx || !old_voxel || !new_voxel) return;

    // Break fires when a solid voxel is removed or replaced.
    // The old voxel's palette_index selects the material-appropriate sound.
    if (!old_voxel->isEmpty())
        g_ctx->play_material_sound(g_ctx, AudioEvent::Break,
                                    old_voxel->material.palette_index, pos);

    // Place fires when a new solid voxel appears.
    // The new voxel's palette_index selects the material-appropriate sound.
    if (!new_voxel->isEmpty())
        g_ctx->play_material_sound(g_ctx, AudioEvent::Place,
                                    new_voxel->material.palette_index, pos);

    // Swap (old non-empty → new non-empty): both Break and Place fire —
    // the old material breaks, the new one is placed.
    // Fail-soft at play time when a binding is absent (ARCHITECTURE §16).
}

// ---------------------------------------------------------------------------
// Example sound assets and material bindings
//
// This is the reference pattern for authoring material-driven audio:
//   register_sound    — name an asset (WAV path + default SoundParams)
//   register_material_sound — bind (material_id, AudioEvent) → sound_id
//
// The engine resolves material_id → palette_index at registration time so
// the play-time lookup is keyed by the index the voxel itself carries, not
// a string (ARCHITECTURE §16 "Sound Data Lives Beside the Palette").
//
// To add audio for a new material: add a row below and drop the file into
// assets/audio/ — no engine change required.
// ---------------------------------------------------------------------------

struct MaterialAudio {
    const char* material_id;
    const char* block_file;     // used for both Break and Place (one file per material)
    const char* footstep_file;  // nullptr → no footstep binding for this material
};

// Material strata from material-showcase plus water (M11 hazards).
// Paths are relative to the working directory (assets/audio/).
// iron reuses stone sounds; bedrock is indestructible so no block sound.
static const MaterialAudio kMaterials[] = {
    { "grass",   "assets/audio/grass_break_place.wav",   "assets/audio/footstep_grass.wav"  },
    { "dirt",    "assets/audio/dirt_break_place.wav",    "assets/audio/footstep_dirt.wav"   },
    { "stone",   "assets/audio/stone_break_place.wav",   "assets/audio/footstep_stone.wav"  },
    { "iron",    "assets/audio/stone_break_place.wav",   "assets/audio/footstep_stone.wav"  },
    { "diamond", "assets/audio/crystal_break_place.wav", nullptr                            },
    { "bedrock", nullptr,                                 "assets/audio/footstep_stone.wav"  },
    { "water",   "assets/audio/water_break_place.wav",   "assets/audio/footstep_water.wav"  },
};

void register_sounds(PluginContext* ctx) {
    // Block-scale sound params: audible within ~24 m, closer near-source
    // threshold than the default (hand-placed blocks don't need map-wide reach).
    SoundParams blockParams{};
    blockParams.min_distance = 0.5f;
    blockParams.max_distance = 24.0f;

    // Footstep params: quieter and shorter range — heard only by the local player
    // and nearby listeners.
    SoundParams footParams{};
    footParams.volume       = 0.65f;
    footParams.min_distance = 0.3f;
    footParams.max_distance = 8.0f;

    for (const auto& m : kMaterials) {
        // Sound ids: "<material>_block" (shared by Break and Place since we have
        // one file per material), "<material>_footstep".
        std::string blockId    = std::string(m.material_id) + "_block";
        std::string footstepId = std::string(m.material_id) + "_footstep";

        if (m.block_file) {
            ctx->register_sound(ctx, blockId.c_str(), m.block_file, blockParams);
            ctx->register_material_sound(ctx, m.material_id, AudioEvent::Break, blockId.c_str());
            ctx->register_material_sound(ctx, m.material_id, AudioEvent::Place, blockId.c_str());
        }

        if (m.footstep_file) {
            ctx->register_sound(ctx, footstepId.c_str(), m.footstep_file, footParams);
            ctx->register_material_sound(ctx, m.material_id, AudioEvent::Footstep,
                                          footstepId.c_str());
        }
    }
}

} // namespace

VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    g_ctx = ctx;

    // Register assets and bindings first — on_voxel_modified may fire immediately
    // after registration in a replay scenario, so sounds must be available.
    register_sounds(ctx);

    ctx->register_on_voxel_modified(ctx, on_voxel_modified, nullptr);
    return 0;
}
