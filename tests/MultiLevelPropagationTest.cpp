// M17 (gap audit G1) — multi-level upward damage propagation. M13 resolved
// structural stability at a single composite level only; M17 walks the full
// ancestor chain the M10 cascade computes, so a *grandchild* edit (a terminal
// voxel) re-evaluates not just its immediate parent macro but every coarser
// ancestor. These tests pin down the multi-level contract on a three-layer stack
// (macro 4 m → micro 2 m → grid 1 m, the demo's macro→micro→grid shape):
//
//   - Detection (PropagationSystem, driven directly): the support flood runs at a
//     coarse level using *recursive* aggregation — a macro's effective strength is
//     the average of its micro children's aggregates, which are in turn the average
//     of their grid children — so a macro cantilever is unstable beyond its span
//     and a doubly-anchored macro beam is stable, exactly as the single level was,
//     but two layers up from the edited voxels.
//   - Driver (PhysicsSystem): hollowing grid voxels (grandchildren) out of a
//     macro propagates dirty up the chain and fires a *macro-scale* structural
//     event for the now-unsupported grandparent — the behavior M13 deferred and
//     the single-level engine never produced. With no response plugin the engine
//     still writes nothing; with a macro-level crumble response the collapse
//     cascades macro-by-macro and terminates at the anchored remnant.

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "net/NetworkManager.h"
#include "simulation/PhysicsSystem.h"
#include "simulation/PropagationSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

// macro 4 m (composite→micro) → micro 2 m (composite→grid) → grid 1 m (terminal).
// Chunk sizes obey the §4 "parent voxel ≥ child chunk world size" rule, so each
// macro voxel maps to exactly one micro chunk (4 m) and a 2×2×2 block of grid
// chunks (2 m each). The single macro chunk {0,0,0} (chunk_size 8 → 32 m) makes
// macro voxels 0..7 resident in every axis, so the perpendicular beam neighbors
// are resident-empty (not anchors) and only the x=-1 / x=8 macro boundaries
// anchor (no immutable layer). Each macro's micro+grid subtree is streamed in on
// demand by ensureResident(). ratio macro→grid = 4.
LayerConfig threeLayer() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: macro
    voxel_size_m: 4.0
    mode: composite
    decompose_to: micro
    chunk_size_voxels: 8
  - name: micro
    voxel_size_m: 2.0
    mode: composite
    decompose_to: grid
    chunk_size_voxels: 2
  - name: grid
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 2
)");
}

constexpr int64_t kMacroToGrid = 4;  // grid voxels per macro voxel edge

// Stream in the micro + grid chunks covering one macro voxel's footprint so its
// aggregate can be computed (and its grid voxels edited). The macro chunk itself
// is loaded once by each rig. One macro voxel = one micro chunk = 2×2×2 grid chunks.
void ensureResident(World& w, VoxelCoord m) {
    w.layer("micro")->loadChunk(
        ChunkCoord{static_cast<int32_t>(m.x), static_cast<int32_t>(m.y),
                   static_cast<int32_t>(m.z)},
        nullptr);
    for (int az = 0; az < 2; ++az)
        for (int ay = 0; ay < 2; ++ay)
            for (int ax = 0; ax < 2; ++ax)
                w.layer("grid")->loadChunk(
                    ChunkCoord{static_cast<int32_t>(2 * m.x + ax),
                               static_cast<int32_t>(2 * m.y + ay),
                               static_cast<int32_t>(2 * m.z + az)},
                    nullptr);
}

Voxel stone() {
    Voxel v;
    v.material.density             = 1000.0f;
    v.material.structural_strength = 0.9f;  // span ≈ 4.5 macros at any scale
    v.material.palette_index       = 1;
    return v;
}

// The 64 grid voxels under one macro voxel.
std::vector<VoxelCoord> macroGridCells(VoxelCoord m) {
    const VoxelCoord gmin = chunkmath::childVoxelMin(m, kMacroToGrid);
    std::vector<VoxelCoord> cells;
    cells.reserve(kMacroToGrid * kMacroToGrid * kMacroToGrid);
    for (int64_t dz = 0; dz < kMacroToGrid; ++dz)
        for (int64_t dy = 0; dy < kMacroToGrid; ++dy)
            for (int64_t dx = 0; dx < kMacroToGrid; ++dx)
                cells.push_back({gmin.x + dx, gmin.y + dy, gmin.z + dz});
    return cells;
}

// A run of macro voxels along x at fixed (y, z), x in [x0, x1].
std::vector<VoxelCoord> macroBeam(int x0, int x1, int y, int z) {
    std::vector<VoxelCoord> out;
    for (int x = x0; x <= x1; ++x) out.push_back({x, y, z});
    return out;
}

// ── Detection rig: PropagationSystem driven directly, no NetworkManager /
//    PhysicsSystem. Grid voxels are written straight into the terminal layer; the
//    flood recomputes macro aggregates on demand by recursing macro→micro→grid. ──
struct DetRig {
    World                  world;
    sim::PropagationSystem prop;

    DetRig() : world(threeLayer()), prop(world) {
        world.layer("macro")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    void fillMacro(VoxelCoord m, const Voxel& v) {
        ensureResident(world, m);
        const double g = world.layer("grid")->voxelSizeM();
        for (const VoxelCoord& c : macroGridCells(m))
            world.layer("grid")->setVoxel(chunkmath::voxelCenter(c, g), v);
    }
};

std::set<int64_t> unstableXs(const std::vector<sim::PropagationSystem::Unstable>& u) {
    std::set<int64_t> xs;
    for (const auto& e : u) xs.insert(e.macro.x);
    return xs;
}

}  // namespace

// The chain has exactly two composite levels: micro (level 0) and macro (level 1).
TEST(MultiLevelPropagation, DiscoversFullCompositeChain) {
    DetRig rig;
    ASSERT_EQ(rig.prop.levelCount(), 2);
    EXPECT_EQ(rig.prop.compositeLayer(0)->name(), "micro");
    EXPECT_EQ(rig.prop.compositeLayer(1)->name(), "macro");
    EXPECT_DOUBLE_EQ(rig.prop.macroVoxelSizeM(1), 4.0);
    EXPECT_DOUBLE_EQ(rig.prop.childVoxelSizeM(1), 2.0);  // macro's child is micro
}

// A macro's aggregate is the recursive average of its grandchildren: fill all 64
// grid voxels of a macro with stone (0.9) and the MACRO (level 1) aggregate reads
// 0.9, even though nothing was ever written to the macro or micro layers.
TEST(MultiLevelPropagation, MacroAggregateIsRecursiveGrandchildAverage) {
    DetRig rig;
    const VoxelCoord m{2, 4, 4};
    rig.fillMacro(m, stone());

    EXPECT_NEAR(rig.prop.aggregateStrength(0, {m.x * 2, m.y * 2, m.z * 2}), 0.9f, 1e-5)
        << "a micro macro aggregates its 8 grid children";
    EXPECT_NEAR(rig.prop.aggregateStrength(1, m), 0.9f, 1e-5)
        << "a macro aggregates its 8 micro children, which aggregate their grid children";

    // Hollow out half the grandchildren: the macro aggregate halves — the whole
    // point of upward propagation, now resolved two levels up.
    auto cells = macroGridCells(m);
    const double g = rig.world.layer("grid")->voxelSizeM();
    for (size_t i = 0; i < cells.size() / 2; ++i)
        rig.world.layer("grid")->setVoxel(chunkmath::voxelCenter(cells[i], g),
                                          Voxel::empty());
    EXPECT_NEAR(rig.prop.aggregateStrength(1, m), 0.45f, 1e-5)
        << "removing half the grandchildren halves the grandparent aggregate";
}

// A macro cantilever anchored only at the x=-1 region boundary is stable within
// its support span and unstable beyond it — the single-level flood behavior, now
// at the MACRO scale driven by grid-level fills.
TEST(MultiLevelPropagation, GrandparentCantileverUnstableBeyondSpan) {
    DetRig rig;
    for (const VoxelCoord& m : macroBeam(0, 5, 4, 4)) rig.fillMacro(m, stone());

    const auto u = rig.prop.findUnstable(1, macroBeam(0, 5, 4, 4));
    const auto xs = unstableXs(u);
    EXPECT_TRUE(xs.count(4)) << "macro x=4 is beyond stone's ~4.5-macro span";
    EXPECT_TRUE(xs.count(5)) << "macro x=5 is beyond stone's span";
    EXPECT_FALSE(xs.count(0));
    EXPECT_FALSE(xs.count(1));
    EXPECT_FALSE(xs.count(2));
    EXPECT_FALSE(xs.count(3)) << "the near macros stay supported by the x=-1 anchor";
}

// A macro beam anchored at BOTH region boundaries (x=-1 and x=8), short enough to
// be spanned from each end, is stable at the macro scale — and the result is
// reproducible across independent floods.
TEST(MultiLevelPropagation, GrandparentDoublyAnchoredBeamStableAndReproducible) {
    DetRig rig;
    for (const VoxelCoord& m : macroBeam(0, 7, 4, 4)) rig.fillMacro(m, stone());

    const auto a = rig.prop.findUnstable(1, macroBeam(0, 7, 4, 4));
    const auto b = rig.prop.findUnstable(1, macroBeam(0, 7, 4, 4));
    EXPECT_TRUE(a.empty()) << "a doubly-anchored macro beam is stable at its own scale";
    EXPECT_EQ(a.size(), b.size()) << "the multi-level flood is deterministic";
}

// ── Driver rig: full PhysicsSystem feedback path. Grid edits flow through the
//    NetworkManager choke point so PropagationSystem observes them and the upward
//    cascade fires. ─────────────────────────────────────────────────────────────
struct DrvRig {
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::PhysicsSystem  physics;

    DrvRig() : world(threeLayer()), physics(world, pm) {
        nm.init(world, pm);
        world.layer("macro")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
    }

    // Edit every grid grandchild of macro `m` through the choke point.
    void editMacro(VoxelCoord m, const Voxel& v) {
        ensureResident(world, m);
        const double g = world.layer("grid")->voxelSizeM();
        for (const VoxelCoord& c : macroGridCells(m))
            nm.applyEdit(kLocalPlayer, chunkmath::voxelCenter(c, g), v);
    }

    bool macroSolid(VoxelCoord m) const {
        const double g = world.layer("grid")->voxelSizeM();
        for (const VoxelCoord& c : macroGridCells(m))
            if (!world.getVoxel(chunkmath::voxelCenter(c, g)).isEmpty()) return true;
        return false;
    }

    int runToSettle(int maxFrames = 64) {
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

namespace {

// Records every structural event's scale + macro coord.
struct RecordedEvent { double voxelSizeM; VoxelCoord macro; };
std::vector<RecordedEvent> g_events;

void recordResponse(const StructuralEvent* ev, void*) {
    if (ev) g_events.push_back({ev->voxel_size_m, {ev->voxel_x, ev->voxel_y, ev->voxel_z}});
}
int recorderInit(PluginContext* ctx) {
    ctx->register_on_structural_event(ctx, recordResponse, nullptr);
    return 0;
}

// Macro-level crumble: ignores micro/grid events and, for a macro-scale event,
// clears the macro's full grid footprint through the public edit path — a
// legitimate multi-level-aware response. Closes the feedback loop at the macro
// scale: cleared grid voxels re-dirty level 0 and the cascade re-aggregates up.
PluginContext* g_resp = nullptr;
int g_macroResponses = 0;
constexpr double kGridSizeM = 1.0;

void macroCrumble(const StructuralEvent* ev, void*) {
    if (!g_resp || !ev) return;
    if (std::llround(ev->voxel_size_m) != 4) return;  // macro scale only
    ++g_macroResponses;
    const double half  = ev->voxel_size_m * 0.5;
    const double ghalf = kGridSizeM * 0.5;
    const int n = static_cast<int>(std::llround(ev->voxel_size_m / kGridSizeM));
    const Voxel empty = Voxel::empty();
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
int macroCrumbleInit(PluginContext* ctx) {
    g_resp = ctx;
    ctx->register_on_structural_event(ctx, macroCrumble, nullptr);
    return 0;
}

size_t macroScaleEvents() {
    size_t n = 0;
    for (const auto& e : g_events)
        if (std::llround(e.voxelSizeM) == 4) ++n;
    return n;
}

}  // namespace

// The headline G1 behavior: mining GRID voxels (grandchildren) out of a macro
// fires a MACRO-scale structural event for the now-unsupported grandparent — an
// event the single-level M13 engine never produced — and the engine still writes
// nothing without a response plugin.
TEST(MultiLevelPropagation, GrandchildEditDestabilizesGrandparent) {
    g_events.clear();
    DrvRig rig;
    rig.pm.wireInPlugin(recorderInit);

    // Build a doubly-anchored macro beam (stable at the macro scale).
    for (const VoxelCoord& m : macroBeam(0, 7, 4, 4)) rig.editMacro(m, stone());
    rig.runToSettle();
    ASSERT_TRUE(rig.physics.propagation().findUnstable(1, macroBeam(0, 7, 4, 4)).empty())
        << "the doubly-anchored macro beam must be stable at the macro scale as built";

    // Now mine the two macros at the LEFT anchor end, entirely at the GRID scale.
    g_events.clear();
    rig.editMacro({0, 4, 4}, Voxel::empty());
    rig.editMacro({1, 4, 4}, Voxel::empty());
    rig.runToSettle();

    EXPECT_GT(macroScaleEvents(), 0u)
        << "hollowing grandchildren must fire a macro-scale (grandparent) event";
    bool firedForRemnant = false;
    for (const auto& e : g_events)
        if (std::llround(e.voxelSizeM) == 4 && (e.macro.x == 2 || e.macro.x == 3))
            firedForRemnant = true;
    EXPECT_TRUE(firedForRemnant)
        << "the macros that lost their path to the x=-1 anchor (x=2,3) must destabilize";

    // Engine-never-writes: no response plugin, so the suspended beam is untouched.
    EXPECT_TRUE(rig.macroSolid({2, 4, 4}));
    EXPECT_TRUE(rig.macroSolid({3, 4, 4}));
}

// With a macro-level crumble response wired in, the grandparent collapse cascades
// macro-by-macro through the public edit path and terminates at the anchored
// remnant — the multi-level analogue of the M13 single-level cascade.
TEST(MultiLevelPropagation, MacroCollapseCascadesAndTerminates) {
    g_events.clear();
    g_resp = nullptr;
    g_macroResponses = 0;

    DrvRig rig;
    rig.pm.wireInPlugin(macroCrumbleInit);

    for (const VoxelCoord& m : macroBeam(0, 7, 4, 4)) rig.editMacro(m, stone());
    rig.runToSettle();
    ASSERT_EQ(g_macroResponses, 0) << "a doubly-anchored beam triggers no macro collapse";

    // Mine out the left-anchor macros; the left-supported remnant must cascade.
    rig.editMacro({0, 4, 4}, Voxel::empty());
    rig.editMacro({1, 4, 4}, Voxel::empty());

    const int frames = rig.runToSettle();
    EXPECT_LT(frames, 64) << "the macro cascade must terminate, not loop forever";
    EXPECT_GT(g_macroResponses, 0) << "the response plugin must collapse the unsupported macros";

    // x=2,3 lose their path to the x=-1 anchor and crumble; x=4..7 stay anchored
    // on the x=8 boundary.
    EXPECT_FALSE(rig.macroSolid({2, 4, 4}));
    EXPECT_FALSE(rig.macroSolid({3, 4, 4}));
    EXPECT_TRUE(rig.macroSolid({4, 4, 4})) << "the cascade must stop at the anchored remnant";
    EXPECT_TRUE(rig.macroSolid({7, 4, 4}));
}
