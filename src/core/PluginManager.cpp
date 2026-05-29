#include "PluginManager.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#  include <windows.h>
static std::string platformDlError() {
    DWORD err = GetLastError();
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf), nullptr);
    return buf;
}
static void* platformDlOpen(const char* path)              { return static_cast<void*>(LoadLibraryA(path)); }
static void* platformDlSym(void* h, const char* sym)       { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), sym)); }
static void  platformDlClose(void* h)                      { FreeLibrary(static_cast<HMODULE>(h)); }
#else
#  include <dlfcn.h>
static std::string platformDlError()                       { return dlerror(); }
static void* platformDlOpen(const char* path)              { return dlopen(path, RTLD_LAZY); }
static void* platformDlSym(void* h, const char* sym)       { return dlsym(h, sym); }
static void  platformDlClose(void* h)                      { dlclose(h); }
#endif

PluginManager::PluginManager() {}

PluginManager::~PluginManager() {
    for (void* handle : handles_)
        if (handle) platformDlClose(handle);
}

bool PluginManager::loadPlugin(const std::string& path) {
    void* handle = platformDlOpen(path.c_str());
    if (!handle) {
        std::cerr << "[PluginManager] Cannot open " << path << ": " << platformDlError() << "\n";
        return false;
    }

    auto* init = reinterpret_cast<VoxelPluginInitFn*>(
        platformDlSym(handle, VOXEL_PLUGIN_INIT_SYMBOL));
    if (!init) {
        std::cerr << "[PluginManager] " << path << " does not export '"
                  << VOXEL_PLUGIN_INIT_SYMBOL << "': " << platformDlError() << "\n";
        platformDlClose(handle);
        return false;
    }

    PluginContext ctx = buildContext();
    int result = init(&ctx);
    if (result != 0) {
        std::cerr << "[PluginManager] Plugin init failed for " << path
                  << " (returned " << result << ")\n";
        platformDlClose(handle);
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

    ctx.register_importer = [](PluginContext* c, const char* ext, ImporterFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->importers_.push_back({ext, fn, ud});
    };

    ctx.register_exporter = [](PluginContext* c, const char* ext, ExporterFn fn, void* ud) {
        static_cast<PluginManager*>(c->engine_data)->exporters_.push_back({ext, fn, ud});
    };

    return ctx;
}
