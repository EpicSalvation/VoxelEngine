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

// Called when a terminal-layer voxel is modified by the player or simulation.
using OnVoxelModifiedFn = void(*)(
    WorldCoord   position,
    const Voxel* old_voxel,
    const Voxel* new_voxel,
    void*        user_data
);

// Called when structural integrity falls below the load threshold (collapse candidate).
using OnStructuralEventFn = void(*)(
    WorldCoord  position,
    float       structural_strength_remaining,
    void*       user_data
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
