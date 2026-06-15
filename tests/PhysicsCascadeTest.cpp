// M13 driver — PhysicsSystem feedback loop and the engine-never-writes invariant.
//
// These tests confirm the *Driver* half of M13 (docs/ARCHITECTURE.md §7):
//   - PhysicsSystem::tick() detects a destabilized macro and FIRES a structural
//     event, but with no response plugin loaded leaves the world byte-identical
//     (the engine never moves a voxel — "no structural plugin ⇒ no cave-ins").
//   - With a crumble-style response wired in, the cascade closes through the
//     public edit path (ctx.apply_edit → NetworkManager::applyEdit →
//     on_voxel_modified → re-dirty → next tick) and TERMINATES once the structure
//     is stable — no recursive in-engine collapse routine.
//
// The comprehensive detection/aggregation/budget suites named in the README
// (SupportFloodTest, StructuralCascadeTest, ...) remain their own tasks; this is
// the driver-level confirmation the Driver task calls for.

#include <gtest/gtest.h>

#include <cmath>

#include "core/PluginManager.h"
#include "core/LayerConfig.h"
#include "net/NetworkManager.h"
#include "simulation/PhysicsSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

// Composite 2 m "blocks" → terminal 1 m "terrain" (ratio 2). One blocks chunk
// (8 macros) and one terrain chunk (16 voxels) both cover world [0,16) m, so a
// macro's children are always resident. No immutable layer: the only anchors are
// the conservative non-resident-region boundary (§7).
LayerConfig twoLayer() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: blocks
    voxel_size_m: 2.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 8
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 16
)");
}

Voxel stone() {
    Voxel v;
    v.material.density             = 1000.0f;
    v.material.structural_strength = 0.9f;  // maxSpan ≈ 4.5 macros
    v.material.palette_index       = 1;
    return v;
}

// A headless single-player stack: world + plugin registry + offline network
// manager (the edit choke point) + the structural driver.
struct Rig {
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::PhysicsSystem  physics;

    Rig() : world(twoLayer()), physics(world, pm) {
        nm.init(world, pm);  // installs the apply_edit + on_voxel_modified routing
        // Make both the composite and terminal layers resident & empty.
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    // Edit every terminal child of macro `m` to `v`, through the choke point.
    void editMacro(VoxelCoord m, const Voxel& v) {
        const int64_t ratio = 2;
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, ratio);
        const double cvs = world.layer("terrain")->voxelSizeM();
        for (int64_t dz = 0; dz < ratio; ++dz)
            for (int64_t dy = 0; dy < ratio; ++dy)
                for (int64_t dx = 0; dx < ratio; ++dx) {
                    const VoxelCoord c{cmin.x + dx, cmin.y + dy, cmin.z + dz};
                    nm.applyEdit(kLocalPlayer, chunkmath::voxelCenter(c, cvs), v);
                }
    }

    bool macroSolid(VoxelCoord m) const {
        const int64_t ratio = 2;
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, ratio);
        const double cvs = world.layer("terrain")->voxelSizeM();
        for (int64_t dz = 0; dz < ratio; ++dz)
            for (int64_t dy = 0; dy < ratio; ++dy)
                for (int64_t dx = 0; dx < ratio; ++dx) {
                    const VoxelCoord c{cmin.x + dx, cmin.y + dy, cmin.z + dz};
                    if (!world.getVoxel(chunkmath::voxelCenter(c, cvs)).isEmpty())
                        return true;
                }
        return false;
    }

    // A horizontal beam at interior y=4,z=4, anchored only at its x=0 end by the
    // x=-1 region boundary. x in [0, len).
    void buildBeam(int len) {
        for (int x = 0; x < len; ++x) editMacro(VoxelCoord{x, 4, 4}, stone());
    }
};

// ── A crumble-style response plugin, wired in-process (no .dll path needed). It
//    clears each unstable macro's children via the public edit path — exactly
//    what plugins/crumble does. ───────────────────────────────────────────────
PluginContext* g_resp = nullptr;
int g_responses = 0;

void crumbleResponse(const StructuralEvent* ev, void*) {
    if (!g_resp || !ev) return;
    ++g_responses;
    const int ratio = static_cast<int>(
        std::llround(ev->voxel_size_m / ev->child_voxel_size_m));
    const double half = ev->voxel_size_m * 0.5;
    const double chalf = ev->child_voxel_size_m * 0.5;
    const Voxel empty = Voxel::empty();
    for (int dz = 0; dz < ratio; ++dz)
        for (int dy = 0; dy < ratio; ++dy)
            for (int dx = 0; dx < ratio; ++dx)
                g_resp->apply_edit(
                    g_resp,
                    WorldCoord(ev->position.value.x - half + chalf + dx * ev->child_voxel_size_m,
                               ev->position.value.y - half + chalf + dy * ev->child_voxel_size_m,
                               ev->position.value.z - half + chalf + dz * ev->child_voxel_size_m),
                    &empty);
}

int crumbleInit(PluginContext* ctx) {
    g_resp = ctx;
    ctx->register_on_structural_event(ctx, crumbleResponse, nullptr);
    return 0;
}

}  // namespace

// With no response plugin: a destabilized macro fires an event, but the engine
// writes nothing — the world stays byte-identical (the detect/respond split).
TEST(PhysicsCascade, EngineFiresButNeverWritesVoxels) {
    Rig rig;
    rig.buildBeam(4);  // x=0..3, fully within the support span → stable

    rig.physics.tick();
    EXPECT_EQ(rig.physics.eventsFiredLastTick(), 0)
        << "a fully-anchored beam must not fire spurious structural events";

    // Mine out the anchor-adjacent macro: the rest of the beam loses its path to
    // the only anchor and must destabilize.
    rig.editMacro(VoxelCoord{0, 4, 4}, Voxel::empty());

    rig.physics.tick();
    EXPECT_GT(rig.physics.eventsFiredLastTick(), 0)
        << "removing the support must fire structural events for the now-unsupported beam";

    // ENGINE-NEVER-WRITES: with no response plugin, the suspended beam macros are
    // still solid — PhysicsSystem fired events but moved no voxel.
    EXPECT_TRUE(rig.macroSolid(VoxelCoord{1, 4, 4}));
    EXPECT_TRUE(rig.macroSolid(VoxelCoord{2, 4, 4}));
    EXPECT_TRUE(rig.macroSolid(VoxelCoord{3, 4, 4}));
}

// With a crumble response: the cascade closes through the public edit path and
// terminates once nothing is left unstable.
TEST(PhysicsCascade, CrumbleResponseCascadesAndTerminates) {
    g_resp = nullptr;
    g_responses = 0;

    Rig rig;
    rig.pm.wireInPlugin(crumbleInit);  // register the structural-response hook
    rig.buildBeam(4);

    rig.physics.tick();  // stable: no events, no response
    ASSERT_EQ(g_responses, 0);

    rig.editMacro(VoxelCoord{0, 4, 4}, Voxel::empty());  // remove the support

    // Drive end-of-frame passes until the feedback loop settles. It MUST settle in
    // a small bounded number of frames (the loop terminates at the anchor).
    int frames = 0;
    const int kMaxFrames = 32;
    while (frames < kMaxFrames) {
        rig.physics.tick();
        ++frames;
        if (rig.physics.eventsFiredLastTick() == 0 &&
            rig.physics.carryBacklog() == 0 &&
            !rig.physics.propagation().hasDirty())
            break;
    }
    EXPECT_LT(frames, kMaxFrames) << "the cascade must terminate, not loop forever";
    EXPECT_GT(g_responses, 0) << "the response plugin must have acted on the events";

    // The whole beam has crumbled away — every macro is empty terminal space.
    for (int x = 0; x < 4; ++x)
        EXPECT_FALSE(rig.macroSolid(VoxelCoord{x, 4, 4}))
            << "macro x=" << x << " should have crumbled";
}
