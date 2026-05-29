#pragma once

#include <string>
#include <vector>
#include "plugin_api.h"

// Aggregated registration records — one per registered callback.
struct RegisteredLayerGenerator {
    std::string      layer_name;
    LayerGeneratorFn fn;
    void*            user_data;
};

struct RegisteredFeatureGenerator {
    std::string        generator_id;
    FeatureGeneratorFn fn;
    void*              user_data;
};

struct RegisteredMaterial {
    std::string        material_id;
    MaterialProperties props;
};

struct RegisteredVoxelModifiedHook {
    OnVoxelModifiedFn fn;
    void*             user_data;
};

struct RegisteredStructuralEventHook {
    OnStructuralEventFn fn;
    void*               user_data;
};

struct RegisteredChunkLifecycleHook {
    std::string      layer_name;
    ChunkLifecycleFn fn;
    void*            user_data;
};

// Loads plugins from disk and maintains the callback registries that engine
// subsystems query to invoke registered behavior.
//
// Plugins register via a PluginContext (flat callback registration) rather than
// subclassing engine types. See docs/ARCHITECTURE.md §8.
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load a plugin .so/.dylib/.dll from path. Calls its voxel_plugin_init function
    // with a PluginContext. Returns false and logs an error on failure.
    bool loadPlugin(const std::string& path);

    // Load all shared libraries in dirPath matching the platform extension.
    void loadPluginsFromDirectory(const std::string& dirPath);

    // Wire in a plugin that is compiled directly into the executable rather than loaded
    // as a .so. Useful for the example plugin and for testing without a .so build step.
    void wireInPlugin(VoxelPluginInitFn* initFn);

    // Registries — read by engine subsystems to invoke registered callbacks.
    const std::vector<RegisteredLayerGenerator>&      layerGenerators()      const { return layerGenerators_; }
    const std::vector<RegisteredFeatureGenerator>&    featureGenerators()    const { return featureGenerators_; }
    const std::vector<RegisteredMaterial>&            materials()            const { return materials_; }
    const std::vector<RegisteredVoxelModifiedHook>&   voxelModifiedHooks()   const { return voxelModifiedHooks_; }
    const std::vector<RegisteredStructuralEventHook>& structuralEventHooks() const { return structuralEventHooks_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkCreatedHooks()    const { return chunkCreatedHooks_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkEvictedHooks()    const { return chunkEvictedHooks_; }

private:
    PluginContext buildContext();

    std::vector<RegisteredLayerGenerator>      layerGenerators_;
    std::vector<RegisteredFeatureGenerator>    featureGenerators_;
    std::vector<RegisteredMaterial>            materials_;
    std::vector<RegisteredVoxelModifiedHook>   voxelModifiedHooks_;
    std::vector<RegisteredStructuralEventHook> structuralEventHooks_;
    std::vector<RegisteredChunkLifecycleHook>  chunkCreatedHooks_;
    std::vector<RegisteredChunkLifecycleHook>  chunkEvictedHooks_;

    std::vector<void*> handles_;  // dlopen handles retained for dlclose on destruction
};
