#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "plugin_api.h"
#include "world/Recipe.h"  // Recipe value type (deep-copied from RecipeDesc at registration)

// Forward declarations: PluginManager.h must not include audio/ headers because
// AudioManager.h includes PluginManager.h — circular at the .h level.
// PluginManager.cpp includes audio/AudioManager.h for the lambda bodies.
namespace audio   { class AudioManager;   }
namespace texture { class TextureManager; }

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

struct RegisteredEditReceivedHook {
    OnEditReceivedFn fn;
    void*            user_data;
    PluginId         owner;
};

struct RegisteredPlayerJoinedHook {
    OnPlayerJoinedFn fn;
    void*            user_data;
    PluginId         owner;
};

struct RegisteredPlayerLeftHook {
    OnPlayerLeftFn fn;
    void*          user_data;
    PluginId       owner;
};

struct RegisteredNetworkMessageHook {
    std::string        channel_prefix;
    OnNetworkMessageFn fn;
    void*              user_data;
    PluginId           owner;
};

struct RegisteredAuthorityPolicy {
    AuthorityPolicyFn fn;
    void*             user_data;
    PluginId          owner;
};

struct RegisteredInterestFilter {
    InterestFilterFn fn;
    void*            user_data;
    PluginId         owner;
};

// ---------------------------------------------------------------------------
// Audio registries (M12, ARCHITECTURE §16)
// ---------------------------------------------------------------------------

// A named audio asset: sound file plus its default SoundParams.
// Keyed by sound_id; torn down on plugin unload via the standard eraseOwned path.
struct RegisteredSound {
    std::string sound_id;
    std::string path;
    SoundParams params;
    PluginId    owner;
};

// A binding from (AudioEvent, palette_index) to a registered sound_id.
// register_material_sound resolves material_id → palette_index at registration
// so AudioManager's lookup is keyed by the index the voxel carries (§16).
struct RegisteredMaterialSound {
    std::string material_id;    // retained for validation / error messages
    uint8_t     palette_index = 0;
    AudioEvent  event         = AudioEvent::Break;
    std::string sound_id;
    PluginId    owner;
};

struct RegisteredStructuralEventHook {
    OnStructuralEventFn fn;
    void*               user_data;
    PluginId            owner;
};

// ---------------------------------------------------------------------------
// Fluid / thermal registries (M14, ARCHITECTURE §17)
// ---------------------------------------------------------------------------

struct RegisteredFluidEventHook {
    OnFluidEventFn fn;
    void*          user_data;
    PluginId       owner;
};

struct RegisteredThermalEventHook {
    OnThermalEventFn fn;
    void*            user_data;
    PluginId         owner;
};

// A plugin-registered heat emitter. The engine (ThermalSystem) injects `rate`
// into the thermal overlay at `pos` every tick. Torn down on owner unload.
struct RegisteredHeatSource {
    WorldCoord pos;
    float      rate = 0.0f;
    PluginId   owner;
};

// A plugin-registered fluid emitter. fluid_material is resolved to a
// palette_index at registration time (the register_material_sound pattern),
// so FluidSystem can tag the FluidEvent it fires for a saturated cell without
// a string compare on the hot path. Torn down on owner unload.
struct RegisteredFluidSource {
    WorldCoord  pos;
    float       rate = 0.0f;
    std::string fluid_material;
    uint8_t     palette_index = 0;
    PluginId    owner;
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

// ---------------------------------------------------------------------------
// Texture registry (M15, docs/m15-textured-voxels-audit.md T3)
// ---------------------------------------------------------------------------

// A plugin-registered image asset for the material texture atlas. Keyed by
// texture_id; the TextureManager decodes `path` and packs it into the shared
// atlas. Torn down on plugin unload via the standard eraseOwned path, after
// which the TextureManager rebuilds the atlas from the surviving registry
// entries — so a plugin's tiles disappear with it (the §8 teardown contract).
struct RegisteredTexture {
    std::string          texture_id;
    std::string          path;   // file source; empty when bytes are inline (below)
    PluginId             owner;
    std::vector<uint8_t> data;   // inline encoded image (register_texture_data);
                                 // when non-empty the TextureManager decodes this
                                 // instead of reading `path` — the M15 T6 path for
                                 // a Blockbench .bbmodel's embedded base64 texture
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
    // register_noise of the same id overrides a built-in on lookup. Called from
    // the constructor so the floor exists before any plugin's init can call
    // ctx->resolve_noise (M16, C2); idempotent, so the Engine::init() call and
    // test call sites that invoke it again are harmless no-ops.
    void registerBuiltinNoise();

    // Outbound network-message routing. PluginManager stores the registries only;
    // the actual send is performed by NetworkManager, which installs its handler
    // here during init (see ctx.send_network_message). A null fn (the default)
    // makes plugin sends a silent no-op — the single-player behaviour.
    using NetworkSendFn = void(*)(const MessageEnvelope* envelope, void* user);
    void setNetworkSendHandler(NetworkSendFn fn, void* user) {
        netSendFn_   = fn;
        netSendUser_ = user;
    }

    // World-edit routing (M13). PluginManager stores the handler only; the actual
    // write is performed by NetworkManager, which installs it during init so that
    // a plugin's ctx.apply_edit reaches the single edit choke point
    // (NetworkManager::applyEdit → World::setVoxel → on_voxel_modified). A null fn
    // (the default) makes ctx.apply_edit a silent no-op — a host with no
    // NetworkManager attached gets the engine-never-writes behaviour by default.
    using EditApplyFn = void(*)(WorldCoord pos, const Voxel* voxel, void* user);
    void setEditHandler(EditApplyFn fn, void* user) {
        editApplyFn_   = fn;
        editApplyUser_ = user;
    }

    // Register an engine-owned on_voxel_modified hook (owner kBuiltinOwnerId, so it
    // is never torn down by a plugin unload). Used by the structural PhysicsSystem
    // to observe edits at the choke point without coupling NetworkManager to the
    // simulation tier — PropagationSystem rides the same on_voxel_modified path a
    // plugin would (§7 detection contract, §13 dependency map).
    void registerEngineVoxelModifiedHook(OnVoxelModifiedFn fn, void* user_data);

    // Remove engine-owned on_voxel_modified hooks matching user_data (the inverse
    // of registerEngineVoxelModifiedHook). Called by PhysicsSystem's destructor so
    // a torn-down driver leaves no dangling callback behind it.
    void unregisterEngineVoxelModifiedHook(void* user_data);

    // Audio playback routing (M12). AudioManager installs itself here after init
    // so PluginContext play_sound / create_emitter / etc. route to it. Null (the
    // default) makes plugin audio calls a silent no-op — existing demos without
    // audio are unaffected (ARCHITECTURE §16).
    void setAudioManager(audio::AudioManager* am) { audioManager_ = am; }
    audio::AudioManager* audioManager() const     { return audioManager_; }

    // Texture-atlas routing (M15 T3). The TextureManager installs itself here
    // after init so register_texture records feed the atlas, and so unloadPlugin
    // can ask it to rebuild the atlas once a plugin's tiles are pruned from the
    // registry. Null (the default) makes register_texture a registry-only no-op —
    // a headless or audio-only host is unaffected (mirrors setAudioManager).
    void setTextureManager(texture::TextureManager* tm) { textureManager_ = tm; }
    texture::TextureManager* textureManager() const     { return textureManager_; }

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
    const std::vector<RegisteredFluidEventHook>&      fluidEventHooks()      const { return fluidEventHooks_; }
    const std::vector<RegisteredThermalEventHook>&    thermalEventHooks()    const { return thermalEventHooks_; }
    const std::vector<RegisteredHeatSource>&          heatSources()          const { return heatSources_; }
    const std::vector<RegisteredFluidSource>&         fluidSources()         const { return fluidSources_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkCreatedHooks()    const { return chunkCreatedHooks_; }
    const std::vector<RegisteredChunkLifecycleHook>&  chunkEvictedHooks()    const { return chunkEvictedHooks_; }
    const std::vector<RegisteredImporter>&            importers()            const { return importers_; }
    const std::vector<RegisteredExporter>&            exporters()            const { return exporters_; }
    const std::vector<RegisteredRecipe>&              recipes()              const { return recipes_; }
    const std::vector<RegisteredNoise>&               noises()               const { return noises_; }
    const std::vector<RegisteredEditReceivedHook>&    editReceivedHooks()    const { return editReceivedHooks_; }
    const std::vector<RegisteredPlayerJoinedHook>&    playerJoinedHooks()    const { return playerJoinedHooks_; }
    const std::vector<RegisteredPlayerLeftHook>&      playerLeftHooks()      const { return playerLeftHooks_; }
    const std::vector<RegisteredNetworkMessageHook>&  networkMessageHooks()  const { return networkMessageHooks_; }
    const std::vector<RegisteredAuthorityPolicy>&     authorityPolicies()    const { return authorityPolicies_; }
    const std::vector<RegisteredInterestFilter>&      interestFilters()      const { return interestFilters_; }
    const std::vector<RegisteredSound>&               sounds()               const { return sounds_; }
    const std::vector<RegisteredMaterialSound>&       materialSounds()       const { return materialSounds_; }
    const std::vector<RegisteredTexture>&             textures()             const { return textures_; }

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

    // Feature-generator lookup by id. Returns the winning entry (the last
    // registration of an id wins, matching register_feature_generator's
    // overwrite semantics), or nullptr when none is registered. Used at
    // decomposition-job-build time to resolve a recipe's feature overlay ids to
    // their FeatureGeneratorFn before the job leaves the main thread (§13).
    const RegisteredFeatureGenerator* findFeatureGenerator(const std::string& generator_id) const;

    // Noise lookup by id. Returns the winning entry (a plugin registration
    // overrides a built-in of the same id; the last registration of each kind
    // wins), or nullptr if no noise with that id is registered. The resolved
    // entry carries both fn and user_data for the eventual NoiseFn call.
    const RegisteredNoise* resolveNoise(const std::string& noise_id) const;

    // Audio lookups (M12). Last registration of an id wins, consistent with the
    // other registry lookup conventions. Both return nullptr when not found.
    const RegisteredSound*         findSound(const std::string& sound_id) const;
    const RegisteredMaterialSound* findMaterialSound(AudioEvent event,
                                                      uint8_t palette_index) const;

private:
    PluginContext buildContext();

    std::vector<RegisteredLayerGenerator>      layerGenerators_;
    std::vector<RegisteredFeatureGenerator>    featureGenerators_;
    std::vector<RegisteredMaterial>            materials_;
    std::vector<RegisteredVoxelModifiedHook>   voxelModifiedHooks_;
    std::vector<RegisteredStructuralEventHook> structuralEventHooks_;
    std::vector<RegisteredFluidEventHook>      fluidEventHooks_;
    std::vector<RegisteredThermalEventHook>    thermalEventHooks_;
    std::vector<RegisteredHeatSource>          heatSources_;
    std::vector<RegisteredFluidSource>         fluidSources_;
    std::vector<RegisteredChunkLifecycleHook>  chunkCreatedHooks_;
    std::vector<RegisteredChunkLifecycleHook>  chunkEvictedHooks_;
    std::vector<RegisteredImporter>            importers_;
    std::vector<RegisteredExporter>            exporters_;
    std::vector<RegisteredRecipe>              recipes_;
    std::vector<RegisteredNoise>               noises_;
    std::vector<RegisteredEditReceivedHook>    editReceivedHooks_;
    std::vector<RegisteredPlayerJoinedHook>    playerJoinedHooks_;
    std::vector<RegisteredPlayerLeftHook>      playerLeftHooks_;
    std::vector<RegisteredNetworkMessageHook>  networkMessageHooks_;
    std::vector<RegisteredAuthorityPolicy>     authorityPolicies_;
    std::vector<RegisteredInterestFilter>      interestFilters_;
    std::vector<RegisteredSound>               sounds_;
    std::vector<RegisteredMaterialSound>       materialSounds_;
    std::vector<RegisteredTexture>             textures_;

    // A plugin that has been loaded and whose registrations are live.
    struct LoadedPlugin {
        PluginId id;
        void*    handle;  // platform dynamic-library handle; nullptr for wired-in plugins
    };
    std::vector<LoadedPlugin> loaded_;

    // Each plugin's PluginContext must outlive its init() call: plugins retain the
    // ctx pointer to invoke the playback / send_network_message / play_* function
    // pointers from their own callbacks long after init returns (e.g. material-audio
    // and the M11 chat plugin store it as g_ctx). A std::deque never invalidates the
    // addresses of existing elements on push_back, so a retained pointer stays valid
    // for the PluginManager's lifetime. (A stack-local or std::vector context would
    // dangle — passing &local to init and using it later is undefined behavior.)
    std::deque<PluginContext> pluginContexts_;

    PluginId nextPluginId_ = 1;          // 0 is reserved for kInvalidPluginId
    PluginId currentOwner_ = kInvalidPluginId;  // set around a plugin's init() call
    bool     builtinNoiseRegistered_ = false;   // guards registerBuiltinNoise (idempotent)

    NetworkSendFn        netSendFn_    = nullptr;  // installed by NetworkManager::init
    void*                netSendUser_  = nullptr;
    EditApplyFn          editApplyFn_  = nullptr;  // installed by NetworkManager::init
    void*                editApplyUser_= nullptr;
    audio::AudioManager*     audioManager_   = nullptr;  // installed by AudioManager after init
    texture::TextureManager* textureManager_ = nullptr;  // installed by TextureManager after init
};
