#include "PluginManager.h"
#include <algorithm>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>

PluginManager::PluginManager() {}

PluginManager::~PluginManager() {
    for (void* handle : handles_)
        if (handle) dlclose(handle);
}

bool PluginManager::loadPlugin(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_LAZY);
    if (!handle) {
        std::cerr << "[PluginManager] Cannot open " << path << ": " << dlerror() << "\n";
        return false;
    }

    auto* init = reinterpret_cast<VoxelPluginInitFn*>(
        dlsym(handle, VOXEL_PLUGIN_INIT_SYMBOL));
    if (!init) {
        std::cerr << "[PluginManager] " << path << " does not export '"
                  << VOXEL_PLUGIN_INIT_SYMBOL << "': " << dlerror() << "\n";
        dlclose(handle);
        return false;
    }

    PluginContext ctx = buildContext();
    int result = init(&ctx);
    if (result != 0) {
        std::cerr << "[PluginManager] Plugin init failed for " << path
                  << " (returned " << result << ")\n";
        dlclose(handle);
        return false;
    }

    handles_.push_back(handle);
    std::cout << "[PluginManager] Loaded: " << path << "\n";
    return true;
}

void PluginManager::loadPluginsFromDirectory(const std::string& dirPath) {
#ifdef _WIN32
    const std::string ext = ".dll";
#elif defined(__APPLE__)
    const std::string ext = ".dylib";
#else
    const std::string ext = ".so";
#endif

    namespace fs = std::filesystem;
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        std::cout << "[PluginManager] Plugin directory not found: " << dirPath << "\n";
        return;
    }

    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() == ext)
            loadPlugin(entry.path().string());
    }
}

void PluginManager::wireInPlugin(VoxelPluginInitFn* initFn) {
    PluginContext ctx = buildContext();
    int result = initFn(&ctx);
    if (result != 0)
        std::cerr << "[PluginManager] Wired-in plugin init returned " << result << "\n";
}

PluginContext PluginManager::buildContext() {
    PluginContext ctx{};
    ctx.engine_data = this;

    ctx.register_layer_generator = [](PluginContext* c, const char* name,
                                       LayerGeneratorFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->layerGenerators_.push_back({name, fn, ud});
    };

    ctx.register_feature_generator = [](PluginContext* c, const char* id,
                                         FeatureGeneratorFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->featureGenerators_.push_back({id, fn, ud});
    };

    ctx.register_material = [](PluginContext* c, const char* id, MaterialProperties props) {
        auto* mgr  = static_cast<PluginManager*>(c->engine_data);
        auto& mats = mgr->materials_;
        // If a material with the same ID is already registered, warn and overwrite.
        auto it = std::find_if(mats.begin(), mats.end(),
            [id](const RegisteredMaterial& m) { return m.material_id == id; });
        if (it != mats.end()) {
            std::cerr << "[PluginManager] Warning: material '" << id
                      << "' already registered; overwriting.\n";
            it->props = props;
        } else {
            mats.push_back({id, props});
        }
    };

    ctx.register_on_voxel_modified = [](PluginContext* c, OnVoxelModifiedFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->voxelModifiedHooks_.push_back({fn, ud});
    };

    ctx.register_on_structural_event = [](PluginContext* c, OnStructuralEventFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->structuralEventHooks_.push_back({fn, ud});
    };

    ctx.register_on_chunk_created = [](PluginContext* c, const char* name,
                                        ChunkLifecycleFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->chunkCreatedHooks_.push_back({name, fn, ud});
    };

    ctx.register_on_chunk_evicted = [](PluginContext* c, const char* name,
                                        ChunkLifecycleFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->chunkEvictedHooks_.push_back({name, fn, ud});
    };

    return ctx;
}
