// Example-hooks plugin (M17): a teaching catalog of the engine extension points
// the focused content plugins don't already show.
//
// The reference plugins each demonstrate ONE concern in context — base-terrain a
// generator, flow a fluid responder, crumble a structural responder, material-audio
// the sound hooks, chat the networking hooks, blockbench the importer/textures, and
// so on. A few first-class hooks were left without any example to read. This plugin
// fills that gap so the example-plugin suite covers every major hook type:
//
//   register_noise               — add/override a named noise field for recipes
//   register_on_thermal_event    — observe heat-overlay threshold crossings
//   register_on_lighting_event   — observe light-overlay threshold crossings
//   register_light_source        — inject a point light into the lighting overlay
//   register_on_chunk_created    — observe a layer chunk loading/generating
//   register_on_chunk_evicted    — observe a layer chunk leaving the cache
//   register_exporter            — serialize a region to a custom file format
//   register_interest_filter     — override network edit-replication targeting
//
// Each section is independent and copy-pasteable: take the one you need. The
// callbacks are intentionally trivial (they record into example_hooks::observed()
// through the user_data the engine threads back) — the lesson is the registration
// wiring and the callback signatures, not the behavior.
//
// Like every plugin, this links ZERO engine symbols: it sees only the public
// plugin_api.h (plus the in-tree Voxel definition a generator/exporter fills) and
// receives a PluginContext function-pointer table. See docs/ARCHITECTURE.md §8.

#include "example_hooks.h"
#include "plugin_api.h"
#include "world/Voxel.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  define VOXEL_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define VOXEL_PLUGIN_EXPORT extern "C"
#endif

VOXEL_PLUGIN_ABI_STAMP();  // no-op in compiled-in host builds (VOXEL_PLUGIN_NO_ABI_STAMP)

namespace {

// ── Generation: a custom noise field (register_noise) ────────────────────────
// A NoiseFn is a pure scalar field selected by id, sampled at a WORLD position so
// adjacent macro voxels' child grids stay seamless (the §6 contract). It must be
// deterministic — only integer hashing of the world coordinate + the engine-given
// seed, no rand()/time()/static state — so a streamed chunk regenerates identically
// every reload. This one is a simple ridged field; a recipe can tune its feature
// size through a "scale" param. Registering the id "example_ridges" makes it usable
// from any recipe's DistributionDesc::noise_id; registering a BUILT-IN id (value/
// fbm/ridged/worley) would override that built-in instead.
float example_noise(WorldCoord pos, uint64_t seed,
                    const RecipeParam* params, size_t param_count, void* /*user_data*/) {
    const double scale = recipe_param_num(params, param_count, "scale", 32.0);
    const int64_t ix = static_cast<int64_t>(std::llround(pos.value.x / scale));
    const int64_t iz = static_cast<int64_t>(std::llround(pos.value.z / scale));

    uint64_t state = voxel_seed_mix(static_cast<uint64_t>(ix), static_cast<uint64_t>(iz));
    state = voxel_seed_mix(state, seed);
    const float u = voxel_rng_norm(&state);   // uniform [0,1)
    return 1.0f - std::fabs(2.0f * u - 1.0f); // fold to a ridge at u = 0.5; stays [0,1]
}

// ── Field-event observers (register_on_thermal_event / on_lighting_event) ─────
// The engine DETECTS overlay cells crossing a reporting threshold and reports them;
// the plugin decides the response (ignite, melt, play audio, tint the sky). The
// engine never writes a voxel for these events. The event pointer is valid only for
// the call — never retain it. user_data is the plugin's own state (here, Observed).
void on_thermal(const ThermalFieldEvent* event, void* user_data) {
    auto& o = *static_cast<example_hooks::Observed*>(user_data);
    ++o.thermalEvents;
    o.lastTemperature = event->temperature;
}

void on_lighting(const LightingEvent* event, void* user_data) {
    auto& o = *static_cast<example_hooks::Observed*>(user_data);
    ++o.lightingEvents;
    o.lastBrightness = event->brightness;
}

// ── Chunk lifecycle observers (register_on_chunk_created / on_chunk_evicted) ──
// Fire when a chunk of the named layer is created (loaded or generated) or evicted
// from the cache. Useful for spawning/retiring per-chunk content (entities, audio
// beds, decorations) in lockstep with streaming. chunk_origin is the chunk's
// world-space corner.
void on_chunk_created(WorldCoord /*chunk_origin*/, void* user_data) {
    ++static_cast<example_hooks::Observed*>(user_data)->chunksCreated;
}

void on_chunk_evicted(WorldCoord /*chunk_origin*/, void* user_data) {
    ++static_cast<example_hooks::Observed*>(user_data)->chunksEvicted;
}

// ── Networking: an interest filter (register_interest_filter) ────────────────
// Called at the authority for each (peer, edit) pair before the built-in broadcast
// or streaming-radius check; returning true sends the edit to that peer, false
// suppresses it. Registering one overrides the built-in interest mode entirely —
// a game implements area-of-interest, team visibility, etc. here. This example
// forwards everything (the BroadcastAll default) and just counts the checks.
bool interest_filter(PlayerId /*target_peer*/, WorldCoord /*edit_position*/, void* user_data) {
    ++static_cast<example_hooks::Observed*>(user_data)->interestChecks;
    return true;
}

// ── IO: a custom exporter (register_exporter) ────────────────────────────────
// An ExporterFn serializes a grid_size³ region (x-fastest) to a freshly malloc'd
// buffer; the engine calls free() on *out_data after use, so allocate with malloc,
// not new. Return 0 on success. The matching importer side is shown by the
// blockbench plugin. This trivial ".ehv" format is a 8-byte header ("EHV1" + the
// little-endian grid size) followed by one palette_index byte per voxel — enough to
// demonstrate the contract end to end.
int example_exporter(const Voxel* in_voxels, int grid_size, WorldCoord /*anchor*/,
                     uint8_t** out_data, size_t* out_size, void* /*user_data*/) {
    if (grid_size <= 0) return 1;
    const size_t count = static_cast<size_t>(grid_size) * grid_size * grid_size;
    const size_t size  = 8 + count;
    auto* buf = static_cast<uint8_t*>(std::malloc(size));
    if (!buf) return 1;

    std::memcpy(buf, "EHV1", 4);
    const uint32_t g = static_cast<uint32_t>(grid_size);
    buf[4] = static_cast<uint8_t>(g & 0xff);
    buf[5] = static_cast<uint8_t>((g >> 8) & 0xff);
    buf[6] = static_cast<uint8_t>((g >> 16) & 0xff);
    buf[7] = static_cast<uint8_t>((g >> 24) & 0xff);
    for (size_t i = 0; i < count; ++i)
        buf[8 + i] = in_voxels[i].material.palette_index;

    *out_data = buf;
    *out_size = size;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------

// Unique entry point used when the plugin is compiled directly into a binary that
// already contains another voxel_plugin_init (the test binary links ExamplePlugin's
// via the engine library). Mirrors the kinematic-body reference plugin.
VOXEL_PLUGIN_EXPORT int example_hooks_plugin_init(PluginContext* ctx) {
    // Each callback receives the plugin's own state through user_data — the engine
    // never interprets it, it just threads it back. Here that state is the shared
    // Observed singleton so a host/test can confirm the hooks fired.
    void* obs = &example_hooks::observed();

    ctx->register_noise(ctx, "example_ridges", example_noise, nullptr);

    ctx->register_on_thermal_event(ctx, on_thermal, obs);
    ctx->register_on_lighting_event(ctx, on_lighting, obs);

    // A point light at (0.5, 5.5, 0.5): the engine injects it into the lighting
    // overlay and propagates it each tick (owner-tracked — torn down on unload).
    ctx->register_light_source(ctx, WorldCoord(0.5, 5.5, 0.5), 1.0f);

    ctx->register_on_chunk_created(ctx, "terrain", on_chunk_created, obs);
    ctx->register_on_chunk_evicted(ctx, "terrain", on_chunk_evicted, obs);

    ctx->register_exporter(ctx, ".ehv", example_exporter, nullptr);

    ctx->register_interest_filter(ctx, interest_filter, obs);
    return 0;
}

// Standard plugin entry point for .so/.dll loading via PluginManager::loadPlugin.
// Suppressed when compiled into a binary that already has a voxel_plugin_init.
#ifndef EXAMPLE_HOOKS_COMPILED_IN
VOXEL_PLUGIN_EXPORT int voxel_plugin_init(PluginContext* ctx) {
    return example_hooks_plugin_init(ctx);
}
#endif
