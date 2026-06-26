// Tests for the LODManager chunk-budget math (src/renderer/LODManager.{h,cpp}).
// Headless: pure set/distance math built from a LayerConfig string.

#include "core/EngineConfig.h"
#include "core/LayerConfig.h"
#include "renderer/LODManager.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace {

LayerConfig makeConfig(int viewDistance) {
    return LayerConfig::loadFromString(
        "layers:\n"
        "  - name: terrain\n"
        "    voxel_size_m: 1.0\n"
        "    mode: terminal\n"
        "    view_distance_chunks: " + std::to_string(viewDistance) + "\n");
}

bool contains(const std::vector<ChunkCoord>& v, ChunkCoord c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

}  // namespace

TEST(LODManager, ReadsPerLayerViewDistance) {
    LayerConfig cfg = makeConfig(5);
    LODManager lod(cfg);
    EXPECT_EQ(lod.viewDistanceChunks("terrain"), 5);
    EXPECT_EQ(lod.evictDistanceChunks("terrain"), 5 + engineConfig().streamingHysteresisChunks);
    EXPECT_EQ(lod.viewDistanceChunks("nonexistent"), 0);
}

TEST(LODManager, DesiredChunkCountMatchesRadiusAndBand) {
    LayerConfig cfg = makeConfig(3);
    LODManager lod(cfg);
    lod.setVerticalBand(0, 1);  // 2 chunk-Y layers

    auto desired = lod.desiredChunks(ChunkCoord{0, 0, 0}, "terrain");
    const int side = 2 * 3 + 1;  // 7
    EXPECT_EQ(desired.size(), static_cast<size_t>(side * side * 2));
    EXPECT_TRUE(contains(desired, ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(contains(desired, ChunkCoord{3, 1, -3}));
    EXPECT_FALSE(contains(desired, ChunkCoord{4, 0, 0}));   // outside XZ radius
    EXPECT_FALSE(contains(desired, ChunkCoord{0, 2, 0}));   // outside band
}

TEST(LODManager, DesiredCentersOnCamera) {
    LayerConfig cfg = makeConfig(2);
    LODManager lod(cfg);
    lod.setVerticalBand(0, 0);
    auto desired = lod.desiredChunks(ChunkCoord{10, 0, -5}, "terrain");
    EXPECT_TRUE(contains(desired, ChunkCoord{10, 0, -5}));
    EXPECT_TRUE(contains(desired, ChunkCoord{12, 0, -3}));
    EXPECT_FALSE(contains(desired, ChunkCoord{13, 0, -5}));
}

TEST(LODManager, HysteresisBetweenLoadAndEvict) {
    LayerConfig cfg = makeConfig(4);
    LODManager lod(cfg);
    lod.setVerticalBand(0, 0);
    ChunkCoord center{0, 0, 0};

    // Inside load radius: desired and not evicted.
    EXPECT_TRUE(lod.withinViewDistance(center, ChunkCoord{4, 0, 0}, "terrain"));
    EXPECT_FALSE(lod.shouldEvict(center, ChunkCoord{4, 0, 0}, "terrain"));

    // In the hysteresis margin (radius < d <= radius+hyst): not desired, not yet evicted.
    EXPECT_FALSE(lod.withinViewDistance(center, ChunkCoord{5, 0, 0}, "terrain"));
    EXPECT_FALSE(lod.shouldEvict(center, ChunkCoord{5, 0, 0}, "terrain"));
    EXPECT_FALSE(lod.shouldEvict(center, ChunkCoord{6, 0, 0}, "terrain"));

    // Beyond the eviction radius: evicted.
    EXPECT_TRUE(lod.shouldEvict(center, ChunkCoord{7, 0, 0}, "terrain"));
}
