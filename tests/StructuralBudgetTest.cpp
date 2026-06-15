// M13 driver — per-frame budgets and determinism of the propagation pass
// (docs/ARCHITECTURE.md §7, §4). The deferred end-of-frame pass is capped by the
// tuning::physics knobs so a huge dirty burst or a big collapse spreads across
// frames instead of stalling one. These tests confirm:
//
//   - kMaxAggregateRecomputesPerFrame bounds the re-sum pass: a dirty burst larger
//     than the cap is processed in cap-sized slices, the overflow carrying frame to
//     frame until it drains;
//   - structural events beyond kMaxStructuralEventsPerFrame carry to following
//     frames — a collapse with more unstable macros than the event cap fires the
//     cap's worth this frame and the rest next frame(s), never stalling, and every
//     unstable macro fires exactly once;
//   - the fired unstable sequence is byte-identical across independent runs — no
//     rand()/time()/unordered iteration enters the path.
//
// All scenarios use a passive recorder response (it records fired macros but writes
// no voxel), so the world stays byte-identical and the budget mechanics are
// observed in isolation from any cascade.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "core/LayerConfig.h"
#include "core/PluginManager.h"
#include "core/Tuning.h"
#include "net/NetworkManager.h"
#include "simulation/PhysicsSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

// Small stack: 8 macros / axis (one chunk each), composite 2 m → terminal 1 m.
LayerConfig smallStack() {
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

// Large stack: 64 macros / axis, room for a wide unsupported region.
LayerConfig largeStack() {
    return LayerConfig::loadFromString(R"(
layers:
  - name: blocks
    voxel_size_m: 2.0
    mode: composite
    decompose_to: terrain
    chunk_size_voxels: 64
  - name: terrain
    voxel_size_m: 1.0
    mode: terminal
    chunk_size_voxels: 128
)");
}

Voxel stone() {
    Voxel v;
    v.material.density             = 1000.0f;
    v.material.structural_strength = 0.9f;
    v.material.palette_index       = 1;
    return v;
}

struct Rig {
    static constexpr int64_t kRatio = 2;
    World               world;
    PluginManager       pm;
    net::NetworkManager nm;
    sim::PhysicsSystem  physics;
    double              cvs;

    explicit Rig(const LayerConfig& cfg) : world(cfg), physics(world, pm) {
        nm.init(world, pm);
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        cvs = world.layer("terrain")->voxelSizeM();
    }

    WorldCoord childCenter(VoxelCoord cv) const {
        return chunkmath::voxelCenter(cv, cvs);
    }
    // Through the choke point → solid AND dirty (incremental + dirty mark).
    void fillMacroNm(VoxelCoord m, const Voxel& v) {
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, kRatio);
        for (int64_t dz = 0; dz < kRatio; ++dz)
            for (int64_t dy = 0; dy < kRatio; ++dy)
                for (int64_t dx = 0; dx < kRatio; ++dx)
                    nm.applyEdit(kLocalPlayer,
                                 childCenter({cmin.x + dx, cmin.y + dy, cmin.z + dz}), v);
    }
    // Directly on the layer → solid but NOT dirty (no incremental, no dirty mark).
    void fillMacroDirect(VoxelCoord m, const Voxel& v) {
        const VoxelCoord cmin = chunkmath::childVoxelMin(m, kRatio);
        for (int64_t dz = 0; dz < kRatio; ++dz)
            for (int64_t dy = 0; dy < kRatio; ++dy)
                for (int64_t dx = 0; dx < kRatio; ++dx)
                    world.layer("terrain")->setVoxel(
                        childCenter({cmin.x + dx, cmin.y + dy, cmin.z + dz}), v);
    }

    bool settled() const {
        return physics.eventsFiredLastTick() == 0 && physics.carryBacklog() == 0 &&
               !physics.propagation().hasDirty();
    }
};

// ── Passive recorder: records each fired macro, writes no voxel. ───────────────
std::vector<VoxelCoord> g_fired;

void recordHook(const StructuralEvent* ev, void*) {
    if (ev) g_fired.push_back({ev->voxel_x, ev->voxel_y, ev->voxel_z});
}
int recordInit(PluginContext* ctx) {
    ctx->register_on_structural_event(ctx, recordHook, nullptr);
    return 0;
}

// Builds the spaced-lattice scenario in `rig`: 64 macros on a stride-3 lattice
// (so no two share a face neighbor) made dirty, each surrounded by its 6 face
// neighbors filled solid but not dirty. The whole 64·7 = 448-macro set is interior
// (no anchor) so every one is unstable, but only via the 64 dirty seeds — letting a
// single pass discover > kMaxStructuralEventsPerFrame unstable macros at once.
int buildLatticeBlob(Rig& rig) {
    auto neighbors = [](VoxelCoord m) {
        return std::array<VoxelCoord, 6>{
            VoxelCoord{m.x - 1, m.y, m.z}, VoxelCoord{m.x + 1, m.y, m.z},
            VoxelCoord{m.x, m.y - 1, m.z}, VoxelCoord{m.x, m.y + 1, m.z},
            VoxelCoord{m.x, m.y, m.z - 1}, VoxelCoord{m.x, m.y, m.z + 1}};
    };
    int total = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                const VoxelCoord c{2 + 3 * i, 2 + 3 * j, 2 + 3 * k};
                for (const VoxelCoord& n : neighbors(c)) {
                    rig.fillMacroDirect(n, stone());  // solid, not dirty
                    ++total;
                }
                rig.fillMacroNm(c, stone());          // solid AND dirty seed
                ++total;
            }
    return total;  // 64 * 7 = 448 distinct interior macros
}

}  // namespace

// A dirty burst larger than kMaxAggregateRecomputesPerFrame is processed in
// cap-sized slices: the overflow carries frame to frame until it drains, and a
// fully-supported (stable) solid cube fires nothing throughout.
TEST(StructuralBudget, RecomputeCapBoundsTheResumPass) {
    namespace tp = tuning::physics;
    Rig rig(smallStack());

    // Fill the whole 8³ macro cube solid → anchored on every face → fully stable.
    int dirtied = 0;
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y)
            for (int z = 0; z < 8; ++z) { rig.fillMacroNm({x, y, z}, stone()); ++dirtied; }
    ASSERT_EQ(dirtied, 512);

    rig.physics.tick();
    EXPECT_EQ(rig.physics.eventsFiredLastTick(), 0) << "a solid anchored cube is stable";
    // Exactly kMaxAggregateRecomputesPerFrame were processed; the rest carried.
    EXPECT_EQ(rig.physics.carryBacklog(),
              static_cast<std::size_t>(512 - tp::kMaxAggregateRecomputesPerFrame));

    // Each subsequent frame drains another cap's worth, monotonically, to zero.
    std::size_t prev = rig.physics.carryBacklog();
    int frames = 1;
    while (rig.physics.carryBacklog() > 0 && frames < 64) {
        rig.physics.tick();
        ++frames;
        EXPECT_EQ(rig.physics.eventsFiredLastTick(), 0);
        EXPECT_LT(rig.physics.carryBacklog(), prev) << "backlog must strictly shrink";
        prev = rig.physics.carryBacklog();
    }
    EXPECT_EQ(rig.physics.carryBacklog(), 0u);
    EXPECT_EQ(frames, 512 / tp::kMaxAggregateRecomputesPerFrame);
}

// Events beyond kMaxStructuralEventsPerFrame carry to following frames: a collapse
// with more unstable macros than the cap fires the cap's worth now and the rest
// next frame(s), never stalling, and each unstable macro fires exactly once.
TEST(StructuralBudget, EventCapCarriesAcrossFrames) {
    namespace tp = tuning::physics;
    g_fired.clear();

    Rig rig(largeStack());
    rig.pm.wireInPlugin(recordInit);
    const int total = buildLatticeBlob(rig);
    ASSERT_EQ(total, 448);

    rig.physics.tick();
    // The first frame discovers all 448 unstable but fires only the cap; the rest
    // carry.
    EXPECT_EQ(rig.physics.eventsFiredLastTick(), tp::kMaxStructuralEventsPerFrame);
    EXPECT_EQ(rig.physics.carryBacklog(),
              static_cast<std::size_t>(total - tp::kMaxStructuralEventsPerFrame));

    int maxPerTick = rig.physics.eventsFiredLastTick();
    int frames = 1;
    while (!rig.settled() && frames < 300) {
        rig.physics.tick();
        ++frames;
        maxPerTick = std::max(maxPerTick, rig.physics.eventsFiredLastTick());
    }
    EXPECT_LT(frames, 300) << "the carried collapse must drain, not stall";
    EXPECT_LE(maxPerTick, tp::kMaxStructuralEventsPerFrame)
        << "no frame may exceed the event budget";
    EXPECT_GT(frames, 1) << "a > cap collapse must span multiple frames";
    EXPECT_EQ(g_fired.size(), static_cast<std::size_t>(total))
        << "every unstable macro must fire exactly once across the carried frames";
}

// The fired unstable sequence is byte-identical across independent runs — the §4
// determinism contract (sorted-coord order, no rand/time/unordered iteration).
TEST(StructuralBudget, FiredSequenceIsByteIdenticalAcrossRuns) {
    auto runOnce = []() {
        g_fired.clear();
        Rig rig(largeStack());
        rig.pm.wireInPlugin(recordInit);
        buildLatticeBlob(rig);
        int frames = 0;
        while (!rig.settled() && frames < 300) { rig.physics.tick(); ++frames; }
        return g_fired;  // copy out the full fired sequence
    };

    const std::vector<VoxelCoord> a = runOnce();
    const std::vector<VoxelCoord> b = runOnce();
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].x, b[i].x);
        EXPECT_EQ(a[i].y, b[i].y);
        EXPECT_EQ(a[i].z, b[i].z) << "fired sequence diverged at index " << i;
    }
}
