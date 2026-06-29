#include "ResolvedRecipe.h"

#include <algorithm>
#include <cmath>

#include "Chunk.h"
#include "Voxel.h"

namespace {

// Salt mixed into the decomposition seed for the material-distribution noise, so
// the distribution field is decorrelated from the per-feature seeds (which are
// salted by their declared index + 1). Any fixed nonzero constant works; it only
// has to differ from the feature salts.
constexpr uint64_t kDistributionSalt = 0xD15D15D15D15D15Dull;

// The occupancy (carve) salt is kRecipeOccupancySalt, declared in ResolvedRecipe.h
// so the engine-derived coarse-occupancy generator (DecompositionManager) samples
// the same seeded field a macro's decomposition will.

// Flatten owning RecipeParamValues into the POD RecipeParam array a NoiseFn /
// FeatureGeneratorFn consumes. The returned array's const char* point into the
// source vector's strings, so the source must outlive the array — it does: the
// ResolvedRecipe (and thus its param vectors) lives for the whole fill call.
std::vector<RecipeParam> flatten(const std::vector<RecipeParamValue>& params) {
    std::vector<RecipeParam> out;
    out.reserve(params.size());
    for (const RecipeParamValue& p : params) {
        RecipeParam rp;
        rp.key    = p.key.c_str();
        rp.kind   = p.kind;
        rp.number = p.number;
        rp.text   = p.text.empty() ? nullptr : p.text.c_str();
        out.push_back(rp);
    }
    return out;
}

// Map a noise value to a material by walking the cumulative weights. Given a
// noise field uniform on [0,1) the long-run proportion of each material matches
// its weight; the mapping is monotone, so contiguous noise bands map to the same
// material (spatial coherence). An empty distribution yields empty space.
Voxel sampleDistribution(const ResolvedDistribution& d,
                         const std::vector<RecipeParam>& flatParams,
                         const WorldCoord& center, uint64_t seed) {
    if (d.materials.empty())
        return Voxel::empty();

    float total = 0.0f;
    for (const ResolvedWeight& w : d.materials)
        total += (w.weight > 0.0f) ? w.weight : 0.0f;
    if (total <= 0.0f)
        return Voxel{d.materials.front().props};  // degenerate: all-zero weights

    float n = d.noise ? d.noise(center, seed, flatParams.data(), flatParams.size(),
                                d.noiseUser)
                      : 0.0f;
    if (n < 0.0f) n = 0.0f;
    if (n >= 1.0f) n = 0.99999994f;  // clamp into [0,1)

    const float threshold = n * total;
    float acc = 0.0f;
    for (const ResolvedWeight& w : d.materials) {
        acc += (w.weight > 0.0f) ? w.weight : 0.0f;
        if (threshold < acc)
            return Voxel{w.props};
    }
    return Voxel{d.materials.back().props};
}

}  // namespace

void fillChildChunk(Chunk& chunk, double voxelSizeM, const ResolvedRecipe& recipe,
                    chunkmath::VoxelCoord macroChildMin, int64_t ratio,
                    uint64_t decompSeed, const glm::dvec3& gravityDir) {
    const int n = chunk.size();

    // Resolve which macro-grid axis is "vertical" and which end is "down" under
    // gravity, so the authored bottom/top/side roles land on the gravity-relative
    // faces (M16 G2, the shared role seam from axisrole::roleOf). Default -Y ⇒
    // gravity axis Y, down at low-Y, up at high-Y, side on the X/Z faces — exactly
    // the historical mapping. A degenerate (zero) gravity vector keeps axis Y so
    // recipes without a gravity policy decompose as before.
    const double gx = std::abs(gravityDir.x);
    const double gy = std::abs(gravityDir.y);
    const double gz = std::abs(gravityDir.z);
    const double gmax = std::max({gx, gy, gz});
    const int    gAxis = (gmax <= 1e-12) ? 1 : ((gx == gmax) ? 0 : (gy == gmax ? 1 : 2));
    const bool   downAtLow = (gravityDir[gAxis] <= 0.0);  // -Y default ⇒ down at index 0
    const int    latA = (gAxis == 0) ? 1 : 0;             // the two non-vertical axes
    const int    latB = (gAxis == 2) ? 1 : 2;
    const ChunkCoord cc = chunk.coord();
    const WorldCoord origin = chunk.origin();
    const uint64_t distSeed = voxel_seed_mix(decompSeed, kDistributionSalt);

    // ── (0) occupancy carve setup ────────────────────────────────────────────
    // Resolved once for the chunk: the carve noise + its own seed and flattened
    // (seed-merged) params. Absent (present == false) or unresolved (null fn) ⇒
    // no carve, so the fill is byte-identical to the pre-M18.5 path.
    const bool carve = recipe.occupancy.present && recipe.occupancy.noise != nullptr;
    const uint64_t occSeed = voxel_seed_mix(decompSeed, kRecipeOccupancySalt);
    const std::vector<RecipeParam> flatOcc =
        carve ? flatten(recipe.occupancy.params) : std::vector<RecipeParam>{};

    // Flat param arrays per distribution, built once (params are constant over
    // the chunk). Only the present boundaries need flattening.
    const std::vector<RecipeParam> flatInterior = flatten(recipe.interior.noiseParams);
    const std::vector<RecipeParam> flatTop =
        recipe.top.present ? flatten(recipe.top.distribution.noiseParams)
                           : std::vector<RecipeParam>{};
    const std::vector<RecipeParam> flatBottom =
        recipe.bottom.present ? flatten(recipe.bottom.distribution.noiseParams)
                              : std::vector<RecipeParam>{};
    const std::vector<RecipeParam> flatSide =
        recipe.side.present ? flatten(recipe.side.distribution.noiseParams)
                            : std::vector<RecipeParam>{};

    // ── (1)+(2) distribution + boundary overrides ────────────────────────────
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const chunkmath::VoxelCoord gv =
                    chunkmath::chunkLocalToVoxel(cc, x, y, z, n);
                const int64_t mx = gv.x - macroChildMin.x;  // position within the
                const int64_t my = gv.y - macroChildMin.y;  // macro voxel's child
                const int64_t mz = gv.z - macroChildMin.z;  // grid, in [0, ratio)

                // Coarse-supersets-fine invariant (M10, ARCHITECTURE §4): child
                // voxels outside the macro voxel's subvolume are left empty. This
                // arises only when child_chunk_world_size > parent_voxel_size (a
                // config with large chunk sizes relative to voxel sizes). Keeping
                // these cells empty ensures no fine-layer voxel exists without a
                // covering coarse-layer voxel.
                if (mx < 0 || mx >= ratio || my < 0 || my >= ratio || mz < 0 || mz >= ratio) {
                    chunk.at(x, y, z) = Voxel::empty();
                    continue;
                }

                const WorldCoord center(
                    origin.value.x + (static_cast<double>(x) + 0.5) * voxelSizeM,
                    origin.value.y + (static_cast<double>(y) + 0.5) * voxelSizeM,
                    origin.value.z + (static_cast<double>(z) + 0.5) * voxelSizeM);

                // (0) Occupancy carve: a cell whose sampled field value is below
                // the threshold is empty — applied BEFORE the distribution sample
                // so the recipe can follow a surface. Carve-only: it never makes a
                // cell solid, so coarse-supersets-fine (§4) is preserved. Sampled
                // at the WORLD center so adjacent macros' grids stay seamless.
                if (carve &&
                    recipe.occupancy.noise(center, occSeed, flatOcc.data(),
                                           flatOcc.size(), recipe.occupancy.noiseUser)
                        < recipe.occupancy.threshold) {
                    chunk.at(x, y, z) = Voxel::empty();
                    continue;
                }

                // Distance (in child voxels) inward from the gravity-relative
                // down face and up face, and nearness to either lateral face —
                // computed off the resolved gravity axis so the roles follow
                // "down" (M16 G2). Default -Y ⇒ distDown == my, distUp ==
                // ratio-1-my, lateral on X/Z: identical to the historical mapping.
                const int64_t mc[3] = {mx, my, mz};
                const int64_t distDown = downAtLow ? mc[gAxis] : (ratio - 1 - mc[gAxis]);
                const int64_t distUp   = downAtLow ? (ratio - 1 - mc[gAxis]) : mc[gAxis];
                const auto    nearFace = [&](int axis, int depth) {
                    return mc[axis] < depth || mc[axis] >= ratio - depth;
                };

                // Overlap order bottom -> side -> top (top wins), per §6. Only
                // MacroFace-mode boundaries paint here; Surface-mode top/bottom are
                // applied in the per-column second pass below (their depth is
                // measured from the carved surface, unknown until the column is
                // filled). A Surface-mode boundary is skipped here (treated as
                // not-present) so it does not double-paint.
                const auto inlineFace = [](const ResolvedBoundary& b) {
                    return b.present && b.mode == BoundaryMode::MacroFace;
                };
                const ResolvedDistribution* dist = &recipe.interior;
                const std::vector<RecipeParam>* flat = &flatInterior;
                if (inlineFace(recipe.bottom) && distDown < recipe.bottom.depth) {
                    dist = &recipe.bottom.distribution; flat = &flatBottom;
                }
                if (inlineFace(recipe.side) &&
                    (nearFace(latA, recipe.side.depth) || nearFace(latB, recipe.side.depth))) {
                    dist = &recipe.side.distribution; flat = &flatSide;
                }
                if (inlineFace(recipe.top) && distUp < recipe.top.depth) {
                    dist = &recipe.top.distribution; flat = &flatTop;
                }

                chunk.at(x, y, z) = sampleDistribution(*dist, *flat, center, distSeed);
            }

    // ── (2.5) surface-relative boundary caps (M18.5) ─────────────────────────
    // A Surface-mode top/bottom boundary measures its depth from the CARVED
    // surface of each column, not the macro's geometric face — so a cap tracks a
    // sloped heightmap instead of landing on a flat face. Per lateral column,
    // walk the gravity axis from the surface end inward: a solid cell whose
    // outward neighbor is air (an exposed surface) starts a `depth`-cell band that
    // is repainted with the boundary distribution, ending at the first empty cell.
    //
    // The "outward neighbor" is known only within this chunk. When a macro spans
    // several child chunks vertically (ratio > childChunkSize), a column whose
    // surface lands exactly on an internal chunk boundary is conservatively NOT
    // capped (the cell above is in the next chunk, unseen) — a one-chunk under-cap
    // seam, the §6 exposure-aware-boundary deferral. Configs with the macro one
    // child-chunk tall (ratio == childChunkSize, the common case) never hit it.
    const int64_t chunkBase[3] = {static_cast<int64_t>(cc.x) * n,
                                  static_cast<int64_t>(cc.y) * n,
                                  static_cast<int64_t>(cc.z) * n};
    const int64_t macroMin[3] = {macroChildMin.x, macroChildMin.y, macroChildMin.z};
    const auto applySurfaceCap = [&](const ResolvedBoundary& b, bool towardUp) {
        if (!b.present || b.mode != BoundaryMode::Surface || b.depth <= 0) return;
        const std::vector<RecipeParam> flat = flatten(b.distribution.noiseParams);
        // The surface end is the up end for a top cap, the down end for a bottom
        // cap; whether that end is the high or low index depends on gravity.
        const bool surfaceEndIsHigh = towardUp ? downAtLow : !downAtLow;
        const int  axStart = surfaceEndIsHigh ? (n - 1) : 0;
        const int  axStep  = surfaceEndIsHigh ? -1 : 1;
        const int64_t surfaceFaceLocal = surfaceEndIsHigh ? (ratio - 1) : 0;
        for (int ia = 0; ia < n; ++ia)
            for (int ib = 0; ib < n; ++ib) {
                // Lateral position within the macro; skip columns outside it.
                const int64_t latAloc = chunkBase[latA] + ia - macroMin[latA];
                const int64_t latBloc = chunkBase[latB] + ib - macroMin[latB];
                if (latAloc < 0 || latAloc >= ratio || latBloc < 0 || latBloc >= ratio)
                    continue;
                // The neighbor toward the surface end ("outward" — above for a top
                // cap, below for a bottom cap) is air iff the chunk's surface-end
                // row is the macro's surface face (else the macro continues into a
                // neighbor chunk — unknown, treated as not air).
                bool outwardAir =
                    (chunkBase[gAxis] + axStart - macroMin[gAxis]) == surfaceFaceLocal;
                int remaining = 0;
                int coord[3];
                coord[latA] = ia;
                coord[latB] = ib;
                for (int s = 0; s < n; ++s) {
                    const int axial = axStart + s * axStep;
                    const int64_t mAxial = chunkBase[gAxis] + axial - macroMin[gAxis];
                    coord[gAxis] = axial;
                    const bool inMacro = (mAxial >= 0 && mAxial < ratio);
                    Voxel& v = chunk.at(coord[0], coord[1], coord[2]);
                    if (inMacro && !v.isEmpty()) {
                        if (outwardAir) remaining = b.depth;  // exposed surface: new band
                        if (remaining > 0) {
                            const WorldCoord c(
                                origin.value.x + (coord[0] + 0.5) * voxelSizeM,
                                origin.value.y + (coord[1] + 0.5) * voxelSizeM,
                                origin.value.z + (coord[2] + 0.5) * voxelSizeM);
                            v = sampleDistribution(b.distribution, flat, c, distSeed);
                            --remaining;
                        }
                        outwardAir = false;
                    } else {
                        remaining = 0;      // air (or outside macro) breaks the band
                        outwardAir = true;  // and exposes the next cell inward
                    }
                }
            }
    };
    applySurfaceCap(recipe.top, /*towardUp=*/true);
    applySurfaceCap(recipe.bottom, /*towardUp=*/false);

    // ── (3) feature overlays, in declared order ──────────────────────────────
    for (const ResolvedFeature& f : recipe.features) {
        if (!f.fn) continue;
        const std::vector<RecipeParam> flatParams = flatten(f.params);
        const uint64_t fseed = voxel_seed_mix(decompSeed, f.seedSalt);
        f.fn(origin, voxelSizeM, n, chunk.data(),
             flatParams.data(), flatParams.size(), fseed, f.user);
    }
}
