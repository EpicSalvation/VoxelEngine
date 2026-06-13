// Tests for AudioManager's floating-origin projection and distance culling.
//
// The floating-origin rule (ARCHITECTURE §1/§9/§16): the audio listener is
// always at the local origin; sources are fed toLocalFloat(camera) before
// the backend sees them. This file verifies:
//   1. The local vector is correctly computed as (source - listener).
//   2. The result is independent of absolute world position (the floating-origin
//      guarantee): at 1e9 m scale, a 5 m source offset is still 5 m local.
//   3. Sources beyond their registered max_distance are culled before a voice
//      is allocated; sources within range are not.
//
// No miniaudio types appear here — the backend is a lightweight fake that
// records the camera-local position it receives from AudioManager.

#include "audio/AudioManager.h"
#include "audio/IAudioBackend.h"
#include "core/PluginManager.h"
#include "WorldCoord.h"
#include "plugin_api.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

namespace {

// Records the camera-local position from the most recent playOneShot call.
class PositionCapturingBackend : public audio::IAudioBackend {
public:
    glm::vec3 capturedPos{};
    bool      played = false;

    bool isReady() const override { return true; }
    bool loadSound(const std::string&, const std::string&, const SoundParams&) override { return true; }
    void playOneShot(const std::string&, const glm::vec3& pos, const SoundParams*) override {
        capturedPos = pos;
        played      = true;
    }
    AudioEmitterId createEmitter(const std::string&, const glm::vec3&,
                                  const EmitterParams&) override { return 1; }
    void setEmitterPosition(AudioEmitterId, const glm::vec3&) override {}
    void stopEmitter(AudioEmitterId) override {}
    void setListener(const glm::vec3&, const glm::vec3&) override {}
    void update() override {}
    void shutdown() override {}
};

// Sound with default SoundParams (max_distance = 100.0f).
int initDefaultSound(PluginContext* ctx) {
    ctx->register_sound(ctx, "snd", "", SoundParams{});
    return 0;
}

// Sound with max_distance = 10.0f.
int initShortRangeSound(PluginContext* ctx) {
    SoundParams p{};
    p.max_distance = 10.0f;
    ctx->register_sound(ctx, "snd", "", p);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Floating-origin projection
// ---------------------------------------------------------------------------

TEST(AudioSpatial, SourceAtPositiveZProducesPositiveZLocal) {
    PluginManager pm;
    pm.wireInPlugin(initDefaultSound);

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd", WorldCoord(0.0, 0.0, 5.0));

    ASSERT_TRUE(cap.played);
    EXPECT_NEAR(cap.capturedPos.x, 0.0f, 1e-4f);
    EXPECT_NEAR(cap.capturedPos.y, 0.0f, 1e-4f);
    EXPECT_NEAR(cap.capturedPos.z, 5.0f, 1e-4f);
}

TEST(AudioSpatial, LocalVectorIsSourceMinusListener) {
    PluginManager pm;
    pm.wireInPlugin(initDefaultSound);

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    // Listener at (10, 20, 30), source at (13, 20, 30) → local = (3, 0, 0).
    am.setListener(WorldCoord(10.0, 20.0, 30.0), {1,0,0}, {0,1,0});
    am.playSound("snd", WorldCoord(13.0, 20.0, 30.0));

    ASSERT_TRUE(cap.played);
    EXPECT_NEAR(cap.capturedPos.x, 3.0f, 1e-4f);
    EXPECT_NEAR(cap.capturedPos.y, 0.0f, 1e-4f);
    EXPECT_NEAR(cap.capturedPos.z, 0.0f, 1e-4f);
}

TEST(AudioSpatial, FloatingOriginPreservesLocalPrecision) {
    // At 1e9 m scale a 32-bit float has insufficient precision to distinguish
    // two points 5 m apart — world-absolute floats would give ~0 or garbage.
    // toLocalFloat() subtracts in double before narrowing, so the 5 m offset
    // must survive intact.
    PluginManager pm;
    pm.wireInPlugin(initDefaultSound);

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    constexpr double kBigX = 1.0e9;
    am.setListener(WorldCoord(kBigX, 0.0, 0.0), {1,0,0}, {0,1,0});
    am.playSound("snd", WorldCoord(kBigX + 5.0, 0.0, 0.0));

    ASSERT_TRUE(cap.played);
    // Must be 5.0, not 0.0 (which float-precision subtraction at 1e9 would give).
    EXPECT_NEAR(cap.capturedPos.x, 5.0f, 0.01f);
    EXPECT_NEAR(cap.capturedPos.y, 0.0f, 1e-4f);
    EXPECT_NEAR(cap.capturedPos.z, 0.0f, 1e-4f);
}

TEST(AudioSpatial, ResultIndependentOfAbsoluteListenerPosition) {
    // A source 3 m north of the listener must give local (0, 0, 3) regardless
    // of whether the listener is at world origin or 1e8 m away.
    PluginManager pm;
    pm.wireInPlugin(initDefaultSound);

    PositionCapturingBackend capNear, capFar;
    audio::AudioManager amNear(&capNear, pm);
    audio::AudioManager amFar (&capFar,  pm);

    amNear.setListener(WorldCoord(0.0,   0.0, 0.0), {1,0,0}, {0,1,0});
    amFar .setListener(WorldCoord(1.0e8, 0.0, 0.0), {1,0,0}, {0,1,0});

    amNear.playSound("snd", WorldCoord(0.0,   0.0, 3.0));
    amFar .playSound("snd", WorldCoord(1.0e8, 0.0, 3.0));

    ASSERT_TRUE(capNear.played);
    ASSERT_TRUE(capFar.played);
    EXPECT_NEAR(capNear.capturedPos.z, 3.0f, 1e-4f);
    EXPECT_NEAR(capFar .capturedPos.z, 3.0f, 0.01f);  // double-precision subtraction at 1e8
}

// ---------------------------------------------------------------------------
// Distance culling
// ---------------------------------------------------------------------------

TEST(AudioSpatial, SourceBeyondMaxDistanceIsCulled) {
    PluginManager pm;
    pm.wireInPlugin(initShortRangeSound);  // max_distance = 10.0f

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd", WorldCoord(0.0, 0.0, 15.0));  // 15 m > 10 m

    EXPECT_FALSE(cap.played);
}

TEST(AudioSpatial, SourceAtExactMaxDistanceIsNotCulled) {
    // Culling check is distSq > maxDist² (strict greater-than), so a source
    // exactly at max_distance should still be heard.
    PluginManager pm;
    pm.wireInPlugin(initShortRangeSound);  // max_distance = 10.0f

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd", WorldCoord(0.0, 0.0, 10.0));  // exactly at boundary

    EXPECT_TRUE(cap.played);
}

TEST(AudioSpatial, SourceWithinMaxDistanceIsNotCulled) {
    PluginManager pm;
    pm.wireInPlugin(initShortRangeSound);  // max_distance = 10.0f

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd", WorldCoord(0.0, 0.0, 5.0));

    ASSERT_TRUE(cap.played);
    EXPECT_NEAR(cap.capturedPos.z, 5.0f, 1e-4f);
}

TEST(AudioSpatial, CallSiteOverrideMaxDistSupersedesRegisteredDefault) {
    // A call-site override with a tighter max_distance must cull a source
    // that the registered params would have passed.
    PluginManager pm;
    pm.wireInPlugin(initDefaultSound);  // registered max_distance = 100.0f

    PositionCapturingBackend cap;
    audio::AudioManager am(&cap, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});

    SoundParams ov{};
    ov.max_distance = 3.0f;  // override: only 3 m range
    am.playSound("snd", WorldCoord(0.0, 0.0, 5.0), &ov);  // 5 m > 3 m → culled

    EXPECT_FALSE(cap.played);
}
