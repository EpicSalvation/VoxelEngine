#include "PluginManager.h"
#include "world/Noise.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

// Built-in .vox import/export handlers are registered via marker records so the
// Engine dispatch can distinguish them from plugin-registered handlers.
// kBuiltinOwnerId is a reserved PluginId that is never allocated to a real plugin
// (real IDs start at 1 and advance via nextPluginId_).
static constexpr PluginId kBuiltinOwnerId = 0xFFFFFFFFu;

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

namespace {
// Erase every entry of `vec` whose owner == id. Used by unloadPlugin to tear
// down a single plugin's registrations across all registries uniformly.
template <typename Vec>
void eraseOwned(Vec& vec, PluginId id) {
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [id](const auto& e) { return e.owner == id; }),
              vec.end());
}
}  // namespace

PluginManager::PluginManager() {}

PluginManager::~PluginManager() {
    for (const LoadedPlugin& p : loaded_)
        if (p.handle) platformDlClose(p.handle);
}

PluginId PluginManager::loadPlugin(const std::string& path) {
    void* handle = platformDlOpen(path.c_str());
    if (!handle) {
        std::cerr << "[PluginManager] Cannot open " << path << ": " << platformDlError() << "\n";
        return kInvalidPluginId;
    }

    auto* init = reinterpret_cast<VoxelPluginInitFn*>(
        platformDlSym(handle, VOXEL_PLUGIN_INIT_SYMBOL));
    if (!init) {
        std::cerr << "[PluginManager] " << path << " does not export '"
                  << VOXEL_PLUGIN_INIT_SYMBOL << "': " << platformDlError() << "\n";
        platformDlClose(handle);
        return kInvalidPluginId;
    }

    const PluginId id = nextPluginId_++;
    PluginContext ctx = buildContext();
    currentOwner_ = id;
    int result = init(&ctx);
    currentOwner_ = kInvalidPluginId;
    if (result != 0) {
        std::cerr << "[PluginManager] Plugin init failed for " << path
                  << " (returned " << result << ")\n";
        // init may have registered some callbacks before failing; remove them
        // before closing the library so none dangle.
        eraseOwned(layerGenerators_,    id);
        eraseOwned(featureGenerators_,  id);
        eraseOwned(materials_,          id);
        eraseOwned(voxelModifiedHooks_, id);
        eraseOwned(structuralEventHooks_, id);
        eraseOwned(chunkCreatedHooks_,  id);
        eraseOwned(chunkEvictedHooks_,  id);
        eraseOwned(importers_,          id);
        eraseOwned(exporters_,          id);
        eraseOwned(recipes_,            id);
        eraseOwned(noises_,             id);
        platformDlClose(handle);
        return kInvalidPluginId;
    }

    loaded_.push_back({id, handle});
    std::cout << "[PluginManager] Loaded: " << path << " (id " << id << ")\n";
    return id;
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

PluginId PluginManager::wireInPlugin(VoxelPluginInitFn* initFn) {
    const PluginId id = nextPluginId_++;
    PluginContext ctx = buildContext();
    currentOwner_ = id;
    int result = initFn(&ctx);
    currentOwner_ = kInvalidPluginId;
    if (result != 0) {
        std::cerr << "[PluginManager] Wired-in plugin init returned " << result << "\n";
        eraseOwned(layerGenerators_,    id);
        eraseOwned(featureGenerators_,  id);
        eraseOwned(materials_,          id);
        eraseOwned(voxelModifiedHooks_, id);
        eraseOwned(structuralEventHooks_, id);
        eraseOwned(chunkCreatedHooks_,  id);
        eraseOwned(chunkEvictedHooks_,  id);
        eraseOwned(importers_,          id);
        eraseOwned(exporters_,          id);
        eraseOwned(recipes_,            id);
        eraseOwned(noises_,             id);
        return kInvalidPluginId;
    }
    loaded_.push_back({id, nullptr});
    return id;
}

bool PluginManager::unloadPlugin(PluginId id) {
    auto it = std::find_if(loaded_.begin(), loaded_.end(),
                           [id](const LoadedPlugin& p) { return p.id == id; });
    if (it == loaded_.end())
        return false;

    // Remove every registration owned by this plugin BEFORE closing the library,
    // so no engine subsystem can invoke a callback that points into unloaded code.
    eraseOwned(layerGenerators_,      id);
    eraseOwned(featureGenerators_,    id);
    eraseOwned(materials_,            id);
    eraseOwned(voxelModifiedHooks_,   id);
    eraseOwned(structuralEventHooks_, id);
    eraseOwned(chunkCreatedHooks_,    id);
    eraseOwned(chunkEvictedHooks_,    id);
    eraseOwned(importers_,            id);
    eraseOwned(exporters_,            id);
    eraseOwned(recipes_,              id);
    eraseOwned(noises_,               id);

    void* handle = it->handle;
    loaded_.erase(it);
    if (handle) platformDlClose(handle);
    std::cout << "[PluginManager] Unloaded plugin id " << id << "\n";
    return true;
}

MaterialProperties PluginManager::material(const std::string& material_id) const {
    for (const auto& m : materials_)
        if (m.material_id == material_id)
            return m.props;
    return MaterialProperties{};  // neutral fail-soft default for an unknown id
}

MaterialProperties PluginManager::materialForPalette(std::uint8_t palette_index) const {
    MaterialProperties result{};
    result.palette_index = palette_index;
    // Last registration with this palette_index wins, matching the overwrite-by-id
    // semantics of register_material (a later duplicate replaces the earlier one).
    for (const auto& m : materials_)
        if (m.props.palette_index == palette_index)
            result = m.props;
    return result;
}

const Recipe* PluginManager::findRecipe(const std::string& layer_name) const {
    for (const auto& r : recipes_)
        if (r.layer_name == layer_name)
            return &r.recipe;
    return nullptr;  // unregistered => synthesized default recipe (resolved at job build)
}

const RegisteredNoise* PluginManager::resolveNoise(const std::string& noise_id) const {
    const RegisteredNoise* builtin = nullptr;
    const RegisteredNoise* plugin  = nullptr;
    // Last registration of each kind wins; a plugin entry overrides a built-in.
    for (const auto& n : noises_) {
        if (n.noise_id != noise_id) continue;
        if (n.isBuiltin) builtin = &n;
        else             plugin  = &n;
    }
    return plugin ? plugin : builtin;
}

void PluginManager::registerBuiltinNoise() {
    for (const auto& b : noise::builtins())
        noises_.push_back({b.id, b.fn, nullptr, kBuiltinOwnerId, true});
}

void PluginManager::registerBuiltinHandlers() {
    // Register marker entries for the built-in .vox importer and exporter.
    // fn and user_data are null — the Engine dispatch never calls them; it
    // recognises these entries by isBuiltin=true and invokes VoxImporter /
    // VoxExporter directly instead.
    importers_.push_back({"vox", nullptr, nullptr, kBuiltinOwnerId, true});
    exporters_.push_back({"vox", nullptr, nullptr, kBuiltinOwnerId, true});
}

PluginContext PluginManager::buildContext() {
    PluginContext ctx{};
    ctx.engine_data = this;

    ctx.register_layer_generator = [](PluginContext* c, const char* name,
                                       LayerGeneratorFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->layerGenerators_.push_back({name, fn, ud, mgr->currentOwner_});
    };

    ctx.register_feature_generator = [](PluginContext* c, const char* id,
                                         FeatureGeneratorFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->featureGenerators_.push_back({id, fn, ud, mgr->currentOwner_});
    };

    ctx.register_recipe = [](PluginContext* c, const char* name, const RecipeDesc* desc) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!name || !desc) {
            std::cerr << "[PluginManager] register_recipe called with null "
                      << (name ? "recipe" : "layer name") << "; ignored.\n";
            return;
        }
        Recipe recipe = Recipe::fromDesc(*desc);  // deep copy; plugin arrays need not outlive
        auto& recs = mgr->recipes_;
        // Keyed by layer name; a later registration for the same layer overwrites
        // the earlier one (mirroring register_material's overwrite-by-id).
        auto it = std::find_if(recs.begin(), recs.end(),
            [name](const RegisteredRecipe& r) { return r.layer_name == name; });
        if (it != recs.end()) {
            std::cerr << "[PluginManager] Warning: recipe for layer '" << name
                      << "' already registered; overwriting.\n";
            it->recipe = std::move(recipe);
            it->owner  = mgr->currentOwner_;
        } else {
            recs.push_back({name, std::move(recipe), mgr->currentOwner_});
        }
    };

    ctx.register_noise = [](PluginContext* c, const char* id, NoiseFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        // Plugin entries are appended (isBuiltin = false); resolveNoise prefers
        // them over a built-in of the same id, so this overrides the floor.
        mgr->noises_.push_back({id, fn, ud, mgr->currentOwner_, false});
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
            it->owner = mgr->currentOwner_;
        } else {
            mats.push_back({id, props, mgr->currentOwner_});
        }
    };

    ctx.register_on_voxel_modified = [](PluginContext* c, OnVoxelModifiedFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->voxelModifiedHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_structural_event = [](PluginContext* c, OnStructuralEventFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->structuralEventHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_chunk_created = [](PluginContext* c, const char* name,
                                        ChunkLifecycleFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->chunkCreatedHooks_.push_back({name, fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_chunk_evicted = [](PluginContext* c, const char* name,
                                        ChunkLifecycleFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->chunkEvictedHooks_.push_back({name, fn, ud, mgr->currentOwner_});
    };

    ctx.register_importer = [](PluginContext* c, const char* ext, ImporterFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->importers_.push_back({ext, fn, ud, mgr->currentOwner_});
    };

    ctx.register_exporter = [](PluginContext* c, const char* ext, ExporterFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->exporters_.push_back({ext, fn, ud, mgr->currentOwner_});
    };

    return ctx;
}
