// Tests for the runtime engine-config surface (include/core/EngineConfig.h, M17 D3b).
//
// The per-frame work budgets that used to be compile-time constants in
// src/core/Tuning.h are now read from the process-global engineConfig() each
// tick. These tests pin three things:
//   1. The runtime defaults are byte-identical to the Tuning.h baseline, so an
//      engine that never touches the config behaves exactly as before.
//   2. resetEngineConfig() restores every field.
//   3. A subsystem actually reads the runtime value — proven end-to-end through
//      LODManager's eviction hysteresis (one of the promoted knobs).

#include "core/EngineConfig.h"
#include "core/LayerConfig.h"
#include "core/Tuning.h"
#include "world/LODManager.h"

#include <gtest/gtest.h>

namespace {

// Every test restores defaults on teardown so the shared global never leaks
// across cases regardless of run order.
class EngineConfigTest : public ::testing::Test {
protected:
    void TearDown() override { resetEngineConfig(); }
};

LayerConfig makeConfig(int viewDistance) {
    return LayerConfig::loadFromString(
        "layers:\n"
        "  - name: terrain\n"
        "    voxel_size_m: 1.0\n"
        "    mode: terminal\n"
        "    view_distance_chunks: " + std::to_string(viewDistance) + "\n");
}

}  // namespace

// The runtime defaults mirror the Tuning.h compile-time baseline exactly — this is
// what makes the promotion a no-op for any consumer that never sets the config.
TEST_F(EngineConfigTest, DefaultsMatchTuningBaseline) {
    const EngineConfig& c = engineConfig();
    EXPECT_EQ(c.decompositionLoadPerFrame,   tuning::decomposition::kDefaultLoadPerFrame);
    EXPECT_EQ(c.decompositionDecompPerFrame, tuning::decomposition::kDefaultDecompPerFrame);
    EXPECT_EQ(c.decompositionApplyPerFrame,  tuning::decomposition::kDefaultApplyPerFrame);
    EXPECT_EQ(c.streamingHysteresisChunks,   tuning::streaming::kHysteresisChunks);
    EXPECT_EQ(c.physicsMaxAggregateRecomputesPerFrame, tuning::physics::kMaxAggregateRecomputesPerFrame);
    EXPECT_EQ(c.physicsMaxStructuralEventsPerFrame,    tuning::physics::kMaxStructuralEventsPerFrame);
    EXPECT_EQ(c.physicsMaxSupportFloodNodes,           tuning::physics::kMaxSupportFloodNodes);
    EXPECT_EQ(c.thermalMaxCellsPerFrame,     tuning::thermal::kMaxThermalCellsPerFrame);
    EXPECT_EQ(c.fluidMaxCellsPerFrame,       tuning::fluid::kMaxFluidCellsPerFrame);
    EXPECT_EQ(c.fluidMaxEventsPerFrame,      tuning::fluid::kMaxFluidEventsPerFrame);
    EXPECT_EQ(c.lightingMaxCellsPerFrame,    tuning::lighting::kMaxLightingCellsPerFrame);
    EXPECT_EQ(c.lightingMaxEventsPerFrame,   tuning::lighting::kMaxLightingEventsPerFrame);
}

// resetEngineConfig() puts every field back to its default.
TEST_F(EngineConfigTest, ResetRestoresDefaults) {
    engineConfig().thermalMaxCellsPerFrame = 7;
    engineConfig().streamingHysteresisChunks = 99;
    ASSERT_NE(engineConfig().thermalMaxCellsPerFrame, tuning::thermal::kMaxThermalCellsPerFrame);

    resetEngineConfig();

    EXPECT_EQ(engineConfig().thermalMaxCellsPerFrame, tuning::thermal::kMaxThermalCellsPerFrame);
    EXPECT_EQ(engineConfig().streamingHysteresisChunks, tuning::streaming::kHysteresisChunks);
}

// End-to-end: LODManager's eviction radius is view distance + the *runtime*
// hysteresis margin. Changing the config at runtime changes the computed radius
// with no rebuild — the whole point of D3b.
TEST_F(EngineConfigTest, LODManagerHonorsRuntimeHysteresis) {
    LayerConfig cfg = makeConfig(5);
    LODManager lod(cfg);

    // Default margin reproduces the documented baseline.
    EXPECT_EQ(lod.evictDistanceChunks("terrain"), 5 + LODManager::kHysteresisChunks);

    // Crank the margin up; the eviction radius follows immediately.
    engineConfig().streamingHysteresisChunks = 10;
    EXPECT_EQ(lod.evictDistanceChunks("terrain"), 5 + 10);

    // And a zero margin collapses eviction back to the load radius.
    engineConfig().streamingHysteresisChunks = 0;
    EXPECT_EQ(lod.evictDistanceChunks("terrain"), 5);
}
