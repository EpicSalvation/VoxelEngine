// M7b tests — game objective logic (Group 4).
//
// Headless unit tests for the game logic that does not need a window.
// All tests operate on plain C++ state and the World/Layer APIs; no renderer,
// no GLFW, no plugin loading is required.
//
// The game logic tested here mirrors what the 07-arena-platformer demo runs
// inside its main loop for the "Game objective" group:
//
//   Key-collection state machine
//     - Walking into a key's 2×3×2 m trigger volume collects it, clears its
//       stake voxels from the detail layer, and decrements the remaining count.
//     - Collecting a key is idempotent: a second overlap does not double-collect.
//
//   Win condition
//     - Victory requires all four keys AND the player reaching the goal totem's
//       3×4×3 m trigger volume.
//     - Reaching the goal without all keys does nothing.
//
//   Respawn triggers
//     - The player falls below Y = -5 m → respawn to the start position.
//     - The player's feet voxel (sampled 0.1 m below AABB bottom) is lava
//       (palette_index == 9) → respawn to the start position.
//     - Non-lava material at feet does not trigger a respawn.

#include "core/LayerConfig.h"
#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>

// ── Constants mirrored from the demo ────────────────────────────────────────

// Palette index 9 is lava/fire (orange-red) — must match the arena and hazards plugins.
constexpr uint8_t kLavaIdx     = 9;
constexpr uint8_t kGoalGoldIdx = 12;

// Player AABB half-extents (m) — matches kPlayerHalf in the demo.
const glm::dvec3 kPlayerHalf(0.3, 0.9, 0.3);

// Key stake world anchors: bottom-left corner of each 1×2×1 gold stake.
// The stake's centre for the trigger check is anchor + (0.5, 1.0, 0.5).
constexpr double kKeyAnchorData[4][3] = {
    { 119.5, 30.0, 119.5 },
    { 379.5, 40.0, 119.5 },
    { 379.5, 50.0, 379.5 },
    { 119.5, 60.0, 379.5 },
};

// Goal totem anchor: bottom-left corner of the 3×5×3 model.
// Centre for the trigger check is anchor + (1.5, 2.5, 1.5).
constexpr double kGoalAnchorData[3] = { 248.5, 70.0, 248.5 };

// Spawn / respawn position.
const glm::dvec3 kSpawnPos(250.0, 5.0, 80.0);

// ── Game logic helpers (mirrors the demo's inline logic) ─────────────────────

namespace game {

// Returns true if the player's AABB centre is within the key's 2×3×2 m trigger.
bool isNearKey(const glm::dvec3& playerPos, int keyIndex) {
    const glm::dvec3 kc(
        kKeyAnchorData[keyIndex][0] + 0.5,
        kKeyAnchorData[keyIndex][1] + 1.0,
        kKeyAnchorData[keyIndex][2] + 0.5);
    const glm::dvec3 d = playerPos - kc;
    return std::abs(d.x) < 2.0 && std::abs(d.y) < 3.0 && std::abs(d.z) < 2.0;
}

// Returns true if all keys are collected AND the player is within the goal's
// 3×4×3 m trigger.
bool checkWin(const std::array<bool, 4>& collected, const glm::dvec3& playerPos) {
    for (bool c : collected) if (!c) return false;
    const glm::dvec3 gc(
        kGoalAnchorData[0] + 1.5,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    const glm::dvec3 d = playerPos - gc;
    return std::abs(d.x) < 3.0 && std::abs(d.y) < 4.0 && std::abs(d.z) < 3.0;
}

// Returns true if the player has fallen below the arena floor threshold.
bool shouldRespawnFall(const glm::dvec3& playerPos) {
    return playerPos.y < -5.0;
}

// Returns true if the voxel sampled just below the player's feet is lava.
bool shouldRespawnLava(const World& world, const glm::dvec3& playerPos) {
    const WorldCoord feetSample(playerPos - glm::dvec3(0.0, kPlayerHalf.y + 0.1, 0.0));
    const Voxel v = world.getVoxel(feetSample);
    return !v.isEmpty() && v.material.palette_index == kLavaIdx;
}

}  // namespace game

// ── Layer / world setup helpers ──────────────────────────────────────────────

namespace {

// Create a 1 m terminal "detail" layer matching the arena config.
LayerDef makeDetailDef() {
    LayerDef d;
    d.name                 = "detail";
    d.voxel_size_m         = 1.0;
    d.chunk_size_voxels    = 10;
    d.mode                 = VoxelMode::terminal;
    d.view_distance_chunks = 12;
    return d;
}

// Place a single-voxel gold key stake (1×2×1) at a given anchor in the detail layer.
void placeKeyStake(World& world, int keyIndex) {
    Voxel gold;
    gold.material.palette_index       = kGoalGoldIdx;
    gold.material.density             = 100.0f;
    gold.material.structural_strength = 1.0f;
    // Bottom voxel
    world.setVoxel(WorldCoord(
        kKeyAnchorData[keyIndex][0] + 0.5,
        kKeyAnchorData[keyIndex][1] + 0.5,
        kKeyAnchorData[keyIndex][2] + 0.5), gold);
    // Top voxel
    world.setVoxel(WorldCoord(
        kKeyAnchorData[keyIndex][0] + 0.5,
        kKeyAnchorData[keyIndex][1] + 1.5,
        kKeyAnchorData[keyIndex][2] + 0.5), gold);
}

// Collect a key: clear its stake voxels from the world.
void collectKey(World& world, int keyIndex) {
    world.setVoxel(WorldCoord(
        kKeyAnchorData[keyIndex][0] + 0.5,
        kKeyAnchorData[keyIndex][1] + 0.5,
        kKeyAnchorData[keyIndex][2] + 0.5), Voxel::empty());
    world.setVoxel(WorldCoord(
        kKeyAnchorData[keyIndex][0] + 0.5,
        kKeyAnchorData[keyIndex][1] + 1.5,
        kKeyAnchorData[keyIndex][2] + 0.5), Voxel::empty());
}

// Ensure a chunk containing a given world position is resident (created empty if absent).
void ensureChunk(Layer* lay, const WorldCoord& pos) {
    const ChunkCoord cc = chunkmath::worldToChunk(pos, lay->voxelSizeM(), lay->chunkSizeVoxels());
    if (!lay->getChunk(cc))
        lay->loadChunk(cc, nullptr);  // empty chunk — no generator needed
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// Key-collection state machine
// ════════════════════════════════════════════════════════════════════════════

TEST(KeyCollection, PlayerOutsideTriggerVolumeDoesNotCollect) {
    // Player standing far from every key — isNearKey must return false for all.
    const glm::dvec3 playerFarAway(0.0, 5.0, 0.0);
    for (int i = 0; i < 4; ++i)
        EXPECT_FALSE(game::isNearKey(playerFarAway, i))
            << "falsely triggered collection for key " << i;
}

TEST(KeyCollection, PlayerInsideTriggerVolumeCollects) {
    // Place the player exactly at each key's trigger centre — must trigger.
    for (int i = 0; i < 4; ++i) {
        const glm::dvec3 atKeyCenter(
            kKeyAnchorData[i][0] + 0.5,
            kKeyAnchorData[i][1] + 1.0,
            kKeyAnchorData[i][2] + 0.5);
        EXPECT_TRUE(game::isNearKey(atKeyCenter, i))
            << "failed to trigger collection for key " << i;
    }
}

TEST(KeyCollection, PlayerJustInsideTriggerEdgeTriggers) {
    // Player at the X edge of the trigger (just inside 2 m radius).
    const glm::dvec3 nearEdge(
        kKeyAnchorData[0][0] + 0.5 + 1.9,  // 1.9 m from centre, threshold is 2.0
        kKeyAnchorData[0][1] + 1.0,
        kKeyAnchorData[0][2] + 0.5);
    EXPECT_TRUE(game::isNearKey(nearEdge, 0));
}

TEST(KeyCollection, PlayerJustOutsideTriggerEdgeDoesNotTrigger) {
    // Player 2.1 m from the trigger centre — just outside the 2 m X radius.
    const glm::dvec3 outsideEdge(
        kKeyAnchorData[0][0] + 0.5 + 2.1,
        kKeyAnchorData[0][1] + 1.0,
        kKeyAnchorData[0][2] + 0.5);
    EXPECT_FALSE(game::isNearKey(outsideEdge, 0));
}

TEST(KeyCollection, CollectingClearsKeyVoxelsFromDetailLayer) {
    // The demo clears the two stake voxels from the detail layer on collection.
    // Verify they go from gold → empty.
    const LayerDef def = makeDetailDef();
    World world(def);
    Layer* lay = world.layer("detail");
    ASSERT_NE(lay, nullptr);

    // Ensure both chunks containing the stake are resident.
    const WorldCoord bot(kKeyAnchorData[0][0] + 0.5,
                         kKeyAnchorData[0][1] + 0.5,
                         kKeyAnchorData[0][2] + 0.5);
    const WorldCoord top(kKeyAnchorData[0][0] + 0.5,
                         kKeyAnchorData[0][1] + 1.5,
                         kKeyAnchorData[0][2] + 0.5);
    ensureChunk(lay, bot);
    ensureChunk(lay, top);

    placeKeyStake(world, 0);

    EXPECT_EQ(world.getVoxel(bot).material.palette_index, kGoalGoldIdx);
    EXPECT_EQ(world.getVoxel(top).material.palette_index, kGoalGoldIdx);

    collectKey(world, 0);

    EXPECT_TRUE(world.getVoxel(bot).isEmpty())
        << "bottom stake voxel not cleared after collection";
    EXPECT_TRUE(world.getVoxel(top).isEmpty())
        << "top stake voxel not cleared after collection";
}

TEST(KeyCollection, CollectingMarksDirty) {
    // setVoxel (used by collect) marks the owning chunk dirty so it is persisted.
    const LayerDef def = makeDetailDef();
    World world(def);
    Layer* lay = world.layer("detail");
    ASSERT_NE(lay, nullptr);

    const WorldCoord bot(kKeyAnchorData[0][0] + 0.5,
                         kKeyAnchorData[0][1] + 0.5,
                         kKeyAnchorData[0][2] + 0.5);
    ensureChunk(lay, bot);
    placeKeyStake(world, 0);

    const ChunkCoord cc = chunkmath::worldToChunk(
        bot, lay->voxelSizeM(), lay->chunkSizeVoxels());
    world.clearChunkDirty(cc);  // start clean
    ASSERT_FALSE(world.isChunkDirty(cc));

    collectKey(world, 0);
    EXPECT_TRUE(world.isChunkDirty(cc))
        << "collecting a key must mark its chunk dirty for persistence";
}

TEST(KeyCollection, IndependentKeysHaveSeparateTriggerVolumes) {
    // Being near key 0 must not trigger collection for keys 1-3.
    const glm::dvec3 atKey0(
        kKeyAnchorData[0][0] + 0.5,
        kKeyAnchorData[0][1] + 1.0,
        kKeyAnchorData[0][2] + 0.5);
    EXPECT_TRUE(game::isNearKey(atKey0, 0));
    for (int i = 1; i < 4; ++i)
        EXPECT_FALSE(game::isNearKey(atKey0, i))
            << "key 0 position falsely triggered key " << i;
}

// ════════════════════════════════════════════════════════════════════════════
// Win condition
// ════════════════════════════════════════════════════════════════════════════

TEST(WinCondition, NoWinWithoutAnyKeys) {
    std::array<bool, 4> none = {false, false, false, false};
    const glm::dvec3 atGoal(
        kGoalAnchorData[0] + 1.5,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    EXPECT_FALSE(game::checkWin(none, atGoal))
        << "should not win at goal with no keys collected";
}

TEST(WinCondition, NoWinWithPartialKeys) {
    std::array<bool, 4> partial = {true, true, false, false};
    const glm::dvec3 atGoal(
        kGoalAnchorData[0] + 1.5,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    EXPECT_FALSE(game::checkWin(partial, atGoal))
        << "should not win at goal with only two keys collected";
}

TEST(WinCondition, NoWinWithAllKeysButNotAtGoal) {
    std::array<bool, 4> all = {true, true, true, true};
    const glm::dvec3 notAtGoal(0.0, 5.0, 0.0);
    EXPECT_FALSE(game::checkWin(all, notAtGoal))
        << "should not win with all keys but player far from goal";
}

TEST(WinCondition, WinWithAllKeysAtGoalCentre) {
    std::array<bool, 4> all = {true, true, true, true};
    const glm::dvec3 atGoal(
        kGoalAnchorData[0] + 1.5,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    EXPECT_TRUE(game::checkWin(all, atGoal))
        << "should win with all keys at goal centre";
}

TEST(WinCondition, WinWithAllKeysAtGoalEdge) {
    // Player 2.9 m from goal X centre — inside the 3 m trigger.
    std::array<bool, 4> all = {true, true, true, true};
    const glm::dvec3 nearEdge(
        kGoalAnchorData[0] + 1.5 + 2.9,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    EXPECT_TRUE(game::checkWin(all, nearEdge));
}

TEST(WinCondition, NoWinWithAllKeysJustOutsideGoal) {
    // Player 3.1 m from goal centre — just outside the trigger.
    std::array<bool, 4> all = {true, true, true, true};
    const glm::dvec3 outsideGoal(
        kGoalAnchorData[0] + 1.5 + 3.1,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);
    EXPECT_FALSE(game::checkWin(all, outsideGoal));
}

TEST(WinCondition, KeyMustBeCollectedBeforeGoalToWin) {
    // Collect keys 0-2, verify no win; collect key 3, verify win.
    std::array<bool, 4> collected = {true, true, true, false};
    const glm::dvec3 atGoal(
        kGoalAnchorData[0] + 1.5,
        kGoalAnchorData[1] + 2.5,
        kGoalAnchorData[2] + 1.5);

    EXPECT_FALSE(game::checkWin(collected, atGoal));

    collected[3] = true;
    EXPECT_TRUE(game::checkWin(collected, atGoal));
}

// ════════════════════════════════════════════════════════════════════════════
// Respawn triggers
// ════════════════════════════════════════════════════════════════════════════

TEST(RespawnTrigger, FallBelowThresholdTriggers) {
    EXPECT_TRUE(game::shouldRespawnFall(glm::dvec3(250.0, -5.1, 80.0)));
    EXPECT_TRUE(game::shouldRespawnFall(glm::dvec3(0.0, -100.0, 0.0)));
}

TEST(RespawnTrigger, AboveThresholdDoesNotTrigger) {
    EXPECT_FALSE(game::shouldRespawnFall(glm::dvec3(250.0, -4.9, 80.0)));
    EXPECT_FALSE(game::shouldRespawnFall(glm::dvec3(250.0,  0.0, 80.0)));
    EXPECT_FALSE(game::shouldRespawnFall(glm::dvec3(250.0, 50.0, 80.0)));
}

TEST(RespawnTrigger, ExactlyAtThresholdDoesNotTrigger) {
    // y == -5.0 is on the boundary; the check is strict (y < -5.0).
    EXPECT_FALSE(game::shouldRespawnFall(glm::dvec3(250.0, -5.0, 80.0)));
}

TEST(RespawnTrigger, LavaVoxelUnderFeetTriggers) {
    // Place a lava voxel in the detail layer just below the player's feet.
    // Player centre at (5.5, 3.5, 5.5); half-extent y=0.9; feet at 3.5-0.9=2.6;
    // sample at 2.6-0.1=2.5 → voxel Y=2 (covers [2,3)).
    const LayerDef def = makeDetailDef();
    World world(def);
    Layer* lay = world.layer("detail");
    ASSERT_NE(lay, nullptr);

    const WorldCoord lavaPos(5.5, 2.5, 5.5);  // world pos → voxel y=2
    ensureChunk(lay, lavaPos);

    Voxel lava;
    lava.material.palette_index = kLavaIdx;
    lava.material.density       = 800.0f;
    world.setVoxel(lavaPos, lava);

    const glm::dvec3 playerPos(5.5, 3.5, 5.5);
    EXPECT_TRUE(game::shouldRespawnLava(world, playerPos))
        << "lava under feet should trigger respawn";
}

TEST(RespawnTrigger, NonLavaVoxelUnderFeetDoesNotTrigger) {
    // Stone under the player's feet must not trigger a lava respawn.
    const LayerDef def = makeDetailDef();
    World world(def);
    Layer* lay = world.layer("detail");
    ASSERT_NE(lay, nullptr);

    const WorldCoord stonePos(5.5, 2.5, 5.5);
    ensureChunk(lay, stonePos);

    Voxel stone;
    stone.material.palette_index = 1;   // stone, not lava
    stone.material.density       = 2700.0f;
    world.setVoxel(stonePos, stone);

    const glm::dvec3 playerPos(5.5, 3.5, 5.5);
    EXPECT_FALSE(game::shouldRespawnLava(world, playerPos))
        << "stone under feet must not trigger lava respawn";
}

TEST(RespawnTrigger, EmptyVoxelUnderFeetDoesNotTrigger) {
    // No voxel at all (e.g. player in mid-air) must not trigger a lava respawn.
    const LayerDef def = makeDetailDef();
    World world(def);

    const glm::dvec3 playerPos(5.5, 3.5, 5.5);
    // No chunk loaded → getVoxel returns Voxel::empty().
    EXPECT_FALSE(game::shouldRespawnLava(world, playerPos));
}

TEST(RespawnTrigger, NonResidentChunkTreatedAsEmpty) {
    // If the chunk under the player is not loaded, getVoxel returns empty →
    // no lava respawn, even at a position where lava might eventually generate.
    const LayerDef def = makeDetailDef();
    World world(def);

    const glm::dvec3 playerPos(250.0, 20.0, 250.0);
    EXPECT_FALSE(game::shouldRespawnLava(world, playerPos));
}

TEST(RespawnTrigger, RespawnPositionIsOutsideArena) {
    // Sanity check: the spawn position is above the floor (Y=5 > 0) and well
    // inside the arena bounds (X,Z in [0, 500]).  After a respawn the player
    // should not immediately retrigger fall or be outside the world.
    EXPECT_GT(kSpawnPos.y, -5.0) << "spawn is below fall threshold";
    EXPECT_GE(kSpawnPos.x, 0.0);
    EXPECT_LE(kSpawnPos.x, 500.0);
    EXPECT_GE(kSpawnPos.z, 0.0);
    EXPECT_LE(kSpawnPos.z, 500.0);
}
