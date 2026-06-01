// Multi-layer World container (M6).
//
// World holds a stack of Layers built from a LayerConfig (terminal, composite,
// immutable), each at its own voxel scale. These tests pin down the container
// contract the demo and decomposition systems rely on: layers are built in
// config order, the terminal layer backs the single-layer forwarding API, a
// composite layer resolves its decompose_to child, and anySolidAt samples every
// layer at its own scale (the cross-layer query collision and picking use).

#include "world/World.h"
#include "world/Layer.h"
#include "world/ChunkCoordMath.h"
#include "world/Voxel.h"
#include "core/LayerConfig.h"

#include <gtest/gtest.h>

namespace {

// A composite "blocks" layer (8 m) that decomposes to a terminal "terrain"
// layer (1 m). Sizes are strictly descending with whole-integer ratios, as the
// LayerConfig validator requires.
LayerConfig twoLayer() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: blocks
    voxel_size_m: 8.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
)");
}

// A stack with an immutable "backdrop" layer (2 m) above a terminal layer (1 m).
LayerConfig immutableStack() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: backdrop
    voxel_size_m: 2.0
    mode: immutable
    chunk_size_voxels: 8
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 8
)");
}

// Fills an entire chunk with a solid voxel — lets a test make a layer report
// solid at a known world position without depending on any generator.
void fillSolid(WorldCoord, int n, Voxel* out, void*) {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = 1;
    for (int i = 0; i < n * n * n; ++i) out[i] = v;
}

}  // namespace

TEST(MultiLayerWorld, BuildsOneLayerPerDefInOrder) {
    World world(twoLayer());
    ASSERT_EQ(world.layers().size(), 2u);
    EXPECT_EQ(world.layers()[0]->name(), "blocks");
    EXPECT_EQ(world.layers()[1]->name(), "terrain");
    EXPECT_EQ(world.layers()[0]->mode(), VoxelMode::composite);
    EXPECT_EQ(world.layers()[1]->mode(), VoxelMode::terminal);
}

TEST(MultiLayerWorld, PrimaryIsTheTerminalLayer) {
    World world(twoLayer());
    ASSERT_NE(world.primaryLayer(), nullptr);
    EXPECT_EQ(world.primaryLayer()->name(), "terrain");
    // The forwarding API reports the terminal layer's scale, not the (coarser,
    // first-in-config) composite layer's.
    EXPECT_DOUBLE_EQ(world.voxelSizeM(), 1.0);
}

TEST(MultiLayerWorld, ChildLayerResolvesDecomposeTo) {
    World world(twoLayer());
    Layer* blocks  = world.layer("blocks");
    Layer* terrain = world.layer("terrain");
    ASSERT_NE(blocks, nullptr);
    ASSERT_NE(terrain, nullptr);
    EXPECT_EQ(world.childLayer(*blocks), terrain);
    // The terminal layer has no decompose_to target.
    EXPECT_EQ(world.childLayer(*terrain), nullptr);
}

TEST(MultiLayerWorld, AnySolidAtSamplesEveryLayer) {
    World world(twoLayer());
    Layer* blocks = world.layer("blocks");
    ASSERT_NE(blocks, nullptr);

    // Make the composite layer solid in chunk {0,0,0}; leave terrain empty.
    blocks->loadChunk(ChunkCoord{0, 0, 0}, fillSolid, nullptr);

    // A point inside that composite chunk is solid via the composite layer even
    // though the terminal (primary) layer has nothing there.
    const WorldCoord inside =
        chunkmath::voxelCenter(chunkmath::VoxelCoord{0, 0, 0}, blocks->voxelSizeM());
    EXPECT_TRUE(world.anySolidAt(inside));
    EXPECT_TRUE(world.getVoxel(inside).isEmpty());  // primary (terrain) is empty

    // A point far outside any resident chunk is empty in every layer.
    const WorldCoord elsewhere(10000.0, 10000.0, 10000.0);
    EXPECT_FALSE(world.anySolidAt(elsewhere));
}

TEST(MultiLayerWorld, ImmutableLayerParticipatesInCollisionQuery) {
    World world(immutableStack());
    Layer* backdrop = world.layer("backdrop");
    ASSERT_NE(backdrop, nullptr);
    EXPECT_EQ(backdrop->mode(), VoxelMode::immutable);

    backdrop->loadChunk(ChunkCoord{0, 0, 0}, fillSolid, nullptr);

    const WorldCoord inside =
        chunkmath::voxelCenter(chunkmath::VoxelCoord{0, 0, 0}, backdrop->voxelSizeM());
    EXPECT_TRUE(world.anySolidAt(inside));    // immutable voxel is collidable
    EXPECT_TRUE(world.getVoxel(inside).isEmpty());  // but not part of the terminal layer
}

TEST(MultiLayerWorld, ImmutableLayerIsNeverDirtyAfterGeneration) {
    World world(immutableStack());
    Layer* backdrop = world.layer("backdrop");
    ASSERT_NE(backdrop, nullptr);

    // Generation populates the chunk but must leave it clean — an immutable layer
    // is never edited, so it never dirties and is never a persistence candidate.
    backdrop->loadChunk(ChunkCoord{0, 0, 0}, fillSolid, nullptr);
    EXPECT_FALSE(backdrop->isChunkDirty(ChunkCoord{0, 0, 0}));
    EXPECT_TRUE(backdrop->dirtyChunkCoords().empty());

    // The single-layer forwarding API targets the terminal layer, so a world-space
    // edit cannot reach (and dirty) the immutable layer even by coordinate overlap.
    const WorldCoord overlap =
        chunkmath::voxelCenter(chunkmath::VoxelCoord{0, 0, 0}, backdrop->voxelSizeM());
    world.setVoxel(overlap, Voxel{});
    EXPECT_TRUE(backdrop->dirtyChunkCoords().empty());
}
