#pragma once

// Public plugin interface for the Voxel Game Engine.
//
// Plugins register flat callbacks for named engine hooks rather than subclassing
// engine types. The full set of extension points is visible here without tracing
// any class hierarchy. See docs/ARCHITECTURE.md §8 for design rationale.

#include "WorldCoord.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Material properties — carried by every voxel.
//
// Simulation systems query these values and respond to them; they never check
// a block type ID. A new material (stone, ice, volcanic rock) is defined by
// filling this struct — no changes to PhysicsSystem, fluid system, or
// voxel-removal system are required.
// ---------------------------------------------------------------------------
struct MaterialProperties {
    float   density              = 0.0f;  // kg/m³; drives physics mass and load
    float   structural_strength  = 0.0f;  // collapse resistance; queried by PropagationSystem
    float   thermal_conductivity = 0.0f;  // W/(m·K); drives heat and fire spread
    float   porosity             = 0.0f;  // 0.0–1.0; fraction permeable to fluid
    float   hardness             = 0.0f;  // relative resistance to removal/destruction
    uint8_t palette_index        = 0;     // index into the 256-entry visual palette (.vox compat)
    uint8_t _pad[3]              = {};    // explicit padding — keeps memcmp-based determinism checks
                                          // valid across GCC optimization levels. Must stay zero;
                                          // never read by engine code.
};

// Forward declaration — full definition in src/world/Voxel.h
struct Voxel;

// ---------------------------------------------------------------------------
// Recipe parameters and deterministic RNG (M9, docs/ARCHITECTURE.md §6)
//
// Recipe, feature-generator, and noise parameters cross the plugin ABI as a flat
// array of tagged key-value pairs — no std:: container crosses the boundary. A
// value is numeric ("no ore above depth 32") or a string ("bias toward granite"
// -> a material id by name). seed_parameters use the same type.
// ---------------------------------------------------------------------------
enum class RecipeParamKind : uint8_t { Number, String };

struct RecipeParam {
    const char*     key    = nullptr;
    RecipeParamKind kind   = RecipeParamKind::Number;
    double          number = 0.0;      // valid when kind == Number
    const char*     text   = nullptr;  // valid when kind == String (e.g. a material id)
};

// Header-only readers so a generator pulls a param without hand-rolling strcmp.
inline double recipe_param_num(const RecipeParam* params, size_t count,
                               const char* key, double fallback) {
    for (size_t i = 0; i < count; ++i)
        if (params[i].kind == RecipeParamKind::Number && std::strcmp(params[i].key, key) == 0)
            return params[i].number;
    return fallback;
}
inline const char* recipe_param_str(const RecipeParam* params, size_t count,
                                    const char* key, const char* fallback) {
    for (size_t i = 0; i < count; ++i)
        if (params[i].kind == RecipeParamKind::String && std::strcmp(params[i].key, key) == 0)
            return params[i].text;
    return fallback;
}

// Deterministic, header-only RNG (splitmix64). Seed from the value the engine
// hands a generator; the same seed yields the same sequence on any thread, every
// run. No engine RNG object crosses the ABI — the seed does (see DecompositionWorker).
inline uint64_t voxel_rng_next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline float voxel_rng_norm(uint64_t* state) {            // uniform [0,1)
    return (voxel_rng_next(state) >> 40) * (1.0f / 16777216.0f);  // 24-bit mantissa
}
inline uint64_t voxel_seed_mix(uint64_t a, uint64_t b) {  // fold two ints into a seed
    uint64_t s = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
    return voxel_rng_next(&s);
}

// ---------------------------------------------------------------------------
// Callback type definitions
// ---------------------------------------------------------------------------

// Procedural layer generator: fills a chunk's voxel grid from scratch.
//   chunk_origin — world-space origin of the chunk being generated
//   grid_size    — voxels per side; the grid is grid_size³ (row-major, x-fastest)
//   out_voxels   — flat array of grid_size³ Voxels to populate
using LayerGeneratorFn = void(*)(
    WorldCoord  chunk_origin,
    int         grid_size,
    Voxel*      out_voxels,
    void*       user_data
);

// Feature generator: stamps spatial structures into an already-filled child grid.
// Examples: cave networks, ore veins, water tables, dungeon seeds. Recipe-driven
// since M9: `params` is the EFFECTIVE param set (the recipe entry's own params
// merged with inherited seed_parameters; the entry wins on a key collision) and
// `seed` is the deterministic per-decomposition seed (see DecompositionWorker).
using FeatureGeneratorFn = void(*)(
    WorldCoord         chunk_origin,
    double             voxel_size_m,
    int                grid_size,
    Voxel*             inout_voxels,
    const RecipeParam* params,
    size_t             param_count,
    uint64_t           seed,
    void*              user_data
);

// Noise function: a pure scalar field selected by id, used by a recipe's material
// distribution. Sampled at a WORLD position so adjacent macro voxels' child grids
// are seamless; returns a scalar normalized to [0,1) by convention. Built-in ids
// (value/fbm/ridged/worley) ship with the engine; register_noise adds a new id or
// overrides a built-in (docs/ARCHITECTURE.md §6).
using NoiseFn = float(*)(
    WorldCoord         pos,
    uint64_t           seed,
    const RecipeParam* params,
    size_t             param_count,
    void*              user_data
);

// ---------------------------------------------------------------------------
// Networking types (M11, docs/ARCHITECTURE.md §15)
//
// All types cross the plugin ABI as POD — no std:: containers, no ENet types.
// The engine deep-copies payload data on send.
// ---------------------------------------------------------------------------

// Opaque player identifier. kLocalPlayer (0) always refers to the local client.
using PlayerId = uint32_t;
static constexpr PlayerId kLocalPlayer = 0;

// Message routing target.
enum class MessageTarget : uint8_t {
    Broadcast,   // deliver to all connected peers
    Server,      // deliver to the authority node only
    Player,      // deliver to the specific peer in MessageEnvelope::target_player
};

// Reliability class used when sending a network message.
enum class MessageReliability : uint8_t {
    Reliable,
    Unreliable,
};

// POD envelope passed to on_network_message and send_network_message.
// channel_id is a null-terminated string identifying the logical channel.
// The engine never inspects the payload — routing is by envelope fields only.
struct MessageEnvelope {
    const char*        channel_id    = nullptr;
    PlayerId           sender_id     = 0;
    PlayerId           target_player = 0;      // used when target == Player
    MessageTarget      target        = MessageTarget::Broadcast;
    MessageReliability reliability   = MessageReliability::Reliable;
    const void*        payload       = nullptr;
    size_t             payload_size  = 0;
};

// Resolution returned by an on_edit_received handler. Transform means the
// authority commits out_voxel (written by the handler) instead of the proposed
// voxel. Apply is the default built-in (last-write-wins).
enum class EditResolution : uint8_t {
    Apply,
    Discard,
    Transform,  // commit *out_voxel instead of the proposed voxel
};

// Called when a terminal-layer voxel is modified by the player or simulation.
// source is kLocalPlayer for local edits and the remote peer's id for replicated
// ones. Single-player plugins that ignore source continue to work without change —
// the field is appended at the end so existing registration call sites are unaffected.
using OnVoxelModifiedFn = void(*)(
    WorldCoord   position,
    const Voxel* old_voxel,
    const Voxel* new_voxel,
    PlayerId     source,
    void*        user_data
);

// Flat-POD payload for on_structural_event (M13, docs/ARCHITECTURE.md §7/§8).
//
// Describes a single composite macro voxel the engine has found can no longer
// reach structural support. It carries everything a response plugin needs to
// decide *what to do* — clear the voxels, spawn debris, relocate material —
// without calling back into the engine for context.
//
// Same flat-struct ABI rule as RecipeDesc / MessageEnvelope: no std:: type
// crosses the boundary. The macro's layer voxel index is carried as three plain
// int64 fields rather than the engine-internal chunkmath::VoxelCoord type, which
// is not part of the public ABI. Field order is APPEND-ONLY — new fields go at
// the end so existing plugins keep their offsets.
struct StructuralEvent {
    WorldCoord  position;            // world-space center of the unstable macro
    int64_t     voxel_x = 0;         // macro VoxelCoord (layer voxel index): x
    int64_t     voxel_y = 0;         //                                       y
    int64_t     voxel_z = 0;         //                                       z
    const char* layer_name = nullptr;// composite layer the macro belongs to
    double      voxel_size_m = 0.0;  // edge length of this macro in meters (its scale)
    float       aggregate_strength = 0.0f;  // post-edit volume-weighted structural_strength
    float       support_potential  = 0.0f;  // residual support potential; <= 0 ⇒ unstable
};

// Called when PropagationSystem finds a composite macro voxel can no longer
// reach structural support (collapse candidate). The engine only detects and
// reports; the registered plugin owns the response (§7). The pointer is valid
// only for the duration of the call — the plugin must not retain it.
using OnStructuralEventFn = void(*)(
    const StructuralEvent* event,
    void*                  user_data
);

// Called at the authority node before an edit is committed. The handler returns
// Apply, Discard, or Transform. On Transform it writes the substituted voxel to
// *out_voxel; on Apply/Discard out_voxel is ignored. The default built-in returns
// Apply (last-write-wins). Must be called from the single edit-application choke
// point (NetworkManager::applyEdit) so every edit, local or remote, passes through.
using OnEditReceivedFn = EditResolution(*)(
    PlayerId      proposing_player,
    WorldCoord    position,
    const Voxel*  proposed_voxel,
    Voxel*        out_voxel,   // written when returning Transform
    void*         user_data
);

// Called after the join handshake completes (joined) or after a peer disconnects
// or times out (left).
using OnPlayerJoinedFn = void(*)(
    PlayerId   player_id,
    WorldCoord initial_position,
    void*      user_data
);

using OnPlayerLeftFn = void(*)(
    PlayerId  player_id,
    void*     user_data
);

// Called when a MessageEnvelope addressed to this plugin's registered channel
// prefix arrives. The handler must not retain the pointer beyond the call —
// the engine owns the buffer.
using OnNetworkMessageFn = void(*)(
    const MessageEnvelope* envelope,
    void*                  user_data
);

// Called at the authority for each (peer, edit) pair before the built-in
// broadcast or radius check. Returns true to send, false to suppress. When
// registered this overrides the built-in interest mode entirely.
using InterestFilterFn = bool(*)(
    PlayerId   target_peer,
    WorldCoord edit_position,
    void*      user_data
);

// Called by NetworkManager to validate edit intents before they reach
// on_edit_received. Returns true to forward, false to reject without notifying
// the authority. When no policy is registered the engine defaults to built-in
// server authority (all edits forwarded).
using AuthorityPolicyFn = bool(*)(
    PlayerId     peer_id,
    WorldCoord   position,
    const Voxel* proposed_voxel,
    void*        user_data
);

// Called when a layer chunk is created (loaded or generated) or evicted from cache.
using ChunkLifecycleFn = void(*)(
    WorldCoord  chunk_origin,
    void*       user_data
);

// Import handler: reads file_data and fills out_voxels (grid_size³, x-fastest).
// anchor is the world-space corner of the target volume. Returns 0 on success.
using ImporterFn = int(*)(
    const uint8_t* file_data,
    size_t         file_size,
    WorldCoord     anchor,
    int            grid_size,
    Voxel*         out_voxels,
    void*          user_data
);

// Export handler: serialises in_voxels (grid_size³) to *out_data / *out_size.
// The engine calls free() on *out_data after use. Returns 0 on success.
using ExporterFn = int(*)(
    const Voxel*   in_voxels,
    int            grid_size,
    WorldCoord     anchor,
    uint8_t**      out_data,
    size_t*        out_size,
    void*          user_data
);

// ---------------------------------------------------------------------------
// Audio types (M12, docs/ARCHITECTURE.md §16)
//
// POD types that cross the plugin ABI for sound registration and playback.
// No std:: type and no miniaudio type appears here — the same rule as the
// networking types above. SoundParams/EmitterParams are the engine's own
// override knobs surfaced through IAudioBackend, never leaking miniaudio directly.
// ---------------------------------------------------------------------------
enum class AudioEvent : uint8_t {
    Footstep,
    Break,
    Place,
    // Collapse / Flow reserved for M13/M14
};

enum class AttenuationModel : uint8_t {
    Inverse,      // default — inverse-distance falloff
    Linear,
    Exponential,
    None,         // no distance attenuation
};

struct SoundParams {
    float          volume       = 1.0f;
    AttenuationModel attenuation = AttenuationModel::Inverse;
    float          min_distance = 1.0f;
    float          max_distance = 100.0f;
    float          rolloff      = 1.0f;
    float          doppler      = 0.0f;  // 0 = Doppler off (the default)
};

struct EmitterParams {
    SoundParams sound;
    bool        loop = true;
};

// Opaque handle for a persistent positioned emitter. kInvalidEmitterId (0)
// denotes a missing or failed emitter; same sentinel convention as kInvalidPeer.
using AudioEmitterId = uint32_t;
static constexpr AudioEmitterId kInvalidEmitterId = 0;

// ---------------------------------------------------------------------------
// Composition recipe (M9, docs/ARCHITECTURE.md §6)
//
// A flat, POD description a plugin passes to register_recipe for a composite
// layer. Pointers + counts only, so it crosses the plugin ABI safely (no std::
// types). The engine deep-copies it into an internal value type
// (src/world/Recipe.h) at registration, so the plugin's arrays need not outlive
// the register_recipe call. Material/feature/noise ids are resolved (via the M8
// material lookup and the feature/noise registries) when a decomposition job is
// built on the main thread, keeping DecompositionWorker off PluginManager (§13).
// ---------------------------------------------------------------------------
struct MaterialWeight {
    const char* material_id = nullptr;  // resolved to MaterialProperties via the M8 lookup
    float       weight      = 0.0f;     // relative; normalized across the list
};

// A weighted material distribution arranged spatially by a named noise field.
struct DistributionDesc {
    const MaterialWeight* materials         = nullptr;
    size_t                material_count    = 0;
    const char*           noise_id          = nullptr;  // nullptr => built-in "value"
    const RecipeParam*    noise_params      = nullptr;
    size_t                noise_param_count = 0;
};

// One ordered feature overlay reference with its params.
struct FeatureRef {
    const char*        generator_id = nullptr;
    const RecipeParam* params       = nullptr;
    size_t             param_count  = 0;
};

// A per-face boundary override (top / bottom / side). `depth` is how many
// child-voxel layers inward from the face it replaces; `present == false` leaves
// the face to the interior distribution.
struct BoundaryDesc {
    DistributionDesc distribution;
    int              depth   = 1;
    bool             present = false;
};

struct RecipeDesc {
    DistributionDesc   interior;                       // the bulk distribution

    const FeatureRef*  features             = nullptr; // applied in array order
    size_t             feature_count        = 0;

    BoundaryDesc       top;                            // overlap order at edges/corners:
    BoundaryDesc       bottom;                         //   bottom -> side -> top (top wins)
    BoundaryDesc       side;                           // shared by all four lateral faces

    const RecipeParam* seed_parameters      = nullptr; // biases the layer below
    size_t             seed_parameter_count = 0;
};

// ---------------------------------------------------------------------------
// Plugin context
//
// The engine constructs one PluginContext and passes it to each plugin's init
// function. Plugins call the register_* function pointers to register callbacks.
// engine_data is an opaque engine pointer; plugins must not read or write it.
//
// Lifetime: the engine keeps each plugin's PluginContext alive for as long as the
// plugin is loaded, so a plugin MAY retain the ctx pointer and invoke its function
// pointers from its own callbacks after init returns (e.g. calling play_sound /
// play_material_sound / send_network_message from an on_voxel_modified or
// on_network_message handler). The pointer is stable until the plugin is unloaded.
// ---------------------------------------------------------------------------
struct PluginContext {
    void* engine_data;  // opaque; used internally by the engine

    void (*register_layer_generator)(
        PluginContext*    ctx,
        const char*       layer_name,
        LayerGeneratorFn  fn,
        void*             user_data
    );

    void (*register_feature_generator)(
        PluginContext*     ctx,
        const char*        generator_id,
        FeatureGeneratorFn fn,
        void*              user_data
    );

    void (*register_recipe)(
        PluginContext*    ctx,
        const char*       layer_name,
        const RecipeDesc* recipe          // deep-copied; need not outlive the call
    );

    void (*register_noise)(
        PluginContext* ctx,
        const char*    noise_id,
        NoiseFn        fn,
        void*          user_data
    );

    void (*register_material)(
        PluginContext*     ctx,
        const char*        material_id,
        MaterialProperties props
    );

    // Install an ABGR colour (0xAABBGGRR) at the given palette index. Lets plugins
    // pair a material's palette_index with a meaningful visual colour rather than
    // relying on the default cycling palette (where some slots are translucent).
    void (*set_palette_color)(
        PluginContext* ctx,
        uint8_t        index,
        uint32_t       abgr
    );

    void (*register_on_voxel_modified)(
        PluginContext*     ctx,
        OnVoxelModifiedFn  fn,
        void*              user_data
    );

    void (*register_on_structural_event)(
        PluginContext*       ctx,
        OnStructuralEventFn  fn,
        void*                user_data
    );

    void (*register_on_chunk_created)(
        PluginContext*    ctx,
        const char*       layer_name,
        ChunkLifecycleFn  fn,
        void*             user_data
    );

    void (*register_on_chunk_evicted)(
        PluginContext*    ctx,
        const char*       layer_name,
        ChunkLifecycleFn  fn,
        void*             user_data
    );

    void (*register_importer)(
        PluginContext* ctx,
        const char*    extension,
        ImporterFn     fn,
        void*          user_data
    );

    void (*register_exporter)(
        PluginContext* ctx,
        const char*    extension,
        ExporterFn     fn,
        void*          user_data
    );

    // -----------------------------------------------------------------------
    // Networking hooks (M11, docs/ARCHITECTURE.md §15)
    // -----------------------------------------------------------------------

    // Register a handler that fires before any edit is committed at the
    // authority. At most one handler is active; re-registering overwrites the
    // previous one (with a logged warning). Pass nullptr to restore the default
    // built-in Apply (last-write-wins) behaviour.
    void (*register_on_edit_received)(
        PluginContext*   ctx,
        OnEditReceivedFn fn,
        void*            user_data
    );

    void (*register_on_player_joined)(
        PluginContext*   ctx,
        OnPlayerJoinedFn fn,
        void*            user_data
    );

    void (*register_on_player_left)(
        PluginContext* ctx,
        OnPlayerLeftFn fn,
        void*          user_data
    );

    // Register a handler for inbound messages whose channel_id begins with
    // channel_prefix. Multiple handlers may be registered; all matching ones
    // are called in registration order.
    void (*register_on_network_message)(
        PluginContext*      ctx,
        const char*          channel_prefix,
        OnNetworkMessageFn   fn,
        void*                user_data
    );

    // Send a message to the target(s) described by the envelope. The engine
    // deep-copies the payload; the caller need not keep the buffer alive after
    // the call returns.
    void (*send_network_message)(
        PluginContext*         ctx,
        const MessageEnvelope* envelope
    );

    // Register an authority-policy validator. Called for each incoming edit
    // intent before it reaches on_edit_received. Returning false rejects the
    // edit without informing the authority. Only one policy may be active;
    // re-registering overwrites with a logged warning.
    void (*register_authority_policy)(
        PluginContext*    ctx,
        AuthorityPolicyFn fn,
        void*             user_data
    );

    // Register an interest filter that overrides the built-in broadcast /
    // streaming-radius mode. Only one filter may be active; re-registering
    // overwrites with a logged warning.
    void (*register_interest_filter)(
        PluginContext*   ctx,
        InterestFilterFn fn,
        void*            user_data
    );

    // -----------------------------------------------------------------------
    // Audio hooks (M12, ARCHITECTURE §16)
    // No existing hook changes — audio rides the existing on_voxel_modified
    // hook; these add sound registration and playback primitives only.
    // -----------------------------------------------------------------------

    // Register a named sound asset.  params sets defaults for all plays of this
    // sound; individual play calls may override them.  Owner-tracked: the asset
    // record is removed when the registering plugin unloads.
    void (*register_sound)(
        PluginContext*   ctx,
        const char*      sound_id,
        const char*      path,
        SoundParams      params    // by value — POD, no std:: across the ABI
    );

    // Bind (material_id, AudioEvent) → sound_id.  The engine resolves
    // material_id → palette_index at registration so play-time lookup is keyed
    // by the index the voxel actually carries (ARCHITECTURE §16).  Owner-tracked.
    void (*register_material_sound)(
        PluginContext* ctx,
        const char*    material_id,
        AudioEvent     event,
        const char*    sound_id
    );

    // Fire-and-forget positional one-shot.  pos is a WorldCoord; AudioManager
    // projects it to camera-local float before submitting to the backend.
    // params may be nullptr to use the defaults registered with the sound.
    // Fail-soft: plays nothing when sound_id is unregistered or AudioManager
    // is not attached (audio is a pure sink — §4).
    void (*play_sound)(
        PluginContext*     ctx,
        const char*        sound_id,
        WorldCoord         pos,
        const SoundParams* params
    );

    // Resolve (AudioEvent, palette_index) → sound_id via the material-sound
    // registry and play.  Fail-soft when the binding is not found.
    void (*play_material_sound)(
        PluginContext* ctx,
        AudioEvent     event,
        uint8_t        palette_index,
        WorldCoord     pos
    );

    // Persistent looping emitter.  Returns kInvalidEmitterId on failure.
    // The emitter is owner-tracked: plugin unload stops all of its emitters so
    // none dangle past the library handle.
    AudioEmitterId (*create_emitter)(
        PluginContext*       ctx,
        const char*          sound_id,
        WorldCoord           pos,
        const EmitterParams* params
    );

    // Re-project an emitter's WorldCoord each tick. Safe to call every frame.
    void (*set_emitter_position)(
        PluginContext*  ctx,
        AudioEmitterId  id,
        WorldCoord      pos
    );

    // Stop and destroy an emitter.  Idempotent for kInvalidEmitterId.
    void (*stop_emitter)(
        PluginContext*  ctx,
        AudioEmitterId  id
    );
};

// ---------------------------------------------------------------------------
// Plugin entry point
//
// Every plugin shared library (.so/.dylib/.dll) must export this symbol with
// C linkage. The engine calls it once at load time. Return 0 on success;
// any non-zero value aborts the load with an error message.
// ---------------------------------------------------------------------------
#define VOXEL_PLUGIN_INIT_SYMBOL "voxel_plugin_init"
extern "C" typedef int (VoxelPluginInitFn)(PluginContext* ctx);
