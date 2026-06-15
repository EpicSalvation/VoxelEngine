// M13 detection — propagation stops dead at an immutable boundary
// (docs/ARCHITECTURE.md §7). Anchors are (a) immutable-layer voxels and (b) the
// conservative non-resident-region boundary ("unknown ⇒ supported"). These tests
// confirm:
//
//   - anything 6-connected to bedrock keeps residual potential > 0 and never fires
//     (propagation stops dead at the immutable boundary) — and the same structure
//     WITHOUT the bedrock anchor does destabilize, proving bedrock is what holds it;
//   - the conservative rule means a structure whose support leaves the resident
//     region (rests on the streaming-edge boundary) is never spuriously collapsed;
//   - a world with no immutable layer produces no false cave-ins for a structure
//     that is genuinely supported.

#include <gtest/gtest.h>

#include <vector>

#include "core/LayerConfig.h"
#include "simulation/PropagationSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;
using Unstable = sim::PropagationSystem::Unstable;

// blocks (2 m composite → terrain) + terrain (1 m terminal) + bedrock (0.5 m
// immutable). Layers must be strictly descending in voxel size with integer ratios
// ≥2 between adjacent entries (LayerConfig validation), so bedrock is the finest
// layer here — a single bedrock voxel sampled at a macro's center is enough to make
// that one macro an anchor (precise, one bedrock voxel ⇒ one anchored macro). One
// chunk of each covers world [0,16) m.
LayerConfig bedrockStack() {
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
  - name: bedrock
    voxel_size_m: 0.5
    mode: immutable
    chunk_size_voxels: 32
)");
}

// No immutable layer at all — used for the "no false cave-ins" case.
LayerConfig plainStack() {
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

struct Rig {
    static constexpr int64_t kRatio = 2;
    World world;
    sim::PropagationSystem prop;
    double cvs, mvs;

    explicit Rig(const LayerConfig& cfg) : world(cfg), prop(world) {
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        if (world.layer("bedrock"))
            world.layer("bedrock")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        cvs = world.layer("terrain")->voxelSizeM();
        mvs = world.layer("blocks")->voxelSizeM();
    }

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
    void buildBeam(int x0, int x1, int y, int z, const Voxel& v) {
        for (int x = x0; x <= x1; ++x) fillMacro({x, y, z}, v);
    }
    // Place an immutable bedrock voxel at a macro coordinate.
    void setBedrock(VoxelCoord macro) {
        world.layer("bedrock")->setVoxel(chunkmath::voxelCenter(macro, mvs), mat(1.0f));
    }
    std::vector<VoxelCoord> beam(int x0, int x1, int y, int z) const {
        std::vector<VoxelCoord> v;
        for (int x = x0; x <= x1; ++x) v.push_back({x, y, z});
        return v;
    }
};

}  // namespace

// A structure 6-connected to bedrock keeps potential > 0 and never fires —
// propagation stops dead at the immutable boundary. The identical interior
// structure with NO bedrock has no anchor and does collapse, proving the
// difference is the immutable anchor (not some other support).
TEST(ImmutableBoundary, PropagationStopsAtImmutableBoundary) {
    // Interior beam x=1..4 at y=4,z=4 — far from every region-boundary anchor.
    {
        Rig rig(bedrockStack());
        rig.setBedrock({0, 4, 4});            // anchor adjacent to the beam's x=1 end
        rig.buildBeam(1, 4, 4, 4, mat(0.9f));
        EXPECT_TRUE(rig.prop.findUnstable(rig.beam(1, 4, 4, 4)).empty())
            << "a beam rooted on bedrock must keep residual potential > 0 and never fire";
    }
    {
        Rig rig(bedrockStack());              // same beam, no bedrock placed
        rig.buildBeam(1, 4, 4, 4, mat(0.9f));
        const auto u = rig.prop.findUnstable(rig.beam(1, 4, 4, 4));
        EXPECT_EQ(u.size(), 4u)
            << "without the immutable anchor the interior beam has no support and collapses";
    }
}

// The conservative "non-resident neighbor ⇒ supported" rule: a structure whose
// support path runs off the edge of the resident region is never spuriously
// collapsed — the x=0 macro is anchored by the x=-1 boundary it cannot see.
TEST(ImmutableBoundary, ConservativeBoundaryNeverSpuriouslyCollapses) {
    Rig rig(bedrockStack());
    rig.buildBeam(0, 3, 4, 4, mat(0.9f));  // x=0 sits on the resident-region boundary
    EXPECT_TRUE(rig.prop.findUnstable(rig.beam(0, 3, 4, 4)).empty())
        << "support leaving the resident region must be treated as solid, not collapsed";
}

// A world with no immutable layer produces no false cave-ins: a genuinely
// supported structure (anchored on both region boundaries) fires nothing.
TEST(ImmutableBoundary, NoImmutableLayerNoFalseCaveins) {
    Rig rig(plainStack());
    ASSERT_EQ(rig.world.layer("bedrock"), nullptr);
    rig.buildBeam(0, 7, 4, 4, mat(0.9f));  // spans the chunk; anchored at x=-1 and x=8
    EXPECT_TRUE(rig.prop.findUnstable(rig.beam(0, 7, 4, 4)).empty())
        << "a supported structure must not cave in just because there is no bedrock";
}
