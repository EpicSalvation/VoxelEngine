#pragma once

// Shared asteroid-field geometry — the single source of truth for WHERE the
// bodies are, included by BOTH halves of the "Asteroid belt miner" demo (M16):
//
//   * plugins/asteroid-field/plugin.cpp  — the volumetric content generators
//     (the rock/ore voxels each body is made of), and
//   * demos/17-asteroid-belt-miner       — the radial GRAVITY policy, which must
//     point "down" at the same body centers the content is built around.
//
// If the demo recomputed body placement on its own, the gravity wells and the
// rock would drift apart the instant a constant changed. Sharing the lattice
// here makes them agree by construction: both call the same `forEachBody` /
// `nearestCenter`, seeded identically.
//
// The field is a pure function of world position and a fixed seed (ARCHITECTURE
// §4): a streamed-out chunk regenerates byte-identically, and — crucially for a
// decomposition cascade (M6) — a coarse macro voxel CONTAINS its fine children
// because the coarse solidity test (`coarseSolid`) is a conservative,
// noise-free superset of the detailed terminal field. See plugin.cpp.

#include "plugin_api.h"  // voxel_rng_norm / voxel_seed_mix (header-only splitmix)

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace asteroidfield {

constexpr uint64_t kSeed = 0xA57E401DF1E1Dull;  // "ASTEROID FIELD"

// Space is partitioned into cubic cells; each cell deterministically hosts at
// most one asteroid. kCellM is the mean body spacing. Body radii are capped well
// below half a cell (kRadiusMax < kCellM/2) so any asteroid covering a query
// point has its center within the point's 1-ring of cells — the 3×3×3
// neighborhood scanned below is then exact, never missing a body reaching in
// from a neighbor cell.
constexpr double kCellM      = 80.0;
constexpr double kFillChance = 0.55;  // fraction of cells that host an asteroid
constexpr double kRadiusMin  = 12.0;
constexpr double kRadiusMax  = 30.0;

// Surface relief: the detailed (terminal) crust modulates the body radius by
// ±kReliefFrac of fbm. The maximal possible surface radius is therefore
// radius·(1 + kReliefFrac); the conservative coarse test uses that bound so it
// supersets every noisy fine voxel regardless of the relief value.
constexpr double kReliefFrac = 0.35;

// One asteroid resolved from its cell. `seed` is the per-body RNG state AFTER
// the placement draws, so the detailed generator's relief noise (which seeds
// from it) decorrelates body-to-body.
struct Body {
    glm::dvec3 center{};
    double     radius = 0.0;
    bool       icy    = false;
    uint64_t   seed   = 0;
};

// Deterministic per-cell seed; chains the header's splitmix mixer over the cell
// index so neighbouring cells decorrelate.
inline uint64_t cellSeed(int64_t cx, int64_t cy, int64_t cz) {
    uint64_t s = kSeed;
    s = voxel_seed_mix(s, static_cast<uint64_t>(cx));
    s = voxel_seed_mix(s, static_cast<uint64_t>(cy));
    s = voxel_seed_mix(s, static_cast<uint64_t>(cz));
    return s;
}

// Resolve the asteroid (if any) hosted by cell (gx, gy, gz). Returns false for
// an empty cell. The draw ORDER (fill, jitter×3, radius, icy) is the field's
// contract — both generators and the gravity policy depend on it, so it must not
// change without regenerating the world.
inline bool bodyInCell(int64_t gx, int64_t gy, int64_t gz, Body& out) {
    uint64_t st = cellSeed(gx, gy, gz);
    if (voxel_rng_norm(&st) >= static_cast<float>(kFillChance))
        return false;  // this cell holds no asteroid

    const double jx = voxel_rng_norm(&st);
    const double jy = voxel_rng_norm(&st);
    const double jz = voxel_rng_norm(&st);
    const double rr = voxel_rng_norm(&st);
    out.icy    = voxel_rng_norm(&st) < 0.18f;
    out.center = glm::dvec3((static_cast<double>(gx) + jx) * kCellM,
                            (static_cast<double>(gy) + jy) * kCellM,
                            (static_cast<double>(gz) + jz) * kCellM);
    out.radius = kRadiusMin + rr * (kRadiusMax - kRadiusMin);
    out.seed   = st;  // post-draw state — the relief-noise seed for this body
    return true;
}

// Invoke fn(const Body&) for every asteroid whose home cell is in the 3×3×3
// neighborhood of world point p. That neighborhood is exact (see kRadiusMax).
template <class Fn>
inline void forEachBody(const glm::dvec3& p, Fn&& fn) {
    const int64_t cx = static_cast<int64_t>(std::floor(p.x / kCellM));
    const int64_t cy = static_cast<int64_t>(std::floor(p.y / kCellM));
    const int64_t cz = static_cast<int64_t>(std::floor(p.z / kCellM));
    for (int64_t dz = -1; dz <= 1; ++dz)
    for (int64_t dy = -1; dy <= 1; ++dy)
    for (int64_t dx = -1; dx <= 1; ++dx) {
        Body b;
        if (bodyInCell(cx + dx, cy + dy, cz + dz, b))
            fn(b);
    }
}

// The nearest asteroid CENTER to world point p (by center distance), the "down"
// target for the radial gravity well. Returns false when no body is near enough
// to be in the 3×3×3 neighborhood — open space, where the demo lets the suit
// drift. Center, not surface: a uniform body's field points at its center.
inline bool nearestCenter(const glm::dvec3& p, glm::dvec3& outCenter,
                          double& outRadius) {
    double bestSq = 0.0;
    bool   found  = false;
    forEachBody(p, [&](const Body& b) {
        const glm::dvec3 d = p - b.center;
        const double sq = d.x * d.x + d.y * d.y + d.z * d.z;
        if (!found || sq < bestSq) {
            bestSq    = sq;
            outCenter = b.center;
            outRadius = b.radius;
            found     = true;
        }
    });
    return found;
}

// Conservative, NOISE-FREE solidity for a coarse (composite-layer) voxel: is the
// axis-aligned cell of side `vs` centered at `cellCenter` within the MAXIMAL
// surface radius of any nearby body? Uses the exact cube-vs-sphere overlap test
// (distance from the body center to the nearest point of the cube), which is
// monotonic under cell nesting — so a solid fine voxel's parent macro is always
// solid too (the coarse-supersets-fine invariant a decomposition cascade needs).
// Reports the covering body's `icy` trait so the coarse crust can be ice or rock.
inline bool coarseSolid(const glm::dvec3& cellCenter, double vs, bool& outIcy) {
    const double half = vs * 0.5;
    bool solid = false;
    forEachBody(cellCenter, [&](const Body& b) {
        if (solid) return;
        const double dxp = std::max(0.0, std::abs(cellCenter.x - b.center.x) - half);
        const double dyp = std::max(0.0, std::abs(cellCenter.y - b.center.y) - half);
        const double dzp = std::max(0.0, std::abs(cellCenter.z - b.center.z) - half);
        const double Rmax = b.radius * (1.0 + kReliefFrac);
        if (dxp * dxp + dyp * dyp + dzp * dzp < Rmax * Rmax) {
            solid  = true;
            outIcy = b.icy;
        }
    });
    return solid;
}

}  // namespace asteroidfield
