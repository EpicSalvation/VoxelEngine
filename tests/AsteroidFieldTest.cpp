// Tests for the asteroid-field body lattice shared by the "Asteroid belt miner"
// demo's content (the plugin's voxels) and its gravity policy (demos/17). The
// lattice is in plugins/asteroid-field/asteroid_field.h; these are pure, headless
// geometry checks — no plugin load, noise, window, or GPU.
//
// Two properties matter for the demo to work:
//   1. The radial gravity well and the rock agree. The demo aims "down" at
//      nearestCenter(p); the plugin builds bodies at those same centers. Both call
//      the shared lattice, so this is true by construction — pinned here against
//      drift (a constant changed in one place but not the other).
//   2. Coarse-supersets-fine. A decomposition cascade (M6) only generates a fine
//      child voxel if its parent macro/micro voxel was solid and decomposed. If
//      the coarse envelope ever missed a cell the fine field fills, the asteroid
//      would sprout holes at coarse-cell boundaries. The coarse test (coarseSolid)
//      uses each body's MAXIMAL relief radius and exact cube-vs-sphere overlap, so
//      it must superset every voxel the (noise-perturbed, effR ≤ Rmax) fine field
//      could ever mark solid. Verified over a dense sample of the field below.

#include "asteroid_field.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <cmath>

namespace {

using asteroidfield::Body;
using asteroidfield::kReliefFrac;

// Center of the axis-aligned cell of side `vs` that contains world point p — the
// macro/micro/grid voxel p falls inside. coarseSolid takes a cell center + size.
glm::dvec3 cellCenterOf(const glm::dvec3& p, double vs) {
    return glm::dvec3((std::floor(p.x / vs) + 0.5) * vs,
                      (std::floor(p.y / vs) + 0.5) * vs,
                      (std::floor(p.z / vs) + 0.5) * vs);
}

// Worst-case fine solidity: is p inside the MAXIMAL possible surface (radius·(1+
// kReliefFrac)) of any nearby body? The real, noise-perturbed terminal field is a
// subset of this (effR ≤ Rmax), so anything the real field can mark solid is
// caught here.
bool maybeSolidFine(const glm::dvec3& p) {
    bool solid = false;
    asteroidfield::forEachBody(p, [&](const Body& b) {
        const glm::dvec3 d = p - b.center;
        const double Rmax = b.radius * (1.0 + kReliefFrac);
        if (d.x * d.x + d.y * d.y + d.z * d.z < Rmax * Rmax) solid = true;
    });
    return solid;
}

}  // namespace

TEST(AsteroidField, NearestCenterIsDeterministic) {
    const glm::dvec3 p(37.0, -12.0, 91.0);
    glm::dvec3 c1, c2;
    double r1 = 0, r2 = 0;
    const bool f1 = asteroidfield::nearestCenter(p, c1, r1);
    const bool f2 = asteroidfield::nearestCenter(p, c2, r2);
    EXPECT_EQ(f1, f2);
    if (f1) {
        EXPECT_EQ(c1, c2);
        EXPECT_EQ(r1, r2);
    }
}

TEST(AsteroidField, NearestCenterReturnsTheActualNearestBody) {
    // Sweep a region; wherever a body is found, no other nearby body may be closer
    // — the gravity well must point at the truly nearest rock.
    int sampledWithBody = 0;
    for (int ix = -2; ix <= 2; ++ix)
    for (int iy = -2; iy <= 2; ++iy)
    for (int iz = -2; iz <= 2; ++iz) {
        const glm::dvec3 p(ix * 33.0, iy * 33.0, iz * 33.0);
        glm::dvec3 center;
        double radius = 0;
        if (!asteroidfield::nearestCenter(p, center, radius)) continue;
        ++sampledWithBody;
        const double bestSq = glm::dot(p - center, p - center);
        asteroidfield::forEachBody(p, [&](const Body& b) {
            const glm::dvec3 d = p - b.center;
            EXPECT_GE(glm::dot(d, d), bestSq - 1e-9)
                << "a nearer body than nearestCenter() reported exists at p="
                << p.x << "," << p.y << "," << p.z;
        });
    }
    EXPECT_GT(sampledWithBody, 0) << "field is empty in the sampled region — check the seed";
}

TEST(AsteroidField, CoarseEnvelopeSupersetsTheFineField) {
    // Densely sample a 256 m cube spanning several body cells (cell size 160 m). At
    // every point the (worst-case) fine field could mark solid, the 1 m grid, 4 m
    // micro, AND 16 m macro voxels CONTAINING it must all be coarse-solid — the
    // cascade decomposes macro→micro→grid, so a gap at any level orphans the fine
    // voxel and punches a hole in the asteroid.
    int fineSolidSamples = 0;
    bool dummyIcy = false;
    for (double x = -128.0; x < 128.0; x += 3.0)
    for (double y = -128.0; y < 128.0; y += 3.0)
    for (double z = -128.0; z < 128.0; z += 3.0) {
        const glm::dvec3 p(x, y, z);
        if (!maybeSolidFine(p)) continue;
        ++fineSolidSamples;

        const glm::dvec3 gridC  = cellCenterOf(p, 1.0);
        const glm::dvec3 microC = cellCenterOf(p, 4.0);
        const glm::dvec3 macroC = cellCenterOf(p, 16.0);

        EXPECT_TRUE(asteroidfield::coarseSolid(gridC, 1.0, dummyIcy))
            << "grid cell missed a solid fine point at " << x << "," << y << "," << z;
        EXPECT_TRUE(asteroidfield::coarseSolid(microC, 4.0, dummyIcy))
            << "micro cell missed a solid fine point at " << x << "," << y << "," << z;
        EXPECT_TRUE(asteroidfield::coarseSolid(macroC, 16.0, dummyIcy))
            << "macro cell missed a solid fine point at " << x << "," << y << "," << z;
    }
    EXPECT_GT(fineSolidSamples, 0) << "no solid field in the sampled cube — check the seed";
}

TEST(AsteroidField, CoarseSolidityIsMonotonicUnderRefinement) {
    // A solid finer cell's enclosing coarser cell is always solid too (the
    // nesting lemma the superset guarantee rests on), checked independently of the
    // fine field over a sweep of nested macro/micro/grid cells.
    bool icy = false;
    for (double x = -96.0; x < 96.0; x += 5.0)
    for (double y = -96.0; y < 96.0; y += 5.0)
    for (double z = -96.0; z < 96.0; z += 5.0) {
        const glm::dvec3 p(x, y, z);
        const glm::dvec3 gridC  = cellCenterOf(p, 1.0);
        const glm::dvec3 microC = cellCenterOf(p, 4.0);
        const glm::dvec3 macroC = cellCenterOf(p, 16.0);
        if (asteroidfield::coarseSolid(gridC, 1.0, icy))
            EXPECT_TRUE(asteroidfield::coarseSolid(microC, 4.0, icy));
        if (asteroidfield::coarseSolid(microC, 4.0, icy))
            EXPECT_TRUE(asteroidfield::coarseSolid(macroC, 16.0, icy));
    }
}
