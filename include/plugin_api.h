#pragma once

// Public plugin interface for the Voxel Game Engine.
//
// Plugins register flat callbacks for named engine hooks rather than subclassing
// engine types. The full set of extension points is visible here without tracing
// any class hierarchy. See docs/ARCHITECTURE.md §8 for design rationale.

#include "WorldCoord.h"
#include <cstddef>
#include <cstdint>

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
// Examples: cave networks, ore veins, water tables, dungeon seeds.
using FeatureGeneratorFn = void(*)(
    WorldCoord  chunk_origin,
    double      voxel_size_m,
    int         grid_size,
    Voxel*      inout_voxels,
    void*       user_data
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
