// Tests for audio::validateAudio (ARCHITECTURE §16 startup validation pass).
//
// validateAudio runs two passes over the PluginManager registries:
//   Pass 1 — dangling material-sound bindings: a register_material_sound
//             entry whose sound_id is not in pm.sounds().
//   Pass 2 — unloadable assets: backend.loadSound() returns false for a
//             registered sound_id.
//
// All issues are collected before the severity policy is applied:
//   Error — throws std::runtime_error with all issues listed.
//   Warn  — logs to stderr and returns (never throws).
//   Auto  — Error in debug (#ifndef NDEBUG), Warn in release.
//
// A null backend skips Pass 2 (only binding completeness is checked).

#include "audio/AudioValidation.h"
#include "audio/IAudioBackend.h"
#include "core/PluginManager.h"
#include "plugin_api.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// Backend whose loadSound return value is controlled per sound_id.
// sound_ids in failIds return false (unloadable); all others return true.
class ProbeBackend : public audio::IAudioBackend {
public:
    std::unordered_set<std::string> failIds;

    bool isReady() const override { return true; }
    bool loadSound(const std::string& id, const std::string&,
                   const SoundParams&) override {
        return failIds.count(id) == 0;
    }
    void playOneShot(const std::string&, const glm::vec3&, const SoundParams*) override {}
    AudioEmitterId createEmitter(const std::string&, const glm::vec3&,
                                  const EmitterParams&) override { return 1; }
    void setEmitterPosition(AudioEmitterId, const glm::vec3&) override {}
    void stopEmitter(AudioEmitterId) override {}
    void setListener(const glm::vec3&, const glm::vec3&) override {}
    void update() override {}
    void shutdown() override {}
};

// Clean plugin: one registered sound, one correctly-bound material-sound entry.
int initClean(PluginContext* ctx) {
    MaterialProperties m{};
    m.palette_index = 1;
    ctx->register_material(ctx, "stone", m);
    ctx->register_sound(ctx, "stone_snd", "", SoundParams{});
    ctx->register_material_sound(ctx, "stone", AudioEvent::Break, "stone_snd");
    return 0;
}

// Plugin with a dangling binding: material-sound points to "ghost_snd" which
// is never registered via register_sound.
int initDanglingBinding(PluginContext* ctx) {
    MaterialProperties m{};
    m.palette_index = 2;
    ctx->register_material(ctx, "dirt", m);
    // Intentionally NOT calling register_sound("ghost_snd", ...).
    ctx->register_material_sound(ctx, "dirt", AudioEvent::Break, "ghost_snd");
    return 0;
}

// Plugin that registers a sound whose asset probe will be forced to fail by
// the ProbeBackend (simulates a missing or corrupt WAV on disk).
int initUnloadableAsset(PluginContext* ctx) {
    ctx->register_sound(ctx, "bad_snd", "missing.wav", SoundParams{});
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Pass 1: dangling binding
// ---------------------------------------------------------------------------

TEST(AudioValidation, DanglingBindingDetected) {
    PluginManager pm;
    pm.wireInPlugin(initDanglingBinding);
    ProbeBackend probe;

    auto issues = audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn);

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind,     audio::AudioValidationIssue::DanglingBinding);
    EXPECT_EQ(issues[0].sound_id, "ghost_snd");
}

// ---------------------------------------------------------------------------
// Pass 2: unloadable asset
// ---------------------------------------------------------------------------

TEST(AudioValidation, UnloadableAssetDetected) {
    PluginManager pm;
    pm.wireInPlugin(initUnloadableAsset);

    ProbeBackend probe;
    probe.failIds.insert("bad_snd");  // tell the probe backend to reject this id

    auto issues = audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn);

    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind,     audio::AudioValidationIssue::UnloadableAsset);
    EXPECT_EQ(issues[0].sound_id, "bad_snd");
}

// ---------------------------------------------------------------------------
// Both issues collected in a single pass
// ---------------------------------------------------------------------------

TEST(AudioValidation, BothIssuesCollectedBeforePolicyApplied) {
    PluginManager pm;
    pm.wireInPlugin(initDanglingBinding);
    pm.wireInPlugin(initUnloadableAsset);

    ProbeBackend probe;
    probe.failIds.insert("bad_snd");

    // Use Warn so we don't throw — we need to inspect the full issue list.
    auto issues = audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn);

    ASSERT_EQ(issues.size(), 2u);
    bool hasDangling = false, hasAsset = false;
    for (const auto& i : issues) {
        if (i.kind == audio::AudioValidationIssue::DanglingBinding) hasDangling = true;
        if (i.kind == audio::AudioValidationIssue::UnloadableAsset) hasAsset    = true;
    }
    EXPECT_TRUE(hasDangling);
    EXPECT_TRUE(hasAsset);
}

// ---------------------------------------------------------------------------
// Severity policies
// ---------------------------------------------------------------------------

TEST(AudioValidation, ErrorPolicyThrowsRuntimeError) {
    PluginManager pm;
    pm.wireInPlugin(initDanglingBinding);
    ProbeBackend probe;

    EXPECT_THROW(
        audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Error),
        std::runtime_error);
}

TEST(AudioValidation, WarnPolicyNeverThrows) {
    PluginManager pm;
    pm.wireInPlugin(initDanglingBinding);
    ProbeBackend probe;

    EXPECT_NO_THROW(audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn));
}

TEST(AudioValidation, AutoPolicyMatchesBuildType) {
    PluginManager pm;
    pm.wireInPlugin(initDanglingBinding);
    ProbeBackend probe;

    // Auto → Error in debug (#ifndef NDEBUG), Warn in release (#ifdef NDEBUG).
#ifndef NDEBUG
    EXPECT_THROW(
        audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Auto),
        std::runtime_error);
#else
    EXPECT_NO_THROW(audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Auto));
#endif
}

// ---------------------------------------------------------------------------
// Null backend
// ---------------------------------------------------------------------------

TEST(AudioValidation, NullBackendSkipsAssetProbe) {
    PluginManager pm;
    pm.wireInPlugin(initUnloadableAsset);

    // No backend → Pass 2 skipped entirely; the sound itself isn't bound to
    // any material-sound, so Pass 1 finds nothing either.
    auto issues = audio::validateAudio(pm, nullptr, audio::AudioStrictPolicy::Warn);
    EXPECT_TRUE(issues.empty());
}

// ---------------------------------------------------------------------------
// Clean setup
// ---------------------------------------------------------------------------

TEST(AudioValidation, CleanSetupProducesNoIssues) {
    PluginManager pm;
    pm.wireInPlugin(initClean);
    ProbeBackend probe;

    auto issues = audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn);
    EXPECT_TRUE(issues.empty());
}

TEST(AudioValidation, CleanSetupNeverThrowsUnderAnyPolicy) {
    PluginManager pm;
    pm.wireInPlugin(initClean);
    ProbeBackend probe;

    EXPECT_NO_THROW(audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Error));
    EXPECT_NO_THROW(audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Warn));
    EXPECT_NO_THROW(audio::validateAudio(pm, &probe, audio::AudioStrictPolicy::Auto));
}
