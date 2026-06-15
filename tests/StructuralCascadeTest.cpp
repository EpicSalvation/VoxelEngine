// M13 driver — the full structural cascade feedback loop (docs/ARCHITECTURE.md §7).
// Drives the engine-fires → plugin-edits → re-dirty → re-evaluate loop with a
// test crumble-style response plugin that clears unstable macros through the
// public edit path (ctx.apply_edit). These tests confirm:
//
//   - removing a support fires a structural event, the response plugin's edit
//     re-dirties the parent, and successive end-of-frame passes fire the NEXT ring
//     until the structure is stable — the cascade marches ring by ring because the
//     per-pass candidate set is the dirty macros plus their neighbors;
//   - the cascade terminates where it meets an anchor (the still-supported remnant
//     survives), in a small bounded number of frames, and is reproducible;
//   - the engine itself never writes a voxel: with NO response plugin loaded the
//     pass still fires events but leaves the world byte-identical ("no structural
//     plugin ⇒ no cave-ins", the detect/respond split).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "simulation/PhysicsSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

// Composite 2 m → terminal 1 m (ratio 2). One chunk of each covers world [0,16) m,
// so all 8 macros along an axis are resident and the x=-1 / x=8 region boundaries
// are the only anchors (no immutable layer).
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
    v.material.structural_strength = 0.9f;  // span ≈ 4.5 macros
    v.material.palette_index       = 1;
    return v;
}

struct Rig {
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::PhysicsSystem  physics;

    Rig() : world(twoLayer()), physics(world, pm) {
        nm.init(world, pm);
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    void editMacro(VoxelCoord m, const Voxel& v) {
        const int64_t ratio = 2;
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, ratio);
        const double cvs = world.layer("terrain")->voxelSizeM();
        for (int64_t dz = 0; dz < ratio; ++dz)
            for (int64_t dy = 0; dy < ratio; ++dy)
                for (int64_t dx = 0; dx < ratio; ++dx)
                    nm.applyEdit(kLocalPlayer,
                                 chunkmath::voxelCenter(
                                     {cmin.x + dx, cmin.y + dy, cmin.z + dz}, cvs),
                                 v);
    }

    bool macroSolid(VoxelCoord m) const {
        const int64_t ratio = 2;
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, ratio);
        const double cvs = world.layer("terrain")->voxelSizeM();
        for (int64_t dz = 0; dz < ratio; ++dz)
            for (int64_t dy = 0; dy < ratio; ++dy)
                for (int64_t dx = 0; dx < ratio; ++dx)
                    if (!world.getVoxel(chunkmath::voxelCenter(
                            {cmin.x + dx, cmin.y + dy, cmin.z + dz}, cvs)).isEmpty())
                        return true;
        return false;
    }

    // Drive end-of-frame passes until the loop settles; returns frame count.
    int runToSettle(int maxFrames = 32) {
        int frames = 0;
        while (frames < maxFrames) {
            physics.tick();
            ++frames;
            if (physics.eventsFiredLastTick() == 0 &&
                physics.carryBacklog() == 0 &&
                !physics.propagation().hasDirty())
                break;
        }
        return frames;
    }
};

// ── In-process crumble response: clears each unstable macro's children via the
//    public edit path, exactly like plugins/crumble. ───────────────────────────
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

// Snapshot every terminal voxel in the resident chunk so we can prove the world
// is byte-identical before/after an engine-only pass.
std::vector<std::pair<float, float>> snapshot(const World& w) {
    std::vector<std::pair<float, float>> out;
    const double cvs = w.layer("terrain")->voxelSizeM();
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const Voxel v = w.getVoxel(chunkmath::voxelCenter({x, y, z}, cvs));
                out.emplace_back(v.material.density, v.material.structural_strength);
            }
    return out;
}

}  // namespace

// Removing a support starts a cascade that marches ring by ring through successive
// passes and terminates at the anchor, leaving the still-supported remnant.
TEST(StructuralCascade, CascadesRingByRingAndTerminatesAtAnchor) {
    g_resp = nullptr;
    g_responses = 0;

    Rig rig;
    rig.pm.wireInPlugin(crumbleInit);
    // A beam x=0..7 at y=4,z=4 anchored at BOTH region boundaries (x=-1 and x=8) —
    // short enough (≤ ~2·span) to be fully supported as built.
    for (int x = 0; x <= 7; ++x) rig.editMacro({x, 4, 4}, stone());

    rig.physics.tick();
    ASSERT_EQ(rig.physics.eventsFiredLastTick(), 0)
        << "a doubly-anchored beam must be stable as built";
    ASSERT_EQ(g_responses, 0);

    // Mine out the two macros at the LEFT anchor end. The left-supported portion
    // now overhangs from the right anchor only and must cascade inward.
    rig.editMacro({0, 4, 4}, Voxel::empty());
    rig.editMacro({1, 4, 4}, Voxel::empty());

    const int frames = rig.runToSettle();
    EXPECT_LT(frames, 32) << "the cascade must terminate, not loop forever";
    EXPECT_GT(g_responses, 0) << "the response plugin must act on the fired events";

    // x=2,3 lose support and crumble; x=4..7 stay anchored on the x=8 boundary.
    EXPECT_FALSE(rig.macroSolid({2, 4, 4}));
    EXPECT_FALSE(rig.macroSolid({3, 4, 4}));
    EXPECT_TRUE(rig.macroSolid({4, 4, 4})) << "the cascade must stop at the anchored remnant";
    EXPECT_TRUE(rig.macroSolid({5, 4, 4}));
    EXPECT_TRUE(rig.macroSolid({6, 4, 4}));
    EXPECT_TRUE(rig.macroSolid({7, 4, 4}));
}

// The cascade is reproducible: the same scenario settles to the same world state
// and the same number of plugin responses across independent runs.
TEST(StructuralCascade, CascadeIsReproducible) {
    auto runOnce = [](std::array<bool, 8>& solid) {
        g_resp = nullptr;
        g_responses = 0;
        Rig rig;
        rig.pm.wireInPlugin(crumbleInit);
        for (int x = 0; x <= 7; ++x) rig.editMacro({x, 4, 4}, stone());
        rig.physics.tick();
        rig.editMacro({0, 4, 4}, Voxel::empty());
        rig.editMacro({1, 4, 4}, Voxel::empty());
        rig.runToSettle();
        for (int x = 0; x < 8; ++x) solid[x] = rig.macroSolid({x, 4, 4});
        return g_responses;
    };

    std::array<bool, 8> a{}, b{};
    const int ra = runOnce(a);
    const int rb = runOnce(b);
    EXPECT_EQ(ra, rb) << "the same cascade must produce the same number of responses";
    EXPECT_EQ(a, b) << "the same cascade must settle to byte-identical world state";
}

// Engine-never-writes invariant: with NO response plugin loaded the pass fires
// structural events but moves no voxel — the world is byte-identical afterward.
TEST(StructuralCascade, EngineFiresButWritesNoVoxel) {
    Rig rig;  // no plugin wired
    // A one-end cantilever x=0..5: the far macros are unsupported and must fire.
    for (int x = 0; x <= 5; ++x) rig.editMacro({x, 4, 4}, stone());

    const auto before = snapshot(rig.world);
    rig.physics.tick();
    const auto after = snapshot(rig.world);

    EXPECT_GT(rig.physics.eventsFiredLastTick(), 0)
        << "the unsupported cantilever must fire structural events";
    EXPECT_EQ(before, after)
        << "with no response plugin the engine must leave every voxel untouched";
}
