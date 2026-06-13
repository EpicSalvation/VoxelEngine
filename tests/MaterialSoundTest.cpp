// Tests for the material-sound registry resolution path and the M4 teardown
// contract (plugin unload must remove all owned audio registrations and stop
// all emitters owned by the plugin).
//
// Verifies:
//   - register_material_sound resolves (AudioEvent, palette_index) → sound_id
//   - An unbound pair falls back to no-op (never throws — play-time fail-soft)
//   - Both register_sound and register_material_sound entries are removed when
//     the owning plugin unloads
//   - Emitters created during a plugin's init are stopped on plugin unload

#include "audio/AudioManager.h"
#include "audio/IAudioBackend.h"
#include "core/PluginManager.h"
#include "plugin_api.h"

#include <gtest/gtest.h>
#include <string>

namespace {

// Tracks playOneShot and stopEmitter calls.
class TrackingBackend : public audio::IAudioBackend {
public:
    bool           played        = false;
    int            stopCallCount = 0;
    AudioEmitterId nextId_       = 1;

    bool isReady() const override { return true; }
    bool loadSound(const std::string&, const std::string&, const SoundParams&) override { return true; }
    void playOneShot(const std::string&, const glm::vec3&, const SoundParams*) override { played = true; }
    AudioEmitterId createEmitter(const std::string&, const glm::vec3&,
                                  const EmitterParams&) override { return nextId_++; }
    void setEmitterPosition(AudioEmitterId, const glm::vec3&) override {}
    void stopEmitter(AudioEmitterId) override { ++stopCallCount; }
    void setListener(const glm::vec3&, const glm::vec3&) override {}
    void update() override {}
    void shutdown() override {}
};

// Plugin: registers material "stone" (palette_index=5) with a Break binding.
int initMaterialAndSound(PluginContext* ctx) {
    MaterialProperties m{};
    m.palette_index = 5;
    ctx->register_material(ctx, "stone", m);
    ctx->register_sound(ctx, "stone_snd", "", SoundParams{});
    ctx->register_material_sound(ctx, "stone", AudioEvent::Break, "stone_snd");
    return 0;
}

// Plugin: registers a material + sound + material binding, then creates a
// looping emitter during init (so the emitter is owner-tagged to this plugin).
int initSoundAndEmitter(PluginContext* ctx) {
    MaterialProperties m{};
    m.palette_index = 7;
    ctx->register_material(ctx, "iron", m);
    ctx->register_sound(ctx, "iron_snd", "", SoundParams{});
    ctx->register_material_sound(ctx, "iron", AudioEvent::Place, "iron_snd");
    EmitterParams ep{};
    ep.loop = true;
    ctx->create_emitter(ctx, "iron_snd", WorldCoord{}, &ep);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry resolution
// ---------------------------------------------------------------------------

TEST(MaterialSound, RegisteredBindingResolvesToSoundId) {
    PluginManager pm;
    pm.wireInPlugin(initMaterialAndSound);

    // palette_index=5 was registered via "stone" material.
    const auto* binding = pm.findMaterialSound(AudioEvent::Break, 5);
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->sound_id, "stone_snd");
}

TEST(MaterialSound, FindMaterialSoundReturnsNullForUnboundEvent) {
    PluginManager pm;
    pm.wireInPlugin(initMaterialAndSound);

    // Only Break was bound for stone; Place was never registered.
    EXPECT_EQ(pm.findMaterialSound(AudioEvent::Place, 5), nullptr);
}

TEST(MaterialSound, FindMaterialSoundReturnsNullForUnknownPalette) {
    PluginManager pm;
    pm.wireInPlugin(initMaterialAndSound);

    EXPECT_EQ(pm.findMaterialSound(AudioEvent::Break, 99), nullptr);
}

// ---------------------------------------------------------------------------
// Fail-soft at play time
// ---------------------------------------------------------------------------

TEST(MaterialSound, UnboundPairIsNoOpNoThrow) {
    PluginManager pm;
    pm.wireInPlugin(initMaterialAndSound);

    TrackingBackend fake;
    audio::AudioManager am(&fake, pm);

    // palette_index=99 has no binding — must be a silent no-op.
    EXPECT_NO_THROW(am.playMaterialSound(AudioEvent::Break, 99, WorldCoord{}));
    EXPECT_FALSE(fake.played);
}

TEST(MaterialSound, BoundPairRoutesToBackend) {
    PluginManager pm;
    pm.wireInPlugin(initMaterialAndSound);

    TrackingBackend fake;
    audio::AudioManager am(&fake, pm);
    // listener + source both at origin → distSq=0, within any max_distance
    am.setListener(WorldCoord{}, {0,0,-1}, {0,1,0});
    am.playMaterialSound(AudioEvent::Break, 5, WorldCoord{});
    EXPECT_TRUE(fake.played);
}

// ---------------------------------------------------------------------------
// M4 teardown contract
// ---------------------------------------------------------------------------

TEST(MaterialSound, PluginUnloadRemovesSoundEntry) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initMaterialAndSound);
    ASSERT_NE(id, kInvalidPluginId);
    ASSERT_NE(pm.findSound("stone_snd"), nullptr);

    pm.unloadPlugin(id);

    EXPECT_EQ(pm.findSound("stone_snd"), nullptr);
}

TEST(MaterialSound, PluginUnloadRemovesMaterialSoundEntry) {
    PluginManager pm;
    PluginId id = pm.wireInPlugin(initMaterialAndSound);
    ASSERT_NE(id, kInvalidPluginId);
    ASSERT_NE(pm.findMaterialSound(AudioEvent::Break, 5), nullptr);

    pm.unloadPlugin(id);

    EXPECT_EQ(pm.findMaterialSound(AudioEvent::Break, 5), nullptr);
}

TEST(MaterialSound, PluginUnloadStopsOwnedEmitters) {
    PluginManager pm;
    TrackingBackend fake;
    audio::AudioManager am(&fake, pm);
    // AudioManager must be attached before wireInPlugin so the create_emitter
    // call inside initSoundAndEmitter can route through it.
    pm.setAudioManager(&am);

    PluginId id = pm.wireInPlugin(initSoundAndEmitter);
    ASSERT_NE(id, kInvalidPluginId);

    EXPECT_EQ(fake.stopCallCount, 0);  // emitter running

    pm.unloadPlugin(id);

    EXPECT_EQ(fake.stopCallCount, 1);  // emitter stopped on unload
}

TEST(MaterialSound, PluginUnloadLeavesOtherPluginEntriesIntact) {
    PluginManager pm;
    PluginId idA = pm.wireInPlugin(initMaterialAndSound);
    PluginId idB = pm.wireInPlugin([](PluginContext* ctx) -> int {
        MaterialProperties m{};
        m.palette_index = 9;
        ctx->register_material(ctx, "crystal", m);
        ctx->register_sound(ctx, "crystal_snd", "", SoundParams{});
        ctx->register_material_sound(ctx, "crystal", AudioEvent::Place, "crystal_snd");
        return 0;
    });
    ASSERT_NE(idA, kInvalidPluginId);
    ASSERT_NE(idB, kInvalidPluginId);

    pm.unloadPlugin(idA);

    // Plugin A's entries are gone.
    EXPECT_EQ(pm.findSound("stone_snd"),                              nullptr);
    EXPECT_EQ(pm.findMaterialSound(AudioEvent::Break, 5),             nullptr);

    // Plugin B's entries must remain.
    EXPECT_NE(pm.findSound("crystal_snd"),                            nullptr);
    EXPECT_NE(pm.findMaterialSound(AudioEvent::Place, 9),             nullptr);
}
