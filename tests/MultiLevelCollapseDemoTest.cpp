// Pins the scene that demos/19-multilevel-collapse builds: the multi-level
// (grandparent) structural cascade driven by grid-voxel edits, anchored by an
// IMMUTABLE bedrock layer rather than the resident-region boundary the engine-level
// MultiLevelPropagationTest uses. This guards the demo's central claim — mining the
// 1 m grid grandchildren of a deck macro near one bedrock tower caves the 4 m macro
// grandparents and the cascade STOPS at the macros still anchored to the far tower —
// and exercises PropagationSystem::isAnchor's immutable-layer path at every level of
// a deep stack (only the single-level demo 13 covered immutable anchors before).
//
// It mirrors the demo exactly: the macro→micro→grid composite chain plus a bedrock
// immutable layer, demo 13's proven 8-macro doubly-anchored bridge geometry at the
// macro scale, stone strength 0.9 (~4.5-macro span), and a grandparent-scale-only
// crumble response (the demo's grandparentCrumble / the engine test's macroCrumble).

#include <gtest/gtest.h>

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

// Identical to the demo's kLayerYaml: macro 4 m → micro 2 m → grid 1 m, plus a
// bedrock immutable anchor. macro chunk 16 (64 m) keeps the whole scene resident in
// one chunk so deck-perpendicular neighbors read resident-empty (not anchors).
LayerConfig demoLayers() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: macro
    voxel_size_m: 4.0
    mode: composite
    decompose_to: micro
    chunk_size_voxels: 16
  - name: micro
    voxel_size_m: 2.0
    mode: composite
    decompose_to: grid
    chunk_size_voxels: 2
  - name: grid
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 2
  - name: bedrock
    voxel_size_m: 0.5
    mode: immutable
    chunk_size_voxels: 64
)");
}

constexpr int64_t kMacroToGrid = 4;
constexpr int64_t kZ       = 8;
constexpr int64_t kYFloor  = 2;
constexpr int64_t kYDeck   = 6;
constexpr int64_t kXLTower = 3,  kXRTower = 12;
constexpr int64_t kDeckX0  = 4,  kDeckX1  = 11;
constexpr int     kMacroLevel = 1;   // macro is the coarsest composite (micro=0)

Voxel stone() {
    Voxel v;
    v.material.density             = 2700.0f;
    v.material.structural_strength = 0.9f;
    v.material.hardness            = 0.7f;
    v.material.palette_index       = 1;
    return v;
}
Voxel bedrock() {
    Voxel v;
    v.material.density             = 5000.0f;
    v.material.structural_strength = 9.9f;
    v.material.hardness            = -1.0f;
    v.material.palette_index       = 10;
    return v;
}

// ── Grandparent-scale crumble response (demo's grandparentCrumble), in-process ──
PluginContext* g_resp = nullptr;
int g_macroCollapses = 0;
constexpr double kGridSizeM = 1.0;

void grandparentCrumble(const StructuralEvent* ev, void*) {
    if (!ev || std::llround(ev->voxel_size_m) != 4) return;  // grandparent scale only
    ++g_macroCollapses;
    if (!g_resp) return;
    const double half  = ev->voxel_size_m * 0.5;
    const double ghalf = kGridSizeM * 0.5;
    const int    n     = static_cast<int>(std::llround(ev->voxel_size_m / kGridSizeM));
    const Voxel  empty = Voxel::empty();
    for (int dz = 0; dz < n; ++dz)
        for (int dy = 0; dy < n; ++dy)
            for (int dx = 0; dx < n; ++dx)
                g_resp->apply_edit(
                    g_resp,
                    WorldCoord(ev->position.value.x - half + ghalf + dx * kGridSizeM,
                               ev->position.value.y - half + ghalf + dy * kGridSizeM,
                               ev->position.value.z - half + ghalf + dz * kGridSizeM),
                    &empty);
}
int grandparentCrumbleInit(PluginContext* ctx) {
    g_resp = ctx;
    ctx->register_on_structural_event(ctx, grandparentCrumble, nullptr);
    return 0;
}

// Mirrors the demo's world. Owns build (direct setVoxel, silent), mining (choke
// point), and a run-to-settle loop.
struct DemoRig {
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::PhysicsSystem  physics;

    DemoRig() : world(demoLayers()), physics(world, pm) {
        nm.init(world, pm);
        world.layer("macro")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        buildBedrock();
        buildDeck();
    }

    Layer* grid()  { return world.layer("grid"); }
    Layer* micro() { return world.layer("micro"); }
    Layer* bed()   { return world.layer("bedrock"); }

    void ensureResident(VoxelCoord m) {
        micro()->loadChunk(ChunkCoord{static_cast<int32_t>(m.x), static_cast<int32_t>(m.y),
                                      static_cast<int32_t>(m.z)}, nullptr);
        for (int az = 0; az < 2; ++az)
            for (int ay = 0; ay < 2; ++ay)
                for (int ax = 0; ax < 2; ++ax)
                    grid()->loadChunk(ChunkCoord{static_cast<int32_t>(2 * m.x + ax),
                                                 static_cast<int32_t>(2 * m.y + ay),
                                                 static_cast<int32_t>(2 * m.z + az)},
                                      nullptr);
    }

    // Fill a macro's grid children directly (no events) — silent construction.
    void fillMacroGrid(VoxelCoord m, const Voxel& v) {
        const double g = grid()->voxelSizeM();
        const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
        for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
            for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
                for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                    grid()->setVoxel(chunkmath::voxelCenter({gmin.x + dx, gmin.y + dy, gmin.z + dz}, g), v);
    }

    void fillMacroBedrock(VoxelCoord m) {
        const double mvs = world.layer("macro")->voxelSizeM();
        const double bvs = bed()->voxelSizeM();
        const int64_t r = chunkmath::layerRatio(mvs, bvs);
        const int bcs = bed()->chunkSizeVoxels();
        const VoxelCoord bmin{m.x * r, m.y * r, m.z * r};
        for (int64_t dz = 0; dz < r; ++dz)
            for (int64_t dy = 0; dy < r; ++dy)
                for (int64_t dx = 0; dx < r; ++dx) {
                    const VoxelCoord bv{bmin.x + dx, bmin.y + dy, bmin.z + dz};
                    bed()->loadChunk(chunkmath::voxelToChunkLocal(bv, bcs).chunk, nullptr);
                    bed()->setVoxel(chunkmath::voxelCenter(bv, bvs), bedrock());
                }
    }

    void buildBedrock() {
        for (int64_t x = kXLTower; x <= kXRTower; ++x) fillMacroBedrock({x, kYFloor, kZ});
        for (int64_t y = kYFloor; y <= kYDeck; ++y) {
            fillMacroBedrock({kXLTower, y, kZ});
            fillMacroBedrock({kXRTower, y, kZ});
        }
    }
    void buildDeck() {
        for (int64_t x = kDeckX0; x <= kDeckX1; ++x) {
            ensureResident({x, kYDeck, kZ});
            fillMacroGrid({x, kYDeck, kZ}, stone());
        }
    }

    // Mine every grid grandchild of a deck macro through the choke point.
    void mineMacro(VoxelCoord m) {
        const double g = grid()->voxelSizeM();
        const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
        for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
            for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
                for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                    nm.applyEdit(kLocalPlayer,
                                 chunkmath::voxelCenter({gmin.x + dx, gmin.y + dy, gmin.z + dz}, g),
                                 Voxel::empty());
    }

    bool macroSolid(VoxelCoord m) {
        const double g = grid()->voxelSizeM();
        const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
        for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
            for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
                for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                    if (!world.getVoxel(chunkmath::voxelCenter({gmin.x + dx, gmin.y + dy, gmin.z + dz}, g))
                             .isEmpty())
                        return true;
        return false;
    }

    int runToSettle(int maxFrames = 64) {
        int frames = 0;
        while (frames < maxFrames) {
            physics.tick();
            ++frames;
            if (physics.eventsFiredLastTick() == 0 && physics.carryBacklog() == 0 &&
                !physics.propagation().hasDirty())
                break;
        }
        return frames;
    }

    std::vector<VoxelCoord> deckMacros() {
        std::vector<VoxelCoord> out;
        for (int64_t x = kDeckX0; x <= kDeckX1; ++x) out.push_back({x, kYDeck, kZ});
        return out;
    }
};

}  // namespace

// As built (silent construction), the doubly-anchored deck is stable at the macro
// scale — every deck macro is within stone's ~4.5-macro span of a bedrock tower.
TEST(MultiLevelCollapseDemo, DoublyAnchoredDeckStableAsBuilt) {
    g_resp = nullptr; g_macroCollapses = 0;
    DemoRig rig;
    EXPECT_NEAR(rig.physics.propagation().aggregateStrength(kMacroLevel, {kDeckX0, kYDeck, kZ}),
                0.9f, 1e-5)
        << "a freshly built deck macro aggregates its stone grandchildren";
    EXPECT_TRUE(rig.physics.propagation().findUnstable(kMacroLevel, rig.deckMacros()).empty())
        << "the bedrock-anchored deck must be stable at the grandparent scale as built";
}

// Mining the deck macro adjacent to the LEFT tower (entirely at the grid scale)
// fires MACRO-scale grandparent events and the grandparent-scale response caves the
// cut-off run — the cascade terminating at the macros still anchored to the RIGHT
// tower. The multi-level upward propagation made visible.
TEST(MultiLevelCollapseDemo, GrandparentCascadeStopsAtFarAnchoredRemnant) {
    g_resp = nullptr; g_macroCollapses = 0;
    DemoRig rig;
    rig.pm.wireInPlugin(grandparentCrumbleInit);

    // Sever the left anchor by mining the deck macro at x=4 (next to tower x=3),
    // grandchild by grandchild.
    rig.mineMacro({kDeckX0, kYDeck, kZ});
    const int frames = rig.runToSettle();

    EXPECT_LT(frames, 64) << "the macro cascade must terminate, not loop forever";
    EXPECT_GT(g_macroCollapses, 0) << "hollowing grandchildren must collapse grandparent macros";

    // x=4 mined; x=5,6,7 lose their path to the left tower and cannot reach the right
    // tower within ~4.5 macros → they cave.
    EXPECT_FALSE(rig.macroSolid({4, kYDeck, kZ})) << "the mined macro is empty";
    EXPECT_FALSE(rig.macroSolid({5, kYDeck, kZ}));
    EXPECT_FALSE(rig.macroSolid({6, kYDeck, kZ}));
    EXPECT_FALSE(rig.macroSolid({7, kYDeck, kZ}));

    // x=8..11 stay anchored on the right tower (x=12) → the cascade stops here.
    EXPECT_TRUE(rig.macroSolid({8,  kYDeck, kZ})) << "the cascade must stop at the anchored remnant";
    EXPECT_TRUE(rig.macroSolid({9,  kYDeck, kZ}));
    EXPECT_TRUE(rig.macroSolid({10, kYDeck, kZ}));
    EXPECT_TRUE(rig.macroSolid({11, kYDeck, kZ}));
}

// Engine-never-writes (§7): with NO response plugin, the same mining detects and
// fires grandparent events but the suspended deck is left byte-untouched.
TEST(MultiLevelCollapseDemo, NoResponsePluginLeavesDeckStanding) {
    g_resp = nullptr; g_macroCollapses = 0;
    DemoRig rig;  // no wireInPlugin

    rig.mineMacro({kDeckX0, kYDeck, kZ});
    rig.runToSettle();

    EXPECT_EQ(g_macroCollapses, 0) << "no response was registered";
    // The cut-off run is detected unstable but the engine writes nothing.
    EXPECT_TRUE(rig.macroSolid({6, kYDeck, kZ}));
    EXPECT_TRUE(rig.macroSolid({7, kYDeck, kZ}));
}
