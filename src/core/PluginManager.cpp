#include "PluginManager.h"
#include "audio/AudioManager.h"
#include "renderer/Palette.h"
#include "renderer/MaterialFaces.h"
#include "renderer/TextureManager.h"
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

PluginManager::PluginManager() {
    // Establish the built-in noise floor at construction (M16, C2): a plugin's
    // init may call ctx->resolve_noise, and some hosts (the early demos) load
    // plugins without ever calling Engine::init. Registering here guarantees the
    // floor exists before any plugin loads; the call is idempotent, so the later
    // Engine::init / test registerBuiltinNoise() calls are harmless no-ops.
    registerBuiltinNoise();
}

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
    // Store the context in a stable location: the plugin may retain &ctx and call
    // its function pointers long after init returns (see pluginContexts_ in the
    // header). A stack-local ctx would dangle once loadPlugin returns.
    PluginContext& ctx = pluginContexts_.emplace_back(buildContext());
    currentOwner_ = id;
    int result = init(&ctx);
    currentOwner_ = kInvalidPluginId;
    if (result != 0) {
        std::cerr << "[PluginManager] Plugin init failed for " << path
                  << " (returned " << result << ")\n";
        pluginContexts_.pop_back();  // init failed: drop the just-added context
        // init may have registered some callbacks before failing; remove them
        // before closing the library so none dangle.
        eraseOwned(layerGenerators_,    id);
        eraseOwned(featureGenerators_,  id);
        eraseOwned(materials_,          id);
        eraseOwned(voxelModifiedHooks_, id);
        eraseOwned(structuralEventHooks_, id);
        eraseOwned(fluidEventHooks_,    id);
        eraseOwned(thermalEventHooks_,  id);
        eraseOwned(heatSources_,        id);
        eraseOwned(fluidSources_,       id);
        eraseOwned(chunkCreatedHooks_,  id);
        eraseOwned(chunkEvictedHooks_,  id);
        eraseOwned(importers_,          id);
        eraseOwned(exporters_,          id);
        eraseOwned(recipes_,            id);
        eraseOwned(noises_,             id);
        eraseOwned(editReceivedHooks_,  id);
        eraseOwned(playerJoinedHooks_,  id);
        eraseOwned(playerLeftHooks_,    id);
        eraseOwned(networkMessageHooks_,id);
        eraseOwned(authorityPolicies_,  id);
        eraseOwned(interestFilters_,    id);
        eraseOwned(sounds_,             id);
        eraseOwned(materialSounds_,     id);
        eraseOwned(textures_,           id);
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
    // Stable context store — see the loadPlugin equivalent and pluginContexts_.
    PluginContext& ctx = pluginContexts_.emplace_back(buildContext());
    currentOwner_ = id;
    int result = initFn(&ctx);
    currentOwner_ = kInvalidPluginId;
    if (result != 0) {
        std::cerr << "[PluginManager] Wired-in plugin init returned " << result << "\n";
        pluginContexts_.pop_back();  // init failed: drop the just-added context
        eraseOwned(layerGenerators_,    id);
        eraseOwned(featureGenerators_,  id);
        eraseOwned(materials_,          id);
        eraseOwned(voxelModifiedHooks_, id);
        eraseOwned(structuralEventHooks_, id);
        eraseOwned(fluidEventHooks_,    id);
        eraseOwned(thermalEventHooks_,  id);
        eraseOwned(heatSources_,        id);
        eraseOwned(fluidSources_,       id);
        eraseOwned(chunkCreatedHooks_,  id);
        eraseOwned(chunkEvictedHooks_,  id);
        eraseOwned(importers_,          id);
        eraseOwned(exporters_,          id);
        eraseOwned(recipes_,            id);
        eraseOwned(noises_,             id);
        eraseOwned(editReceivedHooks_,  id);
        eraseOwned(playerJoinedHooks_,  id);
        eraseOwned(playerLeftHooks_,    id);
        eraseOwned(networkMessageHooks_,id);
        eraseOwned(authorityPolicies_,  id);
        eraseOwned(interestFilters_,    id);
        eraseOwned(sounds_,             id);
        eraseOwned(materialSounds_,     id);
        eraseOwned(textures_,           id);
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
    eraseOwned(fluidEventHooks_,      id);
    eraseOwned(thermalEventHooks_,    id);
    eraseOwned(heatSources_,          id);
    eraseOwned(fluidSources_,         id);
    eraseOwned(chunkCreatedHooks_,    id);
    eraseOwned(chunkEvictedHooks_,    id);
    eraseOwned(importers_,            id);
    eraseOwned(exporters_,            id);
    eraseOwned(recipes_,              id);
    eraseOwned(noises_,               id);
    eraseOwned(editReceivedHooks_,    id);
    eraseOwned(playerJoinedHooks_,    id);
    eraseOwned(playerLeftHooks_,      id);
    eraseOwned(networkMessageHooks_,  id);
    eraseOwned(authorityPolicies_,    id);
    eraseOwned(interestFilters_,      id);
    eraseOwned(sounds_,               id);
    eraseOwned(materialSounds_,       id);
    eraseOwned(textures_,             id);

    // Stop any live emitters the plugin created before its code is unloaded
    // (ARCHITECTURE §16 — no emitter must dangle past its owning library handle).
    if (audioManager_) audioManager_->stopEmittersOwnedBy(id);

    // Rebuild the atlas from the now-pruned texture registry so the unloaded
    // plugin's tiles are gone from the GPU texture (the §8 teardown contract).
    // textures_ has already had this plugin's entries erased above, so the
    // rebuild simply reflects the survivors.
    if (textureManager_) textureManager_->rebuild();

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

const RegisteredFeatureGenerator*
PluginManager::findFeatureGenerator(const std::string& generator_id) const {
    const RegisteredFeatureGenerator* found = nullptr;
    for (const auto& g : featureGenerators_)
        if (g.generator_id == generator_id)
            found = &g;  // last registration of an id wins (overwrite semantics)
    return found;
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
    if (builtinNoiseRegistered_) return;  // idempotent — see the constructor
    for (const auto& b : noise::builtins())
        noises_.push_back({b.id, b.fn, nullptr, kBuiltinOwnerId, true});
    builtinNoiseRegistered_ = true;
}

void PluginManager::registerBuiltinHandlers() {
    // Register marker entries for the built-in .vox importer and exporter.
    // fn and user_data are null — the Engine dispatch never calls them; it
    // recognises these entries by isBuiltin=true and invokes VoxImporter /
    // VoxExporter directly instead.
    importers_.push_back({"vox", nullptr, nullptr, kBuiltinOwnerId, true});
    exporters_.push_back({"vox", nullptr, nullptr, kBuiltinOwnerId, true});
}

const RegisteredSound* PluginManager::findSound(const std::string& sound_id) const {
    const RegisteredSound* found = nullptr;
    for (const auto& s : sounds_)
        if (s.sound_id == sound_id)
            found = &s;  // last registration wins
    return found;
}

const RegisteredMaterialSound* PluginManager::findMaterialSound(AudioEvent event,
                                                                  uint8_t palette_index) const {
    const RegisteredMaterialSound* found = nullptr;
    for (const auto& m : materialSounds_)
        if (m.event == event && m.palette_index == palette_index)
            found = &m;  // last registration wins
    return found;
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

    ctx.set_palette_color = [](PluginContext*, uint8_t index, uint32_t abgr) {
        palette::setColor(index, abgr);
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

    // -----------------------------------------------------------------------
    // Fluid / thermal hooks and sources (M14, ARCHITECTURE §17/§8)
    // -----------------------------------------------------------------------

    ctx.register_on_fluid_event = [](PluginContext* c, OnFluidEventFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->fluidEventHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_thermal_event = [](PluginContext* c, OnThermalEventFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->thermalEventHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_heat_source = [](PluginContext* c, WorldCoord pos, float rate) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->heatSources_.push_back({pos, rate, mgr->currentOwner_});
    };

    ctx.register_fluid_source = [](PluginContext* c, WorldCoord pos, float rate,
                                    const char* fluid_material) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!fluid_material) {
            std::cerr << "[PluginManager] register_fluid_source: null fluid_material; ignored.\n";
            return;
        }
        // Resolve fluid_material -> palette_index at registration time so
        // FluidSystem's hot-path event tagging is an integer copy, not a
        // string lookup — the register_material_sound pattern.
        uint8_t palette_index = 0;
        bool found = false;
        for (const auto& m : mgr->materials_) {
            if (m.material_id == fluid_material) {
                palette_index = m.props.palette_index;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "[PluginManager] Warning: register_fluid_source: material '"
                      << fluid_material << "' not yet registered; palette_index defaults to 0.\n";
        }
        mgr->fluidSources_.push_back({pos, rate, fluid_material, palette_index, mgr->currentOwner_});
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

    ctx.register_on_edit_received = [](PluginContext* c, OnEditReceivedFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!mgr->editReceivedHooks_.empty())
            std::cerr << "[PluginManager] Warning: on_edit_received already registered; overwriting.\n";
        mgr->editReceivedHooks_.clear();
        mgr->editReceivedHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_player_joined = [](PluginContext* c, OnPlayerJoinedFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->playerJoinedHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_player_left = [](PluginContext* c, OnPlayerLeftFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->playerLeftHooks_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_on_network_message = [](PluginContext* c, const char* prefix,
                                          OnNetworkMessageFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        mgr->networkMessageHooks_.push_back({prefix ? prefix : "", fn, ud, mgr->currentOwner_});
    };

    ctx.send_network_message = [](PluginContext* c, const MessageEnvelope* envelope) {
        // Routing is handled by NetworkManager (M11); PluginManager stores the registry
        // only. NetworkManager wires its send implementation in via setNetworkSendHandler
        // on init. With no handler installed (single-player) the send is a no-op.
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->netSendFn_ && envelope) mgr->netSendFn_(envelope, mgr->netSendUser_);
    };

    ctx.register_authority_policy = [](PluginContext* c, AuthorityPolicyFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!mgr->authorityPolicies_.empty())
            std::cerr << "[PluginManager] Warning: authority_policy already registered; overwriting.\n";
        mgr->authorityPolicies_.clear();
        mgr->authorityPolicies_.push_back({fn, ud, mgr->currentOwner_});
    };

    ctx.register_interest_filter = [](PluginContext* c, InterestFilterFn fn, void* ud) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!mgr->interestFilters_.empty())
            std::cerr << "[PluginManager] Warning: interest_filter already registered; overwriting.\n";
        mgr->interestFilters_.clear();
        mgr->interestFilters_.push_back({fn, ud, mgr->currentOwner_});
    };

    // -----------------------------------------------------------------------
    // Audio hooks (M12, ARCHITECTURE §16)
    // -----------------------------------------------------------------------

    ctx.register_sound = [](PluginContext* c, const char* sound_id,
                             const char* path, SoundParams params) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!sound_id || !path) {
            std::cerr << "[PluginManager] register_sound: null sound_id or path; ignored.\n";
            return;
        }
        mgr->sounds_.push_back({sound_id, path, params, mgr->currentOwner_});
    };

    ctx.register_material_sound = [](PluginContext* c, const char* material_id,
                                      AudioEvent event, const char* sound_id) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!material_id || !sound_id) {
            std::cerr << "[PluginManager] register_material_sound: null argument; ignored.\n";
            return;
        }
        // Resolve material_id → palette_index at registration so AudioManager's
        // play-time lookup is keyed by the index the voxel carries (§16).
        uint8_t palette_index = 0;
        bool found = false;
        for (const auto& m : mgr->materials_) {
            if (m.material_id == material_id) {
                palette_index = m.props.palette_index;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "[PluginManager] Warning: register_material_sound: material '"
                      << material_id << "' not yet registered; palette_index defaults to 0.\n";
        }
        mgr->materialSounds_.push_back({material_id, palette_index, event,
                                        sound_id, mgr->currentOwner_});
    };

    ctx.play_sound = [](PluginContext* c, const char* sound_id,
                         WorldCoord pos, const SoundParams* params) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->audioManager_ && sound_id)
            mgr->audioManager_->playSound(sound_id, pos, params);
    };

    ctx.play_material_sound = [](PluginContext* c, AudioEvent event,
                                  uint8_t palette_index, WorldCoord pos) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->audioManager_)
            mgr->audioManager_->playMaterialSound(event, palette_index, pos);
    };

    ctx.create_emitter = [](PluginContext* c, const char* sound_id,
                             WorldCoord pos, const EmitterParams* params) -> AudioEmitterId {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!mgr->audioManager_ || !sound_id || !params) return kInvalidEmitterId;
        // currentOwner_ is set around the plugin's init call, but create_emitter
        // may be called at any time (not just during init). If it is called during
        // init, currentOwner_ is the plugin's id; otherwise kInvalidPluginId (demo
        // emitters still get cleaned up by the explicit stop_emitter call).
        return mgr->audioManager_->createEmitter(sound_id, pos, *params,
                                                  mgr->currentOwner_);
    };

    ctx.set_emitter_position = [](PluginContext* c, AudioEmitterId id, WorldCoord pos) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->audioManager_) mgr->audioManager_->setEmitterPosition(id, pos);
    };

    ctx.stop_emitter = [](PluginContext* c, AudioEmitterId id) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->audioManager_) mgr->audioManager_->stopEmitter(id);
    };

    // -----------------------------------------------------------------------
    // Textured rendering (M15 T3)
    // -----------------------------------------------------------------------

    ctx.register_texture = [](PluginContext* c, const char* texture_id, const char* path) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!texture_id || !path) {
            std::cerr << "[PluginManager] register_texture: null texture_id or path; ignored.\n";
            return;
        }
        mgr->textures_.push_back({texture_id, path, mgr->currentOwner_});
        // If the atlas is live (a renderer is attached), rebuild it so the new
        // tile is available immediately. Registrations during init are batched —
        // the TextureManager's post-load rebuild() picks them all up at once — so
        // this only does real work for a runtime (post-init) registration.
        if (mgr->textureManager_) mgr->textureManager_->rebuild();
    };

    ctx.register_texture_data = [](PluginContext* c, const char* texture_id,
                                   const uint8_t* data, size_t size) {
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (!texture_id || !data || size == 0) {
            std::cerr << "[PluginManager] register_texture_data: null/empty argument; ignored.\n";
            return;
        }
        // Overwrite-by-id (the importer may re-register a texture across reloads).
        std::vector<uint8_t> bytes(data, data + size);
        auto it = std::find_if(mgr->textures_.begin(), mgr->textures_.end(),
            [&](const RegisteredTexture& t) { return t.texture_id == texture_id; });
        if (it != mgr->textures_.end()) {
            it->path.clear();
            it->data  = std::move(bytes);
            it->owner = mgr->currentOwner_;
        } else {
            mgr->textures_.push_back({texture_id, std::string(), mgr->currentOwner_,
                                      std::move(bytes)});
        }
        if (mgr->textureManager_) mgr->textureManager_->rebuild();
    };

    ctx.set_material_faces = [](PluginContext*, uint8_t palette_index,
                                const char* top, const char* bottom,
                                const char* side, float tiling_factor) {
        // Global runtime binding, the set_palette_color pattern (not owner-tracked
        // — see MaterialFaces.h). The mesh builder joins it with the atlas tile
        // rects the TextureManager installs.
        materialfaces::setMaterialFaces(palette_index, top, bottom, side, tiling_factor);
    };

    ctx.apply_edit = [](PluginContext* c, WorldCoord pos, const Voxel* voxel) {
        // Routed to the engine's single edit choke point by NetworkManager (M13);
        // PluginManager stores the handler only. With no handler installed (no
        // NetworkManager attached) the edit is a silent no-op — the structural
        // response then writes nothing, which is the engine-never-writes default.
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        if (mgr->editApplyFn_ && voxel)
            mgr->editApplyFn_(pos, voxel, mgr->editApplyUser_);
    };

    ctx.resolve_noise = [](PluginContext* c, const char* id) -> NoiseFn {
        // Consume the noise registry (M16, C2): hand the plugin the winning
        // NoiseFn for id (plugin override > built-in floor), or nullptr for an
        // unknown id so it can fail loudly (the §6 contract). Built-ins ignore
        // user_data, so only the fn pointer crosses back.
        if (!id) return nullptr;
        auto* mgr = static_cast<PluginManager*>(c->engine_data);
        const RegisteredNoise* n = mgr->resolveNoise(id);
        return n ? n->fn : nullptr;
    };

    return ctx;
}

void PluginManager::registerEngineVoxelModifiedHook(OnVoxelModifiedFn fn,
                                                    void* user_data) {
    if (fn) voxelModifiedHooks_.push_back({fn, user_data, kBuiltinOwnerId});
}

void PluginManager::unregisterEngineVoxelModifiedHook(void* user_data) {
    voxelModifiedHooks_.erase(
        std::remove_if(voxelModifiedHooks_.begin(), voxelModifiedHooks_.end(),
                       [user_data](const RegisteredVoxelModifiedHook& h) {
                           return h.owner == kBuiltinOwnerId &&
                                  h.user_data == user_data;
                       }),
        voxelModifiedHooks_.end());
}
