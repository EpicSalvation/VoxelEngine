// InteractiveLayerTest — M16 L4: explicit interactive-layer selection.
//
// World's single-layer forwarding API (getVoxel/setVoxel, dirty tracking,
// persistence, collision substep scale, picking) targets the layer declared
// interactive: true, rather than inferring "the first terminal layer in config
// order". These tests pin the three README acceptance behaviors:
//   - the API forwards to the DECLARED layer even when it is not the finest
//     terminal layer (a mid-stack playspace — the flying-game config);
//   - a config with no selector falls back to the documented default (first
//     terminal layer, then first layer);
//   - a config marking two interactive layers fails validation at startup.

#include "world/World.h"
#include "world/Layer.h"
#include "core/LayerConfig.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>
#include <stdexcept>

namespace {

// Three terminal layers, coarse→fine. The mid layer "play" is flagged
// interactive: true — it is neither the first terminal ("surface", the old
// inference's pick) nor the finest terminal ("detail").
const char* kMidStackInteractive = R"(
layers:
  - name: surface
    voxel_size_m: 9.0
    mode: terminal
    chunk_size_voxels: 8
  - name: play
    voxel_size_m: 3.0
    mode: terminal
    chunk_size_voxels: 8
    interactive: true
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
)";

// Same stack, no interactive flag anywhere — exercises the default fallback.
const char* kMidStackNoSelector = R"(
layers:
  - name: surface
    voxel_size_m: 9.0
    mode: terminal
    chunk_size_voxels: 8
  - name: play
    voxel_size_m: 3.0
    mode: terminal
    chunk_size_voxels: 8
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
)";

// Two layers both flagged interactive — must fail LayerConfig validation.
const char* kTwoInteractive = R"(
layers:
  - name: surface
    voxel_size_m: 9.0
    mode: terminal
    chunk_size_voxels: 8
    interactive: true
  - name: detail
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
    interactive: true
)";

// An immutable-only stack with no terminal layer and no selector — exercises the
// "then the first layer" leg of the default fallback.
const char* kNoTerminalNoSelector = R"(
layers:
  - name: backdrop
    voxel_size_m: 4.0
    mode: immutable
    chunk_size_voxels: 8
)";

void fillSolid(WorldCoord, int n, Voxel* out, void*) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = 1;
    for (int i = 0; i < n * n * n; ++i) out[i] = v;
}

}  // namespace

TEST(InteractiveLayer, ForwardsToDeclaredMidStackLayer) {
    World world(LayerConfig::loadFromString(kMidStackInteractive));

    // The single-layer API targets the declared "play" layer — not "surface"
    // (the old first-terminal inference) and not "detail" (the finest terminal).
    ASSERT_NE(world.primaryLayer(), nullptr);
    EXPECT_EQ(world.primaryLayer(), world.layer("play"));
    EXPECT_DOUBLE_EQ(world.voxelSizeM(), 3.0);  // the play layer's scale

    // Forwarding actually writes/reads through "play": load a chunk via the
    // single-layer API, edit it, and confirm the edit lands in "play" and is
    // invisible to the sibling layers.
    const ChunkCoord origin{0, 0, 0};
    ASSERT_NE(world.loadChunk(origin, &fillSolid), nullptr);

    const WorldCoord pos(1.5, 1.5, 1.5);
    Voxel edit;
    edit.material.density       = 2000.0f;
    edit.material.palette_index = 7;
    ASSERT_TRUE(world.setVoxel(pos, edit));

    EXPECT_EQ(world.getVoxel(pos).material.palette_index, 7);            // via primary
    EXPECT_EQ(world.layer("play")->getVoxel(pos).material.palette_index, 7);
    EXPECT_TRUE(world.layer("detail")->getVoxel(pos).isEmpty());        // sibling untouched
    EXPECT_TRUE(world.layer("surface")->getVoxel(pos).isEmpty());
}

TEST(InteractiveLayer, NoSelectorFallsBackToFirstTerminal) {
    World world(LayerConfig::loadFromString(kMidStackNoSelector));
    ASSERT_NE(world.primaryLayer(), nullptr);
    // Documented default: the first terminal layer in config order.
    EXPECT_EQ(world.primaryLayer(), world.layer("surface"));
}

TEST(InteractiveLayer, NoTerminalFallsBackToFirstLayer) {
    World world(LayerConfig::loadFromString(kNoTerminalNoSelector));
    ASSERT_NE(world.primaryLayer(), nullptr);
    // No terminal layer: fall back to the first layer.
    EXPECT_EQ(world.primaryLayer(), world.layer("backdrop"));
}

TEST(InteractiveLayer, TwoInteractiveLayersFailValidation) {
    EXPECT_THROW(LayerConfig::loadFromString(kTwoInteractive), std::runtime_error);
}

TEST(InteractiveLayer, SingleInteractiveLayerValidates) {
    EXPECT_NO_THROW(LayerConfig::loadFromString(kMidStackInteractive));
    const LayerConfig cfg = LayerConfig::loadFromString(kMidStackInteractive);
    const LayerDef* play = cfg.findLayer("play");
    ASSERT_NE(play, nullptr);
    EXPECT_TRUE(play->interactive);
    EXPECT_FALSE(cfg.findLayer("surface")->interactive);
}
