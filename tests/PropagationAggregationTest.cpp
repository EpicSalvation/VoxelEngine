// M13 detection — PropagationSystem incremental structural-strength aggregation
// (docs/ARCHITECTURE.md §7). A decomposed composite macro carries an aggregate
// structural_strength equal to the volume-weighted average of its resident child
// voxels. These tests pin down the three aggregation contracts:
//
//   - an on_voxel_modified delta updates the parent macro's running aggregate
//     O(1), and the running value matches a full re-sum after a sequence of mixed
//     add / clear / replace edits (the bounded fallback agrees with incremental);
//   - an atomic (undecomposed) macro reports its own block material directly;
//   - the aggregate is volume-weighted over the macro's full child-cell count, so
//     mining children out lowers the average (the whole point of §7).
//
// PropagationSystem is driven directly here (no NetworkManager / PhysicsSystem):
// onVoxelModified applies the delta, recomputeAggregate is the re-sum fallback,
// aggregateStrength reads the result.

#include <gtest/gtest.h>

#include "core/LayerConfig.h"
#include "simulation/PropagationSystem.h"
#include "world/ChunkCoordMath.h"
#include "world/Layer.h"
#include "world/Voxel.h"
#include "world/World.h"

namespace {

using chunkmath::VoxelCoord;

// Composite 2 m "blocks" → terminal 1 m "terrain" (ratio 2, so 8 child cells per
// macro). One chunk of each covers world [0,16) m, so every macro's children are
// resident. The composite chunk is loaded empty, so every macro is *decomposed*
// (not atomic) unless a test explicitly writes a composite block voxel.
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

struct AggRig {
    static constexpr int64_t kRatio = 2;
    World                 world;
    sim::PropagationSystem prop;
    double                cvs;  // terminal (child) voxel size

    AggRig() : world(twoLayer()), prop(world) {
        world.layer("blocks")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        world.layer("terrain")->loadChunk(ChunkCoord{0, 0, 0}, nullptr);
        cvs = world.layer("terrain")->voxelSizeM();
    }

    WorldCoord childCenter(VoxelCoord cv) const {
        return chunkmath::voxelCenter(cv, cvs);
    }
    VoxelCoord childMin(VoxelCoord macro) const {
        return chunkmath::childVoxelMin(macro, kRatio);
    }

    // Write a child directly (no dirty / no incremental update) — used to set up a
    // baseline world state the test will then re-sum.
    void setChildDirect(VoxelCoord cv, const Voxel& v) {
        world.layer("terrain")->setVoxel(childCenter(cv), v);
    }
    // Write a child AND feed the old→new delta through the incremental hook, exactly
    // as the edit choke point would via on_voxel_modified.
    void setChildNotify(VoxelCoord cv, const Voxel& v) {
        const WorldCoord pos = childCenter(cv);
        const Voxel old = world.layer("terrain")->getVoxel(pos);
        world.layer("terrain")->setVoxel(pos, v);
        prop.onVoxelModified(pos, old, v);
    }

    // Fill all 8 children of a macro directly to the same voxel.
    void fillMacroDirect(VoxelCoord macro, const Voxel& v) {
        const VoxelCoord cmin = childMin(macro);
        for (int64_t dz = 0; dz < kRatio; ++dz)
            for (int64_t dy = 0; dy < kRatio; ++dy)
                for (int64_t dx = 0; dx < kRatio; ++dx)
                    setChildDirect({cmin.x + dx, cmin.y + dy, cmin.z + dz}, v);
    }
};

}  // namespace

// The aggregate is the volume-weighted average over the macro's FULL child-cell
// count — half-filled with strength 0.9 reads 0.45, not 0.9.
TEST(PropagationAggregation, VolumeWeightedAverageOverFullCellCount) {
    AggRig rig;
    const VoxelCoord m{0, 0, 0};

    // Fill 4 of the 8 child cells with stone (0.9); leave the other 4 empty.
    const VoxelCoord cmin = rig.childMin(m);
    int filled = 0;
    for (int64_t dz = 0; dz < AggRig::kRatio && filled < 4; ++dz)
        for (int64_t dy = 0; dy < AggRig::kRatio && filled < 4; ++dy)
            for (int64_t dx = 0; dx < AggRig::kRatio && filled < 4; ++dx) {
                rig.setChildDirect({cmin.x + dx, cmin.y + dy, cmin.z + dz}, mat(0.9f));
                ++filled;
            }

    rig.prop.recomputeAggregate(m);
    EXPECT_NEAR(rig.prop.aggregateStrength(m), 4 * 0.9f / 8.0f, 1e-5)
        << "aggregate must be Σ(child strength) / ratio³, i.e. volume-weighted over "
           "all 8 cells";
}

// An atomic (undecomposed) macro reports its own block material directly, with no
// child sum — recomputeAggregate keeps no baseline for it.
TEST(PropagationAggregation, AtomicMacroReportsOwnBlockMaterial) {
    AggRig rig;
    const VoxelCoord m{5, 5, 5};

    // Write a solid composite block voxel (not decomposed) at the macro.
    rig.world.layer("blocks")->setVoxel(
        chunkmath::voxelCenter(m, rig.world.layer("blocks")->voxelSizeM()), mat(0.7f));

    EXPECT_NEAR(rig.prop.aggregateStrength(m), 0.7f, 1e-5)
        << "an atomic macro's aggregate is its own block structural_strength";

    rig.prop.recomputeAggregate(m);  // must stay a no-op baseline-wise
    EXPECT_NEAR(rig.prop.aggregateStrength(m), 0.7f, 1e-5);
}

// The O(1) incremental running aggregate matches a full re-sum after a sequence of
// mixed add / clear / replace edits — and the bounded recompute fallback agrees.
TEST(PropagationAggregation, IncrementalDeltaMatchesFullResum) {
    AggRig rig;
    const VoxelCoord m{1, 1, 1};
    const VoxelCoord cmin = rig.childMin(m);

    // Baseline: 8 cells of stone (0.9). recomputeAggregate establishes the running
    // sum from current world state.
    rig.fillMacroDirect(m, mat(0.9f));
    rig.prop.recomputeAggregate(m);
    EXPECT_NEAR(rig.prop.aggregateStrength(m), 0.9f, 1e-5);

    const VoxelCoord a{cmin.x + 0, cmin.y + 0, cmin.z + 0};
    const VoxelCoord b{cmin.x + 1, cmin.y + 0, cmin.z + 0};
    const VoxelCoord c{cmin.x + 0, cmin.y + 1, cmin.z + 0};

    // A mix of replace / clear / add, each fed through the incremental hook.
    rig.setChildNotify(a, mat(2.0f));         // replace 0.9 → 2.0
    rig.setChildNotify(b, Voxel::empty());    // clear   0.9 → 0
    rig.setChildNotify(b, mat(1.5f));         // add     0   → 1.5
    rig.setChildNotify(c, Voxel::empty());    // clear   0.9 → 0

    const float incremental = rig.prop.aggregateStrength(m);

    // The bounded fallback re-sum must reproduce the same value from world state.
    rig.prop.recomputeAggregate(m);
    const float resum = rig.prop.aggregateStrength(m);

    EXPECT_NEAR(incremental, resum, 1e-5)
        << "the running incremental aggregate must equal the full re-sum";
    // Σ = 2.0 + 1.5 + 0 + 5·0.9 = 8.0 over 8 cells → 1.0.
    EXPECT_NEAR(resum, 1.0f, 1e-5);
}
