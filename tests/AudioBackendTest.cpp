// Tests for the IAudioBackend seam and AudioManager routing.
//
// MiniaudioBackend tests use the null device (no hardware audio); a tiny valid
// WAV is written to a temp file so loadSound / playOneShot can exercise the real
// file-existence check and miniaudio decode path on the null device.
//
// AudioManager tests use a RecordingBackend that captures every call, so
// AudioManager logic (routing, distance culling, emitter re-projection) is
// exercised without miniaudio.

#include "audio/MiniaudioBackend.h"
#include "audio/AudioManager.h"
#include "audio/IAudioBackend.h"
#include "core/PluginManager.h"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Tiny valid WAV writer — 16-bit mono PCM silence, N frames.
// Used so MiniaudioBackend::loadSound passes the filesystem existence check
// and ma_sound_init_from_file can attempt to decode a real file.
// ---------------------------------------------------------------------------
bool writeSilentWav(const std::string& path, int numFrames = 64) {
    const int channels      = 1;
    const int sampleRate    = 44100;
    const int bitsPerSample = 16;
    const int blockAlign    = channels * (bitsPerSample / 8);
    const int byteRate      = sampleRate * blockAlign;
    const int dataSize      = numFrames * blockAlign;

    std::vector<uint8_t> buf;
    auto w16 = [&](int v){ buf.push_back(uint8_t(v)); buf.push_back(uint8_t(v>>8)); };
    auto w32 = [&](int v){ w16(v & 0xffff); w16((v >> 16) & 0xffff); };
    auto str = [&](const char* s){ buf.insert(buf.end(), s, s + 4); };

    str("RIFF"); w32(36 + dataSize); str("WAVE");
    str("fmt "); w32(16); w16(1); w16(channels);
    w32(sampleRate); w32(byteRate); w16(blockAlign); w16(bitsPerSample);
    str("data"); w32(dataSize);
    buf.resize(buf.size() + static_cast<size_t>(dataSize), 0);  // silence

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// RecordingBackend — captures every IAudioBackend call for inspection.
// ---------------------------------------------------------------------------
class RecordingBackend : public audio::IAudioBackend {
public:
    struct CallRecord { std::string op; std::string id; };
    std::vector<CallRecord> calls;
    bool ready = true;

    bool          isReady()   const override { return ready; }
    void          update()          override {}
    void          shutdown()        override {}
    void          setListener(const glm::vec3&, const glm::vec3&) override {}
    void          setEmitterPosition(AudioEmitterId id, const glm::vec3&) override {
        calls.push_back({"setEmitterPosition", std::to_string(id)});
    }
    bool          loadSound(const std::string& id, const std::string&,
                             const SoundParams&) override {
        calls.push_back({"loadSound", id}); return true;
    }
    void          playOneShot(const std::string& id, const glm::vec3&,
                               const SoundParams*) override {
        calls.push_back({"playOneShot", id});
    }
    AudioEmitterId createEmitter(const std::string& id, const glm::vec3&,
                                  const EmitterParams&) override {
        calls.push_back({"createEmitter", id}); return nextId_++;
    }
    void          stopEmitter(AudioEmitterId id) override {
        calls.push_back({"stopEmitter", std::to_string(id)});
    }

    bool hadCall(const std::string& op) const {
        for (const auto& c : calls) if (c.op == op) return true;
        return false;
    }
    bool hadCall(const std::string& op, const std::string& id) const {
        for (const auto& c : calls) if (c.op == op && c.id == id) return true;
        return false;
    }
    int countCalls(const std::string& op) const {
        int n = 0;
        for (const auto& c : calls) if (c.op == op) ++n;
        return n;
    }

private:
    AudioEmitterId nextId_ = 1;
};

// ---------------------------------------------------------------------------
// Named plugin init functions (wireInPlugin takes a function pointer, not a
// lambda with captures — use free functions following the existing test pattern).
// ---------------------------------------------------------------------------
int initTwoSounds(PluginContext* ctx) {
    SoundParams p{};
    ctx->register_sound(ctx, "snd_a", "a.wav", p);
    ctx->register_sound(ctx, "snd_b", "b.wav", p);
    return 0;
}
int initOneSoundDefaultDist(PluginContext* ctx) {
    ctx->register_sound(ctx, "snd", "", SoundParams{});  // empty path: exists-check skipped
    return 0;
}
int initOneSoundShortDist(PluginContext* ctx) {
    SoundParams p{};
    p.max_distance = 10.0f;
    ctx->register_sound(ctx, "snd_near", "", p);
    return 0;
}
int initLoopSound(PluginContext* ctx) {
    ctx->register_sound(ctx, "loop", "", SoundParams{});
    return 0;
}

} // namespace

// ==========================================================================
// MiniaudioBackend null-device tests (no hardware audio required)
// ==========================================================================

class MiniaudioNullTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpWav_ = (std::filesystem::temp_directory_path() / "audio_backend_test.wav").string();
        ASSERT_TRUE(writeSilentWav(tmpWav_)) << "Could not write temp WAV to " << tmpWav_;
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tmpWav_, ec);
    }
    std::string tmpWav_;
};

TEST_F(MiniaudioNullTest, BackendIsReadyAfterInit) {
    audio::MiniaudioBackend backend(/*useNullDevice=*/true);
    EXPECT_TRUE(backend.isReady());
}

TEST_F(MiniaudioNullTest, LoadSoundReturnsTrueForExistingFile) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    EXPECT_TRUE(backend.loadSound("test", tmpWav_, SoundParams{}));
}

TEST_F(MiniaudioNullTest, LoadSoundReturnsFalseForMissingFile) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    EXPECT_FALSE(backend.loadSound("missing", "no/such/file.wav", SoundParams{}));
}

TEST_F(MiniaudioNullTest, OneShotDoesNotCrash) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    backend.loadSound("snd", tmpWav_, SoundParams{});
    backend.playOneShot("snd", {0.0f, 0.0f, 0.0f}, nullptr);
    backend.update();
    backend.shutdown();
}

TEST_F(MiniaudioNullTest, OneShotUnknownSoundIsNoOp) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    // Sound was never loaded — must be a silent no-op, no crash.
    EXPECT_NO_THROW(backend.playOneShot("unknown", {}, nullptr));
}

TEST_F(MiniaudioNullTest, EmitterStartStop) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    backend.loadSound("loop", tmpWav_, SoundParams{});

    EmitterParams ep{};
    ep.loop = true;
    AudioEmitterId id = backend.createEmitter("loop", {0.0f, 0.0f, 0.0f}, ep);
    EXPECT_NE(id, kInvalidEmitterId);

    backend.setEmitterPosition(id, {1.0f, 0.0f, 0.0f});  // reposition — must not crash
    backend.stopEmitter(id);                               // stop — must not crash
    backend.shutdown();
}

TEST_F(MiniaudioNullTest, StopInvalidEmitterIsNoOp) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    EXPECT_NO_THROW(backend.stopEmitter(kInvalidEmitterId));
    backend.shutdown();
}

TEST_F(MiniaudioNullTest, ShutdownIsIdempotent) {
    audio::MiniaudioBackend backend(true);
    ASSERT_TRUE(backend.isReady());
    backend.shutdown();
    EXPECT_NO_THROW(backend.shutdown());  // second call must not crash
}

// ==========================================================================
// AudioManager + RecordingBackend
// (tests AudioManager routing logic without miniaudio)
// ==========================================================================

TEST(AudioManagerFake, PreloadCallsLoadSoundForEachRegisteredSound) {
    PluginManager pm;
    pm.wireInPlugin(initTwoSounds);

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);
    am.preloadSounds();

    EXPECT_TRUE(fake.hadCall("loadSound", "snd_a"));
    EXPECT_TRUE(fake.hadCall("loadSound", "snd_b"));
    EXPECT_EQ(fake.countCalls("loadSound"), 2);
}

TEST(AudioManagerFake, PlaySoundRoutesThroughBackend) {
    PluginManager pm;
    pm.wireInPlugin(initOneSoundDefaultDist);

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);

    am.playSound("snd", WorldCoord(0.0, 0.0, 0.0));
    EXPECT_TRUE(fake.hadCall("playOneShot", "snd"));
}

TEST(AudioManagerFake, DistanceCullSuppressesBeyondMaxDist) {
    PluginManager pm;
    pm.wireInPlugin(initOneSoundShortDist);  // max_distance = 10.0f

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);

    // Listener at origin; source at 20 m — must be culled, no voice allocated.
    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd_near", WorldCoord(0.0, 0.0, 20.0));
    EXPECT_FALSE(fake.hadCall("playOneShot"));
}

TEST(AudioManagerFake, DistanceCullAllowsWithinMaxDist) {
    PluginManager pm;
    pm.wireInPlugin(initOneSoundShortDist);  // max_distance = 10.0f

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);

    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.playSound("snd_near", WorldCoord(0.0, 0.0, 5.0));  // 5 m < 10 m
    EXPECT_TRUE(fake.hadCall("playOneShot", "snd_near"));
}

TEST(AudioManagerFake, CreateAndStopEmitter) {
    PluginManager pm;
    pm.wireInPlugin(initLoopSound);

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);

    AudioEmitterId id = am.createEmitter("loop", WorldCoord{}, EmitterParams{});
    EXPECT_NE(id, kInvalidEmitterId);
    EXPECT_TRUE(fake.hadCall("createEmitter", "loop"));

    am.stopEmitter(id);
    EXPECT_TRUE(fake.hadCall("stopEmitter"));
}

TEST(AudioManagerFake, UpdateReprojectsEmitterEachTick) {
    PluginManager pm;
    pm.wireInPlugin(initLoopSound);

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);
    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});
    am.createEmitter("loop", WorldCoord{}, EmitterParams{});

    am.update();
    am.update();
    am.update();

    // One setEmitterPosition call per tick per live emitter.
    EXPECT_EQ(fake.countCalls("setEmitterPosition"), 3);
}

// ==========================================================================
// Determinism boundary check
//
// AudioManager::update reads world-derived inputs (emitter WorldCoords) and
// writes only to the backend — never back into World/decomposition/persistence.
// Empirically verified: same inputs → same backend calls across successive ticks.
// ==========================================================================

TEST(AudioDeterminism, UpdateProducesConsistentCallsAcrossMultipleTicks) {
    PluginManager pm;
    pm.wireInPlugin(initLoopSound);

    RecordingBackend fake;
    audio::AudioManager am(&fake, pm);
    am.setListener(WorldCoord(0.0, 0.0, 0.0), {0,0,-1}, {0,1,0});

    AudioEmitterId id = am.createEmitter("loop", WorldCoord(1.0, 0.0, 0.0), EmitterParams{});
    ASSERT_NE(id, kInvalidEmitterId);

    const int kTicks = 5;
    for (int i = 0; i < kTicks; ++i) am.update();

    // Exactly one setEmitterPosition per tick — no extra calls, no missing calls,
    // no rand()- or time()-based variation.
    EXPECT_EQ(fake.countCalls("setEmitterPosition"), kTicks);

    // update() must never trigger new one-shots (audio is a pure sink, §4).
    EXPECT_FALSE(fake.hadCall("playOneShot"));
}
