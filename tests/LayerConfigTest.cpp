// Smoke tests for LayerConfig — primarily here to exercise the test target
// wiring (tests link the voxel-engine library + GoogleTest). Expand as the
// validation rules in docs/ARCHITECTURE.md are implemented.

#include "core/LayerConfig.h"

#include <gtest/gtest.h>

TEST(LayerConfig, LoadsValidSingleLayer) {
    LayerConfig config = LayerConfig::loadFromString(R"(
layers:
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
)");
    ASSERT_EQ(config.layers().size(), 1u);
    EXPECT_EQ(config.layers()[0].name, "terrain");
    EXPECT_EQ(config.layerIndex("terrain"), 0);
    EXPECT_NE(config.findLayer("terrain"), nullptr);
    EXPECT_EQ(config.findLayer("nonexistent"), nullptr);
}

TEST(LayerConfig, RejectsEmptyLayerStack) {
    // At least one layer must be defined — an empty stack is a startup error.
    EXPECT_THROW(LayerConfig::loadFromString("layers: []"), std::exception);
}
