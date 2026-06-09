#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "plugin_api.h"
#include "world/Recipe.h"  // Recipe value type (deep-copied from RecipeDesc at registration)

// Identifies a loaded plugin instance. Returned by loadPlugin/wireInPlugin and
// passed to unloadPlugin. kInvalidPluginId (0) denotes a failed load.
using PluginId = std::uint32_t;
inline constexpr PluginId kInvalidPluginId = 0;

// Aggregated registration records — one per registered callback. Every record
// carries the `owner` PluginId of the plugin that registered it, so a single
// plugin's registrations can be torn down on unload without disturbing others.
struct RegisteredLayerGenerator {
    std::string      layer_name;
    LayerGeneratorFn fn;
    void*            user_data;
    PluginId         owner;
};

struct RegisteredFeatureGenerator {
    std::string        generator_id;
    FeatureGeneratorFn fn;
    void*              user_data;
    PluginId           owner;
};

struct RegisteredMaterial {
    std::string        material_id;
    MaterialProperties props;
    PluginId           owner;
};

// A composition recipe registered for a composite layer (keyed by layer name,
// mirroring register_layer_generator). The flat RecipeDesc a plugin passes is
// deep-copied into the owning Recipe at registration, so the plugin's arrays
// need not outlive the call.
struct RegisteredRecipe {
    std::string layer_name;
    Recipe      recipe;
    PluginId    owner;
};

// A noise function registered by id. Built-in entries (owner kBuiltinOwnerId,
// isBuiltin = true) form the engine floor; a plugin registration of the same id
// overrides a built-in on lookup (the importer dispatch rule), and plugin
// entries are torn down on unload while built-ins persist.
struct RegisteredNoise {
    std::string noise_id;
    NoiseFn     fn        = nullptr;
    void*       user_data = nullptr;
    PluginId    owner;
    bool        isBuiltin = false;
};

struct RegisteredVoxelModifiedHook {
    OnVoxelModifiedFn fn;
    void*             user_data;
    PluginId          owner;
};

struct RegisteredStructuralEventHook {
    OnStructuralEventFn fn;
    void*               user_data;
    PluginId            owner;
};

struct RegisteredChunkLifecycleHook {
    std::string      layer_name;
    ChunkLifecycleFn fn;
    void*            user_data;
    PluginId         owner;
};

struct RegisteredImporter {
    std::string extension;
    ImporterFn  fn;
    void*       user_data;
    PluginId    owner;
    bool        isBuiltin = false;  // built-in handlers have lower dispatch priority
};

struct RegisteredExporter {
    std::string extension;
    ExporterFn  fn;
    void*       user_data;
    PluginId    owner;
    bool        isBuiltin = false;  // built-in handlers have lower dispatch priority
};

// Loads plugins from disk and maintains the callback registries that engine
// subsystems query to invoke registered behavior.
//
// Plugins register via a PluginContext (flat callback registration) rather than
// subclassing engine types. See docs/architecture.md §8.
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load a plugin .so/.dylib/.dll from path. Calls its voxel_plugin_init function
    // with a PluginContext. Returns the new plugin's id, or kInvalidPluginId on failure
    // (logged): cannot-open, missing voxel_plugin_init, or non-zero init return.
    PluginId loadPlugin(const std::string& path);

    // Load all shared libraries in dirPath matching the platform extension.
    void loadPluginsFromDirectory(const std::string& dirPath);

    // Register the engine's built-in .vox import/export handlers. Built-in
    // handlers appear in importers()/exporters() with isBuiltin=true and are
    // skipped by the Engine dispatch when a plugin-registered handler exists.
    // Called by Engine::init() before any plugins are loaded.
    void registerBuiltinHandlers();

    // Register the engine's built-in noise set (value/fbm/ridged/worley) as the
    // floor of the noise registry. Built-in entries are owned by the engine
    // (kBuiltinOwnerId) and never torn down by a plugin unload; a plugin
    // register_noise of the same id overrides a built-in on lookup. Called by
    // Engine::init() before any plugins are loaded.
    void registerBuiltinNoise();

    // Wire in a plugin that is compiled directly into the executable rather than loaded
    // as a .so. Useful for the example plugin and for testing without a .so build step.
    // Returns the new plugin's id (no library handle is associated), or kInvalidPluginId
    // if init returned non-zero.
    PluginId wireInPlugin(VoxelPluginInitFn* initFn);

    // Unload a previously loaded plugin: removes all of its registry entries first
    // (so no dangling callback can be invoked), then closes its library handle if it
    // has one. Returns false if id is unknown. Idempotent for an already-unloaded id.
    bool unloadPlugin(PluginId id);

    // Registries — read by engine subsystems to invoke registered callbacks.
    const std::vector<RegisteredLayerGenerator>&      layerGenerators()      const { return layerGenerators_; }
    const std::vector<RegisteredFeatureGenerator>&    featureGenerators()    const { return featureGenerators_; }
    const std::vector<RegisteredMaterial>&            materials()            const { return materials_; }
    const std::vector<RegisteredVoxelModifiedHook>&   voxelModifiedHooks()   const { return voxelModifiedHooks_; }
    const std::vector<RegisteredStructuralEventHook>& structuralEventHooks() const { return structuralEventHooks_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkCreatedHooks()    const { return chunkCreatedHooks_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkEvictedHooks()    const { return chunkEvictedHooks_; }
    const std::vector<RegisteredImporter>&            importers()            const { return importers_; }
    const std::vector<RegisteredExporter>&            exporters()            const { return exporters_; }
    const std::vector<RegisteredRecipe>&              recipes()              const { return recipes_; }
    const std::vector<RegisteredNoise>&               noises()               const { return noises_; }

    // Keyed material-property lookup. These centralize the registry search that
    // importers, the build menu, and other tooling previously hand-rolled. They
    // are a TOOLING convenience only: simulation systems read a voxel's own
    // MaterialProperties by value and never resolve by id (architecture.md §5).
    //
    // material(): returns the properties registered under material_id, or a
    // neutral default (a zero-initialized MaterialProperties — the fail-soft
    // "removable, weightless" default) when no material with that id exists.
    MaterialProperties material(const std::string& material_id) const;

    // materialForPalette(): returns the properties of the material registered
    // with this palette_index (the last such registration wins, mirroring
    // register_material's overwrite-by-id semantics), or a neutral default whose
    // palette_index is set to the requested index when none is registered.
    MaterialProperties materialForPalette(std::uint8_t palette_index) const;

    // Recipe lookup by composite layer name. Returns a pointer into the registry,
    // or nullptr when no recipe is registered for that layer — the caller treats
    // "unregistered" as the synthesized default recipe (the M6 run-the-child
    // behavior), resolved at decomposition-job-build time (architecture.md §6).
    const Recipe* findRecipe(const std::string& layer_name) const;

    // Noise lookup by id. Returns the winning entry (a plugin registration
    // overrides a built-in of the same id; the last registration of each kind
    // wins), or nullptr if no noise with that id is registered. The resolved
    // entry carries both fn and user_data for the eventual NoiseFn call.
    const RegisteredNoise* resolveNoise(const std::string& noise_id) const;

private:
    PluginContext buildContext();

    std::vector<RegisteredLayerGenerator>      layerGenerators_;
    std::vector<RegisteredFeatureGenerator>    featureGenerators_;
    std::vector<RegisteredMaterial>            materials_;
    std::vector<RegisteredVoxelModifiedHook>   voxelModifiedHooks_;
    std::vector<RegisteredStructuralEventHook> structuralEventHooks_;
    std::vector<RegisteredChunkLifecycleHook>  chunkCreatedHooks_;
    std::vector<RegisteredChunkLifecycleHook>  chunkEvictedHooks_;
    std::vector<RegisteredImporter>            importers_;
    std::vector<RegisteredExporter>            exporters_;
    std::vector<RegisteredRecipe>              recipes_;
    std::vector<RegisteredNoise>               noises_;

    // A plugin that has been loaded and whose registrations are live.
    struct LoadedPlugin {
        PluginId id;
        void*    handle;  // platform dynamic-library handle; nullptr for wired-in plugins
    };
    std::vector<LoadedPlugin> loaded_;

    PluginId nextPluginId_ = 1;          // 0 is reserved for kInvalidPluginId
    PluginId currentOwner_ = kInvalidPluginId;  // set around a plugin's init() call
};
