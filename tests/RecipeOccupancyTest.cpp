// Recipe occupancy carving (M18.5, docs/proposals/recipe-occupancy.md).
//
// The occupancy stage lets a recipe carve child cells to empty BEFORE the
// material distribution fills them, so a recipe-driven composite layer can follow
// a surface instead of refining a solid cube into a smaller-voxel solid cube.
// These tests pin the carve contract:
//   - absent occupancy is byte-identical to the pre-M18.5 full-solid fill;
//   - the carve is deterministic (same seed ⇒ same holes);
//   - it is carve-only — every solid carved cell was solid without the carve, and
//     surviving cells keep their distribution material (coarse-supersets-fine);
//   - threshold edge cases (≤ min value ⇒ all solid; > max value ⇒ all empty);
//   - a heightmap-style field produces a clean per-column surface that tracks a
//     sloped surface (the Mega-Demo use case).

#include "world/Chunk.h"
#include "world/ChunkCoordMath.h"
#include "world/ResolvedRecipe.h"
#include "world/Voxel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

namespace {

constexpr int    kN  = 8;     // child chunk size (voxels per side)
constexpr double kVS = 1.0;   // child voxel size (m)

MaterialProperties mat(uint8_t palette) {
    MaterialProperties m;
    m.palette_index = palette;
    m.density       = 1000.0f;  // non-zero so the voxel is solid, not "empty"
    return m;
}

bool sameVoxel(const Voxel& a, const Voxel& b) {
    const MaterialProperties& x = a.material;
    const MaterialProperties& y = b.material;
    return x.density == y.density && x.structural_strength == y.structural_strength &&
           x.thermal_conductivity == y.thermal_conductivity && x.porosity == y.porosity &&
           x.hardness == y.hardness && x.palette_index == y.palette_index;
}

bool sameGrid(const Chunk& a, const Chunk& b) {
    if (a.size() != b.size()) return false;
    const int n = a.size();
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (!sameVoxel(a.at(x, y, z), b.at(x, y, z))) return false;
    return true;
}

int countEmpty(const Chunk& c) {
    const int n = c.size();
    int e = 0;
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (c.at(x, y, z).isEmpty()) ++e;
    return e;
}

// A per-cell uniform [0,1) hash of the voxel-center lattice cell — a stand-in for
// a real carve field that actually produces holes. Pure in (position, seed).
float hashNoise(WorldCoord p, uint64_t seed, const RecipeParam*, size_t, void*) {
    auto fl = [](double v) { return static_cast<int64_t>(std::floor(v)); };
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(fl(p.value.x)) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(fl(p.value.y)) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(fl(p.value.z)) * 0x165667B19E3779F9ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= h >> 31;
    return static_cast<float>(h >> 40) / static_cast<float>(1ull << 24);  // [0,1)
}

// A heightmap "surface" field: 1.0 below a sloped surface, 0.0 above it. With a
// 0.5 threshold a cell is solid iff its center sits at or below surfaceY(x),
// where the surface rises one metre per metre of +X — a 45° ramp through the
// macro. This is the Mega-Demo shape (a recipe tracking a heightmap).
float surfaceField(WorldCoord p, uint64_t, const RecipeParam*, size_t, void*) {
    const double surfaceY = 1.5 + p.value.x;  // rises with +X
    return (p.value.y <= surfaceY) ? 1.0f : 0.0f;
}

// A solid two-property interior so a "solid" cell is unambiguous.
ResolvedRecipe solidInterior(uint8_t palette = 1) {
    ResolvedRecipe r;
    r.interior.materials = {{mat(palette), 1.0f}};  // null noise => first material everywhere
    return r;
}

// fillChildChunk over a single macro that exactly fills one chunk (ratio == kN,
// macro min at the chunk's first voxel) at chunk origin (0,0,0).
Chunk fill(const ResolvedRecipe& r, uint64_t seed) {
    Chunk c(ChunkCoord{0, 0, 0}, kN, WorldCoord(0.0, 0.0, 0.0));
    fillChildChunk(c, kVS, r, chunkmath::VoxelCoord{0, 0, 0}, kN, seed);
    return c;
}

}  // namespace

// ── Absent occupancy is the pre-M18.5 behavior ───────────────────────────────

TEST(RecipeOccupancy, AbsentOccupancyFillsFullySolid) {
    const Chunk c = fill(solidInterior(7), 123u);
    EXPECT_EQ(countEmpty(c), 0);  // every cell solid — a recipe with no carve
    EXPECT_EQ(c.at(0, 0, 0).material.palette_index, 7u);
    EXPECT_EQ(c.at(7, 7, 7).material.palette_index, 7u);
}

TEST(RecipeOccupancy, PresentButNullNoiseIsTreatedAsAbsent) {
    // A resolved occupancy flagged present but with an unresolved (null) fn — the
    // fail-soft path validateRecipes guards against at startup. fillChildChunk
    // must not dereference it; the fill stays fully solid.
    ResolvedRecipe r = solidInterior(3);
    r.occupancy.present   = true;
    r.occupancy.noise     = nullptr;
    r.occupancy.threshold = 0.5f;
    const Chunk c = fill(r, 123u);
    EXPECT_EQ(countEmpty(c), 0);
}

// ── Determinism and carve-only (coarse-supersets-fine) ───────────────────────

TEST(RecipeOccupancy, CarveIsDeterministicForFixedSeed) {
    ResolvedRecipe r = solidInterior(1);
    r.occupancy.present   = true;
    r.occupancy.noise     = &hashNoise;
    r.occupancy.threshold = 0.3f;  // carve the ~30% of cells whose field < 0.3

    const Chunk a = fill(r, 4242u);
    const Chunk b = fill(r, 4242u);
    EXPECT_TRUE(sameGrid(a, b));

    const int empties = countEmpty(a);
    EXPECT_GT(empties, 0);                 // it actually carved something
    EXPECT_LT(empties, kN * kN * kN);      // but not everything
}

TEST(RecipeOccupancy, CarveOnlyEmptiesAndLeavesSurvivorsUnchanged) {
    // The carved grid's solid cells must be a subset of the un-carved solid cells,
    // and each survivor must keep the exact material the distribution assigned it.
    const Chunk plain = fill(solidInterior(5), 909u);

    ResolvedRecipe r = solidInterior(5);
    r.occupancy.present   = true;
    r.occupancy.noise     = &hashNoise;
    r.occupancy.threshold = 0.5f;
    const Chunk carved = fill(r, 909u);

    for (int z = 0; z < kN; ++z)
        for (int y = 0; y < kN; ++y)
            for (int x = 0; x < kN; ++x) {
                const Voxel& p = plain.at(x, y, z);
                const Voxel& c = carved.at(x, y, z);
                // Carve never turns an empty plain cell solid (plain has none here),
                // and a surviving carved cell is byte-identical to the plain fill.
                if (!c.isEmpty()) EXPECT_TRUE(sameVoxel(c, p));
            }
    EXPECT_GT(countEmpty(carved), countEmpty(plain));  // strictly more holes
}

// ── Threshold edge cases ─────────────────────────────────────────────────────

TEST(RecipeOccupancy, ThresholdAtOrBelowMinKeepsEverythingSolid) {
    ResolvedRecipe r = solidInterior(1);
    r.occupancy.present   = true;
    r.occupancy.noise     = &hashNoise;   // values in [0,1)
    r.occupancy.threshold = 0.0f;         // solid iff value >= 0 — always true
    EXPECT_EQ(countEmpty(fill(r, 1u)), 0);
}

TEST(RecipeOccupancy, ThresholdAboveMaxCarvesEverything) {
    ResolvedRecipe r = solidInterior(1);
    r.occupancy.present   = true;
    r.occupancy.noise     = &hashNoise;   // values in [0,1)
    r.occupancy.threshold = 2.0f;         // no value reaches it — all empty
    EXPECT_EQ(countEmpty(fill(r, 1u)), kN * kN * kN);
}

// ── A heightmap surface (the Mega-Demo shape) ────────────────────────────────

TEST(RecipeOccupancy, HeightmapFieldProducesCleanPerColumnSurface) {
    ResolvedRecipe r = solidInterior(2);
    r.occupancy.present   = true;
    r.occupancy.noise     = &surfaceField;
    r.occupancy.threshold = 0.5f;
    const Chunk c = fill(r, 0u);

    // Each (x,z) column must be solid from the bottom up to its surface and empty
    // above — no floating cells, no holes below the surface (a real surface, not
    // swiss cheese), and the solid height must rise with +X (the 45° ramp).
    int prevHeight = -1;
    for (int x = 0; x < kN; ++x) {
        const int z = 0;
        int solidCount = 0;
        bool seenEmpty = false;
        for (int y = 0; y < kN; ++y) {
            const bool solid = !c.at(x, y, z).isEmpty();
            if (solid) {
                EXPECT_FALSE(seenEmpty) << "floating solid at x=" << x << " y=" << y;
                ++solidCount;
            } else {
                seenEmpty = true;
            }
        }
        // surfaceY(x) = 1.5 + (x + 0.5); cells with center y+0.5 <= surfaceY solid.
        const double surfaceY = 1.5 + (static_cast<double>(x) + 0.5);
        int expected = 0;
        for (int y = 0; y < kN; ++y)
            if (static_cast<double>(y) + 0.5 <= surfaceY) ++expected;
        EXPECT_EQ(solidCount, expected) << "column x=" << x;
        EXPECT_GE(solidCount, prevHeight);  // monotonic ramp with +X
        prevHeight = solidCount;
    }
    EXPECT_GT(prevHeight, 0);  // the far column has real terrain
}

// ── Surface-relative boundary cap (M18.5 v2) ─────────────────────────────────

namespace {
// Index of the topmost solid cell in column (x,z) along +Y (default gravity up),
// or -1 if the column is empty.
int topSolidY(const Chunk& c, int x, int z) {
    for (int y = c.size() - 1; y >= 0; --y)
        if (!c.at(x, y, z).isEmpty()) return y;
    return -1;
}
}  // namespace

TEST(RecipeOccupancy, SurfaceTopCapTracksSlopedCarvedSurface) {
    // Stone interior, carved to the +X ramp, with a depth-1 grass cap in SURFACE
    // mode: the cap must sit on the topmost solid cell of every column (which
    // varies with the slope), not on the flat macro top face.
    ResolvedRecipe r = solidInterior(2);                  // stone
    r.occupancy.present   = true;
    r.occupancy.noise     = &surfaceField;
    r.occupancy.threshold = 0.5f;
    r.top.present = true;
    r.top.depth   = 1;
    r.top.mode    = BoundaryMode::Surface;
    r.top.distribution.materials = {{mat(9), 1.0f}};      // grass

    const Chunk c = fill(r, 0u);
    for (int x = 0; x < kN; ++x) {
        const int top = topSolidY(c, x, 0);
        ASSERT_GE(top, 0) << "column x=" << x << " should have terrain";
        EXPECT_EQ(c.at(x, top, 0).material.palette_index, 9u) << "surface cap at x=" << x;
        if (top > 0)  // the cell just below the surface is still stone interior
            EXPECT_EQ(c.at(x, top - 1, 0).material.palette_index, 2u);
    }
    // The cap follows the slope: the surface Y rises with +X, so it is not a flat
    // plane (which a macro-face cap would have produced).
    EXPECT_LT(topSolidY(c, 0, 0), topSolidY(c, kN - 1, 0));
}

TEST(RecipeOccupancy, SurfaceTopCapDepthBandFollowsSurface) {
    ResolvedRecipe r = solidInterior(2);
    r.occupancy.present   = true;
    r.occupancy.noise     = &surfaceField;
    r.occupancy.threshold = 0.5f;
    r.top.present = true;
    r.top.depth   = 2;                                    // two-cell cap
    r.top.mode    = BoundaryMode::Surface;
    r.top.distribution.materials = {{mat(9), 1.0f}};

    const Chunk c = fill(r, 0u);
    for (int x = 0; x < kN; ++x) {
        const int top = topSolidY(c, x, 0);
        ASSERT_GE(top, 0);
        EXPECT_EQ(c.at(x, top, 0).material.palette_index, 9u);
        if (top - 1 >= 0) EXPECT_EQ(c.at(x, top - 1, 0).material.palette_index, 9u);
        if (top - 2 >= 0) EXPECT_EQ(c.at(x, top - 2, 0).material.palette_index, 2u);  // back to stone
    }
}

TEST(RecipeOccupancy, SurfaceCapWithoutCarveReducesToMacroFaceTop) {
    // No occupancy stage: the "surface" is the macro's top face, so a Surface-mode
    // top cap must paint the same cells a MacroFace-mode cap would (the top row).
    auto build = [](BoundaryMode mode) {
        ResolvedRecipe r = solidInterior(2);
        r.top.present = true;
        r.top.depth   = 1;
        r.top.mode    = mode;
        r.top.distribution.materials = {{mat(9), 1.0f}};
        return fill(r, 7u);
    };
    const Chunk surfaceCap  = build(BoundaryMode::Surface);
    const Chunk macroFaceCap = build(BoundaryMode::MacroFace);
    EXPECT_TRUE(sameGrid(surfaceCap, macroFaceCap));
    // And it is the top row (y = n-1) that is capped.
    EXPECT_EQ(surfaceCap.at(3, kN - 1, 3).material.palette_index, 9u);
    EXPECT_EQ(surfaceCap.at(3, kN - 2, 3).material.palette_index, 2u);
}

TEST(RecipeOccupancy, SurfaceModeAndMacroFaceModeAreIndependentPasses) {
    // A Surface-mode top cap and a MacroFace-mode bottom cap coexist: grass on the
    // carved surface, bedrock on the flat macro bottom.
    ResolvedRecipe r = solidInterior(2);
    r.occupancy.present   = true;
    r.occupancy.noise     = &surfaceField;
    r.occupancy.threshold = 0.5f;
    r.top.present = true;  r.top.depth = 1; r.top.mode = BoundaryMode::Surface;
    r.top.distribution.materials = {{mat(9), 1.0f}};       // grass
    r.bottom.present = true; r.bottom.depth = 1; r.bottom.mode = BoundaryMode::MacroFace;
    r.bottom.distribution.materials = {{mat(14), 1.0f}};   // bedrock

    const Chunk c = fill(r, 0u);
    for (int x = 0; x < kN; ++x) {
        const int top = topSolidY(c, x, 0);
        ASSERT_GE(top, 0);
        EXPECT_EQ(c.at(x, top, 0).material.palette_index, 9u);   // surface grass
        EXPECT_EQ(c.at(x, 0, 0).material.palette_index, 14u);    // macro-bottom bedrock
    }
}
