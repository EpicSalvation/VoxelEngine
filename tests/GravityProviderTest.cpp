// Tests for the M16 L7 gravity provider seam (docs/ARCHITECTURE.md §18).
//
// Gravity is a *policy* read through the single `gravityAt(WorldCoord) -> dvec3`
// seam: the engine default is constant -Y (every current demo/test unchanged), a
// radial provider returns a unit vector pointing at a body's center from any
// side, and a zero-g provider returns the zero vector. These three built-in
// shapes cover the M16 demos (Beyond blocks's zero-g, Asteroid belt miner's
// per-body well); `custom` wraps an out-of-tree function pointer.

#include "world/GravityProvider.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

namespace {

constexpr double kEps = 1e-9;

void expectVec(const glm::dvec3& a, const glm::dvec3& b) {
    EXPECT_NEAR(a.x, b.x, kEps);
    EXPECT_NEAR(a.y, b.y, kEps);
    EXPECT_NEAR(a.z, b.z, kEps);
}

}  // namespace

TEST(GravityProvider, DefaultIsConstantNegativeYEverywhere) {
    const GravityProvider g;  // engine default
    expectVec(g.gravityAt(WorldCoord(0.0, 0.0, 0.0)),       {0.0, -1.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(1000.0, -50.0, 7.0)),  {0.0, -1.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(-3.0, 99999.0, 12.0)), {0.0, -1.0, 0.0});

    // The explicit constant factory matches the default.
    expectVec(GravityProvider::constant({0.0, -1.0, 0.0}).gravityAt(WorldCoord(5, 5, 5)),
              {0.0, -1.0, 0.0});
}

TEST(GravityProvider, ConstantCanPointAnyFixedAxis) {
    const GravityProvider g = GravityProvider::constant({-1.0, 0.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(10.0, 0.0, 0.0)),  {-1.0, 0.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(-10.0, 9.0, 3.0)), {-1.0, 0.0, 0.0});
}

TEST(GravityProvider, RadialPointsAtCenterAsAUnitVectorFromAnySide) {
    const WorldCoord center(100.0, 100.0, 100.0);
    const GravityProvider g = GravityProvider::radial(center);

    // From +X of the center, "down" is -X; from +Y, "down" is -Y; etc. Always
    // unit length, always toward the center.
    expectVec(g.gravityAt(WorldCoord(110.0, 100.0, 100.0)), {-1.0, 0.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(100.0, 110.0, 100.0)), {0.0, -1.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(100.0, 100.0, 90.0)),  {0.0, 0.0, 1.0});

    // A diagonal sample is still unit length toward the center.
    const glm::dvec3 diag = g.gravityAt(WorldCoord(105.0, 103.0, 101.0));
    EXPECT_NEAR(glm::length(diag), 1.0, kEps);
    const glm::dvec3 toCenter = glm::normalize(center.value - glm::dvec3(105.0, 103.0, 101.0));
    expectVec(diag, toCenter);

    // At the center exactly there is no defined direction → zero.
    expectVec(g.gravityAt(center), {0.0, 0.0, 0.0});
}

TEST(GravityProvider, RadialScalesByStrength) {
    const GravityProvider g = GravityProvider::radial(WorldCoord(0, 0, 0), 9.81);
    const glm::dvec3 down = g.gravityAt(WorldCoord(0.0, 5.0, 0.0));
    expectVec(down, {0.0, -9.81, 0.0});
}

TEST(GravityProvider, ZeroGIsTheZeroVectorEverywhere) {
    const GravityProvider g = GravityProvider::zeroG();
    expectVec(g.gravityAt(WorldCoord(0, 0, 0)),       {0.0, 0.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(42, -17, 1000)), {0.0, 0.0, 0.0});
}

TEST(GravityProvider, CustomWrapsAFunctionPointer) {
    // A bespoke provider: gravity points back toward the world Y axis (a cylinder
    // well), proving an out-of-tree policy threads through the same seam.
    auto fn = [](WorldCoord pos, void*) -> glm::dvec3 {
        glm::dvec3 toAxis(-pos.value.x, 0.0, -pos.value.z);
        const double len = glm::length(toAxis);
        return len > 1e-9 ? toAxis / len : glm::dvec3(0.0);
    };
    const GravityProvider g = GravityProvider::custom(fn, nullptr);
    expectVec(g.gravityAt(WorldCoord(4.0, 99.0, 0.0)), {-1.0, 0.0, 0.0});
    expectVec(g.gravityAt(WorldCoord(0.0, 99.0, -8.0)), {0.0, 0.0, 1.0});
}
