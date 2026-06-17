// M14 infrastructure — FieldOverlay sparse store (docs/ARCHITECTURE.md §17).
//
// The shared coord→float map both ThermalSystem and FluidSystem sit on. These
// tests pin down the four contracts named in the README task:
//
//   - round-trips set/get against the ambient default;
//   - tracks the active set and its frontier correctly across add/clear;
//   - drops cells that return to ambient (stays sparse);
//   - iterates in deterministic sorted-coord order.

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "simulation/FieldOverlay.h"
#include "simulation/NeighborWalk.h"
#include "world/ChunkCoordMath.h"

namespace {

using chunkmath::VoxelCoord;

constexpr float kAmbient = 20.0f;

sim::FieldOverlay makeOverlay() { return sim::FieldOverlay(kAmbient); }

}  // namespace

// An unset cell returns the ambient default; a set cell returns the stored value.
TEST(FieldOverlay, RoundTripSetGetAgainstAmbient) {
    auto f = makeOverlay();
    const VoxelCoord c{3, 5, 7};

    EXPECT_FLOAT_EQ(f.get(c), kAmbient) << "unset cell must return ambient";
    EXPECT_FALSE(f.isActive(c));

    f.set(c, 100.0f);
    EXPECT_FLOAT_EQ(f.get(c), 100.0f) << "set cell must return the stored value";
    EXPECT_TRUE(f.isActive(c));

    f.set(c, -50.0f);
    EXPECT_FLOAT_EQ(f.get(c), -50.0f) << "overwrite must take the new value";
}

// Setting several cells grows the active set; clearing one shrinks it.
TEST(FieldOverlay, ActiveSetTracksAddAndClear) {
    auto f = makeOverlay();
    f.set({0, 0, 0}, 1.0f);
    f.set({1, 0, 0}, 2.0f);
    f.set({0, 1, 0}, 3.0f);
    EXPECT_EQ(f.activeCount(), 3u);

    f.clear({1, 0, 0});
    EXPECT_EQ(f.activeCount(), 2u);
    EXPECT_FLOAT_EQ(f.get({1, 0, 0}), kAmbient)
        << "cleared cell must return ambient";
    EXPECT_FALSE(f.isActive({1, 0, 0}));
}

// The frontier is the 6-connected ambient neighbors of the active set.
TEST(FieldOverlay, FrontierIsAmbientNeighborsOfActiveSet) {
    auto f = makeOverlay();
    const VoxelCoord c{5, 5, 5};
    f.set(c, 42.0f);

    const auto frontier = f.frontierSorted();
    const auto nbrs = sim::neighbors6(c);
    ASSERT_EQ(frontier.size(), 6u)
        << "a single active cell with no active neighbors has exactly 6 frontier cells";
    for (const VoxelCoord& n : nbrs)
        EXPECT_NE(std::find(frontier.begin(), frontier.end(), n), frontier.end())
            << "each 6-connected neighbor of the sole active cell must be in the frontier";
}

// Frontier excludes cells that are themselves active.
TEST(FieldOverlay, FrontierExcludesActiveCells) {
    auto f = makeOverlay();
    const VoxelCoord a{0, 0, 0};
    const VoxelCoord b{1, 0, 0};  // +x neighbor of a
    f.set(a, 1.0f);
    f.set(b, 2.0f);

    const auto frontier = f.frontierSorted();
    EXPECT_EQ(std::find(frontier.begin(), frontier.end(), a), frontier.end())
        << "active cell a must not appear in the frontier";
    EXPECT_EQ(std::find(frontier.begin(), frontier.end(), b), frontier.end())
        << "active cell b must not appear in the frontier";
    EXPECT_EQ(frontier.size(), 10u)
        << "two adjacent active cells share one frontier face: 6+6-2=10 unique frontier cells";
}

// Setting a cell back to ambient drops it from the active set — the overlay stays sparse.
TEST(FieldOverlay, SettingToAmbientDropsFromActiveSet) {
    auto f = makeOverlay();
    const VoxelCoord c{2, 3, 4};
    f.set(c, 99.0f);
    ASSERT_TRUE(f.isActive(c));

    f.set(c, kAmbient);
    EXPECT_FALSE(f.isActive(c)) << "setting to ambient must remove the cell";
    EXPECT_EQ(f.activeCount(), 0u) << "the overlay must be empty after all cells return to ambient";
    EXPECT_FLOAT_EQ(f.get(c), kAmbient);
}

// Multiple cells set and decayed back: the overlay ends empty.
TEST(FieldOverlay, DecayingAllCellsLeavesOverlayEmpty) {
    auto f = makeOverlay();
    f.set({0, 0, 0}, 10.0f);
    f.set({1, 1, 1}, 50.0f);
    f.set({2, 2, 2}, 30.0f);
    ASSERT_EQ(f.activeCount(), 3u);

    f.set({0, 0, 0}, kAmbient);
    f.set({1, 1, 1}, kAmbient);
    f.set({2, 2, 2}, kAmbient);
    EXPECT_EQ(f.activeCount(), 0u);
    EXPECT_TRUE(f.activeSorted().empty());
    EXPECT_TRUE(f.frontierSorted().empty());
}

// activeSorted() returns cells in deterministic VoxelCoordLess order.
TEST(FieldOverlay, DeterministicSortedCoordIteration) {
    auto f = makeOverlay();
    f.set({5, 0, 0}, 1.0f);
    f.set({0, 5, 0}, 2.0f);
    f.set({0, 0, 5}, 3.0f);
    f.set({1, 1, 1}, 4.0f);
    f.set({0, 0, 0}, 5.0f);

    const auto sorted = f.activeSorted();
    ASSERT_EQ(sorted.size(), 5u);
    EXPECT_TRUE(std::is_sorted(sorted.begin(), sorted.end(), sim::VoxelCoordLess{}))
        << "activeSorted() must return cells in VoxelCoordLess order";
}

// frontierSorted() also returns cells in deterministic VoxelCoordLess order.
TEST(FieldOverlay, FrontierSortedIsDeterministic) {
    auto f = makeOverlay();
    f.set({3, 3, 3}, 1.0f);
    f.set({4, 3, 3}, 2.0f);

    const auto frontier = f.frontierSorted();
    ASSERT_FALSE(frontier.empty());
    EXPECT_TRUE(std::is_sorted(frontier.begin(), frontier.end(), sim::VoxelCoordLess{}))
        << "frontierSorted() must return cells in VoxelCoordLess order";
}

// Repeated calls to activeSorted() produce byte-identical results (determinism).
TEST(FieldOverlay, RepeatedCallsProduceIdenticalResults) {
    auto f = makeOverlay();
    f.set({10, 20, 30}, 1.0f);
    f.set({5, 15, 25}, 2.0f);
    f.set({0, 0, 0}, 3.0f);

    const auto a = f.activeSorted();
    const auto b = f.activeSorted();
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].x, b[i].x);
        EXPECT_EQ(a[i].y, b[i].y);
        EXPECT_EQ(a[i].z, b[i].z);
    }
}
