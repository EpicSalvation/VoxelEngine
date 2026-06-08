// Tests for the per-target removal accumulator (src/simulation/RemovalAccumulator).
//
// The accumulator is the transient tool state that turns the pure RemovalModel
// into a held-to-mine interaction (ARCHITECTURE.md §5):
//   - work accrues at power * dt against the targeted voxel
//   - the voxel clears only when accrued work meets removalWork(hardness)
//   - switching targets (or releasing) resets progress to zero
//   - an indestructible (hardness < 0) target or a powerless tool never clears
//   - hardness is passed in off the voxel's own material — never an id

#include "simulation/RemovalAccumulator.h"
#include "simulation/RemovalModel.h"

#include <gtest/gtest.h>

using chunkmath::VoxelCoord;
using sim::RemovalAccumulator;

namespace {
constexpr VoxelCoord kA{1, 2, 3};
constexpr VoxelCoord kB{4, 5, 6};
}  // namespace

// --- hardness > 0: accrues over multiple ticks ----------------------------

TEST(RemovalAccumulator, HardVoxelTakesMultipleTicks) {
    // hardness 1.0, power 0.5 -> removalWork = 1.0, so 1.0 / (0.5 * dt) ticks.
    RemovalAccumulator acc;
    const float hardness = 1.0f, power = 0.5f, dt = 0.1f;  // 0.05 work/tick
    // 19 ticks accrue 0.95 < 1.0 — not yet cleared.
    for (int i = 0; i < 19; ++i)
        EXPECT_FALSE(acc.accrue(kA, hardness, power, dt)) << "tick " << i;
    // The 20th tick reaches 1.0 and clears.
    EXPECT_TRUE(acc.accrue(kA, hardness, power, dt));
}

TEST(RemovalAccumulator, HarderVoxelTakesMoreTicksThanSofter) {
    const float power = 0.5f, dt = 0.1f;
    auto ticksToClear = [&](float hardness) {
        RemovalAccumulator acc;
        int ticks = 0;
        while (!acc.accrue(kA, hardness, power, dt)) {
            if (++ticks > 100000) break;  // guard
        }
        return ticks + 1;
    };
    EXPECT_LT(ticksToClear(0.2f), ticksToClear(0.7f));  // grass < stone
}

TEST(RemovalAccumulator, ProgressRisesMonotonicallyTowardOne) {
    RemovalAccumulator acc;
    const float hardness = 1.0f, power = 0.5f, dt = 0.1f;
    float last = 0.0f;
    for (int i = 0; i < 19; ++i) {
        acc.accrue(kA, hardness, power, dt);
        const float p = acc.progress();
        EXPECT_GE(p, last);
        EXPECT_LE(p, 1.0f);
        last = p;
    }
    EXPECT_GT(last, 0.0f);
    EXPECT_LT(last, 1.0f);
}

// --- retargeting resets progress ------------------------------------------

TEST(RemovalAccumulator, SwitchingTargetResetsProgress) {
    RemovalAccumulator acc;
    const float hardness = 1.0f, power = 0.5f, dt = 0.1f;
    for (int i = 0; i < 19; ++i) acc.accrue(kA, hardness, power, dt);  // ~0.95
    EXPECT_GT(acc.progress(), 0.9f);

    // A new target starts from zero — one tick on B is far from clearing.
    EXPECT_FALSE(acc.accrue(kB, hardness, power, dt));
    EXPECT_EQ(acc.target(), kB);
    EXPECT_LT(acc.progress(), 0.1f);
}

TEST(RemovalAccumulator, ResetDropsTargetAndProgress) {
    RemovalAccumulator acc;
    const float hardness = 1.0f, power = 0.5f, dt = 0.1f;
    for (int i = 0; i < 10; ++i) acc.accrue(kA, hardness, power, dt);
    EXPECT_TRUE(acc.hasTarget());
    EXPECT_GT(acc.progress(), 0.0f);

    acc.reset();
    EXPECT_FALSE(acc.hasTarget());
    EXPECT_FLOAT_EQ(acc.progress(), 0.0f);

    // Re-targeting the same coord after a reset starts fresh, not where it left off.
    EXPECT_FALSE(acc.accrue(kA, hardness, power, dt));
    EXPECT_LT(acc.progress(), 0.1f);
}

// --- hardness == 0: clears on the first tick ------------------------------

TEST(RemovalAccumulator, ZeroHardnessClearsImmediately) {
    RemovalAccumulator acc;
    EXPECT_TRUE(acc.accrue(kA, 0.0f, 0.5f, 0.1f));
}

// --- hardness < 0: indestructible never clears ----------------------------

TEST(RemovalAccumulator, IndestructibleNeverClearsOrAccrues) {
    RemovalAccumulator acc;
    for (int i = 0; i < 1000; ++i)
        EXPECT_FALSE(acc.accrue(kA, sim::kIndestructible, 100.0f, 0.1f));
    EXPECT_FLOAT_EQ(acc.progress(), 0.0f);  // no progress is ever shown
}

// --- power <= 0: powerless tool never clears ------------------------------

TEST(RemovalAccumulator, PowerlessToolNeverClears) {
    RemovalAccumulator acc;
    for (int i = 0; i < 1000; ++i)
        EXPECT_FALSE(acc.accrue(kA, 0.7f, 0.0f, 0.1f));
    EXPECT_FLOAT_EQ(acc.progress(), 0.0f);
}

// --- determinism: same held sequence -> same clear tick -------------------

TEST(RemovalAccumulator, DeterministicAcrossRuns) {
    auto ticksToClear = [] {
        RemovalAccumulator acc;
        int ticks = 1;
        while (!acc.accrue(kA, 0.7f, 0.5f, 0.1f)) ++ticks;
        return ticks;
    };
    EXPECT_EQ(ticksToClear(), ticksToClear());
}
