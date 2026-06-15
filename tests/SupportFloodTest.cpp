// M13 detection — PropagationSystem support-potential flood (docs/ARCHITECTURE.md
// §7). The axis-free stability model: an anchor emits kAnchorPotential (1.0) that
// floods 6-connected through solid macros, draining 1/maxSpan(s) per step where
// maxSpan(s) = clamp(s·kSupportSpanPerStrength, 0, kMaxSupportSpan); a macro is
// unstable iff residual potential ≤ 0. These tests reproduce the §7 behaviors
// *without special cases*:
//
//   - strong material cantilevers farther than weak (the span formula);
//   - the weakest macro on a path drains potential fastest (weakest-link bridging);
//   - a macro that loses its only support path goes unstable;
//   - strength < kMinSupportStrength transmits nothing (even next to an anchor);
//   - the unstable set is deterministic (sorted-coord order) across repeated runs.
//
// No immutable layer here: the only anchor is the conservative non-resident-region
// boundary (a non-resident neighbor ⇒ solid support). A single composite chunk of
// 8 macros covers world [0,16) m, so the macro at x=0 is anchored by the x=-1
// boundary and the interior is free of any other anchor.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "core/LayerConfig.h"
#include "core/Tuning.h"
#include "simulation/PropagationSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;
using Unstable = sim::PropagationSystem::Unstable;

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

Voxel mat(float strength) {
    Voxel v;
    v.material.density             = 1000.0f;
    v.material.structural_strength = strength;
    v.material.palette_index       = 1;
    return v;
}

struct FloodRig {
    static constexpr int64_t kRatio = 2;
    World world;
    sim::PropagationSystem prop;
    double cvs;

    FloodRig() : world(twoLayer()), prop(world) {
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        cvs = world.layer("terrain")->voxelSizeM();
    }

    // Fill / clear a macro by writing all its terminal child cells directly.
    void fillMacro(VoxelCoord macro, const Voxel& v) {
        const VoxelCoord cmin = chunkmath::childVoxelMin(macro, kRatio);
        for (int64_t dz = 0; dz < kRatio; ++dz)
            for (int64_t dy = 0; dy < kRatio; ++dy)
                for (int64_t dx = 0; dx < kRatio; ++dx)
                    world.layer("terrain")->setVoxel(
                        chunkmath::voxelCenter(
                            {cmin.x + dx, cmin.y + dy, cmin.z + dz}, cvs),
                        v);
    }
    void clearMacro(VoxelCoord macro) { fillMacro(macro, Voxel::empty()); }

    // A straight beam of one material along x at fixed y,z, x in [x0, x1].
    void buildBeam(int x0, int x1, int y, int z, const Voxel& v) {
        for (int x = x0; x <= x1; ++x) fillMacro({x, y, z}, v);
    }

    std::vector<Unstable> findUnstable(const std::vector<VoxelCoord>& cand) const {
        return prop.findUnstable(cand);
    }
    static bool contains(const std::vector<Unstable>& u, VoxelCoord m) {
        return std::any_of(u.begin(), u.end(),
                           [&](const Unstable& e) { return e.macro.x == m.x &&
                                                           e.macro.y == m.y &&
                                                           e.macro.z == m.z; });
    }
};

std::vector<VoxelCoord> beamCoords(int x0, int x1, int y, int z) {
    std::vector<VoxelCoord> v;
    for (int x = x0; x <= x1; ++x) v.push_back({x, y, z});
    return v;
}

}  // namespace

// Strong material bridges a longer cantilever than weak: with span ≈ s·5 macros,
// a length-6 stone (0.9 → span 4.5) beam has unsupported far macros, while the
// identical-length strong (2.0 → span 10) beam is fully supported.
TEST(SupportFlood, StrongCantileversFartherThanWeak) {
    const auto beam = beamCoords(0, 5, 4, 4);  // anchored only at x=0 by x=-1 boundary

    {
        FloodRig weak;
        weak.buildBeam(0, 5, 4, 4, mat(0.9f));
        const auto u = weak.findUnstable(beam);
        // Stable within ~4 macros of the anchor; x=4,5 fall off the span.
        EXPECT_FALSE(FloodRig::contains(u, {0, 4, 4}));
        EXPECT_FALSE(FloodRig::contains(u, {3, 4, 4}));
        EXPECT_TRUE(FloodRig::contains(u, {4, 4, 4}));
        EXPECT_TRUE(FloodRig::contains(u, {5, 4, 4}));
    }
    {
        FloodRig strong;
        strong.buildBeam(0, 5, 4, 4, mat(2.0f));
        EXPECT_TRUE(strong.findUnstable(beam).empty())
            << "a strong-material beam of the same length must stay fully supported";
    }
}

// Weakest-link bridging: one very weak macro on an otherwise strong path drains
// potential fastest, so the bridge fails *at* the weak macro — everything from it
// outward goes unstable while the strongly-supported root stays put.
TEST(SupportFlood, WeakestLinkLimitsTheBridge) {
    // x=0..6 is a one-end cantilever (anchored only at x=0 by the x=-1 boundary; the
    // far end x=6 stops short of the x=8 boundary, so it has no second anchor).
    const auto beam = beamCoords(0, 6, 4, 4);

    {
        FloodRig allStrong;
        allStrong.buildBeam(0, 6, 4, 4, mat(2.0f));  // span 10 → length 7 fully bridged
        EXPECT_TRUE(allStrong.findUnstable(beam).empty());
    }
    {
        FloodRig weakLink;
        weakLink.buildBeam(0, 6, 4, 4, mat(2.0f));
        weakLink.fillMacro({3, 4, 4}, mat(0.1f));  // span 0.5 → drains > full potential
        const auto u = weakLink.findUnstable(beam);
        // The root up to the weak link stays supported; the weak macro and beyond fail.
        EXPECT_FALSE(FloodRig::contains(u, {0, 4, 4}));
        EXPECT_FALSE(FloodRig::contains(u, {2, 4, 4}));
        EXPECT_TRUE(FloodRig::contains(u, {3, 4, 4}));
        EXPECT_TRUE(FloodRig::contains(u, {6, 4, 4}));
    }
}

// A macro that loses its only support path goes unstable: a beam anchored at one
// end is stable until the anchor-adjacent macro is removed, after which the rest
// can no longer reach support.
TEST(SupportFlood, LosingTheOnlySupportPathDestabilizes) {
    FloodRig rig;
    rig.buildBeam(0, 3, 4, 4, mat(0.9f));  // x=0 anchored by x=-1 boundary
    EXPECT_TRUE(rig.findUnstable(beamCoords(0, 3, 4, 4)).empty())
        << "a fully-anchored short beam must be stable";

    rig.clearMacro({0, 4, 4});  // remove the single support
    const auto u = rig.findUnstable(beamCoords(0, 3, 4, 4));
    EXPECT_FALSE(FloodRig::contains(u, {0, 4, 4}));  // now empty space, not solid
    EXPECT_TRUE(FloodRig::contains(u, {1, 4, 4}));
    EXPECT_TRUE(FloodRig::contains(u, {2, 4, 4}));
    EXPECT_TRUE(FloodRig::contains(u, {3, 4, 4}));
}

// Material below kMinSupportStrength transmits no support at all — even the macro
// directly adjacent to the anchor is unstable (rubble/water/lava cannot hold up).
TEST(SupportFlood, BelowMinStrengthTransmitsNothing) {
    FloodRig rig;
    const float weak = tuning::physics::kMinSupportStrength * 0.5f;  // below the floor
    rig.buildBeam(0, 3, 4, 4, mat(weak));
    const auto u = rig.findUnstable(beamCoords(0, 3, 4, 4));
    EXPECT_TRUE(FloodRig::contains(u, {0, 4, 4}))
        << "even the anchor-adjacent macro fails when strength < kMinSupportStrength";
    EXPECT_TRUE(FloodRig::contains(u, {3, 4, 4}));
    EXPECT_EQ(u.size(), 4u) << "the whole sub-min beam is unstable";
}

// The flood output is deterministic: repeated runs over identical state produce a
// byte-identical, sorted-coord unstable set (the §4 determinism contract).
TEST(SupportFlood, DeterministicAndSortedOutput) {
    FloodRig rig;
    rig.buildBeam(0, 5, 4, 4, mat(0.9f));
    const auto beam = beamCoords(0, 5, 4, 4);

    const auto a = rig.findUnstable(beam);
    const auto b = rig.findUnstable(beam);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].macro.x, b[i].macro.x);
        EXPECT_EQ(a[i].macro.y, b[i].macro.y);
        EXPECT_EQ(a[i].macro.z, b[i].macro.z);
    }
    // Strictly increasing in VoxelCoordLess order.
    EXPECT_TRUE(std::is_sorted(a.begin(), a.end(),
                               [](const Unstable& l, const Unstable& r) {
                                   return sim::VoxelCoordLess{}(l.macro, r.macro);
                               }));
}
