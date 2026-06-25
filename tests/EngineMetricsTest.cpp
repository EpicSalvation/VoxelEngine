// Tests for the EngineMetrics queryable struct (M17, sanity-check D2).

#include "core/Engine.h"
#include "core/EngineMetrics.h"
#include "audio/AudioManager.h"
#include "audio/IAudioBackend.h"
#include "world/World.h"
#include "world/Layer.h"
#include "core/LayerConfig.h"
#include "core/PluginManager.h"

#include <gtest/gtest.h>

namespace {

LayerDef terminalLayer(const std::string& name = "terrain") {
    LayerDef def;
    def.name = name;
    def.mode = VoxelMode::terminal;
    def.voxel_size_m = 1.0;
    def.chunk_size_voxels = 32;
    return def;
}

class StubAudioBackend : public audio::IAudioBackend {
public:
    size_t voices = 0;
    bool loadSound(const std::string&, const std::string&, const SoundParams&) override { return true; }
    void playOneShot(const std::string&, const glm::vec3&, const SoundParams*) override {}
    AudioEmitterId createEmitter(const std::string&, const glm::vec3&, const EmitterParams&) override { return kInvalidEmitterId; }
    void setEmitterPosition(AudioEmitterId, const glm::vec3&) override {}
    void stopEmitter(AudioEmitterId) override {}
    void setListener(const glm::vec3&, const glm::vec3&) override {}
    void update() override {}
    void shutdown() override {}
    bool isReady() const override { return true; }
    size_t activeVoiceCount() const override { return voices; }
};

TEST(EngineMetrics, DefaultsWithNoAttachments) {
    Engine engine;
    World world(terminalLayer("test"));
    PluginManager pm;
    engine.init(pm, world);

    EngineMetrics m = engine.getMetrics();
    EXPECT_EQ(m.drawCalls, 0u);
    EXPECT_EQ(m.voiceCount, 0u);
    EXPECT_EQ(m.decompInFlight, 0u);
    ASSERT_EQ(m.layers.size(), 1u);
    EXPECT_EQ(m.layers[0].layerName, "test");
    EXPECT_EQ(m.layers[0].residentChunks, 0u);
}

TEST(EngineMetrics, ReportsVoiceCount) {
    Engine engine;
    World world(terminalLayer());
    PluginManager pm;
    engine.init(pm, world);

    auto* backend = new StubAudioBackend();
    backend->voices = 7;
    audio::AudioManager audioMgr(backend, pm);
    engine.setAudioManager(&audioMgr);

    EngineMetrics m = engine.getMetrics();
    EXPECT_EQ(m.voiceCount, 7u);
}

TEST(EngineMetrics, ReportsResidentChunks) {
    Engine engine;
    World world(terminalLayer());
    PluginManager pm;
    engine.init(pm, world);

    world.loadChunk({0, 0, 0}, nullptr);
    world.loadChunk({1, 0, 0}, nullptr);

    EngineMetrics m = engine.getMetrics();
    ASSERT_EQ(m.layers.size(), 1u);
    EXPECT_EQ(m.layers[0].residentChunks, 2u);
    EXPECT_EQ(m.totalResidentChunks(), 2u);
}

TEST(EngineMetrics, MultiLayerChunkCounts) {
    const std::string yaml = R"(
layers:
  - name: coarse
    mode: composite
    voxel_size_m: 8.0
    chunk_size_voxels: 32
    decompose_to: fine
  - name: fine
    mode: terminal
    voxel_size_m: 1.0
    chunk_size_voxels: 32
    interactive: true
)";
    LayerConfig config = LayerConfig::loadFromString(yaml);
    World world(config);
    PluginManager pm;
    Engine engine;
    engine.init(pm, world);

    Layer* fineLayer = world.layer("fine");
    ASSERT_NE(fineLayer, nullptr);
    fineLayer->loadChunk({0, 0, 0}, nullptr);
    fineLayer->loadChunk({1, 0, 0}, nullptr);
    fineLayer->loadChunk({2, 0, 0}, nullptr);

    EngineMetrics m = engine.getMetrics();
    ASSERT_EQ(m.layers.size(), 2u);

    size_t fineIdx = (m.layers[0].layerName == "fine") ? 0 : 1;
    EXPECT_EQ(m.layers[fineIdx].residentChunks, 3u);
    EXPECT_EQ(m.totalResidentChunks(), 3u);
}

TEST(EngineMetrics, FrameTimeReflectsDeltaTime) {
    Engine engine;
    World world(terminalLayer());
    PluginManager pm;
    engine.init(pm, world);

    engine.update(0.016);
    EngineMetrics m = engine.getMetrics();
    EXPECT_DOUBLE_EQ(m.frameTimeSec, 0.016);
}

TEST(EngineMetrics, TotalResidentChunksEmpty) {
    EngineMetrics m;
    EXPECT_EQ(m.totalResidentChunks(), 0u);
}

} // namespace
