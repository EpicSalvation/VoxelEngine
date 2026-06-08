// Tests for the voxel-removal cost model (src/simulation/RemovalModel.{h,cpp}).
//
// The removal effort is a pure function of the target's `hardness` and the
// tool's `power` — property-driven, never branched on a block-type id
// (ARCHITECTURE.md §5). These tests pin the four contract branches:
//   hardness  > 0  -> work/time proportional to hardness (strictly increasing)
//   hardness == 0  -> removes in a single step
//   hardness  < 0  -> indestructible (never removable)
//   power    <= 0  -> never removable

#include "simulation/RemovalModel.h"

#include <gtest/gtest.h>

#include <cmath>

using sim::isRemovable;
using sim::removalTime;
using sim::removalWork;

namespace {

// A removal accumulator's threshold-met check, expressed against the model the
// way the tool path will consume it: gate on isRemovable, then compare accrued
// work to the hardness-derived threshold.
bool clearsWithWork(float hardness, float power, float accruedWork) {
    return isRemovable(hardness, power) && accruedWork >= removalWork(hardness);
}

}  // namespace

// --- hardness > 0: effort scales with hardness ----------------------------

TEST(RemovalModel, WorkIsStrictlyIncreasingInHardness) {
    // Effort must be strictly increasing in hardness for hardness > 0, so a
    // harder material visibly takes longer than a softer one.
    for (float h = 0.5f; h < 100.0f; h += 0.5f) {
        EXPECT_LT(removalWork(h), removalWork(h + 0.5f));
    }
}

TEST(RemovalModel, TimeIsStrictlyIncreasingInHardnessAtFixedPower) {
    const float power = 4.0f;
    for (float h = 0.5f; h < 100.0f; h += 0.5f) {
        EXPECT_LT(removalTime(h, power), removalTime(h + 0.5f, power));
    }
}

TEST(RemovalModel, TimeIsHardnessOverPower) {
    EXPECT_FLOAT_EQ(removalTime(10.0f, 2.0f), 5.0f);
    EXPECT_FLOAT_EQ(removalTime(10.0f, 5.0f), 2.0f);
    // Halving power doubles the time for the same target.
    EXPECT_FLOAT_EQ(removalTime(8.0f, 1.0f), 2.0f * removalTime(8.0f, 2.0f));
}

TEST(RemovalModel, TwoVoxelsDifferingOnlyInHardnessNeedDifferentWork) {
    // The property-driven point: identical materials except hardness require
    // measurably different effort, with no id involved.
    MaterialProperties soft;  soft.hardness = 1.0f;
    MaterialProperties hard;  hard.hardness = 9.0f;
    EXPECT_NE(removalWork(soft), removalWork(hard));
    EXPECT_LT(removalWork(soft), removalWork(hard));

    const float power = 3.0f;
    EXPECT_TRUE(isRemovable(soft, power));
    EXPECT_TRUE(isRemovable(hard, power));
    EXPECT_LT(removalTime(soft, power), removalTime(hard, power));
}

// --- hardness == 0: instant, fail-soft default ----------------------------

TEST(RemovalModel, ZeroHardnessClearsInOneStep) {
    EXPECT_TRUE(isRemovable(0.0f, 1.0f));
    EXPECT_FLOAT_EQ(removalWork(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(removalTime(0.0f, 1.0f), 0.0f);
    // Any positive work increment immediately meets the zero threshold.
    EXPECT_TRUE(clearsWithWork(0.0f, 1.0f, 0.0001f));
}

TEST(RemovalModel, DefaultConstructedMaterialIsRemovable) {
    // The zero-initialized struct default means "removable" (fail-soft) — the
    // engine substitutes no hidden default for an unset hardness.
    MaterialProperties unset;  // hardness defaults to 0.0f
    EXPECT_TRUE(isRemovable(unset, 1.0f));
    EXPECT_FLOAT_EQ(removalWork(unset), 0.0f);
}

// --- hardness < 0: indestructible sentinel --------------------------------

TEST(RemovalModel, NegativeHardnessIsNeverRemovable) {
    EXPECT_FALSE(isRemovable(sim::kIndestructible, 1.0f));
    EXPECT_FALSE(isRemovable(-0.001f, 1000.0f));
    EXPECT_TRUE(std::isinf(removalTime(sim::kIndestructible, 1.0f)));
    EXPECT_TRUE(std::isinf(removalWork(sim::kIndestructible)));
}

TEST(RemovalModel, IndestructibleNeverAccruesToThreshold) {
    // Even an enormous amount of accrued work never clears an indestructible
    // voxel, because the threshold is +infinity.
    EXPECT_FALSE(clearsWithWork(sim::kIndestructible, 100.0f, 1e30f));
}

// --- power <= 0: powerless tool -------------------------------------------

TEST(RemovalModel, NonPositivePowerIsNeverRemovable) {
    EXPECT_FALSE(isRemovable(5.0f, 0.0f));
    EXPECT_FALSE(isRemovable(5.0f, -1.0f));
    // Even a zero-hardness (otherwise instant) target cannot be removed with no
    // power behind the tool.
    EXPECT_FALSE(isRemovable(0.0f, 0.0f));
    EXPECT_TRUE(std::isinf(removalTime(5.0f, 0.0f)));
    EXPECT_TRUE(std::isinf(removalTime(0.0f, 0.0f)));
}

// --- determinism -----------------------------------------------------------

TEST(RemovalModel, IdenticalInputsAreDeterministic) {
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FLOAT_EQ(removalWork(7.25f), removalWork(7.25f));
        EXPECT_FLOAT_EQ(removalTime(7.25f, 3.0f), removalTime(7.25f, 3.0f));
        EXPECT_EQ(isRemovable(7.25f, 3.0f), isRemovable(7.25f, 3.0f));
    }
}
