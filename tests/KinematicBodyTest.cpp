// Tests for the per-frame tick hook (M17 B1) and the kinematic-body plugin's
// body registry, gravity integration, ground detection, and multi-body stepping.

#include "plugin_api.h"
#include "kinematic_body.h"
#include "world/VoxelCollision.h"
#include "world/World.h"
#include "world/ChunkCoordMath.h"
#include "core/LayerConfig.h"
#include "core/Engine.h"
#include "PluginManager.h"

#include <gtest/gtest.h>

// The kinematic-body plugin.cpp is compiled into this test binary (see
// CMakeLists.txt), so its voxel_plugin_init and the kinbody::api() singleton
// are in the same address space. Forward-declare the init function.
extern "C" int voxel_plugin_init(PluginContext* ctx);

namespace {

Voxel solid() {
    Voxel v;
    v.material.density       = 1000.0f;
    v.material.palette_index = 2;
    return v;
}

LayerDef terminalLayer(double voxelSize = 1.0, int chunkSize = 32) {
    LayerDef d;
    d.name = "terrain"; d.voxel_size_m = voxelSize;
    d.mode = VoxelMode::terminal; d.chunk_size_voxels = chunkSize;
    return d;
}

void setCell(World& w, int64_t x, int64_t y, int64_t z) {
    w.setVoxel(chunkmath::voxelCenter(chunkmath::VoxelCoord{x, y, z}, w.voxelSizeM()), solid());
}

World makeFlooredWorld() {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            setCell(w, x, 0, z);
    return w;
}

World makeEmptyWorld() {
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    return w;
}

// Static tick counters for the tick-hook tests (user_data pattern).
int  g_tickCount = 0;

void countingTickFn(double /*dt*/, void* /*ud*/) { ++g_tickCount; }

// Wrapper plugin init that registers the counting tick hook.
int tickPluginInit(PluginContext* ctx) {
    ctx->register_on_tick(ctx, countingTickFn, nullptr);
    return 0;
}

// move_aabb result capture for the ABI test.
BodyMoveResult g_moveResult{};

int moveAabbPluginInit(PluginContext* ctx) {
    g_moveResult = ctx->move_aabb(ctx,
        WorldCoord(5.0, 6.0, 5.0),
        0.3, 0.9, 0.3,
        0.0, -10.0, 0.0,
        0.0, -1.0, 0.0);
    return 0;
}

int moveAabbNoWorldPluginInit(PluginContext* ctx) {
    g_moveResult = ctx->move_aabb(ctx,
        WorldCoord(5.0, 6.0, 5.0),
        0.3, 0.9, 0.3,
        0.0, -10.0, 0.0,
        0.0, -1.0, 0.0);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine-level: tick hook fires
// ---------------------------------------------------------------------------

TEST(TickHook, RegisterAndFire) {
    PluginManager pm;
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    Engine engine;
    engine.init(pm, w);

    g_tickCount = 0;
    pm.wireInPlugin(tickPluginInit);

    engine.update(1.0 / 60.0);
    EXPECT_EQ(g_tickCount, 1);
    engine.update(1.0 / 60.0);
    EXPECT_EQ(g_tickCount, 2);
}

TEST(TickHook, UnloadedPluginStopsFiring) {
    PluginManager pm;
    World w(terminalLayer());
    w.loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    Engine engine;
    engine.init(pm, w);

    g_tickCount = 0;
    PluginId pid = pm.wireInPlugin(tickPluginInit);

    engine.update(1.0 / 60.0);
    EXPECT_EQ(g_tickCount, 1);

    pm.unloadPlugin(pid);
    engine.update(1.0 / 60.0);
    EXPECT_EQ(g_tickCount, 1);  // no longer fires
}

// ---------------------------------------------------------------------------
// Engine-level: move_aabb through the plugin ABI
// ---------------------------------------------------------------------------

TEST(MoveAABB_PluginABI, FallsOntoFloor) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);

    g_moveResult = {};
    pm.wireInPlugin(moveAabbPluginInit);

    EXPECT_TRUE(g_moveResult.grounded);
    EXPECT_TRUE(g_moveResult.hitY);
    EXPECT_NEAR(g_moveResult.position.value.y, 1.9, 1e-4);
}

TEST(MoveAABB_PluginABI, NoWorldReturnsCenter) {
    PluginManager pm;
    // No Engine::init — world is not set on PluginManager.

    g_moveResult = {};
    pm.wireInPlugin(moveAabbNoWorldPluginInit);

    EXPECT_NEAR(g_moveResult.position.value.x, 5.0, 1e-9);
    EXPECT_NEAR(g_moveResult.position.value.y, 6.0, 1e-9);
    EXPECT_FALSE(g_moveResult.grounded);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: body registry
// ---------------------------------------------------------------------------

TEST(KinematicBody, CreateAndDestroy) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 5.0, 5.0);
    kinbody::BodyId id = kinbody::api().create_body(&desc);
    EXPECT_NE(id, kinbody::kInvalidBody);
    EXPECT_EQ(kinbody::api().body_count(), 1u);

    kinbody::api().destroy_body(id);
    EXPECT_EQ(kinbody::api().body_count(), 0u);
}

TEST(KinematicBody, GetStateReturnsInitialPosition) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 10.0, 5.0);
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    const kinbody::BodyState* state = kinbody::api().get_state(id);
    ASSERT_NE(state, nullptr);
    EXPECT_NEAR(state->center.value.x, 5.0, 1e-9);
    EXPECT_NEAR(state->center.value.y, 10.0, 1e-9);
    EXPECT_NEAR(state->center.value.z, 5.0, 1e-9);

    kinbody::api().destroy_body(id);
}

TEST(KinematicBody, GetStateInvalidIdReturnsNull) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    EXPECT_EQ(kinbody::api().get_state(999), nullptr);
    kinbody::api().destroy_body(kinbody::kInvalidBody);  // idempotent
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: gravity and grounding
// ---------------------------------------------------------------------------

TEST(KinematicBody, FallsAndLandsOnFloor) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 5.0, 5.0);
    desc.gravity_accel = 25.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    for (int i = 0; i < 120; ++i)
        engine.update(1.0 / 60.0);

    const kinbody::BodyState* state = kinbody::api().get_state(id);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->grounded);
    EXPECT_NEAR(state->center.value.y, 1.9, 0.1);

    kinbody::api().destroy_body(id);
}

TEST(KinematicBody, FreeFallInEmptyWorld) {
    PluginManager pm;
    World w = makeEmptyWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 10.0, 5.0);
    desc.gravity_accel = 10.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    engine.update(1.0);

    const kinbody::BodyState* state = kinbody::api().get_state(id);
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->grounded);
    EXPECT_LT(state->center.value.y, 10.0);

    kinbody::api().destroy_body(id);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: jump
// ---------------------------------------------------------------------------

TEST(KinematicBody, JumpFromGround) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 1.9, 5.0);
    desc.gravity_accel = 25.0;
    desc.jump_speed = 9.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    // Settle on the floor.
    for (int i = 0; i < 10; ++i)
        engine.update(1.0 / 60.0);
    EXPECT_TRUE(kinbody::api().get_state(id)->grounded);

    kinbody::BodyInput input{};
    input.jump = true;
    kinbody::api().set_input(id, &input);
    engine.update(1.0 / 60.0);

    EXPECT_GT(kinbody::api().get_state(id)->center.value.y, 1.9);

    kinbody::api().destroy_body(id);
}

TEST(KinematicBody, JumpDoesNothingInAir) {
    PluginManager pm;
    World w = makeEmptyWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 10.0, 5.0);
    desc.gravity_accel = 0.0;
    desc.gravity_dir_x = 0.0; desc.gravity_dir_y = 0.0; desc.gravity_dir_z = 0.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    kinbody::BodyInput input{};
    input.jump = true;
    kinbody::api().set_input(id, &input);
    engine.update(1.0 / 60.0);

    EXPECT_NEAR(kinbody::api().get_state(id)->center.value.y, 10.0, 1e-4);

    kinbody::api().destroy_body(id);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: horizontal movement
// ---------------------------------------------------------------------------

TEST(KinematicBody, WishDirectionMovesHorizontally) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 1.9, 5.0);
    desc.walk_speed = 10.0;
    desc.gravity_accel = 25.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    for (int i = 0; i < 10; ++i)
        engine.update(1.0 / 60.0);

    double xBefore = kinbody::api().get_state(id)->center.value.x;

    kinbody::BodyInput input{};
    input.wish_x = 1.0;
    kinbody::api().set_input(id, &input);
    engine.update(1.0 / 60.0);

    EXPECT_GT(kinbody::api().get_state(id)->center.value.x, xBefore);

    kinbody::api().destroy_body(id);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: multi-body
// ---------------------------------------------------------------------------

TEST(KinematicBody, MultipleBodiesStepIndependently) {
    PluginManager pm;
    World w = makeFlooredWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc d1;
    d1.center = WorldCoord(3.0, 5.0, 3.0);
    kinbody::BodyId id1 = kinbody::api().create_body(&d1);

    kinbody::BodyDesc d2;
    d2.center = WorldCoord(10.0, 5.0, 10.0);
    kinbody::BodyId id2 = kinbody::api().create_body(&d2);

    EXPECT_EQ(kinbody::api().body_count(), 2u);

    for (int i = 0; i < 60; ++i)
        engine.update(1.0 / 60.0);

    const kinbody::BodyState* s1 = kinbody::api().get_state(id1);
    const kinbody::BodyState* s2 = kinbody::api().get_state(id2);
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    EXPECT_TRUE(s1->grounded);
    EXPECT_TRUE(s2->grounded);
    EXPECT_NEAR(s1->center.value.x, 3.0, 0.1);
    EXPECT_NEAR(s2->center.value.x, 10.0, 0.1);

    kinbody::api().destroy_body(id1);
    kinbody::api().destroy_body(id2);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: set_gravity at runtime
// ---------------------------------------------------------------------------

TEST(KinematicBody, SetGravityChangesDirection) {
    PluginManager pm;
    World w = makeEmptyWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(5.0, 5.0, 5.0);
    desc.gravity_accel = 10.0;
    desc.gravity_dir_x = 0.0; desc.gravity_dir_y = 0.0; desc.gravity_dir_z = 0.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    engine.update(0.5);
    EXPECT_NEAR(kinbody::api().get_state(id)->center.value.y, 5.0, 1e-6);

    kinbody::api().set_gravity(id, 1.0, 0.0, 0.0, 10.0);
    engine.update(0.5);

    EXPECT_GT(kinbody::api().get_state(id)->center.value.x, 5.0);

    kinbody::api().destroy_body(id);
}

// ---------------------------------------------------------------------------
// Kinematic body plugin: set_position (teleport)
// ---------------------------------------------------------------------------

TEST(KinematicBody, SetPositionTeleports) {
    PluginManager pm;
    World w = makeEmptyWorld();
    Engine engine;
    engine.init(pm, w);
    pm.wireInPlugin(voxel_plugin_init);

    kinbody::BodyDesc desc;
    desc.center = WorldCoord(1.0, 1.0, 1.0);
    desc.gravity_accel = 0.0;
    desc.gravity_dir_x = 0.0; desc.gravity_dir_y = 0.0; desc.gravity_dir_z = 0.0;
    kinbody::BodyId id = kinbody::api().create_body(&desc);

    kinbody::api().set_position(id, WorldCoord(99.0, 99.0, 99.0));
    const kinbody::BodyState* state = kinbody::api().get_state(id);
    EXPECT_NEAR(state->center.value.x, 99.0, 1e-9);
    EXPECT_NEAR(state->center.value.y, 99.0, 1e-9);
    EXPECT_NEAR(state->center.value.z, 99.0, 1e-9);
    EXPECT_NEAR(state->vel_x, 0.0, 1e-9);

    kinbody::api().destroy_body(id);
}
